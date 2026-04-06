#include "modding.h"
#include "global.h"
#include "recomputils.h"
#include "string.h"

#define UUID_STRING_LENGTH 37

#ifdef _DEBUG
    #define SERVER_URL "ws://localhost:8080"
#else
    #define SERVER_URL "wss://mm-net.dcvz.io"
#endif

// MARK: - Imports

RECOMP_IMPORT("mm_network_sync", void NS_Init());
RECOMP_IMPORT("mm_network_sync", u8 NS_Connect(const char* host));
RECOMP_IMPORT("mm_network_sync", u8 NS_JoinSession(const char* session));
RECOMP_IMPORT("mm_network_sync", u8 NS_LeaveSession());
RECOMP_IMPORT("mm_network_sync", void NS_SyncActor(Actor* actor, const char* actorId, int isOwnedLocally));
RECOMP_IMPORT("mm_network_sync", const char* NS_GetActorNetworkId(Actor *actor));
RECOMP_IMPORT("mm_network_sync", u32 NS_GetRemoteActorIDs(u32 maxActors, char* idsBuffer, u32 idBufferSize));
RECOMP_IMPORT("mm_network_sync", u32 NS_GetRemoteActorData(const char* actorID, void* dataBuffer));

RECOMP_IMPORT("mm_network_sync", u8 NS_RegisterMessageHandler(const char* messageId, u32 payloadSize, void* callback));
RECOMP_IMPORT("mm_network_sync", u8 NS_EmitMessage(const char* messageId, void* data));

RECOMP_IMPORT("ProxyMM_Notifications", void Notifications_Emit(const char* prefix, const char* msg, const char* suffix));
RECOMP_IMPORT("ProxyMM_CustomActor", s16 CustomActor_Register(ActorProfile* profile));

// MARK: - Forward Declarations

void remote_actors_update(PlayState* play);

// MARK: - Emit / Receive Remote Events

#define MSG_ITEM_USED "item_used"

typedef struct { u32 dummy_data; } ItemUsedMessage;

void handle_item_used_message(const char *senderId, void* data) {
    ItemUsedMessage* msg = (ItemUsedMessage*)data;
    Notifications_Emit(
        "", // Prefix (Purple)
        "Remote actor used item", // Main Message (white)
        "" // Suffix (Blue)
    );
}

// MARK: - Custom Actors

extern ActorProfile RemotePlayer_InitVars;
s16 ACTOR_REMOTE_PLAYER = ACTOR_ID_MAX;

// MARK: - Events

u8 has_connected = 0;
u8 has_loaded_save = 0;
s32 current_scene_id = -1; 
// Track local player actor ID
static char local_player_id[UUID_STRING_LENGTH] = {0};
static u8 has_local_player = 0;

RECOMP_CALLBACK("*", recomp_on_init)
void init_runtime() {
    has_connected = 0;
    has_local_player = 0;
    memset(local_player_id, 0, UUID_STRING_LENGTH);

    NS_Init();
    ACTOR_REMOTE_PLAYER = CustomActor_Register(&RemotePlayer_InitVars);

    // Register message handlers
    NS_RegisterMessageHandler(MSG_ITEM_USED, sizeof(ItemUsedMessage), handle_item_used_message);
}

RECOMP_CALLBACK("*", recomp_on_play_init)
void on_play_init(PlayState* play) {
    if (has_connected) return;
    recomp_printf("Connecting to server...\n");
    has_connected = NS_Connect(SERVER_URL);

    if (has_connected) {
        Notifications_Emit(
            "", // Prefix (Purple)
            "Connected to server", // Main Message (white)
            "" // Suffix (Blue)
        );

        u8 result = NS_JoinSession("test");
        if (result) {
            Notifications_Emit(
                "", // Prefix (Purple)
                "Joined session", // Main Message (white)
                "" // Suffix (Blue)
            );
        } else {
            Notifications_Emit(
                "Failed to join session", // Prefix (Purple)
                "", // Main Message (white)
                "" // Suffix (Blue)
            );
        }
    } else {
        Notifications_Emit(
            "Failed to connect to server", // Prefix (Purple)
            "", // Main Message (white)
            "" // Suffix (Blue)
        );
    }
}

// Process remote players on frame
RECOMP_CALLBACK("*", recomp_on_play_main)
void on_play_main(PlayState* play) {
    static u32 last_update = 0;

    if (!has_connected) return;
    remote_actors_update(play);
}

// MARK: - Hooks

RECOMP_HOOK("FileSelect_LoadGame")
void OnFileSelect_LoadGame(PlayState* play) {
    recomp_printf("FileSelect_LoadGame called\n");
    has_loaded_save = 1;
}

RECOMP_HOOK("Player_Init")
void OnPlayerInit(Actor* thisx, PlayState* play) {
    if (!has_loaded_save) return;

    recomp_printf("Player initialized in scene %d\n", play->sceneId);
    
    // Check if we already have a local player registered
    if (has_local_player) {
        recomp_printf("Local player already exists with ID %s, updating actor reference\n", local_player_id);
        // Use existing ID for this actor
        NS_SyncActor(thisx, local_player_id, 1);
    } else {
        recomp_printf("Registering new local player\n");
        // Register new actor and save the ID
        NS_SyncActor(thisx, NULL, 1);
        const char* actorNetworkId = NS_GetActorNetworkId(thisx);
        if (actorNetworkId != NULL) {
            strcpy(local_player_id, actorNetworkId);
            has_local_player = 1;
            recomp_printf("Saved local player ID: %s\n", local_player_id);
        }
    }

}

RECOMP_HOOK("Player_UseItem")
void OnPlayer_UseItem(PlayState* play, Player* this, ItemId item) {
    if (!has_loaded_save) return;
    
    ItemUsedMessage msg;
    msg.dummy_data = 7;
    recomp_printf("Player_UseItem called\n");
    NS_EmitMessage(MSG_ITEM_USED, &msg);
}

// MARK: - Remote Actor Processing

#define MAX_REMOTE_ACTORS 32 // matches the mod's MAX_SYNCED_ACTORS
static char remoteActorIds[MAX_REMOTE_ACTORS][UUID_STRING_LENGTH];
static u32 remoteActorCount = 0;

// Checks whether we need to create or destroy actors
void remote_actors_update(PlayState* play) {
    // Clear the buffer to avoid garbage values
    memset(remoteActorIds, 0, sizeof(remoteActorIds));
    
    // Call with the proper buffer size parameter (UUID_STRING_LENGTH)
    remoteActorCount = NS_GetRemoteActorIDs(MAX_REMOTE_ACTORS, (char*)remoteActorIds, UUID_STRING_LENGTH);

    // Create actors for new remote entities (only if we have any)
    for (u32 i = 0; i < remoteActorCount; i++) {
        // 1. Check if entity already has an actor
        bool remoteActorAlreadyCreated = false;
        Actor* actor = play->actorCtx.actorLists[ACTORCAT_PLAYER].first;

        // Find actor with given ID
        while (actor != NULL) {
            if (actor->id == ACTOR_REMOTE_PLAYER) {
                const char* actorNetworkId = NS_GetActorNetworkId(actor);
                const char* remoteId = remoteActorIds[i];

                if (actorNetworkId != NULL && remoteId != NULL && strcmp(actorNetworkId, remoteId) == 0) {
                    remoteActorAlreadyCreated = true;
                    break;
                }
            }

            actor = actor->next;
        }

        // 2. If actor not found, create new actor
        if (!remoteActorAlreadyCreated) {
            const char* remoteId = remoteActorIds[i];
            recomp_printf("Creating actor for remote entity %s\n", remoteId);
            actor = Actor_SpawnAsChildAndCutscene(&play->actorCtx, play, ACTOR_REMOTE_PLAYER, -9999.0f, -9999.0f, -9999.0f, 0, 0, 0, 0, 0, 0, 0);
            NS_SyncActor(actor, remoteId, 0);
        }
    }

    // Check for entities that no longer exist and remove their actors
    Actor* actor = play->actorCtx.actorLists[ACTORCAT_PLAYER].first;
    while (actor != NULL) {
        Actor* next = actor->next; // Save next pointer as we may delete this actor

        if (actor->id == ACTOR_REMOTE_PLAYER) {
            const char* actorNetworkId = NS_GetActorNetworkId(actor);
            if (actorNetworkId == NULL) {
                Actor_Kill(actor);
                recomp_printf("Removed remote actor with NULL ID\n");
            } else {
                bool stillExists = false;

                // Only check if there are remote actors to compare against
                if (remoteActorCount > 0) {
                    for (u32 i = 0; i < remoteActorCount; i++) {
                        if (remoteActorIds[i][0] != '\0' && strcmp(actorNetworkId, remoteActorIds[i]) == 0) {
                            stillExists = true;
                            break;
                        }
                    }
                }

                if (!stillExists) {
                    Actor_Kill(actor);
                    recomp_printf("Removed remote actor %s\n", actorNetworkId);
                }
            }
        }

        actor = next;
    }
}
