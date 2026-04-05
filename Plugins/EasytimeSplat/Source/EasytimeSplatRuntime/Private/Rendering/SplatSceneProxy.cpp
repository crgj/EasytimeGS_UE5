/*
  Copyright (c) 2025 Easytime Technology Co., Ltd. See LICENSE.md.
*/

#include "SplatSceneProxy.h"

#include "Logging.h"
#include "MaterialDomain.h"
#include "Materials/MaterialRenderProxy.h"
#include "PackedTypes.h"
#include "SplatConstants.h"
#include "SplatSettings.h"
#include "SplatSubsystem.h"

#if WITH_EDITOR
#include "Materials/Material.h"
#include "SceneManagement.h"
#endif

namespace Easytime::Splat
{

FSplatSceneProxy::FSplatSceneProxy(USplatComponent& Component)
	: FPrimitiveSceneProxy(&Component)
	, Asset(Component.GetAsset())
	, Transforms(Asset->GetNumSplats(), EPixelFormat::PF_FloatRGBA)
	, bIsSortingOnGPU(USplatSettings::IsSortingOnGPU())
#if WITH_EDITOR
	, VertexFactory(GetScene().GetFeatureLevel(), "FSplatSceneProxy")
	, BodySetup(Component.GetBodySetup())
#endif
{
	bIs4D = Cast<USplat4DAsset>(Asset) != nullptr;
	bUse4DInterpolation = bIs4D;
	EASYTIME_LOGL(
		"[SceneProxy Ctor] Name=%s Is4D=%d Use4DInterp=%d NumSplats=%u SortGPU=%d",
		*GetResourceName().ToString(),
		bIs4D ? 1 : 0,
		bUse4DInterpolation ? 1 : 0,
		Asset ? Asset->GetNumSplats() : 0,
		bIsSortingOnGPU ? 1 : 0);

	if (bIsSortingOnGPU)
	{
		Indices = FSplatGPUToGPUBuffer(
			Asset->GetNumSplats(), EPixelFormat::PF_R32_UINT);
	}
	else
	{
		CPUSorting = std::make_shared<FMultithreadedSortingBuffers>(
			Asset->GetNumSplats());
	}

#if WITH_EDITOR
	TConstArrayView<uint32> ConvexHullIndices = Asset->GetConvexHullIndices();
	TConstArrayView<FVector3f> ConvexHullVertices =
		Asset->GetConvexHullVertices();
	NumConvexHullTris = ConvexHullIndices.Num() / 3;

	TArray<FDynamicMeshVertex> OutVerts;
	for (int32 Index = 0; Index < ConvexHullVertices.Num(); ++Index)
	{
		OutVerts.Push(ConvexHullVertices[Index]);
	}
	// Enqueues RHI init for each buffer.
	VertexBuffers.InitFromDynamicVertex(&VertexFactory, OutVerts);

	IndexBuffer.Indices.SetNumUninitialized(ConvexHullIndices.Num());
	for (int32 Index = 0; Index < ConvexHullIndices.Num(); ++Index)
	{
		IndexBuffer.Indices[Index] = ConvexHullIndices[Index];
	}
	BeginInitResource(&IndexBuffer);

	Name = Component.GetOwner()->GetActorLabel();
#else
	Name = Component.GetOwner()->GetName();
#endif
}

FShaderResourceViewRHIRef FSplatSceneProxy::GetXYZBankSRV() const
{
	if (auto* Asset4D = Cast<USplat4DAsset>(Asset)) return Asset4D->GetXYZBankSRV();
	return nullptr;
}

FShaderResourceViewRHIRef FSplatSceneProxy::GetRotBankSRV() const
{
	if (auto* Asset4D = Cast<USplat4DAsset>(Asset)) return Asset4D->GetRotBankSRV();
	return nullptr;
}

FShaderResourceViewRHIRef FSplatSceneProxy::GetDCBankSRV() const
{
	if (auto* Asset4D = Cast<USplat4DAsset>(Asset)) return Asset4D->GetDCBankSRV();
	return nullptr;
}

FShaderResourceViewRHIRef FSplatSceneProxy::GetLifetimeMuWSRV() const
{
	if (auto* Asset4D = Cast<USplat4DAsset>(Asset)) return Asset4D->GetLifetimeMuWSRV();
	return nullptr;
}

FShaderResourceViewRHIRef FSplatSceneProxy::GetScalesSRV() const
{
	USplat4DAsset* Asset4D = Cast<USplat4DAsset>(Asset);
	return Asset4D ? Asset4D->GetScalesSRV() : nullptr;
}

void FSplatSceneProxy::Get4DMetadata(int32& OutXYZStride, int32& OutRotStride, int32& OutDCStride, uint32& OutNumXYZBanks, uint32& OutNumRotBanks, uint32& OutNumDCBanks) const
{
	USplat4DAsset* Asset4D = Cast<USplat4DAsset>(Asset);
	if (Asset4D)
	{
		OutXYZStride = Asset4D->GetXYZStride();
		OutRotStride = Asset4D->GetRotStride();
		OutDCStride = Asset4D->GetDCStride();
		OutNumXYZBanks = Asset4D->GetNumXYZBanks();
		OutNumRotBanks = Asset4D->GetNumRotBanks();
		OutNumDCBanks = Asset4D->GetNumDCBanks();
	}
	else
	{
		OutXYZStride = OutRotStride = OutDCStride = 1;
		OutNumXYZBanks = OutNumRotBanks = OutNumDCBanks = 0;
	}
}

#if WITH_EDITOR
FPrimitiveViewRelevance
FSplatSceneProxy::GetViewRelevance(const FSceneView* View) const
{
	FPrimitiveViewRelevance Result{};

	// Even with WITH_EDITOR guard, Unreal generally checks GIsEditor, so do the
	// same.
	if (GIsEditor)
	{
		// We always draw in Editor, as this is used to select the Splat Actor.
		Result.bDrawRelevance = IsShown(View);
		// Triggers a call to GetDynamicMeshElements().
		Result.bDynamicRelevance = true;
		// Enables Editor highlighting / selection outline.
		Result.bEditorStaticSelectionRelevance = (IsSelected() || IsHovered());
	}

	return Result;
}
#endif

void FSplatSceneProxy::CreateRenderThreadResources(
	FRHICommandListBase& RHICmdList)
{
	EASYTIME_LOGL(
		"[SceneProxy RTR] Name=%s Is4D=%d NumSplats=%u",
		*GetName(),
		bIs4D ? 1 : 0,
		GetNumSplats());
	if (bIsSortingOnGPU)
	{
		Indices->InitRHI(RHICmdList);
	}
	else
	{
		CPUSorting->InitResources_RenderThread(RHICmdList);
	}
	Transforms.InitRHI(RHICmdList);

	if (Uses4DInterpolation())
	{
		DynamicPositions.emplace(GetNumSplats(), PF_R32_UINT);
		DynamicPositions->InitRHI(RHICmdList);
		DynamicCovariances.emplace(GetNumSplats(), PF_R32G32_UINT);
		DynamicCovariances->InitRHI(RHICmdList);
		DynamicColors.emplace(GetNumSplats(), PF_FloatRGBA);
		DynamicColors->InitRHI(RHICmdList);

		int32 XYZStride = 1, RotStride = 1, DCStride = 1;
		uint32 NumXYZBanks = 0, NumRotBanks = 0, NumDCBanks = 0;
		Get4DMetadata(
			XYZStride,
			RotStride,
			DCStride,
			NumXYZBanks,
			NumRotBanks,
			NumDCBanks);
		EASYTIME_LOGL(
			"[SceneProxy 4D Resources] Name=%s xyzBanks=%u rotBanks=%u dcBanks=%u xyzStride=%d rotStride=%d dcStride=%d XYZ=%d ROT=%d DC=%d Life=%d Scale=%d AssetColor=%d",
			*GetName(),
			NumXYZBanks,
			NumRotBanks,
			NumDCBanks,
			XYZStride,
			RotStride,
			DCStride,
			GetXYZBankSRV().IsValid() ? 1 : 0,
			GetRotBankSRV().IsValid() ? 1 : 0,
			GetDCBankSRV().IsValid() ? 1 : 0,
			GetLifetimeMuWSRV().IsValid() ? 1 : 0,
			GetScalesSRV().IsValid() ? 1 : 0,
			GetAssetColorsSRV().IsValid() ? 1 : 0);
	}

	check(GEngine);
	USplatSubsystem* Subsystem = GEngine->GetEngineSubsystem<USplatSubsystem>();
	check(Subsystem);
	Subsystem->RegisterSplat_RenderThread(this);
}

void FSplatSceneProxy::DestroyRenderThreadResources()
{
	check(GEngine);
	USplatSubsystem* Subsystem = GEngine->GetEngineSubsystem<USplatSubsystem>();
	check(Subsystem);
	Subsystem->UnregisterSplat_RenderThread(this);

	if (bIsSortingOnGPU)
	{
		Indices->ReleaseResource();
	}
	else
	{
		CPUSorting->ReleaseResources();
	}

	Transforms.ReleaseResource();

	if (Uses4DInterpolation())
	{
		DynamicPositions->ReleaseResource();
		DynamicCovariances->ReleaseResource();
		DynamicColors->ReleaseResource();
	}

#if WITH_EDITOR
	VertexFactory.ReleaseResource();

	VertexBuffers.PositionVertexBuffer.ReleaseResource();
	VertexBuffers.StaticMeshVertexBuffer.ReleaseResource();
	VertexBuffers.ColorVertexBuffer.ReleaseResource();

	IndexBuffer.ReleaseResource();
#endif
}

#if WITH_EDITOR
void FSplatSceneProxy::GetDynamicMeshElements(
	const TArray<const FSceneView*>& Views,
	const FSceneViewFamily& ViewFamily,
	uint32 VisibilityMap,
	class FMeshElementCollector& Collector) const
{
	check(GEngine);
	check(BodySetup);

	if (GIsEditor)
	{
		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
		{
			/**
			 * Collision Views.
			 *
			 * Collision: Show > Collision.
			 * CollisionPawn: View Mode > Player Collision.
			 * CollisionVisibility: View Mode > Visibility Collision.
			 */
			const bool bDrawPawnCollision =
				ViewFamily.EngineShowFlags.CollisionPawn;
			const bool bDrawVisCollision =
				ViewFamily.EngineShowFlags.CollisionVisibility;
			const bool bDrawCollisionOverlay =
				ViewFamily.EngineShowFlags.Collision;

			const bool bIsCollisionView = AllowDebugViewmodes() &&
			                              IsCollisionEnabled() &&
			                              bDrawPawnCollision;
			const bool bIsWireframeView =
				AllowDebugViewmodes() && ViewFamily.EngineShowFlags.Wireframe;

			if (bIsCollisionView)
			{
				FLinearColor SelectionColor =
					GetSelectionColor(EditorColor, IsSelected(), IsHovered());

				TObjectPtr<UMaterial> Material;

				// If overlay is active, collisions become wireframe.
				const bool bDrawSolid = !bDrawCollisionOverlay;
				if (bDrawSolid)
				{
					Material = GEngine->ShadedLevelColorationUnlitMaterial;
				}
				else
				{
					Material = GEngine->WireframeMaterial;
				}

				// Note: This will be registered for deletion within
				// RegisterOneFrameMaterialProxy().
				FColoredMaterialRenderProxy* CollisionMaterialInstance =
					new FColoredMaterialRenderProxy(
						Material->GetRenderProxy(), SelectionColor);
				Collector.RegisterOneFrameMaterialProxy(
					CollisionMaterialInstance);
				BodySetup->AggGeom.GetAggGeom(
					FTransform(GetLocalToWorld()),
					SelectionColor.ToFColor(false),
					CollisionMaterialInstance,
					false,
					bDrawSolid,
					AlwaysHasVelocity(),
					ViewIndex,
					Collector);
			}

			/**
			 * Wireframe: View Mode > Wireframe.
			 */
			else if (bIsWireframeView)
			{
				FLinearColor ViewWireframeColor =
					ViewFamily.EngineShowFlags.ActorColoration
						? GetPrimitiveColor()
						: GetWireframeColor();

				// Note: This will be registered for deletion within
				// RegisterOneFrameMaterialProxy().
				FColoredMaterialRenderProxy* WireframeMaterialInstance =
					new FColoredMaterialRenderProxy(
						GEngine->WireframeMaterial->GetRenderProxy(),
						GetSelectionColor(
							ViewWireframeColor,
							IsSelected(),
							IsHovered(),
							false));
				Collector.RegisterOneFrameMaterialProxy(
					WireframeMaterialInstance);

				FMeshBatch& Mesh = Collector.AllocateMesh();
				Mesh.bDisableBackfaceCulling = true; // In case we're inside.
				Mesh.LODIndex = 0;
				Mesh.MaterialRenderProxy = WireframeMaterialInstance;
				Mesh.bUseWireframeSelectionColoring = IsSelected();
				Mesh.VertexFactory = &VertexFactory;
				Mesh.bWireframe = true;

				FMeshBatchElement& BatchElement = Mesh.Elements[0];
				BatchElement.FirstIndex = 0;
				BatchElement.IndexBuffer = &IndexBuffer;
				BatchElement.NumPrimitives = NumConvexHullTris;

				Collector.AddMesh(ViewIndex, Mesh);
			}

			/**
			 * If no special display, render an invisible mesh to enable mouse
			 * selection.
			 */
			else
			{
				/**
				 * Note: I haven't confirmed this is deleted by Unreal, but
				 * other scene proxies do the same thing.
				 */
				FColoredMaterialRenderProxy* HullMaterialInstance =
					new FColoredMaterialRenderProxy(
						GEngine->GeomMaterial->GetRenderProxy(),
						FLinearColor(0, 0, 0, 0));
				FMeshBatch& Mesh = Collector.AllocateMesh();
				Mesh.bDisableBackfaceCulling = true; // In case we're inside.
				Mesh.LODIndex = 0;
				Mesh.MaterialRenderProxy = HullMaterialInstance;
				Mesh.VertexFactory = &VertexFactory;

				FMeshBatchElement& BatchElement = Mesh.Elements[0];
				BatchElement.FirstIndex = 0;
				BatchElement.IndexBuffer = &IndexBuffer;
				BatchElement.NumPrimitives = NumConvexHullTris;

				Collector.AddMesh(ViewIndex, Mesh);
			}
		}
	}
}
#endif

bool FSplatSceneProxy::IsVisible(const FSceneView& View) const
{
	check(Asset);

	bool bIsShown = IsShown(&View);
	bool bIsInScene = &GetScene() == View.Family->Scene;
	bool bIsVisible = bIsShown && bIsInScene;

#if WITH_EDITOR
	const FEngineShowFlags& Flags = View.Family->EngineShowFlags;
	bool bIsWireframe = Flags.Wireframe;
	bool bIsCollision =
		Flags.Collision || Flags.CollisionPawn || Flags.CollisionVisibility;

	return !bIsWireframe && !bIsCollision && bIsVisible;
#else
	return bIsVisible;
#endif
}

void FSplatSceneProxy::TryEnqueueSort(
	const FVector3f& OriginCM, const FVector3f& Forward)
{
	check(!bIsSortingOnGPU);
	check(Asset);
	check(CPUSorting);

	if (!CPUSorting->IsReadyForSorting())
	{
		return;
	}

	// This launches a new sorting task which will `delete` itself once finished.
	// This is necessary as we otherwise must wait on the task to be completed in
	// our destructor before it can be deleted.
	// See AsyncWork.h.
	(new FAutoDeleteAsyncTask<FCPUSortingTask>(
		 Asset->GetPositions(),
		 CPUSorting,
		 OriginCM,
		 Forward,
		 FMatrix44f(GetLocalToWorld())))
		->StartBackgroundTask();
}

} // namespace Easytime::Splat

