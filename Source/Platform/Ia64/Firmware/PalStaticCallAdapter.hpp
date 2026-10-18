// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_FIRMWARE_PAL_STATIC_CALL_ADAPTER_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_FIRMWARE_PAL_STATIC_CALL_ADAPTER_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Platform/Ia64/Firmware/SalEntrypointBinder.hpp"
#include "Platform/Ia64/Memory/PalVirtualMemoryDecoder.hpp"

namespace Asteria::Platform::Ia64::Firmware
{
enum class PalStaticCallAdapterError : Foundation::UInt32
{
    None,
    InvalidBinding,
    UnsupportedProcessorState,
};

/// Invokes only the five buffer-free static PAL procedures consumed by the initial IA-64 bootstrap:
/// the four virtual-memory and purge queries and PAL_RSE_INFO. The raw SAL entry is a physical
/// bundle address, never a C++ function pointer or a region-number-derived alias. Preconditions not
/// representable by the value arguments: - execution is at CPL0 in the UEFI/SAL physical handoff
/// environment; - the binding is owned and remains stable for the complete adapter call; - PSR.bn
/// is one and at least 8 KiB of PAL stack space remains available; - the call is serialized on the
/// current processor and cannot be preempted; - native translation state has not replaced the
/// inherited SAL identity environment. The IA-64 assembly stub rejects visible incompatible PSR
/// state before the PAL branch, establishes PSR.i=PSR.ic=0 and enforced-lazy RSE operation for the
/// call, and restores the caller's PSR/RSE/P64 preserved state afterward.
class PalStaticCallAdapter final
{
public:
    [[nodiscard]] static Foundation::Result<Memory::PalStaticCallResult, PalStaticCallAdapterError>
    QueryMemoryAttributes(const SalEntrypointBinding& binding) noexcept;

    [[nodiscard]] static Foundation::Result<Memory::PalStaticCallResult, PalStaticCallAdapterError>
    QueryTranslationCachePurgeInformation(const SalEntrypointBinding& binding) noexcept;

    [[nodiscard]] static Foundation::Result<Memory::PalStaticCallResult, PalStaticCallAdapterError>
    QueryRseInformation(const SalEntrypointBinding& binding) noexcept;

    [[nodiscard]] static Foundation::Result<Memory::PalStaticCallResult, PalStaticCallAdapterError>
    QueryVirtualMemorySummary(const SalEntrypointBinding& binding) noexcept;

    [[nodiscard]] static Foundation::Result<Memory::PalStaticCallResult, PalStaticCallAdapterError>
    QueryVirtualMemoryPageSizes(const SalEntrypointBinding& binding) noexcept;

private:
    [[nodiscard]] static Foundation::Result<Memory::PalStaticCallResult, PalStaticCallAdapterError>
    Invoke(const SalEntrypointBinding& binding, Foundation::UInt64 procedureIndex) noexcept;

    PalStaticCallAdapter() = delete;
};

static_assert(sizeof(PalStaticCallAdapterError) == 4U);
static_assert(sizeof(Foundation::Result<Memory::PalStaticCallResult, PalStaticCallAdapterError>) == 40U);
static_assert(alignof(Foundation::Result<Memory::PalStaticCallResult, PalStaticCallAdapterError>) == 8U);
} // Namespace Asteria::Platform::Ia64::Firmware

#endif // ASTERIA_PLATFORM_IA64_FIRMWARE_PAL_STATIC_CALL_ADAPTER_HPP_INCLUDED
