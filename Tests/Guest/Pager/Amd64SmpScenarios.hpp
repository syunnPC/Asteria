// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_GUEST_PAGER_AMD64_SMP_SCENARIOS_HPP_INCLUDED
#define ASTERIA_TESTS_GUEST_PAGER_AMD64_SMP_SCENARIOS_HPP_INCLUDED
#include "User/Pager/RootContext.hpp"
namespace Asteria::User::Pager
{
[[nodiscard]] bool RunAmd64SmpScenarios(ScenarioContext& context, Abi::SystemCallError& error) noexcept;
}
#endif
