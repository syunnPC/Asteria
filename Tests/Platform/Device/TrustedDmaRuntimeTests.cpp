// SPDX-License-Identifier: Apache-2.0
#include "Platform/Device/TrustedDmaRuntime.hpp"

#include <cstring>

#include "Boot/BootInfo.hpp"
#include "Kernel/Boot/BootInventory.hpp"
#include "Kernel/Device/PciFunction.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Platform/Firmware/Diagnostic.hpp"

#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
// The debug channel's lines, kept for the checks.
char g_lines[2048]{};
Foundation::Size g_length{0U};

[[nodiscard]] bool Reported(const char* const line) noexcept
{
    return std::strstr(g_lines, line) != nullptr;
}
} // namespace
} // namespace Asteria::Test

namespace Asteria::Platform::Firmware
{
void WriteDiagnosticAscii(const char* text) noexcept
{
    for (; (*text != '\0') && (Asteria::Test::g_length + 1U < sizeof(Asteria::Test::g_lines)); ++text)
    {
        Asteria::Test::g_lines[Asteria::Test::g_length++] = *text;
    }
}
} // namespace Asteria::Platform::Firmware

namespace Asteria::Test
{
namespace
{
using Boot::BootTrustDriver;
using Boot::BootTrustVerdict;
using Kernel::Device::IommuAvailability;
using Kernel::Device::PciFunction;
using Kernel::Device::PciFunctionAddress;
using Kernel::Device::PciFunctionDescriptor;
using Kernel::Device::PciFunctionError;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Resource::ResourceDomain;
using Kernel::Resource::ResourceDomainError;
using Kernel::Resource::ResourceDomainLedger;
using Kernel::Resource::ResourceDomainLimits;
using Kernel::Resource::ResourceDomainObjectId;
using Platform::Device::TrustedDmaCandidate;
using Platform::Device::TrustedDmaRuntime;
using Platform::Synchronization::SpinLock;

constinit Boot::BootInfo g_information{};

[[nodiscard]] constexpr PciFunctionDescriptor
Describe(const PciFunctionAddress address, const Foundation::UInt16 vendor, const Foundation::UInt16 product,
         const Foundation::UInt8 classCode, const Foundation::UInt8 subclass,
         const Foundation::UInt8 interface) noexcept
{
    return PciFunctionDescriptor{.Address = address,
                                 .VendorId = vendor,
                                 .DeviceId = product,
                                 .RevisionId = 0U,
                                 .ProgrammingInterface = interface,
                                 .Subclass = subclass,
                                 .ClassCode = classCode,
                                 .HeaderLayout = 0U,
                                 .Multifunction = false};
}

// The q35 machine of the VM cases: the AHCI controller, the e1000e and the
// xHCI controller, each one published function.
constexpr PciFunctionDescriptor AhciFunction{Describe({0U, 0U, 0x1FU, 2U}, 0x8086U, 0x2922U, 1U, 6U, 1U)};
constexpr PciFunctionDescriptor NicFunction{Describe({0U, 0U, 0x10U, 0U}, 0x8086U, 0x10D3U, 2U, 0U, 0U)};
constexpr PciFunctionDescriptor XhciFunction{Describe({0U, 0U, 8U, 0U}, 0x1B36U, 0x000DU, 0x0CU, 3U, 0x30U)};

[[nodiscard]] Boot::BootTrustEntry Entry(const PciFunctionDescriptor& function, const Foundation::UInt8 selector,
                                         const BootTrustDriver driver, const BootTrustVerdict verdict) noexcept
{
    Boot::BootTrustEntry entry{};
    entry.Segment = function.Address.GetSegment();
    entry.Bus = function.Address.GetBus();
    entry.Device = function.Address.GetDevice();
    entry.Function = function.Address.GetFunction();
    entry.Selector = selector;
    entry.Vendor = function.VendorId;
    entry.Product = function.DeviceId;
    entry.Driver = driver;
    entry.Verdict = verdict;
    entry.Module = 0U;
    entry.Digest[0U] = 0xABU;
    return entry;
}

[[nodiscard]] Abi::InventoryPciEntry InventoryEntry(const PciFunctionDescriptor& function,
                                                    const Abi::InventoryDriver driver) noexcept
{
    Abi::InventoryPciEntry entry{};
    entry.Function.Segment = function.Address.GetSegment();
    entry.Function.Bus = function.Address.GetBus();
    entry.Function.Device = function.Address.GetDevice();
    entry.Function.Function = function.Address.GetFunction();
    entry.Function.Vendor = function.VendorId;
    entry.Function.Product = function.DeviceId;
    entry.Driver = driver;
    return entry;
}

// Three published functions in one registry, charged to one domain.
class Machine final
{
public:
    Machine() noexcept : Registry{Synchronization}, Issuer{Registry}, Resource{Ledger}
    {
    }

    [[nodiscard]] bool Initialize() noexcept
    {
        const ResourceDomainLimits limits{.CommittedMemoryPages = 0U,
                                          .ResidentMemoryPages = 0U,
                                          .KernelMetadataBytes = 1U << 20U,
                                          .CapabilityHandles = 0U,
                                          .IpcQueueBytes = 0U,
                                          .Threads = 0U,
                                          .ChildDomains = 0U};
        if (Resource.InitializeRoot(ResourceDomainId{61U}, limits).Error != ResourceDomainError::None)
        {
            return false;
        }
        const auto resource = Issuer.CreateObject(Resource);
        if (!resource.HasValue())
        {
            return false;
        }
        const ResourceDomainObjectId identity{Registry, resource.GetValue()};
        const PciFunctionDescriptor* const descriptors[3U]{&AhciFunction, &NicFunction, &XhciFunction};
        for (Foundation::Size index{0U}; index < 3U; ++index)
        {
            if (Functions[index].Initialize(Registry, identity, Resource, *descriptors[index]).Error !=
                PciFunctionError::None)
            {
                return false;
            }
            const auto registered = Issuer.CreateObject(Functions[index]);
            if (!registered.HasValue())
            {
                return false;
            }
            Objects[index] = registered.GetValue();
        }
        return true;
    }

    [[nodiscard]] Foundation::Size Apply(TrustedDmaRuntime& runtime, const IommuAvailability availability) noexcept
    {
        const TrustedDmaCandidate candidates[4U]{
            {Objects[0U], BootTrustDriver::Ahci, {}},
            {Objects[1U], BootTrustDriver::E1000e, {}},
            {Objects[2U], BootTrustDriver::Xhci, {}},
            {ObjectId{}, BootTrustDriver::Xhci, {}},
        };
        return runtime.Apply(Registry, g_information.Trust, g_information.Files, availability, candidates, 4U);
    }

    SpinLock Synchronization;
    Asteria::TestSupport::TestObjectRegistry Registry;
    RootCapabilityIssuer Issuer;
    ResourceDomainLedger Ledger;
    ResourceDomain Resource;
    PciFunction Functions[3U];
    ObjectId Objects[3U]{};
};

[[nodiscard]] const Abi::DeviceInventory& Inventory() noexcept
{
    return reinterpret_cast<const Abi::DeviceInventoryFrame*>(
               static_cast<Foundation::UIntPointer>(g_information.Inventory.PhysicalStart))
        ->Record;
}

[[nodiscard]] const Abi::DeviceInventoryTrust& Trust() noexcept
{
    return reinterpret_cast<const Abi::DeviceInventoryFrame*>(
               static_cast<Foundation::UIntPointer>(g_information.Inventory.PhysicalStart))
        ->Trust;
}

// No usable IOMMU: the verified entry for the AHCI controller applies; a
// digest mismatch, an entry for another driver than the function's and an
// entry without a function do not; the inventory and the debug channel say
// which.
void TestAbsent(TestContext& runner) noexcept
{
    g_information.InventoryRecord.Version = Abi::DeviceInventoryVersion;
    g_information.InventoryRecord.PciCount = 3U;
    g_information.InventoryRecord.Pci[0U] = InventoryEntry(AhciFunction, Abi::InventoryDriver::Ahci);
    g_information.InventoryRecord.Pci[1U] = InventoryEntry(NicFunction, Abi::InventoryDriver::E1000e);
    g_information.InventoryRecord.Pci[2U] = InventoryEntry(XhciFunction, Abi::InventoryDriver::Xhci);
    g_information.Files.ModuleCount = 1U;
    std::memcpy(g_information.Files.Modules[0U].Name, "component", 10U);
    g_information.Trust.Count = 4U;
    g_information.Trust.Entries[0U] =
        Entry(AhciFunction, Boot::BootTrustSelectsAddress | Boot::BootTrustSelectsIdentifier, BootTrustDriver::Ahci,
              BootTrustVerdict::Verified);
    g_information.Trust.Entries[1U] =
        Entry(XhciFunction, Boot::BootTrustSelectsIdentifier, BootTrustDriver::Xhci, BootTrustVerdict::DigestMismatch);
    g_information.Trust.Entries[2U] =
        Entry(NicFunction, Boot::BootTrustSelectsAddress, BootTrustDriver::Ahci, BootTrustVerdict::Verified);
    g_information.Trust.Entries[3U] =
        Entry(Describe({0U, 2U, 0U, 0U}, 0x10ECU, 0x8168U, 2U, 0U, 0U), Boot::BootTrustSelectsAddress,
              BootTrustDriver::E1000e, BootTrustVerdict::Verified);
    const bool imported = Boot::Bootstrap::ImportBootInventory(g_information);
    static Machine machine;
    static TrustedDmaRuntime runtime;
    const bool ready = machine.Initialize();
    const Foundation::Size trusted = machine.Apply(runtime, IommuAvailability::Absent);
    const auto& inventory = Inventory();
    ASTERIA_EXPECT(runner, "only the verified entry for the function's own driver is granted", imported, ready,
                   trusted == 1U, runtime.GetTrustedCount() == 1U, machine.Functions[0U].HasTrustedDma(),
                   !machine.Functions[1U].HasTrustedDma(), !machine.Functions[2U].HasTrustedDma());
    ASTERIA_EXPECT(runner, "the inventory marks the function and records every entry's outcome", Trust().Count == 4U,
                   inventory.Pci[0U].Dma == Abi::InventoryDma::Trusted, inventory.Pci[0U].TrustEntry == 1U,
                   inventory.Pci[1U].Dma == Abi::InventoryDma::Unknown,
                   inventory.Pci[2U].Dma == Abi::InventoryDma::Unknown,
                   Trust().Entries[0U].Outcome == Abi::InventoryTrustOutcome::Applied,
                   Trust().Entries[1U].Outcome == Abi::InventoryTrustOutcome::DigestMismatch,
                   Trust().Entries[2U].Outcome == Abi::InventoryTrustOutcome::DriverMismatch,
                   Trust().Entries[3U].Outcome == Abi::InventoryTrustOutcome::NoFunction,
                   std::strcmp(Trust().Entries[0U].Module, "component") == 0, Trust().Entries[0U].Digest[0U] == 0xABU,
                   Trust().Entries[0U].Driver == static_cast<Foundation::UInt8>(BootTrustDriver::Ahci),
                   Trust().Entries[3U].Bus == 2U);
    ASTERIA_EXPECT(runner, "the debug channel reports the IOMMU and each decision",
                   Reported("ASTERIA:DMA-TRUST:IOMMU:ABSENT\r\n"),
                   Reported("ASTERIA:DMA-TRUST:0000:00:1f.2:ahci:ENTRY-1:TRUSTED-NO-DMA-ISOLATION\r\n"),
                   Reported("ASTERIA:DMA-TRUST:0000:00:08.0:xhci:ENTRY-2:REFUSED-DIGEST-MISMATCH\r\n"),
                   Reported("ASTERIA:DMA-TRUST:0000:00:10.0:e1000e:ENTRY-3:REFUSED-DRIVER-MISMATCH\r\n"),
                   Reported("ASTERIA:DMA-TRUST:ENTRY-4:NO-FUNCTION\r\n"));
}

// A working IOMMU leaves every entry unused and grants nothing; a refused
// one applies the entry as an absent one does; no entries, nothing at all.
void TestWorkingAndRefused(TestContext& runner) noexcept
{
    static Machine working;
    static TrustedDmaRuntime confined;
    g_length = 0U;
    std::memset(g_lines, 0, sizeof(g_lines));
    const bool ready = working.Initialize();
    const Foundation::Size none = working.Apply(confined, IommuAvailability::Working);
    ASTERIA_EXPECT(runner, "an IOMMU that works confines the function; its entry is reported unused", ready, none == 0U,
                   !working.Functions[0U].HasTrustedDma(),
                   Trust().Entries[0U].Outcome == Abi::InventoryTrustOutcome::IommuConfines,
                   Reported("ASTERIA:DMA-TRUST:IOMMU:WORKING\r\n"),
                   Reported("ASTERIA:DMA-TRUST:0000:00:1f.2:ahci:ENTRY-1:UNUSED-IOMMU-CONFINES\r\n"));
    static Machine refused;
    static TrustedDmaRuntime applied;
    const bool refusedReady = refused.Initialize();
    const Foundation::Size one = refused.Apply(applied, IommuAvailability::Refused);
    ASTERIA_EXPECT(runner, "an IOMMU the boot refused leaves the entry to apply", refusedReady, one == 1U,
                   refused.Functions[0U].HasTrustedDma(), Reported("ASTERIA:DMA-TRUST:IOMMU:REFUSED\r\n"));
    static Machine empty;
    static TrustedDmaRuntime nothing;
    g_length = 0U;
    std::memset(g_lines, 0, sizeof(g_lines));
    g_information.Trust.Count = 0U;
    const bool emptyReady = empty.Initialize();
    ASTERIA_EXPECT(runner, "without entries nothing is decided or reported", emptyReady,
                   empty.Apply(nothing, IommuAvailability::Absent) == 0U, !empty.Functions[0U].HasTrustedDma(),
                   g_length == 0U);
}
// A display, which nothing may assign, never becomes a trusted function,
// whatever an entry for it says.
void TestNotAssignable(TestContext& runner) noexcept
{
    static SpinLock synchronization;
    static Asteria::TestSupport::TestObjectRegistry registry{synchronization};
    static RootCapabilityIssuer issuer{registry};
    static ResourceDomainLedger ledger;
    static ResourceDomain resource{ledger};
    static PciFunction display;
    static TrustedDmaRuntime runtime;
    const ResourceDomainLimits limits{.CommittedMemoryPages = 0U,
                                      .ResidentMemoryPages = 0U,
                                      .KernelMetadataBytes = 1U << 20U,
                                      .CapabilityHandles = 0U,
                                      .IpcQueueBytes = 0U,
                                      .Threads = 0U,
                                      .ChildDomains = 0U};
    const bool resourceReady =
        resource.InitializeRoot(ResourceDomainId{62U}, limits).Error == ResourceDomainError::None;
    const auto resourceObject = issuer.CreateObject(resource);
    constexpr PciFunctionDescriptor DisplayFunction{Describe({0U, 0U, 2U, 0U}, 0x1234U, 0x1111U, 3U, 0U, 0U)};
    const bool displayReady = resourceObject.HasValue() &&
                              (display
                                   .Initialize(registry, ResourceDomainObjectId{registry, resourceObject.GetValue()},
                                               resource, DisplayFunction)
                                   .Error == PciFunctionError::None);
    const auto displayObject = issuer.CreateObject(display);
    g_length = 0U;
    std::memset(g_lines, 0, sizeof(g_lines));
    g_information.Trust.Count = 1U;
    g_information.Trust.Entries[0U] =
        Entry(DisplayFunction, Boot::BootTrustSelectsAddress, BootTrustDriver::Xhci, BootTrustVerdict::Verified);
    const TrustedDmaCandidate candidate{
        displayObject.HasValue() ? displayObject.GetValue() : ObjectId{}, BootTrustDriver::Xhci, {}};
    const Foundation::Size trusted =
        runtime.Apply(registry, g_information.Trust, g_information.Files, IommuAvailability::Absent, &candidate, 1U);
    ASTERIA_EXPECT(runner, "a function nothing may assign is refused, and the inventory says why", resourceReady,
                   displayReady, displayObject.HasValue(), trusted == 0U, !display.HasTrustedDma(),
                   Trust().Entries[0U].Outcome == Abi::InventoryTrustOutcome::NotAssignable,
                   Reported("ASTERIA:DMA-TRUST:0000:00:02.0:xhci:ENTRY-1:REFUSED-NOT-ASSIGNABLE\r\n"));
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"TrustedDmaRuntimeTests"};
    Asteria::Test::TestAbsent(runner);
    Asteria::Test::TestWorkingAndRefused(runner);
    Asteria::Test::TestNotAssignable(runner);
    return runner.Finish();
}
