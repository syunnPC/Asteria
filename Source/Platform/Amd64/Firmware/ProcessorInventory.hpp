// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_FIRMWARE_PROCESSOR_INVENTORY_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_FIRMWARE_PROCESSOR_INVENTORY_HPP_INCLUDED

#include "Inventory/ReportText.hpp"

namespace Asteria::Platform::Amd64::Firmware
{
/// BSP-only read of CPUID and the current APIC mode. MSR access is gated by
/// CPUID and privilege; hosted callers receive 'unavailable' for APIC mode.
void ReportProcessor(Inventory::ReportText& text) noexcept;
} // namespace Asteria::Platform::Amd64::Firmware

#endif // ASTERIA_PLATFORM_AMD64_FIRMWARE_PROCESSOR_INVENTORY_HPP_INCLUDED
