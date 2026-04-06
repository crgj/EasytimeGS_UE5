/*
  Copyright (c) 2025 Easytime Technology Co., Ltd. See LICENSE.md.
*/

#pragma once

#include "GameFramework/Actor.h"
#include "UObject/UnrealType.h"
#include "Splat4DComponent.h"
#include "Splat4DActor.generated.h"

/**
 * Actor representing a 4D Gaussian Splatting dataset with playback control.
 */
UCLASS(Blueprintable, BlueprintType, meta=(DisplayName="Splat4DActor"))
class EASYTIMESPLATRUNTIME_API ASplat4DActor : public AActor
{
	GENERATED_BODY()

public:
	ASplat4DActor();

	/** Current animation frame for 4D interpolation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Splat 4D", meta = (ClampMin = "0", UIMin = "0", Delta = "1"))
	int32 CurrentFrame = 0;

	/** Auto-play one frame every 1/30 second. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Splat 4D", meta = (DisplayName = "IsPlay"))
	bool bIsPlay = false;

	/** Maximum spherical harmonic order to evaluate. */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "Splat 4D",
		meta = (
			ClampMin = "0",
			ClampMax = "3",
			UIMin = "0",
			UIMax = "3",
			ToolTip = "0 = DC only, 1 = SH1, 2 = SH2, 3 = SH3. The runtime will clamp to the highest order actually stored in the asset."))
	int32 MaxSHDegree = 3;

	/** Overall opacity multiplier for all splats (0 = invisible, 1 = fully opaque). */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "Splat 4D",
		meta = (
			ClampMin = "0",
			ClampMax = "1",
			UIMin = "0",
			UIMax = "1"))
	float Opacity = 1.0f;

	virtual void Tick(float DeltaTime) override;
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Splat 4D", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USplat4DComponent> Splat4DComponent;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	friend class UActorFactorySplat4D;
#endif

private:
	void ClampCurrentFrameToAsset();
	void SyncPropertiesToComponent(bool bRecreateRenderState);

	float PlaybackAccumulator = 0.0f;
};


