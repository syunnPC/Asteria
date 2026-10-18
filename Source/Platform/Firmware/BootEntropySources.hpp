// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_FIRMWARE_BOOT_ENTROPY_SOURCES_HPP_INCLUDED
#define ASTERIA_PLATFORM_FIRMWARE_BOOT_ENTROPY_SOURCES_HPP_INCLUDED

#include "Foundation/Types.hpp"

// Processor entropy sources of the UEFI application (BootEntropyGathering).
// Each ISA implements them in Platform/<Isa>/Firmware/BootEntropySources.cpp;
// hosted tests link their own stand-ins.

namespace Asteria::Platform::Firmware
{
/// Reads the processor's free-running cycle counter: the TSC after LFENCE on
/// AMD64, ar.itc on IA-64.
/// Execution:
/// - Privilege level 0 before or after ExitBootServices; no side effects.
[[nodiscard]] Foundation::UInt64 ReadCycleCounter() noexcept;

/// Reads 64 bits from the processor's random-number instructions: RDSEED,
/// else RDRAND, where CPUID reports them, each retried a bounded number of
/// times while it reports that no value is ready. Returns false when the
/// processor has neither (always on IA-64) or no attempt succeeded. The value
/// is not health-tested here (BootEntropyCollector does that).
/// Execution:
/// - Bounded; no firmware call, no allocation.
[[nodiscard]] bool TryReadProcessorRandom(Foundation::UInt64& value) noexcept;
} // namespace Asteria::Platform::Firmware

#endif // ASTERIA_PLATFORM_FIRMWARE_BOOT_ENTROPY_SOURCES_HPP_INCLUDED
