#include "z_remote_player.h"
#include "globalobjects_api.h"
#include "playermodelmanager_api.h"

RECOMP_IMPORT(YAZMT_PMM_MOD_NAME, bool PlayerModelManager_Actor_setFormModelType(Actor *actor, PlayerModelManagerModelType type));
RECOMP_IMPORT(YAZMT_PMM_MOD_NAME, bool PlayerModelManager_AppearanceData_getTunicColor(ActorAppearanceDataHandle h, PlayerModelManagerModelType type, Color_RGBA8 *out));
RECOMP_IMPORT(YAZMT_PMM_MOD_NAME, const char *PlayerModelManager_AppearanceData_getModelName(ActorAppearanceDataHandle h, PlayerModelManagerModelType type));
RECOMP_IMPORT(YAZMT_PMM_MOD_NAME, bool PlayerModelManager_Actor_getTunicColor(Actor *actor, Color_RGBA8 *out));
RECOMP_IMPORT(YAZMT_PMM_MOD_NAME, void PlayerModelManager_updatePlayerAssets(Player *player));

#define FLAGS                                                                                  \
    (ACTOR_FLAG_ATTENTION_ENABLED | ACTOR_FLAG_FRIENDLY | ACTOR_FLAG_UPDATE_CULLING_DISABLED | \
     ACTOR_FLAG_DRAW_CULLING_DISABLED | ACTOR_FLAG_UPDATE_DURING_SOARING_AND_SOT_CS |          \
     ACTOR_FLAG_UPDATE_DURING_OCARINA | ACTOR_FLAG_CAN_PRESS_SWITCHES | ACTOR_FLAG_MINIMAP_ICON_ENABLED)

void RemotePlayer_Init(Actor *thisx, PlayState *play);
void RemotePlayer_Destroy(Actor *thisx, PlayState *play);
void RemotePlayer_Update(Actor *thisx, PlayState *play);
void RemotePlayer_Draw(Actor *thisx, PlayState *play);

ActorProfile RemotePlayer_InitVars = {
    /**/ ACTOR_ID_MAX,
    /**/ ACTORCAT_PLAYER,
    /**/ FLAGS,
    /**/ GAMEPLAY_KEEP,
    /**/ sizeof(RemotePlayer),
    /**/ RemotePlayer_Init,
    /**/ RemotePlayer_Destroy,
    /**/ RemotePlayer_Update,
    /**/ RemotePlayer_Draw,
};

static void *sPlayerObjects[PLAYER_FORM_MAX];

GLOBAL_OBJECTS_CALLBACK_ON_READY void onGlobalObjectsReady(void) {
    sPlayerObjects[PLAYER_FORM_HUMAN] = GlobalObjects_getGlobalObject(OBJECT_LINK_CHILD);
    sPlayerObjects[PLAYER_FORM_DEKU] = GlobalObjects_getGlobalObject(OBJECT_LINK_NUTS);
    sPlayerObjects[PLAYER_FORM_GORON] = GlobalObjects_getGlobalObject(OBJECT_LINK_GORON);
    sPlayerObjects[PLAYER_FORM_ZORA] = GlobalObjects_getGlobalObject(OBJECT_LINK_ZORA);
    sPlayerObjects[PLAYER_FORM_FIERCE_DEITY] = GlobalObjects_getGlobalObject(OBJECT_LINK_BOY);
}

static void forceObjectDependency(void *obj) {
    gSegments[0x06] = OS_K0_TO_PHYSICAL(obj);
}

static void setGfxObjDependency(PlayState *play, void *obj) {
    OPEN_DISPS(play->state.gfxCtx);
    gSPSegment(POLY_OPA_DISP++, 0x06, obj);
    gSPSegment(POLY_XLU_DISP++, 0x06, obj);
    CLOSE_DISPS(play->state.gfxCtx);
}

void RemotePlayer_Init(Actor *thisx, PlayState *play) {
    RemotePlayer *this = (RemotePlayer *)thisx;
    Player *player = &this->player;

    // Primarily modeled after EnTest3_Init and Player_Init
    player->csId = CS_ID_NONE;
    player->transformation = PLAYER_FORM_HUMAN;
    player->ageProperties = &sPlayerAgeProperties[player->transformation];
    player->heldItemAction = PLAYER_IA_NONE;
    player->heldItemId = ITEM_OCARINA_OF_TIME;

    forceObjectDependency(sPlayerObjects[player->transformation]);

    Player_SetModelGroup(player, PLAYER_MODELGROUP_DEFAULT);
    play->playerInit(player, play, gPlayerSkeletons[player->transformation]);

    player->maskObjectSegment = recomp_alloc(0x3800);
    // play->func_18780(player, play);
    Player_Anim_PlayOnceMorph(play, player, Player_GetIdleAnim(player));
    player->yaw = player->actor.shape.rot.y;

    // Will ensure the actor is always updating even when in a separate room than the player
    player->actor.room = -1;

    extern AnimationHeader gLinkGoronShieldingAnim;
    extern FlexSkeletonHeader gLinkGoronShieldingSkel;

    void *goronObj = sPlayerObjects[PLAYER_FORM_GORON];
    AnimationHeader *goronShieldingAnimGlobal = SEGMENTED_TO_GLOBAL_PTR(goronObj, &gLinkGoronShieldingAnim);
    FlexSkeletonHeader *goronShieldingSkelGlobal = SEGMENTED_TO_GLOBAL_PTR(goronObj, &gLinkGoronShieldingSkel);

    SkelAnime_InitFlex(play, &player->unk_2C8, goronShieldingSkelGlobal, goronShieldingAnimGlobal, player->jointTable, player->morphTable, goronShieldingSkelGlobal->sh.limbCount);
}

void RemotePlayer_Destroy(Actor *thisx, PlayState *play) {
    RemotePlayer *this = (RemotePlayer *)thisx;
    Player *player = &this->player;

    forceObjectDependency(sPlayerObjects[player->transformation]);

    recomp_free(player->maskObjectSegment);
}

void RemotePlayer_Update(Actor *thisx, PlayState *play) {
    RemotePlayer *this = (RemotePlayer *)thisx;
    Player *player = &this->player;

    forceObjectDependency(sPlayerObjects[player->transformation]);

    // Let PMM handle age properties for human (handles adult age props)
    if (player->transformation != PLAYER_FORM_HUMAN) {
        player->ageProperties = &sPlayerAgeProperties[player->transformation];
    }

    player->actor.shape.shadowAlpha = 255;

    func_801229FC(player);

    static PlayerModelManagerModelType transformationToModelType[] = {
        [PLAYER_FORM_HUMAN] = PMM_MODEL_TYPE_CHILD,
        [PLAYER_FORM_DEKU] = PMM_MODEL_TYPE_DEKU,
        [PLAYER_FORM_GORON] = PMM_MODEL_TYPE_GORON,
        [PLAYER_FORM_ZORA] = PMM_MODEL_TYPE_ZORA,
        [PLAYER_FORM_FIERCE_DEITY] = PMM_MODEL_TYPE_FIERCE_DEITY,
    };

    PlayerModelManager_Actor_setFormModelType(&player->actor, transformationToModelType[player->transformation]);
}

s32 RemotePlayer_OverrideLimbDrawGameplayDefault(PlayState *play, s32 limbIndex, Gfx **dList, Vec3f *pos, Vec3s *rot, Actor *actor) {
    return Player_OverrideLimbDrawGameplayDefault(play, limbIndex, dList, pos, rot, actor);
}

void RemotePlayer_Draw(Actor *thisx, PlayState *play) {
    RemotePlayer *this = (RemotePlayer *)thisx;
    Player *player = &this->player;

    forceObjectDependency(sPlayerObjects[player->transformation]);
    setGfxObjDependency(play, sPlayerObjects[player->transformation]);

    Color_RGBA8 tunicColor;

    if (PlayerModelManager_Actor_getTunicColor(thisx, &tunicColor)) {
        OPEN_DISPS(play->state.gfxCtx);
        gDPSetEnvColor(POLY_OPA_DISP++, tunicColor.r, tunicColor.g, tunicColor.b, tunicColor.a);
        CLOSE_DISPS(play->state.gfxCtx);
    }

    PlayerModelManager_updatePlayerAssets(player);

    Player_SetModelGroup(player, player->modelGroup);

    Player_DrawGameplay(play, player, 1, gCullBackDList, RemotePlayer_OverrideLimbDrawGameplayDefault);
}
