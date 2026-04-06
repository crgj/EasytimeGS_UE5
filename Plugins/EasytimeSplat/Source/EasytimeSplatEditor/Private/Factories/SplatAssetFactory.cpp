/*
  Copyright (c) 2025 Easytime Technology Co., Ltd. See LICENSE.md.
*/

#include "SplatAssetFactory.h"

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

	// Convert indices to only reference vertices in hull.

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

USplatAssetFactory::USplatAssetFactory()
{
	SupportedClass = USplatAsset::StaticClass();

	Formats.Emplace(TEXT("ply;Gaussian splat"));
	bEditorImport = true;
}

UObject* USplatAssetFactory::FactoryCreateBinary(
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
	EASYTIME_LOGL("Loading splats from %s.", *InName.ToString());

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

	EASYTIME_LOGL(
		"[3D Import] splats=%d shTriplets=%d",
		(int32)PLYMetadata.num_splats,
		PLYMetadata.num_sh_triplets);

	TArray<FVector3f> Positions;
	Positions.SetNumUninitialized(PLYMetadata.num_splats);
	TArray<FQuat4f> Rotations;
	Rotations.SetNumUninitialized(PLYMetadata.num_splats);
	TArray<FVector3f> Scales;
	Scales.SetNumUninitialized(PLYMetadata.num_splats);
	TArray<FVector4f> Colors;
	Colors.SetNumUninitialized(PLYMetadata.num_splats);
	TArray<FVector3f> SHCoefficients;
	SHCoefficients.SetNumUninitialized(PLYMetadata.num_splats * PLYMetadata.num_sh_triplets);

	ParseSplatFn ParseSplat =
		[P = std::span<FVector3f>(&Positions[0], Positions.Num()),
	     R = std::span<FQuat4f>(&Rotations[0], Rotations.Num()),
	     S = std::span<FVector3f>(&Scales[0], Scales.Num()),
	     C = std::span<FVector4f>(&Colors[0], Colors.Num())](
			uint32_t Index, GetPropertyFn Get)
	{
		P[Index] = FVector3f(
			to<float>(Get(Property::Z)),
			to<float>(Get(Property::X)),
			-to<float>(Get(Property::Y)));

		const float X = -to<float>(Get(Property::RotationZ));
		const float Y = -to<float>(Get(Property::RotationX));
		const float Z = to<float>(Get(Property::RotationY));
		const float W = to<float>(Get(Property::RotationW));
		const float Len = FMath::Max(FMath::Sqrt(X * X + Y * Y + Z * Z + W * W), KINDA_SMALL_NUMBER);
		R[Index] = FQuat4f(X / Len, Y / Len, Z / Len, W / Len);

		S[Index] = FVector3f(
			to_scale_linear(Get(Property::ScaleZ)),
			to_scale_linear(Get(Property::ScaleX)),
			to_scale_linear(Get(Property::ScaleY)));

		// WDD-2026-04-06-LinearDCImportFix-UpgradeComment:ColorMatch-v1
		// Keep imported base color in linear space; render path/sh blending runs linear.
		C[Index] = FVector4f(
			to_color_linear_float(Get(Property::DCRed)),
			to_color_linear_float(Get(Property::DCGreen)),
			to_color_linear_float(Get(Property::DCBlue)),
			to_alpha_linear_float(Get(Property::Opacity)));
	};

	if (!Parser.parse_data(ParseSplat))
	{
		EASYTIME_LOGE("Failed to parse splats from %s.", *InName.ToString());
		return nullptr;
	}

	if (PLYMetadata.num_sh_triplets > 0)
	{
		const uint8* SplatPtr = reinterpret_cast<const uint8*>(Parser.buffer.data());
		const int32 NumTriplets = PLYMetadata.num_sh_triplets;
		for (uint32 i = 0; i < PLYMetadata.num_splats; ++i)
		{
			for (int32 t = 0; t < NumTriplets; ++t)
			{
				// GSplatData/PlayCanvas style layout:
				// f_rest_0..14   -> R channel coeffs
				// f_rest_15..29  -> G channel coeffs
				// f_rest_30..44  -> B channel coeffs
				const uint8* C0 = SplatPtr + Parser.sh_rest[t].offset;
				const uint8* C1 = SplatPtr + Parser.sh_rest[t + NumTriplets].offset;
				const uint8* C2 = SplatPtr + Parser.sh_rest[t + NumTriplets * 2].offset;
				SHCoefficients[i * NumTriplets + t] =
					FVector3f(*(float*)C0, *(float*)C1, *(float*)C2);
			}
			SplatPtr += Parser.splat_size;
		}
	}
	
	USplatAsset* ResultAsset = NewObject<USplatAsset>(InParent, InName, Flags);

	ResultAsset->SetNumSplats(PLYMetadata.num_splats);
	ResultAsset->SetPositionsMeters(std::move(Positions));
	ResultAsset->SetCovariancesQuatScaleMeters(Rotations, Scales);
	ResultAsset->SetColorsLinear(std::move(Colors));
	ResultAsset->SetSHCoefficients(std::move(SHCoefficients), PLYMetadata.num_sh_triplets);

	if (!GenerateConvexHull(
			ResultAsset->PositionsFullPrecision,
			ResultAsset->ConvexHullVertices,
			ResultAsset->ConvexHullIndices))
	{
		EASYTIME_LOGE("Failed to generate convex hull for %s.", *InName.ToString());
		return nullptr;
	}

	ResultAsset->BeginInit();

	return ResultAsset;
}

