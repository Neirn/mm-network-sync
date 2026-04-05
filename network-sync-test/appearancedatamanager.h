#ifndef APPEARANCEDATAMANAGER_H
#define APPEARANCEDATAMANAGER_H

#include "playermodelmanager_api.h"

void AppearanceDataManager_init(void);
ActorAppearanceDataHandle AppearanceDataManager_getAppearanceDataAndRefreshLifetime(const char *key);
void AppearanceDataManager_setLocalInternalName(PlayerModelManagerModelType modelType, const char *internalName);
void AppearanceDataManager_queueModelChangedPacket(PlayerModelManagerModelType modelType);
void AppearanceDataManager_getTunicColor(const char *key);

#endif
