/*
  Copyright (c) 2025 Easytime Technology Co., Ltd. See LICENSE.md.
*/

#include "Splat4DActor.h"

ASplat4DActor::ASplat4DActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	PrimaryActorTick.bTickEvenWhenPaused = true;

	Splat4DComponent = CreateDefaultSubobject<USplat4DComponent>(TEXT("Splat4DComponent"));
	RootComponent = Splat4DComponent;
}

void ASplat4DActor::ClampCurrentFrameToAsset()
{
	if (!Splat4DComponent)
	{
		CurrentFrame = FMath::Max(CurrentFrame, 0);
		return;
	}

	if (const USplat4DAsset* Asset4D = Cast<USplat4DAsset>(Splat4DComponent->GetAsset()))
	{
		const int32 MaxFrame = FMath::Max(0, Asset4D->GetTotalFrames() - 1);
		CurrentFrame = FMath::Clamp(CurrentFrame, 0, MaxFrame);
	}
	else
	{
		CurrentFrame = FMath::Max(CurrentFrame, 0);
	}
}

void ASplat4DActor::SyncPropertiesToComponent(bool bRecreateRenderState)
{
	ClampCurrentFrameToAsset();

	if (Splat4DComponent)
	{
		Splat4DComponent->SetMaxSHDegree(MaxSHDegree);
		Splat4DComponent->CurrentFrame = CurrentFrame;
		Splat4DComponent->Opacity = Opacity;
		Splat4DComponent->ApplyCurrentFrame();
		
		// Only recreate render state for structural changes
		// Opacity/Frame changes are handled via ApplyCurrentFrame's render command
		if (bRecreateRenderState)
		{
			Splat4DComponent->RecreateRenderState_Concurrent();
		}
	}
}

void ASplat4DActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (bIsPlay)
	{
		PlaybackAccumulator += DeltaTime;
		constexpr float FrameInterval = 1.0f / 30.0f;

		while (PlaybackAccumulator >= FrameInterval)
		{
			PlaybackAccumulator -= FrameInterval;

			int32 MaxFrame = 0;
			if (Splat4DComponent)
			{
				if (const USplat4DAsset* Asset4D = Cast<USplat4DAsset>(Splat4DComponent->GetAsset()))
				{
					MaxFrame = FMath::Max(0, Asset4D->GetTotalFrames() - 1);
				}
			}

			if (MaxFrame > 0)
			{
				CurrentFrame = (CurrentFrame >= MaxFrame) ? 0 : (CurrentFrame + 1);
			}
			else
			{
				CurrentFrame = FMath::Max(CurrentFrame, 0);
			}
		}
	}

	SyncPropertiesToComponent(false);
}

#if WITH_EDITOR
void ASplat4DActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	PlaybackAccumulator = 0.0f;

	// Only recreate render state for structural changes, not for dynamic properties
	bool bNeedsRecreateRenderState = false;
	if (PropertyChangedEvent.Property)
	{
		FName PropertyName = PropertyChangedEvent.Property->GetFName();
		if (PropertyName == GET_MEMBER_NAME_CHECKED(ASplat4DActor, MaxSHDegree))
		{
			bNeedsRecreateRenderState = true;
		}
	}
	
	SyncPropertiesToComponent(bNeedsRecreateRenderState);
}
#endif


