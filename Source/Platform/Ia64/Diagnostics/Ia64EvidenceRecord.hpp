// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_DIAGNOSTICS_IA64_EVIDENCE_RECORD_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_DIAGNOSTICS_IA64_EVIDENCE_RECORD_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Diagnostics/ApplicationLaunchEvidence.hpp"
#include "Kernel/Diagnostics/Watchdog.hpp"
#include "Kernel/Time/ProcessorClock.hpp"
#include "Kernel/Diagnostics/UserProcessorEvidence.hpp"
#include "Kernel/Memory/PhysicalPageImportEvidence.hpp"
#include "Boot/BootFiles.hpp"
#include "Platform/Ia64/Diagnostics/Ia64Stop.hpp"
#include "Kernel/Diagnostics/EvidenceRecord.hpp"
#include "Kernel/Scheduler/SchedulerTypes.hpp"
#include "Platform/Ia64/Interrupt/Ia64ExternalInterruptEntry.hpp"
#include "Platform/Ia64/Scheduler/Ia64BspKernelRuntime.hpp"

namespace Asteria::Platform::Ia64::Diagnostics
{
/// Layout version of Ia64EvidenceRecord (EvidenceRecordHeader::Schema). Increment it with every
/// change of a type below.
inline constexpr Foundation::UInt32 Ia64EvidenceRecordSchema{32U};
/// Per-processor slots; slot n holds processor n, and the BSP's slot 0 of the
/// application-processor evidence stays empty.
inline constexpr Foundation::Size Ia64EvidenceProcessorSlots{4U};
/// The ATA IDENTIFY DEVICE data block (256 words) the AHCI evidence retains.
inline constexpr Foundation::Size Ia64EvidenceIdentifyDataBytes{512U};

static_assert(Ia64EvidenceProcessorSlots == Kernel::Scheduler::MaximumProcessorCount);

/// How far the boot path's scripted AHCI transfer got (Ia64AhciEvidence::Outcome).
enum class Ia64AhciEvidenceOutcome : Foundation::UInt64
{
    None = 0U,
    NoDisk = 1U,
    Identified = 2U,
    Read = 3U,
};

/// The processors that completed the SAL rendezvous and joined the runtime
/// profile.
struct Ia64ProcessorEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 ProcessorCount;
    Foundation::UInt64 EnabledProcessorMask;
};

/// The PCI ECAM scan: its first function and the published function count,
/// and the function the boot path bound as the AHCI controller. Vendor and
/// device identifiers are packed as `device << 16 | vendor`.
struct Ia64PciEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 EcamReady;
    Foundation::UInt64 FirstFunctionPresent;
    Foundation::UInt64 FirstSegment;
    Foundation::UInt64 FirstBus;
    Foundation::UInt64 FirstDevice;
    Foundation::UInt64 FirstFunction;
    Foundation::UInt64 FirstVendorDevice;
    Foundation::UInt64 PublishedFunctionCount;
    Foundation::UInt64 AhciPresent;
    Foundation::UInt64 AhciSegment;
    Foundation::UInt64 AhciBus;
    Foundation::UInt64 AhciDevice;
    Foundation::UInt64 AhciFunction;
    Foundation::UInt64 AhciVendorDevice;
    Foundation::UInt64 AhciClassCode;
    Foundation::UInt64 AhciSubclass;
    Foundation::UInt64 AhciProgrammingInterface;
};

/// The boot path's AHCI IDENTIFY DEVICE and its one READ DMA EXT. The kernel
/// checks the receipts against its own requests (API contract) and records
/// the raw IDENTIFY data and where the read landed; it does not look at the
/// sector's contents. ReadBufferPhysicalAddress names the DMA buffer, which
/// keeps the transferred bytes.
struct Ia64AhciEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 Outcome;
    Foundation::UInt64 PortNumber;
    Foundation::UInt64 ReadStartingLba;
    Foundation::UInt64 ReadSectorCount;
    Foundation::UInt64 ReadTransferSizeBytes;
    Foundation::UInt64 ReadBufferPhysicalAddress;
    Foundation::UInt64 ReadBufferSizeBytes;
    Foundation::UInt8 IdentifyData[Ia64EvidenceIdentifyDataBytes];
};

/// One application processor's runtime scenario as the BSP observed it: the
/// AP's first local-timer interrupt, its first cooperative block/wake/resume
/// cycle, one persistent-scheduler service request, and one deferred timer
/// delivery. Sequences, counts, run sequences, cookies and deadlines are
/// recorded as observed and not compared with the values the scenario
/// expects; interrupt records are copied before the BSP acknowledges them,
/// and each acknowledgement's result is recorded separately.
struct Ia64ApplicationProcessorEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 Processor;
    Foundation::UInt64 Generation;
    Foundation::UInt64 LocalTimerVector;
    Foundation::UInt64 LocalTimerTaskPriority;
    Foundation::UInt64 LocalTimerDeadlineTicks;
    Foundation::UInt64 LocalTimerPollDeadlineTicks;
    Foundation::UInt64 LocalTimerPollCodeStart;
    Foundation::UInt64 LocalTimerPollCodeEnd;
    Interrupt::Ia64LocalTimerInterruptEvidence LocalTimerInterrupt;
    Foundation::UInt64 LocalTimerAcknowledgedSequence;
    Foundation::UInt64 InitialRunSequence;
    Foundation::UInt64 FirstResumedRunSequence;
    Foundation::UInt64 TimerQueueScheduledEntries;
    Foundation::UInt64 TimerQueuePendingEntries;
    Foundation::UInt64 TimerQueueClaimedEntries;
    Foundation::UInt64 RunLoopBindingCount;
    Foundation::UInt64 ServiceCookie;
    Foundation::UInt64 ServiceRequestSequence;
    Foundation::UInt64 ServiceCompletionSequence;
    Foundation::UInt64 ServiceRequestCookie;
    Foundation::UInt64 ServiceCompletionCookie;
    Foundation::UInt64 ServiceCompletionValue;
    Foundation::UInt64 ResumedWorkerStackPointer;
    Foundation::UInt64 ServiceCompletionRunSequence;
    Foundation::UInt64 ServiceDispatchCount;
    Foundation::UInt64 ServiceWakeCount;
    Foundation::UInt64 ServiceCount;
    Foundation::UInt64 DeliveryRequestSequence;
    Foundation::UInt64 DeliveryCompletionSequence;
    Foundation::UInt64 DeliveryRegistrationSequence;
    Foundation::UInt64 DeliveryArmSequence;
    Foundation::UInt64 DeliveryArmedNextClaimSequence;
    Foundation::UInt64 DeliveryArmedScheduledEntries;
    Foundation::UInt64 DeliveryDeadlineTicks;
    Foundation::UInt64 DeliveryPollDeadlineTicks;
    Foundation::UInt64 DeliveryDrainedCount;
    Foundation::UInt64 DeliveryFirstClaimSequence;
    Foundation::UInt64 DeliveryLastClaimSequence;
    Foundation::UInt64 DeliveryClaimSequence;
    Foundation::UInt64 DeliveryNotificationClaimSequence;
    Foundation::UInt64 DeliveryFinalNextClaimSequence;
    Foundation::UInt64 DeliveryFinalRegistrationSequence;
    Foundation::UInt64 DeliveryFinalRegisteredBindings;
    Foundation::UInt64 DeliveryFinalSchedulerNextSequence;
    Foundation::UInt64 DeliveryFinalThreadRunSequence;
    Foundation::UInt64 DeliveryFinalRunLoopBindingCount;
    Interrupt::Ia64LocalTimerInterruptEvidence DeliveryInterrupt;
    Foundation::UInt64 DeliveryAcknowledgedSequence;
};

/// Every application processor's runtime scenario. Published is set after
/// the release coordinator reports every AP serving; ServingProcessorMask is
/// the coordinator's online mask at that point.
struct Ia64ApplicationProcessorRuntimeEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 ProcessorCount;
    Foundation::UInt64 ServingProcessorMask;
    Ia64ApplicationProcessorEvidence Processors[Ia64EvidenceProcessorSlots];
};

/// The shared-root TLB shootdown: the one supervisor page mapped into the
/// shared kernel root after every profiled processor serves, and which
/// processors the shootdown targeted, queued and heard acknowledge.
struct Ia64SharedRootShootdownEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 VirtualStart;
    Foundation::UInt64 Pages;
    Foundation::UInt64 PhysicalStart;
    Foundation::UInt64 ShootdownSequence;
    Foundation::UInt64 MappingSequence;
    Foundation::UInt64 TargetProcessorMask;
    Foundation::UInt64 QueuedProcessorMask;
    Foundation::UInt64 AcknowledgedProcessorMask;
};

/// The synthetic user probe's kernel-captured timer evidence: the BSP's first
/// local-timer interrupt (after the BSP acknowledged it) with the TimerQueue
/// transaction it drove, and the interrupt that resumed the probe's
/// WaitSetWait (before its acknowledgement). Each part has its own
/// publication word.
struct Ia64UserProbeEvidence
{
    Foundation::UInt64 LocalTimerPublished;
    Interrupt::Ia64LocalTimerInterruptEvidence LocalTimerInterrupt;
    Scheduler::Ia64BspKernelRuntimeTimerQueueEvidence TimerQueue;
    Foundation::UInt64 WaitTimerPublished;
    Interrupt::Ia64LocalTimerInterruptEvidence WaitTimerInterrupt;
};

/// The one-shot BSP native-preemption proof: the runtime's retained evidence
/// (Sequence is its release publication), the external-preemption control's
/// final capture and restore counts, and the exit code of the worker whose
/// exit ended the proof.
struct Ia64NativePreemptionEvidence
{
    Foundation::UInt64 Published;
    Scheduler::Ia64BspNativePreemptionEvidence Runtime;
    Foundation::UInt64 ControlCaptureCount;
    Foundation::UInt64 ControlRestoreCount;
    Foundation::UInt64 WorkerExitCode;
};

/// Boot user domains whose preemptions are counted, in this slot order: the
/// initial image, the boot pager and the spinner.
inline constexpr Foundation::Size Ia64EvidenceUserDomains{3U};

/// The PL3 timer ticks the boot threads took and, per boot domain (slot order as
/// Ia64EvidenceUserDomains), the runs such a tick ended Ready. Then the spinner, which only the
/// test firmware has: whether this boot has one, the lines it wrote, and whether its final WaitSet
/// wait suspended. Published once the boot threads are admitted and again after every change; a
/// synthetic-probe or native-preemption build runs no boot thread and leaves it unpublished.
struct Ia64UserPreemptionEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 UserTicks;
    Foundation::UInt64 RunStops[Ia64EvidenceUserDomains];
    Foundation::UInt64 SpinnerPresent;
    Foundation::UInt64 SpinnerLines;
    Foundation::UInt64 SpinnerWaitSuspended;
};

/// Object management: the Endpoints the boot pager created, destroyed and the kernel reclaimed, the
/// Endpoint pool's occupied (active or quiescing) slots, the current kernel-metadata and IPC-queue
/// charge of the pager's management grant, the charge of one Endpoint, and the registered objects
/// once the grant was created; then the Frames and MemoryObjects created and reclaimed, the charge
/// of one of each, the grant's committed and resident pages, the frame pool's free and dirty pages,
/// the mappings MemoryMap completed with the last one's virtual and physical start and pages in the
/// pager's own space, and the mappings MemoryUnmap removed; and the BSP's local translation-cache
/// purges, which a root's activation runs after it unmapped (Ia64RuntimeAddressSpacePageTable.md).
/// Published with the grant and again at every wait of the pager, whose management calls all
/// precede its waits.
struct Ia64ObjectManagementEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 Created;
    Foundation::UInt64 Destroyed;
    Foundation::UInt64 Reclaimed;
    Foundation::UInt64 OccupiedSlots;
    Foundation::UInt64 MetadataBytes;
    Foundation::UInt64 IpcQueueBytes;
    Foundation::UInt64 EndpointMetadataBytes;
    Foundation::UInt64 RegisteredObjects;
    Foundation::UInt64 FramesCreated;
    Foundation::UInt64 FramesReclaimed;
    Foundation::UInt64 MemoryObjectsCreated;
    Foundation::UInt64 MemoryObjectsReclaimed;
    Foundation::UInt64 FrameMetadataBytes;
    Foundation::UInt64 MemoryObjectMetadataBytes;
    Foundation::UInt64 CommittedPages;
    Foundation::UInt64 ResidentPages;
    Foundation::UInt64 AllocatorCleanPages;
    Foundation::UInt64 AllocatorDirtyPages;
    Foundation::UInt64 Mappings;
    Foundation::UInt64 MappedVirtualStart;
    Foundation::UInt64 MappedPhysicalStart;
    Foundation::UInt64 MappedPages;
    Foundation::UInt64 Unmappings;
    Foundation::UInt64 TranslationCachePurges;
};

/// Created threads: the threads the pager created, the ones its ObjectDestroy reclaimed and the
/// thread-pool slots the kernel freed; the ThreadExit calls that ended a run and the slots the
/// reaper freed; the exit code ObjectDestroy last returned; the pool's and the table's free slots
/// and the grant's Threads usage; the PL3 ticks that ended a created thread's run; one thread's
/// kernel-metadata charge; the page faults of created threads and those deferred behind another;
/// the threads ThreadTerminate killed; the faults that were PL3 exceptions; and the initial
/// thread's faults deferred behind another and the most consecutive deferrals one fault of the
/// initial thread or a created thread had, which no bound limits. Published with the pager's
/// management evidence and at every created thread's run stop.
struct Ia64ThreadEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 Created;
    Foundation::UInt64 Destroyed;
    Foundation::UInt64 Reclaimed;
    Foundation::UInt64 Exits;
    Foundation::UInt64 Reaped;
    Foundation::UInt64 LastExitCode;
    Foundation::UInt64 FreeSlots;
    Foundation::UInt64 FreeNativeSlots;
    Foundation::UInt64 ThreadsUsage;
    Foundation::UInt64 RunStops;
    Foundation::UInt64 SlotMetadataBytes;
    Foundation::UInt64 Faults;
    Foundation::UInt64 Deferrals;
    Foundation::UInt64 Kills;
    Foundation::UInt64 Exceptions;
    Foundation::UInt64 InitialDeferrals;
    Foundation::UInt64 LongestDeferral;
};

/// The IA-64 evidence record. Sub-records a build or a boot does not reach stay zero (unpublished).
/// Created domains: the AddressSpaces, CapabilitySpaces and ProtectionDomains the pager created and
/// the kernel reclaimed, and each pool's free slots; the run-time AddressSpace slots' preparations,
/// releases and spent slots, and the RID allocator's AddressSpace count (the boot's three roots and
/// every incarnation); the domains the thread table recorded and retired. Published with the
/// pager's management evidence.
struct Ia64DomainEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 AddressSpacesCreated;
    Foundation::UInt64 AddressSpacesReclaimed;
    Foundation::UInt64 CapabilitySpacesCreated;
    Foundation::UInt64 CapabilitySpacesReclaimed;
    Foundation::UInt64 DomainsCreated;
    Foundation::UInt64 DomainsReclaimed;
    Foundation::UInt64 FreeAddressSpaceSlots;
    Foundation::UInt64 FreeCapabilitySpaceSlots;
    Foundation::UInt64 FreeDomainSlots;
    Foundation::UInt64 SlotsPrepared;
    Foundation::UInt64 SlotsReleased;
    Foundation::UInt64 SlotsSpent;
    Foundation::UInt64 AllocatedAddressSpaces;
    Foundation::UInt64 DomainsRecorded;
    Foundation::UInt64 DomainsRetired;
};

/// The root service's components: the ITC ticks of one microsecond the boot measured across its
/// firmware stall, which convert the pager's wait timeouts (zero: none), and the pager's waits that
/// timed out, its watchdog's expiries; the faults of threads in created domains and the tag of the
/// last, and their ThreadExits that woke the supervisor; then the components' ResourceDomains
/// created, destroyed (drained) and reclaimed, and those still occupying a slot. Published with the
/// pager's management evidence.
struct Ia64ComponentEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 ItcTicksPerMicrosecond;
    Foundation::UInt64 WaitExpiries;
    Foundation::UInt64 TaggedFaults;
    Foundation::UInt64 LastTag;
    Foundation::UInt64 DomainExits;
    Foundation::UInt64 ResourceDomainsCreated;
    Foundation::UInt64 ResourceDomainsDestroyed;
    Foundation::UInt64 ResourceDomainsReclaimed;
    Foundation::UInt64 ResourceDomainsLive;
};

/// What the object manager's collections did: the pool passes, the slots they reclaimed, and the
/// dirty frame-pool runs scrubbed and their pages. Written with the object management evidence.
struct Ia64CollectionEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 Collections;
    Foundation::UInt64 Reclaimed;
    Foundation::UInt64 ScrubRuns;
    Foundation::UInt64 ScrubbedPages;
};

/// The run-time control of the assigned PCI function, written with the object management evidence:
/// the assignments the Device Manager made and the releases, the bus-master requests refused (there
/// is no IOMMU), and the functions a drain released into quarantine and the quarantines the Device
/// Manager lifted once it found the device quiet.
struct Ia64DeviceControlEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 Assignments;
    Foundation::UInt64 Releases;
    Foundation::UInt64 BusMasterRefusals;
    Foundation::UInt64 Quarantines;
    Foundation::UInt64 Confirmations;
};

/// Created threads' waits, written with the object management evidence: the WaitSets created and
/// reclaimed, the Endpoints WaitSetBind bound, and the WaitSetWait calls of created threads that
/// blocked, whose deadline passed, that ThreadTerminate aborted, and the threads that then ended as
/// they resumed.
struct Ia64ServiceEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 WaitSetsCreated;
    Foundation::UInt64 WaitSetsReclaimed;
    Foundation::UInt64 Binds;
    Foundation::UInt64 WaitsBlocked;
    Foundation::UInt64 WaitExpiries;
    Foundation::UInt64 WaitAborts;
    Foundation::UInt64 AbortedExits;
};

/// The assigned device's Interrupts, written with the object management evidence: the Interrupts
/// drivers created, destroyed (by ObjectDestroy or a drain) and reclaimed, those still in the pool,
/// and the INTx deliveries that reached a bound vector and an unbound one.
struct Ia64DeviceInterruptEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 Created;
    Foundation::UInt64 Destroyed;
    Foundation::UInt64 Reclaimed;
    Foundation::UInt64 Live;
    Foundation::UInt64 Delivered;
    Foundation::UInt64 Spurious;
};

/// Boot translation state and the confined edu driver's cumulative fault.
struct Ia64IommuEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 Present;
    Foundation::UInt64 Requester;
    Foundation::UInt64 SweptFunctions;
    Foundation::UInt64 Ready;
    Foundation::UInt64 Bypass;
    Foundation::UInt64 QueuePhysical;
    Foundation::UInt64 QueuePages;
    Foundation::UInt64 DomainsCreated;
    Foundation::UInt64 DomainsReclaimed;
    Foundation::UInt64 Faults;
    Foundation::UInt64 LastFaultSource;
    Foundation::UInt64 LastFaultReason;
    Foundation::UInt64 LastFaultAddress;
};

/// One parked Frame pin a Device Manager's confirmation released: the Frame's registry identity,
/// first page and pages, and whether the scrub that zeroed that page afterwards sampled it, with
/// its first words as the scrub found them.
struct Ia64ParkedReleaseEvidence
{
    Foundation::UInt64 Frame;
    Foundation::UInt64 PhysicalStart;
    Foundation::UInt64 Pages;
    Foundation::UInt64 Sampled;
    Foundation::UInt64 Sample[4];
};

/// The block I/O evidence, written with the object management evidence: the Frame pins drained
/// IommuDomains parked with quarantined functions in all, released by confirmations and still
/// parked, the releases the record counted, and the last two by turns.
struct Ia64BlockEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 PinsParked;
    Foundation::UInt64 PinsReleased;
    Foundation::UInt64 ParkedPins;
    Foundation::UInt64 Releases;
    Ia64ParkedReleaseEvidence Released[2];
};

/// One DRHD in DMAR order (AMD64's Amd64IommuUnitEvidence vocabulary).
/// State: 0 untouched or refused, 1 taken over and kept untranslated,
/// 2 translating, 3 failed. DisabledReason: 0 translation requested,
/// 3 refusal, 4 no function behind a scoped unit. TableFlushes counts the
/// table ranges written back with fc (zero on a unit that snoops, ECAP.C = 1).
struct Ia64VtdUnitEvidence
{
    Foundation::UInt64 RegisterBase;
    Foundation::UInt64 State;
    Foundation::UInt64 DisabledReason;
    Foundation::UInt64 ConfigureError;
    Foundation::UInt64 EnableError;
    Foundation::UInt64 Version;
    Foundation::UInt64 Capability;
    Foundation::UInt64 ExtendedCapability;
    Foundation::UInt64 HandoffStatus;
    Foundation::UInt64 Status;
    Foundation::UInt64 Levels;
    Foundation::UInt64 ServedFunctions;
    Foundation::UInt64 Faults;
    Foundation::UInt64 Overflows;
    Foundation::UInt64 Unattributed;
    Foundation::UInt64 TableFlushes;
};

/// The slabs in the runtime page, appended in schema 25: where they begin, their bytes and the
/// bitmap budget after them, and the capacities attached there.
struct Ia64MetadataEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 SlabPhysicalStart;
    Foundation::UInt64 SlabBytes;
    Foundation::UInt64 BitmapBudgetBytes;
    Foundation::UInt64 RegistryCapacity;
    Foundation::UInt64 FrameSlots;
    Foundation::UInt64 MemoryObjectSlots;
};

/// The backend the boot chose: Backend 0 none, 1 VT-d (a DMAR names a remapping unit), 2
/// virtio-iommu. The remaining fields describe VT-d: the topology refusal (VtdTopologyError), the
/// address width that bounds register sets (the smaller of the DMAR host address width and the PAL
/// physical address size), the DRHD count, the endpoints swept master-off, the table pages in the
/// executing image and the direction of the last fault. The shared Iommu record keeps readiness,
/// domains and the last fault for either backend.
struct Ia64IommuBackendEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 Backend;
    Foundation::UInt64 SelectionError;
    Foundation::UInt64 HostAddressBits;
    Foundation::UInt64 UnitCount;
    Foundation::UInt64 SweptFunctions;
    Foundation::UInt64 TablePhysical;
    Foundation::UInt64 TablePages;
    Foundation::UInt64 LastFaultRead;
    Ia64VtdUnitEvidence Units[4U];
};

/// Physical-page allocator accounting, import, zeroing and handoff evidence.
struct Ia64AllocatorEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 ManagedPages;
    Foundation::UInt64 AllocatablePages;
    Foundation::UInt64 CleanPages;
    Foundation::UInt64 DirtyPages;
    Foundation::UInt64 ClaimedPages;
    Foundation::UInt64 RetiredPages;
    Foundation::UInt64 AllocatedPages;
    Foundation::UInt64 UnmanagedPages;
    Foundation::UInt64 FreeLowBlocks;
    Foundation::UInt64 ExcludedDmaPages;
    Foundation::UInt64 ExcludedLoaderPages;
    Foundation::UInt64 ExcludedFirmwarePages;
    Foundation::UInt64 BootServicesImported;
    Foundation::UInt64 ImportRefusal;
    Foundation::UInt64 ZeroingSlots;
    Foundation::UInt64 PeakZeroingSlots;
    Foundation::UInt64 ZeroDeferrals;
    Foundation::UInt64 ZeroFaults;
    Foundation::UInt64 CreationZeroedPages;
    Foundation::UInt64 CreationZeroRoot;
    Foundation::UInt64 HandoffRecords;
};

struct Ia64EvidenceRecord
{
    Kernel::Diagnostics::EvidenceRecordHeader Header;
    Ia64ProcessorEvidence Processors;
    Ia64PciEvidence Pci;
    Ia64AhciEvidence Ahci;
    Ia64ApplicationProcessorRuntimeEvidence ApplicationProcessors;
    Ia64SharedRootShootdownEvidence SharedRootShootdown;
    Ia64UserProbeEvidence UserProbe;
    Ia64NativePreemptionEvidence NativePreemption;
    Ia64UserPreemptionEvidence UserPreemption;
    Ia64ObjectManagementEvidence ObjectManagement;
    Ia64ThreadEvidence Threads;
    Ia64DomainEvidence Domains;
    Ia64ComponentEvidence Components;
    Ia64CollectionEvidence Collection;
    Ia64DeviceControlEvidence DeviceControl;
    Ia64ServiceEvidence Services;
    Ia64DeviceInterruptEvidence Interrupts;
    Ia64StopsEvidence Stops;
    Ia64IommuEvidence Iommu;
    Boot::BootFileRegion BootFiles;
    Ia64BlockEvidence Block;
    Ia64IommuBackendEvidence IommuBackend;
    Ia64MetadataEvidence Metadata;
    Ia64AllocatorEvidence Allocator;
    Kernel::Memory::PhysicalPageImportEvidence MemoryImport;
    Kernel::Diagnostics::ApplicationLaunchEvidence ApplicationLaunch;
    Kernel::Diagnostics::WatchdogEvidence Watchdog;
    Kernel::Time::ProcessorClockEvidence Clocks;
    Kernel::Diagnostics::UserProcessorEvidence UserProcessors;
};

/// The boot-lifetime record. The BSP is its only writer; every writer runs
/// serialized on the BSP with interruption delivery disabled.
[[nodiscard]] Ia64EvidenceRecord& GetIa64EvidenceRecord() noexcept;

/// Zero-fills the record, writes its header and returns its address (the
/// identity-mapped physical address the host reads).
[[nodiscard]] Foundation::UInt64 InitializeIa64EvidenceRecord() noexcept;

/// Makes one sub-record visible: every preceding store to it happens before
/// `published` becomes one.
void PublishIa64Evidence(Foundation::UInt64& published) noexcept;

// Layout (the host decodes the record from these assertions).
static_assert(__is_standard_layout(Ia64EvidenceRecord));
static_assert(__is_trivially_copyable(Ia64EvidenceRecord));
static_assert(alignof(Ia64EvidenceRecord) == 8U);
static_assert(__builtin_offsetof(Ia64ProcessorEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Ia64ProcessorEvidence, ProcessorCount) == 8U);
static_assert(__builtin_offsetof(Ia64ProcessorEvidence, EnabledProcessorMask) == 16U);
static_assert(sizeof(Ia64ProcessorEvidence) == 24U);
static_assert(__builtin_offsetof(Ia64PciEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Ia64PciEvidence, EcamReady) == 8U);
static_assert(__builtin_offsetof(Ia64PciEvidence, FirstFunctionPresent) == 16U);
static_assert(__builtin_offsetof(Ia64PciEvidence, FirstSegment) == 24U);
static_assert(__builtin_offsetof(Ia64PciEvidence, FirstBus) == 32U);
static_assert(__builtin_offsetof(Ia64PciEvidence, FirstDevice) == 40U);
static_assert(__builtin_offsetof(Ia64PciEvidence, FirstFunction) == 48U);
static_assert(__builtin_offsetof(Ia64PciEvidence, FirstVendorDevice) == 56U);
static_assert(__builtin_offsetof(Ia64PciEvidence, PublishedFunctionCount) == 64U);
static_assert(__builtin_offsetof(Ia64PciEvidence, AhciPresent) == 72U);
static_assert(__builtin_offsetof(Ia64PciEvidence, AhciSegment) == 80U);
static_assert(__builtin_offsetof(Ia64PciEvidence, AhciBus) == 88U);
static_assert(__builtin_offsetof(Ia64PciEvidence, AhciDevice) == 96U);
static_assert(__builtin_offsetof(Ia64PciEvidence, AhciFunction) == 104U);
static_assert(__builtin_offsetof(Ia64PciEvidence, AhciVendorDevice) == 112U);
static_assert(__builtin_offsetof(Ia64PciEvidence, AhciClassCode) == 120U);
static_assert(__builtin_offsetof(Ia64PciEvidence, AhciSubclass) == 128U);
static_assert(__builtin_offsetof(Ia64PciEvidence, AhciProgrammingInterface) == 136U);
static_assert(sizeof(Ia64PciEvidence) == 144U);
static_assert(__builtin_offsetof(Ia64AhciEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Ia64AhciEvidence, Outcome) == 8U);
static_assert(__builtin_offsetof(Ia64AhciEvidence, PortNumber) == 16U);
static_assert(__builtin_offsetof(Ia64AhciEvidence, ReadStartingLba) == 24U);
static_assert(__builtin_offsetof(Ia64AhciEvidence, ReadSectorCount) == 32U);
static_assert(__builtin_offsetof(Ia64AhciEvidence, ReadTransferSizeBytes) == 40U);
static_assert(__builtin_offsetof(Ia64AhciEvidence, ReadBufferPhysicalAddress) == 48U);
static_assert(__builtin_offsetof(Ia64AhciEvidence, ReadBufferSizeBytes) == 56U);
static_assert(__builtin_offsetof(Ia64AhciEvidence, IdentifyData) == 64U);
static_assert(sizeof(Ia64AhciEvidence) == 576U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, Processor) == 8U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, Generation) == 16U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, LocalTimerVector) == 24U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, LocalTimerTaskPriority) == 32U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, LocalTimerDeadlineTicks) == 40U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, LocalTimerPollDeadlineTicks) == 48U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, LocalTimerPollCodeStart) == 56U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, LocalTimerPollCodeEnd) == 64U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, LocalTimerInterrupt) == 72U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, LocalTimerAcknowledgedSequence) == 136U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, InitialRunSequence) == 144U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, FirstResumedRunSequence) == 152U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, TimerQueueScheduledEntries) == 160U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, TimerQueuePendingEntries) == 168U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, TimerQueueClaimedEntries) == 176U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, RunLoopBindingCount) == 184U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, ServiceCookie) == 192U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, ServiceRequestSequence) == 200U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, ServiceCompletionSequence) == 208U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, ServiceRequestCookie) == 216U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, ServiceCompletionCookie) == 224U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, ServiceCompletionValue) == 232U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, ResumedWorkerStackPointer) == 240U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, ServiceCompletionRunSequence) == 248U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, ServiceDispatchCount) == 256U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, ServiceWakeCount) == 264U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, ServiceCount) == 272U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, DeliveryRequestSequence) == 280U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, DeliveryCompletionSequence) == 288U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, DeliveryRegistrationSequence) == 296U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, DeliveryArmSequence) == 304U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, DeliveryArmedNextClaimSequence) == 312U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, DeliveryArmedScheduledEntries) == 320U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, DeliveryDeadlineTicks) == 328U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, DeliveryPollDeadlineTicks) == 336U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, DeliveryDrainedCount) == 344U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, DeliveryFirstClaimSequence) == 352U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, DeliveryLastClaimSequence) == 360U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, DeliveryClaimSequence) == 368U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, DeliveryNotificationClaimSequence) == 376U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, DeliveryFinalNextClaimSequence) == 384U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, DeliveryFinalRegistrationSequence) == 392U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, DeliveryFinalRegisteredBindings) == 400U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, DeliveryFinalSchedulerNextSequence) == 408U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, DeliveryFinalThreadRunSequence) == 416U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, DeliveryFinalRunLoopBindingCount) == 424U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, DeliveryInterrupt) == 432U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorEvidence, DeliveryAcknowledgedSequence) == 496U);
static_assert(sizeof(Ia64ApplicationProcessorEvidence) == 504U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeEvidence, ProcessorCount) == 8U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeEvidence, ServingProcessorMask) == 16U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeEvidence, Processors) == 24U);
static_assert(sizeof(Ia64ApplicationProcessorRuntimeEvidence) == 2040U);
static_assert(__builtin_offsetof(Ia64SharedRootShootdownEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Ia64SharedRootShootdownEvidence, VirtualStart) == 8U);
static_assert(__builtin_offsetof(Ia64SharedRootShootdownEvidence, Pages) == 16U);
static_assert(__builtin_offsetof(Ia64SharedRootShootdownEvidence, PhysicalStart) == 24U);
static_assert(__builtin_offsetof(Ia64SharedRootShootdownEvidence, ShootdownSequence) == 32U);
static_assert(__builtin_offsetof(Ia64SharedRootShootdownEvidence, MappingSequence) == 40U);
static_assert(__builtin_offsetof(Ia64SharedRootShootdownEvidence, TargetProcessorMask) == 48U);
static_assert(__builtin_offsetof(Ia64SharedRootShootdownEvidence, QueuedProcessorMask) == 56U);
static_assert(__builtin_offsetof(Ia64SharedRootShootdownEvidence, AcknowledgedProcessorMask) == 64U);
static_assert(sizeof(Ia64SharedRootShootdownEvidence) == 72U);
static_assert(__builtin_offsetof(Ia64UserProbeEvidence, LocalTimerPublished) == 0U);
static_assert(__builtin_offsetof(Ia64UserProbeEvidence, LocalTimerInterrupt) == 8U);
static_assert(__builtin_offsetof(Ia64UserProbeEvidence, TimerQueue) == 72U);
static_assert(__builtin_offsetof(Ia64UserProbeEvidence, WaitTimerPublished) == 152U);
static_assert(__builtin_offsetof(Ia64UserProbeEvidence, WaitTimerInterrupt) == 160U);
static_assert(sizeof(Ia64UserProbeEvidence) == 224U);
static_assert(__builtin_offsetof(Ia64NativePreemptionEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Ia64NativePreemptionEvidence, Runtime) == 8U);
static_assert(__builtin_offsetof(Ia64NativePreemptionEvidence, ControlCaptureCount) == 104U);
static_assert(__builtin_offsetof(Ia64NativePreemptionEvidence, ControlRestoreCount) == 112U);
static_assert(__builtin_offsetof(Ia64NativePreemptionEvidence, WorkerExitCode) == 120U);
static_assert(sizeof(Ia64NativePreemptionEvidence) == 128U);
static_assert(__builtin_offsetof(Ia64EvidenceRecord, Header) == 0U);
static_assert(__builtin_offsetof(Ia64EvidenceRecord, Processors) == 16U);
static_assert(__builtin_offsetof(Ia64EvidenceRecord, Pci) == 40U);
static_assert(__builtin_offsetof(Ia64EvidenceRecord, Ahci) == 184U);
static_assert(__builtin_offsetof(Ia64EvidenceRecord, ApplicationProcessors) == 760U);
static_assert(__builtin_offsetof(Ia64EvidenceRecord, SharedRootShootdown) == 2800U);
static_assert(__builtin_offsetof(Ia64EvidenceRecord, UserProbe) == 2872U);
static_assert(__builtin_offsetof(Ia64UserPreemptionEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Ia64UserPreemptionEvidence, UserTicks) == 8U);
static_assert(__builtin_offsetof(Ia64UserPreemptionEvidence, RunStops) == 16U);
static_assert(__builtin_offsetof(Ia64UserPreemptionEvidence, SpinnerPresent) == 40U);
static_assert(__builtin_offsetof(Ia64UserPreemptionEvidence, SpinnerLines) == 48U);
static_assert(__builtin_offsetof(Ia64UserPreemptionEvidence, SpinnerWaitSuspended) == 56U);
static_assert(sizeof(Ia64UserPreemptionEvidence) == 64U);
static_assert(__builtin_offsetof(Ia64EvidenceRecord, NativePreemption) == 3096U);
static_assert(__builtin_offsetof(Ia64EvidenceRecord, UserPreemption) == 3224U);
static_assert(__builtin_offsetof(Ia64ObjectManagementEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Ia64ObjectManagementEvidence, Created) == 8U);
static_assert(__builtin_offsetof(Ia64ObjectManagementEvidence, Destroyed) == 16U);
static_assert(__builtin_offsetof(Ia64ObjectManagementEvidence, Reclaimed) == 24U);
static_assert(__builtin_offsetof(Ia64ObjectManagementEvidence, OccupiedSlots) == 32U);
static_assert(__builtin_offsetof(Ia64ObjectManagementEvidence, MetadataBytes) == 40U);
static_assert(__builtin_offsetof(Ia64ObjectManagementEvidence, IpcQueueBytes) == 48U);
static_assert(__builtin_offsetof(Ia64ObjectManagementEvidence, EndpointMetadataBytes) == 56U);
static_assert(__builtin_offsetof(Ia64ObjectManagementEvidence, RegisteredObjects) == 64U);
static_assert(__builtin_offsetof(Ia64ObjectManagementEvidence, FramesCreated) == 72U);
static_assert(__builtin_offsetof(Ia64ObjectManagementEvidence, FramesReclaimed) == 80U);
static_assert(__builtin_offsetof(Ia64ObjectManagementEvidence, MemoryObjectsCreated) == 88U);
static_assert(__builtin_offsetof(Ia64ObjectManagementEvidence, MemoryObjectsReclaimed) == 96U);
static_assert(__builtin_offsetof(Ia64ObjectManagementEvidence, FrameMetadataBytes) == 104U);
static_assert(__builtin_offsetof(Ia64ObjectManagementEvidence, MemoryObjectMetadataBytes) == 112U);
static_assert(__builtin_offsetof(Ia64ObjectManagementEvidence, CommittedPages) == 120U);
static_assert(__builtin_offsetof(Ia64ObjectManagementEvidence, ResidentPages) == 128U);
static_assert(__builtin_offsetof(Ia64ObjectManagementEvidence, AllocatorCleanPages) == 136U);
static_assert(__builtin_offsetof(Ia64ObjectManagementEvidence, AllocatorDirtyPages) == 144U);
static_assert(__builtin_offsetof(Ia64ObjectManagementEvidence, Mappings) == 152U);
static_assert(__builtin_offsetof(Ia64ObjectManagementEvidence, MappedVirtualStart) == 160U);
static_assert(__builtin_offsetof(Ia64ObjectManagementEvidence, MappedPhysicalStart) == 168U);
static_assert(__builtin_offsetof(Ia64ObjectManagementEvidence, MappedPages) == 176U);
static_assert(__builtin_offsetof(Ia64ObjectManagementEvidence, Unmappings) == 184U);
static_assert(__builtin_offsetof(Ia64ObjectManagementEvidence, TranslationCachePurges) == 192U);
static_assert(sizeof(Ia64ObjectManagementEvidence) == 200U);
static_assert(__builtin_offsetof(Ia64EvidenceRecord, ObjectManagement) == 3288U);
static_assert(__builtin_offsetof(Ia64ThreadEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Ia64ThreadEvidence, Created) == 8U);
static_assert(__builtin_offsetof(Ia64ThreadEvidence, Destroyed) == 16U);
static_assert(__builtin_offsetof(Ia64ThreadEvidence, Reclaimed) == 24U);
static_assert(__builtin_offsetof(Ia64ThreadEvidence, Exits) == 32U);
static_assert(__builtin_offsetof(Ia64ThreadEvidence, Reaped) == 40U);
static_assert(__builtin_offsetof(Ia64ThreadEvidence, LastExitCode) == 48U);
static_assert(__builtin_offsetof(Ia64ThreadEvidence, FreeSlots) == 56U);
static_assert(__builtin_offsetof(Ia64ThreadEvidence, FreeNativeSlots) == 64U);
static_assert(__builtin_offsetof(Ia64ThreadEvidence, ThreadsUsage) == 72U);
static_assert(__builtin_offsetof(Ia64ThreadEvidence, RunStops) == 80U);
static_assert(__builtin_offsetof(Ia64ThreadEvidence, SlotMetadataBytes) == 88U);
static_assert(__builtin_offsetof(Ia64ThreadEvidence, Faults) == 96U);
static_assert(__builtin_offsetof(Ia64ThreadEvidence, Deferrals) == 104U);
static_assert(__builtin_offsetof(Ia64ThreadEvidence, Kills) == 112U);
static_assert(__builtin_offsetof(Ia64ThreadEvidence, Exceptions) == 120U);
static_assert(__builtin_offsetof(Ia64ThreadEvidence, InitialDeferrals) == 128U);
static_assert(__builtin_offsetof(Ia64ThreadEvidence, LongestDeferral) == 136U);
static_assert(sizeof(Ia64ThreadEvidence) == 144U);
static_assert(__builtin_offsetof(Ia64EvidenceRecord, Threads) == 3488U);
static_assert(__builtin_offsetof(Ia64DomainEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Ia64DomainEvidence, AddressSpacesCreated) == 8U);
static_assert(__builtin_offsetof(Ia64DomainEvidence, AddressSpacesReclaimed) == 16U);
static_assert(__builtin_offsetof(Ia64DomainEvidence, CapabilitySpacesCreated) == 24U);
static_assert(__builtin_offsetof(Ia64DomainEvidence, CapabilitySpacesReclaimed) == 32U);
static_assert(__builtin_offsetof(Ia64DomainEvidence, DomainsCreated) == 40U);
static_assert(__builtin_offsetof(Ia64DomainEvidence, DomainsReclaimed) == 48U);
static_assert(__builtin_offsetof(Ia64DomainEvidence, FreeAddressSpaceSlots) == 56U);
static_assert(__builtin_offsetof(Ia64DomainEvidence, FreeCapabilitySpaceSlots) == 64U);
static_assert(__builtin_offsetof(Ia64DomainEvidence, FreeDomainSlots) == 72U);
static_assert(__builtin_offsetof(Ia64DomainEvidence, SlotsPrepared) == 80U);
static_assert(__builtin_offsetof(Ia64DomainEvidence, SlotsReleased) == 88U);
static_assert(__builtin_offsetof(Ia64DomainEvidence, SlotsSpent) == 96U);
static_assert(__builtin_offsetof(Ia64DomainEvidence, AllocatedAddressSpaces) == 104U);
static_assert(__builtin_offsetof(Ia64DomainEvidence, DomainsRecorded) == 112U);
static_assert(__builtin_offsetof(Ia64DomainEvidence, DomainsRetired) == 120U);
static_assert(sizeof(Ia64DomainEvidence) == 128U);
static_assert(__builtin_offsetof(Ia64EvidenceRecord, Domains) == 3632U);
static_assert(__builtin_offsetof(Ia64ComponentEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Ia64ComponentEvidence, ItcTicksPerMicrosecond) == 8U);
static_assert(__builtin_offsetof(Ia64ComponentEvidence, WaitExpiries) == 16U);
static_assert(__builtin_offsetof(Ia64ComponentEvidence, TaggedFaults) == 24U);
static_assert(__builtin_offsetof(Ia64ComponentEvidence, LastTag) == 32U);
static_assert(__builtin_offsetof(Ia64ComponentEvidence, DomainExits) == 40U);
static_assert(__builtin_offsetof(Ia64ComponentEvidence, ResourceDomainsCreated) == 48U);
static_assert(__builtin_offsetof(Ia64ComponentEvidence, ResourceDomainsDestroyed) == 56U);
static_assert(__builtin_offsetof(Ia64ComponentEvidence, ResourceDomainsReclaimed) == 64U);
static_assert(__builtin_offsetof(Ia64ComponentEvidence, ResourceDomainsLive) == 72U);
static_assert(sizeof(Ia64ComponentEvidence) == 80U);
static_assert(__builtin_offsetof(Ia64EvidenceRecord, Components) == 3760U);
static_assert(__builtin_offsetof(Ia64CollectionEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Ia64CollectionEvidence, Collections) == 8U);
static_assert(__builtin_offsetof(Ia64CollectionEvidence, Reclaimed) == 16U);
static_assert(__builtin_offsetof(Ia64CollectionEvidence, ScrubRuns) == 24U);
static_assert(__builtin_offsetof(Ia64CollectionEvidence, ScrubbedPages) == 32U);
static_assert(sizeof(Ia64CollectionEvidence) == 40U);
static_assert(__builtin_offsetof(Ia64EvidenceRecord, Collection) == 3840U);
static_assert(__builtin_offsetof(Ia64DeviceControlEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Ia64DeviceControlEvidence, Assignments) == 8U);
static_assert(__builtin_offsetof(Ia64DeviceControlEvidence, Releases) == 16U);
static_assert(__builtin_offsetof(Ia64DeviceControlEvidence, BusMasterRefusals) == 24U);
static_assert(__builtin_offsetof(Ia64DeviceControlEvidence, Quarantines) == 32U);
static_assert(__builtin_offsetof(Ia64DeviceControlEvidence, Confirmations) == 40U);
static_assert(sizeof(Ia64DeviceControlEvidence) == 48U);
static_assert(__builtin_offsetof(Ia64EvidenceRecord, DeviceControl) == 3880U);
static_assert(__builtin_offsetof(Ia64ServiceEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Ia64ServiceEvidence, WaitSetsCreated) == 8U);
static_assert(__builtin_offsetof(Ia64ServiceEvidence, WaitSetsReclaimed) == 16U);
static_assert(__builtin_offsetof(Ia64ServiceEvidence, Binds) == 24U);
static_assert(__builtin_offsetof(Ia64ServiceEvidence, WaitsBlocked) == 32U);
static_assert(__builtin_offsetof(Ia64ServiceEvidence, WaitExpiries) == 40U);
static_assert(__builtin_offsetof(Ia64ServiceEvidence, WaitAborts) == 48U);
static_assert(__builtin_offsetof(Ia64ServiceEvidence, AbortedExits) == 56U);
static_assert(sizeof(Ia64ServiceEvidence) == 64U);
static_assert(__builtin_offsetof(Ia64EvidenceRecord, Services) == 3928U);
static_assert(__builtin_offsetof(Ia64DeviceInterruptEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Ia64DeviceInterruptEvidence, Created) == 8U);
static_assert(__builtin_offsetof(Ia64DeviceInterruptEvidence, Destroyed) == 16U);
static_assert(__builtin_offsetof(Ia64DeviceInterruptEvidence, Reclaimed) == 24U);
static_assert(__builtin_offsetof(Ia64DeviceInterruptEvidence, Live) == 32U);
static_assert(__builtin_offsetof(Ia64DeviceInterruptEvidence, Delivered) == 40U);
static_assert(__builtin_offsetof(Ia64DeviceInterruptEvidence, Spurious) == 48U);
static_assert(sizeof(Ia64DeviceInterruptEvidence) == 56U);
static_assert(__builtin_offsetof(Ia64EvidenceRecord, Interrupts) == 3992U);
static_assert(__builtin_offsetof(Ia64EvidenceRecord, Stops) == 4048U);
static_assert(__builtin_offsetof(Ia64IommuEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Ia64IommuEvidence, Present) == 8U);
static_assert(__builtin_offsetof(Ia64IommuEvidence, Requester) == 16U);
static_assert(__builtin_offsetof(Ia64IommuEvidence, SweptFunctions) == 24U);
static_assert(__builtin_offsetof(Ia64IommuEvidence, Ready) == 32U);
static_assert(__builtin_offsetof(Ia64IommuEvidence, Bypass) == 40U);
static_assert(__builtin_offsetof(Ia64IommuEvidence, QueuePhysical) == 48U);
static_assert(__builtin_offsetof(Ia64IommuEvidence, QueuePages) == 56U);
static_assert(__builtin_offsetof(Ia64IommuEvidence, DomainsCreated) == 64U);
static_assert(__builtin_offsetof(Ia64IommuEvidence, DomainsReclaimed) == 72U);
static_assert(__builtin_offsetof(Ia64IommuEvidence, Faults) == 80U);
static_assert(__builtin_offsetof(Ia64IommuEvidence, LastFaultSource) == 88U);
static_assert(__builtin_offsetof(Ia64IommuEvidence, LastFaultReason) == 96U);
static_assert(__builtin_offsetof(Ia64IommuEvidence, LastFaultAddress) == 104U);
static_assert(sizeof(Ia64IommuEvidence) == 112U);
static_assert(__builtin_offsetof(Ia64EvidenceRecord, Iommu) == 4672U);
static_assert(__builtin_offsetof(Ia64EvidenceRecord, BootFiles) == 4784U);
static_assert(__builtin_offsetof(Ia64ParkedReleaseEvidence, Frame) == 0U);
static_assert(__builtin_offsetof(Ia64ParkedReleaseEvidence, PhysicalStart) == 8U);
static_assert(__builtin_offsetof(Ia64ParkedReleaseEvidence, Pages) == 16U);
static_assert(__builtin_offsetof(Ia64ParkedReleaseEvidence, Sampled) == 24U);
static_assert(__builtin_offsetof(Ia64ParkedReleaseEvidence, Sample) == 32U);
static_assert(sizeof(Ia64ParkedReleaseEvidence) == 64U);
static_assert(__builtin_offsetof(Ia64BlockEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Ia64BlockEvidence, PinsParked) == 8U);
static_assert(__builtin_offsetof(Ia64BlockEvidence, PinsReleased) == 16U);
static_assert(__builtin_offsetof(Ia64BlockEvidence, ParkedPins) == 24U);
static_assert(__builtin_offsetof(Ia64BlockEvidence, Releases) == 32U);
static_assert(__builtin_offsetof(Ia64BlockEvidence, Released) == 40U);
static_assert(sizeof(Ia64BlockEvidence) == 168U);
static_assert(__builtin_offsetof(Ia64EvidenceRecord, Block) == 5848U);
static_assert(__builtin_offsetof(Ia64VtdUnitEvidence, RegisterBase) == 0U);
static_assert(__builtin_offsetof(Ia64VtdUnitEvidence, State) == 8U);
static_assert(__builtin_offsetof(Ia64VtdUnitEvidence, DisabledReason) == 16U);
static_assert(__builtin_offsetof(Ia64VtdUnitEvidence, ConfigureError) == 24U);
static_assert(__builtin_offsetof(Ia64VtdUnitEvidence, EnableError) == 32U);
static_assert(__builtin_offsetof(Ia64VtdUnitEvidence, Version) == 40U);
static_assert(__builtin_offsetof(Ia64VtdUnitEvidence, Capability) == 48U);
static_assert(__builtin_offsetof(Ia64VtdUnitEvidence, ExtendedCapability) == 56U);
static_assert(__builtin_offsetof(Ia64VtdUnitEvidence, HandoffStatus) == 64U);
static_assert(__builtin_offsetof(Ia64VtdUnitEvidence, Status) == 72U);
static_assert(__builtin_offsetof(Ia64VtdUnitEvidence, Levels) == 80U);
static_assert(__builtin_offsetof(Ia64VtdUnitEvidence, ServedFunctions) == 88U);
static_assert(__builtin_offsetof(Ia64VtdUnitEvidence, Faults) == 96U);
static_assert(__builtin_offsetof(Ia64VtdUnitEvidence, Overflows) == 104U);
static_assert(__builtin_offsetof(Ia64VtdUnitEvidence, Unattributed) == 112U);
static_assert(__builtin_offsetof(Ia64VtdUnitEvidence, TableFlushes) == 120U);
static_assert(sizeof(Ia64VtdUnitEvidence) == 128U);
static_assert(__builtin_offsetof(Ia64IommuBackendEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Ia64IommuBackendEvidence, Backend) == 8U);
static_assert(__builtin_offsetof(Ia64IommuBackendEvidence, SelectionError) == 16U);
static_assert(__builtin_offsetof(Ia64IommuBackendEvidence, HostAddressBits) == 24U);
static_assert(__builtin_offsetof(Ia64IommuBackendEvidence, UnitCount) == 32U);
static_assert(__builtin_offsetof(Ia64IommuBackendEvidence, SweptFunctions) == 40U);
static_assert(__builtin_offsetof(Ia64IommuBackendEvidence, TablePhysical) == 48U);
static_assert(__builtin_offsetof(Ia64IommuBackendEvidence, TablePages) == 56U);
static_assert(__builtin_offsetof(Ia64IommuBackendEvidence, LastFaultRead) == 64U);
static_assert(__builtin_offsetof(Ia64IommuBackendEvidence, Units) == 72U);
static_assert(sizeof(Ia64IommuBackendEvidence) == 584U);
static_assert(__builtin_offsetof(Ia64EvidenceRecord, IommuBackend) == 6016U);
static_assert(__builtin_offsetof(Ia64MetadataEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Ia64MetadataEvidence, SlabPhysicalStart) == 8U);
static_assert(__builtin_offsetof(Ia64MetadataEvidence, SlabBytes) == 16U);
static_assert(__builtin_offsetof(Ia64MetadataEvidence, BitmapBudgetBytes) == 24U);
static_assert(__builtin_offsetof(Ia64MetadataEvidence, RegistryCapacity) == 32U);
static_assert(__builtin_offsetof(Ia64MetadataEvidence, FrameSlots) == 40U);
static_assert(__builtin_offsetof(Ia64MetadataEvidence, MemoryObjectSlots) == 48U);
static_assert(sizeof(Ia64MetadataEvidence) == 56U);
static_assert(__builtin_offsetof(Ia64EvidenceRecord, Metadata) == 6600U);
static_assert(__builtin_offsetof(Ia64EvidenceRecord, MemoryImport) == 6832U);
static_assert(sizeof(Ia64EvidenceRecord) == 19256U);
static_assert(__builtin_offsetof(Ia64EvidenceRecord, ApplicationLaunch) == 16192U);
static_assert(__builtin_offsetof(Ia64EvidenceRecord, Watchdog) == 16472U);
static_assert(__builtin_offsetof(Ia64EvidenceRecord, Clocks) == 16696U);
static_assert(__builtin_offsetof(Ia64EvidenceRecord, UserProcessors) == 16856U);
static_assert(sizeof(Ia64EvidenceRecord) == __builtin_offsetof(Ia64EvidenceRecord, UserProcessors) +
                                                sizeof(Kernel::Diagnostics::UserProcessorEvidence));
static_assert(__builtin_offsetof(Ia64AllocatorEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Ia64AllocatorEvidence, ManagedPages) == 8U);
static_assert(__builtin_offsetof(Ia64AllocatorEvidence, AllocatablePages) == 16U);
static_assert(__builtin_offsetof(Ia64AllocatorEvidence, CleanPages) == 24U);
static_assert(__builtin_offsetof(Ia64AllocatorEvidence, DirtyPages) == 32U);
static_assert(__builtin_offsetof(Ia64AllocatorEvidence, ClaimedPages) == 40U);
static_assert(__builtin_offsetof(Ia64AllocatorEvidence, RetiredPages) == 48U);
static_assert(__builtin_offsetof(Ia64AllocatorEvidence, AllocatedPages) == 56U);
static_assert(__builtin_offsetof(Ia64AllocatorEvidence, UnmanagedPages) == 64U);
static_assert(__builtin_offsetof(Ia64AllocatorEvidence, FreeLowBlocks) == 72U);
static_assert(__builtin_offsetof(Ia64AllocatorEvidence, ExcludedDmaPages) == 80U);
static_assert(__builtin_offsetof(Ia64AllocatorEvidence, ExcludedLoaderPages) == 88U);
static_assert(__builtin_offsetof(Ia64AllocatorEvidence, ExcludedFirmwarePages) == 96U);
static_assert(__builtin_offsetof(Ia64AllocatorEvidence, BootServicesImported) == 104U);
static_assert(__builtin_offsetof(Ia64AllocatorEvidence, ImportRefusal) == 112U);
static_assert(__builtin_offsetof(Ia64AllocatorEvidence, ZeroingSlots) == 120U);
static_assert(__builtin_offsetof(Ia64AllocatorEvidence, PeakZeroingSlots) == 128U);
static_assert(__builtin_offsetof(Ia64AllocatorEvidence, ZeroDeferrals) == 136U);
static_assert(__builtin_offsetof(Ia64AllocatorEvidence, ZeroFaults) == 144U);
static_assert(__builtin_offsetof(Ia64AllocatorEvidence, CreationZeroedPages) == 152U);
static_assert(__builtin_offsetof(Ia64AllocatorEvidence, CreationZeroRoot) == 160U);
static_assert(__builtin_offsetof(Ia64AllocatorEvidence, HandoffRecords) == 168U);
static_assert(sizeof(Ia64AllocatorEvidence) == 176U);
static_assert(__builtin_offsetof(Ia64EvidenceRecord, Allocator) == 6656U);
} // Namespace Asteria::Platform::Ia64::Diagnostics

#endif // ASTERIA_PLATFORM_IA64_DIAGNOSTICS_IA64_EVIDENCE_RECORD_HPP_INCLUDED
