#include "actor_sync.h"
#include "recomputils.h"
#include "z64recomp_api.h"
#include "network_core.h"

#include <string.h>

// MARK: - Actor Extension

// Extension ID for network player data
#define ACTOR_EXTENSION_INVALID UINT32_MAX
static ActorExtensionId sNetworkSyncerExtension = ACTOR_EXTENSION_INVALID;

#define MAX_ACTOR_CATEGORIES ACTORCAT_MAX
#define MAX_SYNCED_ACTORS 32
static u8 sSyncedActorCategories[MAX_ACTOR_CATEGORIES] = {0}; // Bitset for categories with synced actors

// Structure to hold network-specific data for each actor
typedef struct {
    // UUID string for this actor
    char actor_id[UUID_STRING_LENGTH];
    // Flag indicating if actor is being synced
    u8 is_synced;
    // Flag indicating whether we are in charge of pushing its data to the server
    u8 is_owned_locally;
} NetworkExtendedActorData;

static NetworkExtendedActorData *GetActorNetworkData(Actor *actor) {
    if (sNetworkSyncerExtension == ACTOR_EXTENSION_INVALID) {
        return NULL;
    }

    return (NetworkExtendedActorData *)z64recomp_get_extended_actor_data(actor, sNetworkSyncerExtension);
}

// MARK: - Actor Sync Data

typedef struct {
    Vec3f worldPosition;
    Vec3s shapeRotation;

    // Player Actor specific properties
    Vec3s upperLimbRot;
    Vec3s jointTable[24];
    s8 currentMask;
    s8 currentShield;
} ActorSyncData;

// MARK: - Actor Sync Implementation

void ActorSyncInit() {
    // Create actor extension for network player data
    if (sNetworkSyncerExtension == ACTOR_EXTENSION_INVALID) {
        sNetworkSyncerExtension = z64recomp_extend_actor_all(sizeof(NetworkExtendedActorData));
        if (sNetworkSyncerExtension == ACTOR_EXTENSION_INVALID) {
            recomp_printf("Failed to create network player extension\n");
        }
    }

    // Reset synced categories
    for (int i = 0; i < MAX_ACTOR_CATEGORIES; i++) {
        sSyncedActorCategories[i] = 0;
    }
}

const char *ActorSyncGetNetworkId(Actor *actor) {
    if (actor == NULL) {
        recomp_printf("Cannot get ID for NULL actor\n");
        return NULL;
    }

    NetworkExtendedActorData *netData = GetActorNetworkData(actor);
    if (netData == NULL) {
        recomp_printf("Actor %u is not registered for network play\n", actor->id);
        return NULL;
    }

    if (netData->actor_id[0] == '\0') {
        return NULL;
    }

    return netData->actor_id;
}

void ActorSyncRegister(Actor *actor, const char *playerId, int isOwnedLocally) {
    if (actor == NULL) {
        recomp_printf("Cannot sync NULL actor\n");
        return;
    }

    if (sNetworkSyncerExtension == ACTOR_EXTENSION_INVALID) {
        sNetworkSyncerExtension = z64recomp_extend_actor_all(sizeof(NetworkExtendedActorData));
    }

    NetworkExtendedActorData *netData = GetActorNetworkData(actor);
    if (netData == NULL) {
        recomp_printf("Failed to get network data for actor %u\n", actor->id);
        return;
    }

    netData->is_synced = 1;
    netData->is_owned_locally = isOwnedLocally;

    if (actor->category < MAX_ACTOR_CATEGORIES) {
        sSyncedActorCategories[actor->category] = 1;
    }

    if (playerId == NULL) {
        recomp_printf("WARNING: Registering actor with NULL playerId - generating new UUID\n");

        char *playerIdBuffer = recomp_alloc(37);
        u8 success = NetworkSyncGenerateUUID(playerIdBuffer);

        if (success) {
            strcpy(netData->actor_id, playerIdBuffer);
            recomp_printf("Generated actor ID: %s\n", netData->actor_id);
        } else {
            recomp_printf("Failed to generate actor ID\n");
        }

        recomp_free(playerIdBuffer);
    } else if (playerId != NULL) {
        recomp_printf("Registering actor with provided ID: %s\n", playerId);
        strcpy(netData->actor_id, playerId);
    }

    if (isOwnedLocally) {
        NetworkSyncRegisterActor(netData->actor_id);
    }
}

void ActorSyncUpdate(PlayState *play, Actor *actor) {
    NetworkExtendedActorData *netData = GetActorNetworkData(actor);

    if (netData == NULL || !netData->is_synced || !netData->is_owned_locally) {
        return;
    }

    ActorSyncData *syncData = recomp_alloc(sizeof(ActorSyncData) + sizeof(Vec3s) * 23);
    Math_Vec3s_Copy(&syncData->shapeRotation, &actor->shape.rot);
    Math_Vec3f_Copy(&syncData->worldPosition, &actor->world.pos);

    if (actor->category == ACTORCAT_PLAYER) {
        Player *player = (Player *)actor;
        syncData->currentMask = player->currentMask;
        syncData->currentShield = player->currentShield;

        for (int i = 0; i < 24; i++) {
            Math_Vec3s_Copy(&syncData->jointTable[i], &player->skelAnime.jointTable[i]);
        }

        Math_Vec3s_Copy(&syncData->upperLimbRot, &player->upperLimbRot);
    }

    NetworkSyncSendActorUpdate(netData->actor_id, syncData);
    recomp_free(syncData);
}

void ActorSyncProcessRemoteData(PlayState *play) {
    ActorSyncData remote_data;

    // Allocate buffer with proper size and alignment
    // Each ID needs UUID_STRING_LENGTH bytes (which is 37 with null terminator)
    char *ids_buffer = recomp_alloc(sizeof(char) * MAX_SYNCED_ACTORS * UUID_STRING_LENGTH);

    // Clear the buffer to avoid garbage values
    memset(ids_buffer, 0, sizeof(char) * MAX_SYNCED_ACTORS * UUID_STRING_LENGTH);

    // Get remote actor IDs, using proper buffer size (UUID_STRING_LENGTH, not 64)
    u32 remote_actor_count = NetworkSyncGetRemoteActorIDs(MAX_SYNCED_ACTORS, ids_buffer, UUID_STRING_LENGTH);

    // Check all actor categories that have synced actors
    for (u32 i = 0; i < MAX_ACTOR_CATEGORIES; i++) {
        if (sSyncedActorCategories[i] == 0) {
            continue;
        }

        Actor *actor = play->actorCtx.actorLists[i].first;

        while (actor != NULL) {
            NetworkExtendedActorData *net_data = GetActorNetworkData(actor);
            Actor *next_actor = actor->next; // Save next pointer as we may delete this actor

            if (net_data != NULL && net_data->is_synced) {
                // Skip actors we own locally
                if (net_data->is_owned_locally) {
                    actor = next_actor;
                    continue;
                }

                // Try to find this actor in our remote actors list
                for (u32 j = 0; j < remote_actor_count; j++) {
                    const char *remote_actor_id = &ids_buffer[j * UUID_STRING_LENGTH];

                    // Make sure we're looking at valid strings
                    if (remote_actor_id[0] == '\0' || net_data->actor_id[0] == '\0') {
                        continue;
                    }

                    if (strcmp(net_data->actor_id, remote_actor_id) == 0) {
                        if (NetworkSyncGetRemoteActorData(remote_actor_id, &remote_data)) {
                            // Update actor's position and rotation
                            Math_Vec3s_Copy(&actor->shape.rot, &remote_data.shapeRotation);
                            Math_Vec3f_Copy(&actor->world.pos, &remote_data.worldPosition);

                            // Update player-specific properties if applicable
                            if (actor->category == ACTORCAT_PLAYER) {
                                Player *player = (Player *)actor;
                                player->currentMask = remote_data.currentMask;
                                player->currentShield = remote_data.currentShield;

                                for (int k = 0; k < 24; k++) {
                                    Math_Vec3s_Copy(&player->skelAnime.jointTable[k], &remote_data.jointTable[k]);
                                }

                                Math_Vec3s_Copy(&player->upperLimbRot, &remote_data.upperLimbRot);
                            }
                            break;
                        }
                    }
                }
            }

            actor = next_actor;
        }
    }

    // Free allocated buffer
    recomp_free(ids_buffer);
}
