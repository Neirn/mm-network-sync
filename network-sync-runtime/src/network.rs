use anyhow::Result;
use gamecore::network::NetworkModule;
use network_sync_rs::messages::{
    get_message_type, BasicMessageType, ClientActorUpdateMessage, ClientBasicMessage, ClientJoinSessionMessage,
    ClientRegisterActorMessage, MessageType, RegisteredMessage, ServerSyncSessionMessage,
    ServerWelcomeMessage,
};
use network_sync_rs::{Actor, ActorGameData};
use serde_json;
use std::collections::{HashMap, VecDeque};
use std::panic;
use std::sync::{Arc, Mutex, OnceLock};
use tokio::runtime::Runtime;

// Global singleton instances
pub static NETWORK_PLAY: OnceLock<Arc<Mutex<NetworkSyncModule>>> = OnceLock::new();
pub static TOKIO_RUNTIME: OnceLock<Runtime> = OnceLock::new();

// Get or initialize the tokio runtime
fn get_tokio_runtime() -> &'static Runtime {
    TOKIO_RUNTIME.get_or_init(|| Runtime::new().expect("Failed to create Tokio runtime"))
}

// Get or initialize the network play module singleton
pub fn get_network_sync() -> Arc<Mutex<NetworkSyncModule>> {
    NETWORK_PLAY
        .get_or_init(|| Arc::new(Mutex::new(NetworkSyncModule::new())))
        .clone()
}

/// Minimal network play module with just what we need
pub struct NetworkSyncModule {
    network: NetworkModule,
    connected: bool,
    pub client_id: String,
    current_session_id: Option<String>,
    remote_members: Vec<String>,
    pub remote_actors: HashMap<String, Actor>,
    // Track local actors that we own
    local_actors: Vec<String>,
    /// Queue of (message_id, data) tuples
    pub message_queue: VecDeque<(String, Vec<u8>)>,
}

impl NetworkSyncModule {
    pub fn new() -> Self {
        Self {
            network: NetworkModule::new(),
            connected: false,
            client_id: "".to_string(),
            current_session_id: None,
            remote_members: Vec::new(),
            remote_actors: HashMap::new(),
            local_actors: Vec::new(),
            message_queue: VecDeque::new(),
        }
    }

    pub fn connect(&mut self, url: &str) -> Result<()> {
        // Set up the message handler before connecting
        self.network.on_message(move |message| {
            // Use catch_unwind to prevent thread panics
            if let Err(e) = panic::catch_unwind(|| {
                // Process the message
                match process_network_message(&message) {
                    Ok(_) => {}
                    Err(e) => {
                        log::error!("Error processing message: {}", e);
                    }
                }
            }) {
                // Handle any panics that might occur
                log::error!("Panic in message handler: {:?}", e);
            }
        });

        // Connect to the network using the tokio runtime
        let runtime = get_tokio_runtime();
        runtime.block_on(async { self.network.connect(url).await })?;

        Ok(())
    }

    // Join a specific game session
    pub fn join_session(&mut self, session_id: &str) -> Result<()> {
        let join_msg = ClientJoinSessionMessage::new(session_id.to_string());

        // Send join request using the tokio runtime
        let json = serde_json::to_string(&join_msg)?;
        let runtime = get_tokio_runtime();
        runtime.block_on(async { self.network.send_message(&json).await })?;

        // Update local state - will be confirmed by server response
        self.current_session_id = Some(session_id.to_string());
        log::info!("Sent join request for session: {}", session_id);

        Ok(())
    }

    // Leave the current session
    pub fn leave_session(&mut self) -> Result<()> {
        if !self.connected {
            return Err(anyhow::anyhow!("Not connected to server"));
        }

        if let Some(session_id) = &self.current_session_id {
            let leave_msg = ClientBasicMessage::new(BasicMessageType::LeaveSession);
            let json = serde_json::to_string(&leave_msg)?;

            // Send message using the tokio runtime
            let runtime = get_tokio_runtime();
            runtime.block_on(async { self.network.send_message(&json).await })?;

            // Update local state - will be confirmed by server response
            log::info!("Sent request to leave session: {}", session_id);

            // Clear session state
            self.current_session_id = None;
            self.remote_members.clear();
            self.remote_actors.clear();
        }

        Ok(())
    }

    // Disconnect from the server
    pub fn disconnect(&mut self) -> Result<()> {
        if !self.connected {
            return Ok(());
        }

        // Disconnect using the tokio runtime
        let runtime = get_tokio_runtime();
        runtime.block_on(async { self.network.disconnect().await })?;

        self.connected = false;
        self.current_session_id = None;
        self.remote_members.clear();
        self.remote_actors.clear();

        Ok(())
    }

    pub fn register_actor(&mut self, actor_id: String) -> Result<()> {
        if !self.connected {
            return Err(anyhow::anyhow!("Not connected"));
        }

        // Check if we've already registered this actor locally
        if self.local_actors.contains(&actor_id) {
            log::debug!(
                "Actor {} already registered locally, skipping registration",
                actor_id
            );
            return Ok(());
        }

        // Check if this actor is in remote_actors - we should never try to register a remote actor
        if self.remote_actors.contains_key(&actor_id) {
            log::warn!("Attempted to register remote actor {} - REJECTED", actor_id);
            return Err(anyhow::anyhow!("Cannot register a remote actor"));
        }

        log::debug!("Registering actor {} with server", actor_id);
        let register_msg = ClientRegisterActorMessage::new(actor_id.clone());
        let json = serde_json::to_string(&register_msg)?;

        let runtime = get_tokio_runtime();
        runtime.block_on(async { self.network.send_message(&json).await })?;

        // Add to our local actors list
        self.local_actors.push(actor_id);

        Ok(())
    }

    // Sends an actor sync event
    pub fn send_actor_update(&mut self, actor_id: String, actor_data: ActorGameData) -> Result<()> {
        if !self.connected {
            return Err(anyhow::anyhow!("Not connected"));
        }

        // Check if we're trying to update an actor that's in the remote_actors list
        // This would be an error - we should never update actors we don't own
        if self.remote_actors.contains_key(&actor_id) {
            log::warn!(
                "Attempted to send update for remote actor {} that we don't own",
                actor_id
            );
            return Err(anyhow::anyhow!("Cannot update remote actor"));
        }

        // Make sure it's in our local_actors list
        if !self.local_actors.contains(&actor_id) {
            log::debug!("Auto-registering actor {} before update", actor_id);
            self.register_actor(actor_id.clone())?;
        }

        // Extra ownership verification
        if self.current_session_id.is_none() {
            return Err(anyhow::anyhow!("Not in any session"));
        }

        let player_update_msg = ClientActorUpdateMessage::new(actor_id, actor_data);
        let json = serde_json::to_string(&player_update_msg)?;

        // Send player data to the server
        let runtime = get_tokio_runtime();
        runtime.block_on(async { self.network.send_message(&json).await })?;

        Ok(())
    }

    // Send a message to other clients
    pub fn send_registered_message(&mut self, message_id: &str, data: Vec<u8>) -> Result<()> {
        if !self.connected {
            return Err(anyhow::anyhow!("Not connected"));
        }

        if let Some(session_id) = &self.current_session_id {
            let msg = RegisteredMessage::new(message_id.to_string(), data);
            let json = serde_json::to_string(&msg)?;

            // Send message to the server
            let runtime = get_tokio_runtime();
            runtime.block_on(async { self.network.send_message(&json).await })?;

            log::debug!("Sent message '{}' to session {}", message_id, session_id);
        }

        Ok(())
    }

    // Get the size of the next message in the queue
    pub fn get_pending_message_size(&self) -> u32 {
        if let Some((_, data)) = self.message_queue.front() {
            data.len() as u32
        } else {
            0 // No messages
        }
    }

    // Get the next message from the queue
    pub fn get_message(&mut self, buffer: &mut [u8]) -> Option<String> {
        if let Some((message_id, data)) = self.message_queue.pop_front() {
            if buffer.len() >= data.len() {
                buffer[..data.len()].copy_from_slice(&data);
                Some(message_id)
            } else {
                log::error!(
                    "Buffer too small for message: {} > {}",
                    data.len(),
                    buffer.len()
                );
                None
            }
        } else {
            None
        }
    }

    // Queue a message
    fn queue_message(&mut self, message_id: String, data: Vec<u8>) {
        self.message_queue.push_back((message_id, data));
    }
}

// Separate function to process messages that can safely access the global singleton
pub fn process_network_message(message: &str) -> Result<()> {
    // Check if the message is empty or just whitespace
    if message.trim().is_empty() {
        log::debug!("Received empty message, ignoring");
        return Ok(());
    }

    let network_sync = get_network_sync();
    let lock_result = network_sync.lock();
    let mut module = lock_result.unwrap();

    match get_message_type(message) {
        Some(MessageType::ServerWelcome) => {
            if let Ok(msg) = serde_json::from_str::<ServerWelcomeMessage>(message) {
                module.client_id = msg.client_id.clone();
                module.connected = true;
            }
        }

        Some(MessageType::ServerSyncSession) => {
            if !module.connected {
                log::warn!("Received sync session message before welcome message");
                return Ok(());
            }

            if let Ok(msg) = serde_json::from_str::<ServerSyncSessionMessage>(message) {
                module.remote_members = msg.members.clone();

                // Store previous remote actors to detect ownership changes
                let previous_actors = module.remote_actors.clone();
                let previous_local_actors = module.local_actors.clone();

                // Clear remote actors and rebuild with only those owned by others
                module.remote_actors.clear();

                // Track the actors we own according to the server
                let mut server_confirmed_local_actors = Vec::new();

                // Process all actors from server
                for (actor_id, actor) in msg.actors.iter() {
                    if actor.owner_id == module.client_id {
                        // This is a local actor confirmed by the server
                        server_confirmed_local_actors.push(actor_id.clone());
                    } else {
                        // This is a remote actor owned by someone else
                        module.remote_actors.insert(actor_id.clone(), actor.clone());
                    }
                }

                // Reconcile our local actors list with server's view
                // 1. First add any server-confirmed actors not in our list
                for server_actor in &server_confirmed_local_actors {
                    if !module.local_actors.contains(server_actor) {
                        log::info!(
                            "Adding server-confirmed actor {} to local actors list",
                            server_actor
                        );
                        module.local_actors.push(server_actor.clone());
                    }
                }

                // 2. Check for any actors we think we own that the server doesn't recognize
                for local_actor in &previous_local_actors {
                    if !server_confirmed_local_actors.contains(local_actor) {
                        log::warn!("Local actor {} not confirmed by server", local_actor);
                    }
                }

                // Log any ownership changes
                for (actor_id, actor) in &module.remote_actors {
                    if let Some(prev_actor) = previous_actors.get(actor_id) {
                        if prev_actor.owner_id != actor.owner_id {
                            log::warn!(
                                "Ownership change for actor {}: {} -> {}",
                                actor_id,
                                prev_actor.owner_id,
                                actor.owner_id
                            );
                        }
                    }
                }

                log::debug!(
                    "After processing - Local actors: {}, Remote actors: {}",
                    module.local_actors.len(),
                    module.remote_actors.len()
                );
            }
        }

        Some(MessageType::RegisteredMessage) => {
            if !module.connected {
                log::warn!("Received registered message before welcome message");
                return Ok(());
            }

            if let Ok(msg) = serde_json::from_str::<RegisteredMessage>(message) {
                module.queue_message(msg.message_id.clone(), msg.data);
            }
        }

        _ => {
            log::warn!("Received invalid message: {}", message);
        }
    }

    Ok(())
}
