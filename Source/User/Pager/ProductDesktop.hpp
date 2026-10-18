// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_PAGER_PRODUCT_DESKTOP_HPP_INCLUDED
#define ASTERIA_USER_PAGER_PRODUCT_DESKTOP_HPP_INCLUDED
#include "User/Pager/RootContext.hpp"
namespace Asteria::User::Pager
{
// Root-service control of the product desktop lifecycle.
[[nodiscard]] bool FocusProductDesktop() noexcept;
[[nodiscard]] bool StopProductDesktop() noexcept;
[[nodiscard]] bool ProductDesktopActive() noexcept;
void ConfigureProductDesktop(ScenarioContext& context) noexcept;
void FallbackProductDesktop(ScenarioContext& context, const char* reason) noexcept;
void StartProductDesktop(ScenarioContext& context, const ComponentRecord& input) noexcept;
[[nodiscard]] bool StepProductDesktop(ScenarioContext& context, Abi::SystemCallError& error) noexcept;
}
#endif
