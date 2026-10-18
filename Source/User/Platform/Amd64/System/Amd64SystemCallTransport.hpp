// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_PLATFORM_AMD64_SYSTEM_AMD64_SYSTEM_CALL_TRANSPORT_HPP_INCLUDED
#define ASTERIA_USER_PLATFORM_AMD64_SYSTEM_AMD64_SYSTEM_CALL_TRANSPORT_HPP_INCLUDED

#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::Platform::Amd64::System
{
[[nodiscard]] User::System::SystemCallTransport GetAmd64SystemCallTransport() noexcept;
}

#endif // ASTERIA_USER_PLATFORM_AMD64_SYSTEM_AMD64_SYSTEM_CALL_TRANSPORT_HPP_INCLUDED
