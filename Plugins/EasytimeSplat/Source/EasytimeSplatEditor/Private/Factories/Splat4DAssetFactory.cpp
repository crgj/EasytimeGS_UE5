/*
  Copyright (c) 2025 Easytime Technology Co., Ltd. See LICENSE.md.
*/

#include "Splat4DAssetFactory.h"

#include "CompGeom/ConvexHull3.h"
#include "Logging.h"
#include "Misc/AssertionMacros.h"
#include "SplatConstants.h"
#include "import/ply/splat_ply_conversion.h"
#include "import/ply/splat_ply_parsing.h"

using namespace import;
using import::GetPropertyFn;
using import::Metadata;
using import::ParseSplatFn;
using import::ply::SplatParserPly;
using Easytime::Splat::MetersToCentimeters;

namespace
{
void MaybeAddIndex(TMap<uint32, uint32>& IndexMap, uint32 Index)
{
	if (!IndexMap.Contains(Index))
	{
		IndexMap.Add(Index, IndexMap.Num());
	}
}

TMap<uint32, uint32>
RemapIndices(TConstArrayView<UE::Geometry::FIndex3i> Indices)
{
	TMap<uint32, uint32> IndexMap{};

	for (const auto& Index3 : Indices)
	{
		MaybeAddIndex(IndexMap, Index3.A);
		MaybeAddIndex(IndexMap, Index3.B);
		MaybeAddIndex(IndexMap, Index3.C);
	}

	return IndexMap;
}

bool GenerateConvexHull(
	TConstArrayView<FVector3f> Positions,
	TArray<FVector3f>& OutVertices,
	TArray<uint32>& OutIndices)
{
	UE::Geometry::TConvexHull3<float> ConvexHull{};
	bool Success = ConvexHull.Solve<FVector3f>(Positions);
	if (!Success)
	{
		EASYTIME_LOGE("Failed to solve for convex hull.");
		return false;
	}

	TArray<UE::Geometry::FIndex3i> HullIndices = ConvexHull.MoveTriangles();

	TMap<uint32, uint32> IndexMap = RemapIndices(HullIndices);

	OutIndices.SetNumUninitialized(HullIndices.Num() * 3);
	for (int32 Index = 0; Index < HullIndices.Num(); ++Index)
	{
		OutIndices[Index * 3 + 0] = IndexMap[HullIndices[Index].A];
		OutIndices[Index * 3 + 1] = IndexMap[HullIndices[Index].B];
		OutIndices[Index * 3 + 2] = IndexMap[HullIndices[Index].C];
	}

	OutVertices.SetNumUninitialized(IndexMap.Num());
	for (const auto& Pair : IndexMap)
	{
		OutVertices[Pair.Value] = MetersToCentimeters * Positions[Pair.Key];
	}

	return true;
}
} // namespace

USplat4DAssetFactory::USplat4DAssetFactory()
{
	SupportedClass = USplat4DAsset::StaticClass();
	Formats.Emplace(TEXT("ply4;4D Gaussian splat (Ply4)"));
	bEditorImport = true;
}

UObject* USplat4DAssetFactory::FactoryCreateBinary(
	UClass* InClass,
	UObject* InParent,
	FName InName,
	EObjectFlags Flags,
	UObject* Context,
	const TCHAR* Type,
	const uint8*& Buffer,
	const uint8* BufferEnd,
	FFeedbackContext* Warn)
{
	EASYTIME_LOGL("Loading 4D splats from %s.", *InName.ToString());

	SplatParserPly Parser;

	Metadata PLYMetadata;
	std::span<const uint8_t> BufferView(Buffer, BufferEnd);
	if (!Parser.parse_metadata(BufferView, PLYMetadata))
	{
		EASYTIME_LOGE("Failed to parse metadata from %s.", *InName.ToString());
		return nullptr;
	}

	if (!ply::validate_metadata(PLYMetadata))
	{
		EASYTIME_LOGE("Invalid metadata for %s.", *InName.ToString());
		return nullptr;
	}

	if (PLYMetadata.total_frames <= 0)
	{
		EASYTIME_LOGE("Invalid .ply4 metadata (total_frames <= 0) for %s.", *InName.ToString());
		return nullptr;
	}

	TArray<FVector3f> Positions;
	Positions.SetNumUninitialized(PLYMetadata.num_splats);
	TArray<FQuat4f> Rotations;
	Rotations.SetNumUninitialized(PLYMetadata.num_splats);
	TArray<FVector3f> Scales;
	Scales.SetNumUninitialized(PLYMetadata.num_splats);
	TArray<FColor> Colors;
	Colors.SetNumUninitialized(PLYMetadata.num_splats);

	ParseSplatFn ParseSplat =
		[P = std::span<FVector3f>(&Positions[0], Positions.Num()),
	     R = std::span<FQuat4f>(&Rotations[0], Rotations.Num()),
	     S = std::span<FVector3f>(&Scales[0], Scales.Num()),
	     C = std::span<FColor>(&Colors[0], Colors.Num())](
			uint32_t Index, GetPropertyFn Get)
	{ ply::convert_splat<FVector3f, FQuat4f, FColor>(Index, Get, P, R, S, C); };

	if (!Parser.parse_data(ParseSplat))
	{
		EASYTIME_LOGE("Failed to parse splats from %s.", *InName.ToString());
		return nullptr;
	}

	USplat4DAsset* ResultAsset = NewObject<USplat4DAsset>(InParent, InName, Flags);
	ResultAsset->SetNumSplats(PLYMetadata.num_splats);
	ResultAsset->TotalFrames = PLYMetadata.total_frames;
	ResultAsset->XYZStride = PLYMetadata.xyz_stride;
	ResultAsset->RotStride = PLYMetadata.rot_stride;
	ResultAsset->DCStride = PLYMetadata.dc_stride;
	ResultAsset->NumXYZBanks = PLYMetadata.num_xyz_banks;
	ResultAsset->NumRotBanks = PLYMetadata.num_rot_banks;
	ResultAsset->NumDCBanks = PLYMetadata.num_dc_banks;

	TArray<FVector3f> XYZBankData;
	XYZBankData.SetNumUninitialized(PLYMetadata.num_splats * PLYMetadata.num_xyz_banks);
	TArray<FQuat4f> RotBankData;
	RotBankData.SetNumUninitialized(PLYMetadata.num_splats * PLYMetadata.num_rot_banks);
	TArray<FVector3f> DCBankData;
	DCBankData.SetNumUninitialized(PLYMetadata.num_splats * PLYMetadata.num_dc_banks);
	TArray<FVector2f> LifetimeData;
	LifetimeData.SetNumUninitialized(PLYMetadata.num_splats);

	const uint8* SplatPtr = BufferView.data();
	for (uint32_t i = 0; i < PLYMetadata.num_splats; ++i)
	{
		for (int32 b = 0; b < PLYMetadata.num_xyz_banks; ++b)
		{
			const uint8* B0 = SplatPtr + Parser.xyz_banks[b * 3 + 0].offset;
			const uint8* B1 = SplatPtr + Parser.xyz_banks[b * 3 + 1].offset;
			const uint8* B2 = SplatPtr + Parser.xyz_banks[b * 3 + 2].offset;
			XYZBankData[i * PLYMetadata.num_xyz_banks + b] =
				FVector3f(*(float*)B0, *(float*)B1, *(float*)B2);
		}
		for (int32 b = 0; b < PLYMetadata.num_rot_banks; ++b)
		{
			const uint8* B0 = SplatPtr + Parser.rot_banks[b * 4 + 0].offset;
			const uint8* B1 = SplatPtr + Parser.rot_banks[b * 4 + 1].offset;
			const uint8* B2 = SplatPtr + Parser.rot_banks[b * 4 + 2].offset;
			const uint8* B3 = SplatPtr + Parser.rot_banks[b * 4 + 3].offset;
			RotBankData[i * PLYMetadata.num_rot_banks + b] =
				FQuat4f(*(float*)B1, *(float*)B2, *(float*)B3, *(float*)B0);
		}
		for (int32 b = 0; b < PLYMetadata.num_dc_banks; ++b)
		{
			const uint8* B0 = SplatPtr + Parser.dc_banks[b * 3 + 0].offset;
			const uint8* B1 = SplatPtr + Parser.dc_banks[b * 3 + 1].offset;
			const uint8* B2 = SplatPtr + Parser.dc_banks[b * 3 + 2].offset;
			DCBankData[i * PLYMetadata.num_dc_banks + b] =
				FVector3f(*(float*)B0, *(float*)B1, *(float*)B2);
		}
		if (
			PLYMetadata.properties.contains(Property::LifetimeMu) &&
			PLYMetadata.properties.contains(Property::LifetimeW))
		{
			const uint8* MuPtr = SplatPtr + Parser.layout.at(Property::LifetimeMu).offset;
			const uint8* WPtr = SplatPtr + Parser.layout.at(Property::LifetimeW).offset;
			LifetimeData[i] = FVector2f(*(float*)MuPtr, *(float*)WPtr);
		}

		SplatPtr += Parser.splat_size;
	}

	TStaticMeshVertexData<FVector3f> XYZData;
	XYZData.Assign(XYZBankData);
	ResultAsset->XYZBank = Easytime::Splat::TSplatStaticBuffer(std::move(XYZData));

	TStaticMeshVertexData<FQuat4f> RotData;
	RotData.Assign(RotBankData);
	ResultAsset->RotBank = Easytime::Splat::TSplatStaticBuffer(std::move(RotData));

	TStaticMeshVertexData<FVector3f> DCData;
	DCData.Assign(DCBankData);
	ResultAsset->DCBank = Easytime::Splat::TSplatStaticBuffer(std::move(DCData));

	TStaticMeshVertexData<FVector2f> LData;
	LData.Assign(LifetimeData);
	ResultAsset->LifetimeMuW = Easytime::Splat::TSplatStaticBuffer(std::move(LData));

	TStaticMeshVertexData<FVector3f> SData;
	SData.Assign(Scales);
	ResultAsset->Scales = Easytime::Splat::TSplatStaticBuffer(std::move(SData));

	ResultAsset->SetPositionsMeters(std::move(Positions));
	ResultAsset->SetCovariancesQuatScaleMeters(Rotations, Scales);
	ResultAsset->SetColorsLinear(std::move(Colors));

	if (!GenerateConvexHull(
			ResultAsset->GetPositions(),
			ResultAsset->ConvexHullVertices,
			ResultAsset->ConvexHullIndices))
	{
		EASYTIME_LOGE("Failed to generate convex hull for %s.", *InName.ToString());
		return nullptr;
	}

	ResultAsset->BeginInit();

	return ResultAsset;
}
