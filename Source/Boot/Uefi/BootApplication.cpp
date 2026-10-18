// SPDX-License-Identifier: Apache-2.0

#include "Boot/Uefi/BootReport.hpp"
#include "Boot/Uefi/BootConsole.hpp"
#include "Boot/Uefi/GraphicsModeChoice.hpp"
#include "Boot/Uefi/LoaderDiagnostic.hpp"
#include "Boot/Uefi/BootFileLoader.hpp"
#include "Boot/Uefi/UefiTypes.hpp"

#include "Boot/Acpi/AcpiTableParser.hpp"
#include "Boot/BootInfo.hpp"
#include "Boot/Uefi/BootEntropyGathering.hpp"
#include "Boot/Uefi/BootTransition.hpp"
#include "Boot/Uefi/TableValidation.hpp"
#include "Foundation/Compiler.hpp"
#include "Kernel/Memory/BootstrapPageAllocator.hpp"
#include "Platform/Firmware/Diagnostic.hpp"
#include "Platform/Firmware/AhciPciResourceCapture.hpp"

#if !defined(ASTERIA_IA64_NATIVE_PREEMPTION_PROBE)
#define ASTERIA_IA64_NATIVE_PREEMPTION_PROBE 0
#endif

#if !defined(ASTERIA_IA64_SYNTHETIC_USER_PROBE)
#define ASTERIA_IA64_SYNTHETIC_USER_PROBE 0
#endif

#if (ASTERIA_IA64_NATIVE_PREEMPTION_PROBE != 0) && (ASTERIA_IA64_NATIVE_PREEMPTION_PROBE != 1)
#error "ASTERIA_IA64_NATIVE_PREEMPTION_PROBE must be zero or one"
#endif

#if (ASTERIA_IA64_SYNTHETIC_USER_PROBE != 0) && (ASTERIA_IA64_SYNTHETIC_USER_PROBE != 1)
#error "ASTERIA_IA64_SYNTHETIC_USER_PROBE must be zero or one"
#endif

#if (ASTERIA_IA64_NATIVE_PREEMPTION_PROBE != 0) && (ASTERIA_IA64_SYNTHETIC_USER_PROBE == 0)
#error "native preemption diagnostics require the synthetic user probe"
#endif

#if defined(ASTERIA_TARGET_AMD64)
#include "Platform/Amd64/Firmware/Amd64LoadedImageLayout.hpp"
#include "Platform/Amd64/Time/Amd64TscClock.hpp"
#include "Platform/Amd64/Memory/BootstrapMappingPlan.hpp"
#include "Platform/Amd64/Memory/InitialAddressSpacePreparation.hpp"
#include "Platform/Amd64/Firmware/DiagnosticSinkSelection.hpp"
#include "Platform/Amd64/Memory/InitialPagingCapabilityCapture.hpp"
#elif defined(ASTERIA_TARGET_IA64)
#include "Platform/Ia64/Firmware/Ia64DebugUart.hpp"
#include "Platform/Ia64/Firmware/Ia64LoadedImageLayout.hpp"
#include "Platform/Ia64/Firmware/Ia64ImageRelocation.hpp"
#include "Platform/Ia64/Firmware/SalSystemTableCapture.hpp"
#include "Platform/Ia64/Time/Ia64LocalTimer.hpp"
#endif

#include "Boot/BootInfoValidation.hpp"
#include "Runtime/Memory.hpp"
#include "Boot/Uefi/KernelLoader.hpp"

namespace Asteria::Boot::Uefi
{
constinit KernelFileReceipt g_bootKernelFile{};
#if defined(ASTERIA_TARGET_AMD64)
Platform::Firmware::AhciPciResource g_amd64AhciResource{};
Platform::Firmware::AhciPciResource g_amd64DeviceResource{};
Platform::Firmware::AhciPciResource g_amd64NicResource{};
Platform::Amd64::Firmware::Amd64LoadedImageLayout g_amd64LoadedImageLayout{};
Platform::Amd64::Memory::PagingCapabilities g_amd64InitialPagingCapabilities{};
constexpr Foundation::UInt32 Amd64AssignableIdentifier{0x11E8'1234U};
constexpr Foundation::UInt8 Amd64AssignableBar{0U};
// The e1000e (82574L) NIC and its register BAR.
constexpr Foundation::UInt32 Amd64NicIdentifier{0x10D3'8086U};
constexpr Foundation::UInt8 Amd64NicBar{0U};

#elif defined(ASTERIA_TARGET_IA64)
Platform::Ia64::Firmware::SalSystemTableInformation g_ia64SalSystemTableInformation{};
Platform::Ia64::Firmware::Ia64LoadedImageLayout g_ia64LoadedImageLayout{};
Platform::Firmware::AhciPciResource g_ia64AhciPciResource{};
Foundation::UInt64 g_ia64ItcTicksPerMicrosecond{0U};
Platform::Ia64::Time::Ia64LocalTimer g_ia64ApWaitClock;
#endif
// Retained, pointer-free firmware handoff consumed by the kernel bootstrap.
alignas(8) BootInfo g_bootInfo;
constinit BootFileRegion g_bootFiles{};
constinit BootTrustPolicy g_bootTrust{};
// BOOT.CFG's `lowmemory` blocks, zero without the key.
constinit Foundation::UInt64 g_bootLowMemoryBlocks{0U};
constinit Foundation::UInt64 g_bootWatchdogSeconds{Abi::WatchdogDefaultSeconds};
constinit Foundation::UInt64 g_bootSessionMode{0U};
constinit Foundation::UInt64 g_bootLoginEnabled{0U};
// The managed span of the memory map CheckMemoryMap read, in GiB.
constinit Foundation::UInt64 g_managedSpanGiB{0U};
constinit DisplayPolicy g_displayPolicy{};
constinit char g_bootFileFailure[96U]{};

namespace
{
constexpr Foundation::Size MemoryMapCapacityBytes{64U * 1024U};
constexpr Foundation::Size MaximumConfigurationTables{4096U};
constexpr Foundation::Size MinimumDesktopMargin{8U};
constexpr Foundation::Size DesktopShadowOffset{8U};

constexpr Guid LoadedImageProtocolGuid{
    .Data1 = 0x5B1B31A1U,
    .Data2 = 0x9562U,
    .Data3 = 0x11D2U,
    .Data4 = {0x8EU, 0x3FU, 0x00U, 0xA0U, 0xC9U, 0x69U, 0x72U, 0x3BU},
};
alignas(8) Foundation::UInt8 g_memoryMap[MemoryMapCapacityBytes];
const void* g_acpiRsdpPointer;
// The gathered boot seed until the transition publishes it in g_bootInfo.
constinit BootEntropy g_bootEntropy{};
// The graphics mode the boot leaves for the System Display Broker; zero until the desktop is drawn
// on a linear framebuffer.
constinit BootDisplay g_bootDisplay{};
constinit Kernel::Memory::BootstrapPageAllocator g_bootstrapPageAllocator;
#if defined(ASTERIA_TARGET_IA64)
alignas(8) Foundation::UInt8 g_ia64SalSystemTableSnapshot[Platform::Ia64::Firmware::MaximumSalSystemTableSnapshotBytes];
// Budget of every BSP wait for AP work: rendezvous takeover, runtime start

#endif
alignas(8) volatile Foundation::UInt64 g_bssProbe;

class BootContext final
{
public:
    BootContext(SystemTable& systemTable, SimpleTextOutputProtocol& console) noexcept
        : System{systemTable}, Console{console}
    {
    }

    SystemTable& System;
    SimpleTextOutputProtocol& Console;
    Foundation::UInt32 FailureCount{0U};
    const char* LastFailure{nullptr};
};

[[nodiscard]] bool IsAcpi20Guid(const Guid& guid) noexcept
{
    return (guid.Data1 == 0x8868E871U) && (guid.Data2 == 0xE4F1U) && (guid.Data3 == 0x11D3U) &&
           (guid.Data4[0U] == 0xBCU) && (guid.Data4[1U] == 0x22U) && (guid.Data4[2U] == 0x00U) &&
           (guid.Data4[3U] == 0x80U) && (guid.Data4[4U] == 0xC7U) && (guid.Data4[5U] == 0x3CU) &&
           (guid.Data4[6U] == 0x88U) && (guid.Data4[7U] == 0x81U);
}

[[nodiscard]] bool IsAcpi10Guid(const Guid& guid) noexcept
{
    return (guid.Data1 == 0xEB9D2D30U) && (guid.Data2 == 0x2D88U) && (guid.Data3 == 0x11D3U) &&
           (guid.Data4[0U] == 0x9AU) && (guid.Data4[1U] == 0x16U) && (guid.Data4[2U] == 0x00U) &&
           (guid.Data4[3U] == 0x90U) && (guid.Data4[4U] == 0x27U) && (guid.Data4[5U] == 0x3FU) &&
           (guid.Data4[6U] == 0xC1U) && (guid.Data4[7U] == 0x4DU);
}

[[nodiscard]] bool WriteBootAscii(SimpleTextOutputProtocol& console, const char* const text) noexcept
{
    WriteLoaderMirroredAscii(text);
    return WriteBootConsoleAscii(console, text);
}

void WriteHex64(SimpleTextOutputProtocol& console, const Foundation::UInt64 value) noexcept
{
    constexpr char Digits[]{"0123456789ABCDEF"};
    char text[19U]{'0', 'x'};
    for (Foundation::UInt32 digit{0U}; digit < 16U; ++digit)
    {
        const Foundation::UInt32 shift = (15U - digit) * 4U;
        const Foundation::Size index = static_cast<Foundation::Size>((value >> shift) & 0x0FULL);
        text[digit + 2U] = Digits[index];
    }
    text[18U] = '\0';
    static_cast<void>(WriteBootAscii(console, text));
}

#if defined(ASTERIA_TARGET_IA64)
void WriteIa64DiagnosticHex(const Foundation::UInt64 value, const Foundation::UInt32 digits) noexcept
{
    constexpr char Digits[]{"0123456789ABCDEF"};
    char text[17U]{};
    for (Foundation::UInt32 digit{0U}; digit < digits; ++digit)
    {
        const Foundation::UInt32 shift = (digits - digit - 1U) * 4U;
        const Foundation::Size index = static_cast<Foundation::Size>((value >> shift) & 0x0FULL);
        text[digit] = Digits[index];
    }
    WriteLoaderDiagnosticAscii(text);
}

/// Records what the PCI ECAM scan found: its first function and the function
/// bound as the AHCI controller. The host compares them with QEMU's view.

#endif
void ReportFailure(BootContext& context, const char* const name) noexcept
{
    ++context.FailureCount;
    context.LastFailure = name;
    static_cast<void>(WriteBootAscii(context.Console, "ASTERIA:"));
    static_cast<void>(WriteBootAscii(context.Console, name));
    static_cast<void>(WriteBootAscii(context.Console, ":FAIL\r\n"));
}

/// Reports one pre-exit stage: `ASTERIA:STAGE:<name>` when it completed,
/// `ASTERIA:<name>:FAIL` (counted) when it did not.
void ReportCheck(BootContext& context, const char* const name, const bool passed) noexcept
{
    if (!passed)
    {
        ReportFailure(context, name);
        return;
    }
    static_cast<void>(WriteBootAscii(context.Console, "ASTERIA:STAGE:"));
    static_cast<void>(WriteBootAscii(context.Console, name));
    static_cast<void>(WriteBootAscii(context.Console, "\r\n"));
}

#if defined(ASTERIA_TARGET_IA64)
void ReportIa64OptionalAhciResourceCapture(BootContext& context,
                                           const Platform::Firmware::AhciPciResourceCaptureFailure& failure) noexcept
{
    if (failure.Error == Platform::Firmware::AhciPciResourceCaptureError::None)
    {
        static_cast<void>(WriteBootAscii(context.Console, "ASTERIA:STAGE:IA64-AHCI-PCI-RESOURCE\r\n"));
        return;
    }

    static_cast<void>(WriteBootAscii(context.Console, "ASTERIA:IA64-AHCI-PCI-RESOURCE:UNAVAILABLE:ERROR="));
    WriteHex64(context.Console, static_cast<Foundation::UInt32>(failure.Error));
    static_cast<void>(WriteBootAscii(context.Console, ":DESCRIPTOR="));
    WriteHex64(context.Console, static_cast<Foundation::UInt32>(failure.Descriptor));
    static_cast<void>(WriteBootAscii(context.Console, ":CLEANUP="));
    WriteHex64(context.Console, static_cast<Foundation::UInt32>(failure.Cleanup));
    static_cast<void>(WriteBootAscii(context.Console, "\r\n"));
}
#endif

// Where a failure report goes: the boot console (WriteBootAscii also
// writes the debug port) while boot services may be used, otherwise the
// debug port only.
enum class ReportDestination : Foundation::UInt8
{
    ConsoleAndDebugPort,
    DebugPort,
};

void WriteBootLine(BootContext& context, const ReportDestination destination, const char* const text) noexcept
{
    switch (destination)
    {
    case ReportDestination::ConsoleAndDebugPort:
        static_cast<void>(WriteBootAscii(context.Console, text));
        return;
    case ReportDestination::DebugPort:
        break;
    }
    WriteLoaderDiagnosticAscii(text);
}

void WriteBootHex(BootContext& context, const ReportDestination destination, const Foundation::UInt64 value) noexcept
{
    constexpr char Digits[]{"0123456789ABCDEF"};
    char text[19U]{'0', 'x'};
    for (Foundation::UInt32 digit{0U}; digit < 16U; ++digit)
    {
        text[digit + 2U] = Digits[(value >> ((15U - digit) * 4U)) & 0x0FU];
    }
    text[18U] = '\0';
    WriteBootLine(context, destination, text);
}

[[nodiscard]] const char* GetBootTransitionFailureDetail(const BootTransitionError error) noexcept
{
    const char* detail{"ASTERIA:EXIT-BOOT-SERVICES:DETAIL:INTERNAL\r\n"};
    switch (error)
    {
    case BootTransitionError::InvalidArgument:
        detail = "ASTERIA:EXIT-BOOT-SERVICES:DETAIL:INVALID-ARGUMENT\r\n";
        break;
    case BootTransitionError::MemoryMapBufferTooSmall:
        detail = "ASTERIA:EXIT-BOOT-SERVICES:DETAIL:MAP-BUFFER\r\n";
        break;
    case BootTransitionError::GetMemoryMapFailed:
        detail = "ASTERIA:EXIT-BOOT-SERVICES:DETAIL:GET-MAP\r\n";
        break;
    case BootTransitionError::ExitBootServicesFailed:
        detail = "ASTERIA:EXIT-BOOT-SERVICES:DETAIL:FIRMWARE-REJECTED\r\n";
        break;
    case BootTransitionError::ExitBootServicesRetryLimit:
        detail = "ASTERIA:EXIT-BOOT-SERVICES:DETAIL:RETRY-LIMIT\r\n";
        break;
    case BootTransitionError::InvalidMemoryMap:
        detail = "ASTERIA:EXIT-BOOT-SERVICES:DETAIL:INVALID-MAP\r\n";
        break;
    case BootTransitionError::AcpiTopologyRejected:
        detail = "ASTERIA:EXIT-BOOT-SERVICES:DETAIL:ACPI\r\n";
        break;
    case BootTransitionError::MemoryMapPreparationRejected:
        detail = "ASTERIA:EXIT-BOOT-SERVICES:DETAIL:PREPARATION\r\n";
        break;
    case BootTransitionError::EntropyRejected:
        detail = "ASTERIA:EXIT-BOOT-SERVICES:DETAIL:ENTROPY\r\n";
        break;
    case BootTransitionError::None:
        break;
    }
    return detail;
}

#if defined(ASTERIA_TARGET_AMD64)
// What refused the AMD64 TABLE step: the paging capability capture (an
// absent NX is the one a firmware setup option causes), the processor-state
// capture, or the address-space preparation's failure record.
void ReportAmd64TableRejection(BootContext& context, const ReportDestination destination) noexcept
{
    using Platform::Amd64::Memory::InitialPagingCapabilityCaptureError;
    WriteBootLine(context, destination, "  paging-capability-error=");
    WriteBootHex(context, destination,
                 static_cast<Foundation::UInt32>(ReadKernelQuery(KernelQuery::PagingCapabilityError)));
    WriteBootLine(context, destination, " processor-state-error=");
    WriteBootHex(context, destination,
                 static_cast<Foundation::UInt32>(ReadKernelQuery(KernelQuery::ProcessorStateCaptureError)));
    WriteBootLine(context, destination, "\r\n");
    if (ReadKernelQuery(KernelQuery::PagingCapabilityError) ==
        static_cast<Foundation::UInt64>(InitialPagingCapabilityCaptureError::ExecuteDisableUnavailable))
    {
        WriteBootLine(context, destination,
                      "  The processor reports no execute-disable (NX) support: enable \"Execution Prevention\"\r\n"
                      "  (\"Execute Disable Bit\") in the firmware setup.\r\n");
    }
    if (ReadKernelQuery(KernelQuery::PreparationFailed) == 0U)
    {
        return;
    }
    WriteBootLine(context, destination, "  preparation-error=");
    WriteBootHex(context, destination, static_cast<Foundation::UInt32>(ReadKernelQuery(KernelQuery::PreparationError)));
    WriteBootLine(context, destination, " capture=");
    WriteBootHex(context, destination, static_cast<Foundation::UInt32>(ReadKernelQuery(KernelQuery::CaptureError)));
    WriteBootLine(context, destination, "\r\n  page-table=");
    WriteBootHex(context, destination, static_cast<Foundation::UInt32>(ReadKernelQuery(KernelQuery::PageTableError)));
    WriteBootLine(context, destination, " build=");
    WriteBootHex(context, destination,
                 static_cast<Foundation::UInt32>(ReadKernelQuery(KernelQuery::PageTableBuildError)));
    WriteBootLine(context, destination, "\r\n  verification=");
    WriteBootHex(context, destination,
                 static_cast<Foundation::UInt32>(ReadKernelQuery(KernelQuery::PageTableVerificationError)));
    WriteBootLine(context, destination, " mapping=");
    WriteBootHex(context, destination, static_cast<Foundation::UInt32>(ReadKernelQuery(KernelQuery::MappingError)));
    WriteBootLine(context, destination, "\r\n  mapping-index=");
    WriteBootHex(context, destination, ReadKernelQuery(KernelQuery::MappingIndex));
    WriteBootLine(context, destination, "\r\n");
    if (ReadKernelQuery(KernelQuery::PreparationError) ==
        static_cast<Foundation::UInt64>(
            Platform::Amd64::Memory::InitialAddressSpacePreparationError::InvalidMemoryTypeConfiguration))
    {
        // The MTRRs the kernel's pages failed against: the capability, the
        // default type and each variable range.
        WriteBootLine(context, destination, "  mtrr-capability=");
        WriteBootHex(context, destination, ReadKernelQuery(KernelQuery::MtrrCapability));
        WriteBootLine(context, destination, " mtrr-default-type=");
        WriteBootHex(context, destination, ReadKernelQuery(KernelQuery::MtrrDefaultType));
        WriteBootLine(context, destination, "\r\n");
        for (Foundation::UInt32 index{0U}; index < ReadKernelQuery(KernelQuery::VariableMtrrCount) && index < 16U;
             ++index)
        {
            WriteBootLine(context, destination, "  mtrr-base=");
            WriteBootHex(context, destination, ReadKernelQuery(KernelQuery::VariableMtrrBase, index));
            WriteBootLine(context, destination, " mtrr-mask=");
            WriteBootHex(context, destination, ReadKernelQuery(KernelQuery::VariableMtrrMask, index));
            WriteBootLine(context, destination, "\r\n");
        }
    }
}
#endif

/// Reports a final-map transaction that did not hand over: the step that
/// rejected the map, the transaction's failure and its detail. While no
/// ExitBootServices call has been made the firmware console still works and
/// the report goes there too; after a call (UEFI then allows only
/// GetMemoryMap), only to the debug port.
[[gnu::noinline]] void ReportBootTransitionFailure(BootContext& context, const BootTransitionResult& transition,
                                                   const ReportDestination destination) noexcept
{
#if defined(ASTERIA_TARGET_AMD64)
    const char* const runtimeRejection = ReadKernelDiagnosticString(KernelQuery::RuntimeRejectionAddress);
    if (transition.Error == BootTransitionError::MemoryMapPreparationRejected && runtimeRejection != nullptr)
    {
        WriteBootLine(context, destination, "ASTERIA:AMD64-RUNTIME-PREPARE:FAIL:");
        WriteBootLine(context, destination, runtimeRejection);
        WriteBootLine(context, destination, "\r\n");
        const Foundation::UInt64 tried = ReadKernelQuery(KernelQuery::TscSourcesTried);
        const Foundation::UInt64 present = ReadKernelQuery(KernelQuery::TscSourcesPresent);
        if (tried != 0U && runtimeRejection[0U] == 'T')
        {
            WriteBootLine(context, destination, "ASTERIA:AMD64-TSC:FAIL:PRESENT=");
            for (Foundation::UInt32 source{1U}; source <= 6U; ++source)
            {
                if ((present & (1U << source)) != 0U)
                {
                    WriteBootLine(context, destination,
                                  Platform::Amd64::Time::TscSourceName(
                                      static_cast<Platform::Amd64::Time::Amd64TscSource>(source)));
                    WriteBootLine(context, destination, ",");
                }
            }
            WriteBootLine(context, destination, ":TRIED=");
            for (Foundation::UInt32 source{1U}; source <= 6U; ++source)
            {
                if ((tried & (1U << source)) != 0U)
                {
                    WriteBootLine(context, destination,
                                  Platform::Amd64::Time::TscSourceName(
                                      static_cast<Platform::Amd64::Time::Amd64TscSource>(source)));
                    WriteBootLine(context, destination, ",");
                }
            }
            WriteBootLine(context, destination, "\r\n");
        }
    }
    const char* const rejection = ReadKernelDiagnosticString(KernelQuery::FinalMapRejectionAddress);
    if ((transition.Error == BootTransitionError::MemoryMapPreparationRejected) && (rejection != nullptr))
    {
        WriteBootLine(context, destination, rejection);
    }
#endif
    WriteBootLine(context, destination, "ASTERIA:EXIT-BOOT-SERVICES:FAIL\r\n");
    WriteBootLine(context, destination, GetBootTransitionFailureDetail(transition.Error));
    switch (transition.Error)
    {
    case BootTransitionError::AcpiTopologyRejected:
        WriteBootLine(context, destination, "  acpi-error=");
        WriteBootHex(context, destination, static_cast<Foundation::UInt32>(transition.AcpiError));
        WriteBootLine(context, destination, "\r\n");
        break;
    case BootTransitionError::GetMemoryMapFailed:
    case BootTransitionError::MemoryMapBufferTooSmall:
    case BootTransitionError::ExitBootServicesFailed:
        WriteBootLine(context, destination, "  firmware-status=");
        WriteBootHex(context, destination, static_cast<Foundation::UInt64>(transition.FirmwareStatus));
        WriteBootLine(context, destination, " map-bytes=");
        WriteBootHex(context, destination, static_cast<Foundation::UInt64>(transition.RequiredMemoryMapBytes));
        WriteBootLine(context, destination, "\r\n");
        break;
    case BootTransitionError::MemoryMapPreparationRejected:
#if defined(ASTERIA_TARGET_AMD64)
        if (ReadKernelQuery(KernelQuery::TableRejected))
        {
            ReportAmd64TableRejection(context, destination);
        }
#endif
        break;
    case BootTransitionError::None:
    case BootTransitionError::InvalidArgument:
    case BootTransitionError::ExitBootServicesRetryLimit:
    case BootTransitionError::InvalidMemoryMap:
    case BootTransitionError::EntropyRejected:
        break;
    }
}

[[nodiscard, gnu::noinline]] bool GatherHandoffEntropy(BootContext& context) noexcept
{
    const auto gathered = GatherBootEntropy(context.System);
    if (!gathered.HasValue())
    {
        ReportFailure(context, "BOOT-ENTROPY");
        return false;
    }
    g_bootEntropy = gathered.GetValue();
    static_cast<void>(WriteBootAscii(context.Console, "ASTERIA:BOOT-ENTROPY:SOURCES:"));
    WriteHex64(context.Console, g_bootEntropy.Sources);
    static_cast<void>(WriteBootAscii(context.Console, "\r\n"));
    return true;
}

[[nodiscard]] bool IsBssZeroInitialized() noexcept
{
    return IsZeroInitializedProbe(g_bssProbe);
}

[[nodiscard]] bool CheckLoadedImage(BootContext& context, const Handle imageHandle, Foundation::UInt64& imageBase,
                                    Foundation::UInt64& imageSize) noexcept
{
    void* interface{nullptr};
    Guid protocolGuid = LoadedImageProtocolGuid;
    const Status status =
        context.System.BootServicesTable->HandleProtocolFunction(imageHandle, &protocolGuid, &interface);
    if ((status != Success) || (interface == nullptr))
    {
        return false;
    }

    const auto* const loadedImage = static_cast<const LoadedImageProtocol*>(interface);
    const bool valid = (loadedImage->ImageSystemTable == &context.System) && (loadedImage->ImageBase != nullptr) &&
                       (loadedImage->ImageSize != 0U);
    if (valid)
    {
        imageBase = static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(loadedImage->ImageBase));
        imageSize = loadedImage->ImageSize;
    }
    return valid;
}

[[nodiscard]] bool IsWholeDescriptorArray(const Foundation::Size mapSize,
                                          const Foundation::Size descriptorSize) noexcept
{
    if (descriptorSize == 0U)
    {
        return false;
    }

    Foundation::Size remaining = mapSize;
    while (remaining >= descriptorSize)
    {
        remaining -= descriptorSize;
    }
    return remaining == 0U;
}

[[nodiscard]] bool CheckMemoryMap(BootContext& context) noexcept
{
    Foundation::Size mapSize{sizeof(g_memoryMap)};
    Foundation::Size mapKey{0U};
    Foundation::Size descriptorSize{0U};
    Foundation::UInt32 descriptorVersion{0U};
    const Status status = context.System.BootServicesTable->GetMemoryMapFunction(&mapSize, g_memoryMap, &mapKey,
                                                                                 &descriptorSize, &descriptorVersion);

    static_cast<void>(WriteBootAscii(context.Console, "  memory-map-bytes="));
    WriteHex64(context.Console, static_cast<Foundation::UInt64>(mapSize));
    static_cast<void>(WriteBootAscii(context.Console, " descriptor-bytes="));
    WriteHex64(context.Console, static_cast<Foundation::UInt64>(descriptorSize));
    static_cast<void>(WriteBootAscii(context.Console, "\r\n"));

    const bool valid = (status == Success) && (mapSize <= sizeof(g_memoryMap)) && (descriptorSize >= 40U) &&
                       (mapSize >= descriptorSize) && IsWholeDescriptorArray(mapSize, descriptorSize) &&
                       (descriptorVersion == MemoryDescriptorVersion);
    // The span the kernel sizes its metadata arena for, from this, the last map the loader reads
    // before its reservations.
    g_managedSpanGiB = valid ? ComputeManagedSpanGiB(g_memoryMap, mapSize, descriptorSize) : 0U;
    return valid;
}

#if defined(ASTERIA_TARGET_AMD64)
constexpr Foundation::UInt64 FirmwareMemoryWriteBackAttribute{1U << 3U};
constexpr Foundation::UInt64 FirmwareMemoryRuntimeAttribute{Foundation::UInt64{1U} << 63U};
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};

[[nodiscard]] Platform::Amd64::Firmware::Amd64LoadedImageLayoutError
CaptureAmd64LoadedImageLayout(const Foundation::UInt64 imageBase, const Foundation::UInt64 imageSize) noexcept
{
    if (imageSize > Platform::Amd64::Firmware::MaximumAmd64LoadedImageBytes)
    {
        return Platform::Amd64::Firmware::Amd64LoadedImageLayoutError::ImageSizeOutOfRange;
    }
    Platform::Amd64::Firmware::Amd64LoadedImageLayout layout{};
    const auto error = Platform::Amd64::Firmware::Amd64LoadedImageLayoutParser::Parse(
        reinterpret_cast<const void*>(static_cast<Foundation::UIntPointer>(imageBase)),
        static_cast<Foundation::Size>(imageSize), layout);
    if (error != Platform::Amd64::Firmware::Amd64LoadedImageLayoutError::None)
    {
        return error;
    }
    g_amd64LoadedImageLayout = layout;
    return Platform::Amd64::Firmware::Amd64LoadedImageLayoutError::None;
}

#elif defined(ASTERIA_TARGET_IA64)
[[nodiscard]] bool CaptureIa64LoadedImageLayout(const Foundation::UInt64 imageBase,
                                                const Foundation::UInt64 imageSize) noexcept
{
    const auto layout = Platform::Ia64::Firmware::Ia64LoadedImageLayoutParser::Parse(
        reinterpret_cast<const void*>(static_cast<Foundation::UIntPointer>(imageBase)),
        static_cast<Foundation::Size>(imageSize), Platform::Ia64::Firmware::Ia64LoadedImagePlacement::AlignedCopy);
    if (!layout.HasValue())
    {
        return false;
    }
    g_ia64LoadedImageLayout = layout.GetValue();
    return true;
}

/// Allocates the run-time memory reservation from the final map: the
/// preferred page of SelectRuntimeMemoryReservationPageShifts for the BSP's
/// insertable sizes, or its fallback when no free range holds the preferred
/// page, each aligned to its own size. Single-CPU bootstrap only.
[[nodiscard]] bool CaptureIa64SalSystemTable(BootContext& context) noexcept
{
    const auto capture = Platform::Ia64::Firmware::SalSystemTableCapture::CaptureFromBootServices(
        *context.System.BootServicesTable, context.System.ConfigurationTables, context.System.NumberOfTableEntries,
        g_memoryMap, sizeof(g_memoryMap), g_ia64SalSystemTableSnapshot, sizeof(g_ia64SalSystemTableSnapshot));
    if (!capture.HasValue())
    {
        const auto& error = capture.GetError();
        WriteLoaderDiagnosticAscii("ASTERIA:SAL-SYSTEM-TABLE-ERROR:");
        WriteIa64DiagnosticHex(static_cast<Foundation::UInt32>(error.Kind), 2U);
        WriteLoaderDiagnosticAscii(":");
        WriteIa64DiagnosticHex(static_cast<Foundation::UInt32>(error.LocationError), 2U);
        WriteLoaderDiagnosticAscii(":");
        WriteIa64DiagnosticHex(static_cast<Foundation::UInt32>(error.ParseError), 2U);
        WriteLoaderDiagnosticAscii("\r\n");
        return false;
    }
    g_ia64SalSystemTableInformation = capture.GetValue();
    return true;
}
#endif

[[nodiscard]] bool CheckConsole(SimpleTextOutputProtocol& console) noexcept
{
    if ((console.QueryMode == nullptr) || (console.Mode == nullptr) || (console.Mode->Mode < 0))
    {
        return false;
    }

    Foundation::Size columns{0U};
    Foundation::Size rows{0U};
    return (console.QueryMode(&console, static_cast<Foundation::Size>(console.Mode->Mode), &columns, &rows) ==
            Success) &&
           (columns != 0U) && (rows != 0U);
}

[[nodiscard]] bool TryLocateAcpiRsdp(const SystemTable& systemTable, const void*& rsdpPointer) noexcept
{
    if ((systemTable.NumberOfTableEntries == 0U) || (systemTable.NumberOfTableEntries > MaximumConfigurationTables) ||
        (systemTable.ConfigurationTables == nullptr))
    {
        return false;
    }

    const void* acpi20Pointer{nullptr};
    const void* acpi10Pointer{nullptr};
    for (Foundation::Size index{0U}; index < systemTable.NumberOfTableEntries; ++index)
    {
        const ConfigurationTable& table = systemTable.ConfigurationTables[index];
        if (IsAcpi20Guid(table.VendorGuid))
        {
            if ((table.VendorTable == nullptr) || ((acpi20Pointer != nullptr) && (acpi20Pointer != table.VendorTable)))
            {
                return false;
            }
            acpi20Pointer = table.VendorTable;
        }
        else if (IsAcpi10Guid(table.VendorGuid))
        {
            if ((table.VendorTable == nullptr) || ((acpi10Pointer != nullptr) && (acpi10Pointer != table.VendorTable)))
            {
                return false;
            }
            acpi10Pointer = table.VendorTable;
        }
    }
    rsdpPointer = acpi20Pointer != nullptr ? acpi20Pointer : acpi10Pointer;
    return rsdpPointer != nullptr;
}

#if defined(ASTERIA_TARGET_IA64)
// DIG64 HCDP 1.0a section 2 and PCDP 2.0 section 2: the EFI configuration
// table GUID of the HCDP/PCDP table, F951938D-620B-42EF-8279-A84B79617898.
[[nodiscard]] bool IsHcdpGuid(const Guid& guid) noexcept
{
    constexpr Foundation::UInt8 Tail[8U]{0x82U, 0x79U, 0xA8U, 0x4BU, 0x79U, 0x61U, 0x78U, 0x98U};
    bool equal = (guid.Data1 == 0xF951938DU) && (guid.Data2 == 0x620BU) && (guid.Data3 == 0x42EFU);
    for (Foundation::Size index{0U}; equal && (index < sizeof(Tail)); ++index)
    {
        equal = guid.Data4[index] == Tail[index];
    }
    return equal;
}

/// The HCDP configuration table, or null for none or a malformed list.
[[nodiscard]] const void* FindHcdpConfigurationTable(const SystemTable& systemTable) noexcept
{
    if ((systemTable.NumberOfTableEntries > MaximumConfigurationTables) || (systemTable.ConfigurationTables == nullptr))
    {
        return nullptr;
    }
    for (Foundation::Size index{0U}; index < systemTable.NumberOfTableEntries; ++index)
    {
        const ConfigurationTable& table = systemTable.ConfigurationTables[index];
        if (IsHcdpGuid(table.VendorGuid))
        {
            return table.VendorTable;
        }
    }
    return nullptr;
}

/// UEFI 2.11 7.2.3: the one EfiMemoryMappedIOPortSpace (type 12) range of the
/// current memory map, which the sparse I/O port encoding addresses; a zero
/// window when the map is unavailable or names none or several.
[[nodiscard]] Platform::Ia64::Firmware::Ia64PortSpaceWindow
FindFirmwarePortSpaceWindow(const SystemTable& systemTable) noexcept
{
    constexpr Foundation::Size TypeOffset{0U};
    constexpr Foundation::Size PhysicalStartOffset{8U};
    constexpr Foundation::Size PageCountOffset{24U};
    constexpr Foundation::Size MinimumDescriptorBytes{40U};
    Foundation::Size mapSize{sizeof(g_memoryMap)};
    Foundation::Size mapKey{0U};
    Foundation::Size descriptorSize{0U};
    Foundation::UInt32 descriptorVersion{0U};
    const auto getMap = systemTable.BootServicesTable->GetMemoryMapFunction;
    if ((getMap == nullptr) ||
        (getMap(&mapSize, g_memoryMap, &mapKey, &descriptorSize, &descriptorVersion) != Success) ||
        (mapSize > sizeof(g_memoryMap)) || (descriptorSize < MinimumDescriptorBytes))
    {
        return {};
    }
    const auto read = [](const Foundation::UInt8* const bytes, const Foundation::Size count) noexcept
    {
        Foundation::UInt64 value{0U};
        for (Foundation::Size index{0U}; index < count; ++index)
        {
            value |= static_cast<Foundation::UInt64>(bytes[index]) << (index * 8U);
        }
        return value;
    };
    Platform::Ia64::Firmware::Ia64PortSpaceWindow window{};
    Foundation::Size found{0U};
    for (Foundation::Size offset{0U}; (mapSize - offset) >= descriptorSize; offset += descriptorSize)
    {
        const Foundation::UInt8* const descriptor = g_memoryMap + offset;
        const Foundation::UInt64 type = read(descriptor + TypeOffset, 4U);
        const Foundation::UInt64 start = read(descriptor + PhysicalStartOffset, 8U);
        const Foundation::UInt64 pages = read(descriptor + PageCountOffset, 8U);
        if (type == Platform::Ia64::Firmware::Ia64PortSpaceFirmwareType)
        {
            ++found;
            window = {start, pages <= (~Foundation::UInt64{0U} >> 12U) ? pages << 12U : 0U};
        }
    }
    return found == 1U ? window : Platform::Ia64::Firmware::Ia64PortSpaceWindow{};
}
#endif

[[nodiscard]] bool FillRectangle(GraphicsOutputProtocol& graphics, const GraphicsOutputBltPixel color,
                                 const Foundation::Size x, const Foundation::Size y, const Foundation::Size width,
                                 const Foundation::Size height) noexcept
{
    GraphicsOutputBltPixel mutableColor = color;
    return graphics.Blt(&graphics, &mutableColor, GraphicsOutputBltOperation::VideoFill, 0U, 0U, x, y, width, height,
                        0U) == Success;
}

// Records a mode the System Display Broker can scan out: a page-aligned linear framebuffer of
// four-byte RGBX or BGRX pixels large enough for the mode. Any other mode leaves no display.
void RecordBootDisplay(const GraphicsOutputProtocolMode& mode) noexcept
{
    if (!IsScanoutModeUsable(&mode))
    {
        g_bootDisplay = BootDisplay{};
        return;
    }
    const GraphicsOutputModeInformation& information = *mode.Information;
    const Foundation::UInt32 format{information.PixelFormat == 0U ? BootDisplayFormatRgbx : BootDisplayFormatBgrx};
    const Foundation::UInt64 needed{Foundation::UInt64{information.PixelsPerScanLine} * information.VerticalResolution *
                                    4U};
    g_bootDisplay = BootDisplay{.FrameBufferBase = mode.FrameBufferBase,
                                .FrameBufferBytes = needed,
                                .Width = information.HorizontalResolution,
                                .Height = information.VerticalResolution,
                                .PixelsPerScanLine = information.PixelsPerScanLine,
                                .Format = format};
}

[[nodiscard]] bool DrawBootstrapDesktop(GraphicsOutputProtocol* const selectedGraphics, Foundation::UInt32& width,
                                        Foundation::UInt32& height) noexcept
{
    if (selectedGraphics == nullptr)
    {
        return false;
    }
    auto& graphics = *selectedGraphics;
    if ((graphics.Blt == nullptr) || (graphics.Mode == nullptr) || (graphics.Mode->Information == nullptr) ||
        (graphics.Mode->MaximumMode == 0U) || (graphics.Mode->Mode >= graphics.Mode->MaximumMode) ||
        (graphics.Mode->InformationSize < sizeof(GraphicsOutputModeInformation)))
    {
        return false;
    }

    width = graphics.Mode->Information->HorizontalResolution;
    height = graphics.Mode->Information->VerticalResolution;
    if ((width < 320U) || (height < 200U))
    {
        return false;
    }
    RecordBootDisplay(*graphics.Mode);

    const Foundation::Size screenWidth = static_cast<Foundation::Size>(width);
    const Foundation::Size screenHeight = static_cast<Foundation::Size>(height);
    const Foundation::Size minimumDimension = screenWidth < screenHeight ? screenWidth : screenHeight;
    const Foundation::Size candidateMargin = minimumDimension >> 5U;
    const Foundation::Size margin = candidateMargin < MinimumDesktopMargin ? MinimumDesktopMargin : candidateMargin;
    if ((margin > (screenWidth >> 1U)) || (margin > (screenHeight >> 1U)))
    {
        return false;
    }

    const Foundation::Size panelWidth = screenWidth - (margin << 1U);
    const Foundation::Size panelHeight = screenHeight - (margin << 1U);
    const Foundation::Size titleHeight = screenHeight >> 4U;
    const Foundation::Size railWidth = screenWidth >> 4U;
    if ((panelWidth < 24U) || (panelHeight < 20U) || (titleHeight == 0U) || (titleHeight >= panelHeight) ||
        (railWidth == 0U) || (railWidth > panelWidth) || (DesktopShadowOffset > margin))
    {
        return false;
    }

    const GraphicsOutputBltPixel desktop{0x58U, 0x42U, 0x18U, 0U};
    const GraphicsOutputBltPixel shadow{0x20U, 0x20U, 0x20U, 0U};
    const GraphicsOutputBltPixel panel{0xD8U, 0xD8U, 0xD0U, 0U};
    const GraphicsOutputBltPixel title{0x58U, 0x28U, 0x20U, 0U};
    const GraphicsOutputBltPixel rail{0x40U, 0x78U, 0x70U, 0U};
    const GraphicsOutputBltPixel accent{0x30U, 0xC0U, 0xE8U, 0U};

    return FillRectangle(graphics, desktop, 0U, 0U, screenWidth, screenHeight) &&
           FillRectangle(graphics, shadow, margin + DesktopShadowOffset, margin + DesktopShadowOffset, panelWidth,
                         panelHeight) &&
           FillRectangle(graphics, panel, margin, margin, panelWidth, panelHeight) &&
           FillRectangle(graphics, title, margin, margin, panelWidth, titleHeight) &&
           FillRectangle(graphics, rail, margin, margin + titleHeight, railWidth, panelHeight - titleHeight) &&
           FillRectangle(graphics, accent, margin + 12U, margin + 8U, 12U, 12U);
}

[[nodiscard]] BootPciResource EncodePciResource(const Platform::Firmware::AhciPciResource& resource) noexcept
{
    return {resource.AbarBaseAddress, resource.AbarLengthBytes,
            resource.Segment,         resource.Bus,
            resource.Device,          resource.Function,
            resource.BarIndex,        {resource.Reserved[0U], resource.Reserved[1U]}};
}

[[nodiscard]] bool PrepareKernelHandoff(BootInfo& info) noexcept
{
    // The inventory is owned by the loader and copied while its source is
    // stable. Its entire payload is protected by the enclosing record CRC.
    const auto* const inventory = reinterpret_cast<const Abi::DeviceInventory*>(info.Inventory.PhysicalStart);
    if (inventory != nullptr && info.Inventory.Pages == Abi::DeviceInventoryPages)
    {
        static_cast<void>(memcpy(&info.InventoryRecord, inventory, sizeof(info.InventoryRecord)));
    }
    // The verified allowlist the kernel applies, covered by the record's CRC.
    info.Trust = g_bootTrust;
    // Every firmware DMA reservation the tables declare, and BOOT.CFG's watermark.
    Acpi::AcpiTableParser::CollectDmaExclusions(info, info.Acpi, info.DmaExclusions);
    info.LowMemoryBlocks = g_bootLowMemoryBlocks;
    info.WatchdogSeconds = g_bootWatchdogSeconds;
    info.SessionMode = g_bootSessionMode;
    info.LoginEnabled = g_bootLoginEnabled;
    BootKernelInput& input = info.Kernel;
    input.RsdpPhysicalAddress = reinterpret_cast<Foundation::UIntPointer>(g_acpiRsdpPointer);
    input.EvidencePhysicalAddress = ReadKernelQuery(KernelQuery::EvidenceAddress);
    input.Image = GetPlacedKernelImage();
    input.Bootstrap = GetKernelBootReservation(0U);
    input.RuntimePool = GetKernelBootReservation(1U);
    input.Startup = GetKernelBootReservation(2U);
    input.Ia64Runtime = GetKernelBootReservation(3U);
    input.MetadataArena = GetKernelMetadataArena();
    for (Foundation::Size index{0U}; index < info.MemoryRegionCount; ++index)
    {
        const auto& region = info.MemoryRegions[index];
        if (region.PhysicalStart <= input.Ia64Runtime.PhysicalStart &&
            region.PageCount <= (~Foundation::UInt64{0U} >> 12U) &&
            input.Ia64Runtime.PhysicalStart - region.PhysicalStart < region.PageCount * 4096U &&
            region.FirmwareType == input.Ia64Runtime.FirmwareType)
        {
            input.Ia64Runtime.Attributes = region.Attributes;
            input.Bootstrap.Attributes = region.Attributes;
            break;
        }
    }
    for (Foundation::Size index{0U}; (input.MetadataArena.PageCount != 0U) && (index < info.MemoryRegionCount); ++index)
    {
        const auto& region = info.MemoryRegions[index];
        if (region.PhysicalStart <= input.MetadataArena.PhysicalStart &&
            region.PageCount <= (~Foundation::UInt64{0U} >> 12U) &&
            input.MetadataArena.PhysicalStart - region.PhysicalStart < region.PageCount * 4096U)
        {
            input.MetadataArena.Attributes = region.Attributes;
            break;
        }
    }
    input.Diagnostics = GetLoaderDiagnosticSinks();
    info.DebugPort = GetLoaderDebugPort();
#if defined(ASTERIA_TARGET_AMD64)
    input.Architecture = BootArchitectureAmd64;
    input.Ahci = EncodePciResource(g_amd64AhciResource);
    input.Device = EncodePciResource(g_amd64DeviceResource);
    input.Nic = EncodePciResource(g_amd64NicResource);
    const auto error =
        Platform::Amd64::Memory::InitialPagingCapabilityCapture::Capture(g_amd64InitialPagingCapabilities);
    const auto& paging = g_amd64InitialPagingCapabilities;
    input.Paging = {paging.PhysicalAddressBits,
                    static_cast<Foundation::UInt8>(paging.FourLevelPagingEnabled),
                    static_cast<Foundation::UInt8>(paging.ExecuteDisableEnabled),
                    static_cast<Foundation::UInt8>(paging.SupervisorWriteProtectEnabled),
                    static_cast<Foundation::UInt8>(paging.GlobalPagesEnabled),
                    static_cast<Foundation::UInt8>(paging.PatLayoutConfigured),
                    0U,
                    0U,
                    static_cast<Foundation::UInt32>(error)};
#else
    input.Architecture = BootArchitectureIa64;
    input.Ahci = EncodePciResource(g_ia64AhciPciResource);
    const auto& sal = g_ia64SalSystemTableInformation;
    input.Sal = {sal.PalProcedurePhysicalAddress,
                 sal.SalProcedurePhysicalAddress,
                 sal.SalGlobalDataPhysicalAddress,
                 sal.TranslationRegisterVirtualAddress,
                 sal.TranslationRegisterEncodedPageSize,
                 sal.CoherenceDomainInformationPhysicalAddress,
                 sal.ApWakeVector,
                 sal.TotalTableLength,
                 sal.CoherenceDomainCount,
                 sal.SalRevision,
                 sal.EntryCount,
                 sal.SalAVersion,
                 sal.SalBVersion,
                 sal.MemoryDescriptorCount,
                 sal.PlatformFeatures,
                 static_cast<Foundation::UInt8>(sal.HasPlatformFeatures),
                 static_cast<Foundation::UInt8>(sal.HasTranslationRegister),
                 static_cast<Foundation::UInt8>(sal.HasPurgeCoherenceInformation),
                 static_cast<Foundation::UInt8>(sal.HasApWakeVector),
                 0U};
    input.TimerTicksPerMicrosecond = g_ia64ItcTicksPerMicrosecond;
#endif
    SealBootInfo(info);
    return PrepareLoadedKernel(info);
}

class KernelHandoffPreparation final : public IFinalMemoryMapPreparation
{
public:
    [[nodiscard]] bool Prepare(BootInfo& info) const noexcept override
    {
        return PrepareKernelHandoff(info);
    }
};
constinit const KernelHandoffPreparation g_kernelHandoffPreparation{};

/// The final-map transaction with the handoff this image publishes: the
/// ACPI topology, the boot entropy, and on AMD64 the prepared runtime.
[[nodiscard, gnu::noinline]] BootTransitionResult ExitBootServicesWithHandoff(const Handle imageHandle,
                                                                              BootServices& bootServices) noexcept
{
    const BootInventoryLocation inventory = GetBootInventoryLocation();
    const AcpiCaptureRequest acpiCapture{
        .RsdpPointer = g_acpiRsdpPointer,
#if defined(ASTERIA_TARGET_AMD64)
        .TargetArchitecture = Boot::Acpi::AcpiTargetArchitecture::Amd64,
#else
        .TargetArchitecture = Boot::Acpi::AcpiTargetArchitecture::Ia64,
#endif
        .Reserved = 0U,
#if defined(ASTERIA_TARGET_AMD64)
        .BspApicId = static_cast<Foundation::UInt32>(ReadKernelQuery(KernelQuery::FirmwareApicId) & 0xFFFFFFFFU),
#endif
    };
    return ExitBootServicesAndCaptureMemoryMap(imageHandle, bootServices, g_memoryMap, sizeof(g_memoryMap), g_bootInfo,
                                               &acpiCapture, nullptr, &g_bootEntropy, &g_bootDisplay, &inventory,
                                               &g_bootFiles, &g_kernelHandoffPreparation);
}

#if defined(ASTERIA_TARGET_AMD64)
// The AHCI controller's ABAR and the assignable devices' BARs, apart from
// RunBootApplication to keep each frame within the stack budget.
[[gnu::noinline]] void CaptureAmd64PciResources(BootContext& context) noexcept
{
    const auto amd64Ahci =
        Platform::Firmware::AhciPciResourceCapture::Capture(*context.System.BootServicesTable, g_amd64AhciResource);
    // Optional: only the cases that attach the device have one.
    static_cast<void>(Platform::Firmware::AhciPciResourceCapture::CaptureByIdentifier(
        *context.System.BootServicesTable, Amd64AssignableIdentifier, Amd64AssignableBar, g_amd64DeviceResource));
    static_cast<void>(Platform::Firmware::AhciPciResourceCapture::CaptureByIdentifier(
        *context.System.BootServicesTable, Amd64NicIdentifier, Amd64NicBar, g_amd64NicResource));
    WriteLoaderDiagnosticAscii(amd64Ahci.Error == Platform::Firmware::AhciPciResourceCaptureError::None
                                   ? "ASTERIA:STAGE:AMD64-AHCI-RESOURCE\r\n"
                                   : "ASTERIA:AMD64-AHCI-RESOURCE:UNAVAILABLE\r\n");
}
#endif

// Before an application that failed ahead of ExitBootServices returns to the
// firmware, whose boot manager may clear the screen at once: says so on the
// console and waits long enough to read the failure above.
// IA-64 retains these pointer-free outputs before placement. The copied
// continuation reports them only after the display is ready, including errors.
[[gnu::noinline]] void ReadAndRetainBootFiles(const Handle image, BootServices& services) noexcept
{
    const char* const reason =
        LoadBootFiles(image, services, g_bootFiles, g_bootKernelFile, &g_displayPolicy, &g_bootTrust,
                      &g_bootLowMemoryBlocks, &g_bootWatchdogSeconds, &g_bootSessionMode, &g_bootLoginEnabled);
    g_bootFileFailure[0U] = '\0';
    if (reason != nullptr)
    {
        Foundation::Size index{0U};
        while (reason[index] != '\0' && index + 1U < sizeof(g_bootFileFailure))
        {
            g_bootFileFailure[index] = reason[index];
            ++index;
        }
        g_bootFileFailure[index] = '\0';
    }
}

// Each trust entry with its module's verdict; a mismatch leaves the entry inert and is no boot
// failure.
[[gnu::noinline]] void ReportBootTrust(BootContext& context) noexcept
{
    for (Foundation::Size index{0U}; index < g_bootTrust.Count && index < BootTrustMaximumEntries; ++index)
    {
        char line[192]{};
        Inventory::ReportText text{line, sizeof(line) - 3U};
        FormatBootTrust(text, g_bootTrust.Entries[index], index + 1U, g_bootFiles);
        text.Text("\r\n");
        static_cast<void>(WriteBootAscii(context.Console, line));
    }
}

void ReportRetainedBootFiles(BootContext& context) noexcept
{
    for (Foundation::Size index{0U}; index <= BootFileCount + g_bootFiles.ModuleCount; ++index)
    {
        char line[128]{};
        Inventory::ReportText text{line, sizeof(line) - 1U};
        if (index == BootFileCount + g_bootFiles.ModuleCount)
        {
            FormatBootFile(text, g_bootKernelFile.File, nullptr);
        }
        else
        {
            const BootFileRecord& file =
                index < BootFileCount ? g_bootFiles.Files[index] : g_bootFiles.Modules[index - BootFileCount].File;
            const char* const name = index < BootFileCount ? nullptr : g_bootFiles.Modules[index - BootFileCount].Name;
            FormatBootFile(text, file, name);
        }
        static_cast<void>(WriteBootAscii(context.Console, line));
    }
    ReportBootTrust(context);
}

void ReturnAfterGraphicsFailure(BootContext& context, BootServices& services) noexcept
{
    // Reports may wrap the bounded console. Repeat the failed stage before
    // the timed screen, then restore firmware graphics immediately on return.
    if (context.LastFailure != nullptr)
    {
        static_cast<void>(WriteBootAscii(context.Console, "ASTERIA:FAILED-STAGE:"));
        static_cast<void>(WriteBootAscii(context.Console, context.LastFailure));
        static_cast<void>(WriteBootAscii(context.Console, "\r\n"));
    }
    static_cast<void>(WriteBootAscii(
        context.Console,
        "Asteria did not start: the ASTERIA lines above say why. Returning to the firmware in 30 seconds.\r\n"));
    if (services.StallFunction != nullptr)
    {
        static_cast<void>(services.StallFunction(30'000'000U));
    }
    RestoreOriginalGraphicsMode();
}

[[nodiscard]] KernelElfError PlaceAndReleaseKernelFile(BootServices& services) noexcept
{
    // LoadBootFiles validated the receipt and CRC. The immutable source borrow
    // ends inside LoadKernelImage; no kernel mapping or retained plan uses it.
    const auto* const bytes = reinterpret_cast<const Foundation::UInt8*>(
        static_cast<Foundation::UIntPointer>(g_bootKernelFile.PhysicalStart));
    const KernelElfError result =
        LoadKernelImage(services, {bytes, static_cast<Foundation::Size>(g_bootKernelFile.File.SizeBytes)});
    if (services.FreePagesFunction == nullptr ||
        services.FreePagesFunction(g_bootKernelFile.PhysicalStart,
                                   static_cast<Foundation::Size>(g_bootKernelFile.PageCount)) != Success)
    {
        WriteLoaderDiagnosticAscii("ASTERIA:KERNEL-FILE:FAIL:RELEASE\r\n");
        return KernelElfError::InvalidDestination;
    }
    g_bootKernelFile.PhysicalStart = 0U;
    g_bootKernelFile.PageCount = 0U;
    return result;
}

void RemoveBootServicesAfterExit(SystemTable& system) noexcept
{
    ClearExpiredBootServices(system);
    char line[128]{};
    Inventory::ReportText text{line, sizeof(line) - 1U};
    text.Text("ASTERIA:BOOT-SERVICES:ABSENT system-table=0x");
    text.Hex(reinterpret_cast<Foundation::UIntPointer>(&system), 16U);
    text.Text("\r\n");
    WriteLoaderDiagnosticAscii(line);
}

[[nodiscard]] Status RunBootApplication(const Handle imageHandle, SystemTable& systemTable) noexcept
{
#if defined(ASTERIA_TARGET_AMD64)
    // Before the first diagnostic: where diagnostics go, decided once
    // (QEMU's debug port, a UART the SPCR names or COM1).
    {
        const void* rsdp{nullptr};
        static_cast<void>(TryLocateAcpiRsdp(systemTable, rsdp));
        SelectLoaderDiagnosticSinks(rsdp);
    }
#elif defined(ASTERIA_TARGET_IA64)
    // Before the first diagnostic: the UART the firmware's debug-port tables
    // name (DBG2, DBGP, HCDP, SPCR), else itanium2-vpc's debug UART.
    {
        const void* rsdp{nullptr};
        static_cast<void>(TryLocateAcpiRsdp(systemTable, rsdp));
        const Platform::Ia64::Firmware::Ia64PortSpaceWindow window = FindFirmwarePortSpaceWindow(systemTable);
        SelectLoaderDebugPort(rsdp, FindHcdpConfigurationTable(systemTable), window.Base, window.Bytes);
    }
#endif
    BootContext context{systemTable, *systemTable.ConsoleOut};
    const bool bssZeroInitialized = IsBssZeroInitialized();
#if defined(ASTERIA_TARGET_AMD64)
    ReadAndRetainBootFiles(imageHandle, *systemTable.BootServicesTable);
#endif
    Foundation::UInt32 graphicsWidth{0U};
    Foundation::UInt32 graphicsHeight{0U};
    Foundation::UInt64 loadedImageBase{0U};
    Foundation::UInt64 loadedImageSize{0U};

    GraphicsOutputProtocol* const graphics = ChooseGraphicsMode(systemTable, g_displayPolicy);
    const bool graphicsReady = DrawBootstrapDesktop(graphics, graphicsWidth, graphicsHeight);

    if (!IsBootConsoleConfigured() && context.Console.SetAttribute != nullptr)
    {
        static_cast<void>(context.Console.SetAttribute(&context.Console, 0x0FU));
    }
    if (!IsBootConsoleConfigured() && context.Console.SetCursorPosition != nullptr)
    {
        static_cast<void>(context.Console.SetCursorPosition(&context.Console, 0U, 0U));
    }

    AnnounceGraphicsMode(systemTable, graphics);
    const bool bannerWritten = WriteBootAscii(context.Console, "Asteria capability nucleus - UEFI bootstrap\r\n");
#if defined(ASTERIA_TARGET_IA64)
    static_cast<void>(WriteBootAscii(context.Console, "target=IA-64 little-endian\r\n"));
#elif defined(ASTERIA_TARGET_AMD64)
    static_cast<void>(WriteBootAscii(context.Console, "target=AMD64 little-endian\r\n"));
#endif

    if (g_bootFileFailure[0U] != '\0')
    {
        static_cast<void>(WriteBootAscii(context.Console, "ASTERIA:BOOT-FILES:FAIL:"));
        static_cast<void>(WriteBootAscii(context.Console, g_bootFileFailure));
        static_cast<void>(WriteBootAscii(context.Console, "\r\n"));
        ReturnAfterGraphicsFailure(context, *systemTable.BootServicesTable);
        return DeviceError;
    }
    ReportCheck(context, "SYSTEM-TABLE", systemTable.Header.Signature == SystemTableSignature);
    if (!bssZeroInitialized)
    {
        ReportFailure(context, "BSS-ZERO");
    }
    ReportCheck(context, "CONSOLE", bannerWritten && CheckConsole(context.Console));
    ReportCheck(context, "BOOT-SERVICES", systemTable.BootServicesTable->Header.Signature == BootServicesSignature);

    const auto* const runtimeHeader = static_cast<const TableHeader*>(systemTable.RuntimeServices);
    ReportCheck(context, "RUNTIME-SERVICES",
                (runtimeHeader != nullptr) && (runtimeHeader->Signature == RuntimeServicesSignature));
    ReportCheck(context, "SYSTEM-TABLE-CRC", IsTableCrcValid(&systemTable, systemTable.Header));
    const bool loadedImageValid = CheckLoadedImage(context, imageHandle, loadedImageBase, loadedImageSize);
    if (loadedImageValid)
    {
        static_cast<void>(WriteBootAscii(context.Console, "  firmware-image-base="));
        WriteHex64(context.Console, loadedImageBase);
        static_cast<void>(WriteBootAscii(context.Console, " image-size="));
        WriteHex64(context.Console, loadedImageSize);
        static_cast<void>(WriteBootAscii(context.Console, "\r\n"));
    }
#if defined(ASTERIA_TARGET_IA64)
    loadedImageBase = Platform::Ia64::Firmware::GetIa64ExecutingImageBase();
    loadedImageSize = Platform::Ia64::Firmware::GetIa64ExecutingImageEnd() - loadedImageBase;
#endif
    ReportCheck(context, "LOADED-IMAGE", loadedImageValid);
#if defined(ASTERIA_TARGET_AMD64)
    const auto amd64ImageLayoutError = loadedImageValid
                                           ? CaptureAmd64LoadedImageLayout(loadedImageBase, loadedImageSize)
                                           : Platform::Amd64::Firmware::Amd64LoadedImageLayoutError::NullImage;
    ReportCheck(context, "AMD64-IMAGE-LAYOUT",
                amd64ImageLayoutError == Platform::Amd64::Firmware::Amd64LoadedImageLayoutError::None);
    if (amd64ImageLayoutError != Platform::Amd64::Firmware::Amd64LoadedImageLayoutError::None)
    {
        static_cast<void>(WriteBootAscii(context.Console, "  layout-error="));
        WriteHex64(context.Console, static_cast<Foundation::UInt32>(amd64ImageLayoutError));
        static_cast<void>(WriteBootAscii(context.Console, "\r\n"));
    }
#elif defined(ASTERIA_TARGET_IA64)
    ReportCheck(context, "IA64-IMAGE-LAYOUT",
                loadedImageValid && CaptureIa64LoadedImageLayout(loadedImageBase, loadedImageSize));
#endif
    const KernelElfError kernelError =
        loadedImageValid ? PlaceAndReleaseKernelFile(*context.System.BootServicesTable) : KernelElfError::InvalidSource;
    ReportCheck(context, "KERNEL-ELF", kernelError == KernelElfError::None);
    if (kernelError == KernelElfError::None)
    {
        const auto& image = GetPlacedKernelImage();
        static_cast<void>(WriteBootAscii(context.Console, "  image-base="));
        WriteHex64(context.Console, image.PhysicalStart);
        static_cast<void>(WriteBootAscii(context.Console, " image-size="));
        WriteHex64(context.Console, image.SizeBytes);
        static_cast<void>(WriteBootAscii(context.Console, "\r\n  kernel-base="));
        WriteHex64(context.Console, image.PhysicalStart);
        static_cast<void>(WriteBootAscii(context.Console, "\r\nASTERIA:EVIDENCE:"));
        WriteHex64(context.Console, ReadKernelQuery(KernelQuery::EvidenceAddress));
        static_cast<void>(WriteBootAscii(context.Console, "\r\n"));
    }
    else
    {
        static_cast<void>(WriteBootAscii(context.Console, "ASTERIA:KERNEL-ELF:FAIL:"));
        WriteHex64(context.Console, static_cast<Foundation::UInt32>(kernelError));
        static_cast<void>(WriteBootAscii(context.Console, "\r\n"));
    }
    ReportCheck(context, "MEMORY-MAP", CheckMemoryMap(context));
#if defined(ASTERIA_TARGET_AMD64)
    CaptureAmd64PciResources(context);
#endif
#if defined(ASTERIA_TARGET_IA64)
    ReportCheck(context, "SAL-SYSTEM-TABLE", CaptureIa64SalSystemTable(context));
    const auto ahciPciResourceCapture =
        Platform::Firmware::AhciPciResourceCapture::Capture(*context.System.BootServicesTable, g_ia64AhciPciResource);
    ReportIa64OptionalAhciResourceCapture(context, ahciPciResourceCapture);
#endif
    g_acpiRsdpPointer = nullptr;
    ReportCheck(context, "ACPI", TryLocateAcpiRsdp(systemTable, g_acpiRsdpPointer));
    static_cast<void>(WriteBootAscii(context.Console, graphicsReady ? "ASTERIA:STAGE:GRAPHICS\r\n"
                                                                    : "ASTERIA:GRAPHICS:UNAVAILABLE\r\n"));
    if (graphicsReady)
    {
        static_cast<void>(WriteBootAscii(context.Console, "  width="));
        WriteHex64(context.Console, graphicsWidth);
        static_cast<void>(WriteBootAscii(context.Console, " height="));
        WriteHex64(context.Console, graphicsHeight);
        static_cast<void>(WriteBootAscii(context.Console, "\r\n"));
    }
    ReportRetainedBootFiles(context);
    WriteBootReport(imageHandle, systemTable, g_bootDisplay, g_bootFiles, g_bootTrust);
    static_cast<void>(GatherHandoffEntropy(context));

    BootServices& bootServices = *systemTable.BootServicesTable;
    ReportCheck(context,
#if defined(ASTERIA_TARGET_AMD64)
                "AMD64-BOOTSTRAP-RESERVATION",
#else
                "IA64-BOOTSTRAP-RESERVATION",
#endif
                context.FailureCount == 0U && ReserveLoadedKernelMemory(bootServices, g_managedSpanGiB));

    if (context.FailureCount == 0U)
    {
        static_cast<void>(WriteBootAscii(context.Console, "ASTERIA:STAGE:BOOT\r\n"));
    }
    else
    {
        static_cast<void>(WriteBootAscii(context.Console, "ASTERIA:BOOT:FAIL\r\n"));
    }

    constexpr Foundation::UInt64 BootStallMicroseconds{3'000'000U};
#if defined(ASTERIA_TARGET_IA64)
    const Foundation::UInt64 stallStart = g_ia64ApWaitClock.GetTicks();
#endif
    if (bootServices.StallFunction != nullptr)
    {
        static_cast<void>(bootServices.StallFunction(BootStallMicroseconds));
#if defined(ASTERIA_TARGET_IA64)
        // The firmware's stall is the boot's only other timebase: the ITC
        // ticks it spans give the rate, rounded, if the wait dispatcher can
        // use it (1 to 0x10000 ticks per microsecond).
        const Foundation::UInt64 elapsed = g_ia64ApWaitClock.GetTicks() - stallStart;
        const Foundation::UInt64 rate = (elapsed + (BootStallMicroseconds / 2U)) / BootStallMicroseconds;
        g_ia64ItcTicksPerMicrosecond = ((rate != 0U) && (rate <= 0x1'0000U)) ? rate : 0U;
#endif
    }
    if (context.FailureCount != 0U)
    {
        ReturnAfterGraphicsFailure(context, bootServices);
        return DeviceError;
    }

    const BootTransitionResult transition = ExitBootServicesWithHandoff(imageHandle, bootServices);
    if (!transition.IsSuccessful())
    {
        // Only a transaction that never called ExitBootServices leaves the
        // boot services, and so the console, usable.
        const bool bootServicesUsable = !transition.HasExitedBootServices() && (transition.ExitAttempts == 0U);
#if defined(ASTERIA_TARGET_AMD64)
        if (bootServicesUsable)
        {
            static_cast<void>(ReleaseKernelBootReservations(bootServices));
        }
#endif
        ReportBootTransitionFailure(context, transition,
                                    bootServicesUsable ? ReportDestination::ConsoleAndDebugPort
                                                       : ReportDestination::DebugPort);
        if (!bootServicesUsable)
        {
            Platform::Firmware::HaltAfterFirmwareExit();
        }
        ReturnAfterGraphicsFailure(context, bootServices);
        return DeviceError;
    }
    // BootInfo now holds the only copy of the seed the kernel needs.
    g_bootEntropy = {};
    WriteLoaderDiagnosticAscii("ASTERIA:STAGE:EXIT-BOOT-SERVICES\r\n"
                               "ASTERIA:STAGE:BOOT-INFO\r\n");
    RemoveBootServicesAfterExit(systemTable);
    EnterLoadedKernel(g_bootInfo);
}
} // namespace
} // namespace Asteria::Boot::Uefi
#if defined(ASTERIA_TARGET_IA64)
extern "C" Asteria::Boot::Uefi::Status ASTERIA_UEFI_API EfiMainAligned(
#else
extern "C" Asteria::Boot::Uefi::Status ASTERIA_UEFI_API EfiMain(
#endif
    Asteria::Boot::Uefi::Handle imageHandle, Asteria::Boot::Uefi::SystemTable* const systemTable) noexcept
{
    if (systemTable == nullptr)
    {
        Asteria::Boot::Uefi::WriteLoaderDiagnosticAscii("ASTERIA:BOOT:FAIL:INVALID-FIRMWARE-TABLE\r\n");
        return Asteria::Boot::Uefi::DeviceError;
    }

    if (!Asteria::Boot::Uefi::IsTableHeaderValid(systemTable->Header, Asteria::Boot::Uefi::SystemTableSignature,
                                                 sizeof(Asteria::Boot::Uefi::SystemTable)) ||
        !Asteria::Boot::Uefi::IsTableCrcValid(systemTable, systemTable->Header))
    {
        Asteria::Boot::Uefi::WriteLoaderDiagnosticAscii("ASTERIA:BOOT:FAIL:INVALID-SYSTEM-TABLE\r\n");
        return Asteria::Boot::Uefi::DeviceError;
    }

    Asteria::Boot::Uefi::BootServices* const bootServices = systemTable->BootServicesTable;
    const auto* const runtimeHeader =
        static_cast<const Asteria::Boot::Uefi::TableHeader*>(systemTable->RuntimeServices);
    if ((bootServices == nullptr) ||
        !Asteria::Boot::Uefi::IsTableHeaderValid(bootServices->Header, Asteria::Boot::Uefi::BootServicesSignature,
                                                 Asteria::Boot::Uefi::MinimumConsumedBootServicesSize) ||
        !Asteria::Boot::Uefi::IsTableCrcValid(bootServices, bootServices->Header) || (runtimeHeader == nullptr) ||
        !Asteria::Boot::Uefi::IsTableHeaderValid(*runtimeHeader, Asteria::Boot::Uefi::RuntimeServicesSignature,
                                                 sizeof(Asteria::Boot::Uefi::TableHeader)) ||
        !Asteria::Boot::Uefi::IsTableCrcValid(runtimeHeader, *runtimeHeader))
    {
        Asteria::Boot::Uefi::WriteLoaderDiagnosticAscii("ASTERIA:BOOT:FAIL:INVALID-SERVICE-TABLE\r\n");
        return Asteria::Boot::Uefi::DeviceError;
    }

    if ((systemTable->ConsoleOut == nullptr) || (systemTable->ConsoleOut->OutputString == nullptr) ||
        (bootServices->GetMemoryMapFunction == nullptr) || (bootServices->ExitBootServicesFunction == nullptr)
#if defined(ASTERIA_TARGET_AMD64)
        || (bootServices->AllocatePagesFunction == nullptr) || (bootServices->FreePagesFunction == nullptr)
#endif
        || (bootServices->HandleProtocolFunction == nullptr) || (bootServices->LocateProtocolFunction == nullptr))
    {
        Asteria::Boot::Uefi::WriteLoaderDiagnosticAscii("ASTERIA:BOOT:FAIL:MISSING-FIRMWARE-SERVICE\r\n");
        return Asteria::Boot::Uefi::DeviceError;
    }

#if defined(ASTERIA_TARGET_AMD64)
    // UEFI 2.9A 7.5.1: Timeout zero disables the boot manager's watchdog.
    if (bootServices->SetWatchdogTimerFunction != nullptr)
    {
        static_cast<void>(bootServices->SetWatchdogTimerFunction(0U, 0x10000U, 0U, nullptr));
    }
#endif
    return Asteria::Boot::Uefi::RunBootApplication(imageHandle, *systemTable);
}

#if defined(ASTERIA_TARGET_IA64)

// This entry publishes no boot state. The firmware stack/RSE and original
// image remain owned until EBS; only the copied continuation touches globals.
extern "C" Asteria::Boot::Uefi::Status ASTERIA_UEFI_API
EfiMain(Asteria::Boot::Uefi::Handle imageHandle, Asteria::Boot::Uefi::SystemTable* const systemTable) noexcept
{
    using Asteria::Foundation::UInt64;
    using Asteria::Platform::Ia64::Firmware::Ia64RelocatedImageAlignmentBytes;
    if ((systemTable == nullptr) || (systemTable->BootServicesTable == nullptr))
    {
        return Asteria::Boot::Uefi::InvalidParameter;
    }
    if (!Asteria::Boot::Uefi::IsTableHeaderValid(systemTable->Header, Asteria::Boot::Uefi::SystemTableSignature,
                                                 sizeof(*systemTable)) ||
        !Asteria::Boot::Uefi::IsTableCrcValid(systemTable, systemTable->Header))
    {
        return Asteria::Boot::Uefi::DeviceError;
    }
    Asteria::Boot::Uefi::BootServices& services = *systemTable->BootServicesTable;
    if (!Asteria::Boot::Uefi::IsTableHeaderValid(services.Header, Asteria::Boot::Uefi::BootServicesSignature,
                                                 Asteria::Boot::Uefi::MinimumConsumedBootServicesSize) ||
        !Asteria::Boot::Uefi::IsTableCrcValid(&services, services.Header))
    {
        return Asteria::Boot::Uefi::DeviceError;
    }
    if ((services.AllocatePagesFunction == nullptr) || (services.FreePagesFunction == nullptr))
    {
        return Asteria::Boot::Uefi::InvalidParameter;
    }
    if (systemTable->ConsoleOut == nullptr || systemTable->ConsoleOut->OutputString == nullptr)
    {
        return Asteria::Boot::Uefi::DeviceError;
    }
    if (services.SetWatchdogTimerFunction != nullptr)
    {
        static_cast<void>(services.SetWatchdogTimerFunction(0U, 0x10000U, 0U, nullptr));
    }
    Asteria::Boot::Uefi::ReadAndRetainBootFiles(imageHandle, services);
    const UInt64 source = Asteria::Platform::Ia64::Firmware::GetIa64ExecutingImageBase();
    const UInt64 bytes = Asteria::Platform::Ia64::Firmware::GetIa64ExecutingImageEnd() - source;
    constexpr UInt64 AlignmentMask{Ia64RelocatedImageAlignmentBytes - 1U};
    if ((bytes == 0U) || ((bytes & 0xFFFU) != 0U) || (bytes > (0xFFFF'FFFFU - AlignmentMask)))
    {
        return Asteria::Boot::Uefi::InvalidParameter;
    }
    const UInt64 pages = (bytes + AlignmentMask) >> 12U;
    UInt64 allocation{0U};
    const auto status = services.AllocatePagesFunction(Asteria::Boot::Uefi::AllocateType::AnyPages,
                                                       Asteria::Boot::Uefi::MemoryType::LoaderCode, pages, &allocation);
    if (status != Asteria::Boot::Uefi::Success)
    {
        return status;
    }
    constexpr UInt64 RegionZeroLimit{UInt64{1U} << 61U};
    if ((allocation == 0U) || ((allocation & 0xFFFU) != 0U) || (allocation > (RegionZeroLimit - (pages << 12U))))
    {
        static_cast<void>(services.FreePagesFunction(allocation, pages));
        return Asteria::Boot::Uefi::DeviceError;
    }
    const UInt64 target = (allocation + AlignmentMask) & ~AlignmentMask;
    const UInt64 imagePages = bytes >> 12U;
    const UInt64 prefixPages = (target - allocation) >> 12U;
    const UInt64 suffixPages = pages - prefixPages - imagePages;
    // UEFI permits page-granular partial frees. Trim the temporary alignment
    // slack so the exact active image remains one aligned LoaderCode receipt.
    if ((suffixPages != 0U) &&
        (services.FreePagesFunction(target + bytes, suffixPages) != Asteria::Boot::Uefi::Success))
    {
        static_cast<void>(services.FreePagesFunction(allocation, pages));
        return Asteria::Boot::Uefi::DeviceError;
    }
    if ((prefixPages != 0U) && (services.FreePagesFunction(allocation, prefixPages) != Asteria::Boot::Uefi::Success))
    {
        static_cast<void>(services.FreePagesFunction(allocation, prefixPages + imagePages));
        return Asteria::Boot::Uefi::DeviceError;
    }
    const UInt64 descriptor = Asteria::Platform::Ia64::Firmware::GetIa64RelocatedEntryDescriptor();
    if ((descriptor < source) || ((descriptor - source) > (bytes - 16U)))
    {
        static_cast<void>(services.FreePagesFunction(target, imagePages));
        return Asteria::Boot::Uefi::DeviceError;
    }
    const auto relocated = Asteria::Platform::Ia64::Firmware::Ia64ImageRelocation::RelocateToAlignment(
        reinterpret_cast<const void*>(static_cast<Asteria::Foundation::UIntPointer>(source)), bytes,
        reinterpret_cast<void*>(static_cast<Asteria::Foundation::UIntPointer>(target)), bytes,
        static_cast<Asteria::Foundation::UInt32>(descriptor - source));
    if (!relocated.HasValue())
    {
        static_cast<void>(services.FreePagesFunction(target, imagePages));
        return Asteria::Boot::Uefi::DeviceError;
    }
    Asteria::Platform::Ia64::Firmware::MakeIa64ImageInstructionCoherent(
        reinterpret_cast<void*>(static_cast<Asteria::Foundation::UIntPointer>(target)), bytes);
    const auto continuation = reinterpret_cast<Asteria::Boot::Uefi::ImageEntryPoint>(
        static_cast<Asteria::Foundation::UIntPointer>(relocated.GetValue().EntryDescriptor));
    return continuation(imageHandle, systemTable);
}

extern "C"
{
Asteria::Boot::Uefi::ImageEntryPoint EfiEntryDescriptorReference __attribute__((used)){EfiMain};
}
#endif
