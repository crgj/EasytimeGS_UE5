/*
  Copyright (c) 2025 Easytime Technology Co., Ltd. See LICENSE.md.
*/

#pragma once

#include "Components/PrimitiveComponent.h"
#include "PhysicsEngine/BodySetup.h"
#include "SplatAsset.h"
#include "SplatComponent.generated.h"

/**
 * Component holding a renderable 3DGS model or scene.
 *
 * @see https://dev.epicgames.com/documentation/en-us/unreal-engine/components-in-unreal-engine
 *
 * TODO(seth): I haven't figured out why the BodyInstance's Physics Actor is not
 * being created successfully on device. Until this is resolved, physics won't
 * work on device.
 */
UCLASS()
class USplatComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	//~ Begin UPrimitiveComponent Interface
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual UBodySetup* GetBodySetup() override;
#if WITH_EDITOR
	// Materials are only used in Editor, for mouse selection and debug views.
	virtual void GetUsedMaterials(
		TArray<UMaterialInterface*>& OutMaterials,
		bool bGetDebugMaterials = false) const override;
#endif
	//~ End UPrimitiveComponent Interface

	//~ Begin USceneComponent Interface
	virtual FBoxSphereBounds
	CalcBounds(const FTransform& LocalToWorld) const override;
#if WITH_EDITOR
	virtual bool ShouldCollideWhenPlacing() const override { return true; }
#endif
	//~ End USceneComponent Interface

	/**
	 * Gets the asset this component is tied to, if any.
	 *
	 * @return The asset attached to this component, or nullptr.
	 */
	TObjectPtr<USplatAsset> GetAsset() const { return Asset; }
	int32 GetMaxSHDegree() const { return MaxSHDegree; }
	float GetBrightness() const { return Brightness; }
	float GetContrast() const { return Contrast; }
	FLinearColor GetColorTint() const { return ColorTint; }
	void SetMaxSHDegree(int32 InMaxSHDegree)
	{
		const int32 Clamped = FMath::Clamp(InMaxSHDegree, 0, 3);
		if (MaxSHDegree == Clamped)
		{
			return;
		}

		MaxSHDegree = Clamped;
		MarkRenderStateDirty();
	}
	void SetBrightness(float InBrightness)
	{
		const float Clamped = FMath::Clamp(InBrightness, -1.0f, 1.0f);
		if (FMath::IsNearlyEqual(Brightness, Clamped))
		{
			return;
		}

		Brightness = Clamped;
		ApplyColorControls();
	}
	void SetContrast(float InContrast)
	{
		const float Clamped = FMath::Clamp(InContrast, 0.0f, 4.0f);
		if (FMath::IsNearlyEqual(Contrast, Clamped))
		{
			return;
		}

		Contrast = Clamped;
		ApplyColorControls();
	}
	void SetColorTint(FLinearColor InColorTint)
	{
		InColorTint.A = 1.0f;
		if (ColorTint.Equals(InColorTint))
		{
			return;
		}

		ColorTint = InColorTint;
		ApplyColorControls();
	}

	void ApplyColorControls();

	/**
	 * Sets the asset and refreshes render/collision state.
	 */
	void SetAsset(TObjectPtr<USplatAsset> InAsset)
	{
		if (Asset == InAsset)
		{
			return;
		}

		Asset = InAsset;
		BodySetup = nullptr;
		if (Asset)
		{
			if (USplat4DAsset* Asset4D = Cast<USplat4DAsset>(Asset))
			{
				Asset4D->EnsureExpandedRuntimeData();
			}
			GetBodySetup();
		}
		UpdateBounds();
		MarkRenderStateDirty();
	}

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	UPROPERTY(Category = Splat, EditAnywhere)
	TObjectPtr<USplatAsset> Asset;

	UPROPERTY(
		EditAnywhere,
		Category = Splat,
		meta = (
			ClampMin = "0",
			ClampMax = "3",
			UIMin = "0",
			UIMax = "3",
			ToolTip = "0 = DC only, 1 = SH1, 2 = SH2, 3 = SH3. The runtime will clamp to the highest order actually stored in the asset."))
	int32 MaxSHDegree = 3;

	UPROPERTY(
		EditAnywhere,
		Category = "Splat|Color",
		meta = (
			ClampMin = "-1",
			ClampMax = "1",
			UIMin = "-1",
			UIMax = "1",
			ToolTip = "Brightness offset applied to every splat color after SH evaluation. 0 is neutral."))
	float Brightness = 0.0f;

	UPROPERTY(
		EditAnywhere,
		Category = "Splat|Color",
		meta = (
			ClampMin = "0",
			ClampMax = "4",
			UIMin = "0",
			UIMax = "4",
			ToolTip = "Contrast multiplier applied around 0.5 after SH evaluation. 1 is neutral."))
	float Contrast = 1.0f;

	UPROPERTY(
		EditAnywhere,
		Category = "Splat|Color",
		meta = (
			HideAlphaChannel,
			ToolTip = "Per-channel color tint applied before brightness and contrast. White is neutral."))
	FLinearColor ColorTint = FLinearColor::White;

	UPROPERTY()
	TObjectPtr<UBodySetup> BodySetup;

#if WITH_EDITOR
	friend class UActorFactorySplat;
	friend class UActorFactorySplat4D;
#endif
};

