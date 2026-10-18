// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_PAGER_TOOLKIT_LAUNCH_HPP_INCLUDED
#define ASTERIA_USER_PAGER_TOOLKIT_LAUNCH_HPP_INCLUDED
#include "User/Pager/RootContext.hpp"
namespace Asteria::User::Pager
{
// Root-started toolkit preview, enabled by its BOOT.CFG module name.
void StartToolkitPreview(ScenarioContext& context, ComponentRecord& compositor,
                         System::CapabilityHandle reports) noexcept;
void StepToolkitPreview(ScenarioContext& context) noexcept;
void StopToolkitPreview(ScenarioContext& context) noexcept;
void CollectToolkitPreview(ScenarioContext& context) noexcept;
}
#endif
