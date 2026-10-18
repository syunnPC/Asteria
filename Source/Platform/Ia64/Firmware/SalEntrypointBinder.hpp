// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_FIRMWARE_SAL_ENTRYPOINT_BINDER_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_FIRMWARE_SAL_ENTRYPOINT_BINDER_HPP_INCLUDED

#include "Boot/BootInfo.hpp"
#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Platform/Ia64/Firmware/SalSystemTableParser.hpp"

namespace Asteria::Platform::Ia64::Firmware
{
enum class SalEntrypointBindError : Foundation::UInt32
{
    None,
    InvalidSalInformation,
    InvalidBootInfo,
    PalProcedureOutsidePalCode,
    SalProcedureOutsideRuntimeCode,
    SalGlobalDataOutsideRuntimeStorage,
    MissingRuntimeAttribute,
};

/// Owns value copies of every physical address and normalized memory-map region
/// needed by a later PAL/SAL call adapter. It contains no executable pointer.
struct SalEntrypointBinding
{
    Foundation::UInt64 PalProcedurePhysicalAddress;
    Foundation::UInt64 SalProcedurePhysicalAddress;
    Foundation::UInt64 SalGlobalDataPhysicalAddress;
    Boot::PhysicalMemoryRegion PalCodeRegion;
    Boot::PhysicalMemoryRegion SalCodeRegion;
    Boot::PhysicalMemoryRegion SalGlobalDataRegion;
};

/// Revalidates the consumed SAL entrypoint fields and binds each complete access
/// unit to exactly one normalized, firmware-owned physical-memory region.
/// Both inputs must remain stable throughout the call. Success authorizes only
/// later architecture-specific validation; it does not invoke firmware or form
/// a C++ function pointer from a physical integer.
class SalEntrypointBinder final
{
public:
    [[nodiscard]] static Foundation::Result<SalEntrypointBinding, SalEntrypointBindError>
    Bind(const SalSystemTableInformation& salInformation, const Boot::BootInfo& bootInfo) noexcept;

private:
    SalEntrypointBinder() = delete;
};

static_assert(sizeof(SalEntrypointBindError) == 4U);
static_assert(sizeof(SalEntrypointBinding) == 144U);
static_assert(alignof(SalEntrypointBinding) == 8U);
} // namespace Asteria::Platform::Ia64::Firmware

#endif // ASTERIA_PLATFORM_IA64_FIRMWARE_SAL_ENTRYPOINT_BINDER_HPP_INCLUDED
