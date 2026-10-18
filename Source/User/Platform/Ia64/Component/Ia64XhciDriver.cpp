// SPDX-License-Identifier: Apache-2.0

#include "User/Component/XhciDriver.hpp"

namespace Asteria::User::Component
{
// IA-64 xHCI driver stub; returns ComponentBadStartExitCode.
Foundation::UInt64 ServeXhciDevice(const System::SystemCallClient&,
                                   const Foundation::UInt64 (&)[Abi::ComponentBlockWords], ComponentClock) noexcept
{
    return Abi::ComponentBadStartExitCode;
}
} // namespace Asteria::User::Component
