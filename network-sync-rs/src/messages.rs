use serde::{Deserialize, Serialize};
use std::collections::HashMap;

use crate::structs::{Actor, ActorGameData};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum MessageType {
    // Client
    ClientBasic,
    ClientJoinSession,
    ClientRegisterActor,
    ClientActorUpdate,
    ClientUpdateState,

    // Server
    ServerWelcome,
    ServerSyncSession,
    ServerSyncActors,

    // Shared
    RegisteredMessage,
}

// MARK: - Client Messages

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub enum BasicMessageType {
    LeaveSession,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ClientBasicMessage {
    pub message_type: MessageType,
    pub basic_type: BasicMessageType,
}

impl ClientBasicMessage {
    pub fn new(basic_type: BasicMessageType) -> Self {
        Self {
            message_type: MessageType::ClientBasic,
            basic_type,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ClientJoinSessionMessage {
    pub message_type: MessageType,
    pub session_id: String,
}

impl ClientJoinSessionMessage {
    pub fn new(session_id: String) -> Self {
        Self {
            message_type: MessageType::ClientJoinSession,
            session_id,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ClientRegisterActorMessage {
    pub message_type: MessageType,
    pub actor_id: String,
}

impl ClientRegisterActorMessage {
    pub fn new(actor_id: String) -> Self {
        Self {
            message_type: MessageType::ClientRegisterActor,
            actor_id,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ClientActorUpdateMessage {
    pub message_type: MessageType,
    pub actor_id: String,
    pub actor_data: ActorGameData,
}

impl ClientActorUpdateMessage {
    pub fn new(actor_id: String, actor_data: ActorGameData) -> Self {
        Self {
            message_type: MessageType::ClientActorUpdate,
            actor_id,
            actor_data,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ClientUpdateStateMessage {
    pub message_type: MessageType,
    pub scene: i32,
}

impl ClientUpdateStateMessage {
    pub fn new(scene: i32) -> Self {
        Self {
            message_type: MessageType::ClientUpdateState,
            scene,
        }
    }
}

// MARK: - Server Messages

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ServerWelcomeMessage {
    pub message_type: MessageType,
    pub client_id: String,
}

impl ServerWelcomeMessage {
    pub fn new(client_id: String) -> Self {
        Self {
            message_type: MessageType::ServerWelcome,
            client_id,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ServerSyncSessionMessage {
    pub message_type: MessageType,
    pub members: Vec<String>,
    pub actors: HashMap<String, Actor>,
}

impl ServerSyncSessionMessage {
    pub fn new(members: Vec<String>, actors: HashMap<String, Actor>) -> Self {
        Self {
            message_type: MessageType::ServerSyncSession,
            members,
            actors,
        }
    }
}

// MARK: - Shared Messages

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RegisteredMessage {
    pub message_type: MessageType,
    pub message_id: String,
    pub data: Vec<u8>,
}

impl RegisteredMessage {
    pub fn new(message_id: String, data: Vec<u8>) -> Self {
        Self {
            message_type: MessageType::RegisteredMessage,
            message_id,
            data,
        }
    }
}

// Helpers

pub fn get_message_type(json: &str) -> Option<MessageType> {
    #[derive(Deserialize)]
    struct TypeExtractor {
        message_type: MessageType,
    }

    serde_json::from_str::<TypeExtractor>(json)
        .ok()
        .map(|e| e.message_type)
}
