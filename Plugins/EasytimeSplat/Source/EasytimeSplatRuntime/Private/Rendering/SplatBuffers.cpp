/*
  Copyright (c) 2025 Easytime Technology Co., Ltd. See LICENSE.md.
*/

#include "Rendering/SplatBuffers.h"

#include "Misc/AssertionMacros.h"

namespace Easytime::Splat
{
void FSplatBufferBase::InitRHI(FRHICommandListBase& RHICmdList)
{
	FRHIResourceCreateInfo CreateInfo(*GetFriendlyName(), ResourceArray);
	VertexBufferRHI =
		RHICmdList.CreateBuffer(Size, Usage, Stride, State, CreateInfo);
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

