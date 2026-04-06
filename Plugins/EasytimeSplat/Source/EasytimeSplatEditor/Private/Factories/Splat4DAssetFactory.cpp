/*
  Copyright (c) 2025 Easytime Technology Co., Ltd. See LICENSE.md.
*/

#include "Splat4DAssetFactory.h"

#include "CompGeom/ConvexHull3.h"
#include "Dom/JsonObject.h"
#include "FileUtilities/ZipArchiveReader.h"
#include "HAL/PlatformFileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Logging.h"
#include "Misc/AssertionMacros.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SplatConstants.h"
#include "import/ply/splat_ply_conversion.h"
#include "import/ply/splat_ply_parsing.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <wincodec.h>
#include <wrl/client.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

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

float GetJsonFloatFieldFlexible(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* FieldName,
	float DefaultValue)
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

	return static_cast<float>(Number);
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
	auto DecodeWithWIC = [&CompressedBytes, &OutImage, DebugName]() -> bool
	{
		// WDD-2026-04-06-SOG4WebPFallback-UpgradeComment:SOG4Import-v2
		// .sog4 commonly stores textures in WebP (sometimes without extension).
		// ImageWrapper may return Invalid; fall back to WIC decode on Win64 editor.
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
	const EImageFormat ImageFormat =
		ImageWrapperModule.DetectImageFormat(CompressedBytes.GetData(), CompressedBytes.Num());
	if (ImageFormat != EImageFormat::Invalid)
	{
		TSharedPtr<IImageWrapper> ImageWrapper =
			ImageWrapperModule.CreateImageWrapper(ImageFormat, DebugName);
		if (ImageWrapper.IsValid() &&
			ImageWrapper->SetCompressed(CompressedBytes.GetData(), CompressedBytes.Num()))
		{
			TArray64<uint8> RawRGBA64;
			if (ImageWrapper->GetRaw(ERGBFormat::RGBA, 8, RawRGBA64))
			{
				if (!ensure(RawRGBA64.Num() <= TNumericLimits<int32>::Max()))
				{
					EASYTIME_LOGE("SOG4 decode failed: image too large (%s).", DebugName);
					return false;
				}

				OutImage.Width = static_cast<int32>(ImageWrapper->GetWidth());
				OutImage.Height = static_cast<int32>(ImageWrapper->GetHeight());
				OutImage.Bytes.SetNumUninitialized(static_cast<int32>(RawRGBA64.Num()));
				FMemory::Memcpy(
					OutImage.Bytes.GetData(),
					RawRGBA64.GetData(),
					OutImage.Bytes.Num());
				return true;
			}
		}
	}

#if PLATFORM_WINDOWS
	if (DecodeWithWIC())
	{
		return true;
	}
#endif

	if (ImageFormat == EImageFormat::Invalid)
	{
		EASYTIME_LOGE("SOG4 decode failed: invalid image format (%s).", DebugName);
	}
	else
	{
		EASYTIME_LOGE("SOG4 decode failed: unsupported/failed image decode (%s).", DebugName);
	}
	return false;
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

FQuat4f DecodePackedQuatToUE(uint8 R, uint8 G, uint8 B, uint8 A)
{
	float QWXYZ[4] = {1.0f, 0.0f, 0.0f, 0.0f};
	int32 LargestIdx = static_cast<int32>(A) - 252;
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

	// Source order is [w, x, y, z]. Convert to UE basis exactly like .ply4 path.
	const FQuat4f SourceXYZW(QWXYZ[1], QWXYZ[2], QWXYZ[3], QWXYZ[0]);
	return SafeNormalizedQuat(FQuat4f(
		-SourceXYZW.Z,
		-SourceXYZW.X,
		SourceXYZW.Y,
		SourceXYZW.W));
}

float GetCodebookValueOrDefault(
	const TArray<float>& Codebook,
	int32 Index,
	float DefaultValue = 0.0f)
{
	return Codebook.IsValidIndex(Index) ? Codebook[Index] : DefaultValue;
}

} // namespace

UObject* USplat4DAssetFactory::ImportSOG4(
	UObject* InParent,
	FName InName,
	EObjectFlags Flags,
	const uint8* Buffer,
	const uint8* BufferEnd)
{
	const int64 NumBytes = BufferEnd - Buffer;
	if (NumBytes <= 0)
	{
		EASYTIME_LOGE("Invalid .sog4 buffer for %s.", *InName.ToString());
		return nullptr;
	}
	if (!ensure(NumBytes <= TNumericLimits<int32>::Max()))
	{
		EASYTIME_LOGE("Invalid .sog4 buffer too large for %s.", *InName.ToString());
		return nullptr;
	}

	TArray<uint8> SOGBytes;
	SOGBytes.Append(Buffer, static_cast<int32>(NumBytes));

	const FString TempZipPath = FPaths::CreateTempFilename(
		*FPaths::ProjectIntermediateDir(),
		TEXT("SOG4_"),
		TEXT(".sog4"));
	ON_SCOPE_EXIT
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		PlatformFile.DeleteFile(*TempZipPath);
	};

	if (!FFileHelper::SaveArrayToFile(SOGBytes, *TempZipPath))
	{
		EASYTIME_LOGE("Failed to create temp .sog4 file for %s.", *InName.ToString());
		return nullptr;
	}

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	TUniquePtr<IFileHandle> ZipFileHandle(PlatformFile.OpenRead(*TempZipPath));
	if (!ZipFileHandle.IsValid())
	{
		EASYTIME_LOGE("Failed to open temp .sog4 file for %s.", *InName.ToString());
		return nullptr;
	}

	FZipArchiveReader ZipReader(ZipFileHandle.Release(), GWarn);
	if (!ZipReader.IsValid())
	{
		EASYTIME_LOGE("Failed to parse .sog4 zip archive for %s.", *InName.ToString());
		return nullptr;
	}

	TArray<uint8> MetaBytes;
	if (!TryReadZipEntry(ZipReader, TEXT("meta.json"), MetaBytes))
	{
		EASYTIME_LOGE("Invalid .sog4: missing meta.json for %s.", *InName.ToString());
		return nullptr;
	}

	FString MetaJsonString;
	FFileHelper::BufferToString(MetaJsonString, MetaBytes.GetData(), MetaBytes.Num());
	if (MetaJsonString.IsEmpty())
	{
		EASYTIME_LOGE("Invalid .sog4: failed to decode meta.json for %s.", *InName.ToString());
		return nullptr;
	}

	TSharedPtr<FJsonObject> MetaObject;
	const TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(MetaJsonString);
	if (!FJsonSerializer::Deserialize(JsonReader, MetaObject) || !MetaObject.IsValid())
	{
		EASYTIME_LOGE("Invalid .sog4: failed to parse meta.json for %s.", *InName.ToString());
		return nullptr;
	}

	const int32 Count = GetJsonIntFieldFlexible(MetaObject, TEXT("count"), 0);
	if (Count <= 0)
	{
		EASYTIME_LOGE("Invalid .sog4: invalid count for %s.", *InName.ToString());
		return nullptr;
	}

	const TSharedPtr<FJsonObject>* CustomObjectPtr = nullptr;
	const TSharedPtr<FJsonObject> CustomObject =
		MetaObject->TryGetObjectField(TEXT("custom"), CustomObjectPtr) && CustomObjectPtr
			? *CustomObjectPtr
			: nullptr;

	// Keep SOG4 assets compressed on disk; decode only when used in level/runtime.
	{
		const int32 TotalFramesFromCustom =
			GetJsonIntFieldFlexible(CustomObject, TEXT("total_frames"), 0);
		const int32 TotalFramesFromMeta =
			GetJsonIntFieldFlexible(MetaObject, TEXT("total_frames"), 0);
		const int32 TotalFrames = FMath::Max(
			1,
			(TotalFramesFromCustom > 0) ? TotalFramesFromCustom : TotalFramesFromMeta);

		USog4SplatAsset* ResultAsset = NewObject<USog4SplatAsset>(InParent, InName, Flags);
		ResultAsset->SetCompressedSOG4Source(TArray<uint8>(SOGBytes), Count);
		ResultAsset->SetNumSplats(Count);
		ResultAsset->TotalFrames = TotalFrames;
		ResultAsset->XYZStride = 1;
		ResultAsset->RotStride = 1;
		ResultAsset->DCStride = 1;
		ResultAsset->NumXYZBanks = 0;
		ResultAsset->NumRotBanks = 0;
		ResultAsset->NumDCBanks = 0;
		EASYTIME_LOGL(
			"[SOG4 Import] Stored compressed asset only. splats=%d frames=%d compressedBytes=%d",
			Count,
			TotalFrames,
			SOGBytes.Num());
		return ResultAsset;
	}

	TArray<FVector3f> PositionsSrc;
	PositionsSrc.Init(FVector3f::ZeroVector, Count);
	TArray<FQuat4f> RotationsUE;
	RotationsUE.Init(FQuat4f::Identity, Count);
	TArray<FVector3f> ScalesUE;
	ScalesUE.Init(FVector3f(1.0f, 1.0f, 1.0f), Count);
	TArray<float> OpacityLogit;
	OpacityLogit.Init(20.0f, Count);
	TArray<FVector4f> ColorsLinear;
	ColorsLinear.Init(
		FVector4f(
			to_color_linear_float(0.0f),
			to_color_linear_float(0.0f),
			to_color_linear_float(0.0f),
			1.0f),
		Count);

	// Static means
	const TSharedPtr<FJsonObject>* MeansObjPtr = nullptr;
	if (MetaObject->TryGetObjectField(TEXT("means"), MeansObjPtr) && MeansObjPtr)
	{
		const TSharedPtr<FJsonObject> MeansObj = *MeansObjPtr;
		TArray<FString> Files;
		FVector3f Mins(0.0f), Maxs(0.0f);
		if (TryGetFilesField(MeansObj, Files) && Files.Num() >= 2 &&
			TryGetJsonVec3Field(MeansObj, TEXT("mins"), Mins) &&
			TryGetJsonVec3Field(MeansObj, TEXT("maxs"), Maxs))
		{
			FSOGImageRGBA8 MeansL;
			FSOGImageRGBA8 MeansU;
			if (TryLoadZipImageRGBA8(ZipReader, Files[0], MeansL) &&
				TryLoadZipImageRGBA8(ZipReader, Files[1], MeansU) &&
				MeansL.HasPixelsForCount(Count) &&
				MeansU.HasPixelsForCount(Count))
			{
				for (int32 Index = 0; Index < Count; ++Index)
				{
					const int32 Base = Index * 4;
					const uint32 NX = (static_cast<uint32>(MeansU.Bytes[Base + 0]) << 8) | MeansL.Bytes[Base + 0];
					const uint32 NY = (static_cast<uint32>(MeansU.Bytes[Base + 1]) << 8) | MeansL.Bytes[Base + 1];
					const uint32 NZ = (static_cast<uint32>(MeansU.Bytes[Base + 2]) << 8) | MeansL.Bytes[Base + 2];

					const float FX = InverseLogTransform((static_cast<float>(NX) / 65535.0f) * (Maxs.X - Mins.X) + Mins.X);
					const float FY = InverseLogTransform((static_cast<float>(NY) / 65535.0f) * (Maxs.Y - Mins.Y) + Mins.Y);
					const float FZ = InverseLogTransform((static_cast<float>(NZ) / 65535.0f) * (Maxs.Z - Mins.Z) + Mins.Z);
					PositionsSrc[Index] = FVector3f(FX, FY, FZ);
				}
			}
		}
	}

	// Static rotations
	const TSharedPtr<FJsonObject>* QuatsObjPtr = nullptr;
	if (MetaObject->TryGetObjectField(TEXT("quats"), QuatsObjPtr) && QuatsObjPtr)
	{
		TArray<FString> Files;
		if (TryGetFilesField(*QuatsObjPtr, Files) && Files.Num() >= 1)
		{
			FSOGImageRGBA8 RotationImage;
			if (TryLoadZipImageRGBA8(ZipReader, Files[0], RotationImage) &&
				RotationImage.HasPixelsForCount(Count))
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

	// Opacity texture (fallback if sh0 alpha is unavailable)
	const TSharedPtr<FJsonObject>* OpacityObjPtr = nullptr;
	if (MetaObject->TryGetObjectField(TEXT("opacity"), OpacityObjPtr) && OpacityObjPtr)
	{
		TArray<FString> Files;
		if (TryGetFilesField(*OpacityObjPtr, Files) && Files.Num() >= 1)
		{
			FSOGImageRGBA8 OpacityImage;
			if (TryLoadZipImageRGBA8(ZipReader, Files[0], OpacityImage) &&
				OpacityImage.HasPixelsForCount(Count))
			{
				for (int32 Index = 0; Index < Count; ++Index)
				{
					OpacityLogit[Index] = DecodeLogitFromUNorm8(OpacityImage.Bytes[Index * 4 + 3]);
				}
			}
		}
	}

	// Static scales
	const TSharedPtr<FJsonObject>* ScalesObjPtr = nullptr;
	if (MetaObject->TryGetObjectField(TEXT("scales"), ScalesObjPtr) && ScalesObjPtr)
	{
		const TSharedPtr<FJsonObject> ScalesObj = *ScalesObjPtr;
		const TArray<float> ScaleCodebook = GetJsonFloatArrayField(ScalesObj, TEXT("codebook"));
		TArray<FString> Files;
		if (ScaleCodebook.Num() > 0 && TryGetFilesField(ScalesObj, Files) && Files.Num() >= 1)
		{
			FSOGImageRGBA8 ScalesImage;
			if (TryLoadZipImageRGBA8(ZipReader, Files[0], ScalesImage) &&
				ScalesImage.HasPixelsForCount(Count))
			{
				for (int32 Index = 0; Index < Count; ++Index)
				{
					const int32 Base = Index * 4;
					const float ScaleXLog = GetCodebookValueOrDefault(ScaleCodebook, ScalesImage.Bytes[Base + 0]);
					const float ScaleYLog = GetCodebookValueOrDefault(ScaleCodebook, ScalesImage.Bytes[Base + 1]);
					const float ScaleZLog = GetCodebookValueOrDefault(ScaleCodebook, ScalesImage.Bytes[Base + 2]);
					ScalesUE[Index] = FVector3f(
						FMath::Exp(ScaleZLog),
						FMath::Exp(ScaleXLog),
						FMath::Exp(ScaleYLog));
				}
			}
		}
	}

	int32 NumSHTriplets = 0;
	TArray<FVector3f> SHCoefficients;
	bool bDecodedSH0 = false;

	// SH0 (DC)
	const TSharedPtr<FJsonObject>* SH0ObjPtr = nullptr;
	if (MetaObject->TryGetObjectField(TEXT("sh0"), SH0ObjPtr) && SH0ObjPtr)
	{
		const TSharedPtr<FJsonObject> SH0Obj = *SH0ObjPtr;
		const TArray<float> SH0Codebook = GetJsonFloatArrayField(SH0Obj, TEXT("codebook"));
		TArray<FString> Files;
		if (SH0Codebook.Num() > 0 && TryGetFilesField(SH0Obj, Files) && Files.Num() >= 1)
		{
			FSOGImageRGBA8 SH0Image;
			if (TryLoadZipImageRGBA8(ZipReader, Files[0], SH0Image) &&
				SH0Image.HasPixelsForCount(Count))
			{
				bDecodedSH0 = true;
				for (int32 Index = 0; Index < Count; ++Index)
				{
					const int32 Base = Index * 4;
					const float DCR = GetCodebookValueOrDefault(SH0Codebook, SH0Image.Bytes[Base + 0]);
					const float DCG = GetCodebookValueOrDefault(SH0Codebook, SH0Image.Bytes[Base + 1]);
					const float DCB = GetCodebookValueOrDefault(SH0Codebook, SH0Image.Bytes[Base + 2]);
					ColorsLinear[Index] = FVector4f(
						to_color_linear_float(DCR),
						to_color_linear_float(DCG),
						to_color_linear_float(DCB),
						to_alpha_linear_float(DecodeLogitFromUNorm8(SH0Image.Bytes[Base + 3])));
				}
			}
		}
	}
	if (!bDecodedSH0)
	{
		if (MetaObject->HasTypedField<EJson::Object>(TEXT("sh0")))
		{
			EASYTIME_LOGE(
				"SOG4 SH0 decode failed or incomplete for %s; falling back to default base color + opacity texture.",
				*InName.ToString());
		}
		for (int32 Index = 0; Index < Count; ++Index)
		{
			ColorsLinear[Index].W = to_alpha_linear_float(OpacityLogit[Index]);
		}
	}

	// SHN (f_rest)
	const TSharedPtr<FJsonObject>* SHNObjPtr = nullptr;
	if (MetaObject->TryGetObjectField(TEXT("shN"), SHNObjPtr) && SHNObjPtr)
	{
		const TSharedPtr<FJsonObject> SHNObj = *SHNObjPtr;
		const int32 Bands = GetJsonIntFieldFlexible(SHNObj, TEXT("bands"), 3);
		const int32 NumCoeffs = (Bands == 1) ? 9 : ((Bands == 2) ? 24 : 45);
		NumSHTriplets = NumCoeffs / 3;
		SHCoefficients.Init(FVector3f::ZeroVector, Count * NumSHTriplets);

		const TArray<float> SHNCodebook = GetJsonFloatArrayField(SHNObj, TEXT("codebook"));
		const int32 PaletteSize = GetJsonIntFieldFlexible(SHNObj, TEXT("count"), 0);
		TArray<FString> Files;
		if (SHNCodebook.Num() > 0 && PaletteSize > 0 &&
			TryGetFilesField(SHNObj, Files) && Files.Num() >= 2)
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

						Palette[PaletteIndex * NumCoeffs + CoeffsPerChannel * 0 + C] =
							GetCodebookValueOrDefault(SHNCodebook, CentersImage.Bytes[ByteIndex + 0]);
						Palette[PaletteIndex * NumCoeffs + CoeffsPerChannel * 1 + C] =
							GetCodebookValueOrDefault(SHNCodebook, CentersImage.Bytes[ByteIndex + 1]);
						Palette[PaletteIndex * NumCoeffs + CoeffsPerChannel * 2 + C] =
							GetCodebookValueOrDefault(SHNCodebook, CentersImage.Bytes[ByteIndex + 2]);
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
					for (int32 T = 0; T < NumSHTriplets; ++T)
					{
						SHCoefficients[Index * NumSHTriplets + T] = FVector3f(
							Palette[PaletteBase + T],
							Palette[PaletteBase + NumSHTriplets + T],
							Palette[PaletteBase + NumSHTriplets * 2 + T]);
					}
				}
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* XYZBankArray = nullptr;
	int32 NumXYZBanks = 0;
	TArray<FVector3f> XYZBankData;
	int32 XYZStride = 1;

	if (MetaObject->TryGetArrayField(TEXT("xyz_bank"), XYZBankArray) &&
		XYZBankArray && XYZBankArray->Num() > 0)
	{
		NumXYZBanks = XYZBankArray->Num();
		XYZStride = GetJsonIntFieldFlexible(CustomObject, TEXT("xyz_bank_keyframe_stride"),
			GetJsonIntFieldFlexible(MetaObject, TEXT("xyz_bank_stride"), 1));
		XYZStride = FMath::Max(1, XYZStride);
		XYZBankData.SetNumUninitialized(Count * NumXYZBanks);

		for (int32 BankIndex = 0; BankIndex < NumXYZBanks; ++BankIndex)
		{
			const TSharedPtr<FJsonObject> BankObject = (*XYZBankArray)[BankIndex]->AsObject();
			TArray<FString> Files;
			FVector3f Mins(0.0f), Maxs(0.0f);
			const bool bCanDecode =
				BankObject.IsValid() &&
				TryGetFilesField(BankObject, Files) &&
				Files.Num() >= 2 &&
				TryGetJsonVec3Field(BankObject, TEXT("mins"), Mins) &&
				TryGetJsonVec3Field(BankObject, TEXT("maxs"), Maxs);

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
					const float FX = InverseLogTransform((static_cast<float>(NX) / 65535.0f) * (Maxs.X - Mins.X) + Mins.X);
					const float FY = InverseLogTransform((static_cast<float>(NY) / 65535.0f) * (Maxs.Y - Mins.Y) + Mins.Y);
					const float FZ = InverseLogTransform((static_cast<float>(NZ) / 65535.0f) * (Maxs.Z - Mins.Z) + Mins.Z);
					XYZBankData[Index * NumXYZBanks + BankIndex] = FVector3f(FX, FY, FZ);
				}
				else
				{
					XYZBankData[Index * NumXYZBanks + BankIndex] = PositionsSrc[Index];
				}
			}
		}
	}
	else
	{
		NumXYZBanks = 1;
		XYZStride = 1;
		XYZBankData.SetNumUninitialized(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			XYZBankData[Index] = PositionsSrc[Index];
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* RotBankArray = nullptr;
	int32 NumRotBanks = 0;
	TArray<FQuat4f> RotBankData;
	int32 RotStride = 1;

	if (MetaObject->TryGetArrayField(TEXT("rot_bank"), RotBankArray) &&
		RotBankArray && RotBankArray->Num() > 0)
	{
		NumRotBanks = RotBankArray->Num();
		RotStride = GetJsonIntFieldFlexible(
			CustomObject,
			TEXT("rot_bank_keyframe_stride"),
			GetJsonIntFieldFlexible(MetaObject, TEXT("rot_bank_stride"), XYZStride));
		RotStride = FMath::Max(1, RotStride);
		RotBankData.SetNumUninitialized(Count * NumRotBanks);

		for (int32 BankIndex = 0; BankIndex < NumRotBanks; ++BankIndex)
		{
			const TSharedPtr<FJsonObject> BankObject = (*RotBankArray)[BankIndex]->AsObject();
			TArray<FString> Files;
			FSOGImageRGBA8 RotationImage;
			const bool bHasImage = BankObject.IsValid() &&
				TryGetFilesField(BankObject, Files) &&
				Files.Num() >= 1 &&
				TryLoadZipImageRGBA8(ZipReader, Files[0], RotationImage) &&
				RotationImage.HasPixelsForCount(Count);

			for (int32 Index = 0; Index < Count; ++Index)
			{
				if (bHasImage)
				{
					const int32 Base = Index * 4;
					RotBankData[Index * NumRotBanks + BankIndex] = DecodePackedQuatToUE(
						RotationImage.Bytes[Base + 0],
						RotationImage.Bytes[Base + 1],
						RotationImage.Bytes[Base + 2],
						RotationImage.Bytes[Base + 3]);
				}
				else
				{
					RotBankData[Index * NumRotBanks + BankIndex] = RotationsUE[Index];
				}
			}
		}
	}
	else
	{
		NumRotBanks = 1;
		RotStride = 1;
		RotBankData.SetNumUninitialized(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			RotBankData[Index] = RotationsUE[Index];
		}
	}

	const int32 TotalFramesFromCustom =
		GetJsonIntFieldFlexible(CustomObject, TEXT("total_frames"), 0);
	int32 TotalFrames = (TotalFramesFromCustom > 0)
		? TotalFramesFromCustom
		: GetJsonIntFieldFlexible(MetaObject, TEXT("total_frames"), 0);
	if (TotalFrames <= 0)
	{
		TotalFrames = (NumXYZBanks > 1) ? ((NumXYZBanks - 1) * XYZStride + 1) : 1;
	}
	TotalFrames = FMath::Max(1, TotalFrames);

	TArray<FVector2f> LifetimeData;
	const float DefaultMu = 0.5f * static_cast<float>(TotalFrames);
	const float DefaultW = FMath::Max(1.0f, static_cast<float>(TotalFrames));
	LifetimeData.Init(FVector2f(DefaultMu, DefaultW), Count);

	const TSharedPtr<FJsonObject>* ParamsObjPtr = nullptr;
	if (MetaObject->TryGetObjectField(TEXT("params"), ParamsObjPtr) && ParamsObjPtr)
	{
		const TSharedPtr<FJsonObject> ParamsObj = *ParamsObjPtr;
		TArray<FString> Files;
		if (TryGetFilesField(ParamsObj, Files) && Files.Num() >= 1)
		{
			TArray<float> MuCodebook = GetJsonFloatArrayField(ParamsObj, TEXT("codebook_mu"));
			if (MuCodebook.Num() == 0)
			{
				MuCodebook = GetJsonFloatArrayField(ParamsObj, TEXT("codebook_mu_list"));
			}
			if (MuCodebook.Num() == 0)
			{
				MuCodebook = GetJsonFloatArrayField(ParamsObj, TEXT("codebook"));
			}

			TArray<float> WCodebook = GetJsonFloatArrayField(ParamsObj, TEXT("codebook_w"));
			if (WCodebook.Num() == 0)
			{
				WCodebook = GetJsonFloatArrayField(ParamsObj, TEXT("codebook_w_list"));
			}
			if (WCodebook.Num() == 0)
			{
				WCodebook = GetJsonFloatArrayField(ParamsObj, TEXT("codebook"));
			}

			FSOGImageRGBA8 ParamsImage;
			if (MuCodebook.Num() > 0 && WCodebook.Num() > 0 &&
				TryLoadZipImageRGBA8(ZipReader, Files[0], ParamsImage) &&
				ParamsImage.HasPixelsForCount(Count))
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
	else
	{
		const TSharedPtr<FJsonObject>* LifetimeObjPtr = nullptr;
		if (MetaObject->TryGetObjectField(TEXT("lifetime"), LifetimeObjPtr) && LifetimeObjPtr)
		{
			const TSharedPtr<FJsonObject> LifetimeObj = *LifetimeObjPtr;
			TArray<FString> Files;
			const TArray<float> MinsArray = GetJsonFloatArrayField(LifetimeObj, TEXT("mins"));
			const TArray<float> MaxsArray = GetJsonFloatArrayField(LifetimeObj, TEXT("maxs"));
			if (TryGetFilesField(LifetimeObj, Files) && Files.Num() >= 1 &&
				MinsArray.Num() >= 2 && MaxsArray.Num() >= 2)
			{
				FSOGImageRGBA8 LifetimeImage;
				if (TryLoadZipImageRGBA8(ZipReader, Files[0], LifetimeImage) &&
					LifetimeImage.HasPixelsForCount(Count))
				{
					const float MinMu = MinsArray[0];
					const float MaxMu = MaxsArray[0];
					const float MinW = MinsArray[1];
					const float MaxW = MaxsArray[1];
					for (int32 Index = 0; Index < Count; ++Index)
					{
						const int32 Base = Index * 4;
						const float Mu = (static_cast<float>(LifetimeImage.Bytes[Base + 0]) / 255.0f) * (MaxMu - MinMu) + MinMu;
						const float W = (static_cast<float>(LifetimeImage.Bytes[Base + 1]) / 255.0f) * (MaxW - MinW) + MinW;
						LifetimeData[Index] = FVector2f(Mu, FMath::Max(W, 1e-3f));
					}
				}
			}
		}
	}

	// WDD-2026-04-06-SOG4Support-UpgradeComment:SOG4Import-v1
	// Keep .sog4 runtime path identical to .ply4 by materializing equivalent
	// per-splat/bank buffers and metadata.
	USog4SplatAsset* ResultAsset = NewObject<USog4SplatAsset>(InParent, InName, Flags);
	ResultAsset->SetCompressedSOG4Source(TArray<uint8>(SOGBytes), Count);
	ResultAsset->SetNumSplats(Count);
	ResultAsset->TotalFrames = TotalFrames;
	ResultAsset->XYZStride = XYZStride;
	ResultAsset->RotStride = RotStride;
	ResultAsset->DCStride = 1;
	ResultAsset->NumXYZBanks = NumXYZBanks;
	ResultAsset->NumRotBanks = NumRotBanks;
	ResultAsset->NumDCBanks = 0;

	TArray<FVector3f> PositionsUE;
	PositionsUE.SetNumUninitialized(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FVector3f& Src = XYZBankData[Index * NumXYZBanks];
		PositionsUE[Index] = FVector3f(Src.Z, Src.X, -Src.Y);
	}

	TArray<FQuat4f> RotationsFromBanks;
	RotationsFromBanks.SetNumUninitialized(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		RotationsFromBanks[Index] = SafeNormalizedQuat(RotBankData[Index * NumRotBanks]);
	}

	FVector3f QuantMin = PositionsUE.Num() > 0 ? PositionsUE[0] : FVector3f::ZeroVector;
	FVector3f QuantMax = QuantMin;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		for (int32 BankIndex = 0; BankIndex < NumXYZBanks; ++BankIndex)
		{
			const FVector3f& Src = XYZBankData[Index * NumXYZBanks + BankIndex];
			const FVector3f Converted(Src.Z, Src.X, -Src.Y);
			QuantMin = QuantMin.ComponentMin(Converted);
			QuantMax = QuantMax.ComponentMax(Converted);
		}
	}

	TStaticMeshVertexData<FVector3f> XYZData;
	XYZData.Assign(XYZBankData);
	ResultAsset->XYZBank = Easytime::Splat::TSplatStaticBuffer(std::move(XYZData));

	TStaticMeshVertexData<FQuat4f> RotData;
	RotData.Assign(RotBankData);
	ResultAsset->RotBank = Easytime::Splat::TSplatStaticBuffer(std::move(RotData));

	TStaticMeshVertexData<FVector3f> DCData;
	// WDD-2026-04-06-SOG4ZeroSizedDCBankCrash-UpgradeComment:SOG4Import-v3
	// Keep DCBank as a valid SRV-backed buffer even when num_dc_banks=0.
	// Runtime interpolation gates DC reads by num_dc_banks, so a single dummy
	// entry avoids zero-sized RHI buffer creation without affecting shading.
	TArray<FVector3f> DummyDCBank;
	DummyDCBank.Add(FVector3f::ZeroVector);
	DCData.Assign(DummyDCBank);
	ResultAsset->DCBank = Easytime::Splat::TSplatStaticBuffer(std::move(DCData));

	TStaticMeshVertexData<FVector2f> LData;
	LData.Assign(LifetimeData);
	ResultAsset->LifetimeMuW = Easytime::Splat::TSplatStaticBuffer(std::move(LData));

	TStaticMeshVertexData<FVector3f> SData;
	SData.Assign(ScalesUE);
	ResultAsset->Scales = Easytime::Splat::TSplatStaticBuffer(std::move(SData));

	if (Count > 0 && ColorsLinear.Num() > 0)
	{
		const FVector4f& C0 = ColorsLinear[0];
		EASYTIME_LOGL(
			"[SOG4 Import DC] DecodedSH0=%d BaseColor0=(%.6f, %.6f, %.6f, %.6f)",
			bDecodedSH0 ? 1 : 0,
			C0.X,
			C0.Y,
			C0.Z,
			C0.W);
	}
	if (NumSHTriplets > 0 && SHCoefficients.Num() > 0)
	{
		const FVector3f& SH0 = SHCoefficients[0];
		EASYTIME_LOGL(
			"[SOG4 Import SH0] Raw=(%.6f, %.6f, %.6f) triplets=%d",
			SH0.X,
			SH0.Y,
			SH0.Z,
			NumSHTriplets);
	}

	ResultAsset->SetPositionsMeters(std::move(PositionsUE), QuantMin, QuantMax);
	ResultAsset->SetCovariancesQuatScaleMeters(RotationsFromBanks, ScalesUE);
	ResultAsset->SetColorsLinear(std::move(ColorsLinear));
	ResultAsset->SetSHCoefficients(std::move(SHCoefficients), NumSHTriplets);

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

	EASYTIME_LOGL(
		"[SOG4 Import] splats=%d frames=%d xyzBanks=%d rotBanks=%d shTriplets=%d xyzStride=%d rotStride=%d",
		Count,
		TotalFrames,
		NumXYZBanks,
		NumRotBanks,
		NumSHTriplets,
		XYZStride,
		RotStride);

	return ResultAsset;
}

USplat4DAssetFactory::USplat4DAssetFactory()
{
	SupportedClass = USplat4DAsset::StaticClass();
	Formats.Emplace(TEXT("ply4;4D Gaussian splat (Ply4)"));
	Formats.Emplace(TEXT("sog4;4D Gaussian splat (SOG4)"));
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

	const bool bLooksLikeZip =
		(BufferEnd - Buffer) >= 4 &&
		Buffer[0] == 0x50 &&
		Buffer[1] == 0x4B &&
		Buffer[2] == 0x03 &&
		Buffer[3] == 0x04;
	const FString TypeString = Type ? FString(Type).ToLower() : FString();
	const bool bIsSOG4 = TypeString == TEXT("sog4") || bLooksLikeZip;
	if (bIsSOG4)
	{
		return ImportSOG4(InParent, InName, Flags, Buffer, BufferEnd);
	}

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

		UPly4SplatAsset* ResultAsset = NewObject<UPly4SplatAsset>(InParent, InName, Flags);
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
			// WDD-2026-04-06-LinearDCImportFix-UpgradeComment:ColorMatch-v1
			// 4D bank frame-0 fallback should also stay linear to match runtime.
			Colors[i] = FVector4f(
				to_color_linear_float(DC0.X),
				to_color_linear_float(DC0.Y),
				to_color_linear_float(DC0.Z),
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
	// WDD-2026-04-06-PLY4ZeroSizedBankGuard-UpgradeComment:PLY4Import-v2
	// Some .ply4 variants may omit temporal banks (count=0). Keep one dummy
	// element so GPU SRV creation never hits zero-sized buffer edge cases.
	if (XYZBankData.Num() == 0)
	{
		XYZBankData.Add(FVector3f::ZeroVector);
	}
	XYZData.Assign(XYZBankData);
	ResultAsset->XYZBank = Easytime::Splat::TSplatStaticBuffer(std::move(XYZData));

	TStaticMeshVertexData<FQuat4f> RotData;
	if (RotBankData.Num() == 0)
	{
		RotBankData.Add(FQuat4f::Identity);
	}
	RotData.Assign(RotBankData);
	ResultAsset->RotBank = Easytime::Splat::TSplatStaticBuffer(std::move(RotData));

	TStaticMeshVertexData<FVector3f> DCData;
	if (DCBankData.Num() == 0)
	{
		DCBankData.Add(FVector3f::ZeroVector);
	}
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
