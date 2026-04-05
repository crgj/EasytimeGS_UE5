/*
  Copyright (c) 2025 Easytime Technology Co., Ltd. See LICENSE.md.
*/

#pragma once

#include "UObject/UnrealType.h"
#include "SplatComponent.h"
#include "Splat4DComponent.generated.h"

/**
 * Component for 4D Gaussian Splatting with frame-based interpolation.
 */
UCLASS(Blueprintable, BlueprintType, meta = (BlueprintSpawnableComponent))
class EASYTIMESPLATRUNTIME_API USplat4DComponent : public USplatComponent
{
	GENERATED_BODY()

public:
	USplat4DComponent();

	/** Current animation frame for 4D interpolation. */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "Splat4D",
		meta = (ClampMin = "0.0"))
	float CurrentFrame = 0.0f;

	void ApplyCurrentFrame();

	virtual void TickComponent(
		float DeltaTime,
		enum ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
