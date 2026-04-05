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
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Splat4D", meta = (ClampMin = "0.0"))
	float CurrentFrame = 0.0f;

	virtual void Tick(float DeltaTime) override;
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = Splat, meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USplat4DComponent> Splat4DComponent;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	friend class UActorFactorySplat4D;
#endif
};


