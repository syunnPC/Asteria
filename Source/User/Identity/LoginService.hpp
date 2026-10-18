// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_IDENTITY_LOGIN_SERVICE_HPP_INCLUDED
#define ASTERIA_USER_IDENTITY_LOGIN_SERVICE_HPP_INCLUDED
#include "User/Identity/AuthService.hpp"
namespace Asteria::User::Identity
{
[[nodiscard]] bool InitializeLogin(const System::SystemCallClient&, const UInt64 (&)[Abi::ComponentBlockWords],
                                   UInt64 = 0U) noexcept;
[[nodiscard]] bool AdvanceLogin() noexcept;
void ClearLogin() noexcept;
[[nodiscard]] UInt64 ServeLogin(const System::SystemCallClient&, const UInt64 (&)[Abi::ComponentBlockWords],
                                UInt64 = 0U) noexcept;
}
#endif
