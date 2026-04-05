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
/**
 * See comment in SplatRendering.cpp.
 */
BEGIN_SHADER_PARAMETER_STRUCT(FCPUSortRenderProducerParameters, )
SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint2>, IndicesUAV)
END_SHADER_PARAMETER_STRUCT()

Shaders::FRenderSplatSharedParameters
SetSharedParameters(const FSceneView& View, FSplatSceneProxy* Proxy)
{
	check(Proxy);

	Shaders::FRenderSplatSharedParameters Params;
	Params.View = View.ViewUniformBuffer;
	Params.InstancedView = View.GetInstancedViewUniformBuffer();
	Params.local_to_world = FMatrix44f(Proxy->GetLocalToWorld());
	FVector3f PosMinCM, PosScaleCM;
	Params.Positions.positions = Proxy->GetPositionsSRV(PosMinCM, PosScaleCM);
	Params.Positions.pos_min_cm = PosMinCM;
	Params.Positions.pos_scale_cm = PosScaleCM;
	Params.transforms = Proxy->GetTransformsSRV();
	Params.colors = Proxy->GetColorsSRV();

	return Params;
}

Shaders::FRenderSplatRDGSharedParameters
SetSharedRDGParameters(FRDGBuilder& GraphBuilder, const FSceneView& View, FSplatSceneProxy* Proxy, const FInterpolationRDGResources& Interp)
{
	check(Proxy);

	Shaders::FRenderSplatRDGSharedParameters Params;
	// WDD-2026-04-05 01:00: 鐢╮dg_鍓嶇紑璧嬪€わ紝閬垮厤涓嶧PackedPositionParameters鍚屽悕鍐茬獊
	Params.rdg_positions = GraphBuilder.CreateSRV(Interp.Positions, EPixelFormat::PF_R32_UINT);
	Params.rdg_transforms = Proxy->GetTransformsSRV(); 
	Params.rdg_colors = GraphBuilder.CreateSRV(Interp.Colors, EPixelFormat::PF_R32_UINT);

	return Params;
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

	InterpolationResources.Empty();

	for (auto& Proxy : Proxies)
	{
		check(Proxy);

		if (!Proxy->IsVisible(View))
		{
			continue;
		}

		uint32 NumSplats = Proxy->GetNumSplats();

		FRDGBufferRef InterpPositions = nullptr;
		FRDGBufferRef InterpCovariances = nullptr;
		FRDGBufferRef InterpColors = nullptr;

		if (Proxy->Is4D())
		{
			InterpPositions = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumSplats),
				TEXT("InterpolatedPositions"));
			InterpCovariances = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateBufferDesc(sizeof(uint32) * 2, NumSplats),
				TEXT("InterpolatedCovariances"));
			InterpColors = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumSplats),
				TEXT("InterpolatedColors"));

			Interpolate4D(
				GraphBuilder,
				Proxy,
				Proxy->GetCurrentFrame(),
				InterpPositions,
				InterpCovariances,
				InterpColors);

			FInterpolationRDGResources Resources;
			Resources.Positions = InterpPositions;
			Resources.Covariances = InterpCovariances;
			Resources.Colors = InterpColors;
			InterpolationResources.Add(Proxy, Resources);
		}

		FRDGPassRef ProjPass = ComputeTransforms(GraphBuilder, View, Proxy, InterpPositions, InterpCovariances);

		if (bIsSortingOnGPU)
		{
			FRDGBufferDesc IndexDesc =
				FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumSplats);
			Proxy->GetIndicesFake() =
				GraphBuilder.CreateBuffer(IndexDesc, TEXT("Indices"));

			FRDGBufferDesc DistanceDesc =
				FRDGBufferDesc::CreateBufferDesc(sizeof(uint16), NumSplats);
			Proxy->GetDistancesFake() =
				GraphBuilder.CreateBuffer(DistanceDesc, TEXT("Distances"));

			FRDGPassRef DistPass = CalculateDistances(
				GraphBuilder,
				View,
				Proxy,
				Proxy->GetIndicesFake(),
				Proxy->GetDistancesFake(),
				InterpPositions);

			FRDGPassRef SortPass = SortSplats(
				GraphBuilder,
				View,
				Proxy,
				Proxy->GetIndicesFake(),
				Proxy->GetDistancesFake());
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
}

void FSplatSceneViewExtension::PrePostProcessPass_RenderThread(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FPostProcessingInputs& Inputs)
{
	for (auto& Proxy : Proxies)
	{
		check(Proxy);

		if (!Proxy->IsVisible(View))
		{
			continue;
		}
		if (Proxy->NeedsSort())
		{
			continue;
		}

		if (!bIsSortingOnGPU)
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

		FInterpolationRDGResources* Interp = InterpolationResources.Find(Proxy);

		if (bIsSortingOnGPU)
		{
			if (Interp)
			{
				FRenderSplatGPURDGParams* PassParameters = GraphBuilder.AllocParameters<FRenderSplatGPURDGParams>();
				PassParameters->Indices = GraphBuilder.CreateSRV(Proxy->GetIndicesFake(), PF_R32_UINT);
				PassParameters->VS.SharedRDG = SetSharedRDGParameters(GraphBuilder, View, Proxy, *Interp);
				PassParameters->VS.bIsUsingRDG = 1;
				PassParameters->VS.Indices = Proxy->GetIndicesSRV();
				PassParameters->PS = ParamsPS;

				GraphBuilder.AddPass(
					RDG_EVENT_NAME("Splat: Render 4D (GPU Sort) %s", *Proxy->GetName()),
					PassParameters,
					ERDGPassFlags::Raster,
					[this, PassParameters, Proxy, &View](FRHICommandList& RHICmdList)
					{
						RenderSplatGPUSortRDG(RHICmdList, PassParameters, Proxy->GetNumSplats(), View);
					});
			}
			else
			{
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
		}
		else
		{
			if (Interp)
			{
				FRenderSplatCPURDGParams* PassParameters = GraphBuilder.AllocParameters<FRenderSplatCPURDGParams>();
				PassParameters->Indices = GraphBuilder.CreateSRV(Proxy->GetIndicesFake(), PF_R32G32_UINT);
				PassParameters->VS.SharedRDG = SetSharedRDGParameters(GraphBuilder, View, Proxy, *Interp);
				PassParameters->VS.bIsUsingRDG = 1;
				PassParameters->VS.Indices = Proxy->GetIndicesSRV(); // (Index, Distance).
				PassParameters->PS = ParamsPS;

				GraphBuilder.AddPass(
					RDG_EVENT_NAME("Splat: Render 4D (CPU Sort) %s", *Proxy->GetName()),
					PassParameters,
					ERDGPassFlags::Raster,
					[this, PassParameters, Proxy, &View](FRHICommandList& RHICmdList)
					{
						RenderSplatCPUSortRDG(RHICmdList, PassParameters, Proxy->GetNumSplats(), View);
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
}

void FSplatSceneViewExtension::PostRenderBasePassMobile_RenderThread(
	FRHICommandList& RHICmdList, FSceneView& InView)
{
	for (auto& Proxy : Proxies)
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
		if (bIsSortingOnGPU)
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

