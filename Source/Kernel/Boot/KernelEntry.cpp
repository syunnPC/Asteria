// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Boot/KernelEntry.hpp"

#include "Boot/KernelEntryAbi.hpp"
#include "Runtime/Placement.hpp"

#include "Boot/BootInfoValidation.hpp"
#include "Runtime/Memory.hpp"
#include "Kernel/Boot/BootInventory.hpp"
#include "Kernel/Boot/UsbHostBootSite.hpp"

#include "Boot/Acpi/AcpiTableParser.hpp"
#include "Boot/Acpi/I8042Evidence.hpp"
#include "Boot/BootInfo.hpp"
#include "Foundation/Compiler.hpp"
#include "Kernel/Memory/BootstrapPageAllocator.hpp"
#include "Kernel/Memory/PhysicalPageImporter.hpp"
#include "Kernel/Device/FirmwareBusMasterGate.hpp"
#include "Kernel/Memory/TlbShootdownDispatcher.hpp"
#include "Platform/Firmware/Diagnostic.hpp"

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
#include "Platform/Amd64/Boot/Amd64BootRuntime.hpp"
#include "Platform/Amd64/Boot/Amd64MetadataArena.hpp"
#include "Platform/Amd64/Time/Amd64TscClock.hpp"
#include "Platform/Amd64/Diagnostics/Amd64EvidenceRecord.hpp"
#include "Platform/Amd64/Memory/InitialAddressSpacePreparation.hpp"
#include "Platform/Amd64/Memory/BootstrapMappingPlan.hpp"
#include "Platform/Amd64/Firmware/DiagnosticSinkSelection.hpp"
#include "Platform/Amd64/Memory/InitialPagingCapabilityCapture.hpp"
#elif defined(ASTERIA_TARGET_IA64)
#include "Kernel/Device/AhciEmulatorDisk.hpp"
#include "Kernel/Device/AhciIdentifyDmaPageBuilder.hpp"
#include "Kernel/Device/AhciReadDmaExtCommandExecutor.hpp"
#include "Kernel/Device/PciFunctionControl.hpp"
#include "Kernel/Device/TrustedDmaPolicy.hpp"
#include "Kernel/Memory/TlbShootdownMailboxTransport.hpp"
#include "Kernel/Object/CapabilityKeyDerivation.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Platform/Device/PciEcamRuntime.hpp"
#if defined(ASTERIA_TARGET_IA64)
#include "Platform/Ia64/Device/Ia64IommuRuntime.hpp"
#endif
#include "Platform/Ia64/Device/Ia64PhysicalMemoryAccess.hpp"
#include "Platform/Ia64/Diagnostics/Ia64EvidenceRecord.hpp"
#include "Platform/Ia64/Executable/Ia64InitialUserImageMappingOwner.hpp"
#include "Platform/Ia64/Executable/Ia64InitialUserImageMemoryOwner.hpp"
#include "Platform/Ia64/Executable/Ia64InitialUserImageOwner.hpp"
#include "Platform/Firmware/PciResource.hpp"
#include "Platform/Ia64/Firmware/Ia64DebugUart.hpp"
#include "Platform/Ia64/Firmware/Ia64DiagnosticWriter.hpp"
#include "Platform/Ia64/Firmware/Ia64LoadedImageLayout.hpp"
#include "Platform/Ia64/Firmware/PalBootstrapCapabilityCapture.hpp"
#include "Platform/Ia64/Firmware/SalBootRendezvousRegistration.hpp"
#include "Platform/Ia64/Firmware/SalEntrypointBinder.hpp"
#include "Platform/Ia64/Interrupt/Ia64ApplicationProcessorRuntimeReleaseCoordinator.hpp"
#include "Platform/Ia64/Interrupt/Ia64ApplicationProcessorTakeoverCoordinator.hpp"
#include "Platform/Ia64/Interrupt/Ia64InterruptLines.hpp"
#include "Platform/Ia64/Interrupt/Ia64IoSapic.hpp"
#include "Platform/Ia64/Interrupt/Ia64LocalSapicIpiController.hpp"
#include "Platform/Ia64/Interrupt/Ia64SalBootRendezvousHandler.hpp"
#include "Platform/Ia64/Memory/ApplicationProcessorExecutionStorageOwner.hpp"
#include "Platform/Ia64/Memory/InitialAddressSpacePreparation.hpp"
#include "Platform/Ia64/Memory/InitialExecutionContextSwitcher.hpp"
#include "Platform/Ia64/Memory/InitialInterruptVectorTableInstaller.hpp"
#include "Platform/Ia64/Memory/InitialTranslationActivator.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeMetadataLayout.hpp"
#include "Platform/Ia64/Memory/InitialTranslationInstaller.hpp"
#include "Platform/Ia64/Memory/Ia64ApplicationProcessorRuntimeMappingOwner.hpp"
#include "Platform/Ia64/Memory/Ia64ApplicationProcessorRuntimePivot.hpp"
#include "Platform/Ia64/Memory/Ia64ApplicationProcessorRuntimeWorkerStorageOwner.hpp"
#include "Platform/Ia64/Memory/Ia64BspRuntimeMemory.hpp"
#include "Platform/Ia64/Diagnostics/Ia64StopStage.hpp"
#include "Platform/Ia64/Memory/Ia64LocalTlbInvalidator.hpp"
#include "Platform/Ia64/Memory/Ia64PolledTlbShootdownNotifier.hpp"
#include "Platform/Ia64/Memory/Ia64TranslationRegisterBudget.hpp"
#include "Platform/Ia64/Scheduler/Ia64ApplicationProcessorKernelRuntime.hpp"
#include "Platform/Ia64/Scheduler/Ia64BspKernelRuntime.hpp"
#include "Platform/Ia64/System/Ia64PagerRuntime.hpp"
#include "Platform/Ia64/Ia64UserVirtualLayout.hpp"
#include "Abi/PagerBootstrap.hpp"
#include "Platform/Ia64/Scheduler/Ia64CrossProcessorWaitBudget.hpp"
#include "Kernel/Scheduler/CrossProcessorWait.hpp"
#include "Platform/Ia64/Scheduler/Ia64UserExecutionProbePayload.hpp"
#include "Platform/Ia64/Time/Ia64LocalTimer.hpp"
#include "Platform/Synchronization/SpinLock.hpp"
#endif

namespace Asteria::Boot::Bootstrap
{
#if defined(ASTERIA_TARGET_AMD64)
Foundation::UInt64 g_amd64RuntimePool{0U};
Platform::Firmware::AhciPciResource g_amd64AhciResource{};
// The test firmware's assignable device, QEMU's edu (vendor 0x1234, device 0x11E8), and its BAR0;
// zero where absent.
Platform::Firmware::AhciPciResource g_amd64DeviceResource{};
Platform::Firmware::AhciPciResource g_amd64NicResource{};
Foundation::UInt64 g_amd64StartupPages{0U};
alignas(8) Platform::Amd64::Firmware::Amd64LoadedImageLayout g_amd64LoadedImageLayout;
constinit Kernel::Memory::InitialMappingPlan g_amd64InitialMappingPlan;
alignas(8) Platform::Amd64::Memory::BootstrapMappingPlanSummary g_amd64BootstrapMappingPlanSummary;
alignas(8) Kernel::Memory::PhysicalPageRange g_amd64BootstrapReservation;
alignas(4) constinit Platform::Amd64::Memory::PagingCapabilities g_amd64InitialPagingCapabilities{};
alignas(8) constinit Platform::Amd64::Memory::PageTableStorage g_amd64PageTableStorage{};
alignas(8) Platform::Amd64::Memory::WritableInitialMemoryStackView g_amd64InitialMemoryStackView;
alignas(8) Platform::Amd64::Memory::WritableInitialDescriptorTableView g_amd64InitialDescriptorTableView;
alignas(8) Platform::Amd64::Memory::InitialProcessorStateReport g_amd64InitialProcessorStateReport;
constinit Platform::Amd64::Memory::InitialAddressSpacePreparation g_amd64InitialAddressSpacePreparation;
// Why the final-map preparation rejected the last candidate map, for the
// failure report after the transaction (ReportBootTransitionFailure): the
// step's record, and for the TABLE step the capture that refused.
constexpr char Amd64FinalMemoryMapTableRejection[]{"ASTERIA:AMD64-FINAL-MEMORY-MAP:FAIL:TABLE\r\n"};
constinit const char* g_amd64FinalMemoryMapRejection{nullptr};
constinit Platform::Amd64::Memory::InitialPagingCapabilityCaptureError g_amd64PagingCapabilityError{
    Platform::Amd64::Memory::InitialPagingCapabilityCaptureError::None};
constinit Platform::Amd64::Memory::InitialProcessorStateCaptureError g_amd64ProcessorStateCaptureError{
    Platform::Amd64::Memory::InitialProcessorStateCaptureError::None};
#elif defined(ASTERIA_TARGET_IA64)
[[nodiscard]] constexpr Platform::Ia64::Memory::InitialTranslationRegionSet MakeIa64InitialTranslationRegions() noexcept
{
    Platform::Ia64::Memory::InitialTranslationRegionSet regions{};
    for (Foundation::Size index{0U}; index < Platform::Ia64::Memory::InitialTranslationRegionCount; ++index)
    {
        regions.Regions[index] = Platform::Ia64::Memory::RegionConfiguration{
            .RegionIdentifier = static_cast<Foundation::UInt32>(0x2000U + index),
            .ProtectionKey = 0U,
            .VhptWalkerEnabled = false,
            .SharedAcrossAddressSpaces = index == 0U,
            .Reserved16 = 0U,
            .Reserved32 = 0U,
        };
    }
    return regions;
}

alignas(8) Platform::Ia64::Firmware::SalSystemTableInformation g_ia64SalSystemTableInformation;
alignas(8) Platform::Ia64::Firmware::SalEntrypointBinding g_ia64SalEntrypointBinding;
alignas(8) Platform::Ia64::Firmware::PalBootstrapCapabilities g_ia64PalBootstrapCapabilities;
alignas(8) Platform::Firmware::AhciPciResource g_ia64AhciPciResource;
alignas(8) Platform::Ia64::Memory::PalVirtualMemoryCapabilities
    g_ia64RuntimeProcessorCapabilities[Kernel::Scheduler::MaximumProcessorCount]{};
alignas(8) Platform::Ia64::Memory::Ia64CommonRuntimeProcessorCapabilities g_ia64CommonRuntimeProcessorCapabilities{};
Foundation::UInt8 g_ia64RuntimeProfiledProcessorMask{0U};
alignas(8) constinit Kernel::Memory::PhysicalPageRange g_ia64SharedRootShootdownPage{};
constinit Platform::Ia64::Memory::Ia64PolledTlbShootdownNotifier g_ia64TlbShootdownNotifier{};
alignas(Kernel::Memory::TlbShootdownMailboxTransport) constinit Foundation::UInt8
    g_ia64TlbShootdownMailboxStorage[sizeof(Kernel::Memory::TlbShootdownMailboxTransport)]{};
constinit Kernel::Memory::TlbShootdownMailboxTransport* g_ia64TlbShootdownMailbox{nullptr};
alignas(8) constinit Platform::Ia64::Firmware::Ia64LoadedImageLayout g_ia64LoadedImageLayout{};
constinit Platform::Ia64::Memory::InitialAddressSpacePreparation g_ia64InitialAddressSpacePreparation;
constinit Platform::Ia64::Memory::InitialTranslationRegionSet g_ia64InitialTranslationRegions{
    MakeIa64InitialTranslationRegions()};
alignas(8) Platform::Ia64::Memory::WritableInterruptVectorTableView g_ia64InterruptVectorTableView;
alignas(8) Platform::Ia64::Memory::WritableRseBackingStoreView g_ia64RseBackingStoreView;
alignas(8) Platform::Ia64::Memory::WritableInitialMemoryStackView g_ia64InitialMemoryStackView;
alignas(8) Platform::Ia64::Memory::InitialInterruptVectorTableInstallSummary
    g_ia64InitialInterruptVectorTableInstallSummary;
constinit Platform::Ia64::Interrupt::Ia64LocalSapicIpiController g_ia64LocalSapicIpiController;
constinit Platform::Ia64::Memory::ApplicationProcessorExecutionStorageOwner
    g_ia64ApplicationProcessorExecutionStorageOwner;
constinit Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeWorkerStorageOwner
    g_ia64ApplicationProcessorRuntimeWorkerStorageOwner;
constinit Platform::Ia64::Memory::Ia64LocalTlbInvalidator g_ia64ApplicationProcessorRuntimeMappingInvalidator;
constinit Kernel::Memory::TlbShootdownDispatcher g_ia64ApplicationProcessorRuntimeMappingDispatcher{
    Kernel::Scheduler::CpuId{0U}, g_ia64ApplicationProcessorRuntimeMappingInvalidator};
alignas(Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeMappingOwner) constinit Foundation::UInt8
    g_ia64ApplicationProcessorRuntimeMappingOwnerStorage[sizeof(
        Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeMappingOwner)]{};
Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeMappingOwner* g_ia64ApplicationProcessorRuntimeMappingOwner{
    nullptr};
constinit Platform::Ia64::Interrupt::Ia64ApplicationProcessorRuntimeReleaseCoordinator
    g_ia64ApplicationProcessorRuntimeReleaseCoordinator;
// This runtime has firmware boot lifetime. Construct it only when its AP
// dependencies are ready, and intentionally never register a destructor.
alignas(Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntime) constinit Foundation::UInt8
    g_ia64ApplicationProcessorKernelRuntimeStorage[sizeof(
        Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntime)]{};
constinit Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntime*
    g_ia64ApplicationProcessorKernelRuntimeOwner{nullptr};
alignas(8) constinit Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeProcessorSnapshot
    g_ia64ApplicationProcessorKernelRuntimeSnapshot{};
constinit Platform::Synchronization::SpinLock g_ia64BspRegistryLock;
constinit Kernel::Object::ObjectRegistry g_ia64BspObjectRegistry{g_ia64BspRegistryLock};
constinit Kernel::Object::RootCapabilityIssuer g_ia64BspRootCapabilityIssuer{g_ia64BspObjectRegistry};
constinit Kernel::Resource::ResourceDomainLedger g_ia64RootResourceDomainLedger;
alignas(Kernel::Resource::ResourceDomain) constinit Foundation::UInt8
    g_ia64RootResourceDomainStorage[sizeof(Kernel::Resource::ResourceDomain)]{};
Kernel::Resource::ResourceDomain* g_ia64RootResourceDomain{nullptr};
alignas(8) Kernel::Resource::ResourceDomainObjectId g_ia64RootResourceDomainIdentity;
// The run-time memory reservation: one naturally aligned page, mapped by one locked DTR, that holds
// Ia64BspRuntimeMemory at its start
alignas(8) constinit Kernel::Memory::PhysicalPageRange g_ia64RuntimeMemoryReservation{};
Platform::Ia64::Memory::Ia64BspRuntimeMemory* g_ia64BspRuntimeMemoryOwner{nullptr};
alignas(Platform::Device::PciEcamRuntime) constinit Foundation::UInt8
    g_ia64PciEcamRuntimeStorage[sizeof(Platform::Device::PciEcamRuntime)]{};
Platform::Device::PciEcamRuntime* g_ia64PciEcamRuntimeOwner{nullptr};
alignas(Platform::Ia64::Device::Ia64IommuRuntime) constinit Foundation::UInt8
    g_ia64IommuStorage[sizeof(Platform::Ia64::Device::Ia64IommuRuntime)]{};
Platform::Ia64::Device::Ia64IommuRuntime* g_ia64Iommu{nullptr};
constinit Platform::Ia64::Device::Ia64PhysicalMemoryAccess g_ia64PhysicalMemoryAccess{};
// The assignable device the boot grants the IA-64 pager, QEMU's edu (vendor 0x1234, device 0x11E8),
// and its BAR0; invalid identities where no case attached it.
constexpr Foundation::UInt32 Ia64AssignableIdentifier{0x11E8'1234U};
constexpr Foundation::UInt8 Ia64AssignableBar{0U};
Platform::Device::AssignableDevice g_ia64AssignableDevice{};
// The AHCI controller, published as the second assignable device once the boot's diagnostic ran,
// And whether it holds no disk but the emulator's (Abi::PagerManagementDiskWritable); invalid and
// false where no controller is bound.
Platform::Device::AssignableDevice g_ia64AhciDevice{};
bool g_ia64AhciDiskWritable{false};
// The USB 1.1/2.0 host controllers published for the USB host group driver
// (Platform/Device/UsbHostPublication.hpp), the device table's USB host
// entries in order.
Platform::Device::AssignableDevice g_ia64UsbHostDevices[Abi::PagerUsbHostControllers]{};
// The NIC the boot grants the IA-64 pager, QEMU's e1000e (82574L: vendor 0x8086, device 0x10D3),
// and its register BAR0; invalid where no case attached one.
constexpr Foundation::UInt32 Ia64NicIdentifier{0x10D3'8086U};
constexpr Foundation::UInt8 Ia64NicBar{0U};
Platform::Device::AssignableDevice g_ia64NicDevice{};
Kernel::Object::ObjectId g_ia64ScanoutRegion{};
// The I/O SAPIC that routes the device's INTx line and the lines its Interrupts take; unconfigured
// Where the MADT has none for it.
constinit Platform::Ia64::Interrupt::Ia64IoSapic g_ia64DeviceIoSapics[Boot::Acpi::MaximumAcpiIoControllers]{};
constinit Platform::Ia64::Interrupt::Ia64InterruptLines g_ia64DeviceInterruptLines{};
bool g_ia64DeviceInterruptsConfigured{false};
struct alignas(Kernel::Device::AhciReadDmaExtCommandBuilder::CommandPageAlignmentBytes) Ia64AhciCommandPageStorage
{
    Foundation::UInt8 Bytes[Kernel::Device::AhciReadDmaExtCommandBuilder::CommandPageSizeBytes];
};
struct alignas(Kernel::Device::AhciReadDmaExtCommandBuilder::CommandPageAlignmentBytes) Ia64AhciReadDataPageStorage
{
    Foundation::UInt8 Bytes[Kernel::Device::AhciReadDmaExtCommandBuilder::CommandPageSizeBytes];
};
constinit Ia64AhciCommandPageStorage g_ia64AhciCommandPage{};
constinit Ia64AhciReadDataPageStorage g_ia64AhciReadDataPage{};
constinit Platform::Ia64::Time::Ia64LocalTimer g_ia64AhciPollingClock;
// Reads the BSP's ITC for the budgets of its waits for AP work.
constinit Platform::Ia64::Time::Ia64LocalTimer g_ia64ApWaitClock;
// The ITC ticks of one microsecond, measured across the boot's firmware stall; zero if the stall
// did not run or the count is out of range. The pager's wait timeouts use it.
constinit Foundation::UInt64 g_ia64ItcTicksPerMicrosecond{0U};
alignas(Platform::Ia64::Scheduler::Ia64BspKernelRuntime) constinit Foundation::UInt8
    g_ia64BspKernelRuntimeStorage[sizeof(Platform::Ia64::Scheduler::Ia64BspKernelRuntime)]{};
#if ASTERIA_IA64_SYNTHETIC_USER_PROBE
alignas(Platform::Ia64::Scheduler::Ia64UserExecutionProbe) constinit Foundation::UInt8
    g_ia64UserExecutionProbeStorage[sizeof(Platform::Ia64::Scheduler::Ia64UserExecutionProbe)]{};
Platform::Ia64::Scheduler::Ia64UserExecutionProbe* g_ia64UserExecutionProbeOwner{nullptr};
#endif
alignas(Platform::Ia64::Executable::Ia64InitialUserImageOwner) constinit Foundation::UInt8
    g_ia64InitialUserImageOwnerStorage[sizeof(Platform::Ia64::Executable::Ia64InitialUserImageOwner)]{};
constinit Platform::Ia64::Executable::Ia64InitialUserImageOwner* g_ia64InitialUserImageOwner{nullptr};
alignas(Platform::Ia64::Executable::Ia64InitialUserImageMemoryOwner) constinit Foundation::UInt8
    g_ia64InitialUserImageMemoryOwnerStorage[sizeof(Platform::Ia64::Executable::Ia64InitialUserImageMemoryOwner)]{};
constinit Platform::Ia64::Executable::Ia64InitialUserImageMemoryOwner* g_ia64InitialUserImageMemoryOwner{nullptr};
alignas(Platform::Ia64::Executable::Ia64InitialUserImageMappingOwner) constinit Foundation::UInt8
    g_ia64InitialUserImageMappingOwnerStorage[sizeof(Platform::Ia64::Executable::Ia64InitialUserImageMappingOwner)]{};
constinit Platform::Ia64::Executable::Ia64InitialUserImageMappingOwner* g_ia64InitialUserImageMappingOwner{nullptr};
// The boot pager's image, staged, materialized and mapped into its own root
// by the same one-shot owners.
alignas(Platform::Ia64::Executable::Ia64InitialUserImageOwner) constinit Foundation::UInt8
    g_ia64PagerImageOwnerStorage[sizeof(Platform::Ia64::Executable::Ia64InitialUserImageOwner)]{};
constinit Platform::Ia64::Executable::Ia64InitialUserImageOwner* g_ia64PagerImageOwner{nullptr};
alignas(Platform::Ia64::Executable::Ia64InitialUserImageMemoryOwner) constinit Foundation::UInt8
    g_ia64PagerImageMemoryOwnerStorage[sizeof(Platform::Ia64::Executable::Ia64InitialUserImageMemoryOwner)]{};
constinit Platform::Ia64::Executable::Ia64InitialUserImageMemoryOwner* g_ia64PagerImageMemoryOwner{nullptr};
alignas(Platform::Ia64::Executable::Ia64InitialUserImageMappingOwner) constinit Foundation::UInt8
    g_ia64PagerImageMappingOwnerStorage[sizeof(Platform::Ia64::Executable::Ia64InitialUserImageMappingOwner)]{};
constinit Platform::Ia64::Executable::Ia64InitialUserImageMappingOwner* g_ia64PagerImageMappingOwner{nullptr};
// The Frame of the initial image's lazy region the pager serves, and the
// pager's runtime.
constinit Kernel::Memory::PhysicalPageRange g_ia64LazyRegion{};
constinit Kernel::Memory::PhysicalPageRange g_ia64FramePool{};
alignas(Platform::Ia64::System::Ia64PagerRuntime) constinit Foundation::UInt8
    g_ia64PagerRuntimeStorage[sizeof(Platform::Ia64::System::Ia64PagerRuntime)]{};
// The native slots of the threads the pager creates.
alignas(Platform::Ia64::System::Ia64UserThreadTable) constinit Foundation::UInt8
    g_ia64UserThreadTableStorage[sizeof(Platform::Ia64::System::Ia64UserThreadTable)]{};

static_assert(alignof(Platform::Ia64::Memory::Ia64BspRuntimeMemory) == Kernel::Memory::MappingPageSizeBytes);
// The preferred reservation page holds the owner; a fallback page is chosen
// at boot to hold it too.
static_assert(sizeof(Platform::Ia64::Memory::Ia64BspRuntimeMemory) <=
              Platform::Ia64::Memory::RuntimeMemoryReservationPreferredBytes);
static_assert(sizeof(g_ia64PciEcamRuntimeStorage) == sizeof(Platform::Device::PciEcamRuntime));
static_assert(sizeof(Ia64AhciCommandPageStorage) == Kernel::Device::AhciReadDmaExtCommandBuilder::CommandPageSizeBytes);
static_assert(alignof(Ia64AhciCommandPageStorage) ==
              Kernel::Device::AhciReadDmaExtCommandBuilder::CommandPageAlignmentBytes);
static_assert(sizeof(Ia64AhciReadDataPageStorage) ==
              Kernel::Device::AhciReadDmaExtCommandBuilder::CommandPageSizeBytes);
static_assert(alignof(Ia64AhciReadDataPageStorage) ==
              Kernel::Device::AhciReadDmaExtCommandBuilder::CommandPageAlignmentBytes);
static_assert(Kernel::Device::AhciIdentifyDmaPageBuilder::PageSizeBytes ==
              Kernel::Device::AhciReadDmaExtCommandBuilder::CommandPageSizeBytes);
static_assert(sizeof(g_ia64ApplicationProcessorKernelRuntimeStorage) ==
              sizeof(Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntime));
static_assert(sizeof(g_ia64TlbShootdownMailboxStorage) == sizeof(Kernel::Memory::TlbShootdownMailboxTransport));
static_assert(alignof(Platform::Ia64::Scheduler::Ia64BspKernelRuntime) == Kernel::Memory::MappingPageSizeBytes);
static_assert(sizeof(g_ia64BspKernelRuntimeStorage) == sizeof(Platform::Ia64::Scheduler::Ia64BspKernelRuntime));
static_assert(alignof(Platform::Ia64::Scheduler::Ia64UserExecutionProbe) == 16U);
#if ASTERIA_IA64_SYNTHETIC_USER_PROBE
static_assert(sizeof(g_ia64UserExecutionProbeStorage) == sizeof(Platform::Ia64::Scheduler::Ia64UserExecutionProbe));
#endif
static_assert(sizeof(g_ia64InitialUserImageOwnerStorage) ==
              sizeof(Platform::Ia64::Executable::Ia64InitialUserImageOwner));
static_assert(alignof(Platform::Ia64::Executable::Ia64InitialUserImageOwner) == 16U);
static_assert(sizeof(g_ia64InitialUserImageMemoryOwnerStorage) ==
              sizeof(Platform::Ia64::Executable::Ia64InitialUserImageMemoryOwner));
static_assert(alignof(Platform::Ia64::Executable::Ia64InitialUserImageMemoryOwner) == 16U);
static_assert(sizeof(g_ia64InitialUserImageMappingOwnerStorage) ==
              sizeof(Platform::Ia64::Executable::Ia64InitialUserImageMappingOwner));
static_assert(alignof(Platform::Ia64::Executable::Ia64InitialUserImageMappingOwner) == 16U);
static_assert(alignof(Kernel::Resource::ResourceDomainObjectId) == 8U);
// The revocation roots of the boot's registry: the kernel runtime's space, the initial image's
// space and diagnostic, and the pager's space and fault tokens. With one per assignable device and
// one per FaultChannel they fit a registry's 64 domains.
constexpr Foundation::Size Ia64BootRevocationRoots{5U};
static_assert(Ia64BootRevocationRoots + Platform::Device::PciEcamRevocationRoots +
                  Platform::Device::FaultChannelRevocationRoots <=
              Kernel::Object::RevocationRegistry::MaximumDomains);
static_assert(Platform::Ia64::Memory::InitialMemoryStackSizeBytes >
              Platform::Ia64::Firmware::SalRegistrationFreeStorageBytes);
static_assert(Platform::Ia64::Memory::InitialRseBackingStoreSizeBytes >
              Platform::Ia64::Firmware::SalRegistrationFreeStorageBytes);
#endif

alignas(8) constinit BootInfo g_bootInfo{};

namespace
{
constinit Kernel::Memory::BootstrapPageAllocator g_bootstrapPageAllocator;
// And serving, timer delivery (Ia64CrossProcessorWaitBudget). A wait gives up only after both
// minimums, so it depends neither on how fast the host runs the emulator nor on how long the host
// stalls it: 10'000'000 polls take the BSP seconds of its own execution, and 8'000'000'000 ITC
// ticks are 20 s at the emulator's 400 MHz, over ten times the slowest AP start (its timer delivery
// waits a 1 s lead), twice the AP's own timer poll grace so that an AP reports a lost interrupt
// first, and within the VM runner's boot budget.
#if defined(ASTERIA_TARGET_IA64)
constexpr Foundation::Size Ia64ApWaitMinimumPolls{Kernel::Scheduler::CrossProcessorMinimumPolls};
constexpr Foundation::UInt64 Ia64ApWaitMinimumTicks{Kernel::Scheduler::Ia64CrossProcessorMinimumTicks};
constexpr Foundation::UInt64 Ia64ApPortableSchedulerServiceCookieBase{0x4150'5356'4300'0000ULL};
constexpr Foundation::UInt8 Ia64BootstrapTimerVector{0xE8U};
#endif

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
    Platform::Firmware::WriteDiagnosticAscii(text);
}

/// Records what the PCI ECAM scan found: its first function and the function
/// bound as the AHCI controller. The host compares them with QEMU's view.
void RecordIa64PciEvidence(const Platform::Device::PciEcamRuntimeSummary& summary) noexcept
{
    Platform::Ia64::Diagnostics::Ia64PciEvidence& evidence = Platform::Ia64::Diagnostics::GetIa64EvidenceRecord().Pci;
    const Kernel::Device::PciFunctionAddress first = summary.InitialFunction;
    const Kernel::Device::PciFunctionDescriptor& ahci = summary.AhciFunction;
    evidence.EcamReady = 1U;
    evidence.FirstFunctionPresent = summary.InitialFunctionPresent ? 1U : 0U;
    evidence.FirstSegment = first.GetSegment();
    evidence.FirstBus = first.GetBus();
    evidence.FirstDevice = first.GetDevice();
    evidence.FirstFunction = first.GetFunction();
    evidence.FirstVendorDevice = summary.InitialVendorDevice;
    evidence.PublishedFunctionCount = summary.PublishedFunctionCount;
    evidence.AhciPresent = summary.AhciFunctionPresent ? 1U : 0U;
    evidence.AhciSegment = ahci.Address.GetSegment();
    evidence.AhciBus = ahci.Address.GetBus();
    evidence.AhciDevice = ahci.Address.GetDevice();
    evidence.AhciFunction = ahci.Address.GetFunction();
    evidence.AhciVendorDevice =
        static_cast<Foundation::UInt64>(ahci.VendorId) | (static_cast<Foundation::UInt64>(ahci.DeviceId) << 16U);
    evidence.AhciClassCode = ahci.ClassCode;
    evidence.AhciSubclass = ahci.Subclass;
    evidence.AhciProgrammingInterface = ahci.ProgrammingInterface;
    Platform::Ia64::Diagnostics::PublishIa64Evidence(evidence.Published);
}

// The assignable device's INTx: every other function routed to its line gets INTx Disable; a line
// that cannot be isolated halts. ConfigureIa64InterruptLines routes the line.
[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool
ConfigureIa64DeviceInterrupts(Platform::Device::PciEcamRuntime& runtime,
                              const Kernel::Device::PciFunctionAddress address) noexcept
{
    return runtime.IsolateInterruptLine(address).HasValue();
}

#if !ASTERIA_IA64_SYNTHETIC_USER_PROBE
// Whether the kernel's re-read of the DSDT still matched the bound namespace evidence; computed at
// kernel input import.
bool g_ia64I8042DsdtMatches{false};

// Every MADT I/O SAPIC routes to the BSP (cr.lid: id in bits 31:24, eid in
// 23:16), whose idle and PL3 interrupt claims dispatch its vectors: PCI INTx
// and the i8042's ISA lines alike, independent of the assignable device. A
// controller whose GSI range overlaps a configured one is left out, so its
// lines stay unroutable; a line no controller serves leaves its device
// without Interrupts. Each controller keeps the slot of its MADT index.
[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool ConfigureIa64InterruptLines() noexcept
{
    const Foundation::UInt64 lid = Platform::Ia64::Interrupt::Ia64SalBootRendezvousHandler::ReadCurrentCrLid();
    Foundation::Size count{0U};
    for (Foundation::Size index{0U};
         (index < g_bootInfo.Acpi.IoControllerCount) && (index < Boot::Acpi::MaximumAcpiIoControllers); ++index)
    {
        const Boot::Acpi::AcpiIoControllerTopology& controller = g_bootInfo.Acpi.IoControllers[index];
        if (controller.ControllerKind != Boot::Acpi::AcpiIoControllerKind::IoSapic)
        {
            continue;
        }
        Platform::Ia64::Interrupt::Ia64IoSapic& sapic = g_ia64DeviceIoSapics[index];
        if (!sapic.Configure(g_ia64PhysicalMemoryAccess, controller.PhysicalAddress,
                             controller.GlobalSystemInterruptBase))
        {
            continue;
        }
        const bool added =
            (count == 0U)
                ? g_ia64DeviceInterruptLines.Configure(sapic, static_cast<Foundation::UInt8>((lid >> 24U) & 0xFFU),
                                                       static_cast<Foundation::UInt8>((lid >> 16U) & 0xFFU))
                : g_ia64DeviceInterruptLines.AddController(sapic);
        if (added)
        {
            ++count;
        }
    }
    g_ia64DeviceInterruptsConfigured = count != 0U;
    return !g_ia64DeviceInterruptsConfigured ||
           Platform::Ia64::Interrupt::ConfigureIa64UserDeviceInterrupts(g_ia64DeviceInterruptLines);
}

// Firmware ABI boundary, as on AMD64: the loader's final-map parser validated
// this immutable table; re-read it before the kernel installs its own
// translations, through the entry regime's identity access the loader used
// (never the uncacheable alias: a cached line hit by an uncacheable access may
// raise a Machine Check, SDM rev. 2.3 Vol. 2 Part I 4.4.11.2).
[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool Ia64I8042DsdtMatches(const Boot::Acpi::AcpiTopology& topology) noexcept
{
    const Boot::Acpi::AcpiTableRecord& dsdt = topology.Dsdt;
    if ((topology.Reserved == 0U) || (dsdt.PhysicalAddress == 0U) || (dsdt.Length < 36U) ||
        (dsdt.Length > 1024U * 1024U))
    {
        return false;
    }
    const auto* const bytes =
        reinterpret_cast<const Foundation::UInt8*>(static_cast<Foundation::UIntPointer>(dsdt.PhysicalAddress));
    Foundation::UInt32 length{0U};
    for (Foundation::Size byte{0U}; byte < 4U; ++byte)
    {
        length |= Foundation::UInt32{bytes[4U + byte]} << (byte * 8U);
    }
    if (length != dsdt.Length)
    {
        return false;
    }
    Foundation::UInt32 sum{0U};
    for (Foundation::Size byte{0U}; byte < length; ++byte)
    {
        sum = (sum + bytes[byte]) & 0xFFU;
    }
    return (sum == 0U) && (bytes[0U] == 'D') && (bytes[1U] == 'S') && (bytes[2U] == 'D') && (bytes[3U] == 'T') &&
           Boot::Acpi::I8042Evidence::Matches(topology, dsdt.PhysicalAddress, length, bytes[9U]);
}
#endif

// Publish only device memory: a firmware scanout may never overlap RAM.
[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool PublishIa64Scanout(Platform::Device::PciEcamRuntime& runtime) noexcept
{
    const BootDisplay& display = g_bootInfo.Display;
    if (display.FrameBufferBase == 0U)
    {
        return true;
    }
    constexpr Foundation::UInt64 maximum{~Foundation::UInt64{0U}};
    if (display.FrameBufferBytes > (maximum - 0xFFFU))
    {
        return false;
    }
    const Foundation::UInt64 bytes = (display.FrameBufferBytes + 0xFFFU) & ~Foundation::UInt64{0xFFFU};
    if (display.FrameBufferBase > (maximum - bytes))
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < g_bootInfo.MemoryRegionCount; ++index)
    {
        const auto& region = g_bootInfo.MemoryRegions[index];
        const PhysicalMemoryKind kind = region.Kind;
        const bool ram = (kind == PhysicalMemoryKind::Conventional) || (kind == PhysicalMemoryKind::Loader) ||
                         (kind == PhysicalMemoryKind::BootServices) || (kind == PhysicalMemoryKind::RuntimeServices) ||
                         (kind == PhysicalMemoryKind::AcpiReclaimable) ||
                         (kind == PhysicalMemoryKind::AcpiNonVolatile) || (kind == PhysicalMemoryKind::Persistent);
        if ((region.PageCount > (maximum >> 12U)) || (region.PhysicalStart > (maximum - (region.PageCount << 12U))))
        {
            return false;
        }
        const Foundation::UInt64 end = region.PhysicalStart + (region.PageCount << 12U);
        if (ram && (region.PhysicalStart < (display.FrameBufferBase + bytes)) && (display.FrameBufferBase < end))
        {
            return true;
        }
    }
    const auto published = runtime.PublishScanoutRegion(
        display.FrameBufferBase, bytes, g_ia64CommonRuntimeProcessorCapabilities.PhysicalAddressBits,
        g_ia64RootResourceDomainIdentity, *g_ia64RootResourceDomain, Kernel::Memory::MemoryCachePolicy::Uncached);
    // Physical-mode terminal UC drawing has no PAL attribute transition corridor. Keep this
    // publication UC until that is available.
    if (!published.HasValue())
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:IA64-SCANOUT:FAIL\r\n");
        return false;
    }
    g_ia64ScanoutRegion = published.GetValue();
    return true;
}

// The assignable device: the IA-64 firmware has no PCI I/O handle for edu, so the runtime finds it
// in ECAM, sizes its BAR there and publishes it. A machine without it boots without a device to
// grant; one whose device cannot be published halts.
[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool
PublishIa64AssignableDevice(Platform::Device::PciEcamRuntime& runtime) noexcept
{
    const auto captured = runtime.CaptureAssignableResource(Ia64AssignableIdentifier, Ia64AssignableBar);
    FinalizeBootInventoryEduCapture(captured.HasValue() ? &captured.GetValue() : nullptr);
    if (!captured.HasValue())
    {
        return true;
    }
    const auto published = runtime.PublishAssignableDevice(captured.GetValue(),
                                                           g_ia64CommonRuntimeProcessorCapabilities.PhysicalAddressBits,
                                                           g_ia64RootResourceDomainIdentity, *g_ia64RootResourceDomain);
    if (!published.HasValue())
    {
        return false;
    }
    g_ia64AssignableDevice = published.GetValue();
    const Platform::Firmware::AhciPciResource& resource = captured.GetValue();
    return ConfigureIa64DeviceInterrupts(
        runtime,
        Kernel::Device::PciFunctionAddress{resource.Segment, resource.Bus, resource.Device, resource.Function});
}

// The e1000e as the device table's NIC entry. As for edu, the IA-64 firmware gives the boot no PCI
// I/O handle for it, so the runtime finds it in ECAM, sizes its register BAR there and publishes
// it, its INTx line isolated, before the IOMMU handoff sweeps its bus mastering off. It has no boot
// diagnostic: only its driver's assignment, behind the IOMMU or under a BOOT.CFG trust entry, may
// let it master the bus. A machine without it boots without a NIC; one whose NIC cannot be
// published halts.
[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool PublishIa64NicDevice(Platform::Device::PciEcamRuntime& runtime) noexcept
{
    const auto captured = runtime.CaptureAssignableResource(Ia64NicIdentifier, Ia64NicBar);
    FinalizeBootInventoryNicCapture(captured.HasValue() ? &captured.GetValue() : nullptr);
    if (!captured.HasValue())
    {
        return true;
    }
    const auto published = runtime.PublishAssignableDevice(captured.GetValue(),
                                                           g_ia64CommonRuntimeProcessorCapabilities.PhysicalAddressBits,
                                                           g_ia64RootResourceDomainIdentity, *g_ia64RootResourceDomain);
    if (!published.HasValue())
    {
        return false;
    }
    g_ia64NicDevice = published.GetValue();
    const Platform::Firmware::AhciPciResource& resource = captured.GetValue();
    if (!ConfigureIa64DeviceInterrupts(runtime, Kernel::Device::PciFunctionAddress{resource.Segment, resource.Bus,
                                                                                   resource.Device, resource.Function}))
    {
        return false;
    }
    Platform::Firmware::WriteDiagnosticAscii("ASTERIA:IA64-NIC:PUBLISHED\r\n");
    return true;
}

// The AHCI controller as the second assignable device: its function and the DeviceRegion of its
// ABAR, published after the boot's diagnostic, its last command, and its INTx line isolated as
// edu's. A machine whose controller the boot did not bind publishes none; one whose controller
// cannot be published halts.
[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool PublishIa64AhciDevice(Platform::Device::PciEcamRuntime& runtime) noexcept
{
    if (runtime.GetAhciAvailability() != Platform::Device::AhciPciAvailability::Ready)
    {
        return true;
    }
    const Kernel::Device::PciFunctionAddress address{g_ia64AhciPciResource.Segment, g_ia64AhciPciResource.Bus,
                                                     g_ia64AhciPciResource.Device, g_ia64AhciPciResource.Function};
    // The diagnostic's pages are never device-mapped again: a driver brings its own, and only its
    // assignment may let the controller master the bus.
    auto& configuration = runtime.GetConfigurationSpace();
    const auto published = runtime.PublishAhciDevice(g_ia64AhciPciResource);
    if (!Kernel::Device::SetPciBusMastering(configuration, configuration, address, false) || !published.HasValue())
    {
        return false;
    }
    g_ia64AhciDevice = published.GetValue();
    return ConfigureIa64DeviceInterrupts(runtime, address);
}

// The USB 1.1/2.0 host controllers, every one or after a refused IOMMU handoff those a trust entry
// names, each with its INTx line isolated as edu's where it can be; a line that cannot be leaves
// that controller to the driver's polling. USB is never a reason to stop the machine.
ASTERIA_NOINLINE_NOIPA
void PublishIa64UsbHosts(Platform::Device::PciEcamRuntime& runtime, const bool trustedOnly) noexcept
{
    UsbHostBootSite site{trustedOnly ? &g_bootInfo.Trust : nullptr, &runtime};
    static_cast<void>(Platform::Device::PublishUsbHosts(
        Platform::Device::UsbHostPublicationInput{.Runtime = &runtime,
                                                  .Site = &site,
                                                  .PhysicalAddressBits =
                                                      g_ia64CommonRuntimeProcessorCapabilities.PhysicalAddressBits,
                                                  .ResourceIdentity = g_ia64RootResourceDomainIdentity,
                                                  .Resources = g_ia64RootResourceDomain},
        g_ia64UsbHostDevices));
}

// After a refused IOMMU handoff: whether a verified trust entry names the bound AHCI controller for
// the AHCI driver, which then runs without
[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool
IsIa64AhciTrustedAfterRefusedHandoff(Platform::Device::PciEcamRuntime& runtime) noexcept
{
    Platform::Device::PciEcamRuntimeSummary summary{};
    if ((runtime.GetAhciAvailability() != Platform::Device::AhciPciAvailability::Ready) ||
        (runtime.GetSummary(summary) != Platform::Device::PciEcamFunctionQueryError::None) ||
        !summary.AhciFunctionPresent)
    {
        return false;
    }
    const Kernel::Device::PciFunctionDescriptor& ahci = summary.AhciFunction;
    const Kernel::Device::TrustedDmaFunction function{.Address = ahci.Address,
                                                      .Vendor = ahci.VendorId,
                                                      .Product = ahci.DeviceId,
                                                      .Driver = BootTrustDriver::Ahci,
                                                      .Reserved = 0U};
    return Kernel::Device::DecideTrustedDma(g_bootInfo.Trust, function, Kernel::Device::IommuAvailability::Refused)
               .Outcome == Kernel::Device::TrustedDmaOutcome::Applied;
}

[[nodiscard]] bool IsIa64WritableLoadedImageDmaSpan(const Foundation::UInt64 start, const Foundation::UInt64 sizeBytes,
                                                    const Foundation::UInt64 alignmentBytes) noexcept
{
    constexpr Foundation::UInt64 MaximumUInt64Value{~Foundation::UInt64{0U}};
    constexpr Foundation::UInt64 PhysicalAliasBit{Foundation::UInt64{1U} << 63U};
    if ((sizeBytes == 0U) || (alignmentBytes == 0U) || ((alignmentBytes & (alignmentBytes - 1U)) != 0U) ||
        ((start & (alignmentBytes - 1U)) != 0U) || ((start & PhysicalAliasBit) != 0U) ||
        (start > (MaximumUInt64Value - sizeBytes)))
    {
        return false;
    }
    const Foundation::UInt64 end = start + sizeBytes;
    if (((end - 1U) & PhysicalAliasBit) != 0U)
    {
        return false;
    }

    for (Foundation::Size index{0U}; index < Platform::Ia64::Firmware::Ia64LoadedImageSectionCount; ++index)
    {
        const Platform::Ia64::Firmware::Ia64LoadedImageSection& section = g_ia64LoadedImageLayout.Sections[index];
        if ((section.Purpose != Platform::Ia64::Firmware::Ia64LoadedImageSectionPurpose::WritableData) ||
            (section.PageCount > (MaximumUInt64Value >> 12U)))
        {
            continue;
        }
        const Foundation::UInt64 sectionBytes = section.PageCount << 12U;
        if (section.VirtualStart > (MaximumUInt64Value - sectionBytes))
        {
            continue;
        }
        const Foundation::UInt64 sectionEnd = section.VirtualStart + sectionBytes;
        if ((start >= section.VirtualStart) && (end <= sectionEnd))
        {
            return true;
        }
    }
    return false;
}

/// The boot path's one scripted READ DMA EXT request: one sector at LBA 8.
/// The host judges what arrived in the buffer (the evidence record names it).
constexpr Foundation::UInt64 Ia64AhciReadStartingLba{8U};
constexpr Foundation::UInt64 Ia64AhciReadSectorCount{1U};
constexpr Foundation::UInt64 Ia64AhciReadTransferSizeBytes{512U};

[[nodiscard]] bool TryGetIa64AhciDmaSpans(Kernel::Device::AhciDmaPageWritableView& identifyPage,
                                          Kernel::Device::AhciReadDmaExtCommandPageWritableView& commandPage,
                                          Kernel::Device::AhciReadDmaExtDataBufferSpan& dataBuffer) noexcept
{
    constexpr Foundation::UInt64 PageSize{Kernel::Device::AhciReadDmaExtCommandBuilder::CommandPageSizeBytes};
    constexpr Foundation::UInt64 PageAlignment{Kernel::Device::AhciReadDmaExtCommandBuilder::CommandPageAlignmentBytes};
    const Foundation::UInt64 commandStart =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(g_ia64AhciCommandPage.Bytes));
    const Foundation::UInt64 dataStart =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(g_ia64AhciReadDataPage.Bytes));

    // The current IA-64 bootstrap retains the EFI identity mapping for loaded
    // image storage, has no IOMMU, and runs on coherent QEMU memory. Therefore
    // these structural virtual addresses are also the controller-visible
    // bit-63-clear physical addresses. Neither buffer is volatile storage.
    if (!IsIa64WritableLoadedImageDmaSpan(commandStart, PageSize, PageAlignment) ||
        !IsIa64WritableLoadedImageDmaSpan(dataStart, PageSize, PageAlignment))
    {
        return false;
    }

    identifyPage = Kernel::Device::AhciDmaPageWritableView{
        .Data = g_ia64AhciCommandPage.Bytes,
        .SizeBytes = sizeof(g_ia64AhciCommandPage.Bytes),
        .PhysicalAddress = commandStart,
    };
    commandPage = Kernel::Device::AhciReadDmaExtCommandPageWritableView{
        .Data = g_ia64AhciCommandPage.Bytes,
        .SizeBytes = sizeof(g_ia64AhciCommandPage.Bytes),
        .PhysicalAddress = commandStart,
    };
    dataBuffer = Kernel::Device::AhciReadDmaExtDataBufferSpan{
        .PhysicalAddress = dataStart,
        .SizeBytes = Ia64AhciReadTransferSizeBytes,
    };
    return true;
}

[[nodiscard]] bool TryGetIa64AhciDeadline(Foundation::UInt64& output) noexcept
{
    constexpr Foundation::UInt64 MaximumUInt64Value{~Foundation::UInt64{0U}};
    constexpr Foundation::UInt64 DeadlineIntervalTicks{500'000'000U};
    const Foundation::UInt64 now = g_ia64AhciPollingClock.GetTicks();
    if ((now == MaximumUInt64Value) || (now > (MaximumUInt64Value - DeadlineIntervalTicks)))
    {
        return false;
    }
    output = now + DeadlineIntervalTicks;
    return output != MaximumUInt64Value;
}

enum class Ia64AhciReadDiagnosticStage : Foundation::UInt8
{
    Prerequisite = 1U,
    Storage = 2U,
    Deadline = 3U,
    Executor = 4U,
    Receipt = 5U,
};

void WriteIa64AhciReadFailureDiagnostic(const Ia64AhciReadDiagnosticStage stage, const Foundation::UInt64 detail,
                                        const Platform::Device::AhciReadDmaExtRuntimeFailure& failure = {}) noexcept
{
    Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AHCI-READ-DMA-EXT:FAIL:STAGE=0x");
    WriteIa64DiagnosticHex(static_cast<Foundation::UInt8>(stage), 2U);
    Platform::Firmware::WriteDiagnosticAscii(":DETAIL=0x");
    WriteIa64DiagnosticHex(detail, 16U);
    Platform::Firmware::WriteDiagnosticAscii(":RUNTIME=0x");
    WriteIa64DiagnosticHex(static_cast<Foundation::UInt8>(failure.Error), 2U);
    Platform::Firmware::WriteDiagnosticAscii(":COMMAND=0x");
    WriteIa64DiagnosticHex(static_cast<Foundation::UInt8>(failure.Command.Error), 2U);
    Platform::Firmware::WriteDiagnosticAscii(":RECOVERY=0x");
    WriteIa64DiagnosticHex(static_cast<Foundation::UInt8>(failure.Command.RecoveryError), 2U);
    Platform::Firmware::WriteDiagnosticAscii(":PHASE=0x");
    WriteIa64DiagnosticHex(static_cast<Foundation::UInt8>(failure.Command.Phase), 2U);
    Platform::Firmware::WriteDiagnosticAscii(":REGISTER=0x");
    WriteIa64DiagnosticHex(static_cast<Foundation::UInt8>(failure.Command.Register), 2U);
    Platform::Firmware::WriteDiagnosticAscii(":PORT=0x");
    WriteIa64DiagnosticHex(failure.Command.PortNumber, 2U);
    Platform::Firmware::WriteDiagnosticAscii(":COMMAND-LIVE=0x");
    Platform::Firmware::WriteDiagnosticAscii(failure.Command.CommandPageMustRemainLive ? "1" : "0");
    Platform::Firmware::WriteDiagnosticAscii(":DATA-LIVE=0x");
    Platform::Firmware::WriteDiagnosticAscii(failure.Command.DataBufferMustRemainLive ? "1\r\n" : "0\r\n");
}

void WriteIa64AhciIdentifyFailureDiagnostic(const Platform::Device::AhciIdentifyRuntimeFailure& failure) noexcept
{
    using Kernel::Device::AhciIdentifyCommandError;
    if ((failure.Error == Platform::Device::AhciIdentifyRuntimeError::CommandRejected) &&
        (failure.Command.Error == AhciIdentifyCommandError::NoActiveDiskPort))
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AHCI-IDENTIFY:NO-DISK\r\n");
        return;
    }

    Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AHCI-IDENTIFY:FAIL:RUNTIME=0x");
    WriteIa64DiagnosticHex(static_cast<Foundation::UInt8>(failure.Error), 2U);
    Platform::Firmware::WriteDiagnosticAscii(":COMMAND=0x");
    WriteIa64DiagnosticHex(static_cast<Foundation::UInt8>(failure.Command.Error), 2U);
    Platform::Firmware::WriteDiagnosticAscii(":RECOVERY=0x");
    WriteIa64DiagnosticHex(static_cast<Foundation::UInt8>(failure.Command.RecoveryError), 2U);
    Platform::Firmware::WriteDiagnosticAscii(":DMA-LIVE=");
    Platform::Firmware::WriteDiagnosticAscii(failure.Command.DmaPageMustRemainLive ? "1\r\n" : "0\r\n");
}

#if ASTERIA_IA64_SYNTHETIC_USER_PROBE
void WriteIa64UserProbeDispatchFailureDiagnostic(
    const Platform::Ia64::Scheduler::Ia64BspKernelRuntimeUserProbeError error,
    const Platform::Ia64::Scheduler::Ia64BspKernelRuntimeUserProbeFailure& failure) noexcept
{
    Platform::Firmware::WriteDiagnosticAscii("ASTERIA:USER-EXECUTION-PROBE:FAIL:ERROR=0x");
    WriteIa64DiagnosticHex(static_cast<Foundation::UInt8>(error), 2U);
    Platform::Firmware::WriteDiagnosticAscii(":RUN-LOOP=0x");
    WriteIa64DiagnosticHex(static_cast<Foundation::UInt8>(failure.RunLoop), 2U);
    Platform::Firmware::WriteDiagnosticAscii(":CONTROLLER=0x");
    WriteIa64DiagnosticHex(static_cast<Foundation::UInt8>(failure.Controller), 2U);
    Platform::Firmware::WriteDiagnosticAscii(":SCHEDULER=0x");
    WriteIa64DiagnosticHex(static_cast<Foundation::UInt8>(failure.Scheduler), 2U);
    Platform::Firmware::WriteDiagnosticAscii(":DEADLINE=0x");
    WriteIa64DiagnosticHex(static_cast<Foundation::UInt8>(failure.Deadline), 2U);
    Platform::Firmware::WriteDiagnosticAscii(":WAIT-CONTINUATION=0x");
    WriteIa64DiagnosticHex(static_cast<Foundation::UInt8>(failure.WaitContinuation), 2U);
    Platform::Firmware::WriteDiagnosticAscii(":WAIT-POLL=0x");
    WriteIa64DiagnosticHex(static_cast<Foundation::UInt8>(failure.WaitPoll), 2U);
    Platform::Firmware::WriteDiagnosticAscii("\r\n");
}
#endif

[[nodiscard]] bool Ia64AhciReadReceiptMatches(const Kernel::Device::AhciIdentifyCommandReceipt& identify,
                                              const Kernel::Device::AhciReadDmaExtExecutionReceipt& read,
                                              const Foundation::UInt64 deadlineTicks) noexcept
{
    constexpr Foundation::Size MaximumPollCount{5'000'000U};
    constexpr Foundation::Size MaximumRecoveryPollCount{1'000'000U};
    constexpr Foundation::UInt32 DeviceToHostRegisterFisInterrupt{1U};
    constexpr Foundation::UInt32 TaskFileBusyOrErrorMask{0xA9U};
    bool reservedAreZero{true};
    for (const Foundation::UInt8 value : read.Reserved)
    {
        reservedAreZero = reservedAreZero && (value == 0U);
    }
    return reservedAreZero && (read.StartTicks != 0U) && (read.StartTicks != ~Foundation::UInt64{0U}) &&
           (read.CompletionTicks >= read.StartTicks) && (read.CompletionTicks <= deadlineTicks) &&
           (read.PollCount <= (MaximumPollCount + MaximumRecoveryPollCount)) &&
           (read.DeviceSectorCount == identify.Device.SectorCount) && (read.StartingLba == Ia64AhciReadStartingLba) &&
           (read.LogicalSectorSizeBytes == identify.Device.LogicalSectorSizeBytes) &&
           (read.LogicalSectorSizeBytes == Ia64AhciReadTransferSizeBytes) &&
           (read.SectorCount == Ia64AhciReadSectorCount) && (read.TransferSizeBytes == Ia64AhciReadTransferSizeBytes) &&
           (read.Capabilities == identify.Capabilities) && (read.PortsImplemented == identify.PortsImplemented) &&
           (read.PortSignature == identify.PortSignature) &&
           ((read.CompletionInterruptStatus & DeviceToHostRegisterFisInterrupt) != 0U) &&
           ((read.CompletionTaskFileData & TaskFileBusyOrErrorMask) == 0U) &&
           (read.InitialPortCommand == identify.RestoredPortCommand) &&
           (read.RestoredPortCommand == identify.RestoredPortCommand) && (read.PortNumber == identify.PortNumber) &&
           (read.PortDisposition == Kernel::Device::AhciReadDmaExtPortDisposition::Restored) &&
           (read.DeviceToHostFisStatus == 0x50U) && (read.DeviceToHostFisError == 0U);
}

/// Records the IDENTIFY DEVICE data before the READ DMA EXT command reuses
/// the DMA page that received it.
void RecordIa64AhciIdentifyEvidence(const Kernel::Device::AhciIdentifyCommandReceipt& receipt) noexcept
{
    static_assert(Kernel::Device::AhciIdentifyDmaPageBuilder::IdentifyDataSizeBytes ==
                  Platform::Ia64::Diagnostics::Ia64EvidenceIdentifyDataBytes);
    Platform::Ia64::Diagnostics::Ia64AhciEvidence& evidence = Platform::Ia64::Diagnostics::GetIa64EvidenceRecord().Ahci;
    const Foundation::UInt8* const identify =
        g_ia64AhciCommandPage.Bytes + Kernel::Device::AhciIdentifyDmaPageBuilder::IdentifyDataOffsetBytes;
    for (Foundation::Size index{0U}; index < Platform::Ia64::Diagnostics::Ia64EvidenceIdentifyDataBytes; ++index)
    {
        evidence.IdentifyData[index] = identify[index];
    }
    RecordBootInventoryDisk(receipt.PortNumber, receipt.Device.SectorCount, receipt.Device.ModelNumber,
                            receipt.Device.ModelNumberLength);
    // Only the emulator's disk may be written by the root service's demonstrations.
    g_ia64AhciDiskWritable = Kernel::Device::IsEmulatorDisk(receipt.Device);
    evidence.PortNumber = receipt.PortNumber;
    evidence.Outcome =
        static_cast<Foundation::UInt64>(Platform::Ia64::Diagnostics::Ia64AhciEvidenceOutcome::Identified);
}

/// Records where the scripted read landed; the buffer keeps the transferred
/// bytes for the host to compare with the disk it attached.
void RecordIa64AhciReadEvidence(const Kernel::Device::AhciReadDmaExtExecutionReceipt& receipt,
                                const Kernel::Device::AhciReadDmaExtDataBufferSpan dataBuffer) noexcept
{
    Platform::Ia64::Diagnostics::Ia64AhciEvidence& evidence = Platform::Ia64::Diagnostics::GetIa64EvidenceRecord().Ahci;
    evidence.ReadStartingLba = receipt.StartingLba;
    evidence.ReadSectorCount = receipt.SectorCount;
    evidence.ReadTransferSizeBytes = receipt.TransferSizeBytes;
    evidence.ReadBufferPhysicalAddress = dataBuffer.PhysicalAddress;
    evidence.ReadBufferSizeBytes = dataBuffer.SizeBytes;
    evidence.Outcome = static_cast<Foundation::UInt64>(Platform::Ia64::Diagnostics::Ia64AhciEvidenceOutcome::Read);
    Platform::Ia64::Diagnostics::PublishIa64Evidence(evidence.Published);
}

void RecordIa64AhciNoDiskEvidence() noexcept
{
    // A controller without a disk has nothing a demonstration could harm.
    g_ia64AhciDiskWritable = true;
    Platform::Ia64::Diagnostics::Ia64AhciEvidence& evidence = Platform::Ia64::Diagnostics::GetIa64EvidenceRecord().Ahci;
    evidence.Outcome = static_cast<Foundation::UInt64>(Platform::Ia64::Diagnostics::Ia64AhciEvidenceOutcome::NoDisk);
    Platform::Ia64::Diagnostics::PublishIa64Evidence(evidence.Published);
}

[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool ExecuteIa64AhciReadDmaExt(
    Platform::Device::PciEcamRuntime& runtime, const Kernel::Device::AhciReadDmaExtCommandPageWritableView commandPage,
    const Kernel::Device::AhciReadDmaExtDataBufferSpan dataBuffer, const Foundation::UInt64 deadlineTicks) noexcept
{
    using Platform::Device::AhciReadDmaExtRuntimeError;
    const auto failure = runtime.ExecuteAhciReadDmaExt(g_ia64AhciPollingClock, commandPage, dataBuffer,
                                                       Ia64AhciReadStartingLba, Ia64AhciReadSectorCount,
                                                       Kernel::Device::AhciReadDmaExtPollingPolicy{
                                                           .DeadlineTicks = deadlineTicks,
                                                           .MaximumPollCount = 5'000'000U,
                                                           .MaximumRecoveryPollCount = 1'000'000U,
                                                       });
    if (failure.Error != AhciReadDmaExtRuntimeError::None)
    {
        WriteIa64AhciReadFailureDiagnostic(Ia64AhciReadDiagnosticStage::Executor, failure.Command.ObservedTicks,
                                           failure);
        return false;
    }
    return true;
}

[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool
RunIa64AhciReadDmaExt(Platform::Device::PciEcamRuntime& runtime,
                      const Kernel::Device::AhciIdentifyCommandReceipt& identify,
                      const Kernel::Device::AhciReadDmaExtCommandPageWritableView commandPage,
                      const Kernel::Device::AhciReadDmaExtDataBufferSpan dataBuffer) noexcept
{
    using Platform::Device::AhciReadDmaExtRuntimeQueryError;
    using Platform::Device::AhciReadDmaExtRuntimeState;

    Foundation::UInt64 deadlineTicks{0U};
    if (!TryGetIa64AhciDeadline(deadlineTicks))
    {
        WriteIa64AhciReadFailureDiagnostic(Ia64AhciReadDiagnosticStage::Deadline, 0U);
        return false;
    }
    for (Foundation::Size index{0U}; index < Ia64AhciReadTransferSizeBytes; ++index)
    {
        g_ia64AhciReadDataPage.Bytes[index] = 0xA5U;
    }

    if (!ExecuteIa64AhciReadDmaExt(runtime, commandPage, dataBuffer, deadlineTicks))
    {
        return false;
    }

    Kernel::Device::AhciReadDmaExtExecutionReceipt receipt{};
    const auto receiptQuery = runtime.GetAhciReadDmaExtReceipt(receipt);
    if ((receiptQuery != AhciReadDmaExtRuntimeQueryError::None) ||
        (runtime.GetAhciReadDmaExtState() != AhciReadDmaExtRuntimeState::Succeeded) ||
        !Ia64AhciReadReceiptMatches(identify, receipt, deadlineTicks))
    {
        WriteIa64AhciReadFailureDiagnostic(Ia64AhciReadDiagnosticStage::Receipt,
                                           static_cast<Foundation::UInt8>(receiptQuery));
        return false;
    }

    RecordIa64AhciReadEvidence(receipt, dataBuffer);
    Platform::Firmware::WriteDiagnosticAscii("ASTERIA:STAGE:AHCI-READ-DMA-EXT\r\n");
    return true;
}

enum class Ia64AhciIdentifyBootResult : Foundation::UInt8
{
    Succeeded,
    NoDisk,
    Failed,
};

[[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64AhciIdentifyBootResult ExecuteIa64AhciIdentify(
    Platform::Device::PciEcamRuntime& runtime, const Kernel::Device::AhciDmaPageWritableView identifyPage,
    const Foundation::UInt64 deadlineTicks) noexcept
{
    using Platform::Device::AhciIdentifyRuntimeError;
    const auto failure = runtime.ExecuteAhciIdentify(g_ia64AhciPollingClock, identifyPage,
                                                     Kernel::Device::AhciIdentifyPollingPolicy{
                                                         .DeadlineTicks = deadlineTicks,
                                                         .MaximumPollCount = 5'000'000U,
                                                         .MaximumRecoveryPollCount = 1'000'000U,
                                                     });
    if (failure.Error == AhciIdentifyRuntimeError::None)
    {
        return Ia64AhciIdentifyBootResult::Succeeded;
    }

    WriteIa64AhciIdentifyFailureDiagnostic(failure);
    if ((failure.Error == AhciIdentifyRuntimeError::CommandRejected) &&
        (failure.Command.Error == Kernel::Device::AhciIdentifyCommandError::NoActiveDiskPort))
    {
        RecordIa64AhciNoDiskEvidence();
        return Ia64AhciIdentifyBootResult::NoDisk;
    }
    const Foundation::UInt64 detail =
        (static_cast<Foundation::UInt64>(failure.Error) << 8U) | static_cast<Foundation::UInt8>(failure.Command.Error);
    WriteIa64AhciReadFailureDiagnostic(Ia64AhciReadDiagnosticStage::Prerequisite, detail);
    return Ia64AhciIdentifyBootResult::Failed;
}

[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool
FinishIa64AhciIdentifyAndRunRead(Platform::Device::PciEcamRuntime& runtime,
                                 const Kernel::Device::AhciReadDmaExtCommandPageWritableView commandPage,
                                 const Kernel::Device::AhciReadDmaExtDataBufferSpan dataBuffer) noexcept
{
    using Platform::Device::AhciIdentifyRuntimeQueryError;
    Kernel::Device::AhciIdentifyCommandReceipt receipt{};
    if (runtime.GetAhciIdentifyReceipt(receipt) != AhciIdentifyRuntimeQueryError::None)
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AHCI-IDENTIFY:RECEIPT-UNAVAILABLE\r\n");
        WriteIa64AhciReadFailureDiagnostic(Ia64AhciReadDiagnosticStage::Receipt, 0U);
        return false;
    }
    RecordIa64AhciIdentifyEvidence(receipt);
    Platform::Firmware::WriteDiagnosticAscii("ASTERIA:STAGE:AHCI-IDENTIFY\r\n");
    return RunIa64AhciReadDmaExt(runtime, receipt, commandPage, dataBuffer);
}

[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool RunIa64AhciIdentifyAndRead(Platform::Device::PciEcamRuntime& runtime) noexcept
{
    // AHCI block I/O / DMA isolation: enumeration does not require storage. A present controller's
    // binding failure still fails.
    if (runtime.GetAhciAvailability() == Platform::Device::AhciPciAvailability::Absent)
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:IA64-AHCI:SKIPPED:NO-CONTROLLER\r\n");
        return true;
    }
    if (runtime.GetAhciAvailability() != Platform::Device::AhciPciAvailability::Ready)
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AHCI-IDENTIFY:UNAVAILABLE\r\n");
        WriteIa64AhciReadFailureDiagnostic(Ia64AhciReadDiagnosticStage::Prerequisite,
                                           static_cast<Foundation::UInt8>(runtime.GetAhciAvailability()));
        return false;
    }

    Kernel::Device::AhciDmaPageWritableView identifyPage{};
    Kernel::Device::AhciReadDmaExtCommandPageWritableView commandPage{};
    Kernel::Device::AhciReadDmaExtDataBufferSpan dataBuffer{};
    if (!TryGetIa64AhciDmaSpans(identifyPage, commandPage, dataBuffer))
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AHCI-IDENTIFY:DMA-PAGE-UNAVAILABLE\r\n");
        WriteIa64AhciReadFailureDiagnostic(Ia64AhciReadDiagnosticStage::Storage, 0U);
        return false;
    }
    Foundation::UInt64 deadlineTicks{0U};
    if (!TryGetIa64AhciDeadline(deadlineTicks))
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AHCI-IDENTIFY:CLOCK-UNAVAILABLE\r\n");
        WriteIa64AhciReadFailureDiagnostic(Ia64AhciReadDiagnosticStage::Deadline, 0U);
        return false;
    }

    const Ia64AhciIdentifyBootResult identifyResult = ExecuteIa64AhciIdentify(runtime, identifyPage, deadlineTicks);
    if (identifyResult == Ia64AhciIdentifyBootResult::NoDisk)
    {
        return true;
    }
    if (identifyResult != Ia64AhciIdentifyBootResult::Succeeded)
    {
        return false;
    }
    return FinishIa64AhciIdentifyAndRunRead(runtime, commandPage, dataBuffer);
}

#endif

/// Reports, after ExitBootServices, the usable ACPI processors beyond the four
/// the topology retains as the fact `ASTERIA:ACPI-PROCESSORS:IGNORED:0x<count>`;
/// silent when the machine is within capacity.
[[gnu::noinline]] void ReportIgnoredProcessors() noexcept
{
    const Foundation::UInt64 ignored = g_bootInfo.Acpi.IgnoredProcessorCount;
    if (ignored == 0U)
    {
        return;
    }
    constexpr char Digits[]{"0123456789ABCDEF"};
    char text[19U]{'0', 'x'};
    for (Foundation::UInt32 digit{0U}; digit < 16U; ++digit)
    {
        text[digit + 2U] = Digits[(ignored >> ((15U - digit) * 4U)) & 0x0FU];
    }
    text[18U] = '\0';
    Platform::Firmware::WriteDiagnosticAscii("ASTERIA:ACPI-PROCESSORS:IGNORED:");
    Platform::Firmware::WriteDiagnosticAscii(text);
    Platform::Firmware::WriteDiagnosticAscii("\r\n");
}

#if defined(ASTERIA_TARGET_AMD64)
constexpr Foundation::UInt64 FirmwareMemoryWriteBackAttribute{1U << 3U};
constexpr Foundation::UInt64 FirmwareMemoryRuntimeAttribute{Foundation::UInt64{1U} << 63U};
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};

[[nodiscard]] bool TryGetAmd64PageRangeEnd(const Foundation::UInt64 physicalStart, const Foundation::UInt64 pageCount,
                                           Foundation::UInt64& physicalEnd) noexcept
{
    if (((physicalStart & 0xFFFU) != 0U) || (pageCount == 0U) || (pageCount > (MaximumUInt64 >> 12U)))
    {
        return false;
    }
    const Foundation::UInt64 byteCount = pageCount << 12U;
    if (physicalStart > (MaximumUInt64 - byteCount))
    {
        return false;
    }
    physicalEnd = physicalStart + byteCount;
    return true;
}

[[nodiscard]] bool PrepareAmd64Runtime(BootInfo& info) noexcept
{
    const auto* const bytes = reinterpret_cast<const Foundation::UInt8*>(info.Files.PhysicalStart);
    const BootFileRecord& user = info.Files.Files[BootUserFile];
    const BootFileRecord& pager = info.Files.Files[BootInitFile];
    const BootFileRecord& spinner = info.Files.Files[BootSpinnerFile];
    if (bytes == nullptr || user.SizeBytes == 0U || pager.SizeBytes == 0U)
    {
        return false;
    }
    return Platform::Amd64::Bootstrap::PrepareAmd64BootRuntime(
        {&info,
         &g_amd64InitialMappingPlan,
         &g_amd64InitialPagingCapabilities,
         g_amd64RuntimePool,
         g_amd64StartupPages,
         info.Kernel.MetadataArena.PhysicalStart,
         info.Kernel.MetadataArena.PageCount,
         {bytes + user.Offset, static_cast<Foundation::Size>(user.SizeBytes)},
         {bytes + pager.Offset, static_cast<Foundation::Size>(pager.SizeBytes)},
         {spinner.SizeBytes == 0U ? nullptr : bytes + spinner.Offset, static_cast<Foundation::Size>(spinner.SizeBytes)},
         g_amd64AhciResource,
         g_amd64DeviceResource,
         g_amd64NicResource});
}

[[nodiscard]] bool BindAmd64BootstrapReservation(const BootInfo& bootInfo,
                                                 Kernel::Memory::PhysicalPageRange& reservation) noexcept
{
    Foundation::UInt64 reservationEnd{0U};
    if ((bootInfo.Signature != Boot::BootInfoSignature) || (bootInfo.Version != Boot::BootInfoVersion) ||
        (bootInfo.SizeBytes != sizeof(BootInfo)) || (bootInfo.MemoryRegionCount == 0U) ||
        (bootInfo.MemoryRegionCount > Boot::MaximumMemoryRegions) || (bootInfo.FirmwareDescriptorVersion != 1U) ||
        (bootInfo.FirmwareDescriptorSize < 40U) || (bootInfo.Reserved != 0U) ||
        (bootInfo.FirmwareMemoryMapSizeBytes !=
         (static_cast<Foundation::UInt64>(bootInfo.MemoryRegionCount) * bootInfo.FirmwareDescriptorSize)) ||
        (g_amd64BootstrapReservation.PhysicalStart == 0U) ||
        !TryGetAmd64PageRangeEnd(g_amd64BootstrapReservation.PhysicalStart, g_amd64BootstrapReservation.PageCount,
                                 reservationEnd))
    {
        return false;
    }

    for (Foundation::Size index{0U}; index < bootInfo.MemoryRegionCount; ++index)
    {
        const Boot::PhysicalMemoryRegion& region = bootInfo.MemoryRegions[index];
        Foundation::UInt64 regionEnd{0U};
        if (!TryGetAmd64PageRangeEnd(region.PhysicalStart, region.PageCount, regionEnd))
        {
            return false;
        }
        if ((region.PhysicalStart > g_amd64BootstrapReservation.PhysicalStart) ||
            (regionEnd <= g_amd64BootstrapReservation.PhysicalStart))
        {
            continue;
        }
        if ((regionEnd < reservationEnd) || (region.FirmwareType != 2U) ||
            (region.Kind != Boot::PhysicalMemoryKind::Loader) ||
            ((region.Attributes & FirmwareMemoryWriteBackAttribute) == 0U) ||
            ((region.Attributes & FirmwareMemoryRuntimeAttribute) != 0U))
        {
            return false;
        }
        reservation = Kernel::Memory::PhysicalPageRange{
            .PhysicalStart = g_amd64BootstrapReservation.PhysicalStart,
            .PageCount = g_amd64BootstrapReservation.PageCount,
            .Attributes = region.Attributes,
            .FirmwareType = region.FirmwareType,
            .Reserved = 0U,
        };
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool Amd64ReservationsEqual(const Kernel::Memory::PhysicalPageRange& first,
                                                    const Kernel::Memory::PhysicalPageRange& second) noexcept
{
    return (first.PhysicalStart == second.PhysicalStart) && (first.PageCount == second.PageCount) &&
           (first.Attributes == second.Attributes) && (first.FirmwareType == second.FirmwareType) &&
           (first.Reserved == second.Reserved);
}

[[nodiscard]] constexpr bool
Amd64MappingSummariesEqual(const Platform::Amd64::Memory::BootstrapMappingPlanSummary& first,
                           const Platform::Amd64::Memory::BootstrapMappingPlanSummary& second) noexcept
{
    return (first.ReservedPhysicalStart == second.ReservedPhysicalStart) &&
           (first.StackPhysicalStart == second.StackPhysicalStart) &&
           (first.PageTableStoragePhysicalStart == second.PageTableStoragePhysicalStart) &&
           (first.DescriptorTableStoragePhysicalStart == second.DescriptorTableStoragePhysicalStart) &&
           (first.MappingCount == second.MappingCount) && (first.Reserved16 == second.Reserved16) &&
           (first.Reserved32 == second.Reserved32);
}

// Runs inside the transaction, where no output may disturb the map: the
// failure report writes the record once the transaction has returned.
[[nodiscard]] bool RejectAmd64FinalMemoryMap(const char* const diagnostic) noexcept
{
    g_amd64FinalMemoryMapRejection = diagnostic;
    return false;
}

[[nodiscard]] bool BuildAmd64InitialMappingPlan(const BootInfo& finalBootInfo,
                                                const Kernel::Memory::PhysicalPageRange& reservedPages) noexcept
{
    return Platform::Amd64::Memory::BootstrapMappingPlanBuilder::Build(
               g_amd64LoadedImageLayout, finalBootInfo, reservedPages, g_amd64InitialMappingPlan,
               g_amd64BootstrapMappingPlanSummary) == Platform::Amd64::Memory::BootstrapMappingPlanError::None;
}

[[nodiscard]] bool PrepareAmd64InitialAddressSpace() noexcept
{
    using Platform::Amd64::Memory::InitialAddressSpacePreparationStatus;
    using Platform::Amd64::Memory::InitialPagingCapabilityCapture;
    using Platform::Amd64::Memory::InitialPagingCapabilityCaptureError;

    g_amd64PageTableStorage = Platform::Amd64::Memory::PageTableStorage{
        .PhysicalStart =
            Kernel::Memory::PhysicalAddress{g_amd64BootstrapMappingPlanSummary.PageTableStoragePhysicalStart},
        .Entries = reinterpret_cast<Foundation::UInt64*>(
            static_cast<Foundation::UIntPointer>(g_amd64BootstrapMappingPlanSummary.PageTableStoragePhysicalStart)),
        .PageCapacity = Platform::Amd64::Memory::BootstrapPageTableStoragePageCount,
        .Reserved = 0U,
    };
    g_amd64InitialMemoryStackView = Platform::Amd64::Memory::WritableInitialMemoryStackView{
        .Data = reinterpret_cast<Foundation::UInt8*>(
            static_cast<Foundation::UIntPointer>(g_amd64BootstrapMappingPlanSummary.StackPhysicalStart)),
        .SizeBytes = Platform::Amd64::Memory::InitialMemoryStackSizeBytes,
        .PhysicalStart = g_amd64BootstrapMappingPlanSummary.StackPhysicalStart,
    };
    g_amd64InitialDescriptorTableView = Platform::Amd64::Memory::WritableInitialDescriptorTableView{
        .Data = reinterpret_cast<Foundation::UInt64*>(static_cast<Foundation::UIntPointer>(
            g_amd64BootstrapMappingPlanSummary.DescriptorTableStoragePhysicalStart)),
        .SizeBytes = Platform::Amd64::Memory::InitialDescriptorTableStorageSizeBytes,
        .PhysicalStart = g_amd64BootstrapMappingPlanSummary.DescriptorTableStoragePhysicalStart,
    };
    g_amd64PagingCapabilityError =
        static_cast<InitialPagingCapabilityCaptureError>(g_bootInfo.Kernel.Paging.CaptureError);
    if (g_amd64PagingCapabilityError != InitialPagingCapabilityCaptureError::None)
    {
        return false;
    }
    g_amd64ProcessorStateCaptureError = Platform::Amd64::Memory::InitialAddressSpacePreparation::CaptureProcessorState(
        g_amd64InitialPagingCapabilities, g_amd64InitialProcessorStateReport);
    if (g_amd64ProcessorStateCaptureError != Platform::Amd64::Memory::InitialProcessorStateCaptureError::None)
    {
        return false;
    }
    const auto result = g_amd64InitialAddressSpacePreparation.Prepare(
        g_amd64InitialMappingPlan, g_amd64BootstrapMappingPlanSummary, g_amd64InitialPagingCapabilities,
        g_amd64PageTableStorage, g_amd64InitialMemoryStackView, g_amd64InitialDescriptorTableView,
        g_amd64InitialProcessorStateReport);
    return result.HasValue() && (result.GetValue() == InitialAddressSpacePreparationStatus::Prepared) &&
           g_amd64InitialAddressSpacePreparation.IsPrepared();
}

[[nodiscard]] bool IsAmd64InitialAddressSpacePrepared() noexcept
{
    const auto summaryResult = g_amd64InitialAddressSpacePreparation.TryGetSummary();
    const auto pageTablePreparationResult = g_amd64InitialAddressSpacePreparation.TryGetPageTablePreparation();
    if (!g_amd64InitialAddressSpacePreparation.IsPrepared() || !summaryResult.HasValue() ||
        !pageTablePreparationResult.HasValue() ||
        (g_amd64InitialAddressSpacePreparation.ValidatePreparedState() !=
         Platform::Amd64::Memory::InitialAddressSpacePreparationError::None))
    {
        return false;
    }
    const Platform::Amd64::Memory::InitialAddressSpacePreparationSummary& addressSpaceSummary =
        *summaryResult.GetValue();
    const Platform::Amd64::Memory::InitialPageTablePreparation& pageTablePreparation =
        *pageTablePreparationResult.GetValue();
    const auto pageTableSummaryResult = pageTablePreparation.TryGetSummary();
    const auto planResult = pageTablePreparation.TryGetMappingPlan();
    if (!pageTableSummaryResult.HasValue() || !planResult.HasValue())
    {
        return false;
    }
    const Platform::Amd64::Memory::InitialPageTablePreparationSummary& summary = *pageTableSummaryResult.GetValue();
    const Kernel::Memory::InitialMappingPlan& plan = *planResult.GetValue();
    return plan.IsSealed() && (plan.GetMappingCount() == Platform::Amd64::Memory::BootstrapInitialMappingCount) &&
           (summary.Storage.RootPhysicalAddress.GetValue() ==
            g_amd64BootstrapMappingPlanSummary.PageTableStoragePhysicalStart) &&
           (summary.Storage.Entries == g_amd64PageTableStorage.Entries) &&
           (summary.Storage.PageCapacity == Platform::Amd64::Memory::BootstrapPageTableStoragePageCount) &&
           (summary.Storage.UsedTablePages != 0U) && (summary.Storage.UsedTablePages <= summary.Storage.PageCapacity) &&
           (summary.MappingCount == Platform::Amd64::Memory::BootstrapInitialMappingCount) &&
           (summary.Reserved16 == 0U) && (summary.Reserved32 == 0U) &&
           (addressSpaceSummary.RootPhysicalAddress.GetValue() == summary.Storage.RootPhysicalAddress.GetValue()) &&
           (addressSpaceSummary.StackPhysicalStart == g_amd64BootstrapMappingPlanSummary.StackPhysicalStart) &&
           (addressSpaceSummary.GlobalDescriptorTableAddress ==
            g_amd64BootstrapMappingPlanSummary.DescriptorTableStoragePhysicalStart) &&
           (addressSpaceSummary.InterruptDescriptorTableAddress ==
            g_amd64BootstrapMappingPlanSummary.DescriptorTableStoragePhysicalStart +
                Kernel::Memory::MappingPageSizeBytes) &&
           (g_amd64InitialMemoryStackView.PhysicalStart == addressSpaceSummary.StackPhysicalStart) &&
           (g_amd64InitialDescriptorTableView.PhysicalStart == addressSpaceSummary.GlobalDescriptorTableAddress);
}

[[nodiscard]] bool PrepareAmd64FinalMemoryMap(BootInfo& finalBootInfo) noexcept
{
    Kernel::Memory::PhysicalPageRange reservation{};
    if (!BindAmd64BootstrapReservation(finalBootInfo, reservation))
    {
        return RejectAmd64FinalMemoryMap("ASTERIA:AMD64-FINAL-MEMORY-MAP:FAIL:RESERVATION\r\n");
    }

    if (g_amd64InitialAddressSpacePreparation.IsPrepared())
    {
        Platform::Amd64::Memory::BootstrapMappingPlanSummary summary{};
        if (!Amd64ReservationsEqual(reservation, g_amd64BootstrapReservation) ||
            (Platform::Amd64::Memory::BootstrapMappingPlanBuilder::Validate(g_amd64LoadedImageLayout, finalBootInfo,
                                                                            reservation, summary) !=
             Platform::Amd64::Memory::BootstrapMappingPlanError::None) ||
            !Amd64MappingSummariesEqual(summary, g_amd64BootstrapMappingPlanSummary) ||
            !IsAmd64InitialAddressSpacePrepared())
        {
            return RejectAmd64FinalMemoryMap("ASTERIA:AMD64-FINAL-MEMORY-MAP:FAIL:RETRY\r\n");
        }
        return PrepareAmd64Runtime(finalBootInfo);
    }
    if (g_amd64InitialAddressSpacePreparation.HasFailed() || (g_amd64InitialMappingPlan.GetMappingCount() != 0U) ||
        g_amd64InitialMappingPlan.IsSealed())
    {
        return RejectAmd64FinalMemoryMap("ASTERIA:AMD64-FINAL-MEMORY-MAP:FAIL:STATE\r\n");
    }

    g_amd64BootstrapReservation = reservation;
    if (!BuildAmd64InitialMappingPlan(finalBootInfo, g_amd64BootstrapReservation))
    {
        return RejectAmd64FinalMemoryMap("ASTERIA:AMD64-FINAL-MEMORY-MAP:FAIL:PLAN\r\n");
    }
    if (!PrepareAmd64InitialAddressSpace())
    {
        return RejectAmd64FinalMemoryMap(Amd64FinalMemoryMapTableRejection);
    }
    if (!IsAmd64InitialAddressSpacePrepared())
    {
        return RejectAmd64FinalMemoryMap("ASTERIA:AMD64-FINAL-MEMORY-MAP:FAIL:PUBLICATION\r\n");
    }
    return PrepareAmd64Runtime(finalBootInfo);
}

#elif defined(ASTERIA_TARGET_IA64)
// The registry's entries live in the runtime page after the owner, DTR0's like the owner; nothing
// registers before the root domain. Physical execution: the identity address is the storage, which
// AttachEntries builds.
[[nodiscard]] bool AttachIa64RegistryEntries() noexcept
{
    const auto& reservation = g_ia64RuntimeMemoryReservation;
    if ((reservation.PhysicalStart == 0U) ||
        (reservation.PageCount <
         (Platform::Ia64::Memory::Ia64RuntimeRequiredBytes / Kernel::Memory::PhysicalPageSizeBytes)))
    {
        return false;
    }
    void* const entries = reinterpret_cast<void*>(static_cast<Foundation::UIntPointer>(
        reservation.PhysicalStart + Platform::Ia64::Memory::Ia64RuntimeRegistryOffsetBytes));
    if (!g_ia64BspObjectRegistry.AttachEntries(entries, Kernel::Object::ObjectRegistry::MaximumObjects))
    {
        return false;
    }
    auto& evidence = Platform::Ia64::Diagnostics::GetIa64EvidenceRecord().Metadata;
    evidence.SlabPhysicalStart = reservation.PhysicalStart + Platform::Ia64::Memory::Ia64RuntimeRegistryOffsetBytes;
    evidence.SlabBytes = Platform::Ia64::Memory::Ia64RuntimeSlabBytes;
    evidence.BitmapBudgetBytes = (reservation.PageCount * Kernel::Memory::PhysicalPageSizeBytes) -
                                 Platform::Ia64::Memory::Ia64RuntimeRequiredBytes;
    evidence.RegistryCapacity = g_ia64BspObjectRegistry.GetCapacity();
    return true;
}

/// The address of the runtime page's slab at offset (Ia64RuntimeMetadataLayout.hpp).
[[nodiscard, maybe_unused]] void* Ia64RuntimeSlab(const Foundation::UInt64 offset) noexcept
{
    return reinterpret_cast<void*>(
        static_cast<Foundation::UIntPointer>(g_ia64RuntimeMemoryReservation.PhysicalStart + offset));
}

[[nodiscard]] bool PrepareIa64RootResourceDomain() noexcept
{
    if (!AttachIa64RegistryEntries())
    {
        return false;
    }
    constexpr Kernel::Object::ResourceDomainId rootIdentifier{1U};
    const Foundation::UInt64 freePages = g_bootstrapPageAllocator.GetFreePageCount();
    const Foundation::UInt64 allocatedPages = g_bootstrapPageAllocator.GetAllocatedPageCount();
    constexpr Foundation::UInt64 maximumValue{~Foundation::UInt64{0U}};
    if ((freePages > (maximumValue - allocatedPages)) || ((freePages + allocatedPages) > (maximumValue >> 12U)))
    {
        return false;
    }
    const Foundation::UInt64 totalPages = freePages + allocatedPages;
    if (totalPages == 0U)
    {
        return false;
    }
    const Foundation::UInt64 totalBytes = totalPages << 12U;
    const Kernel::Resource::ResourceDomainLimits limits{
        .CommittedMemoryPages = totalPages,
        .ResidentMemoryPages = totalPages,
        .KernelMetadataBytes = totalBytes,
        .CapabilityHandles = Kernel::Object::ObjectRegistry::MaximumObjects,
        .IpcQueueBytes = totalBytes,
        .Threads = Kernel::Object::ObjectRegistry::MaximumObjects,
        .ChildDomains = Kernel::Object::ObjectRegistry::MaximumObjects,
    };
    auto* const resourceDomain = ::new (static_cast<void*>(g_ia64RootResourceDomainStorage))
        Kernel::Resource::ResourceDomain{g_ia64RootResourceDomainLedger};
    g_ia64RootResourceDomain = resourceDomain;
    const auto initialization = resourceDomain->InitializeRoot(rootIdentifier, limits);
    if (initialization.Error != Kernel::Resource::ResourceDomainError::None)
    {
        return false;
    }
    const auto registration = g_ia64BspRootCapabilityIssuer.CreateObject(*resourceDomain);
    if (!registration.HasValue())
    {
        return false;
    }
    g_ia64RootResourceDomainIdentity =
        Kernel::Resource::ResourceDomainObjectId{g_ia64BspObjectRegistry, registration.GetValue()};
    const auto snapshot = resourceDomain->GetSnapshot();
    return g_ia64RootResourceDomainIdentity.IsValid() &&
           g_ia64RootResourceDomainIdentity.IsFromRegistry(g_ia64BspObjectRegistry) &&
           (snapshot.State == Kernel::Resource::ResourceDomainState::Ready) &&
           (snapshot.Identity.GetValue() == rootIdentifier.GetValue());
}

[[nodiscard]] [[gnu::noinline]] bool PrepareIa64BspRuntimeMemory() noexcept
{
    // Physical execution: the reservation's identity address is its storage.
    // Unlike.bss, Conventional memory carries no zero guarantee, so the
    // owner extent is cleared between the live bootstrap prefix and top RSE.
    const Foundation::UInt64 reservationBytes =
        g_ia64RuntimeMemoryReservation.PageCount * Kernel::Memory::PhysicalPageSizeBytes;
    if ((g_ia64RuntimeMemoryReservation.PhysicalStart == 0U) ||
        (reservationBytes < Platform::Ia64::Memory::Ia64RuntimeRequiredBytes))
    {
        return false;
    }
    auto* const words = reinterpret_cast<Foundation::UInt64*>(static_cast<Foundation::UIntPointer>(
        g_ia64RuntimeMemoryReservation.PhysicalStart + Platform::Ia64::Memory::BootstrapRuntimeObjectOffsetBytes));
    for (Foundation::UInt64 word{0U}; word < (sizeof(Platform::Ia64::Memory::Ia64BspRuntimeMemory) / 8U); ++word)
    {
        words[word] = 0U;
    }
    auto* const owner = ::new (static_cast<void*>(words)) Platform::Ia64::Memory::Ia64BspRuntimeMemory{};

    Platform::Ia64::Diagnostics::PrepareIa64Stop(owner->StopStorage(), g_bootInfo.Display,
                                                 g_ia64ApplicationProcessorExecutionStorageOwner.GetProcessorCount());
    const Kernel::Scheduler::CpuId bspProcessor{0U};
    const bool prepared =
        owner
            ->PrepareAndAdmit(bspProcessor, g_ia64RuntimeProfiledProcessorMask, g_ia64RuntimeProcessorCapabilities,
                              g_ia64InitialAddressSpacePreparation, g_ia64RootResourceDomain->GetIdentifier(),
                              g_ia64BspObjectRegistry, g_ia64BspRootCapabilityIssuer)
            .HasValue() &&
        owner->ConfigureTranslationCachePurge(bspProcessor, g_ia64PalBootstrapCapabilities.TranslationCachePurge);
    if (prepared)
    {
        g_ia64BspRuntimeMemoryOwner = owner;
    }
    return prepared;
}

[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool PrepareIa64InitialUserImageObjects() noexcept
{
    if ((g_ia64InitialUserImageOwner == nullptr) || (g_ia64RootResourceDomain == nullptr) ||
        (g_ia64InitialUserImageMemoryOwner != nullptr))
    {
        return false;
    }

    auto* const owner = ::new (static_cast<void*>(g_ia64InitialUserImageMemoryOwnerStorage))
        Platform::Ia64::Executable::Ia64InitialUserImageMemoryOwner{};
    const bool materialized =
        owner
            ->Materialize(*g_ia64InitialUserImageOwner, g_ia64BspObjectRegistry, g_ia64BspRootCapabilityIssuer,
                          g_ia64RootResourceDomainIdentity, *g_ia64RootResourceDomain)
            .HasValue();
    if (materialized)
    {
        g_ia64InitialUserImageMemoryOwner = owner;
    }
    return materialized;
}

// The boot pager's objects; a configuration without a staged pager has none.
[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool PrepareIa64PagerImageObjects() noexcept
{
    if (g_ia64PagerImageOwner == nullptr)
    {
        return true;
    }
    if ((g_ia64RootResourceDomain == nullptr) || (g_ia64PagerImageMemoryOwner != nullptr))
    {
        return false;
    }
    auto* const pager = ::new (static_cast<void*>(g_ia64PagerImageMemoryOwnerStorage))
        Platform::Ia64::Executable::Ia64InitialUserImageMemoryOwner{};
    const bool pagerMaterialized =
        pager
            ->Materialize(*g_ia64PagerImageOwner, g_ia64BspObjectRegistry, g_ia64BspRootCapabilityIssuer,
                          g_ia64RootResourceDomainIdentity, *g_ia64RootResourceDomain)
            .HasValue();
    if (pagerMaterialized)
    {
        g_ia64PagerImageMemoryOwner = pager;
    }
    return pagerMaterialized;
}

[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool PrepareIa64InitialUserImageMappings() noexcept
{
    if ((g_ia64InitialUserImageMemoryOwner == nullptr) || (g_ia64BspRuntimeMemoryOwner == nullptr) ||
        (g_ia64InitialUserImageMappingOwner != nullptr))
    {
        return false;
    }

    auto* const owner = ::new (static_cast<void*>(g_ia64InitialUserImageMappingOwnerStorage))
        Platform::Ia64::Executable::Ia64InitialUserImageMappingOwner{};
    const bool installed =
        owner->Install(*g_ia64InitialUserImageMemoryOwner, *g_ia64BspRuntimeMemoryOwner, g_ia64BspObjectRegistry)
            .HasValue();
    if (installed)
    {
        g_ia64InitialUserImageMappingOwner = owner;
    }
    return installed;
}

// The boot pager's mappings, into its own root.
[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool PrepareIa64PagerImageMappings() noexcept
{
    if (g_ia64PagerImageMemoryOwner == nullptr)
    {
        return g_ia64PagerImageOwner == nullptr;
    }
    if ((g_ia64BspRuntimeMemoryOwner == nullptr) || (g_ia64PagerImageMappingOwner != nullptr))
    {
        return false;
    }
    auto* const pager = ::new (static_cast<void*>(g_ia64PagerImageMappingOwnerStorage))
        Platform::Ia64::Executable::Ia64InitialUserImageMappingOwner{};
    const bool pagerInstalled =
        pager
            ->Install(*g_ia64PagerImageMemoryOwner, *g_ia64BspRuntimeMemoryOwner, g_ia64BspObjectRegistry,
                      Platform::Ia64::Memory::Ia64BspRuntimeUserDomain::Pager)
            .HasValue() &&
        pager->InstallBootModules(g_bootInfo, g_ia64BspRootCapabilityIssuer, *g_ia64RootResourceDomain,
                                  g_ia64RootResourceDomainIdentity);
    if (pagerInstalled)
    {
        g_ia64PagerImageMappingOwner = pager;
    }
    return pagerInstalled;
}

[[nodiscard]] Kernel::Object::CapabilityValidationKey
DeriveIa64CapabilityKey(Kernel::Object::CapabilityKeyPurpose purpose) noexcept;

#if !ASTERIA_IA64_SYNTHETIC_USER_PROBE
// The validation keys of the CapabilitySpaces the pager creates, derived into boot-lifetime
// storage.
[[nodiscard]] ASTERIA_NOINLINE_NOIPA const Kernel::Object::CapabilityValidationKey*
DeriveIa64RuntimeSpaceKeys() noexcept
{
    static constinit Kernel::Object::CapabilityValidationKey keys[Kernel::Object::CapabilitySpacePool::Capacity]{};
    for (Foundation::Size slot{0U}; slot < Kernel::Object::CapabilitySpacePool::Capacity; ++slot)
    {
        keys[slot] = DeriveIa64CapabilityKey(Kernel::Object::RuntimeSpaceKeyPurpose(slot));
    }
    return keys;
}

// The last bootstrap residue, after every unit's final Initialize outcome, before the pager gets an
// allocatable-memory grant.
static constinit Kernel::Memory::PhysicalPageImporter g_ia64MemoryImporter{};
[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool PrepareIa64MemoryImport() noexcept
{
    class MasterProof final : public Kernel::Device::IFirmwareMasterProof
    {
    public:
        [[nodiscard]] bool IsConfined(const Kernel::Device::PciFunctionAddress address) const noexcept override
        {
            return g_ia64Iommu != nullptr && g_ia64Iommu->IsConfined(address);
        }
        [[nodiscard]] bool IsHaltedEhci(Kernel::Device::PciFunctionAddress) const noexcept override
        {
            return false;
        }
        [[nodiscard]] bool KeepUnconfined(const Kernel::Device::PciFunctionAddress address) const noexcept override
        {
            return g_ia64Iommu != nullptr && g_ia64Iommu->IsQueueFunction(address);
        }
        void Report(const Kernel::Device::PciFunctionAddress address, const Foundation::UInt32 kind,
                    const Kernel::Device::FirmwareMasterOutcome outcome) noexcept override
        {
            if (outcome == Kernel::Device::FirmwareMasterOutcome::Unreadable)
            {
                return;
            }
            auto& evidence = Platform::Ia64::Diagnostics::GetIa64EvidenceRecord().MemoryImport;
            if (evidence.MasterRecorded < 64U)
            {
                const Foundation::Size index{static_cast<Foundation::Size>(evidence.MasterRecorded++)};
                evidence.MasterRequester[index] =
                    (static_cast<Foundation::UInt64>(address.GetSegment()) << 16U) | address.GetRequesterId();
                evidence.MasterClass[index] = kind;
                evidence.MasterOutcome[index] = static_cast<Foundation::UInt64>(outcome);
            }
            if (outcome != Kernel::Device::FirmwareMasterOutcome::Unconfined)
            {
                return;
            }
            Platform::Firmware::WriteDiagnosticAscii("ASTERIA:FIRMWARE-MASTER:UNCONFINED:");
            WriteIa64DiagnosticHex(address.GetSegment(), 4U);
            WriteIa64DiagnosticHex(address.GetRequesterId(), 4U);
            Platform::Firmware::WriteDiagnosticAscii(":CLASS=");
            WriteIa64DiagnosticHex(kind, 6U);
            Platform::Firmware::WriteDiagnosticAscii("\r\n");
        }
    } proof{};
    auto& evidence = Platform::Ia64::Diagnostics::GetIa64EvidenceRecord().MemoryImport;
    bool services{false};
    if (g_ia64PciEcamRuntimeOwner != nullptr)
    {
        auto& configuration = g_ia64PciEcamRuntimeOwner->GetConfigurationSpace();
        const auto gate = Kernel::Device::CheckFirmwareBusMasters(configuration, configuration,
                                                                  configuration.GetRegions(), true, proof);
        evidence.GateComplete = gate.Complete ? 1U : 0U;
        evidence.FunctionsChecked = gate.Checked;
        evidence.MasteringFunctions = gate.Mastering;
        evidence.UnconfinedFunctions = gate.Unconfined;
        services = gate.AllowsBootServices();
    }
    if (!g_ia64MemoryImporter.ExcludeBootInfo(g_bootInfo))
    {
        return false;
    }
    const auto* const records = g_ia64Iommu != nullptr ? g_ia64Iommu->GetHandoffRecords() : nullptr;
    for (Foundation::Size i{0U}; i < g_bootInfo.Acpi.IommuHandoffCount; ++i)
    {
        evidence.UnitBase[i] = g_bootInfo.Acpi.IommuHandoffs[i].RegisterBase;
        evidence.UnitKind[i] = g_bootInfo.Acpi.IommuHandoffs[i].Kind;
        if (records == nullptr || g_bootInfo.Acpi.IommuHandoffs[i].Kind != 1U)
        {
            services = false;
            continue;
        }
        const auto& record = records[i];
        services = services && record.AllowsBootServices();
        evidence.UnitOutcome[i] = static_cast<Foundation::UInt64>(record.Outcome);
        evidence.UnitReason[i] = record.Reason;
        evidence.UnitStatus[i] = record.Status;
        evidence.UnitFaultStatus[i] = record.FaultStatus;
        evidence.UnitQueue[i] = record.QueueAddress;
        evidence.UnitHead[i] = record.QueueHead;
        evidence.UnitTail[i] = record.QueueTail;
        evidence.UnitRoot[i] = record.RootAddress;
        evidence.UnitInterruptTable[i] = record.InterruptTable;
        evidence.UnitPageRequestQueue[i] = record.PageRequestQueue;
        evidence.UnitAdvancedFaultLog[i] = record.AdvancedFaultLog;
        Kernel::Memory::PhysicalPageImportExclusion exclusions[5U]{};
        const auto count = record.Exclusions(exclusions);
        for (Foundation::Size n{0U}; n < count; ++n)
        {
            if (!g_ia64MemoryImporter.Exclude(exclusions[n].Start, exclusions[n].Bytes))
            {
                return false;
            }
        }
    }
    evidence.UnitCount = g_bootInfo.Acpi.IommuHandoffCount;
    if (!g_ia64MemoryImporter.Build(g_bootInfo, services, &g_bootstrapPageAllocator))
    {
        return false;
    }
    g_ia64MemoryImporter.Record(evidence, g_bootInfo, services);
    if (evidence.BootInfoFaultRanges != 0U)
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:MEMORY:BOOTINFO-IN-BOOTSERVICES:");
        WriteIa64DiagnosticHex(evidence.BootInfoFaultRanges, 16U);
        Platform::Firmware::WriteDiagnosticAscii("\r\n");
    }
    return true;
}

// The pager runtime's boot inputs: its image, lazy region, frame pool,
// created spaces' keys, the measured ITC rate and the assignable devices
// (edu, the AHCI controller and the NIC) with the ECAM configuration space
// that controls them.
[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool
InitializeIa64Pager(Platform::Ia64::System::Ia64PagerRuntime& pager,
                    Platform::Ia64::Scheduler::Ia64BspKernelRuntime& runtime) noexcept
{
    if (!ConfigureIa64InterruptLines() || !PrepareIa64MemoryImport())
    {
        return false;
    }
    // This enlarged boot-only input lives in static storage, outside the 512-byte kernel-stack
    // budget.
    static const Platform::Ia64::System::Ia64PagerRuntimeInput input{
        .Collaborators = runtime.GetPagerCollaborators(),
        .Transport = g_ia64TlbShootdownMailbox,
        .ImageMemory = g_ia64PagerImageMemoryOwner,
        .ImageMapping = g_ia64PagerImageMappingOwner,
        .LazyPhysicalStart = g_ia64LazyRegion.PhysicalStart,
        .LazyVirtualStart = Platform::Ia64::Ia64InitialUserLazyVirtualStart,
        .LazyPages = g_ia64LazyRegion.PageCount,
        .FramePoolPhysicalStart = 0U,
        .FramePoolPages = 0U,
        .ImportRanges = g_ia64MemoryImporter.Ranges(),
        .ImportRangeCount = g_ia64MemoryImporter.Count(),
        .BitmapStorage = Ia64RuntimeSlab(Platform::Ia64::Memory::Ia64RuntimeBitmapOffsetBytes),
        .BitmapBytes = static_cast<Foundation::Size>(g_ia64RuntimeMemoryReservation.PageCount * 4096U -
                                                     Platform::Ia64::Memory::Ia64RuntimeRequiredBytes),
        .LowMemoryBlocks = g_bootInfo.LowMemoryBlocks,
        .FrameSlab = Ia64RuntimeSlab(Platform::Ia64::Memory::Ia64RuntimeFrameSlabOffsetBytes),
        .MemoryObjectSlab = Ia64RuntimeSlab(Platform::Ia64::Memory::Ia64RuntimeMemoryObjectSlabOffsetBytes),
        .SpaceKeys = DeriveIa64RuntimeSpaceKeys(),
        .TicksPerMicrosecond = g_ia64ItcTicksPerMicrosecond,
        .WatchdogSeconds = g_bootInfo.WatchdogSeconds,
        .SessionMode = g_bootInfo.SessionMode,
        .LoginEnabled = g_bootInfo.LoginEnabled,
        .DeviceFunction = g_ia64AssignableDevice.Function,
        .DeviceRegion = g_ia64AssignableDevice.Region,
        .AhciFunction = g_ia64AhciDevice.Function,
        .AhciRegion = g_ia64AhciDevice.Region,
        .AhciDiskWritable = g_ia64AhciDiskWritable,
        .UsbHostFunctions = {g_ia64UsbHostDevices[0U].Function, g_ia64UsbHostDevices[1U].Function,
                             g_ia64UsbHostDevices[2U].Function, g_ia64UsbHostDevices[3U].Function},
        .UsbHostRegions = {g_ia64UsbHostDevices[0U].Region, g_ia64UsbHostDevices[1U].Region,
                           g_ia64UsbHostDevices[2U].Region, g_ia64UsbHostDevices[3U].Region},
        .NicFunction = g_ia64NicDevice.Function,
        .NicRegion = g_ia64NicDevice.Region,
        .ScanoutRegion = g_ia64ScanoutRegion,
        .ScanoutMode =
            {
                g_bootInfo.Display.Width | (static_cast<Foundation::UInt64>(g_bootInfo.Display.Height) << 32U),
                g_bootInfo.Display.PixelsPerScanLine |
                    (static_cast<Foundation::UInt64>(g_bootInfo.Display.Format) << 32U),
            },
        .DeviceConfiguration = ((g_ia64PciEcamRuntimeOwner != nullptr) &&
                                (g_ia64AssignableDevice.Function.IsValid() || g_ia64AhciDevice.Function.IsValid() ||
                                 g_ia64UsbHostDevices[0U].Function.IsValid() || g_ia64NicDevice.Function.IsValid()))
                                   ? &g_ia64PciEcamRuntimeOwner->GetConfigurationSpace()
                                   : nullptr,
        .DeviceInterrupts = g_ia64DeviceInterruptsConfigured ? &g_ia64DeviceInterruptLines : nullptr,
        .I8042Topology = &g_bootInfo.Acpi,
        .PowerFacts = &g_bootInfo.Power,
        .PowerSalFacts = &g_bootInfo.Kernel.Sal,
        .I8042DsdtMatches = g_ia64I8042DsdtMatches,
        .PortSpaceBase =
            []() noexcept
        {
            Foundation::UInt64 base{0U};
            Foundation::Size count{0U};
            for (Foundation::Size index{0U}; index < g_bootInfo.MemoryRegionCount; ++index)
            {
                if (g_bootInfo.MemoryRegions[index].FirmwareType == 12U)
                {
                    base = g_bootInfo.MemoryRegions[index].PhysicalStart;
                    ++count;
                }
            }
            return count == 1U ? base : 0U;
        }(),
        .PortSpaceBytes =
            []() noexcept
        {
            for (Foundation::Size index{0U}; index < g_bootInfo.MemoryRegionCount; ++index)
            {
                const auto& region = g_bootInfo.MemoryRegions[index];
                if (region.FirmwareType == 12U)
                {
                    return (region.PageCount <= (~Foundation::UInt64{0U} >> 12U)) ? region.PageCount << 12U : 0U;
                }
            }
            return Foundation::UInt64{0U};
        }(),
        .Iommu = g_ia64Iommu,
        .InventoryPhysicalStart = g_bootInfo.Inventory.PhysicalStart,
        .InventoryPages = g_bootInfo.Inventory.Pages,
        .ModuleBytes = g_bootInfo.Files.ModuleBytes,
    };
    const auto initialized = pager.Initialize(input);
    RecordBootInventoryI8042(pager.GetI8042Declared());
    if (initialized != Platform::Ia64::System::Ia64PagerRuntimeError::None)
    {
        return false;
    }
    auto& evidence = Platform::Ia64::Diagnostics::GetIa64EvidenceRecord().Metadata;
    evidence.FrameSlots = Kernel::Memory::FrameObjectPool::Capacity;
    evidence.MemoryObjectSlots = Kernel::Memory::MemoryObjectPool::Capacity;
    Platform::Ia64::Diagnostics::PublishIa64Evidence(evidence.Published);
    return true;
}

// The boot pager's domain: its objects, capabilities and thread, attached to
// the kernel runtime so the initial-image dispatch admits it first.
[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool
PrepareIa64Pager(Platform::Ia64::Scheduler::Ia64BspKernelRuntime& runtime) noexcept
{
    if ((g_ia64PagerImageMemoryOwner == nullptr) || (g_ia64PagerImageMappingOwner == nullptr) ||
        (g_ia64TlbShootdownMailbox == nullptr))
    {
        return false;
    }
    auto* const pager = ::new (static_cast<void*>(g_ia64PagerRuntimeStorage)) Platform::Ia64::System::Ia64PagerRuntime{
        runtime.GetPagerCollaborators(), *g_ia64TlbShootdownMailbox, g_ia64RootResourceDomainLedger,
        DeriveIa64CapabilityKey(Kernel::Object::CapabilityKeyPurpose::PagerSpace),
        DeriveIa64CapabilityKey(Kernel::Object::CapabilityKeyPurpose::FaultTokenSpace)};
    if (!InitializeIa64Pager(*pager, runtime) || !runtime.AttachPager(*pager) ||
        (g_ia64DeviceInterruptsConfigured && !runtime.AttachDeviceInterrupts(g_ia64DeviceInterruptLines)))
    {
        return false;
    }
    // Threads the pager creates run in its domain on the runtime memory's
    // thread entry slots, admitted before any thread enters PL3.
    auto* const threads = ::new (static_cast<void*>(g_ia64UserThreadTableStorage))
        Platform::Ia64::System::Ia64UserThreadTable{runtime.GetThreadTableCollaborators()};
    return threads->Configure(pager->GetDomainRecord()) && pager->AttachThreads(*threads) &&
           runtime.AttachThreads(*threads);
}
#endif

[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool PrepareIa64ApplicationProcessorRuntimeMappings() noexcept
{
    if ((g_ia64BspRuntimeMemoryOwner == nullptr) || (g_ia64RootResourceDomain == nullptr) ||
        (g_ia64ApplicationProcessorRuntimeMappingOwner != nullptr))
    {
        return false;
    }
    if (g_ia64ApplicationProcessorRuntimeMappingInvalidator.Initialize(g_ia64PalBootstrapCapabilities.VirtualMemory) !=
        Platform::Ia64::Memory::Ia64LocalTlbInvalidatorInitializationError::None)
    {
        return false;
    }

    auto* const owner = ::new (static_cast<void*>(g_ia64ApplicationProcessorRuntimeMappingOwnerStorage))
        Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeMappingOwner{};
    const bool prepared = owner
                              ->Prepare(*g_ia64BspRuntimeMemoryOwner, g_ia64ApplicationProcessorExecutionStorageOwner,
                                        g_ia64ApplicationProcessorRuntimeWorkerStorageOwner, g_ia64BspObjectRegistry,
                                        g_ia64BspRootCapabilityIssuer, g_ia64RootResourceDomainIdentity,
                                        *g_ia64RootResourceDomain, g_ia64ApplicationProcessorRuntimeMappingDispatcher)
                              .HasValue();
    if (prepared)
    {
        g_ia64ApplicationProcessorRuntimeMappingOwner = owner;
    }
    return prepared;
}

[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool InitializeIa64ApplicationProcessorRuntimeRelease() noexcept
{
    const auto takeover =
        Platform::Ia64::Interrupt::GetNativeIa64ApplicationProcessorTakeoverCoordinator().GetProgressSnapshot();
    return g_ia64ApplicationProcessorRuntimeReleaseCoordinator.Initialize(takeover,
                                                                          g_ia64RuntimeProfiledProcessorMask) ==
           Platform::Ia64::Interrupt::Ia64ApplicationProcessorRuntimeInitializationError::None;
}

[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool PrepareIa64ApplicationProcessorKernelRuntime() noexcept
{
    if ((g_ia64ApplicationProcessorRuntimeMappingOwner == nullptr) || (g_ia64BspRuntimeMemoryOwner == nullptr) ||
        (g_ia64RootResourceDomain == nullptr) || (g_ia64TlbShootdownMailbox != nullptr) ||
        (g_ia64ApplicationProcessorKernelRuntimeOwner != nullptr))
    {
        return false;
    }
    const auto plan = g_ia64ApplicationProcessorRuntimeMappingOwner->GetPlan();
    if (!plan.HasValue())
    {
        return false;
    }
    if (g_ia64TlbShootdownNotifier.Initialize(g_ia64RuntimeProfiledProcessorMask) !=
        Platform::Ia64::Memory::Ia64PolledTlbShootdownNotifierInitializationError::None)
    {
        return false;
    }
    const auto notifierSnapshot = g_ia64TlbShootdownNotifier.GetSnapshot();
    if ((notifierSnapshot.State != Platform::Ia64::Memory::Ia64PolledTlbShootdownNotifierState::Ready) ||
        (notifierSnapshot.ProfiledProcessorMask != g_ia64RuntimeProfiledProcessorMask) ||
        (notifierSnapshot.PendingProcessorMask != 0U) || (notifierSnapshot.Reserved != 0U))
    {
        return false;
    }
    auto* const mailbox = ::new (static_cast<void*>(g_ia64TlbShootdownMailboxStorage))
        Kernel::Memory::TlbShootdownMailboxTransport{g_ia64TlbShootdownNotifier, g_ia64RuntimeProfiledProcessorMask};
    g_ia64TlbShootdownMailbox = mailbox;
    if (mailbox->GetOnlineProcessorMask() != g_ia64RuntimeProfiledProcessorMask)
    {
        return false;
    }
    auto* const runtime = ::new (static_cast<void*>(g_ia64ApplicationProcessorKernelRuntimeStorage))
        Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntime{};
    g_ia64ApplicationProcessorKernelRuntimeOwner = runtime;

    const auto prepared =
        runtime->Prepare(plan.GetValue(), g_ia64CommonRuntimeProcessorCapabilities.PhysicalAddressBits,
                         g_ia64CommonRuntimeProcessorCapabilities.ImplementedVirtualAddressMostSignificantBit,
                         g_ia64ApplicationProcessorRuntimeReleaseCoordinator);
    if (!prepared.HasValue())
    {
        return false;
    }
    if (runtime->ConfigurePortableSchedulerDependencies(*g_ia64BspRuntimeMemoryOwner, g_ia64BspObjectRegistry,
                                                        g_ia64RootResourceDomainIdentity, *g_ia64RootResourceDomain,
                                                        g_ia64BspRootCapabilityIssuer) !=
        Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeError::None)
    {
        return false;
    }
    return runtime->ConfigureTlbShootdownDependencies(g_ia64TlbShootdownNotifier, *mailbox,
                                                      g_ia64RuntimeProcessorCapabilities) ==
           Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeError::None;
}

[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool WaitForIa64ApplicationProcessorRuntimeRelease(
    const Kernel::Scheduler::CpuId processor,
    Platform::Ia64::Interrupt::Ia64ApplicationProcessorRuntimeRelease& release) noexcept
{
    using Platform::Ia64::Interrupt::Ia64ApplicationProcessorRuntimeTransitionError;
    using Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeError;

    for (;;)
    {
        const auto observed = g_ia64ApplicationProcessorRuntimeReleaseCoordinator.TryObserveRelease(processor);
        if (observed.HasValue())
        {
            auto* const runtime = g_ia64ApplicationProcessorKernelRuntimeOwner;
            if (runtime == nullptr)
            {
                return false;
            }
            const auto candidate = observed.GetValue();
            if (runtime->ClaimPhysicalRelease(candidate) != Ia64ApplicationProcessorKernelRuntimeError::None)
            {
                return false;
            }
            release = candidate;
            return true;
        }

        const Ia64ApplicationProcessorRuntimeTransitionError error = observed.GetError();
        if ((error != Ia64ApplicationProcessorRuntimeTransitionError::CoordinatorNotReady) &&
            (error != Ia64ApplicationProcessorRuntimeTransitionError::ProcessorNotReleased))
        {
            return false;
        }
#if defined(__ia64__)
        __asm__ volatile("hint @pause" ::: "memory");
#endif
    }
}

[[noreturn]] ASTERIA_NOINLINE_NOIPA void
AdmitAndPivotIa64ApplicationProcessorRuntime(const Kernel::Scheduler::CpuId processor,
                                             const Foundation::UInt64 generation) noexcept
{
    using Platform::Ia64::Memory::Ia64BspRuntimeExternalInterruptPublicationError;
    using Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeError;
    using Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimePhysicalFailure;

    auto* const runtime = g_ia64ApplicationProcessorKernelRuntimeOwner;
    if (runtime == nullptr)
    {
        Platform::Firmware::HaltAfterFirmwareExit();
    }
    if (g_ia64BspRuntimeMemoryOwner == nullptr)
    {
        static_cast<void>(runtime->PublishPhysicalFailure(
            processor, generation, Ia64ApplicationProcessorKernelRuntimePhysicalFailure::RootAdmissionRejected, 0U));
        Platform::Firmware::HaltAfterFirmwareExit();
    }
    if (g_ia64ApplicationProcessorRuntimeMappingOwner == nullptr)
    {
        static_cast<void>(runtime->PublishPhysicalFailure(
            processor, generation, Ia64ApplicationProcessorKernelRuntimePhysicalFailure::PivotRejected, 0U));
        Platform::Firmware::HaltAfterFirmwareExit();
    }

    const auto admission = g_ia64BspRuntimeMemoryOwner->AdmitApplicationProcessorPhysicalRoot(processor);
    if (!admission.HasValue())
    {
        static_cast<void>(runtime->PublishPhysicalFailure(
            processor, generation, Ia64ApplicationProcessorKernelRuntimePhysicalFailure::RootAdmissionRejected,
            static_cast<Foundation::UInt64>(admission.GetError())));
        Platform::Firmware::HaltAfterFirmwareExit();
    }

    const Ia64BspRuntimeExternalInterruptPublicationError publicationError =
        g_ia64BspRuntimeMemoryOwner->PublishExternalInterruptEntryContext(processor);
    if (publicationError != Ia64BspRuntimeExternalInterruptPublicationError::None)
    {
        static_cast<void>(runtime->PublishPhysicalFailure(
            processor, generation,
            Ia64ApplicationProcessorKernelRuntimePhysicalFailure::LocalTimerProofContextPublicationRejected,
            static_cast<Foundation::UInt64>(publicationError)));
        Platform::Firmware::HaltAfterFirmwareExit();
    }

    const auto evidence = g_ia64BspRuntimeMemoryOwner->GetPublishedExternalInterruptEvidence(processor);
    if (!evidence.HasValue())
    {
        static_cast<void>(runtime->PublishPhysicalFailure(
            processor, generation,
            Ia64ApplicationProcessorKernelRuntimePhysicalFailure::LocalTimerProofContextAccessRejected,
            static_cast<Foundation::UInt64>(evidence.GetError())));
        Platform::Firmware::HaltAfterFirmwareExit();
    }
    if (runtime->ConfigureLocalTimerInterruptProof(processor, generation, evidence.GetValue()) !=
        Ia64ApplicationProcessorKernelRuntimeError::None)
    {
        Platform::Firmware::HaltAfterFirmwareExit();
    }

    const auto plan = g_ia64ApplicationProcessorRuntimeMappingOwner->GetPlan();
    if (!plan.HasValue())
    {
        static_cast<void>(runtime->PublishPhysicalFailure(
            processor, generation, Ia64ApplicationProcessorKernelRuntimePhysicalFailure::PivotRejected,
            static_cast<Foundation::UInt64>(plan.GetError())));
        Platform::Firmware::HaltAfterFirmwareExit();
    }

    const Foundation::Size layoutIndex = static_cast<Foundation::Size>(processor.GetValue() - 1U);
    const auto pivotError = Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimePivot::Pivot(
        processor, plan.GetValue().Processors[layoutIndex],
        g_ia64CommonRuntimeProcessorCapabilities.PhysicalAddressBits,
        g_ia64CommonRuntimeProcessorCapabilities.ImplementedVirtualAddressMostSignificantBit);
    static_cast<void>(runtime->PublishPhysicalFailure(
        processor, generation, Ia64ApplicationProcessorKernelRuntimePhysicalFailure::PivotRejected,
        static_cast<Foundation::UInt64>(pivotError)));
    Platform::Firmware::HaltAfterFirmwareExit();
}

[[nodiscard]] constexpr bool Ia64RuntimeReservedBytesAreZero(const Foundation::UInt8* const bytes,
                                                             const Foundation::Size count) noexcept
{
    for (Foundation::Size index{0U}; index < count; ++index)
    {
        if (bytes[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool
Ia64RuntimeLocalTimerEvidenceMatches(const Platform::Ia64::Interrupt::Ia64LocalTimerInterruptEvidence& left,
                                     const Platform::Ia64::Interrupt::Ia64LocalTimerInterruptEvidence& right) noexcept
{
    return (left.Sequence == right.Sequence) && (left.ProgrammedDeadlineTicks == right.ProgrammedDeadlineTicks) &&
           (left.ObservedIntervalTimeCounter == right.ObservedIntervalTimeCounter) &&
           (left.InterruptedInstructionPointer == right.InterruptedInstructionPointer) &&
           (left.InterruptedProcessorStatus == right.InterruptedProcessorStatus) &&
           (left.InterruptedIfsValid == right.InterruptedIfsValid) && (left.ProcessorIndex == right.ProcessorIndex) &&
           (left.Vector == right.Vector) && Ia64RuntimeReservedBytesAreZero(left.Reserved, sizeof(left.Reserved)) &&
           Ia64RuntimeReservedBytesAreZero(right.Reserved, sizeof(right.Reserved)) &&
           (left.AcknowledgedSequence == right.AcknowledgedSequence);
}

[[nodiscard]] constexpr bool Ia64RuntimeLocalTimerEvidencePayloadIsZero(
    const Platform::Ia64::Interrupt::Ia64LocalTimerInterruptEvidence& evidence) noexcept
{
    return (evidence.ProgrammedDeadlineTicks == 0U) && (evidence.ObservedIntervalTimeCounter == 0U) &&
           (evidence.InterruptedInstructionPointer == 0U) && (evidence.InterruptedProcessorStatus == 0U) &&
           (evidence.InterruptedIfsValid == 0U) && (evidence.ProcessorIndex == 0U) && (evidence.Vector == 0U) &&
           Ia64RuntimeReservedBytesAreZero(evidence.Reserved, sizeof(evidence.Reserved));
}

[[nodiscard]] constexpr bool Ia64RuntimeRegionZeroRangeIsValid(const Foundation::UInt64 address,
                                                               const Foundation::UInt64 sizeBytes,
                                                               const Foundation::UInt64 alignment) noexcept
{
    const Foundation::UInt64 regionZeroLimit = Platform::Ia64::Interrupt::Ia64ExternalInterruptRegionZeroLimit;
    return (address != 0U) && (sizeBytes != 0U) && (alignment != 0U) && ((alignment & (alignment - 1U)) == 0U) &&
           ((address & (alignment - 1U)) == 0U) && (sizeBytes <= regionZeroLimit) &&
           (address <= (regionZeroLimit - sizeBytes));
}

[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool
Ia64RuntimeRegisteredObjectStorageMatches(const Kernel::Object::ObjectId identity,
                                          const Kernel::Object::ObjectType type,
                                          const Foundation::UInt64 storageAddress) noexcept
{
    if (!identity.IsValid() || (storageAddress == 0U))
    {
        return false;
    }

    Kernel::Object::ObjectPin pin;
    if (g_ia64BspObjectRegistry.PinObject(identity, type, pin) != Kernel::Object::ObjectPinError::Ok)
    {
        return false;
    }
    Foundation::UInt64 registeredAddress{0U};
    switch (type)
    {
    case Kernel::Object::ObjectType::Timer:
        registeredAddress = static_cast<Foundation::UInt64>(
            reinterpret_cast<Foundation::UIntPointer>(static_cast<Kernel::Time::Timer*>(&pin.GetObject())));
        break;

    case Kernel::Object::ObjectType::Notification:
        registeredAddress = static_cast<Foundation::UInt64>(
            reinterpret_cast<Foundation::UIntPointer>(static_cast<Kernel::Ipc::Notification*>(&pin.GetObject())));
        break;

    default:
        pin.Reset();
        return false;
    }
    const bool matches = pin.GetObjectId().GetValue() == identity.GetValue() && registeredAddress == storageAddress;
    pin.Reset();
    return matches;
}

[[nodiscard]] constexpr bool Ia64RuntimeTimerQueueStatesMatch(const Kernel::Time::TimerQueueState& left,
                                                              const Kernel::Time::TimerQueueState& right) noexcept
{
    return (left.NextDeadlineTicks == right.NextDeadlineTicks) && (left.NextClaimSequence == right.NextClaimSequence) &&
           (left.ScheduledEntries == right.ScheduledEntries) && (left.PendingEntries == right.PendingEntries) &&
           (left.ClaimedEntries == right.ClaimedEntries) &&
           Ia64RuntimeReservedBytesAreZero(left.Reserved, sizeof(left.Reserved)) &&
           Ia64RuntimeReservedBytesAreZero(right.Reserved, sizeof(right.Reserved));
}

[[nodiscard]] constexpr bool Ia64RuntimeServiceSnapshotsMatch(
    const Platform::Ia64::Scheduler::Ia64ApplicationProcessorPortableSchedulerServiceSnapshot& left,
    const Platform::Ia64::Scheduler::Ia64ApplicationProcessorPortableSchedulerServiceSnapshot& right) noexcept
{
    return (left.RequestSequence == right.RequestSequence) && (left.CompletionSequence == right.CompletionSequence) &&
           (left.RequestCookie == right.RequestCookie) && (left.CompletionCookie == right.CompletionCookie) &&
           (left.CompletionEvidenceValue == right.CompletionEvidenceValue) &&
           (left.CompletionRunSequence == right.CompletionRunSequence) && (left.DispatchCount == right.DispatchCount) &&
           (left.WakeCount == right.WakeCount) && (left.ServiceCount == right.ServiceCount) &&
           (left.State == right.State) && (left.Result == right.Result) &&
           Ia64RuntimeReservedBytesAreZero(left.Reserved, sizeof(left.Reserved)) &&
           Ia64RuntimeReservedBytesAreZero(right.Reserved, sizeof(right.Reserved));
}

[[nodiscard]] Platform::Ia64::Diagnostics::Ia64ApplicationProcessorEvidence&
GetIa64ApplicationProcessorEvidence(const Kernel::Scheduler::CpuId processor) noexcept
{
    // Callers pass processors below the validated runtime summary's count, which never exceeds
    // Kernel::Scheduler::MaximumProcessorCount.
    return Platform::Ia64::Diagnostics::GetIa64EvidenceRecord().ApplicationProcessors.Processors[processor.GetValue()];
}

/// Checks an AP's retained local-timer proof against the published interrupt
/// record and the kernel's own contracts (publication, transport addresses,
/// the kernel's polling leaf, the timer's final disarmed state, and the
/// evidence validator for the observed sequence), records what the proof
/// observed, acknowledges the observed sequence, and records the
/// acknowledgement. The scenario's values (sequence, task priority,
/// deadline and grace) are recorded, not compared.
[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool AcknowledgeIa64ApplicationProcessorLocalTimerProof(
    const Kernel::Scheduler::CpuId processor,
    const Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeProcessorSnapshot& snapshot) noexcept
{
    using Platform::Ia64::Interrupt::Ia64ExternalInterruptPollStatus;
    using Platform::Ia64::Interrupt::Ia64LocalTimerInterruptEvidenceValidationError;
    using Platform::Ia64::Memory::Ia64BspRuntimeExternalInterruptAcknowledgementError;
    using Platform::Ia64::Memory::Ia64BspRuntimeExternalInterruptPublicationError;
    using Platform::Ia64::Memory::Ia64RuntimeExternalInterruptPublicationError;
    using Platform::Ia64::Scheduler::Ia64ApplicationProcessorLocalTimerProofPhase;
    using Platform::Ia64::Time::Ia64LocalTimerState;

    constexpr Foundation::UInt64 timerVector = Platform::Ia64::Interrupt::Ia64ExternalInterruptTimerVector;
    constexpr Foundation::UInt64 contextSizeBytes =
        sizeof(Platform::Ia64::Interrupt::Ia64ExternalInterruptEntryContext);
    constexpr Foundation::UInt64 evidenceSizeBytes = sizeof(Platform::Ia64::Interrupt::Ia64LocalTimerInterruptEvidence);

    if (g_ia64BspRuntimeMemoryOwner == nullptr)
    {
        return false;
    }

    const auto& proof = snapshot.LocalTimerProof;
    const auto expectedPollCodeRange = Platform::Ia64::Interrupt::GetIa64ExternalInterruptPollCodeRange();
    Foundation::UInt64 publicationContextAddress{0U};
    Foundation::UInt64 publicationEvidenceAddress{0U};
    Foundation::UInt64 observedSequence{0U};
    {
        const auto publicationResult = g_ia64BspRuntimeMemoryOwner->GetExternalInterruptPublicationSnapshot(processor);
        const auto publishedEvidenceResult =
            g_ia64BspRuntimeMemoryOwner->GetPublishedExternalInterruptEvidence(processor);
        if (!publicationResult.HasValue() || !publishedEvidenceResult.HasValue())
        {
            return false;
        }

        const auto& publication = publicationResult.GetValue();
        const auto* const publishedEvidence = publishedEvidenceResult.GetValue();
        const Foundation::UInt64 publishedEvidenceAddress =
            static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(publishedEvidence));
        if (!Ia64RuntimeRegionZeroRangeIsValid(publication.ContextAddress, contextSizeBytes,
                                               alignof(Platform::Ia64::Interrupt::Ia64ExternalInterruptEntryContext)) ||
            !Ia64RuntimeRegionZeroRangeIsValid(publication.EvidenceAddress, evidenceSizeBytes,
                                               alignof(Platform::Ia64::Interrupt::Ia64LocalTimerInterruptEvidence)))
        {
            return false;
        }

        const Foundation::UInt64 contextEnd = publication.ContextAddress + contextSizeBytes;
        const Foundation::UInt64 evidenceEnd = publication.EvidenceAddress + evidenceSizeBytes;
        const bool transportRangesOverlap =
            (publication.ContextAddress < evidenceEnd) && (publication.EvidenceAddress < contextEnd);
        const bool pollCodeRangeIsExact = (proof.PollCodeRange.Start == expectedPollCodeRange.Start) &&
                                          (proof.PollCodeRange.End == expectedPollCodeRange.End);
        const bool pollCodeRangeIsValid =
            (proof.PollCodeRange.Start != 0U) && (proof.PollCodeRange.End != 0U) &&
            ((proof.PollCodeRange.Start & 15U) == 0U) && ((proof.PollCodeRange.End & 15U) == 0U) &&
            (proof.PollCodeRange.Start < proof.PollCodeRange.End) &&
            (proof.PollCodeRange.End <= Platform::Ia64::Interrupt::Ia64ExternalInterruptRegionZeroLimit);
        if ((proof.Phase != Ia64ApplicationProcessorLocalTimerProofPhase::Completed) ||
            (proof.PollStatus != Ia64ExternalInterruptPollStatus::Completed) ||
            (proof.ValidationResult != Ia64LocalTimerInterruptEvidenceValidationError::None) ||
            (proof.EvidenceAddress != publication.EvidenceAddress) ||
            (publishedEvidenceAddress != publication.EvidenceAddress) || (proof.ProgrammedDeadlineTicks == 0U) ||
            !pollCodeRangeIsExact || !pollCodeRangeIsValid || transportRangesOverlap ||
            (proof.FinalTimerSnapshot.State != Ia64LocalTimerState::Ready) ||
            (proof.FinalTimerSnapshot.Vector != timerVector) ||
            (proof.FinalTimerSnapshot.ProgrammedDeadlineTicks != 0U) || (proof.FinalTimerSnapshot.Armed != 0U) ||
            !Ia64RuntimeReservedBytesAreZero(proof.FinalTimerSnapshot.Reserved,
                                             sizeof(proof.FinalTimerSnapshot.Reserved)) ||
            !Ia64RuntimeReservedBytesAreZero(proof.Reserved, sizeof(proof.Reserved)) || !publication.Published ||
            (publication.LastError != Ia64BspRuntimeExternalInterruptPublicationError::None) ||
            (publication.LastInterruptVectorTableError != Ia64RuntimeExternalInterruptPublicationError::None) ||
            (publication.LastAcknowledgementError != Ia64BspRuntimeExternalInterruptAcknowledgementError::None) ||
            !Ia64RuntimeReservedBytesAreZero(publication.Reserved, sizeof(publication.Reserved)) ||
            (publication.Context.EvidenceAddress != publication.EvidenceAddress) ||
            (publication.Context.ProcessorIndex != processor.GetValue()) ||
            (publication.Context.TimerVector != timerVector) ||
            (publication.Context.ExternalPreemptionControlAddress != 0U) ||
            !Ia64RuntimeLocalTimerEvidenceMatches(proof.InterruptEvidence, publication.Evidence) ||
            (publication.Evidence.ProgrammedDeadlineTicks != proof.ProgrammedDeadlineTicks) ||
            (Platform::Ia64::Interrupt::ValidateIa64LocalTimerInterruptEvidenceForSequence(
                 publication.Evidence, publication.Evidence.Sequence, processor.GetValue(),
                 proof.ProgrammedDeadlineTicks, proof.PollCodeRange.Start,
                 proof.PollCodeRange.End) != Ia64LocalTimerInterruptEvidenceValidationError::None))
        {
            return false;
        }
        publicationContextAddress = publication.ContextAddress;
        publicationEvidenceAddress = publication.EvidenceAddress;
        observedSequence = publication.Evidence.Sequence;

        Platform::Ia64::Diagnostics::Ia64ApplicationProcessorEvidence& evidence =
            GetIa64ApplicationProcessorEvidence(processor);
        evidence.LocalTimerVector = proof.Vector;
        evidence.LocalTimerTaskPriority = proof.TaskPriority;
        evidence.LocalTimerDeadlineTicks = proof.ProgrammedDeadlineTicks;
        evidence.LocalTimerPollDeadlineTicks = proof.PollDeadlineTicks;
        evidence.LocalTimerPollCodeStart = proof.PollCodeRange.Start;
        evidence.LocalTimerPollCodeEnd = proof.PollCodeRange.End;
        evidence.LocalTimerInterrupt = publication.Evidence;
    }

    const Ia64BspRuntimeExternalInterruptAcknowledgementError acknowledgementError =
        g_ia64BspRuntimeMemoryOwner->AcknowledgeExternalInterruptEvidence(processor, observedSequence);
    if (acknowledgementError != Ia64BspRuntimeExternalInterruptAcknowledgementError::None)
    {
        return false;
    }

    const auto acknowledgedPublicationResult =
        g_ia64BspRuntimeMemoryOwner->GetExternalInterruptPublicationSnapshot(processor);
    if (!acknowledgedPublicationResult.HasValue())
    {
        return false;
    }
    const auto& acknowledgedPublication = acknowledgedPublicationResult.GetValue();
    if (!acknowledgedPublication.Published || (acknowledgedPublication.ContextAddress != publicationContextAddress) ||
        (acknowledgedPublication.EvidenceAddress != publicationEvidenceAddress) ||
        (acknowledgedPublication.Context.EvidenceAddress != publicationEvidenceAddress) ||
        (acknowledgedPublication.Context.ProcessorIndex != processor.GetValue()) ||
        (acknowledgedPublication.Context.TimerVector != timerVector) ||
        (acknowledgedPublication.Context.ExternalPreemptionControlAddress != 0U) ||
        (acknowledgedPublication.LastError != Ia64BspRuntimeExternalInterruptPublicationError::None) ||
        (acknowledgedPublication.LastInterruptVectorTableError != Ia64RuntimeExternalInterruptPublicationError::None) ||
        (acknowledgedPublication.LastAcknowledgementError !=
         Ia64BspRuntimeExternalInterruptAcknowledgementError::None) ||
        !Ia64RuntimeReservedBytesAreZero(acknowledgedPublication.Reserved, sizeof(acknowledgedPublication.Reserved)) ||
        (acknowledgedPublication.Evidence.Sequence != observedSequence) ||
        (acknowledgedPublication.Evidence.AcknowledgedSequence != observedSequence) ||
        !Ia64RuntimeLocalTimerEvidencePayloadIsZero(acknowledgedPublication.Evidence))
    {
        return false;
    }
    GetIa64ApplicationProcessorEvidence(processor).LocalTimerAcknowledgedSequence =
        acknowledgedPublication.Evidence.AcknowledgedSequence;
    return true;
}

/// The kernel's own state contract of an AP whose portable scheduler serves:
/// registered identities, distinct block tokens, and an online, idle
/// scheduler with its worker blocked on its bound scheduling context. Run
/// sequences, timer-queue entries and run-loop bindings are recorded
/// (RecordIa64ApplicationProcessorScheduler), not compared.
[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool ValidateIa64ApplicationProcessorSchedulerState(
    const Kernel::Scheduler::CpuId processor,
    const Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeProcessorSnapshot& snapshot) noexcept
{
    using Platform::Ia64::Scheduler::Ia64ApplicationProcessorPortableSchedulerPhase;

    const auto& scheduler = snapshot.PortableScheduler;
    return scheduler.AddressSpaceIdentity.IsValid() &&
           scheduler.AddressSpaceIdentity.IsFromRegistry(g_ia64BspObjectRegistry) &&
           scheduler.ProtectionDomainIdentity.IsValid() &&
           scheduler.ProtectionDomainIdentity.IsFromRegistry(g_ia64BspObjectRegistry) &&
           scheduler.ThreadIdentity.IsValid() && scheduler.ThreadIdentity.IsFromRegistry(g_ia64BspObjectRegistry) &&
           scheduler.SchedulingContextIdentity.IsValid() &&
           scheduler.SchedulingContextIdentity.IsFromRegistry(g_ia64BspObjectRegistry) &&
           (scheduler.InitialBlockToken != 0U) && (scheduler.ServingBlockToken != 0U) &&
           (scheduler.InitialBlockToken != scheduler.ServingBlockToken) &&
           (scheduler.Processor.GetValue() == processor.GetValue()) &&
           (scheduler.Phase == Ia64ApplicationProcessorPortableSchedulerPhase::Online) &&
           (scheduler.ThreadState == Kernel::Scheduler::ThreadState::Blocked) &&
           (scheduler.SchedulingContextState == Kernel::Scheduler::SchedulingContextState::Bound) &&
           (scheduler.ControllerState == Kernel::Scheduler::ProcessorRunState::Idle) &&
           (scheduler.RunLoopState == Kernel::Scheduler::ProcessorKernelRunLoopState::Idle) &&
           (scheduler.DeadlineState == Kernel::Time::ProcessorDeadlineState::Disarmed) &&
           (scheduler.LocalTimerState == Platform::Ia64::Time::Ia64LocalTimerState::Ready) &&
           Ia64RuntimeReservedBytesAreZero(scheduler.Reserved, sizeof(scheduler.Reserved));
}

void RecordIa64ApplicationProcessorScheduler(
    const Kernel::Scheduler::CpuId processor,
    const Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeProcessorSnapshot& snapshot) noexcept
{
    const auto& scheduler = snapshot.PortableScheduler;
    Platform::Ia64::Diagnostics::Ia64ApplicationProcessorEvidence& evidence =
        GetIa64ApplicationProcessorEvidence(processor);
    evidence.InitialRunSequence = scheduler.InitialRunSequence;
    evidence.FirstResumedRunSequence = scheduler.FirstResumedRunSequence;
    evidence.TimerQueueScheduledEntries = scheduler.TimerQueueScheduledEntries;
    evidence.TimerQueuePendingEntries = scheduler.TimerQueuePendingEntries;
    evidence.TimerQueueClaimedEntries = scheduler.TimerQueueClaimedEntries;
    evidence.RunLoopBindingCount = scheduler.RunLoopBindingCount;
}

/// The kernel's own contract of an AP's persistent-scheduler service after it
/// completed a request: idle with a completed result, whose completion value
/// is the resumed worker's stack pointer. Sequences, cookie echoes, the
/// completion run sequence and the dispatch/wake/service counters are
/// recorded (RecordIa64ApplicationProcessorService), not compared.
[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool ValidateIa64ApplicationProcessorServiceState(
    const Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeProcessorSnapshot& snapshot) noexcept
{
    using Platform::Ia64::Scheduler::Ia64ApplicationProcessorPortableSchedulerServiceResult;
    using Platform::Ia64::Scheduler::Ia64ApplicationProcessorPortableSchedulerServiceState;

    const auto& service = snapshot.Service;
    return (service.CompletionEvidenceValue == snapshot.ResumedWorkerEntryEvidence.StackPointer) &&
           (service.CompletionEvidenceValue != 0U) &&
           (service.State == Ia64ApplicationProcessorPortableSchedulerServiceState::Idle) &&
           (service.Result == Ia64ApplicationProcessorPortableSchedulerServiceResult::Completed) &&
           Ia64RuntimeReservedBytesAreZero(service.Reserved, sizeof(service.Reserved));
}

void RecordIa64ApplicationProcessorService(
    const Kernel::Scheduler::CpuId processor, const Foundation::UInt64 serviceCookie,
    const Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeProcessorSnapshot& snapshot) noexcept
{
    const auto& service = snapshot.Service;
    Platform::Ia64::Diagnostics::Ia64ApplicationProcessorEvidence& evidence =
        GetIa64ApplicationProcessorEvidence(processor);
    evidence.ServiceCookie = serviceCookie;
    evidence.ServiceRequestSequence = service.RequestSequence;
    evidence.ServiceCompletionSequence = service.CompletionSequence;
    evidence.ServiceRequestCookie = service.RequestCookie;
    evidence.ServiceCompletionCookie = service.CompletionCookie;
    evidence.ServiceCompletionValue = service.CompletionEvidenceValue;
    evidence.ResumedWorkerStackPointer = snapshot.ResumedWorkerEntryEvidence.StackPointer;
    evidence.ServiceCompletionRunSequence = service.CompletionRunSequence;
    evidence.ServiceDispatchCount = service.DispatchCount;
    evidence.ServiceWakeCount = service.WakeCount;
    evidence.ServiceCount = service.ServiceCount;
}

[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool ValidateIa64ApplicationProcessorTimerDeliveryRegisteredObjects(
    const Platform::Ia64::Scheduler::Ia64ApplicationProcessorTimerDeliveryRecord& record) noexcept
{
    const auto& registration = record.Registration;
    const auto& timer = registration.TimerIdentity;
    const auto& notification = registration.NotificationIdentity;
    if (!registration.Registration.IsValid() || !timer.IsValid() || !timer.IsFromRegistry(g_ia64BspObjectRegistry) ||
        !notification.IsValid() || !notification.IsFromRegistry(g_ia64BspObjectRegistry) ||
        (timer.GetValue() == notification.GetValue()) ||
        (record.TimerStorageAddress % alignof(Kernel::Time::Timer)) != 0U ||
        (record.NotificationStorageAddress % alignof(Kernel::Ipc::Notification)) != 0U ||
        (record.TimerStorageAddress == record.NotificationStorageAddress) ||
        !record.Delivery.TimerIdentity.IsEqualTo(timer) ||
        !record.Delivery.NotificationIdentity.IsEqualTo(notification))
    {
        return false;
    }

    return Ia64RuntimeRegisteredObjectStorageMatches(timer.GetObjectId(), Kernel::Object::ObjectType::Timer,
                                                     record.TimerStorageAddress) &&
           Ia64RuntimeRegisteredObjectStorageMatches(
               notification.GetObjectId(), Kernel::Object::ObjectType::Notification, record.NotificationStorageAddress);
}

/// The kernel's own contracts of one AP-local deferred timer delivery: every
/// receipt and snapshot agrees with the others (one arm, one expiration of
/// that arm, one claimed notification, the same final queue everywhere), and
/// every object that should be idle, empty, disarmed or blocked afterwards
/// is. The transaction's sequences and counts are recorded
/// (RecordIa64ApplicationProcessorTimerDelivery), not compared.
[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool ValidateIa64ApplicationProcessorTimerDeliveryTransaction(
    const Kernel::Scheduler::CpuId processor,
    const Platform::Ia64::Scheduler::Ia64ApplicationProcessorTimerDeliveryRecord& record) noexcept
{
    using Kernel::Ipc::NotificationState;
    using Kernel::Scheduler::ProcessorKernelRunLoopState;
    using Kernel::Scheduler::ProcessorRunDispatchMode;
    using Kernel::Scheduler::ProcessorRunState;
    using Kernel::Scheduler::ProcessorSchedulerState;
    using Kernel::Scheduler::SchedulingContextState;
    using Kernel::Scheduler::ThreadState;
    using Kernel::Time::DeferredTimerDrainStop;
    using Kernel::Time::DeferredTimerWorkerState;
    using Kernel::Time::ProcessorDeadlineSource;
    using Kernel::Time::ProcessorDeadlineState;
    using Kernel::Time::TimerState;
    using Platform::Ia64::Interrupt::Ia64ExternalInterruptPollStatus;
    using Platform::Ia64::Scheduler::Ia64ApplicationProcessorTimerDeliveryResult;
    using Platform::Ia64::Time::Ia64LocalTimerState;

    const auto& arm = record.Arm;
    const auto& expiration = record.Delivery.Expiration;
    const auto& queue = record.Drain.QueueState;
    const auto& controller = record.FinalController;
    const Foundation::UInt64 notificationClaimSequence = record.NotificationClaim.GetSequence();
    constexpr Foundation::UInt8 timerQueueSource = static_cast<Foundation::UInt8>(ProcessorDeadlineSource::TimerQueue);
    constexpr Foundation::UInt8 timerVector = Platform::Ia64::Interrupt::Ia64ExternalInterruptTimerVector;

    return ValidateIa64ApplicationProcessorTimerDeliveryRegisteredObjects(record) &&
           (record.Processor.GetValue() == processor.GetValue()) &&
           (record.Result == Ia64ApplicationProcessorTimerDeliveryResult::Delivered) &&
           Ia64RuntimeReservedBytesAreZero(record.Reserved, sizeof(record.Reserved)) && arm.Timer.Token.IsValid() &&
           !arm.Timer.Replaced && (arm.Timer.DeadlineTicks != 0U) &&
           (arm.Timer.ObservedTicks < arm.Timer.DeadlineTicks) &&
           Ia64RuntimeReservedBytesAreZero(arm.Timer.Reserved, sizeof(arm.Timer.Reserved)) &&
           (arm.NextDeadlineTicks == arm.Timer.DeadlineTicks) &&
           (record.ArmedQueue.NextDeadlineTicks == arm.Timer.DeadlineTicks) &&
           (record.ArmedQueue.PendingEntries == 0U) && (record.ArmedQueue.ClaimedEntries == 0U) &&
           Ia64RuntimeReservedBytesAreZero(record.ArmedQueue.Reserved, sizeof(record.ArmedQueue.Reserved)) &&
           (record.ArmedDeadline.SelectedDeadlineTicks == arm.Timer.DeadlineTicks) &&
           (record.ArmedDeadline.ObservedTicks >= arm.Timer.ObservedTicks) &&
           (record.ArmedDeadline.ObservedTicks < arm.Timer.DeadlineTicks) &&
           (record.ArmedDeadline.State == ProcessorDeadlineState::Armed) &&
           (record.ArmedDeadline.SelectedSources == timerQueueSource) && (record.ArmedDeadline.Reprogrammed == 1U) &&
           Ia64RuntimeReservedBytesAreZero(record.ArmedDeadline.Reserved, sizeof(record.ArmedDeadline.Reserved)) &&
           (record.ArmedLocalTimer.State == Ia64LocalTimerState::Ready) &&
           (record.ArmedLocalTimer.Vector == timerVector) && (record.ArmedLocalTimer.Armed == 1U) &&
           (record.ArmedLocalTimer.ProgrammedDeadlineTicks == arm.Timer.DeadlineTicks) &&
           Ia64RuntimeReservedBytesAreZero(record.ArmedLocalTimer.Reserved, sizeof(record.ArmedLocalTimer.Reserved)) &&
           (record.PollStatus == Ia64ExternalInterruptPollStatus::Completed) &&
           (record.Drain.Stop == DeferredTimerDrainStop::Empty) &&
           (record.Drain.State == DeferredTimerWorkerState::Ready) &&
           record.Drain.FirstTimerIdentity.IsEqualTo(record.Registration.TimerIdentity) &&
           record.Drain.LastTimerIdentity.IsEqualTo(record.Registration.TimerIdentity) &&
           Ia64RuntimeReservedBytesAreZero(record.Drain.Reserved, sizeof(record.Drain.Reserved)) &&
           record.Delivery.Expiration.Token.IsEqualTo(arm.Timer.Token) &&
           (expiration.DeadlineTicks == arm.Timer.DeadlineTicks) &&
           (expiration.ObservedTicks >= expiration.DeadlineTicks) &&
           (expiration.ObservedTicks >= record.InterruptEvidence.ObservedIntervalTimeCounter) &&
           (record.Delivery.NotificationPublished == 1U) && (record.Delivery.QueueCommitted == 1U) &&
           (record.Delivery.NotificationState.LastClaimSequence == 0U) &&
           (record.Delivery.NotificationState.State == NotificationState::Ready) &&
           (record.Delivery.NotificationState.Pending == 1U) &&
           Ia64RuntimeReservedBytesAreZero(record.Delivery.NotificationState.Reserved,
                                           sizeof(record.Delivery.NotificationState.Reserved)) &&
           Ia64RuntimeReservedBytesAreZero(record.Delivery.Reserved, sizeof(record.Delivery.Reserved)) &&
           Ia64RuntimeTimerQueueStatesMatch(record.Delivery.QueueState, queue) && record.NotificationClaim.IsValid() &&
           (record.ClaimedNotification.LastClaimSequence == notificationClaimSequence) &&
           (record.ClaimedNotification.State == NotificationState::Claimed) &&
           (record.ClaimedNotification.Pending == 0U) &&
           Ia64RuntimeReservedBytesAreZero(record.ClaimedNotification.Reserved,
                                           sizeof(record.ClaimedNotification.Reserved)) &&
           (record.FinalNotification.LastClaimSequence == notificationClaimSequence) &&
           (record.FinalNotification.State == NotificationState::Ready) && (record.FinalNotification.Pending == 0U) &&
           Ia64RuntimeReservedBytesAreZero(record.FinalNotification.Reserved,
                                           sizeof(record.FinalNotification.Reserved)) &&
           !record.FinalTimer.Token.IsValid() && (record.FinalTimer.Token.GetSequence() == 0U) &&
           (record.FinalTimer.DeadlineTicks == 0U) && (record.FinalTimer.State == TimerState::Disarmed) &&
           Ia64RuntimeReservedBytesAreZero(record.FinalTimer.Reserved, sizeof(record.FinalTimer.Reserved)) &&
           (queue.NextDeadlineTicks == 0U) && (queue.ScheduledEntries == 0U) && (queue.PendingEntries == 0U) &&
           (queue.ClaimedEntries == 0U) && Ia64RuntimeReservedBytesAreZero(queue.Reserved, sizeof(queue.Reserved)) &&
           Ia64RuntimeTimerQueueStatesMatch(record.FinalWorker.QueueState, queue) &&
           (record.FinalWorker.LastRegistrationSequence == record.Registration.Registration.GetSequence()) &&
           (record.FinalWorker.State == DeferredTimerWorkerState::Ready) &&
           Ia64RuntimeReservedBytesAreZero(record.FinalWorker.Reserved, sizeof(record.FinalWorker.Reserved)) &&
           (record.FinalDeadline.SelectedDeadlineTicks == 0U) &&
           (record.FinalDeadline.ObservedTicks >= record.Delivery.Expiration.ObservedTicks) &&
           (record.FinalDeadline.State == ProcessorDeadlineState::Disarmed) &&
           (record.FinalDeadline.SelectedSources == 0U) && (record.FinalDeadline.Reprogrammed == 1U) &&
           Ia64RuntimeReservedBytesAreZero(record.FinalDeadline.Reserved, sizeof(record.FinalDeadline.Reserved)) &&
           (controller.State == ProcessorRunState::Idle) &&
           (controller.Mode == ProcessorRunDispatchMode::HardwareDeadline) &&
           (controller.Scheduler.State == ProcessorSchedulerState::Idle) &&
           (controller.Scheduler.Processor.GetValue() == processor.GetValue()) &&
           !controller.Scheduler.ActiveThread.IsValid() && !controller.Scheduler.ActiveContext.IsValid() &&
           (controller.Scheduler.Queue.ReadyEntries == 0U) && (controller.Scheduler.Queue.ClaimedEntries == 0U) &&
           (controller.Scheduler.Queue.ReservedEntries == 0U) &&
           (controller.Scheduler.Queue.Processor.GetValue() == processor.GetValue()) &&
           Ia64RuntimeReservedBytesAreZero(controller.Scheduler.Queue.Reserved,
                                           sizeof(controller.Scheduler.Queue.Reserved)) &&
           Ia64RuntimeReservedBytesAreZero(controller.Scheduler.Reserved, sizeof(controller.Scheduler.Reserved)) &&
           Ia64RuntimeTimerQueueStatesMatch(controller.TimerQueue, queue) &&
           (controller.Deadline.SelectedDeadlineTicks == 0U) &&
           (controller.Deadline.State == ProcessorDeadlineState::Disarmed) &&
           (controller.Deadline.SelectedSources == 0U) &&
           Ia64RuntimeReservedBytesAreZero(controller.Deadline.Reserved, sizeof(controller.Deadline.Reserved)) &&
           Ia64RuntimeReservedBytesAreZero(controller.Reserved, sizeof(controller.Reserved)) &&
           (record.FinalRunLoop.State == ProcessorKernelRunLoopState::Idle) &&
           (record.FinalRunLoop.IdleFailurePending == 0U) && (record.FinalRunLoop.ActiveThread == nullptr) &&
           (record.FinalRunLoop.ActiveProtectionDomain == nullptr) &&
           !record.FinalRunLoop.ActiveProtectionDomainIdentity.IsValid() &&
           (record.FinalRunLoop.ActiveAddressSpace == nullptr) &&
           !record.FinalRunLoop.ActiveAddressSpaceIdentity.IsValid() &&
           Ia64RuntimeReservedBytesAreZero(record.FinalRunLoop.Reserved, sizeof(record.FinalRunLoop.Reserved)) &&
           (record.FinalThread.State == ThreadState::Blocked) && (record.FinalThread.ExitCode == 0U) &&
           !record.FinalThread.RunningProcessor.IsValid() && record.FinalThread.BlockToken.IsValid() &&
           Ia64RuntimeReservedBytesAreZero(record.FinalThread.Reserved, sizeof(record.FinalThread.Reserved)) &&
           (record.FinalSchedulingContext.State == SchedulingContextState::Bound) &&
           !record.FinalSchedulingContext.RunningProcessor.IsValid() &&
           (record.FinalSchedulingContext.RemainingBudgetTicks != 0U) &&
           (record.FinalSchedulingContext.NextReplenishmentTicks != 0U) &&
           (record.FinalSchedulingContext.AbsoluteDeadlineTicks ==
            record.FinalSchedulingContext.NextReplenishmentTicks) &&
           (record.FinalSchedulingContext.RunStartTicks == 0U) && (record.FinalSchedulingContext.DonationDepth == 0U) &&
           (record.FinalSchedulingContext.IsBudgetExhausted == 0U) &&
           Ia64RuntimeReservedBytesAreZero(record.FinalSchedulingContext.Reserved,
                                           sizeof(record.FinalSchedulingContext.Reserved)) &&
           (record.FinalLocalTimer.State == Ia64LocalTimerState::Ready) &&
           (record.FinalLocalTimer.Vector == timerVector) && (record.FinalLocalTimer.Armed == 0U) &&
           (record.FinalLocalTimer.ProgrammedDeadlineTicks == 0U) &&
           Ia64RuntimeReservedBytesAreZero(record.FinalLocalTimer.Reserved, sizeof(record.FinalLocalTimer.Reserved));
}

/// The kernel's contracts of a completed timer-delivery gate: the completion
/// answers the BSP's request, the AP still serves, the transaction holds
/// together, and its interrupt record passes the evidence validator for the
/// observed sequence against the armed deadline and the kernel's polling
/// leaf.
[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool ValidateIa64ApplicationProcessorTimerDeliveryCompletion(
    const Kernel::Scheduler::CpuId processor, const Foundation::UInt64 generation,
    const Foundation::UInt64 requestSequence,
    const Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeProcessorSnapshot& snapshot) noexcept
{
    using Platform::Ia64::Interrupt::Ia64LocalTimerInterruptEvidenceValidationError;
    using Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeProcessorState;
    using Platform::Ia64::Scheduler::Ia64ApplicationProcessorTimerDeliveryResult;
    using Platform::Ia64::Scheduler::Ia64ApplicationProcessorTimerDeliveryState;

    constexpr Foundation::UInt64 evidenceSizeBytes = sizeof(Platform::Ia64::Interrupt::Ia64LocalTimerInterruptEvidence);

    const auto& gate = snapshot.TimerDelivery;
    const auto& record = snapshot.TimerDeliveryRecord;
    const auto& evidence = record.InterruptEvidence;
    const auto expectedPollCodeRange = Platform::Ia64::Interrupt::GetIa64ExternalInterruptPollCodeRange();
    const Foundation::UInt64 deadline = record.Arm.Timer.DeadlineTicks;

    return (snapshot.Processor.GetValue() == processor.GetValue()) &&
           (snapshot.Layout.Processor.GetValue() == processor.GetValue()) && (snapshot.Generation == generation) &&
           (snapshot.State == Ia64ApplicationProcessorKernelRuntimeProcessorState::IdleServing) &&
           ValidateIa64ApplicationProcessorSchedulerState(processor, snapshot) &&
           ValidateIa64ApplicationProcessorServiceState(snapshot) && (gate.RequestSequence == requestSequence) &&
           (gate.CompletionSequence == requestSequence) &&
           (gate.State == Ia64ApplicationProcessorTimerDeliveryState::Completed) &&
           (gate.Result == Ia64ApplicationProcessorTimerDeliveryResult::Delivered) &&
           Ia64RuntimeReservedBytesAreZero(gate.Reserved, sizeof(gate.Reserved)) &&
           ValidateIa64ApplicationProcessorTimerDeliveryTransaction(processor, record) &&
           Ia64RuntimeServiceSnapshotsMatch(record.FinalService, snapshot.Service) &&
           record.FinalThread.BlockToken.IsValid() &&
           (record.FinalThread.BlockToken.GetValue() == snapshot.PortableScheduler.ServingBlockToken) &&
           record.FinalSchedulingContext.BaseThread.IsEqualTo(snapshot.PortableScheduler.ThreadIdentity) &&
           record.FinalSchedulingContext.CurrentThread.IsEqualTo(snapshot.PortableScheduler.ThreadIdentity) &&
           (record.PollCodeRange.Start == expectedPollCodeRange.Start) &&
           (record.PollCodeRange.End == expectedPollCodeRange.End) && (record.PollCodeRange.Start != 0U) &&
           (record.PollCodeRange.End != 0U) && ((record.PollCodeRange.Start & 15U) == 0U) &&
           ((record.PollCodeRange.End & 15U) == 0U) && (record.PollCodeRange.Start < record.PollCodeRange.End) &&
           (record.PollCodeRange.End <= Platform::Ia64::Interrupt::Ia64ExternalInterruptRegionZeroLimit) &&
           (deadline != 0U) &&
           Ia64RuntimeRegionZeroRangeIsValid(record.EvidenceAddress, evidenceSizeBytes,
                                             alignof(Platform::Ia64::Interrupt::Ia64LocalTimerInterruptEvidence)) &&
           (evidence.ProgrammedDeadlineTicks == deadline) && (evidence.ObservedIntervalTimeCounter >= deadline) &&
           (evidence.ProcessorIndex == processor.GetValue()) &&
           Ia64RuntimeReservedBytesAreZero(evidence.Reserved, sizeof(evidence.Reserved)) &&
           (Platform::Ia64::Interrupt::ValidateIa64LocalTimerInterruptEvidenceForSequence(
                evidence, evidence.Sequence, processor.GetValue(), deadline, record.PollCodeRange.Start,
                record.PollCodeRange.End) == Ia64LocalTimerInterruptEvidenceValidationError::None) &&
           Ia64RuntimeReservedBytesAreZero(snapshot.Reserved, sizeof(snapshot.Reserved));
}

void RecordIa64ApplicationProcessorTimerDelivery(
    const Kernel::Scheduler::CpuId processor,
    const Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeProcessorSnapshot& snapshot) noexcept
{
    const auto& gate = snapshot.TimerDelivery;
    const auto& record = snapshot.TimerDeliveryRecord;
    Platform::Ia64::Diagnostics::Ia64ApplicationProcessorEvidence& evidence =
        GetIa64ApplicationProcessorEvidence(processor);
    evidence.DeliveryRequestSequence = gate.RequestSequence;
    evidence.DeliveryCompletionSequence = gate.CompletionSequence;
    evidence.DeliveryRegistrationSequence = record.Registration.Registration.GetSequence();
    evidence.DeliveryArmSequence = record.Arm.Timer.Token.GetSequence();
    evidence.DeliveryArmedNextClaimSequence = record.ArmedQueue.NextClaimSequence;
    evidence.DeliveryArmedScheduledEntries = record.ArmedQueue.ScheduledEntries;
    evidence.DeliveryDeadlineTicks = record.Arm.Timer.DeadlineTicks;
    evidence.DeliveryPollDeadlineTicks = record.PollDeadlineTicks;
    evidence.DeliveryDrainedCount = record.Drain.DeliveredCount;
    evidence.DeliveryFirstClaimSequence = record.Drain.FirstClaimSequence;
    evidence.DeliveryLastClaimSequence = record.Drain.LastClaimSequence;
    evidence.DeliveryClaimSequence = record.Delivery.ClaimSequence;
    evidence.DeliveryNotificationClaimSequence = record.NotificationClaim.GetSequence();
    evidence.DeliveryFinalNextClaimSequence = record.Drain.QueueState.NextClaimSequence;
    evidence.DeliveryFinalRegistrationSequence = record.FinalWorker.LastRegistrationSequence;
    evidence.DeliveryFinalRegisteredBindings = record.FinalWorker.RegisteredBindings;
    evidence.DeliveryFinalSchedulerNextSequence = record.FinalController.Scheduler.Queue.NextSequence;
    evidence.DeliveryFinalThreadRunSequence = record.FinalThread.RunSequence;
    evidence.DeliveryFinalRunLoopBindingCount = record.FinalRunLoop.BindingCount;
    evidence.DeliveryInterrupt = record.InterruptEvidence;
}

/// Acknowledges the delivery's interrupt record (the observed sequence) after
/// checking the publication against the retained record, and records the
/// acknowledgement.
[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool AcknowledgeIa64ApplicationProcessorTimerDelivery(
    const Kernel::Scheduler::CpuId processor,
    const Platform::Ia64::Scheduler::Ia64ApplicationProcessorTimerDeliveryRecord& record) noexcept
{
    using Platform::Ia64::Memory::Ia64BspRuntimeExternalInterruptAcknowledgementError;
    using Platform::Ia64::Memory::Ia64BspRuntimeExternalInterruptPublicationError;
    using Platform::Ia64::Memory::Ia64RuntimeExternalInterruptPublicationError;

    constexpr Foundation::UInt8 timerVector = Platform::Ia64::Interrupt::Ia64ExternalInterruptTimerVector;
    constexpr Foundation::UInt64 contextSizeBytes =
        sizeof(Platform::Ia64::Interrupt::Ia64ExternalInterruptEntryContext);
    constexpr Foundation::UInt64 evidenceSizeBytes = sizeof(Platform::Ia64::Interrupt::Ia64LocalTimerInterruptEvidence);

    if (g_ia64BspRuntimeMemoryOwner == nullptr)
    {
        return false;
    }

    const auto publicationResult = g_ia64BspRuntimeMemoryOwner->GetExternalInterruptPublicationSnapshot(processor);
    const auto publishedEvidenceResult = g_ia64BspRuntimeMemoryOwner->GetPublishedExternalInterruptEvidence(processor);
    if (!publicationResult.HasValue() || !publishedEvidenceResult.HasValue())
    {
        return false;
    }

    const auto& publication = publicationResult.GetValue();
    const auto* const publishedEvidence = publishedEvidenceResult.GetValue();
    const Foundation::UInt64 publishedEvidenceAddress =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(publishedEvidence));
    if (!Ia64RuntimeRegionZeroRangeIsValid(publication.ContextAddress, contextSizeBytes,
                                           alignof(Platform::Ia64::Interrupt::Ia64ExternalInterruptEntryContext)) ||
        !Ia64RuntimeRegionZeroRangeIsValid(publication.EvidenceAddress, evidenceSizeBytes,
                                           alignof(Platform::Ia64::Interrupt::Ia64LocalTimerInterruptEvidence)))
    {
        return false;
    }
    const Foundation::UInt64 contextEnd = publication.ContextAddress + contextSizeBytes;
    const Foundation::UInt64 evidenceEnd = publication.EvidenceAddress + evidenceSizeBytes;
    if (!publication.Published || (publication.ContextAddress == publication.EvidenceAddress) ||
        ((publication.ContextAddress < evidenceEnd) && (publication.EvidenceAddress < contextEnd)) ||
        (publication.EvidenceAddress != record.EvidenceAddress) ||
        (publishedEvidenceAddress != publication.EvidenceAddress) ||
        (publication.Context.EvidenceAddress != publication.EvidenceAddress) ||
        (publication.Context.ProcessorIndex != processor.GetValue()) ||
        (publication.Context.TimerVector != timerVector) ||
        (publication.Context.ExternalPreemptionControlAddress != 0U) ||
        (publication.LastError != Ia64BspRuntimeExternalInterruptPublicationError::None) ||
        (publication.LastInterruptVectorTableError != Ia64RuntimeExternalInterruptPublicationError::None) ||
        (publication.LastAcknowledgementError != Ia64BspRuntimeExternalInterruptAcknowledgementError::None) ||
        !Ia64RuntimeReservedBytesAreZero(publication.Reserved, sizeof(publication.Reserved)) ||
        !Ia64RuntimeLocalTimerEvidenceMatches(publication.Evidence, record.InterruptEvidence))
    {
        return false;
    }

    const Foundation::UInt64 sequence = record.InterruptEvidence.Sequence;
    if (g_ia64BspRuntimeMemoryOwner->AcknowledgeExternalInterruptEvidence(processor, sequence) !=
        Ia64BspRuntimeExternalInterruptAcknowledgementError::None)
    {
        return false;
    }

    const auto acknowledgedResult = g_ia64BspRuntimeMemoryOwner->GetExternalInterruptPublicationSnapshot(processor);
    if (!acknowledgedResult.HasValue())
    {
        return false;
    }
    const auto& acknowledged = acknowledgedResult.GetValue();
    if (!acknowledged.Published || (acknowledged.ContextAddress != publication.ContextAddress) ||
        (acknowledged.EvidenceAddress != publication.EvidenceAddress) ||
        (acknowledged.Context.EvidenceAddress != publication.EvidenceAddress) ||
        (acknowledged.Context.ProcessorIndex != processor.GetValue()) ||
        (acknowledged.Context.TimerVector != timerVector) ||
        (acknowledged.Context.ExternalPreemptionControlAddress != 0U) ||
        (acknowledged.LastError != Ia64BspRuntimeExternalInterruptPublicationError::None) ||
        (acknowledged.LastInterruptVectorTableError != Ia64RuntimeExternalInterruptPublicationError::None) ||
        (acknowledged.LastAcknowledgementError != Ia64BspRuntimeExternalInterruptAcknowledgementError::None) ||
        !Ia64RuntimeReservedBytesAreZero(acknowledged.Reserved, sizeof(acknowledged.Reserved)) ||
        (acknowledged.Evidence.Sequence != sequence) || (acknowledged.Evidence.AcknowledgedSequence != sequence) ||
        !Ia64RuntimeLocalTimerEvidencePayloadIsZero(acknowledged.Evidence))
    {
        return false;
    }
    GetIa64ApplicationProcessorEvidence(processor).DeliveryAcknowledgedSequence =
        acknowledged.Evidence.AcknowledgedSequence;
    return true;
}

/// Waits until the AP completed the BSP's service request (the completion
/// answers `serviceSequence`) and serves idle, checks the published register
/// evidence and the kernel's scheduler and service contracts, records the
/// scheduler and service values, and acknowledges the AP's local-timer proof.
[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool ValidateIa64ApplicationProcessorRuntimeServing(
    const Kernel::Scheduler::CpuId processor, const Foundation::UInt64 generation,
    const Foundation::UInt64 serviceCookie, const Foundation::UInt64 serviceSequence,
    const Platform::Ia64::Interrupt::Ia64ApplicationProcessorRuntimeServingObservation& observation) noexcept
{
    using Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeAccessError;
    using Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeProcessorState;
    using Platform::Ia64::Scheduler::Ia64ApplicationProcessorPortableSchedulerServiceState;
    using Platform::Ia64::Scheduler::Ia64KernelContextState;

    auto* const runtime = g_ia64ApplicationProcessorKernelRuntimeOwner;
    if (runtime == nullptr)
    {
        return false;
    }
    if ((observation.Generation != generation) || (observation.Processor.GetValue() != processor.GetValue()) ||
        !Ia64RuntimeReservedBytesAreZero(observation.Reserved, sizeof(observation.Reserved)) ||
        !Ia64RuntimeReservedBytesAreZero(observation.Evidence.Reserved, sizeof(observation.Evidence.Reserved)))
    {
        return false;
    }

    auto& snapshot = g_ia64ApplicationProcessorKernelRuntimeSnapshot;
    bool ownerServingPublicationObserved{false};
    for (Platform::Ia64::Scheduler::Ia64CrossProcessorWaitBudget budget{g_ia64ApWaitClock.GetTicks(),
                                                                        Ia64ApWaitMinimumPolls, Ia64ApWaitMinimumTicks};
         budget.TryPoll(g_ia64ApWaitClock.GetTicks());)
    {
        // The BSP also services its mailbox while waiting.
        Platform::Synchronization::ProcessorWaitProgress();
        if (runtime->GetProcessorSnapshot(processor, snapshot) !=
            Ia64ApplicationProcessorKernelRuntimeAccessError::None)
        {
            return false;
        }
        if (snapshot.State == Ia64ApplicationProcessorKernelRuntimeProcessorState::IdleServing &&
            (snapshot.Service.State == Ia64ApplicationProcessorPortableSchedulerServiceState::Idle) &&
            (snapshot.Service.CompletionSequence == serviceSequence))
        {
            ownerServingPublicationObserved = true;
            break;
        }
        if ((snapshot.State != Ia64ApplicationProcessorKernelRuntimeProcessorState::SchedulerWaiting) &&
            (snapshot.State != Ia64ApplicationProcessorKernelRuntimeProcessorState::IdleServing))
        {
            return false;
        }
#if defined(__ia64__)
        __asm__ volatile("hint @pause" ::: "memory");
#endif
    }
    if (!ownerServingPublicationObserved)
    {
        Kernel::Scheduler::CrossProcessorWait::Timeout(Kernel::Scheduler::ProcessorSet::Bit(processor));
        return false;
    }
    if ((snapshot.Processor.GetValue() != processor.GetValue()) ||
        (snapshot.Layout.Processor.GetValue() != processor.GetValue()) || (snapshot.Generation != generation) ||
        (snapshot.State != Ia64ApplicationProcessorKernelRuntimeProcessorState::IdleServing) ||
        (snapshot.EvidenceMask != 0x0FU) || (snapshot.RuntimeEntryEvidence.CaptureComplete != 1U) ||
        (snapshot.FirstWorkerEntryEvidence.CaptureComplete != 1U) ||
        (snapshot.ResumedWorkerEntryEvidence.CaptureComplete != 1U) ||
        (snapshot.ServingIdleEvidence.CaptureComplete != 1U) ||
        !Ia64RuntimeReservedBytesAreZero(snapshot.RuntimeEntryEvidence.Reserved,
                                         sizeof(snapshot.RuntimeEntryEvidence.Reserved)) ||
        !Ia64RuntimeReservedBytesAreZero(snapshot.FirstWorkerEntryEvidence.Reserved,
                                         sizeof(snapshot.FirstWorkerEntryEvidence.Reserved)) ||
        !Ia64RuntimeReservedBytesAreZero(snapshot.ResumedWorkerEntryEvidence.Reserved,
                                         sizeof(snapshot.ResumedWorkerEntryEvidence.Reserved)) ||
        !Ia64RuntimeReservedBytesAreZero(snapshot.ServingIdleEvidence.Reserved,
                                         sizeof(snapshot.ServingIdleEvidence.Reserved)) ||
        (snapshot.IdleContext.State != Ia64KernelContextState::Prepared) ||
        (snapshot.WorkerContext.State != Ia64KernelContextState::Prepared) ||
        !Ia64RuntimeReservedBytesAreZero(snapshot.Layout.Reserved, sizeof(snapshot.Layout.Reserved)) ||
        !Ia64RuntimeReservedBytesAreZero(snapshot.IdleContext.Reserved, sizeof(snapshot.IdleContext.Reserved)) ||
        !Ia64RuntimeReservedBytesAreZero(snapshot.WorkerContext.Reserved, sizeof(snapshot.WorkerContext.Reserved)) ||
        !Ia64RuntimeReservedBytesAreZero(snapshot.Reserved, sizeof(snapshot.Reserved)) ||
        (observation.Evidence.RuntimeEntryValue != snapshot.RuntimeEntryEvidence.StackPointer) ||
        (observation.Evidence.FirstWorkerEntryValue != snapshot.FirstWorkerEntryEvidence.StackPointer) ||
        (observation.Evidence.ResumedWorkerEntryValue != snapshot.ResumedWorkerEntryEvidence.StackPointer) ||
        (observation.Evidence.ServingIdleValue != snapshot.ServingIdleEvidence.StackPointer) ||
        (observation.Evidence.RuntimeEntryValue == 0U) || (observation.Evidence.FirstWorkerEntryValue == 0U) ||
        (observation.Evidence.ResumedWorkerEntryValue == 0U) || (observation.Evidence.ServingIdleValue == 0U) ||
        !ValidateIa64ApplicationProcessorSchedulerState(processor, snapshot) ||
        !ValidateIa64ApplicationProcessorServiceState(snapshot))
    {
        return false;
    }
    RecordIa64ApplicationProcessorScheduler(processor, snapshot);
    RecordIa64ApplicationProcessorService(processor, serviceCookie, snapshot);
    return AcknowledgeIa64ApplicationProcessorLocalTimerProof(processor, snapshot);
}

[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool
RequestAndObserveIa64ApplicationProcessorTimerDelivery(const Kernel::Scheduler::CpuId processor,
                                                       const Foundation::UInt64 generation) noexcept
{
    using Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeAccessError;
    using Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeFailurePoll;
    using Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeProcessorState;
    using Platform::Ia64::Scheduler::Ia64ApplicationProcessorTimerDeliveryResult;
    using Platform::Ia64::Scheduler::Ia64ApplicationProcessorTimerDeliveryState;

    auto* const runtime = g_ia64ApplicationProcessorKernelRuntimeOwner;
    if (runtime == nullptr)
    {
        return false;
    }

    auto& snapshot = g_ia64ApplicationProcessorKernelRuntimeSnapshot;
    if ((runtime->GetProcessorSnapshot(processor, snapshot) !=
         Ia64ApplicationProcessorKernelRuntimeAccessError::None) ||
        (snapshot.Processor.GetValue() != processor.GetValue()) || (snapshot.Generation != generation) ||
        (snapshot.State != Ia64ApplicationProcessorKernelRuntimeProcessorState::IdleServing) ||
        !ValidateIa64ApplicationProcessorSchedulerState(processor, snapshot) ||
        !ValidateIa64ApplicationProcessorServiceState(snapshot) || (snapshot.TimerDelivery.RequestSequence != 0U) ||
        (snapshot.TimerDelivery.CompletionSequence != 0U) ||
        (snapshot.TimerDelivery.State != Ia64ApplicationProcessorTimerDeliveryState::Ready) ||
        (snapshot.TimerDelivery.Result != Ia64ApplicationProcessorTimerDeliveryResult::None) ||
        !Ia64RuntimeReservedBytesAreZero(snapshot.TimerDelivery.Reserved, sizeof(snapshot.TimerDelivery.Reserved)))
    {
        return false;
    }

    const auto request = runtime->RequestTimerDelivery(processor, generation);
    if (!request.HasValue() || (request.GetValue().Sequence == 0U) ||
        (request.GetValue().Processor.GetValue() != processor.GetValue()) ||
        !Ia64RuntimeReservedBytesAreZero(request.GetValue().Reserved, sizeof(request.GetValue().Reserved)))
    {
        return false;
    }
    const Foundation::UInt64 requestSequence = request.GetValue().Sequence;

    for (Platform::Ia64::Scheduler::Ia64CrossProcessorWaitBudget budget{g_ia64ApWaitClock.GetTicks(),
                                                                        Ia64ApWaitMinimumPolls, Ia64ApWaitMinimumTicks};
         budget.TryPoll(g_ia64ApWaitClock.GetTicks());)
    {
        // The BSP also services its mailbox while waiting.
        Platform::Synchronization::ProcessorWaitProgress();
        Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeFailure failure{};
        const auto failurePoll = runtime->PollFirstFailure(failure);
        if (failurePoll == Ia64ApplicationProcessorKernelRuntimeFailurePoll::Available)
        {
            return false;
        }
        if (failurePoll == Ia64ApplicationProcessorKernelRuntimeFailurePoll::PublicationPending)
        {
#if defined(__ia64__)
            __asm__ volatile("hint @pause" ::: "memory");
#endif
            continue;
        }
        if (failurePoll != Ia64ApplicationProcessorKernelRuntimeFailurePoll::None)
        {
            return false;
        }

        const auto access = runtime->GetProcessorSnapshot(processor, snapshot);
        if (access == Ia64ApplicationProcessorKernelRuntimeAccessError::PublicationPending)
        {
#if defined(__ia64__)
            __asm__ volatile("hint @pause" ::: "memory");
#endif
            continue;
        }
        if ((access != Ia64ApplicationProcessorKernelRuntimeAccessError::None) ||
            (snapshot.Processor.GetValue() != processor.GetValue()) || (snapshot.Generation != generation) ||
            (snapshot.State != Ia64ApplicationProcessorKernelRuntimeProcessorState::IdleServing) ||
            !ValidateIa64ApplicationProcessorSchedulerState(processor, snapshot) ||
            !ValidateIa64ApplicationProcessorServiceState(snapshot) ||
            (snapshot.TimerDelivery.RequestSequence != requestSequence) ||
            !Ia64RuntimeReservedBytesAreZero(snapshot.TimerDelivery.Reserved, sizeof(snapshot.TimerDelivery.Reserved)))
        {
            return false;
        }

        if ((snapshot.TimerDelivery.State == Ia64ApplicationProcessorTimerDeliveryState::Pending) ||
            (snapshot.TimerDelivery.State == Ia64ApplicationProcessorTimerDeliveryState::Executing))
        {
            if ((snapshot.TimerDelivery.CompletionSequence != 0U) ||
                (snapshot.TimerDelivery.Result != Ia64ApplicationProcessorTimerDeliveryResult::None))
            {
                return false;
            }
#if defined(__ia64__)
            __asm__ volatile("hint @pause" ::: "memory");
#endif
            continue;
        }

        if (snapshot.TimerDelivery.State != Ia64ApplicationProcessorTimerDeliveryState::Completed)
        {
            return false;
        }
        if (!ValidateIa64ApplicationProcessorTimerDeliveryCompletion(processor, generation, requestSequence, snapshot))
        {
            return false;
        }
        RecordIa64ApplicationProcessorTimerDelivery(processor, snapshot);
        return AcknowledgeIa64ApplicationProcessorTimerDelivery(processor, snapshot.TimerDeliveryRecord);
    }
    Kernel::Scheduler::CrossProcessorWait::Timeout(Kernel::Scheduler::ProcessorSet::Bit(processor));
    return false;
}

/// Releases one AP, requests one persistent-scheduler service and one
/// deferred timer delivery, and records everything the AP reports. A
/// violated contract, an AP failure record or an AP that never serves ends
/// the scenario (the caller reports AP-RUNTIME:FAIL).
[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool
ReleaseAndObserveIa64ApplicationProcessorRuntime(const Kernel::Scheduler::CpuId processor,
                                                 const Foundation::UInt64 generation) noexcept
{
    using Platform::Ia64::Interrupt::Ia64ApplicationProcessorRuntimeTransitionError;
    using Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeError;
    using Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeFailurePoll;

    auto* const runtime = g_ia64ApplicationProcessorKernelRuntimeOwner;
    if (runtime == nullptr)
    {
        return false;
    }
    const auto release = g_ia64ApplicationProcessorRuntimeReleaseCoordinator.Release(processor, generation);
    if (!release.HasValue() || (release.GetValue().Generation != generation) ||
        (release.GetValue().Processor.GetValue() != processor.GetValue()) ||
        !Ia64RuntimeReservedBytesAreZero(release.GetValue().Reserved, sizeof(release.GetValue().Reserved)))
    {
        return false;
    }

    const Foundation::UInt64 serviceCookie =
        Ia64ApPortableSchedulerServiceCookieBase | (generation << 8U) | processor.GetValue();
    Foundation::UInt64 serviceSequence{0U};

    for (Platform::Ia64::Scheduler::Ia64CrossProcessorWaitBudget budget{g_ia64ApWaitClock.GetTicks(),
                                                                        Ia64ApWaitMinimumPolls, Ia64ApWaitMinimumTicks};
         budget.TryPoll(g_ia64ApWaitClock.GetTicks());)
    {
        // The BSP also services its mailbox while waiting.
        Platform::Synchronization::ProcessorWaitProgress();
        Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeFailure failure{};
        const Ia64ApplicationProcessorKernelRuntimeFailurePoll failurePoll = runtime->PollFirstFailure(failure);
        if (failurePoll == Ia64ApplicationProcessorKernelRuntimeFailurePoll::Available)
        {
            return false;
        }
        if (failurePoll != Ia64ApplicationProcessorKernelRuntimeFailurePoll::None)
        {
            continue;
        }

        if (serviceSequence == 0U)
        {
            const Foundation::UInt8 processorValue = processor.GetValue();
            const auto request = runtime->RequestPortableSchedulerService(processor, generation, serviceCookie);
            if (request.HasValue())
            {
                if ((request.GetValue().Sequence == 0U) || (request.GetValue().Cookie != serviceCookie) ||
                    (request.GetValue().Processor.GetValue() != processorValue) ||
                    !Ia64RuntimeReservedBytesAreZero(request.GetValue().Reserved, sizeof(request.GetValue().Reserved)))
                {
                    return false;
                }
                serviceSequence = request.GetValue().Sequence;
            }
            else if (request.GetError() !=
                     Ia64ApplicationProcessorKernelRuntimeError::PortableSchedulerServiceUnavailable)
            {
                return false;
            }
        }

        if (serviceSequence == 0U)
        {
#if defined(__ia64__)
            __asm__ volatile("hint @pause" ::: "memory");
#endif
            continue;
        }

        const auto serving = g_ia64ApplicationProcessorRuntimeReleaseCoordinator.ObserveServing(processor, generation);
        if (serving.HasValue())
        {
            if (!ValidateIa64ApplicationProcessorRuntimeServing(processor, generation, serviceCookie, serviceSequence,
                                                                serving.GetValue()))
            {
                return false;
            }
            if (!RequestAndObserveIa64ApplicationProcessorTimerDelivery(processor, generation))
            {
                return false;
            }
            Platform::Ia64::Diagnostics::Ia64ApplicationProcessorEvidence& evidence =
                GetIa64ApplicationProcessorEvidence(processor);
            evidence.Processor = processor.GetValue();
            evidence.Generation = generation;
            Platform::Ia64::Diagnostics::PublishIa64Evidence(evidence.Published);
            return true;
        }

        const Ia64ApplicationProcessorRuntimeTransitionError error = serving.GetError();
        if ((error != Ia64ApplicationProcessorRuntimeTransitionError::ServingPending) &&
            (error != Ia64ApplicationProcessorRuntimeTransitionError::CoordinatorNotReady))
        {
            return false;
        }
#if defined(__ia64__)
        __asm__ volatile("hint @pause" ::: "memory");
#endif
    }
    Kernel::Scheduler::CrossProcessorWait::Timeout(Kernel::Scheduler::ProcessorSet::Bit(processor));
    return false;
}

[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool RunIa64ApplicationProcessorKernelRuntimes() noexcept
{
    using Platform::Ia64::Interrupt::Ia64ApplicationProcessorRuntimeReleaseCoordinatorState;
    using Platform::Ia64::Interrupt::Ia64ApplicationProcessorRuntimeStage;
    using Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeAccessError;
    using Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeFailurePoll;
    using Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeState;

    auto* const runtime = g_ia64ApplicationProcessorKernelRuntimeOwner;
    if (runtime == nullptr)
    {
        return false;
    }
    Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeSummary summary{};
    if ((runtime->GetSummary(summary) != Ia64ApplicationProcessorKernelRuntimeAccessError::None) ||
        (summary.State != Ia64ApplicationProcessorKernelRuntimeState::Ready) || (summary.ProcessorCount == 0U) ||
        (summary.ProcessorCount > Kernel::Scheduler::MaximumProcessorCount) ||
        (summary.ProcessorCount != g_ia64ApplicationProcessorExecutionStorageOwner.GetProcessorCount()) ||
        (summary.EnabledProcessorMask != g_ia64RuntimeProfiledProcessorMask) ||
        (summary.PreparedContextCount != static_cast<Foundation::UInt8>(summary.ProcessorCount - 1U)) ||
        (summary.CommonPhysicalAddressBits != g_ia64CommonRuntimeProcessorCapabilities.PhysicalAddressBits) ||
        (summary.CommonImplementedVirtualAddressMostSignificantBit !=
         g_ia64CommonRuntimeProcessorCapabilities.ImplementedVirtualAddressMostSignificantBit))
    {
        return false;
    }

    for (Foundation::UInt8 processorValue{1U}; processorValue < summary.ProcessorCount; ++processorValue)
    {
        if (!ReleaseAndObserveIa64ApplicationProcessorRuntime(Kernel::Scheduler::CpuId{processorValue}, processorValue))
        {
            return false;
        }
    }

    Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeFailure failure{};
    if (runtime->PollFirstFailure(failure) != Ia64ApplicationProcessorKernelRuntimeFailurePoll::None)
    {
        return false;
    }

    const auto release = g_ia64ApplicationProcessorRuntimeReleaseCoordinator.GetSnapshot();
    const Foundation::UInt8 applicationProcessorMask =
        static_cast<Foundation::UInt8>(summary.EnabledProcessorMask & Foundation::UInt8{0xFEU});
    if ((release.State != Ia64ApplicationProcessorRuntimeReleaseCoordinatorState::AllServing) ||
        (release.ProcessorCount != summary.ProcessorCount) ||
        (release.EnabledProcessorMask != summary.EnabledProcessorMask) ||
        (release.ReleasedProcessorMask != applicationProcessorMask) ||
        (release.OnlineProcessorMask != applicationProcessorMask) ||
        (release.ObservedProcessorMask != applicationProcessorMask) || (release.ActiveGeneration != 0U) ||
        release.ActiveProcessor.IsValid())
    {
        return false;
    }
    for (Foundation::UInt8 processorValue{1U}; processorValue < summary.ProcessorCount; ++processorValue)
    {
        if ((release.Processors[processorValue].Stage != Ia64ApplicationProcessorRuntimeStage::IdleServing) ||
            (release.Processors[processorValue].Generation != processorValue) ||
            !release.Processors[processorValue].ServingObserved)
        {
            return false;
        }
    }

    Platform::Ia64::Diagnostics::Ia64ApplicationProcessorRuntimeEvidence& evidence =
        Platform::Ia64::Diagnostics::GetIa64EvidenceRecord().ApplicationProcessors;
    evidence.ProcessorCount = summary.ProcessorCount;
    evidence.ServingProcessorMask = release.OnlineProcessorMask;
    g_ia64TlbShootdownMailbox->BindProcessorSet(Platform::Synchronization::RuntimeProcessorSet());
    Platform::Ia64::Diagnostics::PublishIa64Evidence(evidence.Published);
    Platform::Firmware::WriteDiagnosticAscii("ASTERIA:STAGE:AP-RUNTIME\r\n");
    return true;
}

/// Maps the shared-root shootdown page once every profiled processor serves
/// and waits for the shootdown protocol to complete. The kernel checks its
/// own request (the page, the target processors) and the transport's state;
/// which processors acknowledged is recorded.
[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool
MapIa64SharedRootShootdownPage(Platform::Ia64::Scheduler::Ia64BspKernelRuntime& runtime) noexcept
{
    using Platform::Ia64::Memory::Ia64PolledTlbShootdownNotifierState;
    using Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeFailurePoll;
    using Platform::Ia64::Scheduler::Ia64BspSharedRootShootdownState;

    auto* const applicationProcessorRuntime = g_ia64ApplicationProcessorKernelRuntimeOwner;
    auto* const mailbox = g_ia64TlbShootdownMailbox;
    if ((applicationProcessorRuntime == nullptr) || (mailbox == nullptr) ||
        (g_ia64SharedRootShootdownPage.PhysicalStart == 0U))
    {
        return false;
    }
    const auto mapped =
        runtime.MapSharedRootShootdownPage(g_ia64SharedRootShootdownPage, g_ia64RuntimeProfiledProcessorMask, *mailbox);
    if (!mapped.HasValue())
    {
        return false;
    }

    Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeFailure apFailure{};
    const auto notifier = g_ia64TlbShootdownNotifier.GetSnapshot();
    const auto& summary = mapped.GetValue();
    const Foundation::UInt8 queuedProcessorMask =
        static_cast<Foundation::UInt8>(g_ia64RuntimeProfiledProcessorMask & Foundation::UInt8{0xFEU});
    if ((runtime.GetSharedRootShootdownState() != Ia64BspSharedRootShootdownState::Complete) ||
        (summary.State != Ia64BspSharedRootShootdownState::Complete) || !summary.AddressSpaceIdentity.IsValid() ||
        !summary.FrameIdentity.IsValid() || !summary.MemoryObjectIdentity.IsValid() ||
        (summary.Mapping.VirtualStart.GetValue() !=
         Platform::Ia64::Scheduler::Ia64BspSharedRootShootdownVirtualStart) ||
        (summary.Mapping.Pages.GetValue() != 1U) || (summary.Mapping.MappingSequence != summary.ShootdownSequence) ||
        (summary.ShootdownSequence == 0U) || (summary.TargetProcessorMask != g_ia64RuntimeProfiledProcessorMask) ||
        (summary.QueuedProcessorMask != queuedProcessorMask) ||
        !Ia64RuntimeReservedBytesAreZero(summary.Reserved, sizeof(summary.Reserved)) ||
        (mailbox->GetOnlineProcessorMask() != g_ia64RuntimeProfiledProcessorMask) ||
        (notifier.State != Ia64PolledTlbShootdownNotifierState::Ready) ||
        (notifier.ProfiledProcessorMask != g_ia64RuntimeProfiledProcessorMask) ||
        (notifier.PendingProcessorMask != 0U) || (notifier.Reserved != 0U) ||
        (applicationProcessorRuntime->PollFirstFailure(apFailure) !=
         Ia64ApplicationProcessorKernelRuntimeFailurePoll::None))
    {
        return false;
    }
    Platform::Ia64::Diagnostics::Ia64SharedRootShootdownEvidence& evidence =
        Platform::Ia64::Diagnostics::GetIa64EvidenceRecord().SharedRootShootdown;
    evidence.VirtualStart = summary.Mapping.VirtualStart.GetValue();
    evidence.Pages = summary.Mapping.Pages.GetValue();
    evidence.PhysicalStart = g_ia64SharedRootShootdownPage.PhysicalStart;
    evidence.ShootdownSequence = summary.ShootdownSequence;
    evidence.MappingSequence = summary.Mapping.MappingSequence;
    evidence.TargetProcessorMask = summary.TargetProcessorMask;
    evidence.QueuedProcessorMask = summary.QueuedProcessorMask;
    evidence.AcknowledgedProcessorMask = summary.AcknowledgedProcessorMask;
    Platform::Ia64::Diagnostics::PublishIa64Evidence(evidence.Published);
    Platform::Firmware::WriteDiagnosticAscii("ASTERIA:STAGE:IA64-SHARED-ROOT-SHOOTDOWN\r\n");
    return true;
}

[[nodiscard]] bool AllocateIa64RuntimeMemoryReservation(const Foundation::UInt64 insertableMask) noexcept
{
    // The page holds the owner and the slabs; the bitmaps take what a larger page leaves.
    const Platform::Ia64::Memory::RuntimeMemoryReservationPageShifts shifts =
        Platform::Ia64::Memory::SelectRuntimeMemoryReservationPageShifts(
            insertableMask, Platform::Ia64::Memory::Ia64RuntimeRequiredBytes);
    const Foundation::UInt8 candidates[2U]{shifts.Preferred, shifts.Fallback};
    for (const Foundation::UInt8 shift : candidates)
    {
        if (shift == 0U)
        {
            continue;
        }
        const Foundation::UInt64 pages = Foundation::UInt64{1U} << (shift - 12U);
        const auto reservation = g_bootstrapPageAllocator.Allocate((2U * pages) + 1U, pages);
        if (reservation.HasValue() && (reservation.GetValue().PageCount == (2U * pages) + 1U))
        {
            // Reserve alignment slack below the page and one page above it.
            // The adjacent pages stay unmapped in every root and cannot be
            // handed to any later allocation: these are the page-end guards.
            g_ia64RuntimeMemoryReservation = reservation.GetValue();
            g_ia64RuntimeMemoryReservation.PhysicalStart += pages * Kernel::Memory::PhysicalPageSizeBytes;
            g_ia64RuntimeMemoryReservation.PageCount = pages;
            return true;
        }
    }
    return false;
}

/// Checks the sealed plan's translation registers against this processor's
/// PAL counts and the mandatory budget, and reports both counts.
[[nodiscard]] bool VerifyIa64TranslationRegisterBudget() noexcept
{
    const auto plan = g_ia64InitialAddressSpacePreparation.TryGetTranslationResourcePlan();
    if (!plan.HasValue() || (plan.GetValue() == nullptr))
    {
        return false;
    }
    const Platform::Ia64::Memory::PalVirtualMemoryCapabilities& capabilities =
        g_ia64PalBootstrapCapabilities.VirtualMemory;
    const Foundation::UInt16 data = plan.GetValue()->GetDataRegisterCount();
    const Foundation::UInt16 instruction = plan.GetValue()->GetInstructionRegisterCount();
    const Platform::Ia64::Memory::Ia64TranslationRegisterBudgetCheck check =
        Platform::Ia64::Memory::CheckIa64TranslationRegisterBudget(capabilities.DataTranslationRegisterCount,
                                                                   capabilities.InstructionTranslationRegisterCount,
                                                                   data, instruction);
    Platform::Firmware::WriteDiagnosticAscii("ASTERIA:IA64-TRANSLATION-REGISTERS:DTR=");
    WriteIa64DiagnosticHex(data, 2U);
    Platform::Firmware::WriteDiagnosticAscii("/");
    WriteIa64DiagnosticHex(capabilities.DataTranslationRegisterCount, 2U);
    Platform::Firmware::WriteDiagnosticAscii(":ITR=");
    WriteIa64DiagnosticHex(instruction, 2U);
    Platform::Firmware::WriteDiagnosticAscii("/");
    WriteIa64DiagnosticHex(capabilities.InstructionTranslationRegisterCount, 2U);
    Platform::Firmware::WriteDiagnosticAscii("\r\n");
    return check.Error == Platform::Ia64::Memory::Ia64TranslationRegisterBudgetError::None;
}

[[nodiscard]] bool BuildIa64InitialTranslationPlan(const Kernel::Memory::PhysicalPageRange& reservedPages,
                                                   const Kernel::Memory::PhysicalPageRange& runtimeMemoryPages) noexcept
{
    g_ia64InitialMemoryStackView = Platform::Ia64::Memory::WritableInitialMemoryStackView{
        .Data = reinterpret_cast<Foundation::UInt8*>(static_cast<Foundation::UIntPointer>(reservedPages.PhysicalStart)),
        .SizeBytes = Platform::Ia64::Memory::InitialMemoryStackSizeBytes,
        .PhysicalStart = reservedPages.PhysicalStart,
    };
    const Foundation::UInt64 interruptVectorTablePhysicalStart =
        Platform::Ia64::Memory::GetStaticBootstrapInterruptVectorTableAddress();
    const Foundation::UInt64 rseBackingStorePhysicalStart =
        runtimeMemoryPages.PhysicalStart + (runtimeMemoryPages.PageCount * Kernel::Memory::MappingPageSizeBytes) -
        Platform::Ia64::Memory::InitialRseBackingStoreSizeBytes;
    g_ia64InterruptVectorTableView = Platform::Ia64::Memory::WritableInterruptVectorTableView{
        .Data = reinterpret_cast<Foundation::UInt8*>(
            static_cast<Foundation::UIntPointer>(interruptVectorTablePhysicalStart)),
        .SizeBytes = Platform::Ia64::Memory::InitialInterruptVectorTableSizeBytes,
        .PhysicalStart = interruptVectorTablePhysicalStart,
    };
    g_ia64RseBackingStoreView = Platform::Ia64::Memory::WritableRseBackingStoreView{
        .Data =
            reinterpret_cast<Foundation::UInt8*>(static_cast<Foundation::UIntPointer>(rseBackingStorePhysicalStart)),
        .SizeBytes = Platform::Ia64::Memory::InitialRseBackingStoreSizeBytes,
        .PhysicalStart = rseBackingStorePhysicalStart,
    };
    return g_ia64InitialAddressSpacePreparation
        .Prepare(g_ia64LoadedImageLayout, g_bootInfo, reservedPages, runtimeMemoryPages,
                 g_ia64PalBootstrapCapabilities.VirtualMemory, g_ia64SalSystemTableInformation,
                 g_ia64InitialTranslationRegions, g_ia64InterruptVectorTableView, g_ia64RseBackingStoreView)
        .HasValue();
}

[[nodiscard]] ASTERIA_NOINLINE_NOIPA Platform::Ia64::Memory::InitialExecutionContextSwitchFailure
SwitchToIa64InitialExecutionContext() noexcept
{
    return Platform::Ia64::Memory::InitialExecutionContextSwitcher::PrepareAndSwitch(
        g_ia64InitialAddressSpacePreparation, g_ia64InitialMemoryStackView, g_ia64RseBackingStoreView);
}

void WriteIa64InitialExecutionContextSwitchError(
    const Platform::Ia64::Memory::InitialExecutionContextSwitchFailure& failure) noexcept
{
    using Platform::Ia64::Memory::InitialExecutionContextSwitchError;
    switch (failure.Error)
    {
    case InitialExecutionContextSwitchError::AddressSpaceNotPrepared:
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:INITIAL-EXECUTION-CONTEXT-ERROR:PLAN-NOT-READY\r\n");
        return;
    case InitialExecutionContextSwitchError::MemoryStackRejected:
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:INITIAL-EXECUTION-CONTEXT-ERROR:STACK-REJECTED\r\n");
        return;
    case InitialExecutionContextSwitchError::RseBackingStoreRejected:
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:INITIAL-EXECUTION-CONTEXT-ERROR:RSE-REJECTED\r\n");
        return;
    case InitialExecutionContextSwitchError::StorageRangesOverlap:
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:INITIAL-EXECUTION-CONTEXT-ERROR:STORAGE-OVERLAP\r\n");
        return;
    case InitialExecutionContextSwitchError::RseBackingStoreNotZero:
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:INITIAL-EXECUTION-CONTEXT-ERROR:RSE-NOT-ZERO\r\n");
        return;
    case InitialExecutionContextSwitchError::MemoryStackNotZero:
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:INITIAL-EXECUTION-CONTEXT-ERROR:STACK-NOT-ZERO\r\n");
        return;
    case InitialExecutionContextSwitchError::UnsupportedProcessorState:
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:INITIAL-EXECUTION-CONTEXT-ERROR:PROCESSOR-STATE\r\n");
        return;
    case InitialExecutionContextSwitchError::UnexpectedAdapterReturn:
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:INITIAL-EXECUTION-CONTEXT-ERROR:ADAPTER-RETURNED\r\n");
        return;
    case InitialExecutionContextSwitchError::None:
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:INITIAL-EXECUTION-CONTEXT-ERROR:INTERNAL\r\n");
        return;
    }
}

[[nodiscard]] ASTERIA_NOINLINE_NOIPA Platform::Ia64::Memory::InitialInterruptVectorTableInstallError
InstallIa64InitialInterruptVectorTable() noexcept
{
    const auto result = Platform::Ia64::Memory::InitialInterruptVectorTableInstaller::Install(
        g_ia64InitialAddressSpacePreparation, g_ia64InterruptVectorTableView);
    if (!result.HasValue())
    {
        return result.GetError().Error;
    }
    g_ia64InitialInterruptVectorTableInstallSummary = result.GetValue();
    return Platform::Ia64::Memory::InitialInterruptVectorTableInstallError::None;
}

void WriteIa64InitialInterruptVectorTableInstallError(
    const Platform::Ia64::Memory::InitialInterruptVectorTableInstallError error) noexcept
{
    using Platform::Ia64::Memory::InitialInterruptVectorTableInstallError;
    switch (error)
    {
    case InitialInterruptVectorTableInstallError::PreparationNotReady:
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:INITIAL-IVT-INSTALL-ERROR:PREPARATION-NOT-READY\r\n");
        return;
    case InitialInterruptVectorTableInstallError::PreparedTableRejected:
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:INITIAL-IVT-INSTALL-ERROR:TABLE-REJECTED\r\n");
        return;
    case InitialInterruptVectorTableInstallError::InvalidTableContents:
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:INITIAL-IVT-INSTALL-ERROR:INVALID-CONTENTS\r\n");
        return;
    case InitialInterruptVectorTableInstallError::UnsupportedProcessorState:
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:INITIAL-IVT-INSTALL-ERROR:PROCESSOR-STATE\r\n");
        return;
    case InitialInterruptVectorTableInstallError::None:
        return;
    }
}

[[nodiscard]] ASTERIA_NOINLINE_NOIPA Platform::Ia64::Memory::InitialTranslationInstallError
InstallIa64InitialTranslations(const Platform::Ia64::Firmware::PalBootstrapCapabilities& capabilities) noexcept
{
    const auto plan = g_ia64InitialAddressSpacePreparation.TryGetTranslationResourcePlan();
    if (!plan.HasValue())
    {
        return Platform::Ia64::Memory::InitialTranslationInstallError::ResourcePlanNotBuilt;
    }
    const auto budget = Platform::Ia64::Memory::CheckIa64TranslationRegisterBudget(
        capabilities.VirtualMemory.DataTranslationRegisterCount,
        capabilities.VirtualMemory.InstructionTranslationRegisterCount, plan.GetValue()->GetDataRegisterCount(),
        plan.GetValue()->GetInstructionRegisterCount());
    if (budget.Error != Platform::Ia64::Memory::Ia64TranslationRegisterBudgetError::None)
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:IA64-TRANSLATION-REGISTER-BUDGET:FAIL\r\n");
        return Platform::Ia64::Memory::InitialTranslationInstallError::InvalidCapabilities;
    }
    const auto result = Platform::Ia64::Memory::InitialTranslationInstaller::Install(
        *plan.GetValue(), capabilities.VirtualMemory, capabilities.TranslationCachePurge);
    return result.HasValue() ? Platform::Ia64::Memory::InitialTranslationInstallError::None : result.GetError().Error;
}

void WriteIa64InitialTranslationInstallError(
    const Platform::Ia64::Memory::InitialTranslationInstallError error) noexcept
{
    using Platform::Ia64::Memory::InitialTranslationInstallError;
    switch (error)
    {
    case InitialTranslationInstallError::InvalidPurgePlan:
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:INITIAL-TRANSLATION-INSTALL-ERROR:INVALID-PURGE-PLAN\r\n");
        return;
    case InitialTranslationInstallError::ResourcePlanNotBuilt:
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:INITIAL-TRANSLATION-INSTALL-ERROR:PLAN-NOT-BUILT\r\n");
        return;
    case InitialTranslationInstallError::InvalidCapabilities:
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:INITIAL-TRANSLATION-INSTALL-ERROR:INVALID-CAPABILITIES\r\n");
        return;
    case InitialTranslationInstallError::InvalidRegionConfiguration:
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:INITIAL-TRANSLATION-INSTALL-ERROR:INVALID-REGIONS\r\n");
        return;
    case InitialTranslationInstallError::InvalidFirmwareTranslation:
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:INITIAL-TRANSLATION-INSTALL-ERROR:INVALID-FIRMWARE-ITR\r\n");
        return;
    case InitialTranslationInstallError::InvalidResourceEntry:
        Platform::Firmware::WriteDiagnosticAscii(
            "ASTERIA:INITIAL-TRANSLATION-INSTALL-ERROR:INVALID-RESOURCE-ENTRY\r\n");
        return;
    case InitialTranslationInstallError::OverlappingResourceEntries:
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:INITIAL-TRANSLATION-INSTALL-ERROR:OVERLAPPING-ENTRIES\r\n");
        return;
    case InitialTranslationInstallError::UnsupportedProcessorState:
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:INITIAL-TRANSLATION-INSTALL-ERROR:PROCESSOR-STATE\r\n");
        return;
    case InitialTranslationInstallError::None:
        return;
    }
}

/// Records the processors of the retained runtime profile (the BSP and every
/// AP that completed the rendezvous).
void RecordIa64ProcessorEvidence(const Foundation::UInt8 processorCount) noexcept
{
    Platform::Ia64::Diagnostics::Ia64ProcessorEvidence& evidence =
        Platform::Ia64::Diagnostics::GetIa64EvidenceRecord().Processors;
    evidence.ProcessorCount = processorCount;
    evidence.EnabledProcessorMask = g_ia64RuntimeProfiledProcessorMask;
    Platform::Ia64::Diagnostics::PublishIa64Evidence(evidence.Published);
}

[[nodiscard]] constexpr bool
TakeoverFailureIsClear(const Platform::Ia64::Interrupt::Ia64ApplicationProcessorTakeoverFailure& failure) noexcept
{
    return (static_cast<Foundation::UInt32>(failure.Phase) == 0U) &&
           (static_cast<Foundation::UInt32>(failure.InterruptVectorTableInstallationStatus) == 0U) &&
           (static_cast<Foundation::UInt32>(failure.PalCapabilityCapture.Phase) == 0U) &&
           (static_cast<Foundation::UInt32>(failure.PalCapabilityCapture.AdapterError) == 0U) &&
           (static_cast<Foundation::UInt32>(failure.PalCapabilityCapture.VirtualMemoryError) == 0U) &&
           (static_cast<Foundation::UInt32>(failure.PalCapabilityCapture.TranslationCachePurgeError) == 0U) &&
           (static_cast<Foundation::UInt32>(failure.InitialTranslationInstallationError) == 0U);
}

[[nodiscard]] constexpr bool TakeoverReservedBytesAreZero(const Foundation::UInt8 (&reserved)[7U]) noexcept
{
    for (Foundation::Size index{0U}; index < 7U; ++index)
    {
        if (reserved[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool ValidateAndRetainIa64RuntimeProcessorProfile(
    const Foundation::UInt8 profiledProcessorMask,
    const Platform::Ia64::Memory::PalVirtualMemoryCapabilities (
        &processorCapabilities)[Kernel::Scheduler::MaximumProcessorCount]) noexcept
{
    Platform::Ia64::Memory::Ia64RuntimeProcessorCapabilities validation;
    const auto validationError = validation.Initialize(profiledProcessorMask, processorCapabilities);
    const auto retainedMask = validation.GetAdmittedProcessorMask();
    const auto commonCapabilities = validation.GetCommonCapabilities();
    if ((validationError != Platform::Ia64::Memory::Ia64RuntimeProcessorCapabilitiesError::None) ||
        !retainedMask.HasValue() || (retainedMask.GetValue() != profiledProcessorMask) ||
        !commonCapabilities.HasValue())
    {
        return false;
    }

    for (Foundation::Size processor{0U}; processor < Kernel::Scheduler::MaximumProcessorCount; ++processor)
    {
        g_ia64RuntimeProcessorCapabilities[processor] = processorCapabilities[processor];
    }
    g_ia64CommonRuntimeProcessorCapabilities = commonCapabilities.GetValue();
    g_ia64RuntimeProfiledProcessorMask = profiledProcessorMask;
    return true;
}

[[nodiscard]] bool CaptureIa64RuntimeProcessorProfile(
    const Platform::Ia64::Interrupt::Ia64ApplicationProcessorTakeoverProgressSnapshot& progress,
    const Platform::Ia64::Interrupt::Ia64ApplicationProcessorTakeoverCoordinator& takeover) noexcept
{
    using Platform::Ia64::Interrupt::Ia64ApplicationProcessorTakeoverCoordinatorState;
    using Platform::Ia64::Interrupt::Ia64ApplicationProcessorTakeoverLifecycle;

    constexpr Foundation::UInt8 bspProcessorMask{0x01U};
    const Foundation::UInt8 applicationProcessorMask = static_cast<Foundation::UInt8>(
        progress.EnabledProcessorMask & static_cast<Foundation::UInt8>(~bspProcessorMask));
    if ((progress.State != Ia64ApplicationProcessorTakeoverCoordinatorState::Ready) ||
        ((progress.EnabledProcessorMask & bspProcessorMask) == 0U) ||
        (progress.TerminalMask != applicationProcessorMask) || (progress.FailedMask != 0U))
    {
        return false;
    }

    Platform::Ia64::Memory::PalVirtualMemoryCapabilities
        processorCapabilities[Kernel::Scheduler::MaximumProcessorCount]{};
    processorCapabilities[0U] = g_ia64PalBootstrapCapabilities.VirtualMemory;
    Foundation::UInt8 observedProcessorMask{bspProcessorMask};
    for (Foundation::UInt8 processor{1U}; processor < Kernel::Scheduler::MaximumProcessorCount; ++processor)
    {
        const Foundation::UInt8 processorMask = static_cast<Foundation::UInt8>(Foundation::UInt8{1U} << processor);
        if ((progress.EnabledProcessorMask & processorMask) == 0U)
        {
            continue;
        }

        const auto acquired = takeover.GetProcessorSnapshot(Kernel::Scheduler::CpuId{processor});
        if (!acquired.HasValue())
        {
            return false;
        }
        const auto& snapshot = acquired.GetValue();
        if ((snapshot.Lifecycle != Ia64ApplicationProcessorTakeoverLifecycle::InitialTranslationsInstalled) ||
            (snapshot.Takeover.ProcessorValue != processor) || !TakeoverFailureIsClear(snapshot.Failure) ||
            !TakeoverReservedBytesAreZero(snapshot.Reserved))
        {
            return false;
        }
        processorCapabilities[processor] = snapshot.PalCapabilities.VirtualMemory;
        observedProcessorMask = static_cast<Foundation::UInt8>(observedProcessorMask | processorMask);
    }
    if (observedProcessorMask != progress.EnabledProcessorMask)
    {
        return false;
    }

    return ValidateAndRetainIa64RuntimeProcessorProfile(observedProcessorMask, processorCapabilities);
}

[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool PrepareIa64SingleProcessorRuntimeProfile() noexcept
{
    Platform::Ia64::Memory::PalVirtualMemoryCapabilities
        processorCapabilities[Kernel::Scheduler::MaximumProcessorCount]{};
    processorCapabilities[0U] = g_ia64PalBootstrapCapabilities.VirtualMemory;
    if (!ValidateAndRetainIa64RuntimeProcessorProfile(0x01U, processorCapabilities))
    {
        return false;
    }
    RecordIa64ProcessorEvidence(1U);
    return true;
}

[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool WaitForIa64ApplicationProcessorTakeover(
    const Platform::Ia64::Interrupt::Ia64LocalSapicIpiControllerSnapshot& controllerSnapshot,
    const Foundation::UInt8 targetProcessorMask,
    const Platform::Ia64::Interrupt::Ia64ApplicationProcessorTakeoverCoordinator& takeover) noexcept
{
    using Platform::Ia64::Interrupt::Ia64ApplicationProcessorTakeoverCoordinatorState;

    for (Platform::Ia64::Scheduler::Ia64CrossProcessorWaitBudget budget{g_ia64ApWaitClock.GetTicks(),
                                                                        Ia64ApWaitMinimumPolls, Ia64ApWaitMinimumTicks};
         budget.TryPoll(g_ia64ApWaitClock.GetTicks());)
    {
        // The BSP also services its mailbox while waiting.
        Platform::Synchronization::ProcessorWaitProgress();
        const auto progress = takeover.GetProgressSnapshot();
        if ((progress.State != Ia64ApplicationProcessorTakeoverCoordinatorState::Ready) ||
            (progress.ProcessorCount != controllerSnapshot.ProcessorCount) ||
            (progress.EnabledProcessorMask != controllerSnapshot.EnabledProcessorMask))
        {
            Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AP-RENDEZVOUS-ERROR:TAKEOVER-STATE\r\n");
            return false;
        }
        if (progress.FailedMask != 0U)
        {
            Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AP-RENDEZVOUS-ERROR:AP-INITIALIZATION\r\n");
            return false;
        }
        if (progress.TerminalMask == targetProcessorMask)
        {
            if (!CaptureIa64RuntimeProcessorProfile(progress, takeover))
            {
                Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AP-RENDEZVOUS-ERROR:CAPABILITY-PROFILE\r\n");
                return false;
            }
            RecordIa64ProcessorEvidence(progress.ProcessorCount);
            return true;
        }
    }
    Kernel::Scheduler::CrossProcessorWait::Timeout(
        Foundation::ProcessorMask{targetProcessorMask} &
        ~Foundation::ProcessorMask{takeover.GetProgressSnapshot().TerminalMask});
    Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AP-RENDEZVOUS-ERROR:INITIALIZATION-TIMEOUT\r\n");
    return false;
}

[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool BootstrapIa64ApplicationProcessors() noexcept
{
    using Platform::Ia64::Firmware::SalBootRendezvousRegistration;
    using Platform::Ia64::Firmware::SalBootRendezvousRegistrationDisposition;
    using Platform::Ia64::Firmware::SalBootRendezvousRegistrationStatus;
    using Platform::Ia64::Interrupt::Ia64LocalSapicIpiControllerInitializationError;
    using Platform::Ia64::Interrupt::Ia64LocalSapicIpiSendError;
    using Platform::Ia64::Interrupt::Ia64ProcessorInterruptBlockAccess;
    using Platform::Ia64::Interrupt::Ia64ProcessorInterruptBlockBaseKind;
    using Platform::Ia64::Interrupt::Ia64ApplicationProcessorTakeoverInitializationError;
    using Platform::Ia64::Interrupt::Ia64SalBootRendezvousArmError;
    using Platform::Ia64::Interrupt::Ia64SalBootRendezvousHandler;
    using Platform::Ia64::Interrupt::Ia64SalBootRendezvousInitializationError;

    constexpr Foundation::UInt64 PhysicalUncachedAliasBit{Foundation::UInt64{1U} << 63U};
    const Foundation::UInt64 bspCrLid = Ia64SalBootRendezvousHandler::ReadCurrentCrLid();
    const Ia64ProcessorInterruptBlockAccess interruptBlock{
        .BaseAddress = g_bootInfo.Acpi.LocalControllerPhysicalAddress | PhysicalUncachedAliasBit,
        .Kind = Ia64ProcessorInterruptBlockBaseKind::BootstrapPhysical,
        .Reserved = {},
    };
    const Ia64LocalSapicIpiControllerInitializationError controllerError =
        g_ia64LocalSapicIpiController.Initialize(g_bootInfo.Acpi, bspCrLid, interruptBlock);
    if (controllerError != Ia64LocalSapicIpiControllerInitializationError::None)
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AP-RENDEZVOUS-ERROR:IPI-CONTROLLER\r\n");
        return false;
    }

    const auto controllerSnapshot = g_ia64LocalSapicIpiController.GetSnapshot();

    const auto executionStorage =
        g_ia64ApplicationProcessorExecutionStorageOwner.Prepare(g_bootstrapPageAllocator, controllerSnapshot);
    if (!executionStorage.HasValue())
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AP-RENDEZVOUS-ERROR:EXECUTION-STORAGE\r\n");
        return false;
    }

    const auto workerStorage = g_ia64ApplicationProcessorRuntimeWorkerStorageOwner.Prepare(
        g_bootstrapPageAllocator, g_ia64ApplicationProcessorExecutionStorageOwner);
    if (!workerStorage.HasValue())
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AP-RENDEZVOUS-ERROR:WORKER-STORAGE\r\n");
        return false;
    }

    auto& takeover = Platform::Ia64::Interrupt::GetNativeIa64ApplicationProcessorTakeoverCoordinator();
    if (takeover.Initialize(g_ia64ApplicationProcessorExecutionStorageOwner) !=
        Ia64ApplicationProcessorTakeoverInitializationError::None)
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AP-RENDEZVOUS-ERROR:TAKEOVER-STATE\r\n");
        return false;
    }

    if (controllerSnapshot.ProcessorCount == 1U)
    {
        if (controllerSnapshot.EnabledProcessorMask != 0x01U)
        {
            Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AP-RENDEZVOUS-ERROR:PROCESSOR-MASK\r\n");
            return false;
        }
        if (!PrepareIa64SingleProcessorRuntimeProfile())
        {
            Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AP-RENDEZVOUS-ERROR:CAPABILITY-PROFILE\r\n");
            return false;
        }
        return true;
    }
    if (!g_ia64SalSystemTableInformation.HasApWakeVector)
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AP-RENDEZVOUS-ERROR:WAKE-VECTOR\r\n");
        return false;
    }

    Ia64SalBootRendezvousHandler& rendezvous = Platform::Ia64::Interrupt::GetNativeIa64SalBootRendezvousHandler();
    const Ia64SalBootRendezvousInitializationError handlerError =
        rendezvous.Initialize(controllerSnapshot, bspCrLid, g_ia64SalSystemTableInformation);
    if (handlerError != Ia64SalBootRendezvousInitializationError::None)
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AP-RENDEZVOUS-ERROR:HANDLER\r\n");
        return false;
    }

    const Platform::Ia64::Firmware::Ia64LoadedImageSection& codeSection = g_ia64LoadedImageLayout.Sections[0U];
    if ((codeSection.Purpose != Platform::Ia64::Firmware::Ia64LoadedImageSectionPurpose::Code) ||
        (codeSection.PageCount == 0U))
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AP-RENDEZVOUS-ERROR:CODE-EXTENT\r\n");
        return false;
    }
    const Platform::Ia64::Firmware::SalBootRendezvousHandler handler{
        .EntryPhysicalAddress = Ia64SalBootRendezvousHandler::GetPhysicalEntryAddress(),
        .GlobalPointerPhysicalAddress = Ia64SalBootRendezvousHandler::GetPhysicalGlobalPointerAddress(),
        .CodePhysicalStart = codeSection.VirtualStart,
        .CodeSizeBytes = codeSection.PageCount * Kernel::Memory::MappingPageSizeBytes,
    };
    const auto registration =
        SalBootRendezvousRegistration::Register(g_ia64SalEntrypointBinding, g_ia64SalSystemTableInformation, handler);
    if ((registration.Status != SalBootRendezvousRegistrationStatus::Registered) ||
        (registration.Disposition != SalBootRendezvousRegistrationDisposition::Registered))
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AP-RENDEZVOUS-ERROR:SAL-REGISTRATION\r\n");
        return false;
    }
    if (rendezvous.Arm() != Ia64SalBootRendezvousArmError::None)
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AP-RENDEZVOUS-ERROR:ARM\r\n");
        return false;
    }

    const Foundation::UInt8 targetProcessorMask =
        static_cast<Foundation::UInt8>(controllerSnapshot.EnabledProcessorMask & Foundation::UInt8{0xFEU});
    if (g_ia64LocalSapicIpiController.SendExternalInterrupt(g_ia64SalSystemTableInformation.ApWakeVector,
                                                            targetProcessorMask) != Ia64LocalSapicIpiSendError::None)
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AP-RENDEZVOUS-ERROR:IPI-SEND\r\n");
        return false;
    }

    return WaitForIa64ApplicationProcessorTakeover(controllerSnapshot, targetProcessorMask, takeover);
}

/// The validation key of one capability space of this boot, derived from
/// the published boot seed. A BootInfo without a published record stops the
/// boot here: no capability space may run with a predictable key.
[[nodiscard]] Kernel::Object::CapabilityValidationKey
DeriveIa64CapabilityKey(const Kernel::Object::CapabilityKeyPurpose purpose) noexcept
{
    const auto key = Kernel::Object::DeriveCapabilityValidationKey(g_bootInfo.Entropy, purpose);
    if (!key.HasValue())
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:CAPABILITY-KEY:FAIL\r\n");
        Platform::Firmware::HaltAfterFirmwareExit();
    }
    return key.GetValue();
}

#endif
#if defined(ASTERIA_TARGET_IA64)
#if ASTERIA_IA64_SYNTHETIC_USER_PROBE
enum class Ia64UserExecutionProbeBootstrapError : Foundation::UInt8
{
    None,
    Allocation,
    Staging,
};
#endif

[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool AllocateIa64SharedRootShootdownPage() noexcept
{
    if (g_ia64SharedRootShootdownPage.PhysicalStart != 0U)
    {
        return false;
    }
    const auto allocation = g_bootstrapPageAllocator.Allocate(1U, 1U);
    if (!allocation.HasValue())
    {
        return false;
    }
    const Kernel::Memory::PhysicalPageRange& page = allocation.GetValue();
    if ((page.PhysicalStart == 0U) || ((page.PhysicalStart & (Kernel::Memory::PhysicalPageSizeBytes - 1U)) != 0U) ||
        (page.PageCount != 1U))
    {
        return false;
    }
    g_ia64SharedRootShootdownPage = page;
    return true;
}

#if ASTERIA_IA64_SYNTHETIC_USER_PROBE
[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool AllocateIa64UserExecutionProbeStorageRegion(
    const Foundation::UInt64 pageCount, const Foundation::Size sizeBytes,
    Platform::Ia64::Scheduler::Ia64UserExecutionProbeStorageRegion& output) noexcept
{
    const auto allocation = g_bootstrapPageAllocator.Allocate(pageCount, 1U);
    if (!allocation.HasValue())
    {
        return false;
    }

    const Kernel::Memory::PhysicalPageRange& receipt = allocation.GetValue();
    output = Platform::Ia64::Scheduler::Ia64UserExecutionProbeStorageRegion{
        .Allocation = receipt,
        .WritableAlias =
            {
                .Data =
                    reinterpret_cast<Foundation::UInt8*>(static_cast<Foundation::UIntPointer>(receipt.PhysicalStart)),
                .SizeBytes = sizeBytes,
                .PhysicalStart = receipt.PhysicalStart,
            },
    };
    return true;
}

[[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64UserExecutionProbeBootstrapError StageIa64UserExecutionProbeStorage() noexcept
{
    auto* const owner =
        ::new (static_cast<void*>(g_ia64UserExecutionProbeStorage)) Platform::Ia64::Scheduler::Ia64UserExecutionProbe{};
    g_ia64UserExecutionProbeOwner = owner;

    Platform::Ia64::Scheduler::Ia64UserExecutionProbeStorage storage{};
    if (!AllocateIa64UserExecutionProbeStorageRegion(
            Platform::Ia64::Scheduler::Ia64UserExecutionProbeCodePageCount,
            static_cast<Foundation::Size>(Platform::Ia64::Scheduler::Ia64UserExecutionProbeCodeSizeBytes),
            storage.Code) ||
        !AllocateIa64UserExecutionProbeStorageRegion(
            Platform::Ia64::Scheduler::Ia64UserExecutionProbeMemoryStackPageCount,
            static_cast<Foundation::Size>(Platform::Ia64::Scheduler::Ia64UserExecutionProbeMemoryStackSizeBytes),
            storage.MemoryStack) ||
        !AllocateIa64UserExecutionProbeStorageRegion(
            Platform::Ia64::Scheduler::Ia64UserExecutionProbeRseBackingStorePageCount,
            static_cast<Foundation::Size>(Platform::Ia64::Scheduler::Ia64UserExecutionProbeRseBackingStoreSizeBytes),
            storage.RseBackingStore))
    {
        return Ia64UserExecutionProbeBootstrapError::Allocation;
    }

    const auto payload = Platform::Ia64::Scheduler::GetIa64UserExecutionProbePayload();
    const auto stage = owner->StageStorage(storage, Platform::Ia64::Scheduler::Ia64UserExecutionProbeCodeView{
                                                        .Data = payload.Data,
                                                        .SizeBytes = payload.SizeBytes,
                                                    });
    return stage.HasValue() ? Ia64UserExecutionProbeBootstrapError::None
                            : Ia64UserExecutionProbeBootstrapError::Staging;
}
#endif

[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool StageIa64InitialUserImage() noexcept
{
    auto* const owner = ::new (static_cast<void*>(g_ia64InitialUserImageOwnerStorage))
        Platform::Ia64::Executable::Ia64InitialUserImageOwner{};
    g_ia64InitialUserImageOwner = owner;
    const Platform::Ia64::Memory::TranslationCapabilities& translation =
        g_ia64PalBootstrapCapabilities.VirtualMemory.Translation;
    const auto stage = owner->Stage(
        g_bootstrapPageAllocator, translation.PhysicalAddressBits,
        translation.ImplementedVirtualAddressMostSignificantBit,
        Platform::Ia64::Executable::Ia64InitialUserImageSource::Get(g_bootInfo.Files, BootUserFile), g_bootInfo.Files);
    return stage.HasValue();
}

// The boot pager serves the initial image's lazy region; the synthetic probe
// configuration has no pager.
[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool StageIa64PagerImage() noexcept
{
#if ASTERIA_IA64_SYNTHETIC_USER_PROBE
    return true;
#else
    auto* const pager = ::new (static_cast<void*>(g_ia64PagerImageOwnerStorage))
        Platform::Ia64::Executable::Ia64InitialUserImageOwner{};
    g_ia64PagerImageOwner = pager;
    const Platform::Ia64::Memory::TranslationCapabilities& translation =
        g_ia64PalBootstrapCapabilities.VirtualMemory.Translation;
    if (!pager
             ->Stage(g_bootstrapPageAllocator, translation.PhysicalAddressBits,
                     translation.ImplementedVirtualAddressMostSignificantBit,
                     Platform::Ia64::Executable::Ia64InitialUserImageSource::Get(g_bootInfo.Files, BootInitFile),
                     g_bootInfo.Files)
             .HasValue())
    {
        return false;
    }
    // Each lazy page starts with LazyPagePattern and its page index, so the
    // client can tell it reads the pager's Frame; the rest is zero. Physical
    // addressing is still in effect here.
    const auto lazy = g_bootstrapPageAllocator.Allocate(Platform::Ia64::Ia64InitialUserLazyPageCount, 1U);
    if (!lazy.HasValue() || (lazy.GetValue().PageCount != Platform::Ia64::Ia64InitialUserLazyPageCount))
    {
        return false;
    }
    g_ia64LazyRegion = lazy.GetValue();
    for (Foundation::UInt64 page{0U}; page < g_ia64LazyRegion.PageCount; ++page)
    {
        const Foundation::UInt64 address =
            g_ia64LazyRegion.PhysicalStart + (page * Kernel::Memory::PhysicalPageSizeBytes);
        auto* const words = reinterpret_cast<Foundation::UInt64*>(static_cast<Foundation::UIntPointer>(address));
        for (Foundation::UInt64 word{0U}; word < (Kernel::Memory::PhysicalPageSizeBytes / 8U); ++word)
        {
            words[word] = 0U;
        }
        words[0U] = Abi::LazyPagePattern;
        words[1U] = page;
    }
    // No separate low-memory allocation. The last residue is imported dirty after DMA handoff and
    // the mastering gate.
    return true;
#endif
}
#endif

[[noreturn]] ASTERIA_NOINLINE_NOIPA void RunPostExitBootstrap() noexcept
{
#if defined(ASTERIA_TARGET_IA64)
    const auto binding =
        Platform::Ia64::Firmware::SalEntrypointBinder::Bind(g_ia64SalSystemTableInformation, g_bootInfo);
    if (!binding.HasValue())
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:SAL-ENTRYPOINT-BINDING:FAIL\r\n");
        Platform::Firmware::HaltAfterFirmwareExit();
    }
    g_ia64SalEntrypointBinding = binding.GetValue();
    Platform::Firmware::WriteDiagnosticAscii("ASTERIA:STAGE:SAL-ENTRYPOINT-BINDING\r\n");

    const auto palCapabilities =
        Platform::Ia64::Firmware::PalBootstrapCapabilityCapture::Capture(g_ia64SalEntrypointBinding);
    if (!palCapabilities.HasValue())
    {
        // Which query or decoder refused, so a new processor model's PAL
        // report can be diagnosed from the line alone.
        const auto& error = palCapabilities.GetError();
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:PAL-CAPABILITIES:FAIL:PHASE=");
        WriteIa64DiagnosticHex(static_cast<Foundation::UInt32>(error.Phase), 2U);
        Platform::Firmware::WriteDiagnosticAscii(":ADAPTER=");
        WriteIa64DiagnosticHex(static_cast<Foundation::UInt32>(error.AdapterError), 2U);
        Platform::Firmware::WriteDiagnosticAscii(":VM=");
        WriteIa64DiagnosticHex(static_cast<Foundation::UInt32>(error.VirtualMemoryError), 2U);
        Platform::Firmware::WriteDiagnosticAscii(":PURGE=");
        WriteIa64DiagnosticHex(static_cast<Foundation::UInt32>(error.TranslationCachePurgeError), 2U);
        Platform::Firmware::WriteDiagnosticAscii("\r\n");
        Platform::Firmware::HaltAfterFirmwareExit();
    }
    g_ia64PalBootstrapCapabilities = palCapabilities.GetValue();
    Platform::Firmware::WriteDiagnosticAscii("ASTERIA:STAGE:PAL-CAPABILITIES\r\n");
#endif

    const Kernel::Memory::BootstrapPageAllocatorError allocatorError =
        g_bootstrapPageAllocator.IsInitialized() ? Kernel::Memory::BootstrapPageAllocatorError::None
                                                 : g_bootstrapPageAllocator.Initialize(g_bootInfo);
    if (allocatorError != Kernel::Memory::BootstrapPageAllocatorError::None)
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:PAGE-ALLOCATOR:FAIL\r\n");
        Platform::Firmware::HaltAfterFirmwareExit();
    }
#if defined(ASTERIA_TARGET_IA64)
    if ((g_ia64RuntimeMemoryReservation.PhysicalStart == 0U) ||
        ((g_ia64PalBootstrapCapabilities.VirtualMemory.Translation.InsertablePageSizeMask &
          (g_ia64RuntimeMemoryReservation.PageCount << 12U)) == 0U))
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:PAGE-ALLOCATOR:FAIL\r\n");
        Platform::Firmware::HaltAfterFirmwareExit();
    }
#endif
    Platform::Firmware::WriteDiagnosticAscii("ASTERIA:STAGE:PAGE-ALLOCATOR\r\n");
#if defined(ASTERIA_TARGET_IA64)
    if (!BuildIa64InitialTranslationPlan(
            Kernel::Memory::PhysicalPageRange{
                .PhysicalStart = g_ia64RuntimeMemoryReservation.PhysicalStart,
                .PageCount = Platform::Ia64::Memory::BootstrapReservedPageCount,
                .Attributes = g_ia64RuntimeMemoryReservation.Attributes,
                .FirmwareType = g_ia64RuntimeMemoryReservation.FirmwareType,
                .Reserved = 0U,
            },
            g_ia64RuntimeMemoryReservation))
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:INITIAL-TRANSLATION-PLAN:FAIL\r\n");
        Platform::Firmware::HaltAfterFirmwareExit();
    }
    Platform::Firmware::WriteDiagnosticAscii("ASTERIA:STAGE:INITIAL-TRANSLATION-PLAN\r\n");
    if (!VerifyIa64TranslationRegisterBudget())
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:IA64-TRANSLATION-REGISTER-BUDGET:FAIL\r\n");
        Platform::Firmware::HaltAfterFirmwareExit();
    }
    Platform::Firmware::WriteDiagnosticAscii("ASTERIA:STAGE:IA64-TRANSLATION-REGISTER-BUDGET\r\n");

#if ASTERIA_IA64_SYNTHETIC_USER_PROBE
    const Ia64UserExecutionProbeBootstrapError userProbeBootstrapError = StageIa64UserExecutionProbeStorage();
    if (userProbeBootstrapError == Ia64UserExecutionProbeBootstrapError::Allocation)
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:USER-EXECUTION-PROBE-STORAGE:FAIL\r\n");
        Platform::Firmware::HaltAfterFirmwareExit();
    }
    if (userProbeBootstrapError == Ia64UserExecutionProbeBootstrapError::Staging)
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:USER-EXECUTION-PROBE-STAGE:FAIL\r\n");
        Platform::Firmware::HaltAfterFirmwareExit();
    }
#endif
    if (!StageIa64InitialUserImage())
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:INITIAL-USER-IMAGE-STAGE:FAIL\r\n");
        Platform::Firmware::HaltAfterFirmwareExit();
    }
    if (!StageIa64PagerImage())
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:PAGER-IMAGE-STAGE:FAIL\r\n");
        Platform::Firmware::HaltAfterFirmwareExit();
    }
    if (!AllocateIa64SharedRootShootdownPage())
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:SHARED-ROOT-SHOOTDOWN-PAGE:FAIL\r\n");
        Platform::Firmware::HaltAfterFirmwareExit();
    }

    // A successful switch continues in AsteriaIa64RunOwnedExecutionContext,
    // which reports the stage; only a rejected switch returns here.
    const auto executionContextFailure = SwitchToIa64InitialExecutionContext();
    WriteIa64InitialExecutionContextSwitchError(executionContextFailure);
    Platform::Firmware::WriteDiagnosticAscii("ASTERIA:INITIAL-EXECUTION-CONTEXT:FAIL\r\n");
    Platform::Firmware::HaltAfterFirmwareExit();
#endif

#if defined(ASTERIA_TARGET_AMD64)
    if (!IsAmd64InitialAddressSpacePrepared())
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AMD64-INITIAL-MAPPING-PLAN:FAIL\r\n"
                                                 "ASTERIA:AMD64-INITIAL-PAGE-TABLE:FAIL\r\n"
                                                 "ASTERIA:AMD64-INITIAL-ADDRESS-SPACE:FAIL\r\n");
        Platform::Firmware::HaltAfterFirmwareExit();
    }
    Platform::Firmware::WriteDiagnosticAscii("ASTERIA:STAGE:AMD64-INITIAL-MAPPING-PLAN\r\n"
                                             "ASTERIA:STAGE:AMD64-INITIAL-PAGE-TABLE\r\n");
    const auto activationError = g_amd64InitialAddressSpacePreparation.Activate();
    static_cast<void>(activationError);
    Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AMD64-INITIAL-ADDRESS-SPACE:FAIL\r\n");
    Platform::Firmware::HaltAfterFirmwareExit();
#endif
#if !defined(ASTERIA_TARGET_AMD64)
    Platform::Firmware::HaltAfterFirmwareExit();
#endif
}

[[nodiscard]] Platform::Firmware::AhciPciResource DecodePciResource(const BootPciResource& resource) noexcept
{
    return {
        resource.BaseAddress, resource.LengthBytes, resource.Segment,  resource.Bus,
        resource.Device,      resource.Function,    resource.BarIndex, {resource.Reserved[0U], resource.Reserved[1U]}};
}

[[nodiscard]] bool ImportKernelInput(const BootInfo& info) noexcept
{
    if (!ValidateBootInfo(info) || !ValidateBootFileRegion(info) ||
        info.Kernel.Image.SectionCount > BootImageMaximumSections ||
        info.Kernel.EvidencePhysicalAddress != GetKernelEvidenceAddress())
    {
        return false;
    }
    // More firmware DMA reservations than the list holds stop the boot.
    if (!::Asteria::Boot::IsCompleteBootDmaExclusions(info.DmaExclusions))
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:BOOT-INFO:FAIL:DMA-EXCLUSIONS\r\n");
        return false;
    }
    const BootKernelInput& input = info.Kernel;
#if defined(ASTERIA_TARGET_AMD64)
    if (input.Architecture != BootArchitectureAmd64 ||
        input.Image.SectionCount != Platform::Amd64::Firmware::Amd64LoadedImageSectionCount ||
        input.Paging.FourLevelPagingEnabled > 1U || input.Paging.ExecuteDisableEnabled > 1U ||
        input.Paging.SupervisorWriteProtectEnabled > 1U || input.Paging.GlobalPagesEnabled > 1U ||
        input.Paging.PatLayoutConfigured > 1U || input.Paging.Reserved16 != 0U || input.Paging.Reserved32 != 0U ||
        input.Paging.CaptureError >
            static_cast<Foundation::UInt32>(
                Platform::Amd64::Memory::InitialPagingCapabilityCaptureError::InvalidPhysicalAddressWidth))
    {
        return false;
    }
    if (input.Diagnostics.DebugPortPresent > 1U || input.Diagnostics.SerialPort > 0xFFF8U ||
        input.Diagnostics.Reserved != 0U ||
        info.DebugPort.Source != static_cast<Foundation::UInt8>(BootDebugPortSource::None) ||
        !IsValidBootDebugPort(info.DebugPort))
    {
        return false;
    }
    Platform::Amd64::Firmware::InstallDiagnosticSinks(input.Diagnostics);
    g_amd64LoadedImageLayout.ImageBase = input.Image.PhysicalStart;
    g_amd64LoadedImageLayout.ImageSize = input.Image.SizeBytes;
    for (Foundation::Size index{0U}; index < input.Image.SectionCount; ++index)
    {
        const BootImageSection& section = input.Image.Sections[index];
        if (section.Purpose > 2U || section.Reserved != 0U)
        {
            return false;
        }
        g_amd64LoadedImageLayout.Sections[index] = {
            section.PhysicalStart, section.PageCount,
            static_cast<Platform::Amd64::Firmware::Amd64LoadedImageSectionPurpose>(section.Purpose), 0U};
    }
    if (!g_amd64InitialAddressSpacePreparation.IsPrepared())
    {
        g_amd64BootstrapReservation = {input.Bootstrap.PhysicalStart, input.Bootstrap.PageCount,
                                       input.Bootstrap.Attributes, input.Bootstrap.FirmwareType,
                                       input.Bootstrap.Reserved};
    }
    g_amd64RuntimePool = input.RuntimePool.PhysicalStart;
    g_amd64StartupPages = input.Startup.PhysicalStart;
    g_amd64AhciResource = DecodePciResource(input.Ahci);
    g_amd64DeviceResource = DecodePciResource(input.Device);
    g_amd64NicResource = DecodePciResource(input.Nic);
    g_amd64InitialPagingCapabilities = {input.Paging.PhysicalAddressBits,
                                        input.Paging.FourLevelPagingEnabled != 0U,
                                        input.Paging.ExecuteDisableEnabled != 0U,
                                        input.Paging.SupervisorWriteProtectEnabled != 0U,
                                        input.Paging.GlobalPagesEnabled != 0U,
                                        input.Paging.PatLayoutConfigured != 0U,
                                        0U,
                                        0U};
#elif defined(ASTERIA_TARGET_IA64)
    // IA-64 keeps its slabs in the runtime page: no metadata arena.
    if (input.MetadataArena.PhysicalStart != 0U || input.MetadataArena.PageCount != 0U ||
        input.MetadataArena.Attributes != 0U || input.MetadataArena.FirmwareType != 0U)
    {
        return false;
    }
    if (input.Architecture != BootArchitectureIa64 ||
        input.Image.SectionCount != Platform::Ia64::Firmware::Ia64LoadedImageSectionCount ||
        input.Image.Placement > 2U || input.Sal.HasPlatformFeatures > 1U || input.Sal.HasTranslationRegister > 1U ||
        input.Sal.HasPurgeCoherenceInformation > 1U || input.Sal.HasApWakeVector > 1U || input.Sal.Reserved8 != 0U)
    {
        return false;
    }
    g_ia64LoadedImageLayout.ImageBase = input.Image.PhysicalStart;
    g_ia64LoadedImageLayout.ImageSize = input.Image.SizeBytes;
    g_ia64LoadedImageLayout.Placement =
        static_cast<Platform::Ia64::Firmware::Ia64LoadedImagePlacement>(input.Image.Placement);
    for (Foundation::Size index{0U}; index < input.Image.SectionCount; ++index)
    {
        const BootImageSection& section = input.Image.Sections[index];
        if (section.Purpose > 2U || section.Reserved != 0U)
        {
            return false;
        }
        g_ia64LoadedImageLayout.Sections[index] = {
            section.PhysicalStart, section.PageCount,
            static_cast<Platform::Ia64::Firmware::Ia64LoadedImageSectionPurpose>(section.Purpose), 0U};
    }
    if (input.Image.Placement == 2U)
    {
        if (input.Ia64Runtime.PageCount != Platform::Ia64::Memory::RuntimeMemoryReservationPreferredBytes / 4096U ||
            input.Bootstrap.PhysicalStart != input.Ia64Runtime.PhysicalStart ||
            input.Bootstrap.PageCount != Platform::Ia64::Memory::BootstrapReservedPageCount ||
            input.Ia64Runtime.FirmwareType != 2U || input.Bootstrap.FirmwareType != 2U ||
            input.Ia64Runtime.Reserved != 0U || input.Bootstrap.Reserved != 0U ||
            input.Ia64Runtime.Attributes != input.Bootstrap.Attributes)
        {
            return false;
        }
        g_ia64RuntimeMemoryReservation = {input.Ia64Runtime.PhysicalStart, input.Ia64Runtime.PageCount,
                                          input.Ia64Runtime.Attributes, input.Ia64Runtime.FirmwareType,
                                          input.Ia64Runtime.Reserved};
    }
    const BootSalInformation& sal = input.Sal;
    g_ia64SalSystemTableInformation = {sal.PalProcedurePhysicalAddress,
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
                                       sal.HasPlatformFeatures != 0U,
                                       sal.HasTranslationRegister != 0U,
                                       sal.HasPurgeCoherenceInformation != 0U,
                                       sal.HasApWakeVector != 0U,
                                       0U};
    g_ia64AhciPciResource = DecodePciResource(input.Ahci);
    g_ia64ItcTicksPerMicrosecond = input.TimerTicksPerMicrosecond;
    Platform::Synchronization::ConfigureProcessorWatchdog(info.WatchdogSeconds,
                                                          input.TimerTicksPerMicrosecond * 1'000'000U);
    // The loader's debug-port decision (BootProtocol.md): every later
    // diagnostic, on every processor, goes to that UART or nowhere.
    if (!IsValidBootDebugPort(info.DebugPort))
    {
        return false;
    }
    Platform::Firmware::InstallIa64DiagnosticUart(Platform::Ia64::Firmware::ResolveIa64DebugUart(
        info.DebugPort, Platform::Ia64::Firmware::FindIa64PortSpaceWindow(info.MemoryRegions, info.MemoryRegionCount)));
#endif
    static_cast<void>(memcpy(&g_bootInfo, &info, sizeof(info)));
    if (!ImportBootInventory(g_bootInfo))
    {
        return false;
    }
    SealBootInfo(g_bootInfo);
#if defined(ASTERIA_TARGET_IA64) && !ASTERIA_IA64_SYNTHETIC_USER_PROBE
    g_ia64I8042DsdtMatches = Ia64I8042DsdtMatches(g_bootInfo.Acpi);
#endif
    return true;
}

} // Namespace

Foundation::UInt64 GetKernelEvidenceAddress() noexcept
{
#if defined(ASTERIA_TARGET_AMD64)
    return reinterpret_cast<Foundation::UIntPointer>(&Platform::Amd64::Diagnostics::GetAmd64EvidenceRecord());
#else
    return reinterpret_cast<Foundation::UIntPointer>(&Platform::Ia64::Diagnostics::GetIa64EvidenceRecord());
#endif
}

bool PrepareKernel(BootInfo& info) noexcept
{
    if (!ImportKernelInput(info))
    {
        return false;
    }
#if defined(ASTERIA_TARGET_AMD64)
    if (!g_amd64InitialAddressSpacePreparation.IsPrepared())
    {
        static_cast<void>(Platform::Amd64::Diagnostics::InitializeAmd64EvidenceRecord());
    }
    Platform::Amd64::Diagnostics::GetAmd64EvidenceRecord().BootFiles = info.Files;
    const bool prepared = PrepareAmd64FinalMemoryMap(g_bootInfo);
    if (prepared)
    {
        info.Kernel.TimerTicksPerMicrosecond = Platform::Amd64::Bootstrap::GetAmd64BootTscTicksPerMicrosecond();
        g_bootInfo.Kernel.TimerTicksPerMicrosecond = info.Kernel.TimerTicksPerMicrosecond;
        SealBootInfo(g_bootInfo);
        SealBootInfo(info);
    }
    return prepared;
#else
    static_cast<void>(Platform::Ia64::Diagnostics::InitializeIa64EvidenceRecord());
    Platform::Ia64::Diagnostics::GetIa64EvidenceRecord().BootFiles = info.Files;
    if (!g_bootstrapPageAllocator.IsInitialized())
    {
        if (g_bootstrapPageAllocator.Initialize(g_bootInfo) != Kernel::Memory::BootstrapPageAllocatorError::None ||
            (info.Kernel.Image.Placement != 2U &&
             !AllocateIa64RuntimeMemoryReservation(Platform::Ia64::Memory::ArchitectedMinimumInsertablePageSizeMask)))
        {
            return false;
        }
    }
    // Allocation owns its lower alignment slack and upper guard too. A retry
    // accepts only the same Conventional, write-back extent in the new map.
    const auto& reservation = g_ia64RuntimeMemoryReservation;
    const Foundation::UInt64 bytes = reservation.PageCount << 12U;
    bool retained{false};
    for (Foundation::Size index{0U}; index < info.MemoryRegionCount; ++index)
    {
        const auto& region = info.MemoryRegions[index];
        if ((region.Kind == PhysicalMemoryKind::Conventional ||
             (info.Kernel.Image.Placement == 2U && region.Kind == PhysicalMemoryKind::Loader)) &&
            region.FirmwareType == reservation.FirmwareType && region.Attributes == reservation.Attributes &&
            reservation.PhysicalStart >= bytes && region.PhysicalStart <= reservation.PhysicalStart - bytes &&
            region.PageCount <= (~Foundation::UInt64{0U} >> 12U))
        {
            const Foundation::UInt64 offset = reservation.PhysicalStart - bytes - region.PhysicalStart;
            const Foundation::UInt64 regionBytes = region.PageCount << 12U;
            retained = offset <= regionBytes && (2U * bytes + 4096U) <= regionBytes - offset;
            if (retained)
            {
                break;
            }
        }
    }
    if (!retained)
    {
        return false;
    }
    info.Kernel.Ia64Runtime = {reservation.PhysicalStart, reservation.PageCount, reservation.Attributes,
                               reservation.FirmwareType, 0U};
    info.Kernel.Bootstrap = {reservation.PhysicalStart, Platform::Ia64::Memory::BootstrapReservedPageCount,
                             reservation.Attributes, reservation.FirmwareType, 0U};
    g_bootInfo.Kernel.Ia64Runtime = info.Kernel.Ia64Runtime;
    g_bootInfo.Kernel.Bootstrap = info.Kernel.Bootstrap;
    SealBootInfo(g_bootInfo);
    SealBootInfo(info);
    return true;
#endif
}

[[noreturn]] void EnterKernel(const BootInfo& info) noexcept
{
    if (!ImportKernelInput(info))
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:BOOT-INFO:FAIL:ABI\r\n");
        Platform::Firmware::HaltAfterFirmwareExit();
    }
    ReportIgnoredProcessors();
    RunPostExitBootstrap();
}

#if defined(ASTERIA_TARGET_AMD64)
bool IsAmd64TableRejection() noexcept
{
    return g_amd64FinalMemoryMapRejection == Amd64FinalMemoryMapTableRejection;
}
const char* GetAmd64FinalMemoryMapRejection() noexcept
{
    return g_amd64FinalMemoryMapRejection;
}
Platform::Amd64::Memory::InitialPagingCapabilityCaptureError GetAmd64PagingCapabilityError() noexcept
{
    return g_amd64PagingCapabilityError;
}
Platform::Amd64::Memory::InitialProcessorStateCaptureError GetAmd64ProcessorStateCaptureError() noexcept
{
    return g_amd64ProcessorStateCaptureError;
}
const Platform::Amd64::Memory::InitialAddressSpacePreparation& GetAmd64AddressSpacePreparation() noexcept
{
    return g_amd64InitialAddressSpacePreparation;
}
const Platform::Amd64::Memory::InitialProcessorStateReport& GetAmd64ProcessorStateReport() noexcept
{
    return g_amd64InitialProcessorStateReport;
}
#endif
} // Namespace Asteria::Boot::Bootstrap

#if defined(ASTERIA_TARGET_IA64)
extern "C" [[noreturn, gnu::used]] void
AsteriaIa64RunApplicationProcessorOwnedExecutionContext(const Asteria::Foundation::UInt64 processorValue) noexcept
{
    using Asteria::Kernel::Scheduler::CpuId;
    using Asteria::Platform::Ia64::Interrupt::Ia64ApplicationProcessorTakeoverPublicationError;
    using Asteria::Platform::Ia64::Memory::InitialInterruptVectorTableApInstallStatus;

    if ((processorValue == 0U) || (processorValue >= Asteria::Kernel::Scheduler::MaximumProcessorCount))
    {
        Asteria::Platform::Firmware::HaltAfterFirmwareExit();
    }
    const CpuId processor{static_cast<Asteria::Foundation::UInt8>(processorValue)};
    auto& takeover = Asteria::Platform::Ia64::Interrupt::GetNativeIa64ApplicationProcessorTakeoverCoordinator();

    const InitialInterruptVectorTableApInstallStatus interruptVectorTableStatus =
        Asteria::Platform::Ia64::Memory::InitialInterruptVectorTableInstaller::InstallForApplicationProcessor(
            Asteria::Boot::Bootstrap::g_ia64InitialInterruptVectorTableInstallSummary);
    if (interruptVectorTableStatus != InitialInterruptVectorTableApInstallStatus::Installed)
    {
        const auto publication = takeover.PublishInterruptVectorTableFailure(processor, interruptVectorTableStatus);
        if (publication != Ia64ApplicationProcessorTakeoverPublicationError::None)
        {
            Asteria::Platform::Firmware::HaltAfterFirmwareExit();
        }
        Asteria::Platform::Firmware::HaltAfterFirmwareExit();
    }

    if (takeover.PublishOwnedPhysical(processor) != Ia64ApplicationProcessorTakeoverPublicationError::None)
    {
        Asteria::Platform::Firmware::HaltAfterFirmwareExit();
    }

    const auto palCapabilities = Asteria::Platform::Ia64::Firmware::PalBootstrapCapabilityCapture::Capture(
        Asteria::Boot::Bootstrap::g_ia64SalEntrypointBinding);
    if (!palCapabilities.HasValue())
    {
        if (takeover.PublishPalCaptureFailure(processor, palCapabilities.GetErrorReference()) !=
            Ia64ApplicationProcessorTakeoverPublicationError::None)
        {
            Asteria::Platform::Firmware::HaltAfterFirmwareExit();
        }
        Asteria::Platform::Firmware::HaltAfterFirmwareExit();
    }

    if (takeover.PublishPalCaptureSuccess(processor, palCapabilities.GetValue()) !=
        Ia64ApplicationProcessorTakeoverPublicationError::None)
    {
        Asteria::Platform::Firmware::HaltAfterFirmwareExit();
    }

    const auto installError = Asteria::Boot::Bootstrap::InstallIa64InitialTranslations(palCapabilities.GetValue());
    if (installError != Asteria::Platform::Ia64::Memory::InitialTranslationInstallError::None)
    {
        if (takeover.PublishInitialTranslationInstallFailure(processor, installError) !=
            Ia64ApplicationProcessorTakeoverPublicationError::None)
        {
            Asteria::Platform::Firmware::HaltAfterFirmwareExit();
        }
        Asteria::Platform::Firmware::HaltAfterFirmwareExit();
    }

    if (takeover.PublishInitialTranslationInstallSuccess(processor) !=
        Ia64ApplicationProcessorTakeoverPublicationError::None)
    {
        Asteria::Platform::Firmware::HaltAfterFirmwareExit();
    }

    Asteria::Platform::Ia64::Interrupt::Ia64ApplicationProcessorRuntimeRelease release{};
    if (!Asteria::Boot::Bootstrap::WaitForIa64ApplicationProcessorRuntimeRelease(processor, release))
    {
        Asteria::Platform::Firmware::HaltAfterFirmwareExit();
    }
    Asteria::Boot::Bootstrap::AdmitAndPivotIa64ApplicationProcessorRuntime(processor, release.Generation);
}

extern "C" [[noreturn, gnu::used]] void
AsteriaIa64RunApplicationProcessorTranslatedKernel(const Asteria::Foundation::UInt64 processorValue) noexcept
{
    if ((processorValue == 0U) || (processorValue >= Asteria::Kernel::Scheduler::MaximumProcessorCount))
    {
        Asteria::Platform::Firmware::HaltAfterFirmwareExit();
    }

    const Asteria::Kernel::Scheduler::CpuId processor{static_cast<Asteria::Foundation::UInt8>(processorValue)};
    auto* const runtime = Asteria::Boot::Bootstrap::g_ia64ApplicationProcessorKernelRuntimeOwner;
    if (runtime == nullptr)
    {
        Asteria::Platform::Firmware::HaltAfterFirmwareExit();
    }
    runtime->RunTranslatedLanding(processor);
}

extern "C" [[noreturn, gnu::used]] void AsteriaIa64RunOwnedExecutionContext() noexcept
{
    Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:STAGE:INITIAL-EXECUTION-CONTEXT\r\n");
    // SAL requires OS IVA ownership before the advertised external AP wake
    // vector is used. Keep this BSP commit ahead of the rendezvous helper.
    const auto interruptVectorTableInstallError = Asteria::Boot::Bootstrap::InstallIa64InitialInterruptVectorTable();
    if (interruptVectorTableInstallError !=
        Asteria::Platform::Ia64::Memory::InitialInterruptVectorTableInstallError::None)
    {
        Asteria::Boot::Bootstrap::WriteIa64InitialInterruptVectorTableInstallError(interruptVectorTableInstallError);
        Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:INITIAL-IVT-INSTALL:FAIL\r\n");
        Asteria::Platform::Firmware::HaltAfterFirmwareExit();
    }
    Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:STAGE:INITIAL-IVT-INSTALL\r\n");

    if (!Asteria::Boot::Bootstrap::BootstrapIa64ApplicationProcessors())
    {
        Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AP-RENDEZVOUS:FAIL\r\n");
        Asteria::Platform::Firmware::HaltAfterFirmwareExit();
    }
    Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:STAGE:AP-RENDEZVOUS\r\n");

    const auto installError = Asteria::Boot::Bootstrap::InstallIa64InitialTranslations(
        Asteria::Boot::Bootstrap::g_ia64PalBootstrapCapabilities);
    if (installError != Asteria::Platform::Ia64::Memory::InitialTranslationInstallError::None)
    {
        Asteria::Boot::Bootstrap::WriteIa64InitialTranslationInstallError(installError);
        Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:INITIAL-TRANSLATION-INSTALL:FAIL\r\n");
        Asteria::Platform::Firmware::HaltAfterFirmwareExit();
    }
    Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:STAGE:INITIAL-TRANSLATION-INSTALL\r\n");

    if (!Asteria::Boot::Bootstrap::PrepareIa64RootResourceDomain())
    {
        Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:BSP-ROOT-RESOURCE-DOMAIN:FAIL\r\n");
        Asteria::Platform::Firmware::HaltAfterFirmwareExit();
    }

    if (!Asteria::Boot::Bootstrap::PrepareIa64BspRuntimeMemory())
    {
        Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:BSP-RUNTIME-MEMORY:FAIL\r\n");
        Asteria::Platform::Firmware::HaltAfterFirmwareExit();
    }
    Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:STAGE:BSP-RUNTIME-MEMORY\r\n");

    if (!Asteria::Boot::Bootstrap::PrepareIa64InitialUserImageObjects())
    {
        Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:INITIAL-USER-IMAGE-OBJECTS:FAIL\r\n");
        Asteria::Platform::Firmware::HaltAfterFirmwareExit();
    }
    if (!Asteria::Boot::Bootstrap::PrepareIa64PagerImageObjects())
    {
        Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:PAGER-IMAGE-OBJECTS:FAIL\r\n");
        Asteria::Platform::Firmware::HaltAfterFirmwareExit();
    }
    Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:STAGE:INITIAL-USER-IMAGE-OBJECTS\r\n");

    if (!Asteria::Boot::Bootstrap::PrepareIa64InitialUserImageMappings())
    {
        Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:INITIAL-USER-IMAGE-MAPPINGS:FAIL\r\n");
        Asteria::Platform::Firmware::HaltAfterFirmwareExit();
    }
    if (!Asteria::Boot::Bootstrap::PrepareIa64PagerImageMappings())
    {
        Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:PAGER-IMAGE-MAPPINGS:FAIL\r\n");
        Asteria::Platform::Firmware::HaltAfterFirmwareExit();
    }
    Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:STAGE:INITIAL-USER-IMAGE-MAPPINGS\r\n");

    // Successful activation continues in AsteriaIa64RunInitialTranslatedKernel,
    // which reports the stage; only a rejected activation returns here.
    const auto activationError = Asteria::Platform::Ia64::Memory::InitialTranslationActivator::Activate();
    if (activationError ==
        Asteria::Platform::Ia64::Memory::InitialTranslationActivationError::UnsupportedProcessorState)
    {
        Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:INITIAL-TRANSLATION-ACTIVATION:PROCESSOR-STATE\r\n");
    }
    else
    {
        Asteria::Platform::Firmware::WriteDiagnosticAscii(
            "ASTERIA:INITIAL-TRANSLATION-ACTIVATION:ADAPTER-RETURNED\r\n");
    }
    Asteria::Platform::Firmware::HaltAfterFirmwareExit();
}

extern "C" [[noreturn, gnu::used]] void AsteriaIa64RunInitialTranslatedKernel() noexcept
{
    using KernelRuntime = Asteria::Platform::Ia64::Scheduler::Ia64BspKernelRuntime;

    Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:STAGE:INITIAL-TRANSLATION-ACTIVATION\r\n");
    if (Asteria::Boot::Bootstrap::g_ia64BspRuntimeMemoryOwner == nullptr)
    {
        Asteria::Platform::Ia64::Diagnostics::StopIa64(
            Asteria::Kernel::Diagnostics::StopCode::BootStage,
            static_cast<Asteria::Foundation::UInt64>(
                Asteria::Platform::Ia64::Diagnostics::Ia64BootStage::ProcessorAdmission));
    }
    if (!Asteria::Boot::Bootstrap::PrepareIa64ApplicationProcessorRuntimeMappings())
    {
        Asteria::Platform::Ia64::Diagnostics::StopIa64(
            Asteria::Kernel::Diagnostics::StopCode::BootStage,
            static_cast<Asteria::Foundation::UInt64>(
                Asteria::Platform::Ia64::Diagnostics::Ia64BootStage::ApplicationProcessors));
    }

    auto* const runtime = ::new (static_cast<void*>(Asteria::Boot::Bootstrap::g_ia64BspKernelRuntimeStorage))
        KernelRuntime{Asteria::Boot::Bootstrap::g_ia64BspObjectRegistry,
                      Asteria::Boot::Bootstrap::g_ia64BspRegistryLock,
                      Asteria::Boot::Bootstrap::DeriveIa64CapabilityKey(
                          Asteria::Kernel::Object::CapabilityKeyPurpose::BootstrapSpace)};
    const auto preparation = runtime->Prepare(*Asteria::Boot::Bootstrap::g_ia64BspRuntimeMemoryOwner,
                                              Asteria::Boot::Bootstrap::g_ia64RootResourceDomainIdentity,
                                              *Asteria::Boot::Bootstrap::g_ia64RootResourceDomain,
                                              Asteria::Boot::Bootstrap::g_ia64BspRootCapabilityIssuer,
                                              Asteria::Boot::Bootstrap::g_ia64PalBootstrapCapabilities.VirtualMemory,
                                              Asteria::Boot::Bootstrap::Ia64BootstrapTimerVector);
    if (!preparation.HasValue())
    {
        Asteria::Platform::Ia64::Diagnostics::StopIa64(
            Asteria::Kernel::Diagnostics::StopCode::BootStage,
            static_cast<Asteria::Foundation::UInt64>(
                Asteria::Platform::Ia64::Diagnostics::Ia64BootStage::KernelPreparation));
    }

    auto* const pciEcamRuntime = ::new (static_cast<void*>(
        Asteria::Boot::Bootstrap::g_ia64PciEcamRuntimeStorage)) Asteria::Platform::Device::PciEcamRuntime{
        Asteria::Boot::Bootstrap::g_ia64BspObjectRegistry, Asteria::Boot::Bootstrap::g_ia64BspRegistryLock,
        Asteria::Boot::Bootstrap::g_ia64BspRootCapabilityIssuer,
        Asteria::Boot::Bootstrap::DeriveIa64CapabilityKey(Asteria::Kernel::Object::CapabilityKeyPurpose::DeviceSpace),
        Asteria::Boot::Bootstrap::g_ia64PhysicalMemoryAccess};
    Asteria::Boot::Bootstrap::g_ia64PciEcamRuntimeOwner = pciEcamRuntime;
    const auto pciEcamPreparation =
        pciEcamRuntime->Prepare(Asteria::Boot::Bootstrap::g_bootInfo, Asteria::Boot::Bootstrap::g_ia64AhciPciResource,
                                Asteria::Boot::Bootstrap::g_ia64CommonRuntimeProcessorCapabilities.PhysicalAddressBits,
                                Asteria::Boot::Bootstrap::g_ia64RootResourceDomainIdentity,
                                *Asteria::Boot::Bootstrap::g_ia64RootResourceDomain);
    if (pciEcamPreparation.HasValue() &&
        (pciEcamPreparation.GetValue().Availability == Asteria::Platform::Device::PciEcamAvailability::Ready))
    {
        Asteria::Boot::Bootstrap::RecordIa64PciEvidence(pciEcamPreparation.GetValue());
        if (!Asteria::Boot::Bootstrap::PublishIa64AssignableDevice(*pciEcamRuntime) ||
            !Asteria::Boot::Bootstrap::PublishIa64NicDevice(*pciEcamRuntime) ||
            !Asteria::Boot::Bootstrap::PublishIa64Scanout(*pciEcamRuntime))
        {
            Asteria::Platform::Ia64::Diagnostics::StopIa64(
                Asteria::Kernel::Diagnostics::StopCode::BootStage,
                static_cast<Asteria::Foundation::UInt64>(
                    Asteria::Platform::Ia64::Diagnostics::Ia64BootStage::DevicePublication));
        }
        Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:STAGE:PCI-ECAM\r\n");
        if (pciEcamPreparation.GetValue().AhciFunctionPresent)
        {
            Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:STAGE:PCI-DISCOVERY\r\n");
        }
        else
        {
            Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:PCI-DISCOVERY:NO-AHCI\r\n");
        }
    }
    else
    {
        Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:PCI-ECAM:UNAVAILABLE\r\n");
    }
    // This continuation executes in the aligned image copy. Constructing the
    // owner here gives every coherent queue and table page an active-copy
    // physical address.
    Asteria::Boot::Bootstrap::g_ia64Iommu = new (Asteria::Boot::Bootstrap::g_ia64IommuStorage)
        Asteria::Platform::Ia64::Device::Ia64IommuRuntime{pciEcamRuntime->GetConfigurationSpace(),
                                                          Asteria::Boot::Bootstrap::g_ia64PhysicalMemoryAccess};
    // A firmware VT-d queue may receive a wait descriptor only outside what
    // the kernel owns: the executing image copy and the run-time memory.
    const Asteria::Platform::Ia64::Device::Ia64VtdInput iommuInput{
        .BootInformation = &Asteria::Boot::Bootstrap::g_bootInfo,
        .Ownership =
            {
                .PoolStart = Asteria::Boot::Bootstrap::g_ia64LoadedImageLayout.ImageBase,
                .PoolPages = Asteria::Boot::Bootstrap::g_ia64LoadedImageLayout.ImageSize / 4096U,
                .StartupStart = Asteria::Boot::Bootstrap::g_ia64RuntimeMemoryReservation.PhysicalStart,
                .StartupPages = Asteria::Boot::Bootstrap::g_ia64RuntimeMemoryReservation.PageCount,
            },
        .PhysicalAddressBits = Asteria::Boot::Bootstrap::g_ia64CommonRuntimeProcessorCapabilities.PhysicalAddressBits,
        .Reserved = {},
    };
    // VT-d's firmware handoff precedes the boot's first DMA, as on AMD64: every other ordinary
    // endpoint stops mastering and every unit is taken over untranslated, while the boot's AHCI
    // controller keeps the bus mastering its diagnostic needs. A unit that cannot be taken over
    // leaves the boot without DMA devices.
    const Asteria::Kernel::Device::PciFunctionAddress bootStorage{
        Asteria::Boot::Bootstrap::g_ia64AhciPciResource.Segment, Asteria::Boot::Bootstrap::g_ia64AhciPciResource.Bus,
        Asteria::Boot::Bootstrap::g_ia64AhciPciResource.Device,
        Asteria::Boot::Bootstrap::g_ia64AhciPciResource.Function};
    const auto handoff = Asteria::Boot::Bootstrap::g_ia64Iommu->HandOff(
        *pciEcamRuntime, iommuInput,
        (pciEcamRuntime->GetAhciAvailability() == Asteria::Platform::Device::AhciPciAvailability::Ready) ? &bootStorage
                                                                                                         : nullptr);
    if (handoff == Asteria::Platform::Ia64::Device::Ia64VtdHandoff::Failed)
    {
        Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:IOMMU:FAIL\r\n");
        Asteria::Platform::Ia64::Diagnostics::StopIa64(
            Asteria::Kernel::Diagnostics::StopCode::BootStage,
            static_cast<Asteria::Foundation::UInt64>(
                Asteria::Platform::Ia64::Diagnostics::Ia64BootStage::DeviceBinding));
    }
    // A platform exposing PCI configuration only through legacy ports, without
    // an MCFG table, has no ECAM function this boot path can bind.
    // Consequently this path cannot read or publish an AHCI disk.
    const bool pciEcamReady = pciEcamPreparation.HasValue() && (pciEcamPreparation.GetValue().Availability ==
                                                                Asteria::Platform::Device::PciEcamAvailability::Ready);
    if (handoff == Asteria::Platform::Ia64::Device::Ia64VtdHandoff::DmaRefused)
    {
        Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:IA64-AHCI:SKIPPED:IOMMU-HANDOFF-REFUSED\r\n");
        // A verified BOOT.CFG trust entry for the controller still publishes it, without the boot's
        // own diagnostic, for its driver to run without
        if (Asteria::Boot::Bootstrap::IsIa64AhciTrustedAfterRefusedHandoff(*pciEcamRuntime) &&
            !Asteria::Boot::Bootstrap::PublishIa64AhciDevice(*pciEcamRuntime))
        {
            Asteria::Platform::Ia64::Diagnostics::StopIa64(
                Asteria::Kernel::Diagnostics::StopCode::BootStage,
                static_cast<Asteria::Foundation::UInt64>(
                    Asteria::Platform::Ia64::Diagnostics::Ia64BootStage::DevicePublication));
        }
        Asteria::Boot::Bootstrap::PublishIa64UsbHosts(*pciEcamRuntime, true);
    }
    else if (!pciEcamReady)
    {
        Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:IA64-AHCI:SKIPPED:NO-ECAM\r\n");
    }
    else
    {
        if (!Asteria::Boot::Bootstrap::RunIa64AhciIdentifyAndRead(*pciEcamRuntime))
        {
            Asteria::Platform::Ia64::Diagnostics::StopIa64(
                Asteria::Kernel::Diagnostics::StopCode::BootStage,
                static_cast<Asteria::Foundation::UInt64>(
                    Asteria::Platform::Ia64::Diagnostics::Ia64BootStage::DeviceRead));
        }
        // The diagnostic issued the boot's last AHCI command: the controller
        // is a driver's from here on, behind the IOMMU enabled next.
        if (!Asteria::Boot::Bootstrap::PublishIa64AhciDevice(*pciEcamRuntime))
        {
            Asteria::Platform::Ia64::Diagnostics::StopIa64(
                Asteria::Kernel::Diagnostics::StopCode::BootStage,
                static_cast<Asteria::Foundation::UInt64>(
                    Asteria::Platform::Ia64::Diagnostics::Ia64BootStage::DevicePublication));
        }
        Asteria::Boot::Bootstrap::PublishIa64UsbHosts(*pciEcamRuntime, false);
    }
    if (!Asteria::Boot::Bootstrap::g_ia64Iommu->Initialize(*pciEcamRuntime, iommuInput))
    {
        Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:IOMMU:FAIL\r\n");
        Asteria::Platform::Ia64::Diagnostics::StopIa64(
            Asteria::Kernel::Diagnostics::StopCode::BootStage,
            static_cast<Asteria::Foundation::UInt64>(
                Asteria::Platform::Ia64::Diagnostics::Ia64BootStage::DeviceBinding));
    }
    Asteria::Platform::Firmware::WriteDiagnosticAscii(
        Asteria::Platform::Ia64::Diagnostics::GetIa64EvidenceRecord().Iommu.Ready
            ? "ASTERIA:STAGE:IOMMU-TRANSLATION\r\n"
            : "ASTERIA:IOMMU:ABSENT\r\n");
    // The BOOT.CFG trust allowlist for the published DMA functions, once the IOMMU decided: IA-64
    // publishes the AHCI controller, the USB host controllers and the e1000e; edu is never trusted.
    {
        const Asteria::Platform::Device::TrustedDmaCandidate candidates[]{
            {Asteria::Boot::Bootstrap::g_ia64AhciDevice.Function, Asteria::Boot::BootTrustDriver::Ahci, {}},
            {Asteria::Boot::Bootstrap::g_ia64UsbHostDevices[0U].Function, Asteria::Boot::BootTrustDriver::UsbHost, {}},
            {Asteria::Boot::Bootstrap::g_ia64UsbHostDevices[1U].Function, Asteria::Boot::BootTrustDriver::UsbHost, {}},
            {Asteria::Boot::Bootstrap::g_ia64UsbHostDevices[2U].Function, Asteria::Boot::BootTrustDriver::UsbHost, {}},
            {Asteria::Boot::Bootstrap::g_ia64UsbHostDevices[3U].Function, Asteria::Boot::BootTrustDriver::UsbHost, {}},
            {Asteria::Boot::Bootstrap::g_ia64NicDevice.Function, Asteria::Boot::BootTrustDriver::E1000e, {}},
        };
        Asteria::Boot::Bootstrap::g_ia64Iommu->ApplyTrustedDma(Asteria::Boot::Bootstrap::g_ia64BspObjectRegistry,
                                                               Asteria::Boot::Bootstrap::g_bootInfo, candidates, 6U);
    }

#if ASTERIA_IA64_SYNTHETIC_USER_PROBE
    if (Asteria::Boot::Bootstrap::g_ia64UserExecutionProbeOwner == nullptr)
    {
        Asteria::Platform::Ia64::Diagnostics::StopIa64(
            Asteria::Kernel::Diagnostics::StopCode::BootStage,
            static_cast<Asteria::Foundation::UInt64>(
                Asteria::Platform::Ia64::Diagnostics::Ia64BootStage::SyntheticProbe));
    }
    const auto userProbePreparation =
        runtime->PrepareUserExecutionProbe(*Asteria::Boot::Bootstrap::g_ia64UserExecutionProbeOwner);
    if (!userProbePreparation.HasValue())
    {
        Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:IA64-PRIVATE-USER-ROOT:PREPARE:FAIL\r\n");
        Asteria::Platform::Ia64::Diagnostics::StopIa64(
            Asteria::Kernel::Diagnostics::StopCode::BootStage,
            static_cast<Asteria::Foundation::UInt64>(
                Asteria::Platform::Ia64::Diagnostics::Ia64BootStage::SyntheticProbe));
    }
    Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:STAGE:IA64-PRIVATE-USER-ROOT-PREPARED\r\n");
#else
    if ((Asteria::Boot::Bootstrap::g_ia64InitialUserImageMemoryOwner == nullptr) ||
        (Asteria::Boot::Bootstrap::g_ia64InitialUserImageMappingOwner == nullptr))
    {
        Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:INITIAL-USER-IMAGE-LAUNCH:PREPARE:FAIL\r\n");
        Asteria::Platform::Ia64::Diagnostics::StopIa64(
            Asteria::Kernel::Diagnostics::StopCode::BootStage,
            static_cast<Asteria::Foundation::UInt64>(
                Asteria::Platform::Ia64::Diagnostics::Ia64BootStage::UserPreparation));
    }
    const auto initialUserImagePreparation =
        runtime->PrepareInitialUserImage(*Asteria::Boot::Bootstrap::g_ia64InitialUserImageMemoryOwner,
                                         *Asteria::Boot::Bootstrap::g_ia64InitialUserImageMappingOwner);
    if (!initialUserImagePreparation.HasValue())
    {
        Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:INITIAL-USER-IMAGE-LAUNCH:PREPARE:FAIL\r\n");
        Asteria::Platform::Ia64::Diagnostics::StopIa64(
            Asteria::Kernel::Diagnostics::StopCode::BootStage,
            static_cast<Asteria::Foundation::UInt64>(
                Asteria::Platform::Ia64::Diagnostics::Ia64BootStage::UserPreparation));
    }
    Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:STAGE:INITIAL-USER-IMAGE-LAUNCH-PREPARED\r\n");
#endif

    Asteria::Platform::Ia64::Diagnostics::CheckIa64EarlyStopPoint();
    if (!Asteria::Boot::Bootstrap::InitializeIa64ApplicationProcessorRuntimeRelease())
    {
        Asteria::Platform::Ia64::Diagnostics::StopIa64(
            Asteria::Kernel::Diagnostics::StopCode::BootStage,
            static_cast<Asteria::Foundation::UInt64>(
                Asteria::Platform::Ia64::Diagnostics::Ia64BootStage::ApplicationProcessors));
    }
    if (!Asteria::Boot::Bootstrap::PrepareIa64ApplicationProcessorKernelRuntime())
    {
        Asteria::Platform::Ia64::Diagnostics::StopIa64(
            Asteria::Kernel::Diagnostics::StopCode::BootStage,
            static_cast<Asteria::Foundation::UInt64>(
                Asteria::Platform::Ia64::Diagnostics::Ia64BootStage::ApplicationProcessors));
    }
    runtime->BindShootdownMailbox(*Asteria::Boot::Bootstrap::g_ia64TlbShootdownMailbox);
    if (!Asteria::Boot::Bootstrap::RunIa64ApplicationProcessorKernelRuntimes())
    {
        if (Asteria::Boot::Bootstrap::g_ia64ApplicationProcessorExecutionStorageOwner.GetProcessorCount() > 1U)
        {
            Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AP-LOCAL-TIMER:FAIL\r\n");
            Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AP-SCHEDULER:FAIL\r\n");
            Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AP-PERSISTENT-SCHEDULER:FAIL\r\n");
            Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AP-TIMER-DELIVERY:FAIL\r\n");
        }
        Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AP-RUNTIME:FAIL\r\n");
        Asteria::Platform::Ia64::Diagnostics::StopIa64(
            Asteria::Kernel::Diagnostics::StopCode::BootStage,
            static_cast<Asteria::Foundation::UInt64>(
                Asteria::Platform::Ia64::Diagnostics::Ia64BootStage::ApplicationProcessors));
    }
    if (!Asteria::Boot::Bootstrap::MapIa64SharedRootShootdownPage(*runtime))
    {
        Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:IA64-SHARED-ROOT-SHOOTDOWN:FAIL\r\n");
        Asteria::Platform::Ia64::Diagnostics::StopIa64(
            Asteria::Kernel::Diagnostics::StopCode::BootStage,
            static_cast<Asteria::Foundation::UInt64>(
                Asteria::Platform::Ia64::Diagnostics::Ia64BootStage::InitialMappings));
    }

#if ASTERIA_IA64_SYNTHETIC_USER_PROBE
    const auto localTimerProbe = runtime->RunLocalTimerInterruptProbe();
    if (!localTimerProbe.HasValue())
    {
        Asteria::Platform::Ia64::Diagnostics::StopIa64(
            Asteria::Kernel::Diagnostics::StopCode::BootStage,
            static_cast<Asteria::Foundation::UInt64>(
                Asteria::Platform::Ia64::Diagnostics::Ia64BootStage::SyntheticProbe));
    }
#endif

#if ASTERIA_IA64_NATIVE_PREEMPTION_PROBE
    const auto nativePreemption = runtime->DispatchNativeExternalPreemptionProbe();
    if (!nativePreemption.HasValue())
    {
        Asteria::Platform::Ia64::Diagnostics::StopIa64(
            Asteria::Kernel::Diagnostics::StopCode::BootStage,
            static_cast<Asteria::Foundation::UInt64>(
                Asteria::Platform::Ia64::Diagnostics::Ia64BootStage::NativePreemption));
    }
    Asteria::Platform::Firmware::HaltAfterFirmwareExit();
#endif

#if ASTERIA_IA64_SYNTHETIC_USER_PROBE
    const auto dispatch = runtime->DispatchUserExecutionProbe();
    if (!dispatch.HasValue())
    {
        Asteria::Boot::Bootstrap::WriteIa64UserProbeDispatchFailureDiagnostic(dispatch.GetError(),
                                                                              runtime->GetUserExecutionProbeFailure());
        Asteria::Platform::Ia64::Diagnostics::StopIa64(
            Asteria::Kernel::Diagnostics::StopCode::BootStage,
            static_cast<Asteria::Foundation::UInt64>(
                Asteria::Platform::Ia64::Diagnostics::Ia64BootStage::SyntheticProbe));
    }
#else
    if (!Asteria::Boot::Bootstrap::PrepareIa64Pager(*runtime))
    {
        Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:IA64-PAGER:PREPARE:FAIL\r\n");
        Asteria::Platform::Ia64::Diagnostics::StopIa64(
            Asteria::Kernel::Diagnostics::StopCode::BootStage,
            static_cast<Asteria::Foundation::UInt64>(Asteria::Platform::Ia64::Diagnostics::Ia64BootStage::Pager));
    }
    // The last stage the kernel reports: a successful dispatch enters the
    // image at PL3 and does not return.
    Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:STAGE:INITIAL-USER-IMAGE-LAUNCH\r\n");
    const auto dispatch = runtime->DispatchInitialUserImage();
    if (!dispatch.HasValue())
    {
        Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:INITIAL-USER-IMAGE-LAUNCH:DISPATCH:FAIL\r\n");
        Asteria::Platform::Ia64::Diagnostics::StopIa64(
            Asteria::Kernel::Diagnostics::StopCode::BootStage,
            static_cast<Asteria::Foundation::UInt64>(
                Asteria::Platform::Ia64::Diagnostics::Ia64BootStage::UserDispatch));
    }
#endif

    Asteria::Platform::Ia64::Diagnostics::StopIa64(
        Asteria::Kernel::Diagnostics::StopCode::BootStage,
        static_cast<Asteria::Foundation::UInt64>(Asteria::Platform::Ia64::Diagnostics::Ia64BootStage::WorkerReturn));
}
#endif

extern "C" Asteria::Foundation::UInt32 AsteriaKernelPrepare(Asteria::Boot::BootInfo* const info) noexcept
{
    return info != nullptr && Asteria::Boot::Bootstrap::PrepareKernel(*info) ? 1U : 0U;
}

extern "C" [[noreturn]] void AsteriaKernelEnter(const Asteria::Boot::BootInfo* const info) noexcept
{
    if (info == nullptr)
    {
        Asteria::Platform::Firmware::HaltAfterFirmwareExit();
    }
    Asteria::Boot::Bootstrap::EnterKernel(*info);
}

extern "C" Asteria::Foundation::UInt64 AsteriaKernelQuery(const Asteria::Foundation::UInt32 selector,
                                                          const Asteria::Foundation::UInt32 index) noexcept
{
    using Asteria::Boot::KernelQuery;
    using Asteria::Foundation::UInt64;
    if (selector == static_cast<Asteria::Foundation::UInt32>(KernelQuery::EvidenceAddress))
    {
        return index == 0U ? Asteria::Boot::Bootstrap::GetKernelEvidenceAddress() : 0U;
    }
#if defined(ASTERIA_TARGET_AMD64)
    if (selector > static_cast<Asteria::Foundation::UInt32>(KernelQuery::MetadataArenaPages))
    {
        return 0U;
    }
    const KernelQuery query = static_cast<KernelQuery>(selector);
    // The arena the loader allocates for a managed span of index GiB: constant arithmetic, no
    // kernel state.
    if (query == KernelQuery::MetadataArenaPages)
    {
        return Asteria::Platform::Amd64::Bootstrap::GetMetadataArenaPages(index);
    }
    const auto& preparation = Asteria::Boot::Bootstrap::GetAmd64AddressSpacePreparation();
    const auto& failure = preparation.GetFailure();
    const auto& report = Asteria::Boot::Bootstrap::GetAmd64ProcessorStateReport();
    if (query == KernelQuery::VariableMtrrBase || query == KernelQuery::VariableMtrrMask)
    {
        if (index >= report.VariableMtrrCount ||
            index >= Asteria::Platform::Amd64::Memory::MaximumInitialVariableMtrrCount)
        {
            return 0U;
        }
        return query == KernelQuery::VariableMtrrBase ? report.VariableMtrrBase[index] : report.VariableMtrrMask[index];
    }
    if (index != 0U)
    {
        return 0U;
    }
    switch (query)
    {
    case KernelQuery::FirmwareApicId:
        return Asteria::Platform::Amd64::Bootstrap::GetAmd64FirmwareApicId();
    case KernelQuery::PagingCapabilityError:
        return static_cast<UInt64>(Asteria::Boot::Bootstrap::GetAmd64PagingCapabilityError());
    case KernelQuery::ProcessorStateCaptureError:
        return static_cast<UInt64>(Asteria::Boot::Bootstrap::GetAmd64ProcessorStateCaptureError());
    case KernelQuery::PreparationFailed:
        return preparation.HasFailed() ? 1U : 0U;
    case KernelQuery::PreparationError:
        return static_cast<UInt64>(failure.Error);
    case KernelQuery::CaptureError:
        return static_cast<UInt64>(failure.CaptureError);
    case KernelQuery::PageTableError:
        return static_cast<UInt64>(failure.PageTableError);
    case KernelQuery::PageTableBuildError:
        return static_cast<UInt64>(failure.PageTableBuildError);
    case KernelQuery::PageTableVerificationError:
        return static_cast<UInt64>(failure.PageTableVerificationError);
    case KernelQuery::MappingError:
        return static_cast<UInt64>(failure.MappingError);
    case KernelQuery::MappingIndex:
        return failure.MappingIndex;
    case KernelQuery::MtrrCapability:
        return report.MtrrCapability;
    case KernelQuery::MtrrDefaultType:
        return report.MtrrDefaultType;
    case KernelQuery::VariableMtrrCount:
        return report.VariableMtrrCount;
    case KernelQuery::RuntimeRejectionAddress:
        return reinterpret_cast<Asteria::Foundation::UIntPointer>(
            Asteria::Platform::Amd64::Bootstrap::GetAmd64BootRuntimeRejection());
    case KernelQuery::FinalMapRejectionAddress:
        return reinterpret_cast<Asteria::Foundation::UIntPointer>(
            Asteria::Boot::Bootstrap::GetAmd64FinalMemoryMapRejection());
    case KernelQuery::TableRejected:
        return Asteria::Boot::Bootstrap::IsAmd64TableRejection() ? 1U : 0U;
    case KernelQuery::TscSourcesPresent:
        return Asteria::Platform::Amd64::Time::GetTscCalibrationAttempts().Present;
    case KernelQuery::TscSourcesTried:
        return Asteria::Platform::Amd64::Time::GetTscCalibrationAttempts().Tried;
    case KernelQuery::EvidenceAddress:
    case KernelQuery::VariableMtrrBase:
    case KernelQuery::VariableMtrrMask:
    case KernelQuery::MetadataArenaPages:
        return 0U;
    }
#else
    static_cast<void>(index);
#endif
    return 0U;
}
