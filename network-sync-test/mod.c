#include "modding.h"
#include "global.h"
#include "recomputils.h"
#include "string.h"
#include "yazmtcorelib_api.h"
#include "playermodelmanager_api.h"
#include "recompconfig.h"
#include "appearancedatamanager.h"
#include "messages.h"

#ifdef _DEBUG
    #define SERVER_URL "ws://localhost:8080"
#else
    #define SERVER_URL "wss://mm-net.dcvz.io"
#endif

// MARK: - Imports

RECOMP_IMPORT("mm_network_sync", void NS_Init());
RECOMP_IMPORT("mm_network_sync", u8 NS_Connect(const char *host));
RECOMP_IMPORT("mm_network_sync", u8 NS_JoinSession(const char *session));
RECOMP_IMPORT("mm_network_sync", u8 NS_LeaveSession());
RECOMP_IMPORT("mm_network_sync", void NS_SyncActor(Actor *actor, const char *actorId, int isOwnedLocally));
RECOMP_IMPORT("mm_network_sync", const char *NS_GetActorNetworkId(Actor *actor));
RECOMP_IMPORT("mm_network_sync", u32 NS_GetRemoteActorIDs(u32 maxActors, char *idsBuffer, u32 idBufferSize));
RECOMP_IMPORT("mm_network_sync", u32 NS_GetRemoteActorData(const char *actorID, void *dataBuffer));

RECOMP_IMPORT("mm_network_sync", u8 NS_RegisterMessageHandler(const char *messageId, u32 payloadSize, void *callback));
RECOMP_IMPORT("mm_network_sync", u8 NS_EmitMessage(const char *messageId, void *data));

RECOMP_IMPORT("ProxyMM_Notifications", void Notifications_Emit(const char *prefix, const char *msg, const char *suffix));
RECOMP_IMPORT("ProxyMM_CustomActor", s16 CustomActor_Register(ActorProfile *profile));

RECOMP_IMPORT(YAZMT_PMM_MOD_NAME, bool PlayerModelManager_ActorAppearanceData_assignModel(ActorAppearanceDataHandle h, PlayerModelManagerModelType type, const char *internalName));
RECOMP_IMPORT(YAZMT_PMM_MOD_NAME, bool PlayerModelManager_ActorAppearanceData_assignDataToActor(Actor *actor, ActorAppearanceDataHandle h));
RECOMP_IMPORT(YAZMT_PMM_MOD_NAME, bool PlayerModelManager_Actor_setTunicColor(Actor *actor, Color_RGBA8 color));

// MARK: - Forward Declarations

void remote_actors_update(PlayState *play);

// MARK: - Emit / Receive Remote Events

void handle_item_used_message(void *data) {
    ItemUsedMessage *msg = (ItemUsedMessage *)data;
    Notifications_Emit(
        "",                       // Prefix (Purple)
        "Remote actor used item", // Main Message (white)
        ""                        // Suffix (Blue)
    );
}

void handleModelSetMessage(void *data) {
    ModelSetMessage *msg = data;

    msg->id[UUID_STRING_LENGTH - 1] = '\0';
    msg->modelName[MAX_MODEL_NAME_LEN - 1] = '\0';

    if (msg->id[0] != '\0') {
        ActorAppearanceDataHandle h = AppearanceDataManager_getAppearanceDataAndRefreshLifetime(msg->id);

        if (h) {
            PlayerModelManager_ActorAppearanceData_assignModel(h, msg->modelType, msg->modelName);
        }
    }
}

void handleModelRemovedMessage(void *data) {
    ModelRemovedMessage *msg = data;

    msg->id[UUID_STRING_LENGTH - 1] = '\0';

    if (msg->id[0] != '\0') {
        ActorAppearanceDataHandle h = AppearanceDataManager_getAppearanceDataAndRefreshLifetime(msg->id);

        if (h) {
            PlayerModelManager_ActorAppearanceData_assignModel(h, msg->modelType, NULL);
        }
    }
}

// MARK: - Custom Actors

extern ActorProfile RemotePlayer_InitVars;
s16 ACTOR_REMOTE_PLAYER = ACTOR_ID_MAX;

// MARK: - Events

u8 gHasConnected;
// Track local player actor ID
char gLocalPlayerId[UUID_STRING_LENGTH];
bool gHasLocalPlayer;

RECOMP_CALLBACK("*", recomp_on_init) void init_runtime() {
    NS_Init();
    ACTOR_REMOTE_PLAYER = CustomActor_Register(&RemotePlayer_InitVars);

    // Register message handlers
    NS_RegisterMessageHandler(MSG_ITEM_USED, sizeof(ItemUsedMessage), handle_item_used_message);

    NS_RegisterMessageHandler(MSG_MODEL_SET, sizeof(ModelSetMessage), handleModelSetMessage);

    NS_RegisterMessageHandler(MSG_MODEL_REMOVED, sizeof(ModelSetMessage), handleModelRemovedMessage);

    AppearanceDataManager_init();
}

RECOMP_CALLBACK("*", recomp_on_play_init) void on_play_init(PlayState *play) {
    if (gHasConnected)
        return;
    recomp_printf("Connecting to server...\n");

    char *ip = recomp_get_config_string("server_ip");

    if (ip) {
        gHasConnected = NS_Connect(ip);
    }

    if (gHasConnected) {
        Notifications_Emit(
            "",                    // Prefix (Purple)
            "Connected to server", // Main Message (white)
            ""                     // Suffix (Blue)
        );

        u8 result = NS_JoinSession("test");
        if (result) {
            Notifications_Emit(
                "",               // Prefix (Purple)
                "Joined session", // Main Message (white)
                ""                // Suffix (Blue)
            );
        } else {
            Notifications_Emit(
                "Failed to join session", // Prefix (Purple)
                "",                       // Main Message (white)
                ""                        // Suffix (Blue)
            );
        }
    } else {
        Notifications_Emit(
            "Failed to connect to server", // Prefix (Purple)
            "",                            // Main Message (white)
            ""                             // Suffix (Blue)
        );
    }
}

// Process remote players on frame
RECOMP_CALLBACK("*", recomp_on_play_main) void on_play_main(PlayState *play) {
    if (!gHasConnected)
        return;
    remote_actors_update(play);
}

RECOMP_CALLBACK(YAZMT_PMM_MOD_NAME, onMainModelChanged) void updateNetworkedPlayerAppearance(PlayerModelManagerModelType modelType, const char *internalNameOrNull) {
    AppearanceDataManager_setLocalInternalName(modelType, internalNameOrNull);
}

// MARK: - Hooks

RECOMP_HOOK("FileSelect_LoadGame") void OnFileSelect_LoadGame(PlayState *play) {
    recomp_printf("FileSelect_LoadGame called\n");
}

RECOMP_HOOK("Player_Init") void OnPlayerInit(Actor *thisx, PlayState *play) {
    // recomp_printf("Player initialized in scene %d\n", play->sceneId);

    if (GET_PLAYER(play) == NULL || thisx == &GET_PLAYER(play)->actor) {
        // Check if we already have a local player registered
        if (gHasLocalPlayer) {
            recomp_printf("Local player already exists with ID %s, updating actor reference\n", gLocalPlayerId);
            // Use existing ID for this actor
            NS_SyncActor(thisx, gLocalPlayerId, 1);
        } else {
            recomp_printf("Registering new local player\n");
            // Register new actor and save the ID
            NS_SyncActor(thisx, NULL, 1);
            const char *actorNetworkId = NS_GetActorNetworkId(thisx);
            if (actorNetworkId != NULL) {
                strcpy(gLocalPlayerId, actorNetworkId);
                gHasLocalPlayer = true;
                recomp_printf("Saved local player ID: %s\n", gLocalPlayerId);
            }
        }
    }
}

RECOMP_HOOK("Player_UseItem") void OnPlayer_UseItem(PlayState *play, Player *this, ItemId item) {
    ItemUsedMessage msg;
    msg.dummy_data = 7;
    //recomp_printf("Player_UseItem called\n");
    //NS_EmitMessage(MSG_ITEM_USED, &msg);
}

// MARK: - Remote Actor Processing

#define MAX_REMOTE_ACTORS 32 // matches the mod's MAX_SYNCED_ACTORS
static char sRemoteActorIds[MAX_REMOTE_ACTORS][UUID_STRING_LENGTH];
static u32 sRemoteActorCount = 0;

// Checks whether we need to create or destroy actors
void remote_actors_update(PlayState *play) {
    // Clear the buffer to avoid garbage values
    memset(sRemoteActorIds, 0, sizeof(sRemoteActorIds));

    // Call with the proper buffer size parameter (UUID_STRING_LENGTH)
    sRemoteActorCount = NS_GetRemoteActorIDs(MAX_REMOTE_ACTORS, (char *)sRemoteActorIds, UUID_STRING_LENGTH);

    // Create actors for new remote entities (only if we have any)
    for (u32 i = 0; i < sRemoteActorCount; i++) {
        // 1. Check if entity already has an actor
        bool remoteActorAlreadyCreated = false;
        Actor *actor = play->actorCtx.actorLists[ACTORCAT_PLAYER].first;

        // Find actor with given ID
        while (actor != NULL) {
            if (actor->id == ACTOR_REMOTE_PLAYER) {
                const char *actorNetworkId = NS_GetActorNetworkId(actor);
                const char *remoteId = sRemoteActorIds[i];

                if (actorNetworkId != NULL && remoteId != NULL && strcmp(actorNetworkId, remoteId) == 0) {
                    ActorAppearanceDataHandle h = AppearanceDataManager_getAppearanceDataAndRefreshLifetime(actorNetworkId);

                    if (h && !PlayerModelManager_Actor_hasAppearanceData(actor)) {
                        recomp_printf("Attempting to assign appearance data to %s...\n", actorNetworkId);

                        if (h) {
                            PlayerModelManager_ActorAppearanceData_assignDataToActor(actor, h);
                        }
                    }

                    remoteActorAlreadyCreated = true;
                    break;
                }
            }

            actor = actor->next;
        }

        // 2. If actor not found, create new actor
        if (!remoteActorAlreadyCreated) {
            const char *remoteId = sRemoteActorIds[i];
            recomp_printf("Creating actor for remote entity %s\n", remoteId);
            actor = Actor_SpawnAsChildAndCutscene(&play->actorCtx, play, ACTOR_REMOTE_PLAYER, -9999.0f, -9999.0f, -9999.0f, 0, 0, 0, 0, 0, 0, 0);
            NS_SyncActor(actor, remoteId, 0);
        }
    }

    // Check for entities that no longer exist and remove their actors
    Actor *actor = play->actorCtx.actorLists[ACTORCAT_PLAYER].first;
    while (actor != NULL) {
        Actor *next = actor->next; // Save next pointer as we may delete this actor

        if (actor->id == ACTOR_REMOTE_PLAYER) {
            const char *actorNetworkId = NS_GetActorNetworkId(actor);
            if (actorNetworkId == NULL) {
                Actor_Kill(actor);
                recomp_printf("Removed remote actor with NULL ID\n");
            } else {
                bool stillExists = false;

                for (u32 i = 0; i < sRemoteActorCount; i++) {
                    if (sRemoteActorIds[i][0] != '\0' && strcmp(actorNetworkId, sRemoteActorIds[i]) == 0) {
                        stillExists = true;
                        break;
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
