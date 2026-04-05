/*
  Copyright (c) 2025 Easytime Technology Co., Ltd. See LICENSE.md.
*/

#pragma once

#include "AssetDefinitionDefault.h"
#include "SplatAsset.h"
#include "SplatConstants.h"

#include "AssetDefinition_Splat.generated.h"

/**
 * Metadata about `USplatAsset` for Editor UI.
 */
UCLASS()
class UAssetDefinition_Splat final : public UAssetDefinitionDefault
{
	GENERATED_BODY()

public:
	//~ Begin UAssetDefinition Interface
	virtual FText GetAssetDisplayName() const override
	{
		return NSLOCTEXT(
			"AssetTypeActions", "AssetTypeActions_Splat", "Splat Asset");
	}
	virtual TSoftClassPtr<UObject> GetAssetClass() const override
	{
		return USplatAsset::StaticClass();
	}
	virtual FLinearColor GetAssetColor() const override
	{
		return Easytime::Splat::EditorColor.ReinterpretAsLinear();
	}
	//~ End UAssetDefinition Interface
};

/**
 * Metadata about `USplat4DAsset` for Editor UI.
 */
UCLASS()
class UAssetDefinition_Splat4D final : public UAssetDefinitionDefault
{
	GENERATED_BODY()

public:
	//~ Begin UAssetDefinition Interface
	virtual FText GetAssetDisplayName() const override
	{
		return NSLOCTEXT(
			"AssetTypeActions", "AssetTypeActions_Splat4D", "Splat4D Asset");
	}
	virtual TSoftClassPtr<UObject> GetAssetClass() const override
	{
		return USplat4DAsset::StaticClass();
	}
	virtual FLinearColor GetAssetColor() const override
	{
		return FLinearColor(0.20f, 0.62f, 0.95f);
	}
	//~ End UAssetDefinition Interface
};

#undef LOCTEXT_NAMESPACE

