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

	// Internal frame state; the actor owns the editor-facing playback controls.
	int32 CurrentFrame = 0;

	/** Overall opacity multiplier applied to all splats during rendering. */
	float Opacity = 1.0f;

	bool ShouldFreezeEditorInteraction() const;
	void UpdateEditorInteractionFreeze();
	void ApplyCurrentFrame();
	void ForceRefreshSplatRenderData();

	virtual void TickComponent(
		float DeltaTime,
		enum ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;
	virtual void OnUpdateTransform(
		EUpdateTransformFlags UpdateTransformFlags,
		ETeleportType Teleport) override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	bool bLastEditorInteractionFrozen = false;
};
