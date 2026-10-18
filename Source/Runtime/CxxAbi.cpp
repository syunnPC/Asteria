// SPDX-License-Identifier: Apache-2.0

#include "Runtime/CxxAbi.hpp"

#include "Platform/FatalTrap.hpp"

extern "C" [[noreturn]] void __cxa_pure_virtual() noexcept
{
    Asteria::Platform::FatalTrap();
}
