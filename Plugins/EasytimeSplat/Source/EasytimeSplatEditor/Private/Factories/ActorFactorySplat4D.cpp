/*
  Copyright (c) 2025 Easytime Technology Co., Ltd. See LICENSE.md.
*/

#include "ActorFactorySplat4D.h"

#include "Logging.h"
#include "Misc/AssertionMacros.h"

bool UActorFactorySplat4D::CanCreateActorFrom(
	const FAssetData& AssetData, FText& OutErrorMsg)
{
	if (!AssetData.IsValid() ||
	    !AssetData.IsInstanceOf(USplat4DAsset::StaticClass()))
	{
		OutErrorMsg = NSLOCTEXT(
			"CanCreateActor",
			"NoSplat4DAsset",
			"A valid 4D splat asset must be specified.");
		return false;
	}

	return true;
}

void UActorFactorySplat4D::PostSpawnActor(UObject* Asset, AActor* NewActor)
{
	ASplat4DActor* Splat4DActor = CastChecked<ASplat4DActor>(NewActor);
	check(Splat4DActor->Splat4DComponent);
	EASYTIME_LOGL(
		"[4D Spawn] Actor=%s Asset=%s",
		*GetNameSafe(Splat4DActor),
		*GetNameSafe(Asset));
	USplat4DAsset* SplatAsset = CastChecked<USplat4DAsset>(Asset);
	Splat4DActor->Splat4DComponent->SetAsset(SplatAsset);

	Splat4DActor->CurrentFrame = 0.0f;
	Splat4DActor->Splat4DComponent->CurrentFrame = 0.0f;
}
