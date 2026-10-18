// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_PLATFORM_IA64_SYSTEM_IA64_SYSTEM_CALL_TRANSPORT_HPP_INCLUDED
#define ASTERIA_USER_PLATFORM_IA64_SYSTEM_IA64_SYSTEM_CALL_TRANSPORT_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::Platform::Ia64::System
{
inline constexpr Foundation::UInt64 Ia64SystemCallBreakImmediate{0x10'0000U};

/// Returns the stateless IA-64 break transport. The returned function pointer
/// is a P64 function descriptor and therefore requires its retained code and
/// image global pointer to remain mapped for every invocation.
[[nodiscard]] User::System::SystemCallTransport GetIa64SystemCallTransport() noexcept;

static_assert(Ia64SystemCallBreakImmediate == 0x10'0000U);
} // namespace Asteria::User::Platform::Ia64::System

#endif // ASTERIA_USER_PLATFORM_IA64_SYSTEM_IA64_SYSTEM_CALL_TRANSPORT_HPP_INCLUDED
