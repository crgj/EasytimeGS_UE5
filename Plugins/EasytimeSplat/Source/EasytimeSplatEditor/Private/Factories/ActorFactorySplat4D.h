/*
  Copyright (c) 2025 Easytime Technology Co., Ltd. See LICENSE.md.
*/

#pragma once

#include "ActorFactories/ActorFactory.h"
#include "Splat4DActor.h"

#include "ActorFactorySplat4D.generated.h"

/**
 * Creates `ASplat4DActor`s from `USplat4DAsset`s in Editor.
 */
UCLASS()
class UActorFactorySplat4D final : public UActorFactory
{
	GENERATED_BODY()

public:
	UActorFactorySplat4D() { NewActorClass = ASplat4DActor::StaticClass(); }

	//~ Begin UActorFactory Interface
	virtual bool CanCreateActorFrom(
		const FAssetData& AssetData, FText& OutErrorMsg) override;
	virtual void PostSpawnActor(UObject* Asset, AActor* NewActor) override;
	//~ End UActorFactory Interface
};
