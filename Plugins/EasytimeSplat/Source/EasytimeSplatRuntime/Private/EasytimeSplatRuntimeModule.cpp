/*
  Copyright (c) 2025 Easytime Technology Co., Ltd. See LICENSE.md.
*/

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ShaderCore.h"

namespace Easytime::Splat
{

/**
 * Provides 3DGS rendering support, to both games and editor.
 */
class FEasytimeSplatRuntimeModule final : public IModuleInterface
{
	virtual void StartupModule() override
	{
		// Register shader adapters.
		AddShaderSourceDirectoryMapping(
			TEXT("/Plugin/EasytimeSplat"),
			FPaths::Combine(
				IPluginManager::Get()
					.FindPlugin(TEXT("EasytimeSplat"))
					->GetBaseDir(),
				TEXT("Shaders")));

		// Register open-source shaders.
		AddShaderSourceDirectoryMapping(
			TEXT("/Plugin/EasytimeSplat/ThirdParty"),
			FPaths::Combine(
				IPluginManager::Get()
					.FindPlugin(TEXT("EasytimeSplat"))
					->GetBaseDir(),
				TEXT("Source/ThirdParty/Shaders")));
	}
};

} // namespace Easytime::Splat

IMPLEMENT_MODULE(Easytime::Splat::FEasytimeSplatRuntimeModule, EasytimeSplatRuntime);

