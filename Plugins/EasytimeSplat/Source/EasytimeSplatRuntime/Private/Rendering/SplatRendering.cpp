/*
  Copyright (c) 2025 Easytime Technology Co., Ltd. See LICENSE.md.
*/

#include "SplatRendering.h"

#include "GPUSort.h"
#include "Misc/AssertionMacros.h"
#include "RenderGraphUtils.h"
#include "SceneRendering.h"
#include "SplatConstants.h"
#include "SplatRenderingUtilities.h"

namespace Easytime::Splat
{
namespace
{
/**
 * HACK(seth): I'm lying to the RDG using fake SRVs to track resources not
 * actually managed by the RDG. As such, I have to pretend to write to the
 * resource in order to pass validation.
 */
BEGIN_SHADER_PARAMETER_STRUCT(FGPUSortProducerParameters, )
SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, IndicesUAV)
SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, Indices2UAV)
SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, Distances2UAV)
END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FGPUSortParameters, )
SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, IndicesSRV)
SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, IndicesUAV)
SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, Indices2SRV)
SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, Indices2UAV)
SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, DistancesSRV)
SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, DistancesUAV)
SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, Distances2SRV)
SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, Distances2UAV)
END_SHADER_PARAMETER_STRUCT()

uint32 NumThreadGroups(uint32 NumElements)
{
	return (NumElements + (Shaders::THREAD_GROUP_SIZE_X - 1)) /
	       Shaders::THREAD_GROUP_SIZE_X;
}
} // namespace

FRDGPassRef CalculateDistances(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FSplatSceneProxy* Proxy,
	FRDGBufferRef Indices,
	FRDGBufferRef Distances,
	FRDGBufferRef InterpPositions)
{
	check(Proxy);

	const FGlobalShaderMap* GlobalShaderMap =
		GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderRef<Shaders::FComputeDistanceCS> DistanceShader =
		GlobalShaderMap->GetShader<Shaders::FComputeDistanceCS>();

	FRDGBufferUAV* IndicesUAV = GraphBuilder.CreateUAV(Indices, PF_R32_UINT);
	FRDGBufferUAV* DistancesUAV =
		GraphBuilder.CreateUAV(Distances, PF_R16_UINT);

	Shaders::FComputeDistanceCS::FParameters* DistanceParams =
		GraphBuilder
			.AllocParameters<Shaders::FComputeDistanceCS::FParameters>();
	DistanceParams->local_to_clip =
		FMatrix44f(Proxy->GetLocalToWorld() * GetViewProj(View));
	DistanceParams->num_splats = Proxy->GetNumSplats();
	DistanceParams->Positions = MakePositionParams(Proxy);

	if (InterpPositions)
	{
		DistanceParams->bUseRDGPositions = 1;
		// WDD-20260405: FPackedPositionRDGParameters 宸茬畝鍖栵紝鍙湁 positionsRDG SRV
		DistanceParams->positionsRDG =
			GraphBuilder.CreateSRV(InterpPositions, PF_R32_UINT);
	}
	else
	{
		DistanceParams->bUseRDGPositions = 0;
	}

	DistanceParams->indices = Proxy->GetIndicesUAV();
	DistanceParams->distances = DistancesUAV;

	return FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME(
			"Splat: Distances %s", *Proxy->GetResourceName().ToString()),
		ERDGPassFlags::AsyncCompute,
		DistanceShader,
		DistanceParams,
		FIntVector(NumThreadGroups(Proxy->GetNumSplats()), 1, 1));
}

FRDGPassRef Interpolate4D(
	FRDGBuilder& GraphBuilder,
	FSplatSceneProxy* Proxy,
	float CurrentFrame)
{
	check(Proxy);

	const FGlobalShaderMap* GlobalShaderMap =
		GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderRef<Shaders::FInterpolate4DCS> InterpolateShader =
		GlobalShaderMap->GetShader<Shaders::FInterpolate4DCS>();

	Shaders::FInterpolate4DCS::FParameters* Params =
		GraphBuilder.AllocParameters<Shaders::FInterpolate4DCS::FParameters>();

	Params->current_frame = CurrentFrame;
	Params->num_splats = Proxy->GetNumSplats();

	FVector3f PosMinCM, PosScaleCM;
	static_cast<void>(Proxy->GetPositionsSRV(PosMinCM, PosScaleCM));
	Params->pos_min_cm = PosMinCM;
	Params->pos_scale_cm = PosScaleCM;

	Params->xyz_bank = Proxy->GetXYZBankSRV();
	Params->rot_bank = Proxy->GetRotBankSRV();
	Params->dc_bank = Proxy->GetDCBankSRV();
	Params->lifetime_mu_w = Proxy->GetLifetimeMuWSRV();
	Params->scales = Proxy->GetScalesSRV();
	Params->base_colors = Proxy->GetAssetColorsSRV();

	int32 XYZStride, RotStride, DCStride;
	uint32 NumXYZBanks, NumRotBanks, NumDCBanks;
	Proxy->Get4DMetadata(
		XYZStride, RotStride, DCStride, NumXYZBanks, NumRotBanks, NumDCBanks);

	Params->xyz_stride = XYZStride;
	Params->rot_stride = RotStride;
	Params->dc_stride = DCStride;
	Params->num_xyz_banks = NumXYZBanks;
	Params->num_rot_banks = NumRotBanks;
	Params->num_dc_banks = NumDCBanks;

	Params->out_positions = Proxy->GetDynamicPositionsUAV();
	Params->out_covariances = Proxy->GetDynamicCovariancesUAV();
	Params->out_colors = Proxy->GetDynamicColorsUAV();

	return FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME(
			"Splat: Interpolate4D %s", *Proxy->GetResourceName().ToString()),
		ERDGPassFlags::AsyncCompute,
		InterpolateShader,
		Params,
		FIntVector(NumThreadGroups(Proxy->GetNumSplats()), 1, 1));
}

FRDGPassRef ComputeTransforms(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FSplatSceneProxy* Proxy,
	FRDGBufferRef InterpPositions,
	FRDGBufferRef InterpCovariances)
{
	check(Proxy);

	const FGlobalShaderMap* GlobalShaderMap =
		GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderRef<Shaders::FComputeTransformCS> TransformShader =
		GlobalShaderMap->GetShader<Shaders::FComputeTransformCS>();

	Shaders::FComputeTransformCS::FParameters* TransformParams =
		GraphBuilder
			.AllocParameters<Shaders::FComputeTransformCS::FParameters>();

	TransformParams->local_to_view =
		FMatrix44f(Proxy->GetLocalToWorld() * GetView(View));
	TransformParams->two_focal_length = 2.f * GetFocalLength(View);
	TransformParams->num_splats = Proxy->GetNumSplats();

	FVector3f PosMinCM, PosScaleCM;
	TransformParams->Positions.positions =
		Proxy->GetPositionsSRV(PosMinCM, PosScaleCM);
	TransformParams->Positions.pos_min_cm = PosMinCM;
	TransformParams->Positions.pos_scale_cm = PosScaleCM;

	if (InterpPositions)
	{
		TransformParams->bUseRDGPositions = 1;
		TransformParams->positionsRDG =
			GraphBuilder.CreateSRV(InterpPositions, PF_R32_UINT);
	}
	else
	{
		TransformParams->bUseRDGPositions = 0;
	}

	TransformParams->covariances = Proxy->GetCovariancesSRV();
	if (InterpCovariances)
	{
		TransformParams->bUseRDGCovariances = 1;
		TransformParams->covariancesRDG =
			GraphBuilder.CreateSRV(InterpCovariances, PF_R32G32_UINT);
	}
	else
	{
		TransformParams->bUseRDGCovariances = 0;
	}

	TransformParams->transforms = Proxy->GetTransformsUAV();

	return FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME(
			"Splat: Transform %s", *Proxy->GetResourceName().ToString()),
		ERDGPassFlags::AsyncCompute,
		TransformShader,
		TransformParams,
		FIntVector(NumThreadGroups(Proxy->GetNumSplats()), 1, 1));
}

void RenderSplatCPUSort(
	FRHICommandList& RHICmdList,
	FRenderSplatCPUSortDeps* SplatParameters,
	uint32 NumSplats,
	const FSceneView& View)
{
	check(SplatParameters);

	const FGlobalShaderMap* GlobalShaderMap =
		GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderRef<Shaders::FRenderSplatVS<Shaders::ESortingDevice::CPU>>
		VertexShader = GlobalShaderMap->GetShader<
			Shaders::FRenderSplatVS<Shaders::ESortingDevice::CPU>>();
	TShaderRef<Shaders::FRenderSplatPS> PixelShader =
		GlobalShaderMap->GetShader<Shaders::FRenderSplatPS>();

	/**
	 * Sometimes in editor, the displayed area is smaller than the actual
	 * viewport size. By shrinking the viewport to the correct size, we avoid
	 * rendering the splats incorrectly (as they rely on knowing the viewport
	 * size for projection).
	 */
	check(View.bIsViewInfo);
	const FIntRect ViewRect = static_cast<const FViewInfo&>(View).ViewRect;
	RHICmdList.SetViewport(
		float(ViewRect.Min.X),
		float(ViewRect.Min.Y),
		0.f,
		float(ViewRect.Max.X),
		float(ViewRect.Max.Y),
		1.f);

	FGraphicsPipelineStateInitializer GraphicsPSOInit;
	GraphicsPSOInit.PrimitiveType = PT_TriangleList;
	GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI =
		PipelineStateCache::GetOrCreateVertexDeclaration({});
	GraphicsPSOInit.BoundShaderState.VertexShaderRHI =
		VertexShader.GetVertexShader();
	GraphicsPSOInit.BoundShaderState.PixelShaderRHI =
		PixelShader.GetPixelShader();
	GraphicsPSOInit.DepthStencilState =
		TStaticDepthStencilState<false>::GetRHI();
	GraphicsPSOInit.RasterizerState = TStaticRasterizerState<>::GetRHI();
	GraphicsPSOInit.BlendState = TStaticBlendState<
		CW_RGBA,
		BO_Add,
		BF_SourceAlpha,
		BF_InverseSourceAlpha>::GetRHI();
	RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

	SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
	SetShaderParameters(
		RHICmdList,
		VertexShader,
		VertexShader.GetVertexShader(),
		SplatParameters->VS);
	SetShaderParameters(
		RHICmdList,
		PixelShader,
		PixelShader.GetPixelShader(),
		SplatParameters->PS);

	RHICmdList.DrawPrimitive(0, 2 * NumSplats, 1);
}

void RenderSplatGPUSort(
	FRHICommandList& RHICmdList,
	FRenderSplatGPUSortDeps* SplatParameters,
	uint32 NumSplats,
	const FSceneView& View)
{
	check(SplatParameters);

	const FGlobalShaderMap* GlobalShaderMap =
		GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderRef<Shaders::FRenderSplatVS<Shaders::ESortingDevice::GPU>>
		VertexShader = GlobalShaderMap->GetShader<
			Shaders::FRenderSplatVS<Shaders::ESortingDevice::GPU>>();
	TShaderRef<Shaders::FRenderSplatPS> PixelShader =
		GlobalShaderMap->GetShader<Shaders::FRenderSplatPS>();

	check(View.bIsViewInfo);
	const FIntRect ViewRect = static_cast<const FViewInfo&>(View).ViewRect;
	RHICmdList.SetViewport(
		float(ViewRect.Min.X),
		float(ViewRect.Min.Y),
		0.f,
		float(ViewRect.Max.X),
		float(ViewRect.Max.Y),
		1.f);

	FGraphicsPipelineStateInitializer GraphicsPSOInit;
	GraphicsPSOInit.PrimitiveType = PT_TriangleList;
	GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI =
		PipelineStateCache::GetOrCreateVertexDeclaration({});
	GraphicsPSOInit.BoundShaderState.VertexShaderRHI =
		VertexShader.GetVertexShader();
	GraphicsPSOInit.BoundShaderState.PixelShaderRHI =
		PixelShader.GetPixelShader();
	GraphicsPSOInit.DepthStencilState =
		TStaticDepthStencilState<false>::GetRHI();
	GraphicsPSOInit.RasterizerState = TStaticRasterizerState<>::GetRHI();
	GraphicsPSOInit.BlendState = TStaticBlendState<
		CW_RGBA,
		BO_Add,
		BF_SourceAlpha,
		BF_InverseSourceAlpha>::GetRHI();
	RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

	SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
	SetShaderParameters(
		RHICmdList,
		VertexShader,
		VertexShader.GetVertexShader(),
		SplatParameters->VS);
	SetShaderParameters(
		RHICmdList,
		PixelShader,
		PixelShader.GetPixelShader(),
		SplatParameters->PS);

	RHICmdList.DrawPrimitive(0, 2 * NumSplats, 1);
}

FRDGPassRef SortSplats(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FSplatSceneProxy* Proxy,
	FRDGBufferRef Indices,
	FRDGBufferRef Distances)
{
	check(Proxy);
	check(Indices);
	check(Distances);

	uint32 NumSplats = Proxy->GetNumSplats();

	FRDGBufferDesc IndexDesc =
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumSplats);
	FRDGBuffer* Indices2 =
		GraphBuilder.CreateBuffer(IndexDesc, TEXT("Indices2"));

	FRDGBufferDesc DistanceDesc =
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint16), NumSplats);
	FRDGBuffer* Distances2 =
		GraphBuilder.CreateBuffer(DistanceDesc, TEXT("Distances2"));

	FGPUSortProducerParameters* SetupParameters =
		GraphBuilder.AllocParameters<FGPUSortProducerParameters>();
	SetupParameters->IndicesUAV = GraphBuilder.CreateUAV(Indices, PF_R32_UINT);
	SetupParameters->Indices2UAV =
		GraphBuilder.CreateUAV(Indices2, PF_R32_UINT);
	SetupParameters->Distances2UAV =
		GraphBuilder.CreateUAV(Distances2, PF_R16_UINT);

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("Splat: RDG Producer"),
		SetupParameters,
		ERDGPassFlags::Compute,
		[](FRHIComputeCommandList& RHICmdList) {});

	FGPUSortParameters* SortParameters =
		GraphBuilder.AllocParameters<FGPUSortParameters>();
	SortParameters->IndicesSRV = GraphBuilder.CreateSRV(Indices, PF_R32_UINT);
	SortParameters->IndicesUAV = GraphBuilder.CreateUAV(Indices, PF_R32_UINT);
	SortParameters->Indices2SRV = GraphBuilder.CreateSRV(Indices2, PF_R32_UINT);
	SortParameters->Indices2UAV = GraphBuilder.CreateUAV(Indices2, PF_R32_UINT);
	SortParameters->DistancesSRV =
		GraphBuilder.CreateSRV(Distances, PF_R16_UINT);
	SortParameters->DistancesUAV =
		GraphBuilder.CreateUAV(Distances, PF_R16_UINT);
	SortParameters->Distances2SRV =
		GraphBuilder.CreateSRV(Distances2, PF_R16_UINT);
	SortParameters->Distances2UAV =
		GraphBuilder.CreateUAV(Distances2, PF_R16_UINT);

	// `Compute` used for mobile support, but this could be `AsyncCompute`.
	// `NeverCull` ensures that this pass still happens even if RDG doesn't think
	// resources are being used.
	return GraphBuilder.AddPass(
		RDG_EVENT_NAME("Splat: Sort %s", *Proxy->GetName()),
		SortParameters,
		ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
		[NumSplats,
	     SortParameters,
	     SRV = Proxy->GetIndicesSRV(),
	     UAV = Proxy->GetIndicesUAV()](FRHIComputeCommandList& RHICmdList)
		{
			FGPUSortBuffers SortBuffers;
			SortBuffers.RemoteKeySRVs[0] =
				SortParameters->DistancesSRV->GetRHI();
			SortBuffers.RemoteKeySRVs[1] =
				SortParameters->Distances2SRV->GetRHI();
			SortBuffers.RemoteKeyUAVs[0] =
				SortParameters->DistancesUAV->GetRHI();
			SortBuffers.RemoteKeyUAVs[1] =
				SortParameters->Distances2UAV->GetRHI();
			SortBuffers.RemoteValueSRVs[0] = SRV;
			SortBuffers.RemoteValueSRVs[1] =
				SortParameters->Indices2SRV->GetRHI();
			SortBuffers.RemoteValueUAVs[0] = UAV;
			SortBuffers.RemoteValueUAVs[1] =
				SortParameters->Indices2UAV->GetRHI();

			int32 ResultIndex = SortGPUBuffers(
				static_cast<FRHICommandList&>(RHICmdList),
				SortBuffers,
				0,
				DepthMask,
				NumSplats,
				GMaxRHIFeatureLevel);
			check(ResultIndex == 0);
		});
}
} // namespace Easytime::Splat
