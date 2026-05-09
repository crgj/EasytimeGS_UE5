/*
  Copyright (c) 2025 Easytime Technology Co., Ltd. See LICENSE.md.
*/

#pragma once

#include <type_traits>

#include "CoreMinimal.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalShader.h"
#include "HLSLTypeAliases.h"
#include "SceneView.h"
#include "ShaderParameterStruct.h"
#include "SplatConstants.h"

namespace Easytime::Splat
{
namespace Shaders
{

/**
 * For compute shader pre-passes only.
 * TODO(seth): This needs to be tuned for performance.
 */
constexpr uint32 THREAD_GROUP_SIZE_X = 32;

BEGIN_SHADER_PARAMETER_STRUCT(FPackedPositionParameters, )
SHADER_PARAMETER(FVector3f, pos_min_cm)
SHADER_PARAMETER(FVector3f, pos_scale_cm)
SHADER_PARAMETER_SRV(Buffer<uint>, positions)
END_SHADER_PARAMETER_STRUCT()

// WDD-20260405 00:44 RDG版只保留SRV不重复标量，避免packoffset重定义
/**
 * Calculates distances to each splat, for GPU sorting.
 */
class FComputeDistanceCS final : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FComputeDistanceCS);
	SHADER_USE_PARAMETER_STRUCT(FComputeDistanceCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	SHADER_PARAMETER(FMatrix44f, local_to_clip)
	SHADER_PARAMETER(uint32, num_splats)
	SHADER_PARAMETER_STRUCT_INCLUDE(FPackedPositionParameters, Positions)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, positionsRDG)
	SHADER_PARAMETER(uint32, bUseRDGPositions)
	SHADER_PARAMETER_SRV(Buffer<float4>, positions_float)
	SHADER_PARAMETER(uint32, bUseFloatPositions)
	SHADER_PARAMETER(uint32, distance_scale)
	SHADER_PARAMETER(uint32, distance_not_visible)
	SHADER_PARAMETER_UAV(RWBuffer<uint>, indices)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, distances)
	END_SHADER_PARAMETER_STRUCT()

public:
	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(
			TEXT("THREAD_GROUP_SIZE_X"), THREAD_GROUP_SIZE_X);
	}
};

/**
 * Calculates 2x2 transform for each splat.
 */
class FComputeTransformCS final : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FComputeTransformCS);
	SHADER_USE_PARAMETER_STRUCT(FComputeTransformCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	SHADER_PARAMETER(FMatrix44f, local_to_view)
	SHADER_PARAMETER(float, two_focal_length)
	SHADER_PARAMETER(uint32, num_splats)
	SHADER_PARAMETER_STRUCT_INCLUDE(FPackedPositionParameters, Positions)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, positionsRDG)
	SHADER_PARAMETER(uint32, bUseRDGPositions)
	SHADER_PARAMETER_SRV(Buffer<float4>, positions_float)
	SHADER_PARAMETER(uint32, bUseFloatPositions)
	SHADER_PARAMETER_SRV(Buffer<uint2>, covariances)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint2>, covariancesRDG)
	SHADER_PARAMETER(uint32, bUseRDGCovariances)
	SHADER_PARAMETER_SRV(Buffer<float4>, covariances_float)
	SHADER_PARAMETER(uint32, bUseFloatCovariances)
	SHADER_PARAMETER_UAV(RWBuffer<float4>, transforms)
	END_SHADER_PARAMETER_STRUCT()

public:
	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(
			TEXT("THREAD_GROUP_SIZE_X"), THREAD_GROUP_SIZE_X);
	}
};

/**
 * For controlling shader parameters in RenderSplatVS.
 */
enum class ESortingDevice : uint8
{
	GPU = 0,
	CPU = 1
};

/**
 * Parameters shared between both CPU and GPU sorting versions.
 */
BEGIN_SHADER_PARAMETER_STRUCT(FRenderSplatSharedParameters, )
SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
SHADER_PARAMETER_STRUCT_REF(
	FInstancedViewUniformShaderParameters, InstancedView)
SHADER_PARAMETER(FMatrix44f, local_to_world)
SHADER_PARAMETER(uint32, sh_degree)
SHADER_PARAMETER(uint32, sh_num_triplets)
SHADER_PARAMETER(float, opacity)
SHADER_PARAMETER(float, brightness)
SHADER_PARAMETER(float, contrast)
SHADER_PARAMETER(FVector3f, color_tint)
SHADER_PARAMETER_STRUCT_INCLUDE(FPackedPositionParameters, Positions)
SHADER_PARAMETER_SRV(Buffer<float4>, positions_float)
SHADER_PARAMETER(uint32, bUseFloatPositions)
SHADER_PARAMETER_SRV(Buffer<float4>, transforms)
SHADER_PARAMETER_SRV(Buffer<float4>, colors)
SHADER_PARAMETER_SRV(Buffer<float3>, sh_coefficients)
END_SHADER_PARAMETER_STRUCT()

// WDD-2026-04-05 01:00 用rdg_前缀避免与FPackedPositionParameters同名字段的运行时绑定冲突
/**
 * Per splat, creates a containing triangle.
 */
template <ESortingDevice Device>
class FRenderSplatVS final : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRenderSplatVS);
	SHADER_USE_PARAMETER_STRUCT(FRenderSplatVS, FGlobalShader);

	// With HLSLTypeAliases.h, gives access to HLSL style types outside parameter
	// struct.
	using T = std::conditional_t<
		Device == ESortingDevice::GPU,
		UE::HLSL::uint,
		UE::HLSL::uint2>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FRenderSplatSharedParameters, Shared)
	SHADER_PARAMETER_SRV(Buffer<T>, Indices)
	END_SHADER_PARAMETER_STRUCT()

public:
	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment)
	{
		if constexpr (Device == ESortingDevice::GPU)
		{
			FGlobalShader::ModifyCompilationEnvironment(
				Parameters, OutEnvironment);
			OutEnvironment.SetDefine(TEXT("GPU_SORT"), 1);
		}
	}
};

/**
 * Draws a splat into each triangle.
 */
class FRenderSplatPS final : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRenderSplatPS);
	SHADER_USE_PARAMETER_STRUCT(FRenderSplatPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()
};

/**
 * Dynamic interpolation of 3D Gaussian Splats on GPU.
 */
class FInterpolate4DCS final : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FInterpolate4DCS);
	SHADER_USE_PARAMETER_STRUCT(FInterpolate4DCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	SHADER_PARAMETER(float, current_frame)
	SHADER_PARAMETER(uint32, num_xyz_banks)
	SHADER_PARAMETER(uint32, num_rot_banks)
	SHADER_PARAMETER(uint32, num_dc_banks)
	SHADER_PARAMETER(uint32, xyz_stride)
	SHADER_PARAMETER(uint32, rot_stride)
	SHADER_PARAMETER(uint32, dc_stride)
	SHADER_PARAMETER(uint32, num_splats)
	SHADER_PARAMETER(FVector3f, pos_scale_cm)
	SHADER_PARAMETER(FVector3f, pos_min_cm)

	SHADER_PARAMETER_SRV(Buffer<float3>, xyz_bank)
	SHADER_PARAMETER_SRV(Buffer<float4>, rot_bank)
	SHADER_PARAMETER_SRV(Buffer<float3>, dc_bank)
	SHADER_PARAMETER_SRV(Buffer<float2>, lifetime_mu_w)
	SHADER_PARAMETER_SRV(Buffer<float3>, scales)
	SHADER_PARAMETER_SRV(Buffer<float4>, base_colors)

	SHADER_PARAMETER_UAV(RWBuffer<float4>, out_positions)
	SHADER_PARAMETER_UAV(RWBuffer<float4>, out_covariances)
	SHADER_PARAMETER_UAV(RWBuffer<float4>, out_colors)
	END_SHADER_PARAMETER_STRUCT()

public:
	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREAD_GROUP_SIZE_X"), 64);
	}
};

/**
 * Append a per-proxy local distance buffer into global sort buffers.
 */
class FCopyLocalSortToGlobalCS final : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCopyLocalSortToGlobalCS);
	SHADER_USE_PARAMETER_STRUCT(FCopyLocalSortToGlobalCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	SHADER_PARAMETER(uint32, global_offset)
	SHADER_PARAMETER(uint32, num_splats)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, local_distances)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, out_global_indices)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, out_global_distances)
	END_SHADER_PARAMETER_STRUCT()

public:
	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREAD_GROUP_SIZE_X"), THREAD_GROUP_SIZE_X);
	}
};

/**
 * Gather per-proxy render data into global per-splat buffers.
 */
class FGatherProxyRenderDataToGlobalCS final : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGatherProxyRenderDataToGlobalCS);
	SHADER_USE_PARAMETER_STRUCT(FGatherProxyRenderDataToGlobalCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	SHADER_PARAMETER(FMatrix44f, local_to_world)
	SHADER_PARAMETER(FMatrix44f, local_to_view)
	SHADER_PARAMETER(FMatrix44f, local_to_clip)
	SHADER_PARAMETER(uint32, global_offset)
	SHADER_PARAMETER(uint32, num_splats)
	SHADER_PARAMETER(uint32, sh_degree)
	SHADER_PARAMETER(uint32, sh_num_triplets)
	SHADER_PARAMETER(float, two_focal_length)
	SHADER_PARAMETER(float, opacity)
	SHADER_PARAMETER(float, brightness)
	SHADER_PARAMETER(float, contrast)
	SHADER_PARAMETER(FVector3f, color_tint)
	SHADER_PARAMETER(FVector3f, world_camera_origin)
	SHADER_PARAMETER_STRUCT_INCLUDE(FPackedPositionParameters, Positions)
	SHADER_PARAMETER_SRV(Buffer<float4>, positions_float)
	SHADER_PARAMETER(uint32, bUseFloatPositions)
	SHADER_PARAMETER_SRV(Buffer<uint2>, covariances)
	SHADER_PARAMETER_SRV(Buffer<float4>, covariances_float)
	SHADER_PARAMETER(uint32, bUseFloatCovariances)
	SHADER_PARAMETER_SRV(Buffer<float4>, colors)
	SHADER_PARAMETER_SRV(Buffer<float3>, sh_coefficients)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, out_global_indices)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, out_global_distances)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, out_global_world_centers)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, out_global_transforms)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, out_global_colors)
	END_SHADER_PARAMETER_STRUCT()

public:
	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREAD_GROUP_SIZE_X"), THREAD_GROUP_SIZE_X);
	}
};

/**
 * Render globally sorted splats from global buffers.
 */
class FRenderGlobalSplatVS final : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRenderGlobalSplatVS);
	SHADER_USE_PARAMETER_STRUCT(FRenderGlobalSplatVS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
	SHADER_PARAMETER_STRUCT_REF(FInstancedViewUniformShaderParameters, InstancedView)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SortedGlobalIndices)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, GlobalWorldCenters)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, GlobalTransforms)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, GlobalColors)
	END_SHADER_PARAMETER_STRUCT()
};

} // namespace Shaders

BEGIN_SHADER_PARAMETER_STRUCT(FRenderSplatCPUSortDeps, )
SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint2>, Indices)
SHADER_PARAMETER_STRUCT_INCLUDE(
	Shaders::FRenderSplatVS<Shaders::ESortingDevice::CPU>::FParameters, VS)
SHADER_PARAMETER_STRUCT_INCLUDE(Shaders::FRenderSplatPS::FParameters, PS)
END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FRenderSplatGPUSortDeps, )
SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, Indices)
SHADER_PARAMETER_STRUCT_INCLUDE(
	Shaders::FRenderSplatVS<Shaders::ESortingDevice::GPU>::FParameters, VS)
SHADER_PARAMETER_STRUCT_INCLUDE(Shaders::FRenderSplatPS::FParameters, PS)
END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FRenderGlobalSplatDeps, )
SHADER_PARAMETER_STRUCT_INCLUDE(Shaders::FRenderGlobalSplatVS::FParameters, VS)
SHADER_PARAMETER_STRUCT_INCLUDE(Shaders::FRenderSplatPS::FParameters, PS)
END_SHADER_PARAMETER_STRUCT()

} // namespace Easytime::Splat
