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

		ENQUEUE_RENDER_COMMAND(UpdateSplat4DFrame)(
			[Proxy = (Easytime::Splat::FSplatSceneProxy*)SceneProxy,
		     Frame](FRHICommandListImmediate& RHICmdList)
			{ Proxy->SetCurrentFrame_RenderThread(Frame); });
	}
}

void USplat4DComponent::TickComponent(
	float DeltaTime,
	enum ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	ApplyCurrentFrame();
}

#if WITH_EDITOR
void USplat4DComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	if (ASplat4DActor* OwnerActor = Cast<ASplat4DActor>(GetOwner()))
	{
		OwnerActor->CurrentFrame = CurrentFrame;
	}

	ApplyCurrentFrame();
	MarkRenderStateDirty();
}
#endif
