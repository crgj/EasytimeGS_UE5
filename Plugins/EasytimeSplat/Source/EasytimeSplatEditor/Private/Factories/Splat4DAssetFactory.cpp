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
FQuat4f SafeNormalizedQuat(const FQuat4f& InQuat)
{
	const FVector4f V(InQuat.X, InQuat.Y, InQuat.Z, InQuat.W);
	if (!V.ContainsNaN() && FMath::IsFinite(V.X) && FMath::IsFinite(V.Y) && FMath::IsFinite(V.Z) && FMath::IsFinite(V.W))
	{
		const float Sq = InQuat.SizeSquared();
		if (Sq > KINDA_SMALL_NUMBER)
		{
			return InQuat.GetNormalized();
		}
	}
	return FQuat4f::Identity;
}

void MaybeAddIndex4D(TMap<uint32, uint32>& IndexMap, uint32 Index)
{
	if (!IndexMap.Contains(Index))
	{
		IndexMap.Add(Index, IndexMap.Num());
	}
}

TMap<uint32, uint32>
RemapIndices4D(TConstArrayView<UE::Geometry::FIndex3i> Indices)
{
	TMap<uint32, uint32> IndexMap{};

	for (const auto& Index3 : Indices)
	{
		MaybeAddIndex4D(IndexMap, Index3.A);
		MaybeAddIndex4D(IndexMap, Index3.B);
		MaybeAddIndex4D(IndexMap, Index3.C);
	}

	return IndexMap;
}

bool GenerateConvexHull4D(
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

	TMap<uint32, uint32> IndexMap = RemapIndices4D(HullIndices);

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
	EASYTIME_LOGL(
		"[4D Import] splats=%d frames=%d xyzBanks=%d rotBanks=%d dcBanks=%d shTriplets=%d xyzStride=%d rotStride=%d dcStride=%d",
		(int32)PLYMetadata.num_splats,
		PLYMetadata.total_frames,
		PLYMetadata.num_xyz_banks,
		PLYMetadata.num_rot_banks,
		PLYMetadata.num_dc_banks,
		PLYMetadata.num_sh_triplets,
		PLYMetadata.xyz_stride,
		PLYMetadata.rot_stride,
		PLYMetadata.dc_stride);

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

	// Base-frame import for UE runtime path uses the same coordinate conversion
	// as 3DGS import.
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

		C[Index] = FVector4f(
			to_color_srgb_float(Get(Property::DCRed)),
			to_color_srgb_float(Get(Property::DCGreen)),
			to_color_srgb_float(Get(Property::DCBlue)),
			to_alpha_linear_float(Get(Property::Opacity)));
	};

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
		LifetimeData.Init(FVector2f(0.0f, 1.0f), PLYMetadata.num_splats);

	// Offsets in Parser.layout / xyz_banks / rot_banks / dc_banks are relative to
	// the binary body (after end_header), not the file start.
	const uint8* SplatPtr = reinterpret_cast<const uint8*>(Parser.buffer.data());
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
			const FQuat4f RawQuat(*(float*)B1, *(float*)B2, *(float*)B3, *(float*)B0);
			RotBankData[i * PLYMetadata.num_rot_banks + b] =
				SafeNormalizedQuat(FQuat4f(-RawQuat.Z, -RawQuat.X, RawQuat.Y, RawQuat.W));
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
		const int32 NumTriplets = PLYMetadata.num_sh_triplets;
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

	// Prefer bank frame 0 as authoritative static frame for 4D assets.
	// This guarantees USplatAsset base buffers are initialized even when
	// rot_0..3 are absent in .ply4.
	if (PLYMetadata.num_xyz_banks > 0)
	{
		for (uint32 i = 0; i < PLYMetadata.num_splats; ++i)
		{
			const FVector3f& Src = XYZBankData[i * PLYMetadata.num_xyz_banks];
			Positions[i] = FVector3f(Src.Z, Src.X, -Src.Y);
		}
	}
	if (PLYMetadata.num_rot_banks > 0)
	{
		for (uint32 i = 0; i < PLYMetadata.num_splats; ++i)
		{
			Rotations[i] = SafeNormalizedQuat(RotBankData[i * PLYMetadata.num_rot_banks]);
		}
	}
	if (PLYMetadata.num_dc_banks > 0)
	{
		for (uint32 i = 0; i < PLYMetadata.num_splats; ++i)
		{
			const FVector3f& DC0 = DCBankData[i * PLYMetadata.num_dc_banks];
			const float BaseAlpha = Colors[i].W;
			Colors[i] = FVector4f(
				to_color_srgb_float(DC0.X),
				to_color_srgb_float(DC0.Y),
				to_color_srgb_float(DC0.Z),
				BaseAlpha);
		}
	}

	if (PLYMetadata.num_splats > 0)
	{
		const FVector4f& ImportedColor0 = Colors[0];
		float RawOpacity0 = 0.0f;
		if (PLYMetadata.properties.contains(Property::Opacity))
		{
			const uint8* BasePtr0 = reinterpret_cast<const uint8*>(Parser.buffer.data());
			const uint8* OpacityPtr0 = BasePtr0 + Parser.layout.at(Property::Opacity).offset;
			RawOpacity0 = *(float*)OpacityPtr0;
		}
		if (PLYMetadata.num_dc_banks > 0)
		{
			const FVector3f& DC0 = DCBankData[0];
			EASYTIME_LOGL(
				"[4D Import DC0] Raw=(%.6f, %.6f, %.6f) OpacityRaw=%.6f -> Color=(%.6f,%.6f,%.6f,%.6f)",
				DC0.X,
				DC0.Y,
				DC0.Z,
				RawOpacity0,
				ImportedColor0.X,
				ImportedColor0.Y,
				ImportedColor0.Z,
				ImportedColor0.W);

			if (PLYMetadata.num_dc_banks > 1)
			{
				const FVector3f& DC1 = DCBankData[1];
				EASYTIME_LOGL(
					"[4D Import DC1] Raw=(%.6f, %.6f, %.6f)",
					DC1.X,
					DC1.Y,
					DC1.Z);
			}
		}
		else
		{
			EASYTIME_LOGL(
				"[4D Import DC] No dc_bank found. OpacityRaw=%.6f BaseColor0=(%.6f,%.6f,%.6f,%.6f)",
				RawOpacity0,
				ImportedColor0.X,
				ImportedColor0.Y,
				ImportedColor0.Z,
				ImportedColor0.W);
		}

		if (PLYMetadata.num_sh_triplets > 0)
		{
			const FVector3f& SH0 = SHCoefficients[0];
			EASYTIME_LOGL(
				"[4D Import SH0] Raw=(%.6f, %.6f, %.6f) triplets=%d",
				SH0.X,
				SH0.Y,
				SH0.Z,
				PLYMetadata.num_sh_triplets);
			if (PLYMetadata.num_sh_triplets >= 3)
			{
				const FVector3f& SH1 = SHCoefficients[1];
				const FVector3f& SH2 = SHCoefficients[2];
				EASYTIME_LOGL(
					"[4D Import SH1-2] Raw1=(%.6f, %.6f, %.6f) Raw2=(%.6f, %.6f, %.6f)",
					SH1.X,
					SH1.Y,
					SH1.Z,
					SH2.X,
					SH2.Y,
					SH2.Z);
			}
		}

		if (PLYMetadata.num_rot_banks > 0)
		{
			const FQuat4f& R0 = RotBankData[0];
			EASYTIME_LOGL(
				"[4D Import Rot0] UEQuat=(%.6f, %.6f, %.6f, %.6f)",
				R0.X,
				R0.Y,
				R0.Z,
				R0.W);

			if (PLYMetadata.num_rot_banks > 1)
			{
				const FQuat4f& R1 = RotBankData[1];
				EASYTIME_LOGL(
					"[4D Import Rot1] UEQuat=(%.6f, %.6f, %.6f, %.6f)",
					R1.X,
					R1.Y,
					R1.Z,
					R1.W);
			}
		}
	}

	for (FQuat4f& Q : Rotations)
	{
		Q = SafeNormalizedQuat(Q);
	}

	if (Positions.Num() > 0 && Rotations.Num() > 0 && Colors.Num() > 0)
	{
		const FVector3f& P0 = Positions[0];
		const FQuat4f& Q0 = Rotations[0];
		const FVector3f& S0 = Scales[0];
		const FVector4f& C0 = Colors[0];
		EASYTIME_LOGL(
			"[4D Import Base0] P=(%.3f,%.3f,%.3f) Q=(%.3f,%.3f,%.3f,%.3f) S=(%.6f,%.6f,%.6f) C=(%.6f,%.6f,%.6f,%.6f)",
			P0.X,
			P0.Y,
			P0.Z,
			Q0.X,
			Q0.Y,
			Q0.Z,
			Q0.W,
			S0.X,
			S0.Y,
			S0.Z,
			C0.X,
			C0.Y,
			C0.Z,
			C0.W);
	}

	FVector3f QuantMin = Positions.Num() > 0
		? Positions[0]
		: FVector3f::ZeroVector;
	FVector3f QuantMax = QuantMin;
	if (PLYMetadata.num_xyz_banks > 0)
	{
		for (uint32 i = 0; i < PLYMetadata.num_splats; ++i)
		{
			for (int32 b = 0; b < PLYMetadata.num_xyz_banks; ++b)
			{
				const FVector3f& Src = XYZBankData[i * PLYMetadata.num_xyz_banks + b];
				const FVector3f Converted(Src.Z, Src.X, -Src.Y);
				QuantMin = QuantMin.ComponentMin(Converted);
				QuantMax = QuantMax.ComponentMax(Converted);
			}
		}
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

	ResultAsset->SetPositionsMeters(std::move(Positions), QuantMin, QuantMax);
	ResultAsset->SetCovariancesQuatScaleMeters(Rotations, Scales);
	ResultAsset->SetColorsLinear(std::move(Colors));
	ResultAsset->SetSHCoefficients(std::move(SHCoefficients), PLYMetadata.num_sh_triplets);

	if (!GenerateConvexHull4D(
			ResultAsset->GetPositions(),
			ResultAsset->ConvexHullVertices,
			ResultAsset->ConvexHullIndices))
	{
		EASYTIME_LOGE("Failed to generate convex hull for %s.", *InName.ToString());
		return nullptr;
	}

	static_cast<USplatAsset*>(ResultAsset)->BeginInit();
	ResultAsset->BeginInit();

	return ResultAsset;
}
