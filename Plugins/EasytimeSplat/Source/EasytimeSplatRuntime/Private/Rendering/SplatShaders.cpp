/*
  Copyright (c) 2025 Easytime Technology Co., Ltd. See LICENSE.md.
*/

#include "SplatShaders.h"

#define IMPLEMENT_TEMPLATED_GLOBAL_SHADER(                                     \
	ShaderClass, SourceFilename, FunctionName, Frequency)                      \
	IMPLEMENT_SHADER_TYPE(                                                     \
		template <>,                                                           \
		ShaderClass,                                                           \
		TEXT(SourceFilename),                                                  \
		TEXT(FunctionName),                                                    \
		Frequency)

namespace Easytime::Splat::Shaders
{

IMPLEMENT_GLOBAL_SHADER(
	FComputeDistanceCS,
	"/Plugin/EasytimeSplat/Private/ComputeDistanceCS.usf",
	"main",
	SF_Compute);
IMPLEMENT_GLOBAL_SHADER(
	FComputeTransformCS,
	"/Plugin/EasytimeSplat/Private/ComputeTransformCS.usf",
	"main",
	SF_Compute);
IMPLEMENT_TEMPLATED_GLOBAL_SHADER(
	FRenderSplatVS<ESortingDevice::CPU>,
	"/Plugin/EasytimeSplat/Private/RenderSplatVS.usf",
	"main",
	SF_Vertex);
IMPLEMENT_TEMPLATED_GLOBAL_SHADER(
	FRenderSplatVS<ESortingDevice::GPU>,
	"/Plugin/EasytimeSplat/Private/RenderSplatVS.usf",
	"main",
	SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(
	FRenderSplatPS,
	"/Plugin/EasytimeSplat/Private/RenderSplatPS.usf",
	"main",
	SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(
	FInterpolate4DCS,
	"/Plugin/EasytimeSplat/Private/Interpolate4DCS.usf",
	"main",
	SF_Compute);
IMPLEMENT_GLOBAL_SHADER(
	FCopyLocalSortToGlobalCS,
	"/Plugin/EasytimeSplat/Private/CopyLocalSortToGlobalCS.usf",
	"main",
	SF_Compute);
IMPLEMENT_GLOBAL_SHADER(
	FGatherProxyRenderDataToGlobalCS,
	"/Plugin/EasytimeSplat/Private/GatherProxyRenderDataToGlobalCS.usf",
	"main",
	SF_Compute);
IMPLEMENT_GLOBAL_SHADER(
	FRenderGlobalSplatVS,
	"/Plugin/EasytimeSplat/Private/RenderGlobalSplatVS.usf",
	"main",
	SF_Vertex);

} // namespace Easytime::Splat::Shaders

