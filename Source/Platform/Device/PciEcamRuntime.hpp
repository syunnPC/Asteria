// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_DEVICE_PCI_ECAM_RUNTIME_HPP_INCLUDED
#define ASTERIA_PLATFORM_DEVICE_PCI_ECAM_RUNTIME_HPP_INCLUDED

#include "Abi/PagerBootstrap.hpp"
#include "Abi/ObjectCreationLimits.hpp"
#include "Boot/BootInfo.hpp"
#include "Foundation/Result.hpp"
#include "Kernel/Device/AhciHbaRegisterSnapshotReader.hpp"
#include "Kernel/Device/AhciIdentifyCommandExecutor.hpp"
#include "Kernel/Device/AhciPciBootstrapBinding.hpp"
#include "Kernel/Device/AhciReadDmaExtCommandExecutor.hpp"
#include "Kernel/Device/DeviceManager.hpp"
#include "Kernel/Device/PciEcamConfigurationSpace.hpp"
#include "Kernel/Device/PciEndpointBarDecoder.hpp"
#include "Kernel/Object/CapabilityKeyDerivation.hpp"
#include "Kernel/Object/CapabilitySpace.hpp"
#include "Kernel/Object/RevocationRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Kernel/Time/IMonotonicClock.hpp"
#include "Platform/Acpi/Device/AcpiPciEcamTopologyAdapter.hpp"
#include "Platform/Device/IPhysicalMemoryAccess.hpp"
#include "Platform/Device/PhysicalMemoryMappedIo.hpp"
#include "Platform/Firmware/PciResource.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Platform::Device
{
/// The revocation roots this runtime's registry holds at most: the ECAM window's and one per device
/// it publishes (the assignable device, AHCI, the NIC, the scanout, and every xHCI and USB host
/// controller). The boot pager bounds them with the boot's roots and the
/// FaultChannels' (the boot runtimes assert the sum).
inline constexpr Foundation::Size PciEcamRevocationRoots{1U + 4U + Abi::PagerXhciControllers +
                                                         Abi::PagerUsbHostControllers};
/// The token roots of the pool of four FaultChannels, derived from the same bound as
/// its pool and evidence record.
inline constexpr Foundation::Size FaultChannelRevocationRoots{Abi::FaultChannelMaximumCount};

enum class PciEcamRuntimeState : Foundation::UInt8
{
    Uninitialized,
    Unavailable,
    Ready,
    Failed,
};

enum class PciEcamAvailability : Foundation::UInt8
{
    None,
    Absent,
    InvalidOptionalTopology,
    UnsupportedPhysicalRange,
    NoFunctionPresent,
    Ready,
};

enum class PciEcamRuntimeError : Foundation::UInt8
{
    None,
    AlreadyPrepared,
    InvalidConfiguration,
    TopologyRejected,
    RevocationRejected,
    RegionRejected,
    CapabilityCloseRejected,
    MemoryMappedIoRejected,
    ConfigurationSpaceRejected,
    CatalogDiscoveryRejected,
    FunctionCapacityExceeded,
    CatalogBindRejected,
    FunctionPublicationRejected,
    /// An assignable device (PublishAssignableDevice) is not a published
    /// function, decodes no matching 32-bit memory BAR, or does not decode
    /// memory.
    DeviceRejected,
};

/// The registry identities of an assignable device's function and of the DeviceRegion of its memory
/// BAR.
struct AssignableDevice
{
    Kernel::Object::ObjectId Function;
    Kernel::Object::ObjectId Region;
};

enum class PciEcamFunctionQueryError : Foundation::UInt8
{
    None,
    NotReady,
    IndexOutOfRange,
};

enum class AhciPciAvailability : Foundation::UInt8
{
    NotEvaluated,
    EcamUnavailable,
    Absent,
    Failed,
    Ready,
};

enum class AhciPciRuntimeError : Foundation::UInt8
{
    None,
    EcamPreparationFailed,
    EcamUnavailable,
    InvalidFirmwareResource,
    InvalidPhysicalAddressWidth,
    PhysicalRangeRejected,
    CatalogNotReady,
    CatalogQueryRejected,
    NoCatalogAhciController,
    AmbiguousCatalogAhciController,
    FirmwareBdfMismatch,
    CommandReadRejected,
    MemorySpaceDisabled,
    BusMasterDisabled,
    AbarPreflightRejected,
    AbarKindRejected,
    AbarPrefetchable,
    AbarBaseMismatch,
    RegionRejected,
    CapabilityCloseRejected,
    MemoryMappedIoRejected,
    BindingRejected,
    BindingQueryRejected,
    BindingResultMismatch,
};

enum class AhciPciRuntimeQueryError : Foundation::UInt8
{
    None,
    NotReady,
    Unavailable,
    Failed,
};

enum class AhciIdentifyRuntimeState : Foundation::UInt8
{
    NotAttempted,
    Succeeded,
    Failed,
};

enum class AhciIdentifyRuntimeError : Foundation::UInt8
{
    None,
    AlreadyAttempted,
    AhciNotReady,
    RegionAuthorityRejected,
    CommandRejected,
};

enum class AhciIdentifyRuntimeQueryError : Foundation::UInt8
{
    None,
    NotReady,
    Failed,
};

enum class AhciReadDmaExtRuntimeState : Foundation::UInt8
{
    NotAttempted,
    Succeeded,
    Failed,
};

enum class AhciReadDmaExtRuntimeError : Foundation::UInt8
{
    None,
    AlreadyAttempted,
    IdentifyNotSucceeded,
    RegionAuthorityRejected,
    CommandRejected,
};

enum class AhciReadDmaExtRuntimeQueryError : Foundation::UInt8
{
    None,
    NotReady,
    Failed,
};

struct PciFunctionPublication
{
    Kernel::Device::PciFunctionDescriptor Descriptor;
    Kernel::Device::PciFunctionId Function;
    Kernel::Object::CapabilityHandle Capability;
    Kernel::Object::CapabilitySpaceIdentifier CapabilitySpace;
};

struct PciEcamRuntimeSummary
{
    Foundation::Size RegionCount;
    Foundation::Size BusCount;
    Foundation::Size DiscoveredFunctionCount;
    Foundation::Size PublishedFunctionCount;
    Foundation::UInt64 ConfigurationReadCount;
    Foundation::UInt32 InitialVendorDevice;
    Kernel::Device::PciFunctionAddress InitialFunction;
    Kernel::Device::PciFunctionDescriptor AhciFunction;
    PciEcamAvailability Availability;
    bool InitialFunctionPresent;
    bool AhciFunctionPresent;
    Foundation::UInt8 Reserved[3];
};

struct PciEcamRuntimeFailure
{
    Kernel::Device::DeviceManagerFailure DeviceManager;
    Kernel::Device::PciFunctionManagerFailure PciFunctionManager;
    Kernel::Device::PciFunctionCatalogFailure Catalog;
    Foundation::Size PublishedFunctionCount;
    Platform::Acpi::Device::AcpiPciEcamTopologyError Topology;
    Kernel::Object::RevocationError Revocation;
    Kernel::Object::CapabilityError Capability;
    PhysicalMemoryMappedIoInitializationError MemoryMappedIo;
    Kernel::Device::PciEcamConfigurationSpaceInitializationError ConfigurationSpace;
    Kernel::Device::PciFunctionManagerError CatalogBinding;
    PciEcamRuntimeError Error;
};

struct AhciPciRuntimeFailure
{
    Kernel::Device::DeviceManagerFailure DeviceManager;
    Kernel::Device::PciFunctionCatalogFailure Catalog;
    Kernel::Device::PciEndpointBarDecodeFailure Abar;
    Kernel::Device::AhciPciBootstrapBindingFailure Binding;
    Foundation::Size MatchingFunctionCount;
    Foundation::UInt32 CommandRegister;
    Hal::Device::PciConfigurationReadError ConfigurationRead;
    Kernel::Object::CapabilityError Capability;
    PhysicalMemoryMappedIoInitializationError MemoryMappedIo;
    AhciPciRuntimeError Error;
};

struct AhciPciRuntimeResult
{
    Kernel::Device::AhciPciBootstrapBindingResult Binding;
    Kernel::Device::DeviceRegionDescriptor Region;
};

struct AhciIdentifyRuntimeFailure
{
    Kernel::Device::AhciIdentifyCommandFailure Command;
    AhciIdentifyRuntimeError Error;
    Foundation::UInt8 Reserved[7];
};

struct AhciReadDmaExtRuntimeFailure
{
    Kernel::Device::AhciReadDmaExtExecutionFailure Command;
    AhciReadDmaExtRuntimeError Error;
    Foundation::UInt8 Reserved[7];
};

/// Retains BSP-lifetime ownership and checked access for ACPI-described ECAM and the independently
/// authorized firmware-described AHCI ABAR. DeviceRegion objects carry read/write authority for byte, word and DWORD access, while the public configuration interface
/// exposes only checked DWORD reads. Region capabilities are closed after creation. Each discovered
/// PciFunction capability remains live in this runtime's private CapabilitySpace; copying its
/// handle does not make it usable in any other space.
/// Lifetime and serialization: - This object is placement-constructed in immovable boot-lifetime
/// storage and is intentionally non-destructible. A failed Prepare may already retain registered
/// DeviceRegion objects, so failure does not shorten its lifetime. - registrySynchronization is the
/// exact synchronization object used by registry, issuer is bound to registry, and all three
/// objects outlive this runtime and every registered object retained by its embedded manager. -
/// Every ResourceDomain supplied to Prepare, its identity, ledger, registry registration, and
/// backing synchronization outlive this runtime after any DeviceRegion creation succeeds. -
/// Construction and Prepare are BSP-bootstrap-only, never execute in an ISR, and are externally
/// serialized against every operation using the supplied registry synchronization. The caller
/// prevents same-CPU preemption and re-entry for those operations. Read32 begins only after Prepare
/// returns; the embedded readers serialize concurrent calls through their own locks. - Summary,
/// function-publication, and PCI-level AHCI queries begin only after Prepare returns. They are
/// immutable and may then run concurrently with one another. ECAM readiness is independent of AHCI
/// absence or failure. - AHCI publication requires the PCI Command register to have both Memory
/// Space Enable and Bus Master Enable set. This runtime observes but does not modify PCI
/// configuration state; the configuration space it exposes writes only for its caller
/// (GetConfigurationSpace). - The ECAM backend owns only the ACPI ECAM regions. The ABAR backend
/// owns exactly one separate, read/write, DWORD-only, uncached region after AHCI preflight
/// succeeds; that region is retained after any later failure. - ExecuteAhciIdentify and
/// ExecuteAhciReadDmaExt are serialized through one lock and are each one-shot bootstrap
/// operations. Both derive the exact ABAR from the immutable binding and do not expose the MMIO
/// writer. READ additionally derives its selected port, device capacity, logical-sector size, and
/// LBA48 support only from the successful IDENTIFY receipt. - Callers own every coherent
/// controller-visible DMA span and retain the exact span indefinitely whenever its stored command
/// failure requires it.
class PciEcamRuntime final
{
public:
    /// CapabilityValidationKey is this boot's DeviceSpace key (DeriveCapabilityValidationKey),
    /// never a constant.
    PciEcamRuntime(Kernel::Object::ObjectRegistry& registry,
                   Platform::Synchronization::SpinLock& registrySynchronization,
                   Kernel::Object::RootCapabilityIssuer& issuer,
                   const Kernel::Object::CapabilityValidationKey& capabilityValidationKey,
                   const IPhysicalMemoryAccess& physicalMemoryAccess) noexcept;
    ~PciEcamRuntime() noexcept = delete;

    PciEcamRuntime(const PciEcamRuntime&) = delete;
    PciEcamRuntime& operator=(const PciEcamRuntime&) = delete;
    PciEcamRuntime(PciEcamRuntime&&) = delete;
    PciEcamRuntime& operator=(PciEcamRuntime&&) = delete;

    [[nodiscard]] Foundation::Result<PciEcamRuntimeSummary, PciEcamRuntimeError>
    Prepare(const Boot::BootInfo& bootInfo, const Platform::Firmware::AhciPciResource& ahciResource,
            Foundation::UInt8 physicalAddressBits, Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
            Kernel::Resource::ResourceDomain& resourceDomain) noexcept;

    /// Publishes the captured BAR of the published function at resource's address as a read-write,
    /// uncached DeviceRegion charged to resourceDomain, once the BAR decodes to exactly that 32-bit
    /// memory range and the function decodes memory; bus mastering may be off. The region's own
    /// capability closes at once: the boot's issuer mints what it grants. Prepared runtimes only.
    [[nodiscard]] Foundation::Result<AssignableDevice, PciEcamRuntimeError>
    PublishAssignableDevice(const Platform::Firmware::AhciPciResource& resource, Foundation::UInt8 physicalAddressBits,
                            Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
                            Kernel::Resource::ResourceDomain& resourceDomain) noexcept;

    /// A USB 1.1/2.0 host controller (class 0C/03, programming interface 00
    /// UHCI, 10 OHCI, 20 EHCI) at address, published for the USB host group
    /// driver: its function and
    /// a read-write, uncached DeviceRegion of its registers. An EHCI's or
    /// OHCI's memory BAR 0 is firmware's captured extent where given, else
    /// sized as CaptureAssignableResource sizes one; its region covers the
    /// whole pages that hold the BAR, so a driver can map it, and a BAR
    /// smaller than a page is published only when no other function's memory
    /// BAR starts in that page, so the mapping exposes no other device. A
    /// UHCI's I/O BAR 4 becomes a 32-byte port I/O region (UHCI 1.1 section
    /// 2.1) once the function decodes I/O. INTx is disabled until a driver's
    /// Interrupt enables it. DeviceRejected for another function or class, a
    /// BAR not decoded, or a shared page; the function's own restriction
    /// (bridge, display, firmware DMA) is the caller's to check. Boot only,
    /// after Prepare and before anything maps the BAR.
    [[nodiscard, gnu::noinline]] Foundation::Result<AssignableDevice, PciEcamRuntimeError>
    PublishUsbHostDevice(Kernel::Device::PciFunctionAddress address,
                         const Platform::Firmware::AhciPciResource* firmware, Foundation::UInt8 physicalAddressBits,
                         Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
                         Kernel::Resource::ResourceDomain& resourceDomain) noexcept;

    /// Whether a user driver may be assigned the active function at address
    /// (no bridge, display or firmware DMA restriction).
    [[nodiscard]] bool IsAssignable(Kernel::Device::PciFunctionAddress address) noexcept
    {
        return m_deviceManager.GetAssignmentRestriction(address) == Kernel::Device::PciFunctionError::None;
    }

    /// The boot's framebuffer of bytes at base as a read-write, cachePolicy (WC permits UC
    /// fallback) DeviceRegion charged to resourceDomain, for the System Display Broker's scanout:
    /// its registry identity. The region's own capability closes at once: the boot's issuer mints
    /// what it grants. DeviceRejected for a range that is not page-aligned, not implemented by the
    /// physical address width, or overlaps another region. Boot only, after Prepare.
    [[nodiscard]] Foundation::Result<Kernel::Object::ObjectId, PciEcamRuntimeError> PublishScanoutRegion(
        Foundation::UInt64 base, Foundation::UInt64 bytes, Foundation::UInt8 physicalAddressBits,
        Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
        Kernel::Resource::ResourceDomain& resourceDomain,
        Kernel::Memory::MemoryCachePolicy cachePolicy = Kernel::Memory::MemoryCachePolicy::WriteCombining) noexcept;

    /// The AHCI function the runtime bound at resource's address and the DeviceRegion of its ABAR,
    /// as an assignable device: the region is the one the boot's own commands used, whose
    /// capability closed at its creation, so the boot's issuer mints what it grants. DeviceRejected
    /// unless the binding is Ready and its function is published. Boot only, after the boot's last
    /// AHCI command: the runtime never touches the controller again.
    [[nodiscard]] Foundation::Result<AssignableDevice, PciEcamRuntimeError>
    PublishAhciDevice(const Platform::Firmware::AhciPciResource& resource) const noexcept;

    /// The resource of the first published function vendorDevice names (configuration dword 0: the
    /// vendor in the low half, the device in the high) whose 32-bit memory BAR barIndex the
    /// platform assigned, its extent sized through configuration space where no firmware reports
    /// it, for PublishAssignableDevice. Its INTx is disabled meanwhile: nothing takes it until its
    /// interrupt is assigned. DeviceRejected when no published function matches or its BAR cannot
    /// be sized. Boot only, after Prepare and before anything maps the BAR.
    [[nodiscard, gnu::noinline]] Foundation::Result<Platform::Firmware::AhciPciResource, PciEcamRuntimeError>
    CaptureAssignableResource(Foundation::UInt32 vendorDevice, Foundation::UInt8 barIndex) noexcept;
    /// Capture one catalog endpoint by address, including a 64-bit BAR. The
    /// firmware DMA sweep must have stopped bus mastering before this call.
    [[nodiscard]] Foundation::Result<Platform::Firmware::AhciPciResource, PciEcamRuntimeError>
    CaptureAssignableResource(Kernel::Device::PciFunctionAddress address, Foundation::UInt8 barIndex) noexcept;

    /// The interrupt line (configuration byte 0x3C: the GSI the platform routes its INTx to) of the
    /// published function at address, which must use an interrupt pin; every other published
    /// function routed to the same line gets INTx Disable, so only the device can assert it.
    /// DeviceRejected for a function without one. Boot only, after Prepare.
    [[nodiscard, gnu::noinline]] Foundation::Result<Foundation::UInt8, PciEcamRuntimeError>
    IsolateInterruptLine(Kernel::Device::PciFunctionAddress address) noexcept;

    [[nodiscard]] Hal::Device::PciConfigurationReadError Read32(Kernel::Device::PciFunctionAddress address,
                                                                Foundation::UInt16 registerOffset,
                                                                Foundation::UInt32& output) noexcept;

    /// The ECAM configuration space as a run-time configuration reader and writer for
    /// PciFunctionControl on a platform without configuration ports. Normal context only, after
    /// Prepare; a write goes through the ECAM DeviceRegions' checks.
    [[nodiscard]] Kernel::Device::PciEcamConfigurationSpace& GetConfigurationSpace() noexcept
    {
        return m_configurationSpace;
    }

    /// Copies the completed immutable summary. Failure leaves output unchanged.
    [[nodiscard]] PciEcamFunctionQueryError GetSummary(PciEcamRuntimeSummary& output) const noexcept;

    /// Copies one descriptor and exact published authority identity. The handle
    /// is authenticated only for CapabilitySpace and cannot be used directly
    /// by a distinct driver capability space. It must be transferred through a controlled API. Failure leaves output unchanged.
    [[nodiscard]] PciEcamFunctionQueryError GetPublishedFunction(Foundation::Size index,
                                                                 PciFunctionPublication& output) const noexcept;

    /// Boot only, before assigning the captured function.
    [[nodiscard]] bool ReserveFirmwareDma(Kernel::Device::PciFunctionAddress address) noexcept
    {
        return m_deviceManager.ReserveFirmwareDma(address);
    }

    [[nodiscard]] PciEcamRuntimeState GetState() const noexcept;
    [[nodiscard]] PciEcamRuntimeFailure GetFailure() const noexcept;

    [[nodiscard]] AhciPciAvailability GetAhciAvailability() const noexcept;
    [[nodiscard]] AhciPciRuntimeFailure GetAhciFailure() const noexcept;

    /// Copies the immutable AHCI binding only after complete publication.
    /// Every other availability leaves output unchanged.
    [[nodiscard]] AhciPciRuntimeQueryError GetAhciResult(AhciPciRuntimeResult& output) const noexcept;

    /// Executes one polling IDENTIFY transaction against the bound controller.
    /// The clock and DMA page remain caller-owned for the complete call. Calls
    /// are forbidden from ISR context and same-CPU re-entry. Every call after
    /// the first attempt returns AlreadyAttempted together with the original
    /// command failure, preserving its DMA-page lifetime requirement.
    [[nodiscard]] AhciIdentifyRuntimeFailure
    ExecuteAhciIdentify(const Hal::Time::IMonotonicClock& clock, Kernel::Device::AhciDmaPageWritableView dmaPage,
                        Kernel::Device::AhciIdentifyPollingPolicy polling) noexcept;

    /// Copies the immutable successful receipt. Failure leaves output
    /// unchanged.
    [[nodiscard]] AhciIdentifyRuntimeQueryError
    GetAhciIdentifyReceipt(Kernel::Device::AhciIdentifyCommandReceipt& output) const noexcept;

    [[nodiscard]] AhciIdentifyRuntimeState GetAhciIdentifyState() const noexcept;
    [[nodiscard]] AhciIdentifyRuntimeFailure GetAhciIdentifyFailure() const noexcept;

    /// Executes one polling READ DMA EXT transaction against the disk proven
    /// by the successful IDENTIFY transaction. The caller supplies only the
    /// exact DMA spans, LBA/count, polling policy, and clock; controller and
    /// device facts are derived internally. Every call after the first READ
    /// attempt reports AlreadyAttempted with the original executor failure,
    /// preserving both executor lifetime flags exactly.
    [[nodiscard]] AhciReadDmaExtRuntimeFailure
    ExecuteAhciReadDmaExt(const Hal::Time::IMonotonicClock& clock,
                          Kernel::Device::AhciReadDmaExtCommandPageWritableView commandPage,
                          Kernel::Device::AhciReadDmaExtDataBufferSpan dataBuffer, Foundation::UInt64 startingLba,
                          Foundation::UInt64 sectorCount, Kernel::Device::AhciReadDmaExtPollingPolicy polling) noexcept;

    /// Copies the immutable successful READ receipt. Failure leaves output
    /// unchanged.
    [[nodiscard]] AhciReadDmaExtRuntimeQueryError
    GetAhciReadDmaExtReceipt(Kernel::Device::AhciReadDmaExtExecutionReceipt& output) const noexcept;

    [[nodiscard]] AhciReadDmaExtRuntimeState GetAhciReadDmaExtState() const noexcept;
    [[nodiscard]] AhciReadDmaExtRuntimeFailure GetAhciReadDmaExtFailure() const noexcept;

private:
    class BootLifetimeClock final : public Kernel::Time::IMonotonicClock
    {
    public:
        [[nodiscard]] Foundation::UInt64 GetTicks() const noexcept override
        {
            return 0U;
        }
    };

    using PreparationResult = Foundation::Result<PciEcamRuntimeSummary, PciEcamRuntimeError>;

    [[nodiscard]] PreparationResult PublishUnavailable(PciEcamAvailability availability) noexcept;
    [[nodiscard]] PreparationResult Fail(PciEcamRuntimeError error) noexcept;
    [[nodiscard]] static Foundation::UInt64 GetApertureLength(const Kernel::Device::PciEcamRegion& region) noexcept;
    [[nodiscard]] PreparationResult
    DiscoverAndPublishFunctions(Kernel::Object::RevocationDomainId revocationDomain,
                                Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
                                Kernel::Resource::ResourceDomain& resourceDomain) noexcept;
    [[nodiscard]] PreparationResult
    PublishReadySummary(const Kernel::Device::PciFunctionCatalogSnapshot& catalog) noexcept;
    void PrepareAhci(const Platform::Firmware::AhciPciResource& resource, Foundation::UInt8 physicalAddressBits,
                     Kernel::Object::RevocationDomainId revocationDomain,
                     Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
                     Kernel::Resource::ResourceDomain& resourceDomain) noexcept;
    /// The region of PublishAssignableDevice, once its function is known:
    /// memory-mapped, or with portIo the port range of a UHCI's I/O BAR.
    [[nodiscard, gnu::noinline]] Foundation::Result<AssignableDevice, PciEcamRuntimeError>
    CreateAssignableRegion(const Platform::Firmware::AhciPciResource& resource, Kernel::Object::ObjectId function,
                           Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
                           Kernel::Resource::ResourceDomain& resourceDomain, bool portIo = false) noexcept;
    /// The published function at resource's address once resource's memory
    /// BAR decodes exactly as resource says and the function decodes memory.
    [[nodiscard]] const PciFunctionPublication* ValidateMemoryBar(const Platform::Firmware::AhciPciResource& resource,
                                                                  Foundation::UInt8 physicalAddressBits) noexcept;
    /// Whether some published function other than the BAR barIndex of
    /// address has a memory BAR starting in [start, start + bytes).
    [[nodiscard, gnu::noinline]] bool OtherMemoryBarIn(Kernel::Device::PciFunctionAddress address,
                                                       Foundation::UInt8 barIndex, Foundation::UInt64 start,
                                                       Foundation::UInt64 bytes) noexcept;
    [[nodiscard]] bool CreateAhciRegion(const Platform::Firmware::AhciPciResource& resource,
                                        Foundation::UInt8 physicalAddressBits,
                                        Kernel::Object::RevocationDomainId revocationDomain,
                                        Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
                                        Kernel::Resource::ResourceDomain& resourceDomain) noexcept;
    void BindAhci(const Platform::Firmware::AhciPciResource& resource,
                  Foundation::UInt64 maximumPhysicalAddress) noexcept;
    void PublishAhciFailure(AhciPciRuntimeError error,
                            AhciPciAvailability availability = AhciPciAvailability::Failed) noexcept;
    [[nodiscard]] bool SelectCatalogAhci(Kernel::Device::PciFunctionDescriptor& output) noexcept;
    [[nodiscard]] static bool FirmwareResourceIsValid(const Platform::Firmware::AhciPciResource& resource) noexcept;
    [[nodiscard]] static Kernel::Device::PciFunctionAddress
    GetAddress(const Platform::Firmware::AhciPciResource& resource) noexcept;
    [[nodiscard]] static Foundation::UInt64 GetMaximumPhysicalAddress(Foundation::UInt8 physicalAddressBits) noexcept;
    [[nodiscard]] static bool BindingMatchesResource(const Kernel::Device::AhciPciBootstrapBindingResult& binding,
                                                     const Platform::Firmware::AhciPciResource& resource) noexcept;
    [[nodiscard]] static bool IsAhciFunction(const Kernel::Device::PciFunctionDescriptor& descriptor) noexcept;
    [[nodiscard]] static AhciIdentifyRuntimeFailure MakeAhciIdentifyFailure(AhciIdentifyRuntimeError error) noexcept;
    [[nodiscard]] static AhciIdentifyRuntimeFailure
    MakeAhciIdentifyFailure(AhciIdentifyRuntimeError error,
                            const Kernel::Device::AhciIdentifyCommandFailure& command) noexcept;
    [[nodiscard]] static AhciReadDmaExtRuntimeFailure
    MakeAhciReadDmaExtFailure(AhciReadDmaExtRuntimeError error) noexcept;
    [[nodiscard]] static AhciReadDmaExtRuntimeFailure
    MakeAhciReadDmaExtFailure(AhciReadDmaExtRuntimeError error,
                              const Kernel::Device::AhciReadDmaExtExecutionFailure& command) noexcept;
    [[nodiscard]] bool HasWritableAhciRegion() const noexcept;

    static constexpr Foundation::Size MaximumRegions{Kernel::Device::PciEcamConfigurationSpace::MaximumRegions};

    Kernel::Object::ObjectRegistry& m_registry;
    const IPhysicalMemoryAccess& m_physicalMemoryAccess;
    BootLifetimeClock m_clock;
    Kernel::Object::RevocationRegistry m_revocations;
    Kernel::Object::CapabilitySpace m_capabilitySpace;
    // PCI publications can fill all 256 slots. Temporary region authority
    // has a separate bounded space so AHCI/BAR publication still succeeds.
    Kernel::Object::CapabilitySpace m_regionCapabilitySpace;
    Kernel::Device::DeviceManager m_deviceManager;
    PhysicalMemoryMappedIo m_ecamMemoryMappedIo;
    Kernel::Device::PciEcamConfigurationSpace m_configurationSpace;
    Kernel::Device::PciFunctionCatalog m_catalog;
    PhysicalMemoryMappedIo m_abarMemoryMappedIo;
    Kernel::Device::PciEndpointBarDecoder m_barDecoder;
    Kernel::Device::AhciHbaRegisterSnapshotReader m_ahciSnapshotReader;
    Kernel::Device::AhciPciBootstrapBinding m_ahciBinding;
    Kernel::Device::DeviceRegionCreation m_creations[MaximumRegions]{};
    Kernel::Device::DeviceRegion* m_regions[MaximumRegions]{};
    Kernel::Device::DeviceRegionCreation m_abarCreation{};
    PciFunctionPublication m_functionPublications[Kernel::Device::DeviceManager::MaximumPciFunctions]{};
    Platform::Acpi::Device::AcpiPciEcamTopology m_topology{};
    PciEcamRuntimeSummary m_summary{};
    PciEcamRuntimeFailure m_failure{};
    AhciPciRuntimeResult m_ahciResult{};
    AhciPciRuntimeFailure m_ahciFailure{};
    mutable Platform::Synchronization::SpinLock m_ahciCommandSynchronization;
    Kernel::Device::AhciIdentifyCommandReceipt m_ahciIdentifyReceipt{};
    AhciIdentifyRuntimeFailure m_ahciIdentifyFailure{};
    Kernel::Device::AhciReadDmaExtExecutionReceipt m_ahciReadDmaExtReceipt{};
    AhciReadDmaExtRuntimeFailure m_ahciReadDmaExtFailure{};
    Foundation::Size m_regionCount{0U};
    Foundation::Size m_functionCount{0U};
    PciEcamRuntimeState m_state{PciEcamRuntimeState::Uninitialized};
    AhciPciAvailability m_ahciAvailability{AhciPciAvailability::NotEvaluated};
    AhciIdentifyRuntimeState m_ahciIdentifyState{AhciIdentifyRuntimeState::NotAttempted};
    AhciReadDmaExtRuntimeState m_ahciReadDmaExtState{AhciReadDmaExtRuntimeState::NotAttempted};
};

static_assert(sizeof(PciEcamRuntimeState) == 1U);
static_assert(sizeof(PciEcamAvailability) == 1U);
static_assert(sizeof(PciEcamRuntimeError) == 1U);
static_assert(sizeof(PciEcamFunctionQueryError) == 1U);
static_assert(sizeof(AhciPciAvailability) == 1U);
static_assert(sizeof(AhciPciRuntimeError) == 1U);
static_assert(sizeof(AhciPciRuntimeQueryError) == 1U);
static_assert(sizeof(AhciIdentifyRuntimeState) == 1U);
static_assert(sizeof(AhciIdentifyRuntimeError) == 1U);
static_assert(sizeof(AhciIdentifyRuntimeQueryError) == 1U);
static_assert(sizeof(AhciReadDmaExtRuntimeState) == 1U);
static_assert(sizeof(AhciReadDmaExtRuntimeError) == 1U);
static_assert(sizeof(AhciReadDmaExtRuntimeQueryError) == 1U);
static_assert(sizeof(PciFunctionPublication) == 56U);
static_assert(alignof(PciFunctionPublication) == 8U);
static_assert(sizeof(PciEcamRuntimeSummary) == 72U);
static_assert(alignof(PciEcamRuntimeSummary) == 8U);
static_assert(sizeof(AhciPciRuntimeResult) == 104U);
static_assert(alignof(AhciPciRuntimeResult) == 8U);
static_assert(sizeof(AhciIdentifyRuntimeFailure) == 64U);
static_assert(alignof(AhciIdentifyRuntimeFailure) == 8U);
static_assert(sizeof(AhciReadDmaExtRuntimeFailure) == 64U);
static_assert(alignof(AhciReadDmaExtRuntimeFailure) == 8U);
} // Namespace Asteria::Platform::Device

#endif // ASTERIA_PLATFORM_DEVICE_PCI_ECAM_RUNTIME_HPP_INCLUDED
