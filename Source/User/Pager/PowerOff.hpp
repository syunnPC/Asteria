// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_PAGER_POWER_OFF_HPP_INCLUDED
#define ASTERIA_USER_PAGER_POWER_OFF_HPP_INCLUDED
#include "User/Pager/RootContext.hpp"
namespace Asteria::User::Pager
{
[[nodiscard]] bool PreparePowerTestVolume(ScenarioContext& context, Abi::SystemCallError& error) noexcept;
[[nodiscard]] bool RequestPower(ScenarioContext& context, bool reboot, Abi::SystemCallError& error) noexcept;
}
#endif
