/*
  Copyright (c) 2025 Easytime Technology Co., Ltd. See LICENSE.md.
*/

#include "SplatAsset.h"
#include "SplatConstants.h"
#include "SplatSettings.h"

#include "RHIResources.h"
#include "Serialization/CustomVersion.h"

using Easytime::Splat::FPackedCovMat;
using Easytime::Splat::FPackedPos;
using Easytime::Splat::MetersToCentimeters;
using Easytime::Splat::TSplatStaticBuffer;

namespace
{
struct FSplatAssetCustomVersion
{
	static const FGuid GUID;

	enum Type : int32
	{
		BeforeCustomVersionWasAdded = 0,
		AddedSHCoefficients,
		AddedFloatColors,
		LatestVersion = AddedFloatColors
	};
};

const FGuid FSplatAssetCustomVersion::GUID(
	0xC90F0AF3,
	0x4D9E4A31,
	0x9D9A6F4C,
	0x1C3B8B1E);

FCustomVersionRegistration GRegisterSplatAssetCustomVersion(
	FSplatAssetCustomVersion::GUID,
	FSplatAssetCustomVersion::LatestVersion,
	TEXT("SplatAssetCustomVersion"));
} // namespace

void USplatAsset::BeginDestroy()
{
	Super::BeginDestroy();

	// Default Asset will have None for buffers.
	if (Positions)
	{
		BeginReleaseResource(&*Positions);
	}
	if (CovariancesCM)
	{
		BeginReleaseResource(&*CovariancesCM);
	}
	if (Colors)
	{
		BeginReleaseResource(&*Colors);
	}
	if (SHCoefficients)
	{
		BeginReleaseResource(&*SHCoefficients);
	}

	ReleaseResourcesFence.BeginFence();
}

void USplatAsset::BeginInit()
{
	check(Positions);
	check(CovariancesCM);
	check(Colors);
	check(SHCoefficients);

	FName Name = FName(GetPathName());

	Positions->SetOwnerName(Name);
	BeginInitResource(&*Positions);
	CovariancesCM->SetOwnerName(Name);
	BeginInitResource(&*CovariancesCM);
	Colors->SetOwnerName(Name);
	BeginInitResource(&*Colors);
	SHCoefficients->SetOwnerName(Name);
	BeginInitResource(&*SHCoefficients);
}

bool USplatAsset::IsReadyForFinishDestroy()
{
	return ReleaseResourcesFence.IsFenceComplete();
}

void USplatAsset::PostLoad()
{
	Super::PostLoad();

	SetPositionsMetersInternal(PositionsFullPrecision);

	// If we are in the Editor, we cannot erase the full-precision positions else
	// we will save empty data in Serialize().
#if !WITH_EDITOR
	if (USplatSettings::IsSortingOnGPU())
	{
		PositionsFullPrecision.Empty();
	}
#endif

	if (!SHCoefficients && NumSplats > 0)
	{
		SetSHCoefficients(TArray<FVector3f>{}, 0);
	}

	BeginInit();
}

void USplatAsset::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);
	Ar.UsingCustomVersion(FSplatAssetCustomVersion::GUID);

	Ar << NumSplats;

	// We have to support the null case for `UObject::DeclareCustomVersions`,
	// which serializes the default (empty) object. If not, our checks in
	// TSplatStaticBuffer<T>::operator<< will trip.
	if (NumSplats > 0)
	{
		Ar << PositionsFullPrecision;
		const int32 Version = Ar.CustomVer(FSplatAssetCustomVersion::GUID);
		Ar << CovariancesCM;
		if (Ar.IsSaving() || Version >= FSplatAssetCustomVersion::AddedFloatColors)
		{
			Ar << Colors;
		}
		else
		{
			TStaticMeshVertexData<FColor> LegacyColorData;
			LegacyColorData.Serialize(Ar);

			TArray<FVector4f> ConvertedColors;
			ConvertedColors.SetNumUninitialized(NumSplats);
			const FColor* LegacyPtr =
				reinterpret_cast<const FColor*>(LegacyColorData.GetDataPointer());
			for (uint32 Index = 0; Index < NumSplats; ++Index)
			{
				const FLinearColor Linear = LegacyPtr[Index].ReinterpretAsLinear();
				ConvertedColors[Index] = FVector4f(
					Linear.R,
					Linear.G,
					Linear.B,
					Linear.A);
			}
			SetColorsLinear(std::move(ConvertedColors));
		}
		if (Ar.IsSaving() ||
			Version >= FSplatAssetCustomVersion::AddedSHCoefficients)
		{
			Ar << NumSHTriplets << SHCoefficients;
		}
		else
		{
			NumSHTriplets = 0;
		}
		Ar << ConvexHullVertices << ConvexHullIndices;
	}
}

#if WITH_EDITOR
void USplatAsset::SetCovariancesQuatScaleMeters(
	const TArray<FQuat4f>& Rotations, const TArray<FVector3f>& ScalesMeters)
{
	check(Rotations.Num() == NumSplats);
	check(ScalesMeters.Num() == NumSplats);

	TStaticMeshVertexData<FPackedCovMat> Data;
	Data.ResizeBuffer(NumSplats);
	for (int32 Index = 0; Index < Data.Num(); ++Index)
	{
		FMatrix44f R = FRotationMatrix44f::Make(Rotations[Index]);
		FMatrix44f S =
			FScaleMatrix44f::Make(MetersToCentimeters * ScalesMeters[Index]);

		// 危 = R * S * S * R^-1.
		// Note: R^-1 = R^T.
		reinterpret_cast<FPackedCovMat*>(Data.GetDataPointer())[Index] =
			FPackedCovMat(R.GetTransposed() * S * S * R);
	}

	CovariancesCM = TSplatStaticBuffer(std::move(Data));
}

void USplatAsset::SetPositionsMeters(TArray<FVector3f>&& PositionsMeters)
{
	// Do not condition this on sorting implementation. This is executed within
	// the editor at import-time, and must be present to be saved to disk and ran
	// with whatever sorting method is in use at runtime.
	PositionsFullPrecision = std::move(PositionsMeters);

	SetPositionsMetersInternal(PositionsFullPrecision);
}

void USplatAsset::SetPositionsMeters(
	TArray<FVector3f>&& PositionsMeters,
	const FVector3f& PosMinMeters,
	const FVector3f& PosMaxMeters)
{
	PositionsFullPrecision = std::move(PositionsMeters);
	SetPositionsMetersInternal(
		PositionsFullPrecision, &PosMinMeters, &PosMaxMeters);
}
#endif

void USplatAsset::SetPositionsMetersInternal(
	const TArray<FVector3f>& PositionsMeters,
	const FVector3f* OverrideMinMeters,
	const FVector3f* OverrideMaxMeters)
{
	check(PositionsMeters.Num() == NumSplats);

	/**
	 * Find minimum and maximum values for X, Y and Z.
	 * This lets us represent splat positions as unsigned, normized integers
	 * describing a position between the min and max. This is more accurate
	 * at low bit size representations than floating-point.
	 */
	FVector3f PosMaxM(std::numeric_limits<float>::lowest());
	FVector3f PosMinM(std::numeric_limits<float>::max());
	for (const FVector3f& PosM : PositionsMeters)
	{
		PosMaxM = PosMaxM.ComponentMax(PosM);
		PosMinM = PosMinM.ComponentMin(PosM);
	}
	if (OverrideMinMeters && OverrideMaxMeters)
	{
		PosMinM = *OverrideMinMeters;
		PosMaxM = *OverrideMaxMeters;
	}
	check(PosMaxM.GetMin() > std::numeric_limits<float>::lowest());
	check(PosMinM.GetMax() < std::numeric_limits<float>::max());

	PosScaleCM = MetersToCentimeters * (PosMaxM - PosMinM) / FPackedPos::MAX;
	PosMaxCM = MetersToCentimeters * PosMaxM;
	PosMinCM = MetersToCentimeters * PosMinM;

	TStaticMeshVertexData<FPackedPos> Data{/*InNeedsCPUAccess=*/false};
	Data.ResizeBuffer(NumSplats);
	for (int32 Index = 0; Index < Data.Num(); ++Index)
	{
		reinterpret_cast<FPackedPos*>(Data.GetDataPointer())[Index] =
			(PositionsMeters[Index] - PosMinM) / (PosMaxM - PosMinM);
	}
	Positions = TSplatStaticBuffer(std::move(Data));
}

void USplat4DAsset::BeginDestroy()
{
	Super::BeginDestroy();

	if (XYZBank) BeginReleaseResource(&*XYZBank);
	if (RotBank) BeginReleaseResource(&*RotBank);
	if (DCBank) BeginReleaseResource(&*DCBank);
	if (LifetimeMuW) BeginReleaseResource(&*LifetimeMuW);
	if (Scales) BeginReleaseResource(&*Scales);

	ReleaseResourcesFence.BeginFence();
}

bool USplat4DAsset::IsReadyForFinishDestroy()
{
	return ReleaseResourcesFence.IsFenceComplete();
}

void USplat4DAsset::PostLoad()
{
	Super::PostLoad();
	BeginInit();
}

void USplat4DAsset::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);

	Ar << TotalFrames;
	Ar << XYZStride << RotStride << DCStride << ScaleStride;
	Ar << NumXYZBanks << NumRotBanks << NumDCBanks;

	if (GetNumSplats() > 0)
	{
		Ar << XYZBank << RotBank << DCBank << LifetimeMuW << Scales;
	}
}

void USplat4DAsset::BeginInit()
{
	FName Name = FName(GetPathName());

	if (XYZBank)
	{
		XYZBank->SetOwnerName(Name);
		BeginInitResource(&*XYZBank);
	}
	if (RotBank)
	{
		RotBank->SetOwnerName(Name);
		BeginInitResource(&*RotBank);
	}
	if (DCBank)
	{
		DCBank->SetOwnerName(Name);
		BeginInitResource(&*DCBank);
	}
	if (LifetimeMuW)
	{
		LifetimeMuW->SetOwnerName(Name);
		BeginInitResource(&*LifetimeMuW);
	}
	if (Scales)
	{
		Scales->SetOwnerName(Name);
		BeginInitResource(&*Scales);
	}
}

