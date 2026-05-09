/*
  Copyright (c) 2025 Easytime Technology Co., Ltd. See LICENSE.md.
*/

#include "SplatSceneViewExtension.h"

#include "Logging.h"
#include "PostProcess/PostProcessing.h"
#include "SplatRendering.h"
#include "SplatRenderingUtilities.h"
#include "SplatSettings.h"
#include "RenderResource.h"
#include "StereoRendering.h"

namespace Easytime::Splat
{
namespace
{
TAutoConsoleVariable<int32> CVarSplatGlobalSortExperimental(
	TEXT("r.EasytimeSplat.GlobalSortExperimental"),
	1,
	// WDD-2026-04-06-GlobalSortStagingFlag-UpgradeComment:GlobalSortStep3-v1
	// Add runtime guard for global per-splat sort migration.
	TEXT("Enable experimental per-splat global sort pipeline staging.\n")
	TEXT("0: disabled, 1: enabled (staging only in this revision)."),
	ECVF_RenderThreadSafe);

/**
 * See comment in SplatRendering.cpp.
 */
BEGIN_SHADER_PARAMETER_STRUCT(FCPUSortRenderProducerParameters, )
SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint2>, IndicesUAV)
END_SHADER_PARAMETER_STRUCT()

struct FGlobalVisibleSplatRange
{
	FSplatSceneProxy* Proxy = nullptr;
	uint32 GlobalOffset = 0;
	uint32 NumSplats = 0;
};

struct FGlobalSortStagingBuffers
{
	FRDGBufferRef GlobalIndices = nullptr;
	FRDGBufferRef GlobalDistances = nullptr;
	FRDGBufferRef ProxyRanges = nullptr;
	FRDGBufferRef GlobalWorldCenters = nullptr;
	FRDGBufferRef GlobalTransforms = nullptr;
	FRDGBufferRef GlobalColors = nullptr;
	bool IsValid() const
	{
		return GlobalIndices && GlobalDistances && ProxyRanges &&
		       GlobalWorldCenters && GlobalTransforms && GlobalColors;
	}
};

BEGIN_SHADER_PARAMETER_STRUCT(FGlobalSortStagingProducerParameters, )
SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, GlobalIndicesUAV)
SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, GlobalDistancesUAV)
SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint2>, ProxyRangesUAV)
END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FUploadProxyRangesParameters, )
SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint2>, ProxyRangesUAV)
END_SHADER_PARAMETER_STRUCT()

struct FProxyRangeGPUEntry
{
	uint32 GlobalOffset = 0;
	uint32 NumSplats = 0;
};

Shaders::FRenderSplatSharedParameters
SetSharedParameters(const FSceneView& View, FSplatSceneProxy* Proxy)
{
	check(Proxy);

	Shaders::FRenderSplatSharedParameters Params;
	Params.View = View.ViewUniformBuffer;
	Params.InstancedView = View.GetInstancedViewUniformBuffer();
	Params.local_to_world = FMatrix44f(Proxy->GetLocalToWorld());
	Params.sh_degree = Proxy->GetSelectedSHDegree();
	Params.sh_num_triplets = Proxy->GetNumSHTriplets();
	Params.opacity = Proxy->GetOpacity();
	Params.brightness = Proxy->GetBrightness();
	Params.contrast = Proxy->GetContrast();
	Params.color_tint = Proxy->GetColorTint();
	FVector3f PosMinCM, PosScaleCM;
	Params.Positions.positions = Proxy->GetPositionsSRV(PosMinCM, PosScaleCM);
	Params.Positions.pos_min_cm = PosMinCM;
	Params.Positions.pos_scale_cm = PosScaleCM;
	Params.bUseFloatPositions = Proxy->Uses4DInterpolation() ? 1u : 0u;
	Params.positions_float =
		Proxy->Uses4DInterpolation() ? Proxy->GetDynamicPositionsSRV() : nullptr;
	Params.transforms = Proxy->GetTransformsSRV();
	Params.colors = Proxy->GetColorsSRV();
	Params.sh_coefficients = Proxy->GetSHCoefficientsSRV();

	return Params;
}

TArray<FSplatSceneProxy*> GetSortedVisibleProxies(
	const TSet<FSplatSceneProxy*>& InProxies,
	const FSceneView& View)
{
	struct FProxyDepthRange
	{
		FSplatSceneProxy* Proxy = nullptr;
		float NearDepth = 0.0f;
		float FarDepth = 0.0f;
	};

	TArray<FProxyDepthRange> DepthRanges;
	DepthRanges.Reserve(InProxies.Num());

	const FVector3f ViewOrigin = GetOrigin(View);
	const FVector3f ViewForward = GetForward(View);

	// WDD-2026-04-06-ProxyDepthRangeSort-UpgradeComment:GlobalSortStep3-v1
	// Use bounds depth interval instead of actor origin
	// to reduce cross-actor alpha ordering artifacts before true global per-splat sort is ready.
	for (FSplatSceneProxy* Proxy : InProxies)
	{
		if (!Proxy || !Proxy->IsVisible(View))
		{
			continue;
		}

		const FBoxSphereBounds ProxyBounds = Proxy->GetBounds();
		const FVector3f BoundsOrigin = FVector3f(ProxyBounds.Origin);
		const FVector3f BoundsExtent = FVector3f(ProxyBounds.BoxExtent);
		const float Radius =
			FMath::Sqrt(BoundsExtent.X * BoundsExtent.X +
			            BoundsExtent.Y * BoundsExtent.Y +
			            BoundsExtent.Z * BoundsExtent.Z);

		const float CenterDepth =
			FVector3f::DotProduct(BoundsOrigin - ViewOrigin, ViewForward);

		FProxyDepthRange& Range = DepthRanges.AddDefaulted_GetRef();
		Range.Proxy = Proxy;
		Range.NearDepth = CenterDepth - Radius;
		Range.FarDepth = CenterDepth + Radius;
	}

	DepthRanges.Sort([](const FProxyDepthRange& A, const FProxyDepthRange& B)
	{
		// WDD-2026-04-06-BackToFrontTieBreak-UpgradeComment:GlobalSortStep3-v1
		// Keep deterministic back-to-front ordering for alpha blending.
		if (A.FarDepth != B.FarDepth)
		{
			return A.FarDepth > B.FarDepth;
		}

		// WDD-2026-04-06-BackToFrontTieBreak-UpgradeComment:GlobalSortStep3-v1
		// Prefer deeper near-face on equal far-depth.
		return A.NearDepth > B.NearDepth;
	});

	TArray<FSplatSceneProxy*> Sorted;
	Sorted.Reserve(DepthRanges.Num());
	for (const FProxyDepthRange& Range : DepthRanges)
	{
		Sorted.Add(Range.Proxy);
	}

	return Sorted;
}

TArray<FGlobalVisibleSplatRange> BuildGlobalVisibleSplatRanges(
	const TArray<FSplatSceneProxy*>& SortedVisibleProxies,
	uint32& OutTotalVisibleSplats)
{
	OutTotalVisibleSplats = 0;

	TArray<FGlobalVisibleSplatRange> Ranges;
	Ranges.Reserve(SortedVisibleProxies.Num());

	for (FSplatSceneProxy* Proxy : SortedVisibleProxies)
	{
		if (!Proxy)
		{
			continue;
		}

		const uint32 NumSplats = Proxy->GetNumSplats();
		if (NumSplats == 0)
		{
			continue;
		}

		FGlobalVisibleSplatRange& Range = Ranges.AddDefaulted_GetRef();
		Range.Proxy = Proxy;
		Range.GlobalOffset = OutTotalVisibleSplats;
		Range.NumSplats = NumSplats;

		OutTotalVisibleSplats += NumSplats;
	}

	return Ranges;
}

FGlobalSortStagingBuffers CreateGlobalSortStagingBuffers(
	FRDGBuilder& GraphBuilder,
	uint32 TotalVisibleSplats,
	uint32 NumVisibleProxies)
{
	FGlobalSortStagingBuffers Buffers;
	if (TotalVisibleSplats == 0 || NumVisibleProxies == 0)
	{
		return Buffers;
	}

	Buffers.GlobalIndices = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), TotalVisibleSplats),
		TEXT("EasytimeSplat.GlobalSort.Indices"));
	Buffers.GlobalDistances = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), TotalVisibleSplats),
		TEXT("EasytimeSplat.GlobalSort.Distances"));
	Buffers.ProxyRanges = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32) * 2, NumVisibleProxies),
		TEXT("EasytimeSplat.GlobalSort.ProxyRanges"));
	Buffers.GlobalWorldCenters = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(float) * 4, TotalVisibleSplats),
		TEXT("EasytimeSplat.GlobalSort.WorldCenters"));
	Buffers.GlobalTransforms = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(float) * 4, TotalVisibleSplats),
		TEXT("EasytimeSplat.GlobalSort.Transforms"));
	Buffers.GlobalColors = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(float) * 4, TotalVisibleSplats),
		TEXT("EasytimeSplat.GlobalSort.Colors"));

	FGlobalSortStagingProducerParameters* Params =
		GraphBuilder.AllocParameters<FGlobalSortStagingProducerParameters>();
	Params->GlobalIndicesUAV =
		GraphBuilder.CreateUAV(Buffers.GlobalIndices, PF_R32_UINT);
	Params->GlobalDistancesUAV =
		GraphBuilder.CreateUAV(Buffers.GlobalDistances, PF_R32_UINT);
	Params->ProxyRangesUAV =
		GraphBuilder.CreateUAV(Buffers.ProxyRanges, PF_R32G32_UINT);

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("Splat: GlobalSort Staging Producer"),
		Params,
		ERDGPassFlags::Compute,
		[](FRHIComputeCommandList& RHICmdList) {});

	return Buffers;
}

void UploadProxyRanges(
	FRDGBuilder& GraphBuilder,
	FRDGBufferRef ProxyRangesBuffer,
	const TArray<FGlobalVisibleSplatRange>& GlobalRanges)
{
	if (!ProxyRangesBuffer || GlobalRanges.Num() == 0)
	{
		return;
	}

	TArray<FProxyRangeGPUEntry> UploadData;
	UploadData.Reserve(GlobalRanges.Num());
	for (const FGlobalVisibleSplatRange& Range : GlobalRanges)
	{
		FProxyRangeGPUEntry& Entry = UploadData.AddDefaulted_GetRef();
		Entry.GlobalOffset = Range.GlobalOffset;
		Entry.NumSplats = Range.NumSplats;
	}

	FUploadProxyRangesParameters* UploadParams =
		GraphBuilder.AllocParameters<FUploadProxyRangesParameters>();
	UploadParams->ProxyRangesUAV =
		GraphBuilder.CreateUAV(ProxyRangesBuffer, PF_R32G32_UINT);

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("Splat: UploadProxyRanges"),
		UploadParams,
		ERDGPassFlags::Compute,
		[ProxyRangesBuffer, UploadData = MoveTemp(UploadData)](
			FRHIComputeCommandList& RHICmdList)
		{
			const uint32 CopyBytes =
				UploadData.Num() * sizeof(FProxyRangeGPUEntry);
			if (CopyBytes == 0)
			{
				return;
			}

			// WDD-2026-04-06-UploadProxyRanges-UpgradeComment:GlobalSortStep3-v2
			// Upload (globalOffset, numSplats) for each visible proxy to GPU so
			// next global draw stage can map global ids back to proxy ranges.
			void* Dst = RHICmdList.LockBuffer(
				ProxyRangesBuffer->GetRHI(),
				0,
				CopyBytes,
				RLM_WriteOnly);
			FMemory::Memcpy(Dst, UploadData.GetData(), CopyBytes);
			RHICmdList.UnlockBuffer(ProxyRangesBuffer->GetRHI());
		});
}
} // namespace

FSplatSceneViewExtension::FSplatSceneViewExtension(
	const FAutoRegister& AutoRegister)
	: FSceneViewExtensionBase(AutoRegister)
	, bIsSortingOnGPU(USplatSettings::IsSortingOnGPU())
	, Proxies()
{
	FSceneViewExtensionIsActiveFunctor IsActiveFunctor;
	IsActiveFunctor.IsActiveFunction =
		[](const ISceneViewExtension* SceneViewExtension,
	       const FSceneViewExtensionContext& Context)
	{
		check(SceneViewExtension);
		const FSplatSceneViewExtension* SplatSceneViewExtension =
			static_cast<const FSplatSceneViewExtension*>(SceneViewExtension);

		return TOptional<bool>(SplatSceneViewExtension->Proxies.Num() > 0);
	};

	IsActiveThisFrameFunctions.Add(IsActiveFunctor);
}

void FSplatSceneViewExtension::PreRenderView_RenderThread(
	FRDGBuilder& GraphBuilder, FSceneView& View)
{
	GlobalSortFrameState = {};
	/**
	 * Full & primary passes do actual splat calculations, which are shared with
	 * secondary passes (if applicable).
	 *
	 * Full pass: Non-stereo.
	 * Primary: First eye, or both (e.g. instanced stereo or multiview).
	 * Secondary: Second eye.
	 */
	if (IStereoRendering::IsASecondaryView(View))
	{
		return;
	}

	const TArray<FSplatSceneProxy*> SortedProxies = GetSortedVisibleProxies(Proxies, View);
	uint32 TotalVisibleSplats = 0;
	const TArray<FGlobalVisibleSplatRange> GlobalRanges =
		BuildGlobalVisibleSplatRanges(SortedProxies, TotalVisibleSplats);
	bool bHasVisible4DProxy = false;
	for (const FGlobalVisibleSplatRange& Range : GlobalRanges)
	{
		if (Range.Proxy && Range.Proxy->Uses4DInterpolation())
		{
			bHasVisible4DProxy = true;
			break;
		}
	}
	const bool bEnableGlobalSortStaging =
		CVarSplatGlobalSortExperimental.GetValueOnRenderThread() != 0;
	FGlobalSortStagingBuffers GlobalSortBuffers;
	if (bEnableGlobalSortStaging && !bHasVisible4DProxy && GlobalRanges.Num() > 1)
	{
		GlobalSortBuffers = CreateGlobalSortStagingBuffers(
			GraphBuilder, TotalVisibleSplats, GlobalRanges.Num());
		if (GlobalSortBuffers.IsValid())
		{
			UploadProxyRanges(
				GraphBuilder,
				GlobalSortBuffers.ProxyRanges,
				GlobalRanges);
		}

		EASYTIME_LOGL(
			"[GlobalSort Stage] VisibleProxies=%d TotalVisibleSplats=%u Buffers=%d",
			GlobalRanges.Num(),
			TotalVisibleSplats,
			GlobalSortBuffers.IsValid() ? 1 : 0);
	}
	const bool bUseUnifiedGlobalSort =
		GlobalSortBuffers.IsValid() && TotalVisibleSplats > 1;
	uint32 RunningGlobalOffset = 0;
	for (FSplatSceneProxy* Proxy : SortedProxies)
	{
		check(Proxy);
		if (Proxy->Is4D())
		{
			EASYTIME_LOGL(
				"[PreRenderView] 4D Proxy=%s Visible=%d NeedsSort=%d Use4DInterp=%d CurrentFrame=%.3f",
				*Proxy->GetName(),
				Proxy->IsVisible(View) ? 1 : 0,
				Proxy->NeedsSort() ? 1 : 0,
				Proxy->Uses4DInterpolation() ? 1 : 0,
				Proxy->GetCurrentFrame());
		}

		if (!Proxy->IsVisible(View))
		{
			continue;
		}

		uint32 NumSplats = Proxy->GetNumSplats();

		if (Proxy->Uses4DInterpolation())
		{
			Interpolate4D(GraphBuilder, Proxy, Proxy->GetCurrentFrame());
		}

		const bool bProxySortGPU = Proxy->IsSortingOnGPU();
		if (bUseUnifiedGlobalSort)
		{
			// WDD-2026-04-06-UnifiedGlobalGather-UpgradeComment:GlobalSortStep4-v5
			// In unified global sort mode, all proxies (CPU/GPU sort legacy modes)
			// must feed the same global buffers.
			GatherProxyRenderDataToGlobal(
				GraphBuilder,
				View,
				Proxy,
				RunningGlobalOffset,
				NumSplats,
				GlobalSortBuffers.GlobalIndices,
				GlobalSortBuffers.GlobalDistances,
				GlobalSortBuffers.GlobalWorldCenters,
				GlobalSortBuffers.GlobalTransforms,
				GlobalSortBuffers.GlobalColors);
			RunningGlobalOffset += NumSplats;
			continue;
		}
		FRDGPassRef ProjPass = ComputeTransforms(GraphBuilder, View, Proxy);

		if (bProxySortGPU)
		{
			FRDGBufferDesc IndexDesc =
				FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumSplats);
			Proxy->GetIndicesFake() =
				GraphBuilder.CreateBuffer(IndexDesc, TEXT("Indices"));

			const bool bUse32BitDistances = Proxy->Uses4DInterpolation();
			FRDGBufferDesc DistanceDesc =
				FRDGBufferDesc::CreateBufferDesc(
					bUse32BitDistances ? sizeof(uint32) : sizeof(uint16),
					NumSplats);
			Proxy->GetDistancesFake() =
				GraphBuilder.CreateBuffer(DistanceDesc, TEXT("Distances"));

			FRDGPassRef DistPass = CalculateDistances(
				GraphBuilder,
				View,
				Proxy,
				Proxy->GetIndicesFake(),
				Proxy->GetDistancesFake(),
				nullptr);

			FRDGPassRef SortPass = SortSplats(
				GraphBuilder,
				View,
				Proxy,
				Proxy->GetIndicesFake(),
				Proxy->GetDistancesFake());

			// WDD-2026-04-06-GatherGlobalDepth32-UpgradeComment:GlobalSortStep4-v2
			// Gather per-proxy render data and write global 32-bit depth keys
			// directly, avoiding precision loss from local 16-bit distance bins.
			if (GlobalSortBuffers.IsValid())
			{
				GatherProxyRenderDataToGlobal(
					GraphBuilder,
					View,
					Proxy,
					RunningGlobalOffset,
					NumSplats,
					GlobalSortBuffers.GlobalIndices,
					GlobalSortBuffers.GlobalDistances,
					GlobalSortBuffers.GlobalWorldCenters,
					GlobalSortBuffers.GlobalTransforms,
					GlobalSortBuffers.GlobalColors);

				RunningGlobalOffset += NumSplats;
			}
		}
		else
		{
			FRDGBufferDesc IndexDesc =
				FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumSplats);
			Proxy->GetIndicesFake() = GraphBuilder.CreateBuffer(
				IndexDesc, TEXT("IndicesWithDistances"));

			Proxy->TryEnqueueSort(GetOrigin(View), GetForward(View));
		}
	}

	// WDD-2026-04-06-GlobalSortExecution-UpgradeComment:GlobalSortStep3-v1
	// Execute a single global GPU sort over all visible splats.
	if (bUseUnifiedGlobalSort)
	{
		SortGlobalSplats(
			GraphBuilder,
			GlobalSortBuffers.GlobalIndices,
			GlobalSortBuffers.GlobalDistances,
			TotalVisibleSplats);

		// WDD-2026-04-06-GlobalFrameStatePublish-UpgradeComment:GlobalSortStep4-v1
		// Publish global sorted buffers for the subsequent unified raster pass.
		GlobalSortFrameState.SortedGlobalIndices = GlobalSortBuffers.GlobalIndices;
		GlobalSortFrameState.GlobalWorldCenters = GlobalSortBuffers.GlobalWorldCenters;
		GlobalSortFrameState.GlobalTransforms = GlobalSortBuffers.GlobalTransforms;
		GlobalSortFrameState.GlobalColors = GlobalSortBuffers.GlobalColors;
		GlobalSortFrameState.TotalVisibleSplats = TotalVisibleSplats;
		GlobalSortFrameState.bValid = true;
	}
}

void FSplatSceneViewExtension::PrePostProcessPass_RenderThread(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FPostProcessingInputs& Inputs)
{
	if (GlobalSortFrameState.bValid && GlobalSortFrameState.TotalVisibleSplats > 1)
	{
		FRenderGlobalSplatDeps* GlobalPassParameters =
			GraphBuilder.AllocParameters<FRenderGlobalSplatDeps>();
		check(Inputs.SceneTextures);
		GlobalPassParameters->PS.RenderTargets[0] = FRenderTargetBinding(
			(*Inputs.SceneTextures)->SceneColorTexture,
			ERenderTargetLoadAction::ELoad);
		GlobalPassParameters->PS.RenderTargets.DepthStencil = FDepthStencilBinding(
			(*Inputs.SceneTextures)->SceneDepthTexture,
			ERenderTargetLoadAction::ELoad,
			FExclusiveDepthStencil::DepthWrite_StencilNop);
		GlobalPassParameters->VS.View = View.ViewUniformBuffer;
		GlobalPassParameters->VS.InstancedView = View.GetInstancedViewUniformBuffer();
		GlobalPassParameters->VS.SortedGlobalIndices =
			GraphBuilder.CreateSRV(GlobalSortFrameState.SortedGlobalIndices, PF_R32_UINT);
		GlobalPassParameters->VS.GlobalWorldCenters =
			GraphBuilder.CreateSRV(GlobalSortFrameState.GlobalWorldCenters, PF_A32B32G32R32F);
		GlobalPassParameters->VS.GlobalTransforms =
			GraphBuilder.CreateSRV(GlobalSortFrameState.GlobalTransforms, PF_A32B32G32R32F);
		GlobalPassParameters->VS.GlobalColors =
			GraphBuilder.CreateSRV(GlobalSortFrameState.GlobalColors, PF_A32B32G32R32F);

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("Splat: Render GlobalSorted"),
			GlobalPassParameters,
			ERDGPassFlags::Raster,
			[GlobalPassParameters, this, &View](FRHICommandList& RHICmdList)
			{
				RenderGlobalSplats(
					RHICmdList,
					GlobalPassParameters,
					GlobalSortFrameState.TotalVisibleSplats,
					View);
			});

		// WDD-2026-04-06-UnifiedGlobalDrawOnly-UpgradeComment:GlobalSortStep4-v5
		// Global sorted pass already rendered all visible proxies.
		return;
	}

	const TArray<FSplatSceneProxy*> SortedProxies = GetSortedVisibleProxies(Proxies, View);
	for (FSplatSceneProxy* Proxy : SortedProxies)
	{
		check(Proxy);
		if (Proxy->Is4D())
		{
			EASYTIME_LOGL(
				"[PrePostProcess] 4D Proxy=%s Visible=%d NeedsSort=%d Use4DInterp=%d",
				*Proxy->GetName(),
				Proxy->IsVisible(View) ? 1 : 0,
				Proxy->NeedsSort() ? 1 : 0,
				Proxy->Uses4DInterpolation() ? 1 : 0);
		}

		if (!Proxy->IsVisible(View))
		{
			continue;
		}
		if (Proxy->NeedsSort())
		{
			continue;
		}

		const bool bProxySortGPU = Proxy->IsSortingOnGPU();
		if (!bProxySortGPU)
		{
			FCPUSortRenderProducerParameters* SetupParameters =
				GraphBuilder
					.AllocParameters<FCPUSortRenderProducerParameters>();
			SetupParameters->IndicesUAV =
				GraphBuilder.CreateUAV(Proxy->GetIndicesFake(), PF_R32G32_UINT);

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("Splat: RDG Producer"),
				SetupParameters,
				ERDGPassFlags::Compute,
				[](FRHIComputeCommandList& RHICmdList) {});
		}

		Shaders::FRenderSplatSharedParameters Shared =
			SetSharedParameters(View, Proxy);
		Shaders::FRenderSplatPS::FParameters ParamsPS;
		check(Inputs.SceneTextures);
		ParamsPS.RenderTargets[0] = FRenderTargetBinding(
			(*Inputs.SceneTextures)->SceneColorTexture,
			ERenderTargetLoadAction::ELoad);
		ParamsPS.RenderTargets.DepthStencil = FDepthStencilBinding(
			(*Inputs.SceneTextures)->SceneDepthTexture,
			ERenderTargetLoadAction::ELoad,
			FExclusiveDepthStencil::DepthWrite_StencilNop);

		if (bProxySortGPU)
		{
				if (GlobalSortFrameState.bValid && GlobalSortFrameState.TotalVisibleSplats > 1)
				{
					continue;
				}
				FRenderSplatGPUSortDeps* PassParameters =
					GraphBuilder.AllocParameters<FRenderSplatGPUSortDeps>();

				PassParameters->Indices =
					GraphBuilder.CreateSRV(Proxy->GetIndicesFake(), PF_R32_UINT);
				PassParameters->VS.Shared = SetSharedParameters(View, Proxy);
				PassParameters->VS.Indices = Proxy->GetIndicesSRV();
				PassParameters->PS = ParamsPS;

				GraphBuilder.AddPass(
					RDG_EVENT_NAME("Splat: Render %s", *Proxy->GetName()),
					PassParameters,
					ERDGPassFlags::Raster,
					[this, PassParameters, Proxy, &View](
						FRHICommandList& RHICmdList)
					{
						RenderSplatGPUSort(
							RHICmdList,
							PassParameters,
							Proxy->GetNumSplats(),
							View);
					});
		}
		else
		{
				FRenderSplatCPUSortDeps* PassParameters =
					GraphBuilder.AllocParameters<FRenderSplatCPUSortDeps>();

				PassParameters->Indices =
					GraphBuilder.CreateSRV(Proxy->GetIndicesFake(), PF_R32G32_UINT);
				PassParameters->VS.Shared = SetSharedParameters(View, Proxy);
				PassParameters->VS.Indices =
					Proxy->GetIndicesSRV(); // (Index, Distance).
				PassParameters->PS = ParamsPS;

				GraphBuilder.AddPass(
					RDG_EVENT_NAME("Splat: Render %s", *Proxy->GetName()),
					PassParameters,
					ERDGPassFlags::Raster,
					[this, PassParameters, Proxy, &View](
						FRHICommandList& RHICmdList)
					{
						RenderSplatCPUSort(
							RHICmdList,
							PassParameters,
							Proxy->GetNumSplats(),
							View);
					});
		}
	}
}

void FSplatSceneViewExtension::PostRenderBasePassMobile_RenderThread(
	FRHICommandList& RHICmdList, FSceneView& InView)
{
	const TArray<FSplatSceneProxy*> SortedProxies = GetSortedVisibleProxies(Proxies, InView);
	for (FSplatSceneProxy* Proxy : SortedProxies)
	{
		check(Proxy);

		if (!Proxy->IsVisible(InView))
		{
			continue;
		}
		if (Proxy->NeedsSort())
		{
			continue;
		}

		Shaders::FRenderSplatSharedParameters Shared =
			SetSharedParameters(InView, Proxy);

		SCOPED_DRAW_EVENTF(
			RHICmdList,
			RenderSplat,
			TEXT("Splat: Render %s"),
			Proxy->GetName());
		const bool bProxySortGPU = Proxy->IsSortingOnGPU();
		if (bProxySortGPU)
		{
			FRenderSplatGPUSortDeps Parameters{};
			Parameters.VS.Shared = Shared;
			Parameters.VS.Indices = Proxy->GetIndicesSRV();
			RenderSplatGPUSort(
				RHICmdList, &Parameters, Proxy->GetNumSplats(), InView);
		}
		else
		{
			FRenderSplatCPUSortDeps Parameters{};
			Parameters.VS.Shared = Shared;
			Parameters.VS.Indices = Proxy->GetIndicesSRV();
			RenderSplatCPUSort(
				RHICmdList, &Parameters, Proxy->GetNumSplats(), InView);
		}
	}
}

} // namespace Easytime::Splat

