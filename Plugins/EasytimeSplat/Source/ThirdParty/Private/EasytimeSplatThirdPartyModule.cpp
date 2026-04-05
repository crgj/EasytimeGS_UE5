/*
  Copyright (c) 2025 Easytime Technology Co., Ltd. See LICENSE.md.
*/

#include "Modules/ModuleManager.h"

namespace Easytime::Splat
{

class FEasytimeSplatThirdPartyModule final : public IModuleInterface
{
};

} // namespace Easytime::Splat

IMPLEMENT_MODULE(Easytime::Splat::FEasytimeSplatThirdPartyModule, EasytimeSplatThirdParty);

