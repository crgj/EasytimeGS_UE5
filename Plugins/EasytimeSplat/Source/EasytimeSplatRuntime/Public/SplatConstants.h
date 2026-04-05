/*
  Copyright (c) 2025 Easytime Technology Co., Ltd. See LICENSE.md.
*/

#pragma once

#include "Math/Color.h"

namespace Easytime::Splat
{
#if WITH_EDITOR
static constexpr FColor EditorColor(50, 0, 110, 255);
#endif

static constexpr float MetersToCentimeters = 100.f;
static constexpr uint32 DepthMask = 0x0000FFFF;
} // namespace Easytime::Splat

