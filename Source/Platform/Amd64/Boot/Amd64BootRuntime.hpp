// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_BOOT_AMD64_BOOT_RUNTIME_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_BOOT_AMD64_BOOT_RUNTIME_HPP_INCLUDED

#include "Boot/BootInfo.hpp"
#include "Executable/Elf/Elf64Image.hpp"
#include "Kernel/Memory/InitialMappingPlan.hpp"
#include "Platform/Amd64/Boot/Amd64RuntimePoolLayout.hpp"
#include "Platform/Amd64/Memory/InitialPagingCapabilityCapture.hpp"
#include "Platform/Firmware/PciResource.hpp"

namespace Asteria::Platform::Amd64::Bootstrap
{
/// The shared-root shootdown diagnostic: once every processor runs on the
/// shared kernel root, the BSP stores SharedProbeValue in pool page
/// SharedProbePage and maps that page at SharedProbeVirtualAddress into the
/// shared root only, with a TLB shootdown that every processor acknowledges.
/// Each application processor then reads the word through the new mapping.
inline constexpr Foundation::UInt64 SharedProbeVirtualAddress{0x6000'0000U};
inline constexpr Foundation::UInt64 SharedProbeValue{0x4153'5445'544C'4234ULL};

struct Amd64BootRuntimeInput
{
    const ::Asteria::Boot::BootInfo* BootInformation;
    const Kernel::Memory::InitialMappingPlan* InitialMappings;
    const Memory::PagingCapabilities* Capabilities;
    Foundation::UInt64 PoolPhysicalStart;
    Foundation::UInt64 StartupPhysicalStart;
    /// The metadata arena whose receipt ValidateMetadataArenaReceipt accepted, and its pages.
    Foundation::UInt64 MetadataArenaPhysicalStart;
    Foundation::UInt64 MetadataArenaPages;
    ::Asteria::Executable::Elf::Elf64ByteView UserImage;
    /// The boot pager image.
    ::Asteria::Executable::Elf::Elf64ByteView PagerImage;
    /// The spinner image of the test firmware; empty (no spinner) otherwise.
    ::Asteria::Executable::Elf::Elf64ByteView SpinnerImage;
    Platform::Firmware::AhciPciResource AhciResource{};
    /// The assignable device's captured BAR, or zero.
    Platform::Firmware::AhciPciResource DeviceResource{};
    /// The NIC's captured register BAR, or zero.
    Platform::Firmware::AhciPciResource NicResource{};
};

/// Before ExitBootServices only. The final normalized map must retain both
/// allocator receipts as non-runtime, write-back LoaderData. Successful retry
/// validation retains the same prepared owner and never restages user bytes.
[[nodiscard]] bool PrepareAmd64BootRuntime(const Amd64BootRuntimeInput& input) noexcept;
/// Firmware identity only; an invalid APIC returns the broadcast sentinel.
[[nodiscard]] Foundation::UInt32 GetAmd64FirmwareApicId() noexcept;
[[nodiscard]] Foundation::UInt64 GetAmd64BootTscTicksPerMicrosecond() noexcept;
[[nodiscard]] const char* GetAmd64BootRuntimeRejection() noexcept;
extern "C" [[noreturn]] void AsteriaAmd64InitialRuntimeEntry() noexcept;
} // Namespace Asteria::Platform::Amd64::Bootstrap

#endif // ASTERIA_PLATFORM_AMD64_BOOT_AMD64_BOOT_RUNTIME_HPP_INCLUDED
