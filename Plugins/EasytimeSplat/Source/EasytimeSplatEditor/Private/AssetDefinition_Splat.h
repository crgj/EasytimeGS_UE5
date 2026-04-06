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

UCLASS()
class UAssetDefinition_Ply4Splat final : public UAssetDefinitionDefault
{
	GENERATED_BODY()

public:
	virtual FText GetAssetDisplayName() const override
	{
		return NSLOCTEXT(
			"AssetTypeActions", "AssetTypeActions_Ply4Splat", "PLY4 Splat Asset");
	}
	virtual TSoftClassPtr<UObject> GetAssetClass() const override
	{
		return UPly4SplatAsset::StaticClass();
	}
	virtual FText GetAssetDescription(const FAssetData& AssetData) const override
	{
		return NSLOCTEXT(
			"AssetTypeActions",
			"AssetTypeActions_Ply4Splat_Desc",
			"Expanded PLY4 splat asset stored in unified runtime buffers.");
	}
	virtual FLinearColor GetAssetColor() const override
	{
		return FLinearColor(0.20f, 0.62f, 0.95f);
	}
};

UCLASS()
class UAssetDefinition_Sog4Splat final : public UAssetDefinitionDefault
{
	GENERATED_BODY()

public:
	virtual FText GetAssetDisplayName() const override
	{
		return NSLOCTEXT(
			"AssetTypeActions", "AssetTypeActions_Sog4Splat", "SOG4 Splat Asset (Compressed)");
	}
	virtual TSoftClassPtr<UObject> GetAssetClass() const override
	{
		return USog4SplatAsset::StaticClass();
	}
	virtual FText GetAssetDescription(const FAssetData& AssetData) const override
	{
		return NSLOCTEXT(
			"AssetTypeActions",
			"AssetTypeActions_Sog4Splat_Desc",
			"Compressed SOG4 source asset. Decoded to unified runtime buffers only when used in level.");
	}
	virtual FLinearColor GetAssetColor() const override
	{
		return FLinearColor(0.12f, 0.78f, 0.46f);
	}
};

#undef LOCTEXT_NAMESPACE

