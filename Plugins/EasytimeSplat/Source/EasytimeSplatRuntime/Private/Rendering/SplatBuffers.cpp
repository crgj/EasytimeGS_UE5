/*
  Copyright (c) 2025 Easytime Technology Co., Ltd. See LICENSE.md.
*/

#include "Rendering/SplatBuffers.h"

#include "Misc/AssertionMacros.h"

namespace Easytime::Splat
{
void FSplatBufferBase::InitRHI(FRHICommandListBase& RHICmdList)
{
	// WDD-2026-04-06-ZeroSizeBufferGuard-UpgradeComment:RuntimeStability-v1
	// Some import/runtime edge cases can produce empty resource arrays while the
	// render path still expects a bindable SRV/UAV. Allocate one element to keep
	// RHI resource creation valid and avoid fatal zero-sized buffer asserts.
	const uint32 SafeSize = (Size > 0u) ? Size : Stride;

	FRHIResourceCreateInfo CreateInfo(*GetFriendlyName(), ResourceArray);
	VertexBufferRHI =
		RHICmdList.CreateBuffer(SafeSize, Usage, Stride, State, CreateInfo);
	check(VertexBufferRHI);

	FRHIViewDesc::FBufferSRV::FInitializer SRVCreateDesc =
		FRHIViewDesc::CreateBufferSRV();
	SRVCreateDesc.SetType(FRHIViewDesc::EBufferType::Typed).SetFormat(Format);
	ShaderResourceViewRHI =
		RHICmdList.CreateShaderResourceView(VertexBufferRHI, SRVCreateDesc);
	check(ShaderResourceViewRHI);

	if (bNeedsUAV)
	{
		FRHIViewDesc::FBufferUAV::FInitializer UAVCreateDesc =
			FRHIViewDesc::CreateBufferUAV();
		UAVCreateDesc.SetType(FRHIViewDesc::EBufferType::Typed)
			.SetFormat(Format);
		UnorderedAccessViewRHI = RHICmdList.CreateUnorderedAccessView(
			VertexBufferRHI, UAVCreateDesc);
		check(UnorderedAccessViewRHI);
	}
}
} // namespace Easytime::Splat

