#ifndef MESSAGES_H
#define MESSAGES_H

#include "PR/ultratypes.h"

#define UUID_STRING_LENGTH 37

#define MSG_ITEM_USED "item_used"
typedef struct {
    u32 dummy_data;
} ItemUsedMessage;

#define MSG_MODEL_SET "model_changed"
#define MAX_MODEL_NAME_LEN 256
typedef struct ModelSetMessage {
    char id[UUID_STRING_LENGTH];
    char modelName[MAX_MODEL_NAME_LEN];
    u32 modelType;
} ModelSetMessage;

#define MSG_MODEL_REMOVED "model_removed"
typedef struct ModelRemovedMessage {
    char id[UUID_STRING_LENGTH];
    u32 modelType;
} ModelRemovedMessage;

#define MSG_REQUEST_MODEL_DATA "model_data_request"
typedef struct RequestModelDataMessage {
    char idBeingRequested[UUID_STRING_LENGTH];
} RequestModelDataMessage;

#define MSG_PUPPET_UPDATE "puppet_update"
typedef struct PuppetUpdateMessage {
    PlayerModelManagerModelType modelType;
    Color_RGBA8 color;
    s16 sceneId;
    char networkId[UUID_STRING_LENGTH];
} PuppetUpdateMessage;

#endif
