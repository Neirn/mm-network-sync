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

// From z_player.c
void func_80846460(Player *this);

// Mostly taken from Player_Draw
static void RemotePlayer_DrawGoronCurled(RemotePlayer *this, PlayState *play) {
    OPEN_DISPS(play->state.gfxCtx);

    Player *player = &this->player;

    Color_RGB8 spBC;
    f32 spB8 = player->unk_ABC + 1.0f;
    f32 spB4 = 1.0f - (player->unk_ABC * 0.5f);

    func_80846460(player);
    Matrix_Translate(player->actor.world.pos.x, player->actor.world.pos.y + (1200.0f * player->actor.scale.y * spB8),
                     player->actor.world.pos.z, MTXMODE_NEW);

    if (player->unk_B86[0] != 0) {
        Matrix_RotateYS(player->unk_B28, MTXMODE_APPLY);
        Matrix_RotateXS(player->unk_B86[0], MTXMODE_APPLY);
        Matrix_RotateYS(-player->unk_B28, MTXMODE_APPLY);
    }

    Matrix_RotateYS(player->actor.shape.rot.y, MTXMODE_APPLY);
    Matrix_RotateZS(player->actor.shape.rot.z, MTXMODE_APPLY);

    Matrix_Scale(player->actor.scale.x * spB4 * 1.15f, player->actor.scale.y * spB8 * 1.15f,
                 CLAMP_MIN(spB8, spB4) * player->actor.scale.z * 1.15f, MTXMODE_APPLY);
    Matrix_RotateXS(player->actor.shape.rot.x, MTXMODE_APPLY);
    Scene_SetRenderModeXlu(play, 0, 1);

    extern Color_RGB8 D_8085D580;
    extern Color_RGB8 D_8085D584;
    Color_RGB8_Lerp(&D_8085D580, &D_8085D584, player->unk_B10[0], &spBC);

    Gfx *goronCurled = PlayerModelManager_Actor_getDisplayList(&player->actor, PMM_DL_CURLED);

    if (goronCurled) {
        gDPSetEnvColor(POLY_OPA_DISP++, spBC.r, spBC.g, spBC.b, 255);
        MATRIX_FINALIZE_AND_LOAD(POLY_OPA_DISP++, play->state.gfxCtx);
        gSPDisplayList(POLY_OPA_DISP++, goronCurled);
    }

    CLOSE_DISPS(play->state.gfxCtx);
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

    if (player->currentMask == PLAYER_MASK_STONE) {
        return;
    }

    if (player->stateFlags3 & PLAYER_STATE3_1000 && player->transformation == PLAYER_FORM_GORON) {
        RemotePlayer_DrawGoronCurled(this, play);
    } else {
        Player_DrawGameplay(play, player, 1, gCullBackDList, RemotePlayer_OverrideLimbDrawGameplayDefault);
    }
}
