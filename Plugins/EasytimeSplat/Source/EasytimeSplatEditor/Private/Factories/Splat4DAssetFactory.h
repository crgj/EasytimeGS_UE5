/*
  Copyright (c) 2025 Easytime Technology Co., Ltd. See LICENSE.md.
*/

#pragma once

#include "Factories/Factory.h"
#include "SplatAsset.h"

#include "Splat4DAssetFactory.generated.h"

/**
 * Importer for 4DGS `.ply4` files.
 */
UCLASS()
class USplat4DAssetFactory final : public UFactory
{
	GENERATED_BODY()

public:
	/**
	 * Registers `.ply4` file type for import as a 4D Gaussian Splat asset.
	 */
	USplat4DAssetFactory();

	/**
	 * Imports splat `.ply4` files into `USplat4DAsset`s.
	 */
	virtual UObject* FactoryCreateBinary(
		UClass* InClass,
		UObject* InParent,
		FName InName,
		EObjectFlags Flags,
		UObject* Context,
		const TCHAR* Type,
		const uint8*& Buffer,
		const uint8* BufferEnd,
		FFeedbackContext* Warn) override;
};
