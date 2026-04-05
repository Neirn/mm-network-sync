#ifndef APPEARANCEDATAMANAGER_H
#define APPEARANCEDATAMANAGER_H

#include "playermodelmanager_api.h"
#include "z64scene.h"

void AppearanceDataManager_init(void);
ActorAppearanceDataHandle AppearanceDataManager_getAppearanceDataAndRefreshLifetime(const char *key);
void AppearanceDataManager_queueModelChangedPacket(PlayerModelManagerModelType modelType);
void AppearanceDataManager_getTunicColor(const char *key);
bool AppearanceDataManager_getSceneId(const char *key, s16 *out);

#endif
