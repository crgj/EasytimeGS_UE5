/*
  Copyright (c) 2025 Easytime Technology Co., Ltd. See LICENSE.md.
*/

#pragma once

#include "Factories/Factory.h"
#include "SplatAsset.h"

#include "Splat4DAssetFactory.generated.h"

/**
 * Importer for 4DGS `.ply4` and `.sog4` files.
 */
UCLASS()
class USplat4DAssetFactory final : public UFactory
{
	GENERATED_BODY()

public:
	/**
	 * Registers `.ply4` / `.sog4` file types for import as 4D Gaussian Splat assets.
	 */
	USplat4DAssetFactory();

	/**
	 * Imports splat `.ply4` / `.sog4` files into `USplat4DAsset`s.
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

private:
	UObject* ImportSOG4(
		UObject* InParent,
		FName InName,
		EObjectFlags Flags,
		const uint8* Buffer,
		const uint8* BufferEnd);
};
