// SPDX-License-Identifier: Apache-2.0

#include "Platform/Firmware/BootEntropySources.hpp"

namespace Asteria::Platform::Firmware
{
Foundation::UInt64 ReadCycleCounter() noexcept
{
#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
    Foundation::UInt64 value{0U};
    __asm__ volatile("mov %0 = ar.itc\n\t;;" : "=r"(value) : : "memory");
    return value;
#else
    // Only the hosted analysis compiles this branch; nothing links it.
    return 0U;
#endif
}

bool TryReadProcessorRandom(Foundation::UInt64& value) noexcept
{
    // IA-64 has no random-number instruction.
    value = 0U;
    return false;
}
} // namespace Asteria::Platform::Firmware
