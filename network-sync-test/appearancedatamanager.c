#include "global.h"
#include "appearancedatamanager.h"
#include "yazmtcorelib_api.h"
#include "string.h"
#include "messages.h"
#include "playermodelmanager_api.h"
#include "recomputils.h"

RECOMP_IMPORT(YAZMT_PMM_MOD_NAME, ActorAppearanceDataHandle PlayerModelManager_ActorAppearanceData_createData(void));
RECOMP_IMPORT(YAZMT_PMM_MOD_NAME, bool PlayerModelManager_ActorAppearanceData_releaseHandle(ActorAppearanceDataHandle h));
RECOMP_IMPORT(YAZMT_PMM_MOD_NAME, bool PlayerModelManager_AppearanceData_setTunicColor(ActorAppearanceDataHandle h, PlayerModelManagerModelType type, Color_RGBA8 color));
RECOMP_IMPORT(YAZMT_PMM_MOD_NAME, bool PlayerModelManager_Actor_getTunicColor(Actor *actor, Color_RGBA8 *out));
RECOMP_IMPORT(YAZMT_PMM_MOD_NAME, PlayerModelManagerModelType PlayerModelManager_Actor_getFormModelType(Actor *actor));

// Play_Main refreshes at 20 times a second
#define APPEARANCE_DATA_LIFETIME_REFRESH (5 * 20)

RECOMP_IMPORT("mm_network_sync", u8 NS_RegisterMessageHandler(const char *messageId, u32 payloadSize, void *callback));
RECOMP_IMPORT("mm_network_sync", u8 NS_EmitMessage(const char *messageId, void *data));

extern char gLocalPlayerId[UUID_STRING_LENGTH];
extern bool gHasLocalPlayer;

void handleRequestModelDataMessage(void *data) {
    RequestModelDataMessage *msg = data;

    if (gHasLocalPlayer) {
        if (strcmp(msg->idBeingRequested, gLocalPlayerId) == 0) {
            for (int i = 0; i < PMM_MODEL_TYPE_MAX; i++) {
                AppearanceDataManager_queueModelChangedPacket(i);
            }
        }
    }
}

void handleTunicColorMessage(void *data) {
    SetTunicColorMessage *msg = data;

    if (msg->id[0] != '\0') {
        ActorAppearanceDataHandle h = AppearanceDataManager_getAppearanceDataAndRefreshLifetime(msg->id);

        if (h) {
            PlayerModelManager_AppearanceData_setTunicColor(h, msg->modelType, msg->color);
        }
    }
}

static YAZMTCore_StringU32Dictionary *sIdToAppearanceData;

typedef struct AppearanceData AppearanceData;

typedef struct AppearanceData {
    ActorAppearanceDataHandle handle;
    int lifetime;
    AppearanceData *next;
    AppearanceData *prev;
    char id[UUID_STRING_LENGTH];
} AppearanceData;

static AppearanceData *sAppearanceDataListStart;

static AppearanceData *createAppearanceData(const char *id) {
    AppearanceData *data = recomp_alloc(sizeof(AppearanceData));

    data->handle = PlayerModelManager_ActorAppearanceData_createData();
    data->next = sAppearanceDataListStart;
    data->prev = NULL;
    data->lifetime = APPEARANCE_DATA_LIFETIME_REFRESH;
    Lib_MemCpy(data->id, id, UUID_STRING_LENGTH);
    data->id[UUID_STRING_LENGTH - 1] = '\0';

    if (!data->handle) {
        recomp_free(data);
        data = NULL;
    } else {
        if (data->next) {
            data->next->prev = data;
        }

        sAppearanceDataListStart = data;

        YAZMTCore_StringU32Dictionary_set(sIdToAppearanceData, id, (uintptr_t)data);
    }

    return data;
}

static AppearanceData *getAppearanceData(const char *id) {
    uintptr_t retrievedData = 0;

    YAZMTCore_StringU32Dictionary_get(sIdToAppearanceData, id, &retrievedData);

    AppearanceData *appearanceData = (AppearanceData *)retrievedData;

    if (appearanceData) {
        AppearanceData *data = (AppearanceData *)retrievedData;

        data->lifetime = APPEARANCE_DATA_LIFETIME_REFRESH;
    }

    return appearanceData;
}

typedef enum GetOrCreateAppearanceDataResult {
    GOCAD_ERROR = 0,
    GOCAD_ALREADY_EXISTED = 1,
    GOCAD_CREATED = 2,
} GetOrCreateAppearanceDataResult;

static GetOrCreateAppearanceDataResult getOrCreateAppearanceData(const char *id, AppearanceData **out) {
    if (!id || !out) {
        return GOCAD_ERROR;
    }

    GetOrCreateAppearanceDataResult ret = GOCAD_ALREADY_EXISTED;

    if (!YAZMTCore_StringU32Dictionary_contains(sIdToAppearanceData, id)) {
        createAppearanceData(id);
        ret = GOCAD_CREATED;
    }

    AppearanceData *data = getAppearanceData(id);

    if (!data) {
        ret = GOCAD_ERROR;
    } else {
        *out = data;
    }

    return ret;
}

static void destroyAppearanceData(AppearanceData *data) {
    if (data) {
        recomp_printf("Destroying AppearanceData for model with uuid %s...\n", data->id);

        if (data == sAppearanceDataListStart) {
            sAppearanceDataListStart = data->next;
        } else {
            if (data->prev) {
                data->prev->next = data->next;
            }
        }

        if (data->next) {
            data->next->prev = data->prev;
        }

        PlayerModelManager_ActorAppearanceData_releaseHandle(data->handle);
        YAZMTCore_StringU32Dictionary_unset(sIdToAppearanceData, data->id);
        data->handle = 0;
        recomp_free(data);
    }
}

void AppearanceDataManager_init(void) {
    sIdToAppearanceData = YAZMTCore_StringU32Dictionary_new();

    NS_RegisterMessageHandler(MSG_REQUEST_MODEL_DATA, sizeof(RequestModelDataMessage), handleRequestModelDataMessage);
    NS_RegisterMessageHandler(MSG_SET_SUNIC_COLOR, sizeof(SetTunicColorMessage), handleTunicColorMessage);
}

ActorAppearanceDataHandle AppearanceDataManager_getAppearanceDataAndRefreshLifetime(const char *key) {
    AppearanceData *data = NULL;
    GetOrCreateAppearanceDataResult res = getOrCreateAppearanceData(key, &data);

    if (res == GOCAD_CREATED) {
        RequestModelDataMessage msg;

        strcpy(msg.idBeingRequested, data->id);

        NS_EmitMessage(MSG_REQUEST_MODEL_DATA, &msg);
    }

    if (data) {
        return data->handle;
    }

    return 0;
}

typedef struct LocalAppearanceTracker {
    bool isNeedsRefresh;
    char modelName[MAX_MODEL_NAME_LEN];
} LocalAppearanceTracker;

static LocalAppearanceTracker sLocalAppearanceTracker[PMM_MODEL_TYPE_MAX];

void AppearanceDataManager_setLocalInternalName(PlayerModelManagerModelType modelType, const char *internalName) {
    if (modelType >= 0 && modelType < PMM_MODEL_TYPE_MAX && modelType != PMM_MODEL_TYPE_MODEL_PACK) {
        if (!internalName) {
            internalName = "";
        }

        sLocalAppearanceTracker[modelType].isNeedsRefresh = strcmp(internalName, sLocalAppearanceTracker[modelType].modelName) != 0;

        strcpy(sLocalAppearanceTracker[modelType].modelName, internalName);
    }
}

void AppearanceDataManager_queueModelChangedPacket(PlayerModelManagerModelType modelType) {
    if (modelType >= 0 && modelType < PMM_MODEL_TYPE_MAX && modelType != PMM_MODEL_TYPE_MODEL_PACK) {
        sLocalAppearanceTracker[modelType].isNeedsRefresh = true;
    }
}

RECOMP_HOOK("Play_Main") void updateAppearanceDataManager_onPlay_Main(PlayState *play) {
    AppearanceData *curr = sAppearanceDataListStart;

    while (curr) {
        AppearanceData *next = curr->next;

        curr->lifetime--;

        if (curr->lifetime <= 0) {
            destroyAppearanceData(curr);
        }

        curr = next;
    }

    if (gHasLocalPlayer) {
        const int MAX_REFRESHES = 8;
        int numRefreshMessages = 0;

        for (int i = 0; i < PMM_MODEL_TYPE_MAX && numRefreshMessages < MAX_REFRESHES; ++i) {
            LocalAppearanceTracker *curr = &sLocalAppearanceTracker[i];

            if (curr->isNeedsRefresh) {
                numRefreshMessages++;

                if (curr->modelName[0] == '\0') {
                    ModelRemovedMessage msg;

                    strcpy(msg.id, gLocalPlayerId);
                    msg.modelType = i;

                    NS_EmitMessage(MSG_MODEL_REMOVED, &msg);
                } else {
                    ModelSetMessage msg;

                    strcpy(msg.id, gLocalPlayerId);
                    msg.modelType = i;
                    strcpy(msg.modelName, curr->modelName);

                    NS_EmitMessage(MSG_MODEL_SET, &msg);
                }
            }
        }

        Color_RGBA8 tunicColor;
        if (PlayerModelManager_Actor_getTunicColor(&GET_PLAYER(play)->actor, &tunicColor)) {
            SetTunicColorMessage msg;

            msg.color = tunicColor;
            strcpy(msg.id, gLocalPlayerId);
            msg.modelType = PlayerModelManager_Actor_getFormModelType(&GET_PLAYER(play)->actor);

            if (msg.modelType != PMM_MODEL_TYPE_NONE) {
                NS_EmitMessage(MSG_SET_SUNIC_COLOR, &msg);
            }
        }
    }
}
