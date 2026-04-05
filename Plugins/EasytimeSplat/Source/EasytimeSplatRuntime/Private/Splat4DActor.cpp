/*
  Copyright (c) 2025 Easytime Technology Co., Ltd. See LICENSE.md.
*/

#include "Splat4DActor.h"

ASplat4DActor::ASplat4DActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	Splat4DComponent = CreateDefaultSubobject<USplat4DComponent>(TEXT("Splat4DComponent"));
	RootComponent = Splat4DComponent;
}

void ASplat4DActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (Splat4DComponent)
	{
		Splat4DComponent->CurrentFrame = CurrentFrame;
	}
}


