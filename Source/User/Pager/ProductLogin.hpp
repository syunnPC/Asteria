// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_PAGER_PRODUCT_LOGIN_HPP_INCLUDED
#define ASTERIA_USER_PAGER_PRODUCT_LOGIN_HPP_INCLUDED
#include "User/Pager/RootContext.hpp"
namespace Asteria::User::Pager
{
void StartProductLogin(ScenarioContext&, const ComponentRecord&) noexcept;
[[nodiscard]] bool StepProductLogin(ScenarioContext&, Abi::SystemCallError&) noexcept;
// Bounded attention drain during desktop startup's supervisor waits.
void PollProductLogin(ScenarioContext&) noexcept;
}
#endif
