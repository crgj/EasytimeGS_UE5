/*
  Copyright (c) 2025 Easytime Technology Co., Ltd. See LICENSE.md.
*/

#pragma once

#include "Logging/LogMacros.h"

EASYTIMESPLATRUNTIME_API DECLARE_LOG_CATEGORY_EXTERN(LogEasytimeSplat, Log, All);

/**
 * EASYTIME_LOG prefix to avoid collisions, as we are in the global namespace.
 * @see https://dev.epicgames.com/documentation/en-us/unreal-engine/logging-in-unreal-engine
 */

#define EASYTIME_LOGF(Format, ...)                                                 \
	UE_LOG(LogEasytimeSplat, Fatal, TEXT(Format), ##__VA_ARGS__)
#define EASYTIME_LOGE(Format, ...)                                                 \
	UE_LOG(LogEasytimeSplat, Error, TEXT(Format), ##__VA_ARGS__)
#define EASYTIME_LOGW(Format, ...)                                                 \
	UE_LOG(LogEasytimeSplat, Warning, TEXT(Format), ##__VA_ARGS__)
#define EASYTIME_LOGD(Format, ...)                                                 \
	UE_LOG(LogEasytimeSplat, Display, TEXT(Format), ##__VA_ARGS__)
#define EASYTIME_LOGL(Format, ...)                                                 \
	UE_LOG(LogEasytimeSplat, Log, TEXT(Format), ##__VA_ARGS__)
#define EASYTIME_LOGV(Format, ...)                                                 \
	UE_LOG(LogEasytimeSplat, Verbose, TEXT(Format), ##__VA_ARGS__)
#define EASYTIME_LOGVV(Format, ...)                                                \
	UE_LOG(LogEasytimeSplat, VeryVerbose, TEXT(Format), ##__VA_ARGS__)

