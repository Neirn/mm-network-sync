#include "modding.h"
#include "global.h"
#include "recomputils.h"
#include "string.h"
#include "yazmtcorelib_api.h"
#include "playermodelmanager_api.h"

#define UUID_STRING_LENGTH 37

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

RECOMP_IMPORT(YAZMT_PMM_MOD_NAME, ActorAppearanceDataHandle PlayerModelManager_ActorAppearanceData_createData(void));
RECOMP_IMPORT(YAZMT_PMM_MOD_NAME, bool PlayerModelManager_ActorAppearanceData_releaseHandle(ActorAppearanceDataHandle h));
RECOMP_IMPORT(YAZMT_PMM_MOD_NAME, bool PlayerModelManager_ActorAppearanceData_assignModel(ActorAppearanceDataHandle h, PlayerModelManagerModelType type, const char *internalName));
RECOMP_IMPORT(YAZMT_PMM_MOD_NAME, bool PlayerModelManager_ActorAppearanceData_assignDataToActor(Actor *actor, ActorAppearanceDataHandle h));

// MARK: - Forward Declarations

void remote_actors_update(PlayState *play);

// MARK: - Emit / Receive Remote Events

#define MSG_ITEM_USED "item_used"

typedef struct {
    u32 dummy_data;
} ItemUsedMessage;

void handle_item_used_message(void *data) {
    ItemUsedMessage *msg = (ItemUsedMessage *)data;
    Notifications_Emit(
        "",                       // Prefix (Purple)
        "Remote actor used item", // Main Message (white)
        ""                        // Suffix (Blue)
    );
}

typedef struct AppearanceData {
    ActorAppearanceDataHandle handle;
    int refCount;
} AppearanceData;

static AppearanceData *getAppearanceData(const char *id);

#define MAX_MODEL_NAME_LEN 255
typedef struct AppearanceChangedMessage {
    char id[UUID_STRING_LENGTH];
    char modelName[MAX_MODEL_NAME_LEN];
    PlayerModelManagerModelType modelType;
} AppearanceChangedMessage;

#define MSG_APPEARANCE_CHANGED "model_changed"

void handleAppearanceChangedMessage(void *data) {
    AppearanceChangedMessage *msg = data;

    msg->id[UUID_STRING_LENGTH - 1] = '\0';
    msg->modelName[MAX_MODEL_NAME_LEN - 1] = '\0';

    if (msg->id[0] != '\0') {
        AppearanceData *appearanceData = getAppearanceData(msg->id);

        if (appearanceData) {
            PlayerModelManager_ActorAppearanceData_assignModel(appearanceData->handle, msg->modelType, msg->modelName);
        }
    }
}

// MARK: - Custom Actors

extern ActorProfile RemotePlayer_InitVars;
s16 ACTOR_REMOTE_PLAYER = ACTOR_ID_MAX;

// MARK: - Events

u8 gHasConnected;
u8 gHasLoadedSave;
s32 gCurrentSceneId = -1;
// Track local player actor ID
char gLocalPlayerId[UUID_STRING_LENGTH];
static u8 gHasLocalPlayer;

static YAZMTCore_StringU32Dictionary *sIdToAppearanceData;

RECOMP_CALLBACK("*", recomp_on_init) void init_runtime() {
    NS_Init();
    ACTOR_REMOTE_PLAYER = CustomActor_Register(&RemotePlayer_InitVars);

    // Register message handlers
    NS_RegisterMessageHandler(MSG_ITEM_USED, sizeof(ItemUsedMessage), handle_item_used_message);

    NS_RegisterMessageHandler(MSG_APPEARANCE_CHANGED, sizeof(AppearanceChangedMessage), handleAppearanceChangedMessage);

    sIdToAppearanceData = YAZMTCore_StringU32Dictionary_new();
}

RECOMP_CALLBACK("*", recomp_on_play_init) void on_play_init(PlayState *play) {
    if (gHasConnected)
        return;
    recomp_printf("Connecting to server...\n");
    gHasConnected = NS_Connect(SERVER_URL);

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
    AppearanceChangedMessage msg;

    strcpy(msg.id, gLocalPlayerId);
    strcpy(msg.modelName, internalNameOrNull ? internalNameOrNull : "");
    msg.modelType = modelType;

    NS_EmitMessage(MSG_APPEARANCE_CHANGED, &msg);
}

// MARK: - Hooks

RECOMP_HOOK("FileSelect_LoadGame") void OnFileSelect_LoadGame(PlayState *play) {
    recomp_printf("FileSelect_LoadGame called\n");
    gHasLoadedSave = 1;
}

RECOMP_HOOK("Player_Init") void OnPlayerInit(Actor *thisx, PlayState *play) {
    if (!gHasLoadedSave)
        return;

    recomp_printf("Player initialized in scene %d\n", play->sceneId);

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
            gHasLocalPlayer = 1;
            recomp_printf("Saved local player ID: %s\n", gLocalPlayerId);
        }
    }
}

RECOMP_HOOK("Player_UseItem") void OnPlayer_UseItem(PlayState *play, Player *this, ItemId item) {
    if (!gHasLoadedSave)
        return;

    ItemUsedMessage msg;
    msg.dummy_data = 7;
    //recomp_printf("Player_UseItem called\n");
    //NS_EmitMessage(MSG_ITEM_USED, &msg);
}

static void destroyAppearanceData(const char *id);
static AppearanceData *getAppearanceData(const char *id);

RECOMP_HOOK("Actor_Destroy") void onActor_Destroy(Actor *actor, PlayState *play) {
    if (actor->init == NULL) {
        if (actor->destroy != NULL) {
            const char *id = NS_GetActorNetworkId(actor);

            if (id) {
                AppearanceData *data = getAppearanceData(id);

                if (data) {
                    data->refCount--;

                    if (data->refCount == 0) {
                        destroyAppearanceData(id);
                    }
                }
            }
        }
    }
}

// MARK: - Remote Actor Processing

#define MAX_REMOTE_ACTORS 32 // matches the mod's MAX_SYNCED_ACTORS
static char sRemoteActorIds[MAX_REMOTE_ACTORS][UUID_STRING_LENGTH];
static u32 sRemoteActorCount = 0;

static AppearanceData *createAppearanceData(void) {
    AppearanceData *data = recomp_alloc(sizeof(AppearanceData));

    data->refCount = 0;
    data->handle = PlayerModelManager_ActorAppearanceData_createData();

    if (!data->handle) {
        recomp_free(data);
        data = NULL;
    }

    return data;
}

static AppearanceData *getAppearanceData(const char *id) {
    uintptr_t retrievedData = 0;

    YAZMTCore_StringU32Dictionary_get(sIdToAppearanceData, id, &retrievedData);

    return (AppearanceData *)retrievedData;
}

static AppearanceData *getOrCreateAppearanceData(const char *id) {
    if (!YAZMTCore_StringU32Dictionary_contains(sIdToAppearanceData, id)) {
        AppearanceData *data = createAppearanceData();

        if (data) {
            YAZMTCore_StringU32Dictionary_set(sIdToAppearanceData, id, (uintptr_t)data);
        }
    }

    return getAppearanceData(id);
}

static void destroyAppearanceData(const char *id) {
    AppearanceData *data = getAppearanceData(id);

    if (data) {
        PlayerModelManager_ActorAppearanceData_releaseHandle(data->handle);
        YAZMTCore_StringU32Dictionary_unset(sIdToAppearanceData, id);
        data->handle = 0;
        data->refCount = 0;
        recomp_free(data);
    }
}

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
                    if (!PlayerModelManager_Actor_hasAppearanceData(actor)) {
                        recomp_printf("Attempting to assign appearance data...\n");

                        AppearanceData *data = getOrCreateAppearanceData(actorNetworkId);

                        if (data) {
                            data->refCount++;
                            PlayerModelManager_ActorAppearanceData_assignDataToActor(actor, data->handle);
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
