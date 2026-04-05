/*
  Copyright (c) 2025 Easytime Technology Co., Ltd. See LICENSE.md.
*/

#include "ActorFactorySplat4D.h"

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
	Splat4DActor->Splat4DComponent->Asset = CastChecked<USplat4DAsset>(Asset);
}
