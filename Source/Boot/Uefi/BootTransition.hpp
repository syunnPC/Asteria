// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_UEFI_BOOT_TRANSITION_HPP_INCLUDED
#define ASTERIA_BOOT_UEFI_BOOT_TRANSITION_HPP_INCLUDED

#include "Boot/Acpi/AcpiTableParser.hpp"
#include "Boot/BootInfo.hpp"
#include "Boot/Uefi/UefiTypes.hpp"
#include "Foundation/Types.hpp"

namespace Asteria::Boot::Uefi
{
constexpr Foundation::UInt32 MaximumExitBootServicesAttempts{4U};
constexpr Foundation::Size MinimumMemoryDescriptorSize{40U};

enum class BootTransitionError : Foundation::UInt32
{
    None,
    InvalidArgument,
    MemoryMapBufferTooSmall,
    GetMemoryMapFailed,
    ExitBootServicesFailed,
    ExitBootServicesRetryLimit,
    InvalidMemoryMap,
    AcpiTopologyRejected,
    MemoryMapPreparationRejected,
    EntropyRejected,
};

using FinalMemoryMapPreparation = bool (*)(BootInfo&) noexcept;

/// Bounded transaction preparation adapter. Caller owns its storage through
/// every retry; Prepare follows FinalMemoryMapPreparation's firmware contract.
class IFinalMemoryMapPreparation
{
public:
    [[nodiscard]] virtual bool Prepare(BootInfo& info) const noexcept = 0;

protected:
    ~IFinalMemoryMapPreparation() noexcept = default;
};

struct AcpiCaptureRequest
{
    const void* RsdpPointer;
    Acpi::AcpiTargetArchitecture TargetArchitecture;
    Foundation::UInt32 Reserved;
    Foundation::UInt32 BspApicId{0xFFFF'FFFFU};
};

struct BootTransitionResult
{
    BootTransitionError Error;
    Foundation::UInt32 ExitAttempts;
    Status FirmwareStatus;
    Foundation::Size RequiredMemoryMapBytes;
    Foundation::UInt8 BootServicesExited;
    Foundation::UInt8 Reserved[3U];
    /// With AcpiTopologyRejected, why the parser rejected the tables; None
    /// otherwise.
    Acpi::AcpiTableParseError AcpiError;

    [[nodiscard]] bool IsSuccessful() const noexcept
    {
        return Error == BootTransitionError::None;
    }

    [[nodiscard]] bool HasExitedBootServices() const noexcept
    {
        return BootServicesExited != 0U;
    }
};

/// Converts a variable-stride UEFI memory map into the fixed boot handoff format. - files, when
/// given, is the retained LoaderData boot-file receipt copied into every candidate before
/// preparation; without it BootInfo::Files is zero. Allocation: - Does not allocate. Returns: -
/// false when the map metadata, a descriptor range, or the normalized topology is invalid.
[[nodiscard]] bool NormalizeMemoryMap(const void* memoryMap, Foundation::Size memoryMapSize,
                                      Foundation::Size descriptorSize, Foundation::UInt32 descriptorVersion,
                                      BootInfo& bootInfo) noexcept;

/// The managed span in GiB of a UEFI memory map: the end of its highest write-back, non-runtime,
/// non-specific-purpose Conventional or boot-services range, rounded up to GiB; zero for a
/// malformed or empty map. Reads only map.
[[nodiscard]] Foundation::UInt64 ComputeManagedSpanGiB(const void* memoryMap, Foundation::Size memoryMapSize,
                                                       Foundation::Size descriptorSize) noexcept;

/// Retrieves the final map and terminates UEFI boot services with bounded retries. Firmware
/// callbacks: - Calls only GetMemoryMap and ExitBootServices. - After the first ExitBootServices
/// attempt, no other boot service may be called. Preparation: - acpiCapture is optional. When
/// supplied, its RSDP is parsed against every normalized candidate map before preparation runs. -
/// preparation is optional and runs after each successful normalization but before the
/// corresponding ExitBootServices call. - preparation may populate fixed handoff fields outside the
/// memory-region array. The complete BootInfo is reset before every retry. - preparation must not
/// call firmware or perform an operation that changes the firmware memory map. - false rejects the
/// candidate without attempting ExitBootServices. Entropy: - entropy is optional. When supplied, it
/// must be a Published record (GetBootEntropyState), or EntropyRejected is returned before any
/// firmware call; it is copied into BootInfo::Entropy of every normalized candidate before
/// preparation runs, so preparation and the kernel see the same seed on every attempt. Without it
/// BootInfo::Entropy stays zero, which no kernel accepts. - display, when given, is copied into
/// BootInfo::Display of every normalized candidate (the graphics mode the boot leaves for the
/// System Display Broker); without it BootInfo::Display stays zero. - files, when given, is the
/// retained LoaderData boot-file receipt copied into every candidate before preparation; without it
/// BootInfo::Files is zero. Allocation: - Does not allocate.
[[nodiscard]] BootTransitionResult ExitBootServicesAndCaptureMemoryMap(
    Handle imageHandle, BootServices& bootServices, void* memoryMapBuffer, Foundation::Size memoryMapCapacity,
    BootInfo& bootInfo, const AcpiCaptureRequest* acpiCapture = nullptr,
    FinalMemoryMapPreparation preparation = nullptr, const BootEntropy* entropy = nullptr,
    const BootDisplay* display = nullptr, const BootInventoryLocation* inventory = nullptr,
    const BootFileRegion* files = nullptr, const IFinalMemoryMapPreparation* preparationOwner = nullptr) noexcept;

static_assert(sizeof(BootTransitionError) == 4U);
static_assert(sizeof(AcpiCaptureRequest) == 24U);
static_assert(alignof(AcpiCaptureRequest) == 8U);
static_assert(sizeof(BootTransitionResult) == 32U);
static_assert(alignof(BootTransitionResult) == 8U);
} // Namespace Asteria::Boot::Uefi

#endif // ASTERIA_BOOT_UEFI_BOOT_TRANSITION_HPP_INCLUDED
