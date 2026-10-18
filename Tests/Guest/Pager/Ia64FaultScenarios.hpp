// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_GUEST_PAGER_IA64_FAULT_SCENARIOS_HPP_INCLUDED
#define ASTERIA_TESTS_GUEST_PAGER_IA64_FAULT_SCENARIOS_HPP_INCLUDED

#include "User/Pager/RootContext.hpp"

namespace Asteria::User::Pager
{
[[nodiscard]] bool RunIa64FaultScenarios(ScenarioContext& context, Abi::SystemCallError& error) noexcept;
}

#endif // ASTERIA_TESTS_GUEST_PAGER_IA64_FAULT_SCENARIOS_HPP_INCLUDED
