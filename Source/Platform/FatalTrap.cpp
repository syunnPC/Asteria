// SPDX-License-Identifier: Apache-2.0

#include "Platform/FatalTrap.hpp"

namespace Asteria::Platform
{
[[noreturn]] void FatalTrap() noexcept
{
    __builtin_trap();
}
} // namespace Asteria::Platform
