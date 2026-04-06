/*
  Copyright (c) 2025 Easytime Technology Co., Ltd. See LICENSE.md.
*/

#include "SplatAsset.h"
#include "SplatConstants.h"
#include "SplatSettings.h"

#include "CompGeom/ConvexHull3.h"
#include "Dom/JsonObject.h"
#include "FileUtilities/ZipArchiveReader.h"
#include "HAL/PlatformFileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "RHIResources.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/CustomVersion.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <wincodec.h>
#include <wrl/client.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

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
		AddedCompressedSOG4Storage,
		LatestVersion = AddedCompressedSOG4Storage
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

struct FSOGImageRGBA8
{
	TArray<uint8> Bytes;
	int32 Width = 0;
	int32 Height = 0;

	bool HasPixelsForCount(int32 Count) const
	{
		return Width > 0 && Height > 0 && Bytes.Num() >= Count * 4;
	}
};

bool TryGetJsonNumber(const TSharedPtr<FJsonValue>& Value, double& OutValue)
{
	if (!Value.IsValid())
	{
		return false;
	}
	if (Value->Type == EJson::Number)
	{
		OutValue = Value->AsNumber();
		return true;
	}
	if (Value->Type == EJson::String)
	{
		return LexTryParseString(OutValue, *Value->AsString());
	}
	return false;
}

int32 GetJsonIntFieldFlexible(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* FieldName,
	int32 DefaultValue)
{
	if (!Object.IsValid())
	{
		return DefaultValue;
	}

	const TSharedPtr<FJsonValue> Value = Object->TryGetField(FieldName);
	if (!Value.IsValid())
	{
		return DefaultValue;
	}

	double Number = 0.0;
	if (!TryGetJsonNumber(Value, Number))
	{
		return DefaultValue;
	}
	return static_cast<int32>(FMath::RoundToInt(Number));
}

TArray<float> GetJsonFloatArrayField(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* FieldName)
{
	TArray<float> Result;
	if (!Object.IsValid())
	{
		return Result;
	}

	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object->TryGetArrayField(FieldName, Values) || !Values)
	{
		return Result;
	}

	Result.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		double Number = 0.0;
		if (!TryGetJsonNumber(Value, Number))
		{
			return TArray<float>();
		}
		Result.Add(static_cast<float>(Number));
	}
	return Result;
}

bool TryGetJsonVec3Field(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* FieldName,
	FVector3f& OutValue)
{
	TArray<float> Values = GetJsonFloatArrayField(Object, FieldName);
	if (Values.Num() < 3)
	{
		return false;
	}

	OutValue = FVector3f(Values[0], Values[1], Values[2]);
	return true;
}

bool TryGetFilesField(
	const TSharedPtr<FJsonObject>& Object,
	TArray<FString>& OutFiles)
{
	OutFiles.Reset();
	if (!Object.IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object->TryGetArrayField(TEXT("files"), Values) || !Values)
	{
		return false;
	}

	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		if (!Value.IsValid() || Value->Type != EJson::String)
		{
			return false;
		}
		OutFiles.Add(Value->AsString());
	}
	return OutFiles.Num() > 0;
}

bool TryReadZipEntry(
	const FZipArchiveReader& ZipReader,
	const FString& FileName,
	TArray<uint8>& OutBytes)
{
	OutBytes.Reset();
	if (FileName.IsEmpty())
	{
		return false;
	}
	return ZipReader.TryReadFile(FileName, OutBytes, GWarn);
}

bool DecodeImageRGBA8(
	const TArray<uint8>& CompressedBytes,
	FSOGImageRGBA8& OutImage,
	const TCHAR* DebugName)
{
	OutImage = FSOGImageRGBA8{};
	if (CompressedBytes.Num() <= 0)
	{
		return false;
	}

#if PLATFORM_WINDOWS
	auto DecodeWithWIC = [&CompressedBytes, &OutImage]() -> bool
	{
		if (!ensure(static_cast<uint64>(CompressedBytes.Num()) <= static_cast<uint64>(MAX_uint32)))
		{
			return false;
		}

		HRESULT CoInitHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		const bool bShouldCoUninit = SUCCEEDED(CoInitHr);
		if (FAILED(CoInitHr) && CoInitHr != RPC_E_CHANGED_MODE)
		{
			return false;
		}
		ON_SCOPE_EXIT
		{
			if (bShouldCoUninit)
			{
				CoUninitialize();
			}
		};

		Microsoft::WRL::ComPtr<IWICImagingFactory> ImagingFactory;
		HRESULT Hr = CoCreateInstance(
			CLSID_WICImagingFactory2,
			nullptr,
			CLSCTX_INPROC_SERVER,
			IID_PPV_ARGS(&ImagingFactory));
		if (FAILED(Hr))
		{
			Hr = CoCreateInstance(
				CLSID_WICImagingFactory,
				nullptr,
				CLSCTX_INPROC_SERVER,
				IID_PPV_ARGS(&ImagingFactory));
		}
		if (FAILED(Hr) || !ImagingFactory)
		{
			return false;
		}

		Microsoft::WRL::ComPtr<IWICStream> Stream;
		Hr = ImagingFactory->CreateStream(&Stream);
		if (FAILED(Hr) || !Stream)
		{
			return false;
		}

		Hr = Stream->InitializeFromMemory(
			const_cast<BYTE*>(CompressedBytes.GetData()),
			static_cast<DWORD>(CompressedBytes.Num()));
		if (FAILED(Hr))
		{
			return false;
		}

		Microsoft::WRL::ComPtr<IWICBitmapDecoder> Decoder;
		Hr = ImagingFactory->CreateDecoderFromStream(
			Stream.Get(),
			nullptr,
			WICDecodeMetadataCacheOnLoad,
			&Decoder);
		if (FAILED(Hr) || !Decoder)
		{
			return false;
		}

		Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> Frame;
		Hr = Decoder->GetFrame(0, &Frame);
		if (FAILED(Hr) || !Frame)
		{
			return false;
		}

		UINT Width = 0;
		UINT Height = 0;
		Hr = Frame->GetSize(&Width, &Height);
		if (FAILED(Hr) || Width == 0 || Height == 0)
		{
			return false;
		}

		Microsoft::WRL::ComPtr<IWICFormatConverter> Converter;
		Hr = ImagingFactory->CreateFormatConverter(&Converter);
		if (FAILED(Hr) || !Converter)
		{
			return false;
		}

		Hr = Converter->Initialize(
			Frame.Get(),
			GUID_WICPixelFormat32bppRGBA,
			WICBitmapDitherTypeNone,
			nullptr,
			0.0,
			WICBitmapPaletteTypeCustom);
		if (FAILED(Hr))
		{
			return false;
		}

		const uint64 Stride64 = static_cast<uint64>(Width) * 4ull;
		const uint64 NumBytes64 = Stride64 * static_cast<uint64>(Height);
		if (!ensure(NumBytes64 <= TNumericLimits<int32>::Max()))
		{
			return false;
		}

		const int32 NumBytes = static_cast<int32>(NumBytes64);
		OutImage.Width = static_cast<int32>(Width);
		OutImage.Height = static_cast<int32>(Height);
		OutImage.Bytes.SetNumUninitialized(NumBytes);

		Hr = Converter->CopyPixels(
			nullptr,
			static_cast<UINT>(Stride64),
			static_cast<UINT>(NumBytes),
			OutImage.Bytes.GetData());
		if (FAILED(Hr))
		{
			OutImage = FSOGImageRGBA8{};
			return false;
		}

		return true;
	};
#endif

	IImageWrapperModule& ImageWrapperModule =
		FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	EImageFormat ImageFormat =
		ImageWrapperModule.DetectImageFormat(CompressedBytes.GetData(), CompressedBytes.Num());

	if (ImageFormat == EImageFormat::Invalid)
	{
		#if PLATFORM_WINDOWS
		return DecodeWithWIC();
		#else
		return false;
		#endif
	}

	TSharedPtr<IImageWrapper> ImageWrapper =
		ImageWrapperModule.CreateImageWrapper(ImageFormat, DebugName);
	if (!ImageWrapper.IsValid() ||
		!ImageWrapper->SetCompressed(CompressedBytes.GetData(), CompressedBytes.Num()))
	{
		#if PLATFORM_WINDOWS
		return DecodeWithWIC();
		#else
		return false;
		#endif
	}

	TArray64<uint8> RawRGBA64;
	if (!ImageWrapper->GetRaw(ERGBFormat::RGBA, 8, RawRGBA64))
	{
		#if PLATFORM_WINDOWS
		return DecodeWithWIC();
		#else
		return false;
		#endif
	}

	if (RawRGBA64.Num() > TNumericLimits<int32>::Max())
	{
		return false;
	}

	OutImage.Width = static_cast<int32>(ImageWrapper->GetWidth());
	OutImage.Height = static_cast<int32>(ImageWrapper->GetHeight());
	OutImage.Bytes.SetNumUninitialized(static_cast<int32>(RawRGBA64.Num()));
	FMemory::Memcpy(OutImage.Bytes.GetData(), RawRGBA64.GetData(), OutImage.Bytes.Num());
	return true;
}

bool TryLoadZipImageRGBA8(
	const FZipArchiveReader& ZipReader,
	const FString& FileName,
	FSOGImageRGBA8& OutImage)
{
	TArray<uint8> CompressedBytes;
	if (!TryReadZipEntry(ZipReader, FileName, CompressedBytes))
	{
		return false;
	}
	return DecodeImageRGBA8(CompressedBytes, OutImage, *FileName);
}

float InverseLogTransform(float Value)
{
	return FMath::Sign(Value) * (FMath::Exp(FMath::Abs(Value)) - 1.0f);
}

float DecodeLogitFromUNorm8(uint8 Value)
{
	const float P = FMath::Clamp(static_cast<float>(Value) / 255.0f, 1e-6f, 1.0f - 1e-6f);
	return FMath::Loge(P / (1.0f - P));
}

float ToColorLinearFloat(float DC)
{
	const float ColorSRGB = 0.5f + 0.2820948f * DC;
	return FMath::Clamp(FMath::Pow(FMath::Max(ColorSRGB, 0.0f), 2.2f), 0.0f, 1.0f);
}

float ToAlphaLinearFloat(float Logit)
{
	return FMath::Clamp(1.0f / (1.0f + FMath::Exp(-Logit)), 0.0f, 1.0f);
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

float GetCodebookValueOrDefault(
	const TArray<float>& Codebook,
	int32 Index,
	float DefaultValue = 0.0f)
{
	return Codebook.IsValidIndex(Index) ? Codebook[Index] : DefaultValue;
}

FQuat4f DecodePackedQuatToUE(uint8 R, uint8 G, uint8 B, uint8 A)
{
	float QWXYZ[4] = {1.0f, 0.0f, 0.0f, 0.0f};
	const int32 LargestIdx = static_cast<int32>(A) - 252;
	if (LargestIdx >= 0 && LargestIdx <= 3)
	{
		const float Sqrt2 = 1.4142135623730951f;
		float QVals[3] = {
			((static_cast<float>(R) / 255.0f) * 2.0f - 1.0f) / Sqrt2,
			((static_cast<float>(G) / 255.0f) * 2.0f - 1.0f) / Sqrt2,
			((static_cast<float>(B) / 255.0f) * 2.0f - 1.0f) / Sqrt2};

		int32 ReadIdx = 0;
		float SumSq = 0.0f;
		for (int32 C = 0; C < 4; ++C)
		{
			if (C == LargestIdx)
			{
				continue;
			}
			QWXYZ[C] = QVals[ReadIdx++];
			SumSq += QWXYZ[C] * QWXYZ[C];
		}
		QWXYZ[LargestIdx] = FMath::Sqrt(FMath::Max(0.0f, 1.0f - SumSq));
	}

	const FQuat4f SourceXYZW(QWXYZ[1], QWXYZ[2], QWXYZ[3], QWXYZ[0]);
	const FQuat4f UEQuat(-SourceXYZW.Z, -SourceXYZW.X, SourceXYZW.Y, SourceXYZW.W);
	return UEQuat.GetNormalized();
}
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

	const bool bHasExpandedCoreData =
		NumSplats > 0 &&
		PositionsFullPrecision.Num() == static_cast<int32>(NumSplats) &&
		CovariancesCM.has_value() &&
		Colors.has_value();
	if (bHasExpandedCoreData)
	{
		SetPositionsMetersInternal(PositionsFullPrecision);
	}

	// If we are in the Editor, we cannot erase the full-precision positions else
	// we will save empty data in Serialize().
#if !WITH_EDITOR
	if (bHasExpandedCoreData && USplatSettings::IsSortingOnGPU())
	{
		PositionsFullPrecision.Empty();
	}
#endif

	if (bHasExpandedCoreData && !SHCoefficients && NumSplats > 0)
	{
		SetSHCoefficients(TArray<FVector3f>{}, 0);
	}

	if (bHasExpandedCoreData)
	{
		BeginInit();
	}
}

void USplatAsset::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);
	Ar.UsingCustomVersion(FSplatAssetCustomVersion::GUID);

	Ar << NumSplats;

	// We have to support the null case for `UObject::DeclareCustomVersions`,
	// which serializes the default (empty) object. If not, our checks in
	// TSplatStaticBuffer<T>::operator<< will trip.
	if (NumSplats > 0 && (Ar.IsLoading() || ShouldSerializeExpandedPayload()))
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

	FVector3f RangeM = PosMaxM - PosMinM;
	const float MinRange = 1e-6f;
	RangeM.X = FMath::Max(RangeM.X, MinRange);
	RangeM.Y = FMath::Max(RangeM.Y, MinRange);
	RangeM.Z = FMath::Max(RangeM.Z, MinRange);

	PosScaleCM = MetersToCentimeters * RangeM / FPackedPos::MAX;
	PosMaxCM = MetersToCentimeters * PosMaxM;
	PosMinCM = MetersToCentimeters * PosMinM;

	TStaticMeshVertexData<FPackedPos> Data{/*InNeedsCPUAccess=*/false};
	Data.ResizeBuffer(NumSplats);
	for (int32 Index = 0; Index < Data.Num(); ++Index)
	{
		reinterpret_cast<FPackedPos*>(Data.GetDataPointer())[Index] =
			(PositionsMeters[Index] - PosMinM) / RangeM;
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

bool USplat4DAsset::EnsureExpandedRuntimeData()
{
	if (GetNumSplats() > 0 && XYZBank && RotBank && DCBank && LifetimeMuW && Scales &&
		GetPositions().Num() == static_cast<int32>(GetNumSplats()))
	{
		return true;
	}

	if (bStoreAsCompressedSOG4 && CompressedSOG4.Num() > 0)
	{
		if (!DecodeCompressedSOG4ToExpanded())
		{
			return false;
		}
		USplatAsset::BeginInit();
		BeginInit();
		return true;
	}

	return false;
}

bool USplat4DAsset::DecodeCompressedSOG4ToExpanded()
{
	if (!bStoreAsCompressedSOG4 || CompressedSOG4.Num() <= 0)
	{
		return false;
	}

	const FString TempZipPath = FPaths::CreateTempFilename(
		*FPaths::ProjectIntermediateDir(),
		TEXT("SOG4_RT_"),
		TEXT(".sog4"));
	ON_SCOPE_EXIT
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		PlatformFile.DeleteFile(*TempZipPath);
	};

	if (!FFileHelper::SaveArrayToFile(CompressedSOG4, *TempZipPath))
	{
		return false;
	}

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	TUniquePtr<IFileHandle> ZipFileHandle(PlatformFile.OpenRead(*TempZipPath));
	if (!ZipFileHandle.IsValid())
	{
		return false;
	}

	FZipArchiveReader ZipReader(ZipFileHandle.Release(), GWarn);
	if (!ZipReader.IsValid())
	{
		return false;
	}

	TArray<uint8> MetaBytes;
	if (!TryReadZipEntry(ZipReader, TEXT("meta.json"), MetaBytes))
	{
		return false;
	}

	FString MetaJsonString;
	FFileHelper::BufferToString(MetaJsonString, MetaBytes.GetData(), MetaBytes.Num());
	TSharedPtr<FJsonObject> MetaObject;
	const TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(MetaJsonString);
	if (!FJsonSerializer::Deserialize(JsonReader, MetaObject) || !MetaObject.IsValid())
	{
		return false;
	}

	const int32 Count = GetJsonIntFieldFlexible(MetaObject, TEXT("count"), 0);
	if (Count <= 0)
	{
		return false;
	}
	SetNumSplats(static_cast<uint32>(Count));
	CompressedSOG4NumSplats = Count;

	const TSharedPtr<FJsonObject>* CustomObjectPtr = nullptr;
	const TSharedPtr<FJsonObject> CustomObject =
		MetaObject->TryGetObjectField(TEXT("custom"), CustomObjectPtr) && CustomObjectPtr
			? *CustomObjectPtr
			: nullptr;

	TArray<FVector3f> PositionsSrc;
	PositionsSrc.Init(FVector3f::ZeroVector, Count);
	TArray<FQuat4f> RotationsUE;
	RotationsUE.Init(FQuat4f::Identity, Count);
	TArray<FVector3f> ScalesUE;
	ScalesUE.Init(FVector3f(1.0f, 1.0f, 1.0f), Count);
	TArray<float> OpacityLogit;
	OpacityLogit.Init(20.0f, Count);
	TArray<FVector4f> ColorsLinear;
	ColorsLinear.Init(FVector4f(ToColorLinearFloat(0.0f), ToColorLinearFloat(0.0f), ToColorLinearFloat(0.0f), 1.0f), Count);

	const TSharedPtr<FJsonObject>* MeansObjPtr = nullptr;
	bool bDecodedStaticMeans = false;
	if (MetaObject->TryGetObjectField(TEXT("means"), MeansObjPtr) && MeansObjPtr)
	{
		TArray<FString> Files;
		FVector3f Mins(0.0f), Maxs(0.0f);
		if (TryGetFilesField(*MeansObjPtr, Files) && Files.Num() >= 2 &&
			TryGetJsonVec3Field(*MeansObjPtr, TEXT("mins"), Mins) &&
			TryGetJsonVec3Field(*MeansObjPtr, TEXT("maxs"), Maxs))
		{
			FSOGImageRGBA8 MeansL;
			FSOGImageRGBA8 MeansU;
			if (TryLoadZipImageRGBA8(ZipReader, Files[0], MeansL) &&
				TryLoadZipImageRGBA8(ZipReader, Files[1], MeansU) &&
				MeansL.HasPixelsForCount(Count) &&
				MeansU.HasPixelsForCount(Count))
			{
				bDecodedStaticMeans = true;
				for (int32 Index = 0; Index < Count; ++Index)
				{
					const int32 Base = Index * 4;
					const uint32 NX = (static_cast<uint32>(MeansU.Bytes[Base + 0]) << 8) | MeansL.Bytes[Base + 0];
					const uint32 NY = (static_cast<uint32>(MeansU.Bytes[Base + 1]) << 8) | MeansL.Bytes[Base + 1];
					const uint32 NZ = (static_cast<uint32>(MeansU.Bytes[Base + 2]) << 8) | MeansL.Bytes[Base + 2];
					PositionsSrc[Index] = FVector3f(
						InverseLogTransform((static_cast<float>(NX) / 65535.0f) * (Maxs.X - Mins.X) + Mins.X),
						InverseLogTransform((static_cast<float>(NY) / 65535.0f) * (Maxs.Y - Mins.Y) + Mins.Y),
						InverseLogTransform((static_cast<float>(NZ) / 65535.0f) * (Maxs.Z - Mins.Z) + Mins.Z));
				}
			}
		}
	}
	if (!bDecodedStaticMeans)
	{
		UE_LOG(LogTemp, Error, TEXT("[SOG4 Runtime Decode] Failed to decode static means; abort expand for %s"), *GetPathName());
		return false;
	}

	const TSharedPtr<FJsonObject>* QuatsObjPtr = nullptr;
	if (MetaObject->TryGetObjectField(TEXT("quats"), QuatsObjPtr) && QuatsObjPtr)
	{
		TArray<FString> Files;
		if (TryGetFilesField(*QuatsObjPtr, Files) && Files.Num() >= 1)
		{
			FSOGImageRGBA8 RotationImage;
			if (TryLoadZipImageRGBA8(ZipReader, Files[0], RotationImage) && RotationImage.HasPixelsForCount(Count))
			{
				for (int32 Index = 0; Index < Count; ++Index)
				{
					const int32 Base = Index * 4;
					RotationsUE[Index] = DecodePackedQuatToUE(
						RotationImage.Bytes[Base + 0],
						RotationImage.Bytes[Base + 1],
						RotationImage.Bytes[Base + 2],
						RotationImage.Bytes[Base + 3]);
				}
			}
		}
	}

	const TSharedPtr<FJsonObject>* OpacityObjPtr = nullptr;
	if (MetaObject->TryGetObjectField(TEXT("opacity"), OpacityObjPtr) && OpacityObjPtr)
	{
		TArray<FString> Files;
		if (TryGetFilesField(*OpacityObjPtr, Files) && Files.Num() >= 1)
		{
			FSOGImageRGBA8 OpacityImage;
			if (TryLoadZipImageRGBA8(ZipReader, Files[0], OpacityImage) && OpacityImage.HasPixelsForCount(Count))
			{
				for (int32 Index = 0; Index < Count; ++Index)
				{
					OpacityLogit[Index] = DecodeLogitFromUNorm8(OpacityImage.Bytes[Index * 4 + 3]);
				}
			}
		}
	}

	const TSharedPtr<FJsonObject>* ScalesObjPtr = nullptr;
	if (MetaObject->TryGetObjectField(TEXT("scales"), ScalesObjPtr) && ScalesObjPtr)
	{
		const TArray<float> ScaleCodebook = GetJsonFloatArrayField(*ScalesObjPtr, TEXT("codebook"));
		TArray<FString> Files;
		if (ScaleCodebook.Num() > 0 && TryGetFilesField(*ScalesObjPtr, Files) && Files.Num() >= 1)
		{
			FSOGImageRGBA8 ScalesImage;
			if (TryLoadZipImageRGBA8(ZipReader, Files[0], ScalesImage) && ScalesImage.HasPixelsForCount(Count))
			{
				for (int32 Index = 0; Index < Count; ++Index)
				{
					const int32 Base = Index * 4;
					const float ScaleXLog = GetCodebookValueOrDefault(ScaleCodebook, ScalesImage.Bytes[Base + 0]);
					const float ScaleYLog = GetCodebookValueOrDefault(ScaleCodebook, ScalesImage.Bytes[Base + 1]);
					const float ScaleZLog = GetCodebookValueOrDefault(ScaleCodebook, ScalesImage.Bytes[Base + 2]);
					ScalesUE[Index] = FVector3f(FMath::Exp(ScaleZLog), FMath::Exp(ScaleXLog), FMath::Exp(ScaleYLog));
				}
			}
		}
	}

	int32 LocalNumSHTriplets = 0;
	TArray<FVector3f> LocalSHCoefficients;
	bool bDecodedSH0 = false;
	const TSharedPtr<FJsonObject>* SH0ObjPtr = nullptr;
	if (MetaObject->TryGetObjectField(TEXT("sh0"), SH0ObjPtr) && SH0ObjPtr)
	{
		const TArray<float> SH0Codebook = GetJsonFloatArrayField(*SH0ObjPtr, TEXT("codebook"));
		TArray<FString> Files;
		if (SH0Codebook.Num() > 0 && TryGetFilesField(*SH0ObjPtr, Files) && Files.Num() >= 1)
		{
			FSOGImageRGBA8 SH0Image;
			if (TryLoadZipImageRGBA8(ZipReader, Files[0], SH0Image) && SH0Image.HasPixelsForCount(Count))
			{
				bDecodedSH0 = true;
				for (int32 Index = 0; Index < Count; ++Index)
				{
					const int32 Base = Index * 4;
					const float DCR = GetCodebookValueOrDefault(SH0Codebook, SH0Image.Bytes[Base + 0]);
					const float DCG = GetCodebookValueOrDefault(SH0Codebook, SH0Image.Bytes[Base + 1]);
					const float DCB = GetCodebookValueOrDefault(SH0Codebook, SH0Image.Bytes[Base + 2]);
					ColorsLinear[Index] = FVector4f(
						ToColorLinearFloat(DCR),
						ToColorLinearFloat(DCG),
						ToColorLinearFloat(DCB),
						ToAlphaLinearFloat(DecodeLogitFromUNorm8(SH0Image.Bytes[Base + 3])));
				}
			}
		}
	}
	if (!bDecodedSH0)
	{
		for (int32 Index = 0; Index < Count; ++Index)
		{
			ColorsLinear[Index].W = ToAlphaLinearFloat(OpacityLogit[Index]);
		}
	}

	const TSharedPtr<FJsonObject>* SHNObjPtr = nullptr;
	if (MetaObject->TryGetObjectField(TEXT("shN"), SHNObjPtr) && SHNObjPtr)
	{
		const int32 Bands = GetJsonIntFieldFlexible(*SHNObjPtr, TEXT("bands"), 3);
		const int32 NumCoeffs = (Bands == 1) ? 9 : ((Bands == 2) ? 24 : 45);
		LocalNumSHTriplets = NumCoeffs / 3;
		LocalSHCoefficients.Init(FVector3f::ZeroVector, Count * LocalNumSHTriplets);
		const TArray<float> SHNCodebook = GetJsonFloatArrayField(*SHNObjPtr, TEXT("codebook"));
		const int32 PaletteSize = GetJsonIntFieldFlexible(*SHNObjPtr, TEXT("count"), 0);
		TArray<FString> Files;
		if (SHNCodebook.Num() > 0 && PaletteSize > 0 && TryGetFilesField(*SHNObjPtr, Files) && Files.Num() >= 2)
		{
			FSOGImageRGBA8 CentersImage;
			FSOGImageRGBA8 LabelsImage;
			if (TryLoadZipImageRGBA8(ZipReader, Files[0], CentersImage) &&
				TryLoadZipImageRGBA8(ZipReader, Files[1], LabelsImage) &&
				LabelsImage.HasPixelsForCount(Count))
			{
				const int32 CoeffsPerChannel = NumCoeffs / 3;
				TArray<float> Palette;
				Palette.Init(0.0f, PaletteSize * NumCoeffs);
				for (int32 PaletteIndex = 0; PaletteIndex < PaletteSize; ++PaletteIndex)
				{
					const int32 Row = PaletteIndex / 64;
					const int32 ColBase = (PaletteIndex % 64) * CoeffsPerChannel;
					for (int32 C = 0; C < CoeffsPerChannel; ++C)
					{
						const int32 PixelIndex = Row * CentersImage.Width + ColBase + C;
						const int32 ByteIndex = PixelIndex * 4;
						if (ByteIndex + 2 >= CentersImage.Bytes.Num())
						{
							continue;
						}
						Palette[PaletteIndex * NumCoeffs + CoeffsPerChannel * 0 + C] = GetCodebookValueOrDefault(SHNCodebook, CentersImage.Bytes[ByteIndex + 0]);
						Palette[PaletteIndex * NumCoeffs + CoeffsPerChannel * 1 + C] = GetCodebookValueOrDefault(SHNCodebook, CentersImage.Bytes[ByteIndex + 1]);
						Palette[PaletteIndex * NumCoeffs + CoeffsPerChannel * 2 + C] = GetCodebookValueOrDefault(SHNCodebook, CentersImage.Bytes[ByteIndex + 2]);
					}
				}
				for (int32 Index = 0; Index < Count; ++Index)
				{
					const int32 LabelBase = Index * 4;
					const int32 Label =
						static_cast<int32>(LabelsImage.Bytes[LabelBase + 0]) |
						(static_cast<int32>(LabelsImage.Bytes[LabelBase + 1]) << 8);
					if (Label < 0 || Label >= PaletteSize)
					{
						continue;
					}
					const int32 PaletteBase = Label * NumCoeffs;
					for (int32 T = 0; T < LocalNumSHTriplets; ++T)
					{
						LocalSHCoefficients[Index * LocalNumSHTriplets + T] = FVector3f(
							Palette[PaletteBase + T],
							Palette[PaletteBase + LocalNumSHTriplets + T],
							Palette[PaletteBase + LocalNumSHTriplets * 2 + T]);
					}
				}
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* XYZBankArray = nullptr;
	int32 LocalNumXYZBanks = 0;
	TArray<FVector3f> XYZBankData;
	int32 LocalXYZStride = 1;
	if (MetaObject->TryGetArrayField(TEXT("xyz_bank"), XYZBankArray) && XYZBankArray && XYZBankArray->Num() > 0)
	{
		LocalNumXYZBanks = XYZBankArray->Num();
		LocalXYZStride = GetJsonIntFieldFlexible(CustomObject, TEXT("xyz_bank_keyframe_stride"),
			GetJsonIntFieldFlexible(MetaObject, TEXT("xyz_bank_stride"), 1));
		LocalXYZStride = FMath::Max(1, LocalXYZStride);
		XYZBankData.SetNumUninitialized(Count * LocalNumXYZBanks);
		for (int32 BankIndex = 0; BankIndex < LocalNumXYZBanks; ++BankIndex)
		{
			const TSharedPtr<FJsonObject> BankObject = (*XYZBankArray)[BankIndex]->AsObject();
			TArray<FString> Files;
			FVector3f Mins(0.0f), Maxs(0.0f);
			const bool bCanDecode = BankObject.IsValid() && TryGetFilesField(BankObject, Files) && Files.Num() >= 2 &&
				TryGetJsonVec3Field(BankObject, TEXT("mins"), Mins) && TryGetJsonVec3Field(BankObject, TEXT("maxs"), Maxs);
			FSOGImageRGBA8 MeansL;
			FSOGImageRGBA8 MeansU;
			const bool bHasImages = bCanDecode &&
				TryLoadZipImageRGBA8(ZipReader, Files[0], MeansL) &&
				TryLoadZipImageRGBA8(ZipReader, Files[1], MeansU) &&
				MeansL.HasPixelsForCount(Count) &&
				MeansU.HasPixelsForCount(Count);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				if (bHasImages)
				{
					const int32 Base = Index * 4;
					const uint32 NX = (static_cast<uint32>(MeansU.Bytes[Base + 0]) << 8) | MeansL.Bytes[Base + 0];
					const uint32 NY = (static_cast<uint32>(MeansU.Bytes[Base + 1]) << 8) | MeansL.Bytes[Base + 1];
					const uint32 NZ = (static_cast<uint32>(MeansU.Bytes[Base + 2]) << 8) | MeansL.Bytes[Base + 2];
					XYZBankData[Index * LocalNumXYZBanks + BankIndex] = FVector3f(
						InverseLogTransform((static_cast<float>(NX) / 65535.0f) * (Maxs.X - Mins.X) + Mins.X),
						InverseLogTransform((static_cast<float>(NY) / 65535.0f) * (Maxs.Y - Mins.Y) + Mins.Y),
						InverseLogTransform((static_cast<float>(NZ) / 65535.0f) * (Maxs.Z - Mins.Z) + Mins.Z));
				}
				else
				{
					XYZBankData[Index * LocalNumXYZBanks + BankIndex] = PositionsSrc[Index];
				}
			}
		}
	}
	else
	{
		LocalNumXYZBanks = 1;
		LocalXYZStride = 1;
		XYZBankData = PositionsSrc;
	}

	const TArray<TSharedPtr<FJsonValue>>* RotBankArray = nullptr;
	int32 LocalNumRotBanks = 0;
	TArray<FQuat4f> RotBankData;
	int32 LocalRotStride = 1;
	if (MetaObject->TryGetArrayField(TEXT("rot_bank"), RotBankArray) && RotBankArray && RotBankArray->Num() > 0)
	{
		LocalNumRotBanks = RotBankArray->Num();
		LocalRotStride = GetJsonIntFieldFlexible(CustomObject, TEXT("rot_bank_keyframe_stride"),
			GetJsonIntFieldFlexible(MetaObject, TEXT("rot_bank_stride"), LocalXYZStride));
		LocalRotStride = FMath::Max(1, LocalRotStride);
		RotBankData.SetNumUninitialized(Count * LocalNumRotBanks);
		for (int32 BankIndex = 0; BankIndex < LocalNumRotBanks; ++BankIndex)
		{
			const TSharedPtr<FJsonObject> BankObject = (*RotBankArray)[BankIndex]->AsObject();
			TArray<FString> Files;
			FSOGImageRGBA8 RotationImage;
			const bool bHasImage = BankObject.IsValid() && TryGetFilesField(BankObject, Files) && Files.Num() >= 1 &&
				TryLoadZipImageRGBA8(ZipReader, Files[0], RotationImage) && RotationImage.HasPixelsForCount(Count);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				if (bHasImage)
				{
					const int32 Base = Index * 4;
					RotBankData[Index * LocalNumRotBanks + BankIndex] = DecodePackedQuatToUE(
						RotationImage.Bytes[Base + 0], RotationImage.Bytes[Base + 1], RotationImage.Bytes[Base + 2], RotationImage.Bytes[Base + 3]);
				}
				else
				{
					RotBankData[Index * LocalNumRotBanks + BankIndex] = RotationsUE[Index];
				}
			}
		}
	}
	else
	{
		LocalNumRotBanks = 1;
		LocalRotStride = 1;
		RotBankData = RotationsUE;
	}

	const int32 TotalFramesFromCustom = GetJsonIntFieldFlexible(CustomObject, TEXT("total_frames"), 0);
	int32 LocalTotalFrames = (TotalFramesFromCustom > 0) ? TotalFramesFromCustom :
		GetJsonIntFieldFlexible(MetaObject, TEXT("total_frames"), 0);
	if (LocalTotalFrames <= 0)
	{
		LocalTotalFrames = (LocalNumXYZBanks > 1) ? ((LocalNumXYZBanks - 1) * LocalXYZStride + 1) : 1;
	}
	LocalTotalFrames = FMath::Max(1, LocalTotalFrames);

	TArray<FVector2f> LifetimeData;
	const float DefaultMu = 0.5f * static_cast<float>(LocalTotalFrames);
	const float DefaultW = FMath::Max(1.0f, static_cast<float>(LocalTotalFrames));
	LifetimeData.Init(FVector2f(DefaultMu, DefaultW), Count);

	const TSharedPtr<FJsonObject>* ParamsObjPtr = nullptr;
	if (MetaObject->TryGetObjectField(TEXT("params"), ParamsObjPtr) && ParamsObjPtr)
	{
		TArray<FString> Files;
		if (TryGetFilesField(*ParamsObjPtr, Files) && Files.Num() >= 1)
		{
			TArray<float> MuCodebook = GetJsonFloatArrayField(*ParamsObjPtr, TEXT("codebook_mu"));
			if (MuCodebook.Num() == 0) MuCodebook = GetJsonFloatArrayField(*ParamsObjPtr, TEXT("codebook_mu_list"));
			if (MuCodebook.Num() == 0) MuCodebook = GetJsonFloatArrayField(*ParamsObjPtr, TEXT("codebook"));

			TArray<float> WCodebook = GetJsonFloatArrayField(*ParamsObjPtr, TEXT("codebook_w"));
			if (WCodebook.Num() == 0) WCodebook = GetJsonFloatArrayField(*ParamsObjPtr, TEXT("codebook_w_list"));
			if (WCodebook.Num() == 0) WCodebook = GetJsonFloatArrayField(*ParamsObjPtr, TEXT("codebook"));

			FSOGImageRGBA8 ParamsImage;
			if (MuCodebook.Num() > 0 && WCodebook.Num() > 0 &&
				TryLoadZipImageRGBA8(ZipReader, Files[0], ParamsImage) && ParamsImage.HasPixelsForCount(Count))
			{
				for (int32 Index = 0; Index < Count; ++Index)
				{
					const int32 Base = Index * 4;
					const int32 MuIndex = ParamsImage.Bytes[Base + 0];
					const int32 WIndex = ParamsImage.Bytes[Base + 1];
					const float Mu = GetCodebookValueOrDefault(MuCodebook, MuIndex, DefaultMu);
					const float W = GetCodebookValueOrDefault(WCodebook, WIndex, DefaultW);
					LifetimeData[Index] = FVector2f(Mu, FMath::Max(W, 1e-3f));
				}
			}
		}
	}

	TotalFrames = LocalTotalFrames;
	XYZStride = LocalXYZStride;
	RotStride = LocalRotStride;
	DCStride = 1;
	NumXYZBanks = LocalNumXYZBanks;
	NumRotBanks = LocalNumRotBanks;
	NumDCBanks = 0;

	TArray<FVector3f> PositionsUE;
	PositionsUE.SetNumUninitialized(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FVector3f& Src = XYZBankData[Index * LocalNumXYZBanks];
		PositionsUE[Index] = FVector3f(Src.Z, Src.X, -Src.Y);
	}

	TArray<FQuat4f> RotationsFromBanks;
	RotationsFromBanks.SetNumUninitialized(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		RotationsFromBanks[Index] = RotBankData[Index * LocalNumRotBanks].GetNormalized();
	}

	FVector3f QuantMin = PositionsUE.Num() > 0 ? PositionsUE[0] : FVector3f::ZeroVector;
	FVector3f QuantMax = QuantMin;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		for (int32 BankIndex = 0; BankIndex < LocalNumXYZBanks; ++BankIndex)
		{
			const FVector3f& Src = XYZBankData[Index * LocalNumXYZBanks + BankIndex];
			const FVector3f Converted(Src.Z, Src.X, -Src.Y);
			QuantMin = QuantMin.ComponentMin(Converted);
			QuantMax = QuantMax.ComponentMax(Converted);
		}
	}

	TStaticMeshVertexData<FVector3f> XYZData;
	XYZData.Assign(XYZBankData);
	XYZBank = Easytime::Splat::TSplatStaticBuffer(std::move(XYZData));

	TStaticMeshVertexData<FQuat4f> RotData;
	RotData.Assign(RotBankData);
	RotBank = Easytime::Splat::TSplatStaticBuffer(std::move(RotData));

	TArray<FVector3f> DummyDCBank;
	DummyDCBank.Add(FVector3f::ZeroVector);
	TStaticMeshVertexData<FVector3f> DCData;
	DCData.Assign(DummyDCBank);
	DCBank = Easytime::Splat::TSplatStaticBuffer(std::move(DCData));

	TStaticMeshVertexData<FVector2f> LData;
	LData.Assign(LifetimeData);
	LifetimeMuW = Easytime::Splat::TSplatStaticBuffer(std::move(LData));

	TStaticMeshVertexData<FVector3f> SData;
	SData.Assign(ScalesUE);
	Scales = Easytime::Splat::TSplatStaticBuffer(std::move(SData));

	SetPositionsMeters(std::move(PositionsUE), QuantMin, QuantMax);
	SetCovariancesQuatScaleMeters(RotationsFromBanks, ScalesUE);
	SetColorsLinear(std::move(ColorsLinear));
	SetSHCoefficients(std::move(LocalSHCoefficients), LocalNumSHTriplets);
	if (!GenerateConvexHull4D(GetPositions(), ConvexHullVertices, ConvexHullIndices))
	{
		UE_LOG(LogTemp, Error, TEXT("[SOG4 Runtime Decode] Failed to generate convex hull for %s"), *GetPathName());
		return false;
	}
	return true;
}

void USplat4DAsset::PostLoad()
{
	Super::PostLoad();
	if (GetNumSplats() > 0 && XYZBank && RotBank && DCBank && LifetimeMuW && Scales)
	{
		BeginInit();
	}
}

void USplat4DAsset::Serialize(FArchive& Ar)
{
	const bool bSaveCompressed = Ar.IsSaving() && bStoreAsCompressedSOG4;
	const uint32 SavedNumSplats = GetNumSplats();
	if (bSaveCompressed)
	{
		SetNumSplats(0);
	}

	Super::Serialize(Ar);

	if (bSaveCompressed)
	{
		SetNumSplats(SavedNumSplats);
	}

	Ar << TotalFrames;
	Ar << XYZStride << RotStride << DCStride << ScaleStride;
	Ar << NumXYZBanks << NumRotBanks << NumDCBanks;

	const int32 Version = Ar.CustomVer(FSplatAssetCustomVersion::GUID);
	if (Ar.IsSaving() || Version >= FSplatAssetCustomVersion::AddedCompressedSOG4Storage)
	{
		Ar << bStoreAsCompressedSOG4;
		Ar << CompressedSOG4;
		Ar << CompressedSOG4NumSplats;
	}
	else if (Ar.IsLoading())
	{
		bStoreAsCompressedSOG4 = false;
		CompressedSOG4.Reset();
		CompressedSOG4NumSplats = 0;
	}

	if (GetNumSplats() > 0 && !bStoreAsCompressedSOG4)
	{
		Ar << XYZBank << RotBank << DCBank << LifetimeMuW << Scales;
	}

	if (Ar.IsLoading() && bStoreAsCompressedSOG4 && GetNumSplats() == 0 && CompressedSOG4NumSplats > 0)
	{
		SetNumSplats(static_cast<uint32>(CompressedSOG4NumSplats));
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

