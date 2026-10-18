// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Device/Amd64BootstrapDevices.hpp"
#include "Platform/Amd64/Device/AmdViTopology.hpp"
#include "Platform/Amd64/Device/Amd64VtdHardware.hpp"

#include "Kernel/Boot/BootInventory.hpp"
#include "Kernel/Boot/UsbHostBootSite.hpp"

#include "Kernel/Device/AhciEmulatorDisk.hpp"
#include "Kernel/Device/TrustedDmaPolicy.hpp"
#include "Abi/PagerBootstrap.hpp"
#include "Kernel/Device/PciFunctionControl.hpp"
#include "Platform/Amd64/Boot/Amd64RuntimePoolLayout.hpp"
#include "Platform/Amd64/Device/Amd64MmioWindow.hpp"
#include "Platform/Amd64/Device/Amd64PhysicalMemoryAccess.hpp"
#include "Platform/Amd64/Diagnostics/Amd64EvidenceRecord.hpp"
#include "Platform/Amd64/Diagnostics/Amd64Stop.hpp"
#include "Platform/Amd64/Diagnostics/Amd64StopStage.hpp"
#include "Platform/Firmware/Diagnostic.hpp"

#if defined(ASTERIA_TARGET_AMD64)
void* operator new(Asteria::Foundation::Size, void*) noexcept;
#else
#include <new>
#endif

namespace Asteria::Platform::Amd64::Device
{
namespace
{
using Foundation::UInt64;
using Foundation::UInt8;
using Foundation::Size;
using Diagnostics::Amd64AhciEvidenceOutcome;
using Platform::Device::AhciIdentifyRuntimeError;
using Platform::Device::AhciIdentifyRuntimeQueryError;
using Platform::Device::AhciPciAvailability;
using Platform::Device::AhciReadDmaExtRuntimeError;
using Platform::Device::AhciReadDmaExtRuntimeQueryError;
using Platform::Device::PciEcamFunctionQueryError;
using Platform::Device::PciEcamRuntime;
using Platform::Device::PciEcamRuntimeSummary;
// The boot path's one scripted READ DMA EXT request: one sector at LBA 8.
// The host judges what arrived in the buffer (the evidence record names it).
constexpr UInt64 ReadStartingLba{8U};
constexpr UInt64 ReadSectorCount{1U};
constexpr UInt64 ReadTransferSizeBytes{512U};
// Constant, so its vtable pointer lives in.rodata: the PE layout places
// data after.reloc, where no base relocation may apply.
constexpr Amd64PhysicalMemoryAccess PhysicalMemoryAccess{};
alignas(PciEcamRuntime) UInt8 g_storage[sizeof(PciEcamRuntime)]{};
PciEcamRuntime* g_runtime{nullptr};
Kernel::Device::AhciIdentifyCommandReceipt g_identify{};
Kernel::Device::AhciReadDmaExtExecutionReceipt g_read{};
PciEcamRuntimeSummary g_summary{};

[[noreturn]] void Fail(const Diagnostics::Amd64BootStage stage, const UInt64 error) noexcept
{
    Diagnostics::StopAmd64(Kernel::Diagnostics::StopCode::BootStage, static_cast<UInt64>(stage), error);
}
[[nodiscard]] UInt8* Pointer(const UInt64 address) noexcept
{
    return reinterpret_cast<UInt8*>(static_cast<Foundation::UIntPointer>(address));
}

// Whether [base, base + bytes) overlaps a region of the memory map that holds
// RAM: a framebuffer is device memory, never a page the kernel may own.
[[nodiscard]] bool OverlapsRam(const ::Asteria::Boot::BootInfo& information, const UInt64 base,
                               const UInt64 bytes) noexcept
{
    using ::Asteria::Boot::PhysicalMemoryKind;
    for (Size index{0U}; index < information.MemoryRegionCount; ++index)
    {
        const auto& region = information.MemoryRegions[index];
        const PhysicalMemoryKind kind = region.Kind;
        const bool ram = (kind == PhysicalMemoryKind::Conventional) || (kind == PhysicalMemoryKind::Loader) ||
                         (kind == PhysicalMemoryKind::BootServices) || (kind == PhysicalMemoryKind::RuntimeServices) ||
                         (kind == PhysicalMemoryKind::AcpiReclaimable) ||
                         (kind == PhysicalMemoryKind::AcpiNonVolatile) || (kind == PhysicalMemoryKind::Persistent);
        const UInt64 end = region.PhysicalStart + (region.PageCount << 12U);
        if (ram && (region.PhysicalStart < (base + bytes)) && (base < end))
        {
            return true;
        }
    }
    return false;
}

// The boot's framebuffer as the scanout's DeviceRegion, when the boot recorded one that holds no
// RAM.
[[nodiscard, gnu::noinline]] bool PublishScanout(const Amd64BootstrapDeviceInput& input) noexcept
{
    const ::Asteria::Boot::BootDisplay& display = input.BootInformation->Display;
    if ((input.ScanoutRegion == nullptr) || (display.FrameBufferBase == 0U) ||
        OverlapsRam(*input.BootInformation, display.FrameBufferBase, display.FrameBufferBytes))
    {
        return true;
    }
    const UInt64 bytes = (display.FrameBufferBytes + 0xFFFU) & ~UInt64{0xFFFU};
    const auto scanout = g_runtime->PublishScanoutRegion(display.FrameBufferBase, bytes, input.PhysicalAddressBits,
                                                         input.ResourceIdentity, *input.Resources);
    if (!scanout.HasValue())
    {
        Fail(Diagnostics::Amd64BootStage::DeviceScanout, static_cast<UInt64>(scanout.GetError()));
    }
    *input.ScanoutRegion = scanout.GetValue();
    Kernel::Object::ObjectPin pin;
    if (input.Registry->PinObject(scanout.GetValue(), Kernel::Object::ObjectType::DeviceRegion, pin) !=
        Kernel::Object::ObjectPinError::Ok)
    {
        return false;
    }
    // The boot publication and pager's capability retain this region for the
    // runtime lifetime; terminal drawing borrows its lock-free policy word.
    Diagnostics::SetAmd64StopScanoutRegion(static_cast<Kernel::Device::DeviceRegion&>(pin.GetObject()));
    return true;
}

void RecordAhciOutcome(const Amd64AhciEvidenceOutcome outcome) noexcept
{
    auto& evidence = Diagnostics::GetAmd64EvidenceRecord().Ahci;
    evidence.Outcome = static_cast<UInt64>(outcome);
    Diagnostics::PublishAmd64Evidence(evidence.Published);
}

/// Records what the ECAM scan found: its regions and buses, the functions it
/// published, its first function and the function it bound as AHCI.
void RecordPci(const PciEcamRuntimeSummary& summary, const AhciPciAvailability availability) noexcept
{
    auto& evidence = Diagnostics::GetAmd64EvidenceRecord().Pci;
    const Kernel::Device::PciFunctionAddress first = summary.InitialFunction;
    const Kernel::Device::PciFunctionDescriptor& ahci = summary.AhciFunction;
    evidence.RegionCount = summary.RegionCount;
    evidence.BusCount = summary.BusCount;
    evidence.PublishedFunctionCount = summary.PublishedFunctionCount;
    evidence.FirstFunctionPresent = summary.InitialFunctionPresent ? 1U : 0U;
    evidence.FirstSegment = first.GetSegment();
    evidence.FirstBus = first.GetBus();
    evidence.FirstDevice = first.GetDevice();
    evidence.FirstFunction = first.GetFunction();
    evidence.FirstVendorDevice = summary.InitialVendorDevice;
    evidence.AhciAvailability = static_cast<UInt64>(availability);
    evidence.AhciPresent = summary.AhciFunctionPresent ? 1U : 0U;
    evidence.AhciSegment = ahci.Address.GetSegment();
    evidence.AhciBus = ahci.Address.GetBus();
    evidence.AhciDevice = ahci.Address.GetDevice();
    evidence.AhciFunction = ahci.Address.GetFunction();
    evidence.AhciVendorDevice = static_cast<UInt64>(ahci.VendorId) | (static_cast<UInt64>(ahci.DeviceId) << 16U);
    evidence.AhciClassCode = ahci.ClassCode;
    evidence.AhciSubclass = ahci.Subclass;
    evidence.AhciProgrammingInterface = ahci.ProgrammingInterface;
    Diagnostics::PublishAmd64Evidence(evidence.Published);
}

// "ASTERIA:AMD64-AHCI-<what>:SKIPPED:<error>:<phase>:<task file data>": a
// boot diagnostic that failed on a disk the runner did not attach is recorded
// and skipped rather than stopping the machine; nothing writes to the disk.
[[gnu::noinline]] void SkipDiagnostic(const char* const what, const UInt64 error, const UInt64 phase,
                                      const UInt64 taskFile, bool* const diskWritable) noexcept
{
    constexpr char Digits[] = "0123456789ABCDEF";
    char line[96]{};
    Size length{0U};
    const auto append = [&](const char* text) noexcept
    {
        for (; (*text != '\0') && (length < sizeof(line) - 1U); ++text)
        {
            line[length++] = *text;
        }
    };
    const auto hex = [&](const UInt64 value) noexcept
    {
        for (Size shift{32U}; shift != 0U; shift -= 4U)
        {
            if (length < sizeof(line) - 1U)
            {
                line[length++] = Digits[(value >> (shift - 4U)) & 0xFU];
            }
        }
    };
    append("ASTERIA:AMD64-AHCI-");
    append(what);
    append(":SKIPPED:");
    hex(error);
    append(":");
    hex(phase);
    append(":");
    hex(taskFile);
    append("\r\n");
    Platform::Firmware::WriteDiagnosticBytes(reinterpret_cast<const UInt8*>(line), length);
    RecordAhciOutcome(Amd64AhciEvidenceOutcome::DiagnosticFailed);
    if (diskWritable != nullptr)
    {
        *diskWritable = false;
    }
}

// "ASTERIA:XHCI:SKIPPED:<bus><device><function>:<step>:<error>": a controller
// the boot cannot publish (a BAR it cannot capture or a refused assignment)
// is left to the firmware and skipped; USB is not a reason to stop the machine.
[[gnu::noinline]] void SkipXhci(const Kernel::Device::PciFunctionAddress address, const UInt64 step,
                                const UInt64 error) noexcept
{
    Boot::Bootstrap::RecordBootInventoryXhciFailure(address.GetSegment(), address.GetBus(), address.GetDevice(),
                                                    address.GetFunction(), static_cast<UInt8>(step),
                                                    static_cast<UInt8>(error));
    constexpr char Digits[] = "0123456789ABCDEF";
    char line[64]{};
    Size length{0U};
    const auto append = [&](const char* text) noexcept
    {
        for (; (*text != '\0') && (length < sizeof(line) - 1U); ++text)
        {
            line[length++] = *text;
        }
    };
    const auto hex = [&](const UInt64 value, const Size digits) noexcept
    {
        for (Size shift{digits * 4U}; shift != 0U; shift -= 4U)
        {
            if (length < sizeof(line) - 1U)
            {
                line[length++] = Digits[(value >> (shift - 4U)) & 0xFU];
            }
        }
    };
    append("ASTERIA:XHCI:SKIPPED:");
    hex(address.GetBus(), 2U);
    hex(address.GetDevice(), 2U);
    hex(address.GetFunction(), 1U);
    append(":");
    hex(step, 1U);
    append(":");
    hex(error, 2U);
    append("\r\n");
    Platform::Firmware::WriteDiagnosticBytes(reinterpret_cast<const UInt8*>(line), length);
}

// After a refused IOMMU handoff no DMA device is published (the VT-d takeover in
// Documentation/Architecture/IommuDomain.md), except a function a verified BOOT.CFG trust entry
// names for its driver, which the trusted- driver profile then drives without DMA isolation; the
// kernel grants it after EnableIommu.
[[nodiscard]] bool TrustedAfterRefusedHandoff(const Amd64BootstrapDeviceInput& input,
                                              const Kernel::Device::PciFunctionAddress address,
                                              const ::Asteria::Boot::BootTrustDriver driver) noexcept
{
    for (Size index{0U}; index < g_summary.PublishedFunctionCount; ++index)
    {
        Platform::Device::PciFunctionPublication publication{};
        if (g_runtime->GetPublishedFunction(index, publication) != PciEcamFunctionQueryError::None)
        {
            return false;
        }
        const auto& descriptor = publication.Descriptor;
        if ((descriptor.Address.GetSegment() == address.GetSegment()) &&
            (descriptor.Address.GetBus() == address.GetBus()) &&
            (descriptor.Address.GetDevice() == address.GetDevice()) &&
            (descriptor.Address.GetFunction() == address.GetFunction()))
        {
            const Kernel::Device::TrustedDmaFunction function{.Address = descriptor.Address,
                                                              .Vendor = descriptor.VendorId,
                                                              .Product = descriptor.DeviceId,
                                                              .Driver = driver,
                                                              .Reserved = 0U};
            const auto decision = Kernel::Device::DecideTrustedDma(input.BootInformation->Trust, function,
                                                                   Kernel::Device::IommuAvailability::Refused);
            return decision.Outcome == Kernel::Device::TrustedDmaOutcome::Applied;
        }
    }
    return false;
}

// Which xHCI controllers PublishXhci publishes: every one, or after a refused
// handoff those a trust entry names, each other reported skipped at step 4 so
// the inventory's device lines and the console say why.
enum class XhciPublication : UInt8
{
    Every,
    TrustedOnly,
};

[[nodiscard, gnu::noinline]] bool PublishXhci(const Amd64BootstrapDeviceInput& input,
                                              const XhciPublication publication = XhciPublication::Every) noexcept
{
    Size xhciCount{0U};
    for (Size index{0U}; (input.XhciAssignable != nullptr) && (index < g_summary.PublishedFunctionCount); ++index)
    {
        Platform::Device::PciFunctionPublication published{};
        if (g_runtime->GetPublishedFunction(index, published) != PciEcamFunctionQueryError::None)
        {
            return false;
        }
        const auto& descriptor = published.Descriptor;
        if ((descriptor.ClassCode != 0x0CU) || (descriptor.Subclass != 3U) ||
            (descriptor.ProgrammingInterface != 0x30U))
        {
            continue;
        }
        if ((publication == XhciPublication::TrustedOnly) &&
            !TrustedAfterRefusedHandoff(input, descriptor.Address, ::Asteria::Boot::BootTrustDriver::Xhci))
        {
            SkipXhci(descriptor.Address, 4U, 0U);
            continue;
        }
        if (xhciCount == Abi::PagerXhciControllers)
        {
            Platform::Firmware::WriteDiagnosticAscii("ASTERIA:XHCI:PUBLICATION-CAPACITY\r\n");
            SkipXhci(descriptor.Address, 3U,
                     static_cast<UInt64>(Platform::Device::PciEcamRuntimeError::FunctionCapacityExceeded));
            continue;
        }
        Platform::Firmware::AhciPciResource firmwareResource{};
        const auto address = descriptor.Address;
        const bool captured = Boot::Bootstrap::GetBootInventoryXhciResource(
            address.GetSegment(), address.GetBus(), address.GetDevice(), address.GetFunction(), firmwareResource);
        // A running firmware controller must not have its BAR destructively
        // sized. Prefer the authoritative pre-exit firmware extent; the
        // runtime validates it against the complete 32/64-bit BAR again.
        const auto resource = captured
                                  ? Foundation::Result<Platform::Firmware::AhciPciResource,
                                                       Platform::Device::PciEcamRuntimeError>::Success(firmwareResource)
                                  : g_runtime->CaptureAssignableResource(address, 0U);
        if (!resource.HasValue())
        {
            SkipXhci(descriptor.Address, 1U, static_cast<UInt64>(resource.GetError()));
            continue;
        }
        const auto assignable = g_runtime->PublishAssignableDevice(resource.GetValue(), input.PhysicalAddressBits,
                                                                   input.ResourceIdentity, *input.Resources);
        if (!assignable.HasValue())
        {
            SkipXhci(descriptor.Address, 2U, static_cast<UInt64>(assignable.GetError()));
            continue;
        }
        input.XhciAssignable[xhciCount++] = assignable.GetValue();
        Boot::Bootstrap::RecordBootInventoryXhci(resource.GetValue(), static_cast<UInt8>(xhciCount));
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:XHCI:PUBLISHED\r\n");
    }
    return true;
}

// The USB 1.1/2.0 host controllers, every one or after a refused handoff
// those a trust entry names.
void PublishUsbHosts(const Amd64BootstrapDeviceInput& input, const bool trustedOnly) noexcept
{
    if (input.UsbHostAssignable == nullptr)
    {
        return;
    }
    ::Asteria::Boot::Bootstrap::UsbHostBootSite site{trustedOnly ? &input.BootInformation->Trust : nullptr};
    static_cast<void>(Platform::Device::PublishUsbHosts(
        Platform::Device::UsbHostPublicationInput{.Runtime = g_runtime,
                                                  .Site = &site,
                                                  .PhysicalAddressBits = input.PhysicalAddressBits,
                                                  .ResourceIdentity = input.ResourceIdentity,
                                                  .Resources = input.Resources},
        *input.UsbHostAssignable));
}

// After a refused handoff: the NIC, the xHCI and USB host controllers and the
// AHCI controller a trust entry names, the AHCI controller without the boot's
// own diagnostic, whose DMA the refusal forbids; every other one stays withheld.
[[nodiscard, gnu::noinline]] bool PublishTrustedAfterRefusedHandoff(const Amd64BootstrapDeviceInput& input) noexcept
{
    if ((input.Nic.AbarLengthBytes != 0U) && (input.NicAssignable != nullptr) &&
        TrustedAfterRefusedHandoff(input, {input.Nic.Segment, input.Nic.Bus, input.Nic.Device, input.Nic.Function},
                                   ::Asteria::Boot::BootTrustDriver::E1000e))
    {
        const auto nic = g_runtime->PublishAssignableDevice(input.Nic, input.PhysicalAddressBits,
                                                            input.ResourceIdentity, *input.Resources);
        if (!nic.HasValue())
        {
            Fail(Diagnostics::Amd64BootStage::DeviceNicAssignable, static_cast<UInt64>(nic.GetError()));
        }
        *input.NicAssignable = nic.GetValue();
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AMD64-NIC:PUBLISHED\r\n");
    }
    if (!PublishXhci(input, XhciPublication::TrustedOnly))
    {
        return false;
    }
    PublishUsbHosts(input, true);
    if ((g_runtime->GetAhciAvailability() == AhciPciAvailability::Ready) && (input.AhciAssignable != nullptr) &&
        TrustedAfterRefusedHandoff(input, {input.Ahci.Segment, input.Ahci.Bus, input.Ahci.Device, input.Ahci.Function},
                                   ::Asteria::Boot::BootTrustDriver::Ahci))
    {
        const auto ahci = g_runtime->PublishAhciDevice(input.Ahci);
        if (!ahci.HasValue())
        {
            Fail(Diagnostics::Amd64BootStage::DeviceAhciAssignable, static_cast<UInt64>(ahci.GetError()));
        }
        *input.AhciAssignable = ahci.GetValue();
    }
    return true;
}

[[nodiscard, gnu::noinline]] bool ReadSector(const Amd64BootstrapDeviceInput& input) noexcept
{
    const UInt64 command = Bootstrap::RuntimePoolPageAddress(input.RuntimePool, Bootstrap::AhciCommandPage);
    const UInt64 data = Bootstrap::RuntimePoolPageAddress(input.RuntimePool, Bootstrap::AhciReadDataPage);
    const UInt64 now = input.Clock->GetTicks();
    if (now > ~UInt64{0U} - 2'000'000'000U)
    {
        return false;
    }
    const auto result = g_runtime->ExecuteAhciReadDmaExt(
        *input.Clock, {Pointer(command), 4096U, command}, {data, ReadTransferSizeBytes}, ReadStartingLba,
        ReadSectorCount, {now + 2'000'000'000U, 5'000'000U, 1'000'000U});
    if (result.Error != AhciReadDmaExtRuntimeError::None)
    {
        if (input.AhciDiskWritable == nullptr || *input.AhciDiskWritable || result.Command.CommandPageMustRemainLive ||
            result.Command.DataBufferMustRemainLive)
        {
            Fail(Diagnostics::Amd64BootStage::DeviceRead, static_cast<UInt64>(result.Command.Error));
        }
        SkipDiagnostic("READ", static_cast<UInt64>(result.Command.Error), static_cast<UInt64>(result.Command.Phase),
                       result.Command.TaskFileData, input.AhciDiskWritable);
        return true;
    }
    // The receipt must describe the request the kernel made (API contract).
    if (g_runtime->GetAhciReadDmaExtReceipt(g_read) != AhciReadDmaExtRuntimeQueryError::None ||
        g_read.PortDisposition != Kernel::Device::AhciReadDmaExtPortDisposition::Restored ||
        g_read.PortNumber != g_identify.PortNumber || g_read.StartingLba != ReadStartingLba ||
        g_read.SectorCount != ReadSectorCount || g_read.TransferSizeBytes != ReadTransferSizeBytes)
    {
        Fail(Diagnostics::Amd64BootStage::DeviceReadReceipt, 0U);
    }
    // The buffer keeps the transferred bytes; the host compares them with the
    // disk it attached.
    auto& evidence = Diagnostics::GetAmd64EvidenceRecord().Ahci;
    evidence.ReadStartingLba = g_read.StartingLba;
    evidence.ReadSectorCount = g_read.SectorCount;
    evidence.ReadTransferSizeBytes = g_read.TransferSizeBytes;
    evidence.ReadBufferPhysicalAddress = data;
    evidence.ReadBufferSizeBytes = ReadTransferSizeBytes;
    RecordAhciOutcome(Amd64AhciEvidenceOutcome::Read);
    Platform::Firmware::WriteDiagnosticAscii("ASTERIA:STAGE:AMD64-AHCI-READ\r\n");
    return true;
}

[[nodiscard, gnu::noinline]] bool Identify(const Amd64BootstrapDeviceInput& input) noexcept
{
    const UInt64 physical = Bootstrap::RuntimePoolPageAddress(input.RuntimePool, Bootstrap::AhciIdentifyPage);
    const UInt64 now = input.Clock->GetTicks();
    if (now > ~UInt64{0U} - 2'000'000'000U)
    {
        return false;
    }
    const auto result = g_runtime->ExecuteAhciIdentify(*input.Clock, {Pointer(physical), 4096U, physical},
                                                       {now + 2'000'000'000U, 5'000'000U, 1'000'000U});
    if (result.Error == AhciIdentifyRuntimeError::CommandRejected &&
        result.Command.Error == Kernel::Device::AhciIdentifyCommandError::NoActiveDiskPort &&
        !result.Command.DmaPageMustRemainLive)
    {
        RecordAhciOutcome(Amd64AhciEvidenceOutcome::NoDisk);
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AMD64-AHCI-IDENTIFY:NO-DISK\r\n");
        if (input.AhciDiskWritable != nullptr)
        {
            *input.AhciDiskWritable = true;
        }
        return true;
    }
    // A controller whose active ports the diagnostic does not choose between
    // (an optical drive, a port multiplier, several disks: real machines and
    // other hypervisors) is not a boot failure. The diagnostic is skipped and,
    // since no disk was identified as the emulator's, nothing writes to one.
    const auto selection = result.Command.Error;
    if (result.Error == AhciIdentifyRuntimeError::CommandRejected && !result.Command.DmaPageMustRemainLive &&
        (selection == Kernel::Device::AhciIdentifyCommandError::AtapiDeviceNotSupported ||
         selection == Kernel::Device::AhciIdentifyCommandError::UnsupportedDeviceSignature ||
         selection == Kernel::Device::AhciIdentifyCommandError::AmbiguousActiveDiskPorts))
    {
        RecordAhciOutcome(Amd64AhciEvidenceOutcome::UnsupportedPorts);
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AMD64-AHCI-IDENTIFY:UNSUPPORTED-PORTS\r\n");
        if (input.AhciDiskWritable != nullptr)
        {
            *input.AhciDiskWritable = false;
        }
        return true;
    }
    if (result.Error != AhciIdentifyRuntimeError::None)
    {
        if (result.Command.DmaPageMustRemainLive)
        {
            Fail(Diagnostics::Amd64BootStage::DeviceIdentify, static_cast<UInt64>(result.Command.Error));
        }
        SkipDiagnostic("IDENTIFY", static_cast<UInt64>(result.Command.Error), static_cast<UInt64>(result.Command.Phase),
                       result.Command.TaskFileData, input.AhciDiskWritable);
        return true;
    }
    if (g_runtime->GetAhciIdentifyReceipt(g_identify) != AhciIdentifyRuntimeQueryError::None ||
        g_identify.PortDisposition != Kernel::Device::AhciIdentifyPortDisposition::Restored)
    {
        Fail(Diagnostics::Amd64BootStage::DeviceIdentifyReceipt, 0U);
    }
    // The raw IDENTIFY DEVICE data; the host decodes the disk's identity.
    static_assert(Kernel::Device::AhciIdentifyDmaPageBuilder::IdentifyDataSizeBytes ==
                  Diagnostics::Amd64EvidenceIdentifyDataBytes);
    auto& evidence = Diagnostics::GetAmd64EvidenceRecord().Ahci;
    const UInt8* const identify =
        Pointer(physical) + Kernel::Device::AhciIdentifyDmaPageBuilder::IdentifyDataOffsetBytes;
    for (Size index{0U}; index < Diagnostics::Amd64EvidenceIdentifyDataBytes; ++index)
    {
        evidence.IdentifyData[index] = identify[index];
    }
    Boot::Bootstrap::RecordBootInventoryDisk(g_identify.PortNumber, g_identify.Device.SectorCount,
                                             g_identify.Device.ModelNumber, g_identify.Device.ModelNumberLength);
    evidence.PortNumber = g_identify.PortNumber;
    evidence.Outcome = static_cast<UInt64>(Amd64AhciEvidenceOutcome::Identified);
    Platform::Firmware::WriteDiagnosticAscii("ASTERIA:STAGE:AMD64-AHCI-IDENTIFY\r\n");
    if (input.AhciDiskWritable != nullptr)
    {
        *input.AhciDiskWritable = Kernel::Device::IsEmulatorDisk(g_identify.Device);
    }
    return ReadSector(input);
}
} // Namespace

bool RunAmd64BootstrapDevices(const Amd64BootstrapDeviceInput& input) noexcept
{
    if (g_runtime != nullptr || input.BootInformation == nullptr || input.Registry == nullptr ||
        input.RegistryLock == nullptr || input.Issuer == nullptr || input.Resources == nullptr ||
        input.Clock == nullptr || !PrepareAmd64MmioWindow(input.RuntimePool))
    {
        return false;
    }
    const auto key = Kernel::Object::DeriveCapabilityValidationKey(input.BootInformation->Entropy,
                                                                   Kernel::Object::CapabilityKeyPurpose::DeviceSpace);
    if (!key.HasValue())
    {
        Fail(Diagnostics::Amd64BootStage::DeviceCapabilityKey, static_cast<UInt64>(key.GetError()));
    }
    g_runtime = new (g_storage)
        PciEcamRuntime{*input.Registry, *input.RegistryLock, *input.Issuer, key.GetValue(), PhysicalMemoryAccess};
    const auto prepared = g_runtime->Prepare(*input.BootInformation, input.Ahci, input.PhysicalAddressBits,
                                             input.ResourceIdentity, *input.Resources);
    if (!prepared.HasValue())
    {
        if (input.Handoff != nullptr)
        {
            static_cast<void>(input.Handoff(input.HandoffContext, g_runtime->GetConfigurationSpace()));
        }
        Fail(Diagnostics::Amd64BootStage::DeviceEcam, static_cast<UInt64>(prepared.GetError()));
    }
    if (g_runtime->GetSummary(g_summary) != PciEcamFunctionQueryError::None)
    {
        return false;
    }
    if ((input.VtdPlan != nullptr) && (input.VtdError != nullptr))
    {
        static Kernel::Device::PciFunctionAddress functions[VtdUnit::MaximumRequesters]{};
        const Size count = GetAmd64PciFunctions(functions, VtdUnit::MaximumRequesters);
        const auto& acpi = input.BootInformation->Acpi;
        *input.VtdError =
            (count > VtdUnit::MaximumRequesters) ? VtdTopologyError::FunctionCapacity
            : (acpi.DmarPhysicalAddress != 0U)
                ? Platform::Device::ResolveVtdTopology(acpi, g_runtime->GetConfigurationSpace(), functions, count,
                                                       *input.VtdPlan, Amd64VtdAddressBits)
                : ResolveAmdViTopology(acpi.Ivrs, g_runtime->GetConfigurationSpace(), functions, count, *input.VtdPlan);
        // Reservations survive a DRHD/topology refusal. They are authority
        // policy, independent of whether the machine can enable translation.
        for (Size index{0U}; index < input.VtdPlan->Count; ++index)
        {
            const auto& function = input.VtdPlan->Functions[index];
            if ((function.Regions != 0U) && !ReserveAmd64FirmwareDma(function.Address))
            {
                Fail(Diagnostics::Amd64BootStage::Iommu, index);
            }
        }
    }
    if ((input.Handoff != nullptr) && !input.Handoff(input.HandoffContext, g_runtime->GetConfigurationSpace()))
    {
        return false;
    }
    const AhciPciAvailability availability = g_runtime->GetAhciAvailability();
    RecordPci(g_summary, availability);
    Platform::Firmware::WriteDiagnosticAscii("ASTERIA:STAGE:AMD64-PCI\r\n");
    if (!PublishScanout(input))
    {
        return false;
    }
    if (input.DmaRefused != nullptr && *input.DmaRefused)
    {
        RecordAhciOutcome(Amd64AhciEvidenceOutcome::Unavailable);
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AMD64-AHCI:SKIPPED:IOMMU-HANDOFF-REFUSED\r\n");
        if (!PublishTrustedAfterRefusedHandoff(input))
        {
            return false;
        }
        return CloseAmd64MmioWindow();
    }
    if ((input.Device.AbarLengthBytes != 0U) && (input.Assignable != nullptr))
    {
        const auto assignable = g_runtime->PublishAssignableDevice(input.Device, input.PhysicalAddressBits,
                                                                   input.ResourceIdentity, *input.Resources);
        if (!assignable.HasValue())
        {
            Fail(Diagnostics::Amd64BootStage::DeviceAssignable, static_cast<UInt64>(assignable.GetError()));
        }
        *input.Assignable = assignable.GetValue();
    }
    if ((input.Nic.AbarLengthBytes != 0U) && (input.NicAssignable != nullptr))
    {
        const auto nic = g_runtime->PublishAssignableDevice(input.Nic, input.PhysicalAddressBits,
                                                            input.ResourceIdentity, *input.Resources);
        if (!nic.HasValue())
        {
            Fail(Diagnostics::Amd64BootStage::DeviceNicAssignable, static_cast<UInt64>(nic.GetError()));
        }
        *input.NicAssignable = nic.GetValue();
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AMD64-NIC:PUBLISHED\r\n");
    }
    if (!PublishXhci(input))
    {
        return false;
    }
    PublishUsbHosts(input, false);
    if (availability == AhciPciAvailability::Ready)
    {
        // The early firmware handoff swept this endpoint. Only the trusted
        // kernel diagnostic may temporarily bus-master on its reserved pages
        // while translation is disabled, before user assignment begins.
        const Kernel::Device::PciFunctionAddress address{input.Ahci.Segment, input.Ahci.Bus, input.Ahci.Device,
                                                         input.Ahci.Function};
        auto& configuration = g_runtime->GetConfigurationSpace();
        if (!Kernel::Device::SetPciBusMastering(configuration, configuration, address, true))
        {
            return false;
        }
        if (!Identify(input))
        {
            return false;
        }
        if (!Kernel::Device::SetPciBusMastering(configuration, configuration, address, false))
        {
            return false;
        }
        // The diagnostic's pages are never device-mapped again: a driver brings its own.
        if (input.AhciAssignable != nullptr)
        {
            const auto ahci = g_runtime->PublishAhciDevice(input.Ahci);
            if (!ahci.HasValue())
            {
                Fail(Diagnostics::Amd64BootStage::DeviceAhciAssignable, static_cast<UInt64>(ahci.GetError()));
            }
            *input.AhciAssignable = ahci.GetValue();
        }
    }
    else if (input.Ahci.AbarBaseAddress != 0U)
    {
        Fail(Diagnostics::Amd64BootStage::DeviceAhciBinding, static_cast<UInt64>(g_runtime->GetAhciFailure().Error));
    }
    else
    {
        RecordAhciOutcome(Amd64AhciEvidenceOutcome::Unavailable);
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AMD64-AHCI:UNAVAILABLE\r\n");
    }
    return CloseAmd64MmioWindow();
}

bool ReserveAmd64FirmwareDma(const Kernel::Device::PciFunctionAddress address) noexcept
{
    return (g_runtime != nullptr) && g_runtime->ReserveFirmwareDma(address);
}

Foundation::Size GetAmd64PciFunctions(Kernel::Device::PciFunctionAddress* const output,
                                      const Foundation::Size capacity) noexcept
{
    Foundation::Size count{0U};
    for (Foundation::Size index{0U}; (g_runtime != nullptr) && (output != nullptr) && (count < capacity) &&
                                     (index < g_summary.PublishedFunctionCount);
         ++index)
    {
        Platform::Device::PciFunctionPublication publication{};
        if (g_runtime->GetPublishedFunction(index, publication) == PciEcamFunctionQueryError::None)
        {
            output[count] = publication.Descriptor.Address;
            ++count;
        }
    }
    return (g_runtime != nullptr) ? g_summary.PublishedFunctionCount : count;
}
} // Namespace Asteria::Platform::Amd64::Device
