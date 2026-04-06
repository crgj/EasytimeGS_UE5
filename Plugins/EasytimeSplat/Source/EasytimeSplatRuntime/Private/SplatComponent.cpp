/*
  Copyright (c) 2025 Easytime Technology Co., Ltd. See LICENSE.md.
*/

#include "SplatComponent.h"

#include "Logging.h"
#include "Misc/AssertionMacros.h"
#include "Rendering/SplatSceneProxy.h"

#if WITH_EDITOR
#include "Engine.h"
#endif

using Easytime::Splat::FSplatSceneProxy;

// Defined here to avoid needing scene proxy to be module public.
FPrimitiveSceneProxy* USplatComponent::CreateSceneProxy()
{
	// Note: Unreal expects a new here, and will handle deletion itself.
	if (!Asset)
	{
		EASYTIME_LOGW("[CreateSceneProxy] %s has no Asset", *GetNameSafe(this));
		return nullptr;
	}
	if (USplat4DAsset* Asset4D = Cast<USplat4DAsset>(Asset))
	{
		if (!Asset4D->EnsureExpandedRuntimeData())
		{
			EASYTIME_LOGW("[CreateSceneProxy] Failed to prepare 4D runtime data for %s", *GetNameSafe(Asset4D));
			return nullptr;
		}
	}

	EASYTIME_LOGL(
		"[CreateSceneProxy] Component=%s Asset=%s Class=%s",
		*GetNameSafe(this),
		*GetNameSafe(Asset),
		*GetNameSafe(Asset->GetClass()));
	return new FSplatSceneProxy{*this};
}

UBodySetup* USplatComponent::GetBodySetup()
{
	if (!Asset)
	{
		return nullptr;
	}

	if (USplat4DAsset* Asset4D = Cast<USplat4DAsset>(Asset))
	{
		Asset4D->EnsureExpandedRuntimeData();
	}

	const TConstArrayView<FVector3f> ConvexHullVertices = Asset->GetConvexHullVertices();
	const bool bNeedRebuildBodySetup =
		!BodySetup ||
		BodySetup->AggGeom.ConvexElems.Num() == 0;
	if (bNeedRebuildBodySetup)
	{
		BodySetup = NewObject<UBodySetup>(this);
		BodySetup->AggGeom = FKAggregateGeom();

		FKConvexElem Convex;
		if (ConvexHullVertices.Num() >= 3)
		{
			Convex.VertexData.AddUninitialized(ConvexHullVertices.Num());
			for (int32 Index = 0; Index < ConvexHullVertices.Num(); ++Index)
			{
				Convex.VertexData[Index] = FVector(ConvexHullVertices[Index]);
			}
			Convex.UpdateElemBox();

			FKAggregateGeom AggGeom;
			AggGeom.ConvexElems.Add(Convex);
			BodySetup->AddCollisionFrom(AggGeom);
		}
		else
		{
			EASYTIME_LOGW(
				"[GetBodySetup] Convex hull unavailable for %s (vertices=%d).",
				*GetNameSafe(Asset),
				ConvexHullVertices.Num());
		}
	}

	return BodySetup;
}

#if WITH_EDITOR
void USplatComponent::GetUsedMaterials(
	TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const
{
	if (bGetDebugMaterials)
	{
		check(GEngine);
		OutMaterials.Add(GEngine->GeomMaterial);
		OutMaterials.Add(GEngine->ShadedLevelColorationUnlitMaterial);
		OutMaterials.Add(GEngine->WireframeMaterial);
	}
}

void USplatComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	if (PropertyChangedEvent.Property &&
		PropertyChangedEvent.Property->GetFName() == GET_MEMBER_NAME_CHECKED(USplatComponent, MaxSHDegree))
	{
		EASYTIME_LOGL(
			"[SplatComponent] MaxSHDegree changed to %d on %s",
			MaxSHDegree,
			*GetNameSafe(this));
		RecreateRenderState_Concurrent();
		return;
	}

	MarkRenderStateDirty();
}
#endif

FBoxSphereBounds
USplatComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	if (BodySetup)
	{
		FBoxSphereBounds NewBounds;
		BodySetup->AggGeom.CalcBoxSphereBounds(NewBounds, LocalToWorld);
		return NewBounds;
	}

	if (Asset)
	{
		TConstArrayView<FVector3f> Positions = Asset->GetPositions();
		if (Positions.Num() > 0)
		{
			FBox LocalBox(EForceInit::ForceInit);
			for (const FVector3f& Position : Positions)
			{
				LocalBox += FVector(Position);
			}

			if (LocalBox.IsValid)
			{
				return FBoxSphereBounds(LocalBox).TransformBy(LocalToWorld);
			}
		}
	}

	return FBoxSphereBounds(
		LocalToWorld.GetLocation(), FVector::ZeroVector, 0.f);
}


