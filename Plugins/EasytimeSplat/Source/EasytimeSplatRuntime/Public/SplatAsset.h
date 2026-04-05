/*
  Copyright (c) 2025 Easytime Technology Co., Ltd. See LICENSE.md.
*/

#pragma once

#include <optional>

#include "Containers/Array.h"
#include "PackedTypes.h"
#include "RenderCommandFence.h"
#include "Rendering/SplatBuffers.h"
#include "UObject/Object.h"

#include "SplatAsset.generated.h"

/**
 * Container for imported 3DGS scene/model data.
 * Owns CPU data, and handles loading and unloading of GPU data.
 *
 * @see https://dev.epicgames.com/documentation/en-us/unreal-engine/threaded-rendering-in-unreal-engine#staticresources
 */
UCLASS()
class EASYTIMESPLATRUNTIME_API USplatAsset : public UObject
{
	GENERATED_BODY()

public:
	//~ Begin UObject Interface
	virtual void BeginDestroy() override;
	virtual bool IsReadyForFinishDestroy() override;
	virtual void PostLoad() override; // Loading from disk only.
	virtual void Serialize(FArchive& Ar) override;
	//~ End UObject Interface

	/**
	 * @return SRV for this asset's colors.
	 */
	FShaderResourceViewRHIRef GetColorsSRV() const
	{
		check(Colors);
		check(Colors->ShaderResourceViewRHI);
		return Colors->ShaderResourceViewRHI;
	}

	/**
	 * Gets the indices of this asset's convex hull.
	 *
	 * @return Constant view of hull indices.
	 */
	TConstArrayView<uint32> GetConvexHullIndices() const
	{
		return ConvexHullIndices;
	}

	/**
	 * Gets the vertices of this asset's convex hull.
	 *
	 * @return Constant view of hull vertices.
	 */
	TConstArrayView<FVector3f> GetConvexHullVertices() const
	{
		return ConvexHullVertices;
	}

	/**
	 * @return SRV for this asset's covariance matrices.
	 */
	FShaderResourceViewRHIRef GetCovariancesSRV() const
	{
		check(CovariancesCM);
		check(CovariancesCM->ShaderResourceViewRHI);
		return CovariancesCM->ShaderResourceViewRHI;
	}

	/**
	 * @return The number of splats in this asset.
	 */
	uint32 GetNumSplats() const { return NumSplats; }

	/**
	 * @return Constant view of this asset's positions.
	 */
	TConstArrayView<FVector3f> GetPositions() const
	{
		return PositionsFullPrecision;
	}

	/**
	 * Gets this assets positions, alongside element-wise minimum and scaling.
	 *
	 * @param OutPosMinCM - Element-wise minimum, in centimeters.
	 * @param OutPosScaleCM - Element-wise scale, in centimeters.
	 * @return SRV for this asset's packed positions.
	 */
	FShaderResourceViewRHIRef
	GetPositionsSRV(FVector3f& OutPosMinCM, FVector3f& OutPosScaleCM) const
	{
		check(Positions);
		check(Positions->ShaderResourceViewRHI);
		OutPosMinCM = PosMinCM;
		OutPosScaleCM = PosScaleCM;
		return Positions->ShaderResourceViewRHI;
	}

#if WITH_EDITOR
	/**
	 * Populates this asset with the given colors.
	 *
	 * @param ColorsLinear - Array of linear, 8-bit-per-channel colors.
	 */
	void SetColorsLinear(TArray<FColor>&& ColorsLinear)
	{
		check(ColorsLinear.Num() == NumSplats);

		TStaticMeshVertexData<FColor> Data;
		Data.Assign(ColorsLinear);
		Colors = Easytime::Splat::TSplatStaticBuffer(std::move(Data));
	}

	/**
	 * Populates this asset with covariance matrices describing the given
	 * rotations and scales.
	 *
	 * @param Rotations - Array of rotations, one per splat.
	 * @param ScalesMeters - Array of scales, one per splat, in meters.
	 */
	void SetCovariancesQuatScaleMeters(
		const TArray<FQuat4f>& Rotations,
		const TArray<FVector3f>& ScalesMeters);

	/**
	 * Sets the number of splats in the asset.
	 *
	 * @param InNumSplats - Number of splats.
	 */
	void SetNumSplats(uint32 InNumSplats) { NumSplats = InNumSplats; }

	/**
	 * Populates this asset with the given positions. If sorting on CPU, this
	 * buffer will be kept around under the class is destroyed.
	 *
	 * @param PositionsMeters - An array of positions, one per splat, in meters.
	 */
	void SetPositionsMeters(TArray<FVector3f>&& PositionsMeters);

	/**
	 * Populates this asset with the given positions, while forcing the packed
	 * quantization range to match a wider motion envelope.
	 *
	 * @param PosMinMeters - Minimum position over all frames, in meters.
	 * @param PosMaxMeters - Maximum position over all frames, in meters.
	 */
	void SetPositionsMeters(
		TArray<FVector3f>&& PositionsMeters,
		const FVector3f& PosMinMeters,
		const FVector3f& PosMaxMeters);
#endif

private:
	/**
	 * Enqueues RHI initialization for all resources.
	 */
	void BeginInit();

	/**
	 * Creates packed position data from an array of positions. Does not copy or
	 * destroy the given buffer.
	 *
	 * @param PositionsMeters - An array of positions, one per splat, in meters.
	 */
	void SetPositionsMetersInternal(
		const TArray<FVector3f>& PositionsMeters,
		const FVector3f* OverrideMinMeters = nullptr,
		const FVector3f* OverrideMaxMeters = nullptr);

	uint32 NumSplats = 0;

	TArray<FVector3f> PositionsFullPrecision;
	FVector3f PosMinCM;
	FVector3f PosMaxCM;
	FVector3f PosScaleCM;

	/**
	 * Note: Using optionals as these are not populated until after the
	 * asset is constructed. This way, at least these buffers can always be
	 * valid post-construction.
	 */
	std::optional<Easytime::Splat::TSplatStaticBuffer<Easytime::Splat::FPackedPos>>
		Positions;
	std::optional<Easytime::Splat::TSplatStaticBuffer<Easytime::Splat::FPackedCovMat>>
		CovariancesCM;
	std::optional<Easytime::Splat::TSplatStaticBuffer<FColor>> Colors;

	TArray<FVector3f> ConvexHullVertices;
	TArray<uint32> ConvexHullIndices;

protected:
	FRenderCommandFence ReleaseResourcesFence;

#if WITH_EDITOR
	friend class USplatAssetFactory;
	friend class USplat4DAssetFactory;
#endif
};

/**
 * Container for imported 4D Gaussian Splatting data.
 */
UCLASS()
class EASYTIMESPLATRUNTIME_API USplat4DAsset : public USplatAsset
{
	GENERATED_BODY()

public:
	//~ Begin USplatAsset Interface
	virtual void BeginDestroy() override;
	virtual bool IsReadyForFinishDestroy() override;
	virtual void PostLoad() override;
	virtual void Serialize(FArchive& Ar) override;
	//~ End USplatAsset Interface

	int32 GetTotalFrames() const { return TotalFrames; }

	FShaderResourceViewRHIRef GetXYZBankSRV() const { return XYZBank->ShaderResourceViewRHI; }
	FShaderResourceViewRHIRef GetRotBankSRV() const { return RotBank->ShaderResourceViewRHI; }
	FShaderResourceViewRHIRef GetDCBankSRV() const { return DCBank->ShaderResourceViewRHI; }
	FShaderResourceViewRHIRef GetLifetimeMuWSRV() const { return LifetimeMuW->ShaderResourceViewRHI; }

	int32 GetXYZStride() const { return XYZStride; }
	int32 GetRotStride() const { return RotStride; }
	int32 GetDCStride() const { return DCStride; }

	FShaderResourceViewRHIRef GetScalesSRV() const { return Scales->ShaderResourceViewRHI; }

	uint32 GetNumXYZBanks() const { return NumXYZBanks; }
	uint32 GetNumRotBanks() const { return NumRotBanks; }
	uint32 GetNumDCBanks() const { return NumDCBanks; }

	UPROPERTY(VisibleAnywhere, Category = "Splat")
	int32 TotalFrames = 0;

	UPROPERTY(VisibleAnywhere, Category = "Splat")
	int32 NumXYZBanks = 0;
	int32 XYZStride = 1;
	UPROPERTY()
	int32 RotStride = 1;
	UPROPERTY()
	int32 DCStride = 1;
	UPROPERTY()
	int32 ScaleStride = 1;

	UPROPERTY()
	uint32 NumRotBanks = 0;

	int32 NumDCBanks = 0;

	std::optional<Easytime::Splat::TSplatStaticBuffer<FVector3f>> XYZBank;
	std::optional<Easytime::Splat::TSplatStaticBuffer<FQuat4f>> RotBank;
	std::optional<Easytime::Splat::TSplatStaticBuffer<FVector3f>> DCBank;
	std::optional<Easytime::Splat::TSplatStaticBuffer<FVector2f>> LifetimeMuW;
	std::optional<Easytime::Splat::TSplatStaticBuffer<FVector3f>> Scales;

private:
	void BeginInit();

#if WITH_EDITOR
	friend class USplat4DAssetFactory;
#endif
};

