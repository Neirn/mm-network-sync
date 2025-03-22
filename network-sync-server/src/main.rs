use clap::Parser;
use env_logger::Builder;
use futures_util::{SinkExt, StreamExt};
use log::{debug, error, info, warn};
use network_sync_rs::{
    messages::{
        get_message_type, BasicMessageType, ClientActorUpdateMessage, ClientBasicMessage, ClientJoinSessionMessage,
        ClientRegisterActorMessage, ClientUpdateStateMessage, MessageType, RegisteredMessage,
        ServerSyncSessionMessage, ServerWelcomeMessage,
    },
    Actor,
};
use serde::{Deserialize, Serialize};
use std::{
    collections::HashMap,
    net::SocketAddr,
    sync::{Arc, Mutex},
};
use tokio::{
    net::{TcpListener, TcpStream},
    sync::broadcast,
};
use tokio_tungstenite::{accept_async, tungstenite::protocol::Message};
use uuid::Uuid;

type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;
type StateHandle = Arc<Mutex<ServerState>>;
type BroadcastSender = broadcast::Sender<(String, String)>;

// MARK: - Command line arguments
#[derive(Parser, Debug)]
#[clap(author, version, about)]
struct Args {
    /// Port to listen on
    #[clap(short, long, default_value = "8080")]
    port: u16,
}

// MARK: - Structs
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ClientMetadata {
    pub scene: i32,
    pub join_time: u64,
    // Add more metadata fields as needed
}

// Server state
struct ServerState {
    /// Map from connection ID to session ID
    connections: HashMap<String, Option<String>>,
    /// Map from session ID to set of connection IDs
    sessions: HashMap<String, Vec<String>>,
    /// Map from connection ID to client metadata
    client_metadata: HashMap<String, ClientMetadata>,
    /// Map from session ID to actors in that session
    session_actors: HashMap<String, HashMap<String, Actor>>,
}

impl ServerState {
    fn new() -> Self {
        Self {
            connections: HashMap::new(),
            sessions: HashMap::new(),
            client_metadata: HashMap::new(),
            session_actors: HashMap::new(),
        }
    }

    fn register_connection(&mut self, id: &str, scene: i32) {
        info!("Registering connection: {}", id);
        self.connections.insert(id.to_string(), None);
        self.client_metadata.insert(
            id.to_string(),
            ClientMetadata {
                scene,
                join_time: chrono::Utc::now().timestamp() as u64,
            },
        );
    }

    fn remove_connection(&mut self, id: &str) {
        self.leave_session(id);
        self.connections.remove(id);
        self.client_metadata.remove(id);
    }

    fn join_session(
        &mut self,
        connection_id: &str,
        session_id: &str,
    ) -> (Vec<String>, HashMap<String, Actor>) {
        // Update connection's session
        self.connections
            .insert(connection_id.to_string(), Some(session_id.to_string()));

        // Add to session
        let session_members = self
            .sessions
            .entry(session_id.to_string())
            .or_insert_with(Vec::new);

        if !session_members.contains(&connection_id.to_string()) {
            session_members.push(connection_id.to_string());
        }

        // Initialize session actors if needed
        let session_actors = self
            .session_actors
            .entry(session_id.to_string())
            .or_insert_with(HashMap::new);

        (session_members.clone(), session_actors.clone())
    }

    fn get_session_actors(&self, session_id: &str) -> HashMap<String, Actor> {
        self.session_actors
            .get(session_id)
            .cloned()
            .unwrap_or_default()
    }

    fn leave_session(&mut self, connection_id: &str) -> Option<String> {
        if let Some(Some(session_id)) = self.connections.get(connection_id) {
            let session_id = session_id.clone();
            // Update connection to no longer be in a session
            self.connections.insert(connection_id.to_string(), None);

            // Remove from session actors if this is the owner
            if let Some(actors) = self.session_actors.get_mut(&session_id) {
                actors.retain(|_id, actor| actor.owner_id != connection_id);
            }

            // Remove from session
            if let Some(connections) = self.sessions.get_mut(&session_id) {
                connections.retain(|cid| cid != connection_id);
                // Clean up empty sessions
                if connections.is_empty() {
                    self.sessions.remove(&session_id);
                }
            }

            return Some(session_id);
        }
        None
    }

    fn get_session_members(&self, session_id: &str) -> Vec<String> {
        self.sessions.get(session_id).cloned().unwrap_or_default()
    }

    fn get_client_session(&self, connection_id: &str) -> Option<String> {
        self.connections.get(connection_id).and_then(|s| s.clone())
    }

    fn with_session<F, T>(&self, connection_id: &str, f: F) -> Option<T>
    where
        F: FnOnce(&str) -> T,
    {
        self.get_client_session(connection_id)
            .map(|session_id| f(&session_id))
    }
}

// Message handling functions
async fn handle_join_session(
    msg: ClientJoinSessionMessage,
    connection_id: &str,
    state: &StateHandle,
    tx: &BroadcastSender,
) -> Result<()> {
    // Lock state once and do all operations
    let (members, actors) = {
        let mut state = state.lock().unwrap();
        let (members, actors) = state.join_session(connection_id, &msg.session_id);

        debug!(
            "Session {} has {} members and {} actors",
            msg.session_id,
            members.len(),
            actors.len()
        );

        for (actor_id, actor) in &actors {
            debug!("Actor {}: owner_id = {}", actor_id, actor.owner_id);
        }

        (members, actors)
    };

    // Broadcast to all session members with their own client_id
    for member in &members {
        let individual_msg = ServerSyncSessionMessage::new(members.clone(), actors.clone());
        let msg_str = serde_json::to_string(&individual_msg)?;
        debug!("Sending sync message to member {}", member);
        tx.send((member.clone(), msg_str))?;
    }

    info!(
        "Client {} joined session {}",
        connection_id, &msg.session_id
    );
    Ok(())
}

async fn handle_leave_session(
    connection_id: &str,
    state: &StateHandle,
    tx: &BroadcastSender,
) -> Result<()> {
    // Lock state once for all operations
    let (session_id_opt, members_opt, actors_opt) = {
        let mut state = state.lock().unwrap();
        let session_id_opt = state.leave_session(connection_id);

        if let Some(ref session_id) = session_id_opt {
            let members = state.get_session_members(session_id);
            let actors = state.get_session_actors(session_id);
            (session_id_opt, Some(members), Some(actors))
        } else {
            (None, None, None)
        }
    };

    if let (Some(session_id), Some(members), Some(actors)) =
        (session_id_opt, members_opt, actors_opt)
    {
        let msg = ServerSyncSessionMessage::new(members.clone(), actors.clone());

        // Broadcast to remaining session members with their own client_id
        for member in &members {
            let msg_str = serde_json::to_string(&msg)?;
            tx.send((member.clone(), msg_str))?;
        }

        info!("Client {} left session {}", connection_id, session_id);
    }

    Ok(())
}

async fn handle_register_actor(
    msg: ClientRegisterActorMessage,
    connection_id: &str,
    state: &StateHandle,
) -> Result<()> {
    let mut state = state.lock().unwrap();

    if let Some(session_id) = state.get_client_session(connection_id) {
        if let Some(actors) = state.session_actors.get_mut(&session_id) {
            let actor_id = msg.actor_id.clone();

            if actor_id.is_empty() {
                warn!(
                    "Client {} tried to register empty actor ID - REJECTED",
                    connection_id
                );
                return Ok(());
            }

            // Check if actor already exists
            if let Some(existing_actor) = actors.get(&actor_id) {
                // If actor exists but has a different owner, log more details and reject
                if existing_actor.owner_id != connection_id {
                    warn!(
                        "Client {} tried to register actor {} already owned by {} - REJECTED",
                        connection_id, actor_id, existing_actor.owner_id
                    );

                    // Log all current actors and their owners for debugging
                    debug!("Current actors in session {}:", session_id);
                    for (id, act) in actors.iter() {
                        debug!("  - Actor {}: owner = {}", id, act.owner_id);
                    }

                    return Ok(());
                }

                // Actor exists and is owned by this client - this is a re-registration (ok)
                debug!(
                    "Re-registering actor {} for client {}",
                    actor_id, connection_id
                );

                return Ok(());
            }

            // New actor registration - check if this client already owns too many actors
            let client_owned_count = actors
                .values()
                .filter(|actor| actor.owner_id == connection_id)
                .count();

            debug!(
                "Client {} currently owns {} actors",
                connection_id, client_owned_count
            );

            // New actor registration
            debug!(
                "Registering new actor {} for client/owner {}",
                actor_id, connection_id
            );
            actors.insert(
                actor_id.clone(),
                Actor::new(actor_id.clone(), connection_id.to_string(), None),
            );

            // Get updated actors and members for broadcast
            let session_actors = state.get_session_actors(&session_id);
            debug!(
                "Session {} now has {} actors after registration",
                session_id,
                session_actors.len()
            );

            // Log a detailed breakdown of all actors by owner
            let mut owners_map: HashMap<String, Vec<String>> = HashMap::new();
            for (id, actor) in &session_actors {
                owners_map
                    .entry(actor.owner_id.clone())
                    .or_insert_with(Vec::new)
                    .push(id.clone());
            }

            for (owner, actor_ids) in &owners_map {
                debug!(
                    "Owner {} has {} actors: {:?}",
                    owner,
                    actor_ids.len(),
                    actor_ids
                );
            }

            let members = state.get_session_members(&session_id);
            debug!(
                "Session {} has {} members: {:?}",
                session_id,
                members.len(),
                members
            );

            info!("Registered actor {} for client {}", actor_id, connection_id);
        } else {
            warn!("No actors collection found for session {}", session_id);
        }
    } else {
        warn!("Client {} is not in any session", connection_id);
    }

    Ok(())
}

async fn handle_actor_update(
    msg: ClientActorUpdateMessage,
    connection_id: &str,
    state: &StateHandle,
    tx: &BroadcastSender,
) -> Result<()> {
    // Lock once for all operations
    let (session_id_opt, session_actors_opt, members_opt) = {
        let mut state = state.lock().unwrap();
        let session_id_opt = state.get_client_session(connection_id);

        debug!(
            "Actor update from client {} for actor {}",
            connection_id, msg.actor_id
        );

        let mut result = (None, None, None);

        if let Some(ref session_id) = session_id_opt {
            if let Some(actors) = state.session_actors.get_mut(session_id) {
                // Log all actors in this session before processing the update
                debug!(
                    "Before update, session {} has {} actors:",
                    session_id,
                    actors.len()
                );
                for (id, actor) in actors.iter() {
                    debug!("  - Actor {}: owner = {}", id, actor.owner_id);
                }

                if let Some(actor) = actors.get_mut(&msg.actor_id) {
                    debug!("Updating actor {} owned by {}", actor.id, actor.owner_id);

                    // CRITICAL: Verify ownership - only allow updates from the owner
                    if actor.owner_id != connection_id {
                        warn!(
                            "Client {} is trying to update actor {} owned by {} - REJECTED",
                            connection_id, msg.actor_id, actor.owner_id
                        );

                        // This might indicate a critical ownership confusion - log all details
                        debug!("Client {} currently owns these actors:", connection_id);
                        for (id, owned_actor) in actors.iter() {
                            if owned_actor.owner_id == connection_id {
                                debug!("  - Actor {}", id);
                            }
                        }

                        return Ok(());
                    }

                    actor.update(Some(msg.actor_data));
                    debug!(
                        "Successfully updated actor {} owned by {}",
                        actor.id, actor.owner_id
                    );

                    // After updating, get the session actors and members to broadcast
                    let session_actors = state.get_session_actors(session_id);
                    debug!(
                        "Session {} now has {} total actors:",
                        session_id,
                        session_actors.len()
                    );
                    for (id, actor) in &session_actors {
                        debug!("  - Actor {}: owner = {}", id, actor.owner_id);
                    }

                    let members = state.get_session_members(session_id);
                    debug!(
                        "Session {} has {} members: {:?}",
                        session_id,
                        members.len(),
                        members
                    );

                    result = (session_id_opt, Some(session_actors), Some(members));
                } else {
                    warn!(
                        "Client {} attempted to update non-existent actor {} - creating it",
                        connection_id, msg.actor_id
                    );

                    // If the actor doesn't exist, we'll create it and assign ownership to this client
                    // This helps recover from desync situations
                    actors.insert(
                        msg.actor_id.clone(),
                        Actor::new(
                            msg.actor_id.clone(),
                            connection_id.to_string(),
                            Some(msg.actor_data),
                        ),
                    );

                    let session_actors = state.get_session_actors(session_id);
                    let members = state.get_session_members(session_id);
                    result = (session_id_opt, Some(session_actors), Some(members));

                    info!(
                        "Auto-registered actor {} for client {}",
                        msg.actor_id, connection_id
                    );
                }
            } else {
                warn!("No actors found for session {}", session_id);
            }
        } else {
            warn!("Client {} is not in any session", connection_id);
        }

        result
    };

    if let (Some(session_id), Some(actors), Some(members)) =
        (session_id_opt, session_actors_opt, members_opt)
    {
        // Make sure we're sending all actors to each member
        if actors.is_empty() {
            warn!("No actors to broadcast for session {}", session_id);
        } else {
            debug!(
                "Broadcasting {} actors to {} members in session {}",
                actors.len(),
                members.len(),
                session_id
            );

            // Count actors by owner for debugging
            let mut owners_map: HashMap<String, Vec<String>> = HashMap::new();
            for (id, actor) in &actors {
                owners_map
                    .entry(actor.owner_id.clone())
                    .or_insert_with(Vec::new)
                    .push(id.clone());
            }

            for (owner, actor_ids) in &owners_map {
                debug!(
                    "Owner {} has {} actors: {:?}",
                    owner,
                    actor_ids.len(),
                    actor_ids
                );
            }
        }

        // Broadcast actor update to all session members
        let update_msg = ServerSyncSessionMessage::new(members.clone(), actors);
        let msg_str = serde_json::to_string(&update_msg)?;

        // Instead of sending to all members at once, send each client an individual message
        for member in &members {
            debug!("Sending actor update to member {}", member);
            tx.send((member.clone(), msg_str.clone()))?;
        }
    } else {
        warn!("Failed to get session data for client {}", connection_id);
    }

    Ok(())
}

async fn handle_update_state(
    msg: ClientUpdateStateMessage,
    connection_id: &str,
    state: &StateHandle,
) -> Result<()> {
    let mut state = state.lock().unwrap();
    if let Some(client_metadata) = state.client_metadata.get_mut(connection_id) {
        client_metadata.scene = msg.scene;
        debug!(
            "Updated scene state for client {} to {}",
            connection_id, msg.scene
        );
    }
    Ok(())
}

async fn handle_registered_message(
    text: String,
    connection_id: &str,
    state: &StateHandle,
    tx: &BroadcastSender,
) -> Result<()> {
    // Get session and members in one lock
    let members_to_broadcast = {
        let state = state.lock().unwrap();
        state
            .with_session(connection_id, |session_id| {
                state.get_session_members(session_id)
            })
            .unwrap_or_default()
    };

    // Broadcast to all members EXCEPT the sender
    for member in &members_to_broadcast {
        // Skip sending the message back to the original sender
        if member != connection_id {
            tx.send((member.clone(), text.clone()))?;
        }
    }

    Ok(())
}

// Main connection handler
async fn handle_connection(
    stream: TcpStream,
    connection_id: String,
    state: StateHandle,
    tx: BroadcastSender,
) -> Result<()> {
    // Accept WebSocket connection
    let ws_stream = accept_async(stream).await?;
    let (mut ws_sender, mut ws_receiver) = ws_stream.split();

    // Send welcome message
    let welcome_msg = ServerWelcomeMessage::new(connection_id.clone());
    let welcome_msg_str = serde_json::to_string(&welcome_msg)?;
    ws_sender.send(Message::Text(welcome_msg_str)).await?;

    // Subscribe to broadcast messages
    let mut rx = tx.subscribe();

    // Create task to forward broadcasts to this connection
    let conn_id = connection_id.clone();
    let forward_task = tokio::spawn(async move {
        while let Ok((target, msg)) = rx.recv().await {
            // Send if broadcast is for all or specifically for this connection
            if target == "*" || target == conn_id {
                if let Err(e) = ws_sender.send(Message::Text(msg)).await {
                    error!("Failed to forward message: {}", e);
                    break;
                }
            }
        }
    });

    // Process incoming messages
    let clone_conn_id = connection_id.clone();
    while let Some(result) = ws_receiver.next().await {
        let msg = match result {
            Ok(msg) => msg,
            Err(e) => {
                error!("Error receiving message: {}", e);
                break;
            }
        };

        debug!("Received message from {}", &clone_conn_id);

        if let Message::Text(text) = msg {
            match get_message_type(&text) {
                Some(MessageType::ClientJoinSession) => {
                    if let Ok(msg) = serde_json::from_str::<ClientJoinSessionMessage>(&text) {
                        if let Err(e) = handle_join_session(msg, &clone_conn_id, &state, &tx).await
                        {
                            error!("Error handling join session: {}", e);
                        }
                    }
                }

                Some(MessageType::ClientBasic) => {
                    if let Ok(msg) = serde_json::from_str::<ClientBasicMessage>(&text) {
                        match msg.basic_type {
                            BasicMessageType::LeaveSession => {
                                if let Err(e) =
                                    handle_leave_session(&connection_id, &state, &tx).await
                                {
                                    error!("Error handling leave session: {}", e);
                                }
                            }
                        }
                    }
                }

                Some(MessageType::ClientRegisterActor) => {
                    if let Ok(msg) = serde_json::from_str::<ClientRegisterActorMessage>(&text) {
                        if let Err(e) = handle_register_actor(msg, &connection_id, &state).await {
                            error!("Error handling register actor: {}", e);
                        }
                    }
                }

                Some(MessageType::ClientActorUpdate) => {
                    if let Ok(msg) = serde_json::from_str::<ClientActorUpdateMessage>(&text) {
                        if let Err(e) = handle_actor_update(msg, &connection_id, &state, &tx).await
                        {
                            error!("Error handling actor update: {}", e);
                        }
                    }
                }

                Some(MessageType::ClientUpdateState) => {
                    if let Ok(msg) = serde_json::from_str::<ClientUpdateStateMessage>(&text) {
                        if let Err(e) = handle_update_state(msg, &connection_id, &state).await {
                            error!("Error handling state update: {}", e);
                        }
                    }
                }

                Some(MessageType::RegisteredMessage) => {
                    if let Ok(_) = serde_json::from_str::<RegisteredMessage>(&text) {
                        if let Err(e) =
                            handle_registered_message(text, &connection_id, &state, &tx).await
                        {
                            error!("Error handling registered message: {}", e);
                        }
                    }
                }

                _ => {
                    warn!("Received invalid message from {}: {}", connection_id, text);
                }
            }
        }
    }

    // Cancel the forward task when the connection closes
    forward_task.abort();

    // Handle disconnection notification
    let (session_id_opt, members_opt, actors_opt) = {
        let state = state.lock().unwrap();
        let session_id_opt = state.get_client_session(&connection_id);

        if let Some(ref session_id) = session_id_opt {
            let members = state
                .get_session_members(session_id)
                .into_iter()
                .filter(|id| id != &connection_id)
                .collect::<Vec<_>>();

            if !members.is_empty() {
                let actors = state.get_session_actors(session_id);
                (session_id_opt, Some(members), Some(actors))
            } else {
                (session_id_opt, None, None)
            }
        } else {
            (None, None, None)
        }
    };

    if let (Some(_), Some(members), Some(actors)) = (session_id_opt, members_opt, actors_opt) {
        // Broadcast to remaining members with their specific client IDs
        info!("Broadcasting disconnection message to remaining members");
        let msg = ServerSyncSessionMessage::new(members.clone(), actors.clone());

        for member in &members {
            let msg_str = serde_json::to_string(&msg)?;
            let _ = tx.send((member.clone(), msg_str));
        }
    }

    Ok(())
}

#[tokio::main]
async fn main() -> Result<()> {
    let mut builder = Builder::from_default_env();

    #[cfg(debug_assertions)]
    builder.filter_level(log::LevelFilter::Debug);
    #[cfg(not(debug_assertions))]
    builder.filter_level(log::LevelFilter::Info);

    builder.init();

    // Parse command line arguments
    let args = Args::parse();
    let addr = SocketAddr::from(([0, 0, 0, 0], args.port));

    // Set up server
    let listener = TcpListener::bind(&addr).await?;
    info!("Listening on: {}", addr);

    // Create shared server state
    let state = Arc::new(Mutex::new(ServerState::new()));

    // Create broadcast channel for server messages
    let (tx, _) = broadcast::channel::<(String, String)>(100);

    // Accept connections
    while let Ok((stream, addr)) = listener.accept().await {
        info!("New connection from: {}", addr);

        // Clone handles for this connection
        let tx = tx.clone();
        let state = Arc::clone(&state);

        // Generate a unique ID for this connection
        let connection_id = Uuid::new_v4().to_string();

        // Register connection
        {
            let mut state = state.lock().unwrap();
            state.register_connection(&connection_id, -1);
        }

        // Clone state for disconnect handling
        let disconnect_state = Arc::clone(&state);
        let disconnect_id = connection_id.clone();

        // Spawn a task to handle this connection
        tokio::spawn(async move {
            if let Err(e) = handle_connection(stream, connection_id.clone(), state, tx).await {
                error!("Error handling connection {}: {}", disconnect_id, e);
            }

            // On disconnect, clean up
            let mut state = disconnect_state.lock().unwrap();
            state.remove_connection(&connection_id);
            info!("Connection closed: {}", connection_id);
        });
    }

    Ok(())
}
