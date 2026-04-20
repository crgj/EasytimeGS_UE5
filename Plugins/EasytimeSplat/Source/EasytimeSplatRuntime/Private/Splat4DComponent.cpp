/*
  Copyright (c) 2025 Easytime Technology Co., Ltd. See LICENSE.md.
*/

#include "Splat4DComponent.h"
#include "Splat4DActor.h"
#include "Rendering/SplatSceneProxy.h"

USplat4DComponent::USplat4DComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	bTickInEditor = true;
}

void USplat4DComponent::ApplyCurrentFrame()
{
	if (SceneProxy)
	{
		float Frame = CurrentFrame;
		// Boundary check against asset frames
		if (USplat4DAsset* Asset4D = Cast<USplat4DAsset>(GetAsset()))
		{
			if (Frame > (float)Asset4D->TotalFrames - 1.0f)
			{
				Frame = (float)Asset4D->TotalFrames - 1.0f;
			}
			if (Frame < 0.0f)
			{
				Frame = 0.0f;
			}
		}

		float OpacityValue = Opacity;
		float BrightnessValue = GetBrightness();
		float ContrastValue = GetContrast();
		FLinearColor ColorTintValue = GetColorTint();

		ENQUEUE_RENDER_COMMAND(UpdateSplat4DFrame)(
			[Proxy = (Easytime::Splat::FSplatSceneProxy*)SceneProxy,
		     Frame, OpacityValue, BrightnessValue, ContrastValue, ColorTintValue](FRHICommandListImmediate& RHICmdList)
			{ 
				Proxy->SetCurrentFrame_RenderThread(Frame); 
				Proxy->SetOpacity_RenderThread(OpacityValue);
				Proxy->SetColorControls_RenderThread(
					BrightnessValue,
					ContrastValue,
					ColorTintValue);
			});
	}
}

void USplat4DComponent::ForceRefreshSplatRenderData()
{
	ApplyCurrentFrame();
	UpdateBounds();
	MarkRenderTransformDirty();
	MarkRenderDynamicDataDirty();
}

void USplat4DComponent::OnUpdateTransform(
	EUpdateTransformFlags UpdateTransformFlags,
	ETeleportType Teleport)
{
	Super::OnUpdateTransform(UpdateTransformFlags, Teleport);
	ForceRefreshSplatRenderData();
}

void USplat4DComponent::TickComponent(
	float DeltaTime,
	enum ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	ForceRefreshSplatRenderData();
}

#if WITH_EDITOR
void USplat4DComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	// Don't call Super::PostEditChangeProperty to avoid MarkRenderStateDirty() which recreates SceneProxy

	// Only sync frame back to actor if frame property changed (avoid loops)
	if (PropertyChangedEvent.Property &&
		PropertyChangedEvent.Property->GetFName() == GET_MEMBER_NAME_CHECKED(USplat4DComponent, CurrentFrame))
	{
		if (ASplat4DActor* OwnerActor = Cast<ASplat4DActor>(GetOwner()))
		{
			OwnerActor->CurrentFrame = CurrentFrame;
		}
	}

	ForceRefreshSplatRenderData();
}
#endif
