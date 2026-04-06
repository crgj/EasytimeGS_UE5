/*
  Copyright (c) 2025 Easytime Technology Co., Ltd. See LICENSE.md.
*/

#pragma once

#include "RHICommandList.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphResources.h"
#include "SceneView.h"
#include "SplatSceneProxy.h"
#include "SplatShaders.h"

namespace Easytime::Splat
{

/**
 * Adds 4D interpolation pass.
 */
FRDGPassRef Interpolate4D(
	FRDGBuilder& GraphBuilder,
	FSplatSceneProxy* Proxy,
	float CurrentFrame);

/**
 * Adds distance calculation compute shader pass.
 *
 * @param GraphBuilder - Graph to add pass to.
 * @param View - View to measure distance from.
 * @param Proxy - Splat proxy to measure.
 * @param Indices - Output buffer, populated with the index of each splat.
 * @param Distances - Output buffer, populated with the distance to each splat.
 * @param InterpPositions - Optional RDG buffer for interpolated positions.
 * @return A reference to the added pass.
 */
FRDGPassRef CalculateDistances(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FSplatSceneProxy* Proxy,
	FRDGBufferRef Indices,
	FRDGBufferRef Distances,
	FRDGBufferRef InterpPositions = nullptr);

/**
 * Adds transform calculation compute shader pass.
 *
 * @param GraphBuilder - Graph to add pass to.
 * @param View - View to transform relative to.
 * @param Proxy - Splat proxy to transform.
 * @param InterpPositions - Optional RDG buffer for interpolated positions.
 * @param InterpCovariances - Optional RDG buffer for interpolated covariances.
 * @return A reference to the added pass.
 */
FRDGPassRef ComputeTransforms(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FSplatSceneProxy* Proxy,
	FRDGBufferRef InterpPositions = nullptr,
	FRDGBufferRef InterpCovariances = nullptr);

/**
 * Draws a splat, sorted by CPU.
 *
 * @param RHICmdList - Command list to write to.
 * @param SplatParameters - Parameters for draw.
 * @param NumSplats - Number of splats to draw.
 * @param View - View to draw for.
 */
void RenderSplatCPUSort(
	FRHICommandList& RHICmdList,
	FRenderSplatCPUSortDeps* SplatParameters,
	uint32 NumSplats,
	const FSceneView& View);

/**
 * Draws a splat, sorted by GPU.
 *
 * @param RHICmdList - Command list to write to.
 * @param SplatParameters - Parameters for draw.
 * @param NumSplats - Number of splats to draw.
 * @param View - View to draw for.
 */
void RenderSplatGPUSort(
	FRHICommandList& RHICmdList,
	FRenderSplatGPUSortDeps* SplatParameters,
	uint32 NumSplats,
	const FSceneView& View);

/**
 * Adds GPU sorting pass.
 *
 * @param GraphBuilder - Graph to add pass to.
 * @param View - View sorting is relative to.
 * @param Proxy - Splat proxy to sort.
 * @param Indices - In/out buffer, sorted by matching distance.
 * @param Distances - In/out buffer, sorted.
 * @return A reference to the added pass.
 */
FRDGPassRef SortSplats(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FSplatSceneProxy* Proxy,
	FRDGBufferRef Indices,
	FRDGBufferRef Distances);

/**
 * Appends local (per-proxy) distance data into global sort buffers.
 */
FRDGPassRef AppendLocalSortToGlobal(
	FRDGBuilder& GraphBuilder,
	uint32 GlobalOffset,
	uint32 NumSplats,
	FRDGBufferRef LocalDistances,
	FRDGBufferRef GlobalIndices,
	FRDGBufferRef GlobalDistances);

/**
 * Sorts global per-splat buffers (value = global splat id).
 */
FRDGPassRef SortGlobalSplats(
	FRDGBuilder& GraphBuilder,
	FRDGBufferRef GlobalIndices,
	FRDGBufferRef GlobalDistances,
	uint32 TotalVisibleSplats);

/**
 * Gather one proxy's render inputs into global render buffers.
 */
FRDGPassRef GatherProxyRenderDataToGlobal(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FSplatSceneProxy* Proxy,
	uint32 GlobalOffset,
	uint32 NumSplats,
	FRDGBufferRef GlobalIndices,
	FRDGBufferRef GlobalDistances,
	FRDGBufferRef GlobalWorldCenters,
	FRDGBufferRef GlobalTransforms,
	FRDGBufferRef GlobalColors);

/**
 * Draw globally sorted splats in one draw call.
 */
void RenderGlobalSplats(
	FRHICommandList& RHICmdList,
	FRenderGlobalSplatDeps* Parameters,
	uint32 TotalVisibleSplats,
	const FSceneView& View);

} // namespace Easytime::Splat

