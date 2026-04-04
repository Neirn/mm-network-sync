use n64_recomp::{N64MemoryIO, Vec3f, Vec3s};
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Actor {
    pub id: String,
    pub owner_id: String,
    pub data: Option<ActorGameData>,
    pub last_update: u64, // Unix timestamp in seconds
}

impl Actor {
    pub fn new(id: String, owner_id: String, data: Option<ActorGameData>) -> Self {
        Actor {
            id,
            owner_id,
            data,
            last_update: chrono::Utc::now().timestamp() as u64,
        }
    }

    pub fn update(&mut self, data: Option<ActorGameData>) {
        self.data = data;
        self.last_update = chrono::Utc::now().timestamp() as u64;
    }
}

#[repr(C)]
#[derive(Debug, Clone, Serialize, Deserialize, N64MemoryIO)]
pub struct ActorGameData {
    pub world_position: Vec3f,
    pub shape_y_offset: f32,
    pub shape_face: i16,
    pub shape_rotation: Vec3s,
    pub upper_limb_rot: Vec3s,
    pub joint_table: [Vec3s; 24],
    pub current_mask: i8,
    pub current_shield: i8,
    pub model_group: i8,
    pub model_anim_type: i8,
    pub transformation: i8,
    pub movement_flags: i8,
    pub is_shielding: i8,
}
