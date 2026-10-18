// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_DIAGNOSTICS_AMD64_EVIDENCE_RECORD_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_DIAGNOSTICS_AMD64_EVIDENCE_RECORD_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Diagnostics/ApplicationLaunchEvidence.hpp"
#include "Kernel/Diagnostics/Watchdog.hpp"
#include "Kernel/Time/ProcessorClock.hpp"
#include "Kernel/Diagnostics/UserProcessorEvidence.hpp"
#include "Kernel/Memory/PhysicalPageImportEvidence.hpp"
#include "Boot/BootFiles.hpp"
#include "Platform/Amd64/Diagnostics/Amd64Stop.hpp"
#include "Kernel/Diagnostics/EvidenceRecord.hpp"
#include "Kernel/Scheduler/SchedulerTypes.hpp"
#include "Platform/Amd64/Interrupt/Amd64UserEntry.hpp"
#include "Platform/Amd64/Scheduler/Amd64ApplicationProcessorRuntime.hpp"

namespace Asteria::Platform::Amd64::Diagnostics
{
/// Layout version of Amd64EvidenceRecord (EvidenceRecordHeader::Schema). Increment it with every
/// change of a type below.
inline constexpr Foundation::UInt32 Amd64EvidenceRecordSchema{39U};
/// Per-processor slots; slot n holds processor n (the BSP is processor 0).
inline constexpr Foundation::Size Amd64EvidenceProcessorSlots{4U};
/// The ATA IDENTIFY DEVICE data block (256 words) the AHCI evidence retains.
inline constexpr Foundation::Size Amd64EvidenceIdentifyDataBytes{512U};
/// Resumed WaitSet waits whose details are retained; later ones are counted only.
inline constexpr Foundation::Size Amd64EvidenceWaitSlots{4U};
/// General registers the preemption proof's loop stores after it resumed.
inline constexpr Foundation::Size Amd64EvidencePreemptionRegisters{16U};
/// Boot user domains whose preemptions are counted: the initial image, the
/// boot pager and the spinner, in this slot order.
inline constexpr Foundation::Size Amd64EvidenceUserDomains{3U};
inline constexpr Foundation::Size Amd64EvidenceClientDomain{0U};
inline constexpr Foundation::Size Amd64EvidencePagerDomain{1U};
inline constexpr Foundation::Size Amd64EvidenceSpinnerDomain{2U};

static_assert(Amd64EvidenceProcessorSlots == Kernel::Scheduler::MaximumProcessorCount);

/// How far the boot path's scripted AHCI transfer got (Amd64AhciEvidence::Outcome). Unavailable:
/// the ECAM scan bound no AHCI controller and the firmware captured none either.
enum class Amd64AhciEvidenceOutcome : Foundation::UInt64
{
    None = 0U,
    NoDisk = 1U,
    Identified = 2U,
    Read = 3U,
    Unavailable = 4U,
    /// Active ports the diagnostic does not choose between (an ATAPI drive,
    /// an unknown signature or several disks): the disks are left alone.
    UnsupportedPorts = 5U,
    /// The IDENTIFY or the read of the diagnostic sector failed on a disk
    /// the runner did not attach (a real machine or another hypervisor):
    /// recorded and skipped, the disk left alone.
    DiagnosticFailed = 6U,
};

/// Where the boot runtime placed itself once the shared kernel root and the
/// local APIC timer were active: its runtime pool, the physical roots of the
/// shared kernel address space and of the initial image's private address
/// space, the local APIC page it maps (from the ACPI MADT), and the number of
/// processors the ACPI topology names.
struct Amd64RuntimeEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 PoolPhysicalStart;
    Foundation::UInt64 SharedRootPhysicalAddress;
    Foundation::UInt64 UserRootPhysicalAddress;
    Foundation::UInt64 LocalApicPhysicalAddress;
    Foundation::UInt64 ProcessorCount;
};

/// The PCI ECAM scan: the ECAM regions and buses it covered, the functions it
/// published, its first function, and the function it bound as the AHCI
/// controller (AhciAvailability is the AhciPciAvailability value).
/// Vendor and device identifiers are packed as `device << 16 | vendor`.
struct Amd64PciEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 RegionCount;
    Foundation::UInt64 BusCount;
    Foundation::UInt64 PublishedFunctionCount;
    Foundation::UInt64 FirstFunctionPresent;
    Foundation::UInt64 FirstSegment;
    Foundation::UInt64 FirstBus;
    Foundation::UInt64 FirstDevice;
    Foundation::UInt64 FirstFunction;
    Foundation::UInt64 FirstVendorDevice;
    Foundation::UInt64 AhciAvailability;
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
/// keeps the transferred bytes for the boot's lifetime.
struct Amd64AhciEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 Outcome;
    Foundation::UInt64 PortNumber;
    Foundation::UInt64 ReadStartingLba;
    Foundation::UInt64 ReadSectorCount;
    Foundation::UInt64 ReadTransferSizeBytes;
    Foundation::UInt64 ReadBufferPhysicalAddress;
    Foundation::UInt64 ReadBufferSizeBytes;
    Foundation::UInt8 IdentifyData[Amd64EvidenceIdentifyDataBytes];
};

/// One application processor as the BSP observed it after the AP reported
/// serving: its state word, the local-timer interrupts and timer deliveries
/// it counted, what its timer probe observed (written by the AP before it
/// reported serving), and the AP's completed services and worker runs after
/// the BSP's first service request. Nothing is compared with the counts the
/// scenario expects.
struct Amd64ApplicationProcessorEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 Processor;
    Foundation::UInt64 State;
    Foundation::UInt64 TimerInterrupts;
    Foundation::UInt64 TimerDeliveries;
    Scheduler::Amd64ApplicationProcessorTimerObservation Timer;
    Foundation::UInt64 ServiceCompletions;
    Foundation::UInt64 ServiceRuns;
};

/// Every application processor. Published is set once every AP the ACPI
/// topology names serves; ServingProcessorMask has bit n for each serving
/// AP n. The BSP's slot 0 stays empty.
struct Amd64ApplicationProcessorRuntimeEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 ProcessorCount;
    Foundation::UInt64 ServingProcessorMask;
    Amd64ApplicationProcessorEvidence Processors[Amd64EvidenceProcessorSlots];
};

/// The shared-root TLB shootdown diagnostic: the page the BSP mapped into the
/// shared kernel root and the word it stored there first, the shootdown's
/// sequence and processor masks (resident before the mapping, targeted,
/// queued to remote processors and acknowledged by the BSP when the dispatch
/// returned, and acknowledged when the wait ended), and for each processor
/// the word it read through the new mapping (slot 0: the BSP) and, for each
/// AP, its worker runs after that read.
struct Amd64SharedRootShootdownEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 VirtualAddress;
    Foundation::UInt64 PhysicalAddress;
    Foundation::UInt64 Value;
    Foundation::UInt64 ShootdownSequence;
    Foundation::UInt64 ResidentProcessorMask;
    Foundation::UInt64 TargetProcessorMask;
    Foundation::UInt64 QueuedProcessorMask;
    Foundation::UInt64 DispatchAcknowledgedMask;
    Foundation::UInt64 AcknowledgedProcessorMask;
    Foundation::UInt64 ObservedValues[Amd64EvidenceProcessorSlots];
    Foundation::UInt64 ServiceRuns[Amd64EvidenceProcessorSlots];
};

/// The one-shot BSP native-preemption proof (preemption-probe builds): how
/// often a frame was captured, resumed and its worker exited; the loop and
/// worker stack the capture accepted; the preempting deadline and when the
/// capture observed it; the stopped run, its budget left and the
/// replenishment the coordinator waited for; the run loop's external-frame
/// token, resume generation and admission run; the fresh idle continuation's
/// generation; the addresses the loop received in RDI and RSI; the captured
/// frame; and the sixteen general registers the loop stored after it resumed
/// (rax, rbx, rcx, rdx, rbp, r8-r15, rdi, rsi, rsp).
struct Amd64NativePreemptionEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 CaptureCount;
    Foundation::UInt64 ResumeCount;
    Foundation::UInt64 ExitCount;
    Foundation::UInt64 LoopStart;
    Foundation::UInt64 LoopEnd;
    Foundation::UInt64 WorkerStackStart;
    Foundation::UInt64 WorkerStackEnd;
    Foundation::UInt64 ProgrammedDeadlineTicks;
    Foundation::UInt64 ObservedTicks;
    Foundation::UInt64 StoppedRunSequence;
    Foundation::UInt64 RemainingBudgetTicks;
    Foundation::UInt64 ReplenishmentTicks;
    Foundation::UInt64 ExternalFrameToken;
    Foundation::UInt64 ExternalFrameResumeGeneration;
    Foundation::UInt64 AdmissionRunSequence;
    Foundation::UInt64 IdleContinuationGeneration;
    Foundation::UInt64 ResumeFlagAddress;
    Foundation::UInt64 RegisterBufferAddress;
    Interrupt::Amd64TrapFrame Frame;
    Foundation::UInt64 Registers[Amd64EvidencePreemptionRegisters];
};

/// One suspended WaitSetWait of the initial image, from the timer the kernel
/// armed for it to the response the resumed system call returned: the
/// TimerQueue deadline, when the idle loop saw the interrupt and how many
/// BSP timer interrupts arrived since the wait was armed, the
/// DeferredTimerWorker's drain receipt (count, stop reason, whether every
/// delivered timer was the wait fixture's own, the final queue state), the
/// deadline state after the refresh, the WaitSet dispatcher's completion (tag
/// and registration sequence) and the response (error, value 0, value 1).
struct Amd64WaitResumptionEvidence
{
    Foundation::UInt64 DeadlineTicks;
    Foundation::UInt64 ObservedTicks;
    Foundation::UInt64 ObservedInterrupts;
    Foundation::UInt64 DeliveredCount;
    Foundation::UInt64 DrainStop;
    Foundation::UInt64 DeliveredOwnTimer;
    Foundation::UInt64 QueueNextDeadlineTicks;
    Foundation::UInt64 QueueScheduledEntries;
    Foundation::UInt64 QueuePendingEntries;
    Foundation::UInt64 QueueClaimedEntries;
    Foundation::UInt64 DeadlineState;
    Foundation::UInt64 CompletedTag;
    Foundation::UInt64 CompletedRegistrationSequence;
    Foundation::UInt64 ResponseError;
    Foundation::UInt64 ResponseValue0;
    Foundation::UInt64 ResponseValue1;
};

/// The kernel-owned WaitSet fixture of the initial image: the tag its timer
/// Notification is registered with, how many WaitSetWait system calls the
/// image made, how many suspended and how many resumed, and the first
/// Amd64EvidenceWaitSlots resumptions. Published after the registration and
/// again after every resumption.
struct Amd64WaitEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 RegisteredTag;
    Foundation::UInt64 WaitCalls;
    Foundation::UInt64 SuspendedWaits;
    Foundation::UInt64 ResumedWaits;
    Amd64WaitResumptionEvidence Resumptions[Amd64EvidenceWaitSlots];
};

/// The BSP timer interrupts taken while a user thread ran, those whose fired deadline was the
/// TimerQueue's (the wait fixture's timer), and per boot domain the runs such a tick ended Ready.
/// Then the spinner, which only the test firmware has: whether this boot has one, the lines it
/// wrote, and whether its final WaitSet wait suspended. Published once the boot threads are
/// admitted and again after every change.
struct Amd64UserPreemptionEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 UserTicks;
    Foundation::UInt64 TimerQueueTicks;
    Foundation::UInt64 RunStops[Amd64EvidenceUserDomains];
    Foundation::UInt64 SpinnerPresent;
    Foundation::UInt64 SpinnerLines;
    Foundation::UInt64 SpinnerWaitSuspended;
};

/// The faults the boot pager resolved with Terminate, whether the spinner thread exited with which
/// exit code, and the client's and spinner's faults deferred because the pager held another.
/// Published once the boot threads are admitted and again after every Terminate and deferral.
struct Amd64SupervisionEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 Terminations;
    Foundation::UInt64 SpinnerExited;
    Foundation::UInt64 SpinnerExitCode;
    Foundation::UInt64 Deferrals;
};

/// Object management: the Endpoints the boot pager created, destroyed and the kernel reclaimed, the
/// Endpoint pool's occupied (active or quiescing) slots, the pager's management ResourceDomain's
/// current and peak kernel-metadata charge, its current IPC-queue charge, the charge of one
/// Endpoint, and the registered objects once the boot domains were admitted; then the Frames and
/// MemoryObjects created and reclaimed, their metadata charges, the grant's committed and resident
/// pages, the frame pool's free and dirty pages, the mappings MemoryMap completed with the last
/// one's virtual and physical start and pages in the pager's own space, and the mappings
/// MemoryUnmap removed. Published at that admission and after every ObjectCreate, ObjectDestroy,
/// MemoryMap and MemoryUnmap of the pager.
struct Amd64ObjectManagementEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 Created;
    Foundation::UInt64 Destroyed;
    Foundation::UInt64 Reclaimed;
    Foundation::UInt64 OccupiedSlots;
    Foundation::UInt64 MetadataBytes;
    Foundation::UInt64 IpcQueueBytes;
    Foundation::UInt64 PeakMetadataBytes;
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
};

/// Created threads: the threads the pager created, the ones its ObjectDestroy reclaimed (returning
/// their exit code) and the thread-pool slots the kernel freed; the ThreadExit calls that ended a
/// run, the native slots the reaper freed and the lines created threads wrote; the exit code
/// ObjectDestroy last returned; the pool's and the native table's free slots and the grant's
/// Threads usage; the user ticks that ended a created thread's run and the ObjectDestroy calls
/// refused because the thread had not exited; one thread's kernel-metadata charge; and the page
/// faults and other exceptions of created threads, those deferred behind another, and the
/// exceptions among them; and the threads ThreadTerminate killed. Published once the boot threads
/// are admitted and after every management call, created thread's preemption or fault and idle
/// reap.
struct Amd64ThreadEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 Created;
    Foundation::UInt64 Destroyed;
    Foundation::UInt64 Reclaimed;
    Foundation::UInt64 Exits;
    Foundation::UInt64 Reaped;
    Foundation::UInt64 Lines;
    Foundation::UInt64 LastExitCode;
    Foundation::UInt64 FreeSlots;
    Foundation::UInt64 FreeNativeSlots;
    Foundation::UInt64 ThreadsUsage;
    Foundation::UInt64 RunStops;
    Foundation::UInt64 DestroyPolls;
    Foundation::UInt64 SlotMetadataBytes;
    Foundation::UInt64 Faults;
    Foundation::UInt64 Deferrals;
    Foundation::UInt64 Exceptions;
    Foundation::UInt64 Kills;
};

/// Created domains: the AddressSpaces and CapabilitySpaces created and still live, the
/// ProtectionDomains created, destroyed and reclaimed, the faults of threads in created domains,
/// the tag of the last, their ThreadExits that woke the supervisor, and the kernel-metadata charge
/// of one AddressSpace (with its arena) and of one CapabilitySpace; then the AddressSpaces and
/// CapabilitySpaces reclaimed, the arenas prepared, the AddressSpace and CapabilitySpace slots
/// free, and the pager's waits that timed out: its watchdog's expiries and its self-test's polls
/// while a created thread waits; last, the components' ResourceDomains created, destroyed (drained)
/// and reclaimed, and those still occupying a slot. Published with the object management evidence.
struct Amd64ComponentEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 AddressSpacesCreated;
    Foundation::UInt64 AddressSpacesLive;
    Foundation::UInt64 CapabilitySpacesCreated;
    Foundation::UInt64 CapabilitySpacesLive;
    Foundation::UInt64 DomainsCreated;
    Foundation::UInt64 DomainsDestroyed;
    Foundation::UInt64 DomainsReclaimed;
    Foundation::UInt64 TaggedFaults;
    Foundation::UInt64 LastTag;
    Foundation::UInt64 DomainExits;
    Foundation::UInt64 AddressSpaceBytes;
    Foundation::UInt64 CapabilitySpaceBytes;
    Foundation::UInt64 AddressSpacesReclaimed;
    Foundation::UInt64 CapabilitySpacesReclaimed;
    Foundation::UInt64 ArenasPrepared;
    Foundation::UInt64 AddressSpacesFree;
    Foundation::UInt64 CapabilitySpacesFree;
    Foundation::UInt64 WaitExpiries;
    Foundation::UInt64 ResourceDomainsCreated;
    Foundation::UInt64 ResourceDomainsDestroyed;
    Foundation::UInt64 ResourceDomainsReclaimed;
    Foundation::UInt64 ResourceDomainsLive;
};

/// The AMD64 evidence record. Sub-records a build or a boot does not reach stay zero (unpublished).
/// The WaitSets the pager created and the ones the kernel reclaimed, the Endpoints
/// WaitSetBind bound, the WaitSetWait calls of created threads that blocked and those of them their
/// deadline completed, and the waits ThreadTerminate aborted and the threads that ended as they
/// resumed. Published with the thread evidence.
struct Amd64ServiceEvidence
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

/// DMA remapping, published once the boot runtime decided on it and refreshed with the component
/// evidence: why the ACPI DMAR names no usable unit (VtdTopologyError, zero when it does), the
/// unit's register page, the functions it serves and the functions whose bus mastering the boot
/// turned off before translation started, the VtdUnitError of its configuration and enabling,
/// whether it translates and with how many levels, its version, capability, extended capability and
/// global status at handoff and now, and the faults it recorded: their number, overflows, those of
/// a requester no domain held, and the last one's requester, reason, page and direction; then the
/// IommuDomains created and reclaimed.
struct Amd64IommuEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 SelectionError;
    Foundation::UInt64 RegisterBase;
    Foundation::UInt64 ServedFunctions;
    Foundation::UInt64 SweptFunctions;
    Foundation::UInt64 ConfigureError;
    Foundation::UInt64 EnableError;
    Foundation::UInt64 Enabled;
    Foundation::UInt64 Levels;
    Foundation::UInt64 Version;
    Foundation::UInt64 Capability;
    Foundation::UInt64 ExtendedCapability;
    Foundation::UInt64 HandoffStatus;
    Foundation::UInt64 Status;
    Foundation::UInt64 Faults;
    Foundation::UInt64 Overflows;
    Foundation::UInt64 Unattributed;
    Foundation::UInt64 LastFaultSource;
    Foundation::UInt64 LastFaultReason;
    Foundation::UInt64 LastFaultAddress;
    Foundation::UInt64 LastFaultRead;
    Foundation::UInt64 DomainsCreated;
    Foundation::UInt64 DomainsReclaimed;
};

/// Device interrupts, written with the IOMMU evidence: the Interrupts user code created, destroyed
/// (by ObjectDestroy or a drain) and reclaimed, those still in the pool, and the messages that
/// reached a bound device vector and an unbound one.
struct Amd64DeviceInterruptEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 Created;
    Foundation::UInt64 Destroyed;
    Foundation::UInt64 Reclaimed;
    Foundation::UInt64 Live;
    Foundation::UInt64 Delivered;
    Foundation::UInt64 Spurious;
};

/// The run-time control of assigned PCI functions, written with the device interrupts: the
/// assignments the Device Manager made and the releases, the bus-master requests refused, and the
/// functions a drain released into quarantine and the quarantines the Device Manager lifted once it
/// found the device quiet.
struct Amd64DeviceControlEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 Assignments;
    Foundation::UInt64 Releases;
    Foundation::UInt64 BusMasterRefusals;
    Foundation::UInt64 Quarantines;
    Foundation::UInt64 Confirmations;
    /// The Frame pins drains parked with quarantined functions and those the confirmations
    /// released, and those still parked.
    Foundation::UInt64 PinsParked;
    Foundation::UInt64 PinsReleased;
    Foundation::UInt64 ParkedPins;
};

/// What the object manager's collections did: the pool passes, the slots they reclaimed, and the
/// dirty frame-pool runs scrubbed and their pages. Written with the object management evidence.
struct Amd64CollectionEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 Collections;
    Foundation::UInt64 Reclaimed;
    Foundation::UInt64 ScrubRuns;
    Foundation::UInt64 ScrubbedPages;
};

/// A parked Frame pin a Device Manager's confirmation released: the Frame's registry identity,
/// first page and pages, and, once the frame pool scrubbed that page after the release, its first
/// 32 bytes as the scrub found them (Sampled one).
struct Amd64ParkedReleaseEvidence
{
    Foundation::UInt64 Frame;
    Foundation::UInt64 PhysicalStart;
    Foundation::UInt64 Pages;
    Foundation::UInt64 Sampled;
    Foundation::UInt64 Sample[4];
};

/// The block I/O evidence, written with the device interrupts: the parked pins confirmations
/// released, and the last two.
struct Amd64BlockEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 Releases;
    Amd64ParkedReleaseEvidence Released[2];
};

/// Per-DRHD facts, in firmware order. State: 0 pending/refused, 1 kept
/// untranslated, 2 enabled, 3 failed. The legacy Iommu record summarizes
/// enabled units and keeps single-unit observers' meanings.
struct Amd64IommuUnitEvidence
{
    Foundation::UInt64 RegisterBase;
    Foundation::UInt64 State;
    Foundation::UInt64 ConfigureError;
    Foundation::UInt64 EnableError;
    Foundation::UInt64 SweptFunctions;
    Foundation::UInt64 ServedFunctions;
    Foundation::UInt64 ReservedFunctions;
    Foundation::UInt64 ReservedPages;
    Foundation::UInt64 Levels;
    Foundation::UInt64 Version;
    Foundation::UInt64 Capability;
    Foundation::UInt64 ExtendedCapability;
    Foundation::UInt64 HandoffStatus;
    Foundation::UInt64 Status;
    Foundation::UInt64 Faults;
    Foundation::UInt64 Overflows;
    Foundation::UInt64 Unattributed;
    /// 0 translation requested, 1 dedicated display, 3 refusal, 4 empty scoped unit.
    Foundation::UInt64 DisabledReason;
    Foundation::UInt64 DroppedRequesters;
};

struct Amd64IommuTopologyEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 Count;
    Foundation::UInt64 IgnoredScopes;
    Foundation::UInt64 MalformedRmrr;
    Amd64IommuUnitEvidence Units[4U];
};

/// Backend kind: 0 none, 1 VT-d, 2 AMD-Vi. Appended in schema 28.
struct Amd64IommuBackendEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 Kind;
    Foundation::UInt64 MalformedIvrs;
    Foundation::UInt64 SkippedHid;
    Foundation::UInt64 IvhdType;
};

/// The metadata arena, appended in schema 31: the receipt the kernel accepted, where its slabs and
/// bitmaps begin (arena pages), the page-table pages the shared root's seed took of its 64-page
/// arena, and the capacities of the slabs attached there.
struct Amd64MetadataEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 ArenaPhysicalStart;
    Foundation::UInt64 ArenaPages;
    Foundation::UInt64 SlabFirstPage;
    Foundation::UInt64 BitmapFirstPage;
    Foundation::UInt64 SeedTablePages;
    Foundation::UInt64 RegistryCapacity;
    Foundation::UInt64 FrameSlots;
    Foundation::UInt64 MemoryObjectSlots;
};

/// Physical-page allocator accounting, import, zeroing and handoff evidence.
struct Amd64AllocatorEvidence
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
using Kernel::Scheduler::MaximumProcessorCount;
struct Amd64SmpEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 ShootdownSent[MaximumProcessorCount];
    Foundation::UInt64 ShootdownTaken[MaximumProcessorCount];
    Foundation::UInt64 KickPending[MaximumProcessorCount];
    Foundation::UInt64 InterruptWindowStart;
    Foundation::UInt64 InterruptWindowEnd;
};
static_assert(__builtin_offsetof(Amd64SmpEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Amd64SmpEvidence, ShootdownSent) == 8U);
static_assert(__builtin_offsetof(Amd64SmpEvidence, ShootdownTaken) == 40U);
static_assert(__builtin_offsetof(Amd64SmpEvidence, KickPending) == 72U);
static_assert(__builtin_offsetof(Amd64SmpEvidence, InterruptWindowStart) == 104U);
static_assert(__builtin_offsetof(Amd64SmpEvidence, InterruptWindowEnd) == 112U);
static_assert(sizeof(Amd64SmpEvidence) == 120U);
static_assert(104U == sizeof(Foundation::UInt64) * (1U + 3U * Kernel::Scheduler::MaximumProcessorCount));

struct Amd64EvidenceRecord
{
    Kernel::Diagnostics::EvidenceRecordHeader Header;
    Amd64RuntimeEvidence Runtime;
    Amd64PciEvidence Pci;
    Amd64AhciEvidence Ahci;
    Amd64ApplicationProcessorRuntimeEvidence ApplicationProcessors;
    Amd64SharedRootShootdownEvidence SharedRootShootdown;
    Amd64NativePreemptionEvidence NativePreemption;
    Amd64WaitEvidence Wait;
    Amd64UserPreemptionEvidence UserPreemption;
    Amd64SupervisionEvidence Supervision;
    Amd64ObjectManagementEvidence ObjectManagement;
    Amd64ThreadEvidence Threads;
    Amd64ComponentEvidence Components;
    Amd64ServiceEvidence Services;
    Amd64IommuEvidence Iommu;
    Amd64DeviceInterruptEvidence Interrupts;
    Amd64DeviceControlEvidence DeviceControl;
    Amd64CollectionEvidence Collection;
    Amd64BlockEvidence Block;
    Amd64StopsEvidence Stops;
    Amd64IommuTopologyEvidence IommuTopology;
    /// BSP calibration and mode; published with Runtime.Published.
    Foundation::UInt64 TscFrequencyHz;
    Foundation::UInt64 TscSource;
    Foundation::UInt64 X2Apic;
    Amd64IommuBackendEvidence IommuBackend;
    Boot::BootFileRegion BootFiles;
    Amd64MetadataEvidence Metadata;
    Amd64AllocatorEvidence Allocator;
    Kernel::Memory::PhysicalPageImportEvidence MemoryImport;
    Amd64SmpEvidence Smp;
    Kernel::Diagnostics::ApplicationLaunchEvidence ApplicationLaunch;
    Kernel::Diagnostics::WatchdogEvidence Watchdog;
    Kernel::Time::ProcessorClockEvidence Clocks;
    Kernel::Diagnostics::UserProcessorEvidence UserProcessors;
};

/// The boot-lifetime record. Stops has one payload writer per processor; its
/// state words are atomic and the latch winner publishes the final masks.
/// For all other sub-records the BSP is the only writer, and its writers never
/// overlap (the boot path, the system-call path and the preemption interrupt
/// run one at a time); an AP's timer observation reaches it through the AP's
/// release/acquire state handshake.
[[nodiscard]] Amd64EvidenceRecord& GetAmd64EvidenceRecord() noexcept;

/// Zero-fills the record, writes its header and returns its address (the
/// identity-mapped physical address the host reads).
[[nodiscard]] Foundation::UInt64 InitializeAmd64EvidenceRecord() noexcept;

/// Makes one sub-record visible: every preceding store to it happens before
/// `published` becomes one.
void PublishAmd64Evidence(Foundation::UInt64& published) noexcept;

// Layout (the host decodes the record from these assertions).
static_assert(__is_standard_layout(Amd64EvidenceRecord));
static_assert(__is_trivially_copyable(Amd64EvidenceRecord));
static_assert(alignof(Amd64EvidenceRecord) == 8U);
static_assert(__builtin_offsetof(Amd64RuntimeEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Amd64RuntimeEvidence, PoolPhysicalStart) == 8U);
static_assert(__builtin_offsetof(Amd64RuntimeEvidence, SharedRootPhysicalAddress) == 16U);
static_assert(__builtin_offsetof(Amd64RuntimeEvidence, UserRootPhysicalAddress) == 24U);
static_assert(__builtin_offsetof(Amd64RuntimeEvidence, LocalApicPhysicalAddress) == 32U);
static_assert(__builtin_offsetof(Amd64RuntimeEvidence, ProcessorCount) == 40U);
static_assert(sizeof(Amd64RuntimeEvidence) == 48U);
static_assert(__builtin_offsetof(Amd64PciEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Amd64PciEvidence, RegionCount) == 8U);
static_assert(__builtin_offsetof(Amd64PciEvidence, BusCount) == 16U);
static_assert(__builtin_offsetof(Amd64PciEvidence, PublishedFunctionCount) == 24U);
static_assert(__builtin_offsetof(Amd64PciEvidence, FirstFunctionPresent) == 32U);
static_assert(__builtin_offsetof(Amd64PciEvidence, FirstSegment) == 40U);
static_assert(__builtin_offsetof(Amd64PciEvidence, FirstBus) == 48U);
static_assert(__builtin_offsetof(Amd64PciEvidence, FirstDevice) == 56U);
static_assert(__builtin_offsetof(Amd64PciEvidence, FirstFunction) == 64U);
static_assert(__builtin_offsetof(Amd64PciEvidence, FirstVendorDevice) == 72U);
static_assert(__builtin_offsetof(Amd64PciEvidence, AhciAvailability) == 80U);
static_assert(__builtin_offsetof(Amd64PciEvidence, AhciPresent) == 88U);
static_assert(__builtin_offsetof(Amd64PciEvidence, AhciSegment) == 96U);
static_assert(__builtin_offsetof(Amd64PciEvidence, AhciBus) == 104U);
static_assert(__builtin_offsetof(Amd64PciEvidence, AhciDevice) == 112U);
static_assert(__builtin_offsetof(Amd64PciEvidence, AhciFunction) == 120U);
static_assert(__builtin_offsetof(Amd64PciEvidence, AhciVendorDevice) == 128U);
static_assert(__builtin_offsetof(Amd64PciEvidence, AhciClassCode) == 136U);
static_assert(__builtin_offsetof(Amd64PciEvidence, AhciSubclass) == 144U);
static_assert(__builtin_offsetof(Amd64PciEvidence, AhciProgrammingInterface) == 152U);
static_assert(sizeof(Amd64PciEvidence) == 160U);
static_assert(__builtin_offsetof(Amd64AhciEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Amd64AhciEvidence, Outcome) == 8U);
static_assert(__builtin_offsetof(Amd64AhciEvidence, PortNumber) == 16U);
static_assert(__builtin_offsetof(Amd64AhciEvidence, ReadStartingLba) == 24U);
static_assert(__builtin_offsetof(Amd64AhciEvidence, ReadSectorCount) == 32U);
static_assert(__builtin_offsetof(Amd64AhciEvidence, ReadTransferSizeBytes) == 40U);
static_assert(__builtin_offsetof(Amd64AhciEvidence, ReadBufferPhysicalAddress) == 48U);
static_assert(__builtin_offsetof(Amd64AhciEvidence, ReadBufferSizeBytes) == 56U);
static_assert(__builtin_offsetof(Amd64AhciEvidence, IdentifyData) == 64U);
static_assert(sizeof(Amd64AhciEvidence) == 576U);
static_assert(__builtin_offsetof(Amd64ApplicationProcessorEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Amd64ApplicationProcessorEvidence, Processor) == 8U);
static_assert(__builtin_offsetof(Amd64ApplicationProcessorEvidence, State) == 16U);
static_assert(__builtin_offsetof(Amd64ApplicationProcessorEvidence, TimerInterrupts) == 24U);
static_assert(__builtin_offsetof(Amd64ApplicationProcessorEvidence, TimerDeliveries) == 32U);
static_assert(__builtin_offsetof(Amd64ApplicationProcessorEvidence, Timer) == 40U);
static_assert(__builtin_offsetof(Amd64ApplicationProcessorEvidence, ServiceCompletions) == 192U);
static_assert(__builtin_offsetof(Amd64ApplicationProcessorEvidence, ServiceRuns) == 200U);
static_assert(sizeof(Amd64ApplicationProcessorEvidence) == 208U);
static_assert(__builtin_offsetof(Amd64ApplicationProcessorRuntimeEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Amd64ApplicationProcessorRuntimeEvidence, ProcessorCount) == 8U);
static_assert(__builtin_offsetof(Amd64ApplicationProcessorRuntimeEvidence, ServingProcessorMask) == 16U);
static_assert(__builtin_offsetof(Amd64ApplicationProcessorRuntimeEvidence, Processors) == 24U);
static_assert(sizeof(Amd64ApplicationProcessorRuntimeEvidence) == 856U);
static_assert(__builtin_offsetof(Amd64SharedRootShootdownEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Amd64SharedRootShootdownEvidence, VirtualAddress) == 8U);
static_assert(__builtin_offsetof(Amd64SharedRootShootdownEvidence, PhysicalAddress) == 16U);
static_assert(__builtin_offsetof(Amd64SharedRootShootdownEvidence, Value) == 24U);
static_assert(__builtin_offsetof(Amd64SharedRootShootdownEvidence, ShootdownSequence) == 32U);
static_assert(__builtin_offsetof(Amd64SharedRootShootdownEvidence, ResidentProcessorMask) == 40U);
static_assert(__builtin_offsetof(Amd64SharedRootShootdownEvidence, TargetProcessorMask) == 48U);
static_assert(__builtin_offsetof(Amd64SharedRootShootdownEvidence, QueuedProcessorMask) == 56U);
static_assert(__builtin_offsetof(Amd64SharedRootShootdownEvidence, DispatchAcknowledgedMask) == 64U);
static_assert(__builtin_offsetof(Amd64SharedRootShootdownEvidence, AcknowledgedProcessorMask) == 72U);
static_assert(__builtin_offsetof(Amd64SharedRootShootdownEvidence, ObservedValues) == 80U);
static_assert(__builtin_offsetof(Amd64SharedRootShootdownEvidence, ServiceRuns) == 112U);
static_assert(sizeof(Amd64SharedRootShootdownEvidence) == 144U);
static_assert(__builtin_offsetof(Amd64NativePreemptionEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Amd64NativePreemptionEvidence, CaptureCount) == 8U);
static_assert(__builtin_offsetof(Amd64NativePreemptionEvidence, ResumeCount) == 16U);
static_assert(__builtin_offsetof(Amd64NativePreemptionEvidence, ExitCount) == 24U);
static_assert(__builtin_offsetof(Amd64NativePreemptionEvidence, LoopStart) == 32U);
static_assert(__builtin_offsetof(Amd64NativePreemptionEvidence, LoopEnd) == 40U);
static_assert(__builtin_offsetof(Amd64NativePreemptionEvidence, WorkerStackStart) == 48U);
static_assert(__builtin_offsetof(Amd64NativePreemptionEvidence, WorkerStackEnd) == 56U);
static_assert(__builtin_offsetof(Amd64NativePreemptionEvidence, ProgrammedDeadlineTicks) == 64U);
static_assert(__builtin_offsetof(Amd64NativePreemptionEvidence, ObservedTicks) == 72U);
static_assert(__builtin_offsetof(Amd64NativePreemptionEvidence, StoppedRunSequence) == 80U);
static_assert(__builtin_offsetof(Amd64NativePreemptionEvidence, RemainingBudgetTicks) == 88U);
static_assert(__builtin_offsetof(Amd64NativePreemptionEvidence, ReplenishmentTicks) == 96U);
static_assert(__builtin_offsetof(Amd64NativePreemptionEvidence, ExternalFrameToken) == 104U);
static_assert(__builtin_offsetof(Amd64NativePreemptionEvidence, ExternalFrameResumeGeneration) == 112U);
static_assert(__builtin_offsetof(Amd64NativePreemptionEvidence, AdmissionRunSequence) == 120U);
static_assert(__builtin_offsetof(Amd64NativePreemptionEvidence, IdleContinuationGeneration) == 128U);
static_assert(__builtin_offsetof(Amd64NativePreemptionEvidence, ResumeFlagAddress) == 136U);
static_assert(__builtin_offsetof(Amd64NativePreemptionEvidence, RegisterBufferAddress) == 144U);
static_assert(__builtin_offsetof(Amd64NativePreemptionEvidence, Frame) == 152U);
static_assert(__builtin_offsetof(Amd64NativePreemptionEvidence, Registers) == 328U);
static_assert(sizeof(Amd64NativePreemptionEvidence) == 456U);
static_assert(__builtin_offsetof(Amd64WaitResumptionEvidence, DeadlineTicks) == 0U);
static_assert(__builtin_offsetof(Amd64WaitResumptionEvidence, ObservedTicks) == 8U);
static_assert(__builtin_offsetof(Amd64WaitResumptionEvidence, ObservedInterrupts) == 16U);
static_assert(__builtin_offsetof(Amd64WaitResumptionEvidence, DeliveredCount) == 24U);
static_assert(__builtin_offsetof(Amd64WaitResumptionEvidence, DrainStop) == 32U);
static_assert(__builtin_offsetof(Amd64WaitResumptionEvidence, DeliveredOwnTimer) == 40U);
static_assert(__builtin_offsetof(Amd64WaitResumptionEvidence, QueueNextDeadlineTicks) == 48U);
static_assert(__builtin_offsetof(Amd64WaitResumptionEvidence, QueueScheduledEntries) == 56U);
static_assert(__builtin_offsetof(Amd64WaitResumptionEvidence, QueuePendingEntries) == 64U);
static_assert(__builtin_offsetof(Amd64WaitResumptionEvidence, QueueClaimedEntries) == 72U);
static_assert(__builtin_offsetof(Amd64WaitResumptionEvidence, DeadlineState) == 80U);
static_assert(__builtin_offsetof(Amd64WaitResumptionEvidence, CompletedTag) == 88U);
static_assert(__builtin_offsetof(Amd64WaitResumptionEvidence, CompletedRegistrationSequence) == 96U);
static_assert(__builtin_offsetof(Amd64WaitResumptionEvidence, ResponseError) == 104U);
static_assert(__builtin_offsetof(Amd64WaitResumptionEvidence, ResponseValue0) == 112U);
static_assert(__builtin_offsetof(Amd64WaitResumptionEvidence, ResponseValue1) == 120U);
static_assert(sizeof(Amd64WaitResumptionEvidence) == 128U);
static_assert(__builtin_offsetof(Amd64WaitEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Amd64WaitEvidence, RegisteredTag) == 8U);
static_assert(__builtin_offsetof(Amd64WaitEvidence, WaitCalls) == 16U);
static_assert(__builtin_offsetof(Amd64WaitEvidence, SuspendedWaits) == 24U);
static_assert(__builtin_offsetof(Amd64WaitEvidence, ResumedWaits) == 32U);
static_assert(__builtin_offsetof(Amd64WaitEvidence, Resumptions) == 40U);
static_assert(sizeof(Amd64WaitEvidence) == 552U);
static_assert(__builtin_offsetof(Amd64EvidenceRecord, Header) == 0U);
static_assert(__builtin_offsetof(Amd64EvidenceRecord, Runtime) == 16U);
static_assert(__builtin_offsetof(Amd64EvidenceRecord, Pci) == 64U);
static_assert(__builtin_offsetof(Amd64EvidenceRecord, Ahci) == 224U);
static_assert(__builtin_offsetof(Amd64EvidenceRecord, ApplicationProcessors) == 800U);
static_assert(__builtin_offsetof(Amd64EvidenceRecord, SharedRootShootdown) == 1656U);
static_assert(__builtin_offsetof(Amd64EvidenceRecord, NativePreemption) == 1800U);
static_assert(__builtin_offsetof(Amd64UserPreemptionEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Amd64UserPreemptionEvidence, UserTicks) == 8U);
static_assert(__builtin_offsetof(Amd64UserPreemptionEvidence, TimerQueueTicks) == 16U);
static_assert(__builtin_offsetof(Amd64UserPreemptionEvidence, RunStops) == 24U);
static_assert(__builtin_offsetof(Amd64UserPreemptionEvidence, SpinnerPresent) == 48U);
static_assert(__builtin_offsetof(Amd64UserPreemptionEvidence, SpinnerLines) == 56U);
static_assert(__builtin_offsetof(Amd64UserPreemptionEvidence, SpinnerWaitSuspended) == 64U);
static_assert(sizeof(Amd64UserPreemptionEvidence) == 72U);
static_assert(__builtin_offsetof(Amd64EvidenceRecord, Wait) == 2256U);
static_assert(__builtin_offsetof(Amd64EvidenceRecord, UserPreemption) == 2808U);
static_assert(__builtin_offsetof(Amd64SupervisionEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Amd64SupervisionEvidence, Terminations) == 8U);
static_assert(__builtin_offsetof(Amd64SupervisionEvidence, SpinnerExited) == 16U);
static_assert(__builtin_offsetof(Amd64SupervisionEvidence, SpinnerExitCode) == 24U);
static_assert(__builtin_offsetof(Amd64SupervisionEvidence, Deferrals) == 32U);
static_assert(sizeof(Amd64SupervisionEvidence) == 40U);
static_assert(__builtin_offsetof(Amd64EvidenceRecord, Supervision) == 2880U);
static_assert(__builtin_offsetof(Amd64ObjectManagementEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Amd64ObjectManagementEvidence, Created) == 8U);
static_assert(__builtin_offsetof(Amd64ObjectManagementEvidence, Destroyed) == 16U);
static_assert(__builtin_offsetof(Amd64ObjectManagementEvidence, Reclaimed) == 24U);
static_assert(__builtin_offsetof(Amd64ObjectManagementEvidence, OccupiedSlots) == 32U);
static_assert(__builtin_offsetof(Amd64ObjectManagementEvidence, MetadataBytes) == 40U);
static_assert(__builtin_offsetof(Amd64ObjectManagementEvidence, IpcQueueBytes) == 48U);
static_assert(__builtin_offsetof(Amd64ObjectManagementEvidence, PeakMetadataBytes) == 56U);
static_assert(__builtin_offsetof(Amd64ObjectManagementEvidence, EndpointMetadataBytes) == 64U);
static_assert(__builtin_offsetof(Amd64ObjectManagementEvidence, RegisteredObjects) == 72U);
static_assert(__builtin_offsetof(Amd64ObjectManagementEvidence, FramesCreated) == 80U);
static_assert(__builtin_offsetof(Amd64ObjectManagementEvidence, FramesReclaimed) == 88U);
static_assert(__builtin_offsetof(Amd64ObjectManagementEvidence, MemoryObjectsCreated) == 96U);
static_assert(__builtin_offsetof(Amd64ObjectManagementEvidence, MemoryObjectsReclaimed) == 104U);
static_assert(__builtin_offsetof(Amd64ObjectManagementEvidence, FrameMetadataBytes) == 112U);
static_assert(__builtin_offsetof(Amd64ObjectManagementEvidence, MemoryObjectMetadataBytes) == 120U);
static_assert(__builtin_offsetof(Amd64ObjectManagementEvidence, CommittedPages) == 128U);
static_assert(__builtin_offsetof(Amd64ObjectManagementEvidence, ResidentPages) == 136U);
static_assert(__builtin_offsetof(Amd64ObjectManagementEvidence, AllocatorCleanPages) == 144U);
static_assert(__builtin_offsetof(Amd64ObjectManagementEvidence, AllocatorDirtyPages) == 152U);
static_assert(__builtin_offsetof(Amd64ObjectManagementEvidence, Mappings) == 160U);
static_assert(__builtin_offsetof(Amd64ObjectManagementEvidence, MappedVirtualStart) == 168U);
static_assert(__builtin_offsetof(Amd64ObjectManagementEvidence, MappedPhysicalStart) == 176U);
static_assert(__builtin_offsetof(Amd64ObjectManagementEvidence, MappedPages) == 184U);
static_assert(__builtin_offsetof(Amd64ObjectManagementEvidence, Unmappings) == 192U);
static_assert(sizeof(Amd64ObjectManagementEvidence) == 200U);
static_assert(__builtin_offsetof(Amd64EvidenceRecord, ObjectManagement) == 2920U);
static_assert(__builtin_offsetof(Amd64ThreadEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Amd64ThreadEvidence, Created) == 8U);
static_assert(__builtin_offsetof(Amd64ThreadEvidence, Destroyed) == 16U);
static_assert(__builtin_offsetof(Amd64ThreadEvidence, Reclaimed) == 24U);
static_assert(__builtin_offsetof(Amd64ThreadEvidence, Exits) == 32U);
static_assert(__builtin_offsetof(Amd64ThreadEvidence, Reaped) == 40U);
static_assert(__builtin_offsetof(Amd64ThreadEvidence, Lines) == 48U);
static_assert(__builtin_offsetof(Amd64ThreadEvidence, LastExitCode) == 56U);
static_assert(__builtin_offsetof(Amd64ThreadEvidence, FreeSlots) == 64U);
static_assert(__builtin_offsetof(Amd64ThreadEvidence, FreeNativeSlots) == 72U);
static_assert(__builtin_offsetof(Amd64ThreadEvidence, ThreadsUsage) == 80U);
static_assert(__builtin_offsetof(Amd64ThreadEvidence, RunStops) == 88U);
static_assert(__builtin_offsetof(Amd64ThreadEvidence, DestroyPolls) == 96U);
static_assert(__builtin_offsetof(Amd64ThreadEvidence, SlotMetadataBytes) == 104U);
static_assert(__builtin_offsetof(Amd64ThreadEvidence, Faults) == 112U);
static_assert(__builtin_offsetof(Amd64ThreadEvidence, Deferrals) == 120U);
static_assert(__builtin_offsetof(Amd64ThreadEvidence, Exceptions) == 128U);
static_assert(__builtin_offsetof(Amd64ThreadEvidence, Kills) == 136U);
static_assert(sizeof(Amd64ThreadEvidence) == 144U);
static_assert(__builtin_offsetof(Amd64EvidenceRecord, Threads) == 3120U);
static_assert(__builtin_offsetof(Amd64ComponentEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Amd64ComponentEvidence, AddressSpacesCreated) == 8U);
static_assert(__builtin_offsetof(Amd64ComponentEvidence, AddressSpacesLive) == 16U);
static_assert(__builtin_offsetof(Amd64ComponentEvidence, CapabilitySpacesCreated) == 24U);
static_assert(__builtin_offsetof(Amd64ComponentEvidence, CapabilitySpacesLive) == 32U);
static_assert(__builtin_offsetof(Amd64ComponentEvidence, DomainsCreated) == 40U);
static_assert(__builtin_offsetof(Amd64ComponentEvidence, DomainsDestroyed) == 48U);
static_assert(__builtin_offsetof(Amd64ComponentEvidence, DomainsReclaimed) == 56U);
static_assert(__builtin_offsetof(Amd64ComponentEvidence, TaggedFaults) == 64U);
static_assert(__builtin_offsetof(Amd64ComponentEvidence, LastTag) == 72U);
static_assert(__builtin_offsetof(Amd64ComponentEvidence, DomainExits) == 80U);
static_assert(__builtin_offsetof(Amd64ComponentEvidence, AddressSpaceBytes) == 88U);
static_assert(__builtin_offsetof(Amd64ComponentEvidence, CapabilitySpaceBytes) == 96U);
static_assert(__builtin_offsetof(Amd64ComponentEvidence, AddressSpacesReclaimed) == 104U);
static_assert(__builtin_offsetof(Amd64ComponentEvidence, CapabilitySpacesReclaimed) == 112U);
static_assert(__builtin_offsetof(Amd64ComponentEvidence, ArenasPrepared) == 120U);
static_assert(__builtin_offsetof(Amd64ComponentEvidence, AddressSpacesFree) == 128U);
static_assert(__builtin_offsetof(Amd64ComponentEvidence, CapabilitySpacesFree) == 136U);
static_assert(__builtin_offsetof(Amd64ComponentEvidence, WaitExpiries) == 144U);
static_assert(__builtin_offsetof(Amd64ComponentEvidence, ResourceDomainsCreated) == 152U);
static_assert(__builtin_offsetof(Amd64ComponentEvidence, ResourceDomainsDestroyed) == 160U);
static_assert(__builtin_offsetof(Amd64ComponentEvidence, ResourceDomainsReclaimed) == 168U);
static_assert(__builtin_offsetof(Amd64ComponentEvidence, ResourceDomainsLive) == 176U);
static_assert(sizeof(Amd64ComponentEvidence) == 184U);
static_assert(__builtin_offsetof(Amd64EvidenceRecord, Components) == 3264U);
static_assert(__builtin_offsetof(Amd64ServiceEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Amd64ServiceEvidence, WaitSetsCreated) == 8U);
static_assert(__builtin_offsetof(Amd64ServiceEvidence, WaitSetsReclaimed) == 16U);
static_assert(__builtin_offsetof(Amd64ServiceEvidence, Binds) == 24U);
static_assert(__builtin_offsetof(Amd64ServiceEvidence, WaitsBlocked) == 32U);
static_assert(__builtin_offsetof(Amd64ServiceEvidence, WaitExpiries) == 40U);
static_assert(__builtin_offsetof(Amd64ServiceEvidence, WaitAborts) == 48U);
static_assert(__builtin_offsetof(Amd64ServiceEvidence, AbortedExits) == 56U);
static_assert(sizeof(Amd64ServiceEvidence) == 64U);
static_assert(__builtin_offsetof(Amd64EvidenceRecord, Services) == 3448U);
static_assert(__builtin_offsetof(Amd64IommuEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Amd64IommuEvidence, SelectionError) == 8U);
static_assert(__builtin_offsetof(Amd64IommuEvidence, RegisterBase) == 16U);
static_assert(__builtin_offsetof(Amd64IommuEvidence, ServedFunctions) == 24U);
static_assert(__builtin_offsetof(Amd64IommuEvidence, SweptFunctions) == 32U);
static_assert(__builtin_offsetof(Amd64IommuEvidence, ConfigureError) == 40U);
static_assert(__builtin_offsetof(Amd64IommuEvidence, EnableError) == 48U);
static_assert(__builtin_offsetof(Amd64IommuEvidence, Enabled) == 56U);
static_assert(__builtin_offsetof(Amd64IommuEvidence, Levels) == 64U);
static_assert(__builtin_offsetof(Amd64IommuEvidence, Version) == 72U);
static_assert(__builtin_offsetof(Amd64IommuEvidence, Capability) == 80U);
static_assert(__builtin_offsetof(Amd64IommuEvidence, ExtendedCapability) == 88U);
static_assert(__builtin_offsetof(Amd64IommuEvidence, HandoffStatus) == 96U);
static_assert(__builtin_offsetof(Amd64IommuEvidence, Status) == 104U);
static_assert(__builtin_offsetof(Amd64IommuEvidence, Faults) == 112U);
static_assert(__builtin_offsetof(Amd64IommuEvidence, Overflows) == 120U);
static_assert(__builtin_offsetof(Amd64IommuEvidence, Unattributed) == 128U);
static_assert(__builtin_offsetof(Amd64IommuEvidence, LastFaultSource) == 136U);
static_assert(__builtin_offsetof(Amd64IommuEvidence, LastFaultReason) == 144U);
static_assert(__builtin_offsetof(Amd64IommuEvidence, LastFaultAddress) == 152U);
static_assert(__builtin_offsetof(Amd64IommuEvidence, LastFaultRead) == 160U);
static_assert(__builtin_offsetof(Amd64IommuEvidence, DomainsCreated) == 168U);
static_assert(__builtin_offsetof(Amd64IommuEvidence, DomainsReclaimed) == 176U);
static_assert(sizeof(Amd64IommuEvidence) == 184U);
static_assert(__builtin_offsetof(Amd64EvidenceRecord, Iommu) == 3512U);
static_assert(__builtin_offsetof(Amd64DeviceInterruptEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Amd64DeviceInterruptEvidence, Created) == 8U);
static_assert(__builtin_offsetof(Amd64DeviceInterruptEvidence, Destroyed) == 16U);
static_assert(__builtin_offsetof(Amd64DeviceInterruptEvidence, Reclaimed) == 24U);
static_assert(__builtin_offsetof(Amd64DeviceInterruptEvidence, Live) == 32U);
static_assert(__builtin_offsetof(Amd64DeviceInterruptEvidence, Delivered) == 40U);
static_assert(__builtin_offsetof(Amd64DeviceInterruptEvidence, Spurious) == 48U);
static_assert(sizeof(Amd64DeviceInterruptEvidence) == 56U);
static_assert(__builtin_offsetof(Amd64EvidenceRecord, Interrupts) == 3696U);
static_assert(__builtin_offsetof(Amd64DeviceControlEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Amd64DeviceControlEvidence, Assignments) == 8U);
static_assert(__builtin_offsetof(Amd64DeviceControlEvidence, Releases) == 16U);
static_assert(__builtin_offsetof(Amd64DeviceControlEvidence, BusMasterRefusals) == 24U);
static_assert(__builtin_offsetof(Amd64DeviceControlEvidence, Quarantines) == 32U);
static_assert(__builtin_offsetof(Amd64DeviceControlEvidence, Confirmations) == 40U);
static_assert(__builtin_offsetof(Amd64DeviceControlEvidence, PinsParked) == 48U);
static_assert(__builtin_offsetof(Amd64DeviceControlEvidence, PinsReleased) == 56U);
static_assert(__builtin_offsetof(Amd64DeviceControlEvidence, ParkedPins) == 64U);
static_assert(sizeof(Amd64DeviceControlEvidence) == 72U);
static_assert(__builtin_offsetof(Amd64EvidenceRecord, DeviceControl) == 3752U);
static_assert(__builtin_offsetof(Amd64CollectionEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Amd64CollectionEvidence, Collections) == 8U);
static_assert(__builtin_offsetof(Amd64CollectionEvidence, Reclaimed) == 16U);
static_assert(__builtin_offsetof(Amd64CollectionEvidence, ScrubRuns) == 24U);
static_assert(__builtin_offsetof(Amd64CollectionEvidence, ScrubbedPages) == 32U);
static_assert(sizeof(Amd64CollectionEvidence) == 40U);
static_assert(__builtin_offsetof(Amd64EvidenceRecord, Collection) == 3824U);
static_assert(__builtin_offsetof(Amd64ParkedReleaseEvidence, Frame) == 0U);
static_assert(__builtin_offsetof(Amd64ParkedReleaseEvidence, PhysicalStart) == 8U);
static_assert(__builtin_offsetof(Amd64ParkedReleaseEvidence, Pages) == 16U);
static_assert(__builtin_offsetof(Amd64ParkedReleaseEvidence, Sampled) == 24U);
static_assert(__builtin_offsetof(Amd64ParkedReleaseEvidence, Sample) == 32U);
static_assert(sizeof(Amd64ParkedReleaseEvidence) == 64U);
static_assert(__builtin_offsetof(Amd64BlockEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Amd64BlockEvidence, Releases) == 8U);
static_assert(__builtin_offsetof(Amd64BlockEvidence, Released) == 16U);
static_assert(sizeof(Amd64BlockEvidence) == 144U);
static_assert(__builtin_offsetof(Amd64EvidenceRecord, Block) == 3864U);
static_assert(__builtin_offsetof(Amd64EvidenceRecord, Stops) == 4008U);
static_assert(__builtin_offsetof(Amd64IommuUnitEvidence, RegisterBase) == 0U);
static_assert(__builtin_offsetof(Amd64IommuUnitEvidence, State) == 8U);
static_assert(__builtin_offsetof(Amd64IommuUnitEvidence, ConfigureError) == 16U);
static_assert(__builtin_offsetof(Amd64IommuUnitEvidence, EnableError) == 24U);
static_assert(__builtin_offsetof(Amd64IommuUnitEvidence, SweptFunctions) == 32U);
static_assert(__builtin_offsetof(Amd64IommuUnitEvidence, ServedFunctions) == 40U);
static_assert(__builtin_offsetof(Amd64IommuUnitEvidence, ReservedFunctions) == 48U);
static_assert(__builtin_offsetof(Amd64IommuUnitEvidence, ReservedPages) == 56U);
static_assert(__builtin_offsetof(Amd64IommuUnitEvidence, Levels) == 64U);
static_assert(__builtin_offsetof(Amd64IommuUnitEvidence, Version) == 72U);
static_assert(__builtin_offsetof(Amd64IommuUnitEvidence, Capability) == 80U);
static_assert(__builtin_offsetof(Amd64IommuUnitEvidence, ExtendedCapability) == 88U);
static_assert(__builtin_offsetof(Amd64IommuUnitEvidence, HandoffStatus) == 96U);
static_assert(__builtin_offsetof(Amd64IommuUnitEvidence, Status) == 104U);
static_assert(__builtin_offsetof(Amd64IommuUnitEvidence, Faults) == 112U);
static_assert(__builtin_offsetof(Amd64IommuUnitEvidence, Overflows) == 120U);
static_assert(__builtin_offsetof(Amd64IommuUnitEvidence, Unattributed) == 128U);
static_assert(__builtin_offsetof(Amd64IommuUnitEvidence, DisabledReason) == 136U);
static_assert(__builtin_offsetof(Amd64IommuUnitEvidence, DroppedRequesters) == 144U);
static_assert(__builtin_offsetof(Amd64IommuTopologyEvidence, IgnoredScopes) == 16U);
static_assert(__builtin_offsetof(Amd64IommuTopologyEvidence, MalformedRmrr) == 24U);
static_assert(sizeof(Amd64IommuUnitEvidence) == 152U);
static_assert(__builtin_offsetof(Amd64IommuTopologyEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Amd64IommuTopologyEvidence, Count) == 8U);
static_assert(__builtin_offsetof(Amd64IommuTopologyEvidence, Units) == 32U);
static_assert(sizeof(Amd64IommuTopologyEvidence) == 640U);
static_assert(__builtin_offsetof(Amd64EvidenceRecord, IommuTopology) == 4600U);
static_assert(__builtin_offsetof(Amd64EvidenceRecord, TscFrequencyHz) == 5240U);
static_assert(__builtin_offsetof(Amd64EvidenceRecord, TscSource) == 5248U);
static_assert(__builtin_offsetof(Amd64EvidenceRecord, X2Apic) == 5256U);
static_assert(__builtin_offsetof(Amd64EvidenceRecord, IommuBackend) == 5264U);
static_assert(__builtin_offsetof(Amd64IommuBackendEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Amd64IommuBackendEvidence, Kind) == 8U);
static_assert(__builtin_offsetof(Amd64IommuBackendEvidence, MalformedIvrs) == 16U);
static_assert(__builtin_offsetof(Amd64IommuBackendEvidence, SkippedHid) == 24U);
static_assert(__builtin_offsetof(Amd64IommuBackendEvidence, IvhdType) == 32U);
static_assert(sizeof(Amd64IommuBackendEvidence) == 40U);
static_assert(__builtin_offsetof(Amd64EvidenceRecord, BootFiles) == 5304U);
static_assert(__builtin_offsetof(Amd64MetadataEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Amd64MetadataEvidence, ArenaPhysicalStart) == 8U);
static_assert(__builtin_offsetof(Amd64MetadataEvidence, ArenaPages) == 16U);
static_assert(__builtin_offsetof(Amd64MetadataEvidence, SlabFirstPage) == 24U);
static_assert(__builtin_offsetof(Amd64MetadataEvidence, BitmapFirstPage) == 32U);
static_assert(__builtin_offsetof(Amd64MetadataEvidence, SeedTablePages) == 40U);
static_assert(__builtin_offsetof(Amd64MetadataEvidence, RegistryCapacity) == 48U);
static_assert(__builtin_offsetof(Amd64MetadataEvidence, FrameSlots) == 56U);
static_assert(__builtin_offsetof(Amd64MetadataEvidence, MemoryObjectSlots) == 64U);
static_assert(sizeof(Amd64MetadataEvidence) == 72U);
static_assert(__builtin_offsetof(Amd64EvidenceRecord, Metadata) == 6368U);
static_assert(__builtin_offsetof(Amd64AllocatorEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Amd64AllocatorEvidence, ManagedPages) == 8U);
static_assert(__builtin_offsetof(Amd64AllocatorEvidence, AllocatablePages) == 16U);
static_assert(__builtin_offsetof(Amd64AllocatorEvidence, CleanPages) == 24U);
static_assert(__builtin_offsetof(Amd64AllocatorEvidence, DirtyPages) == 32U);
static_assert(__builtin_offsetof(Amd64AllocatorEvidence, ClaimedPages) == 40U);
static_assert(__builtin_offsetof(Amd64AllocatorEvidence, RetiredPages) == 48U);
static_assert(__builtin_offsetof(Amd64AllocatorEvidence, AllocatedPages) == 56U);
static_assert(__builtin_offsetof(Amd64AllocatorEvidence, UnmanagedPages) == 64U);
static_assert(__builtin_offsetof(Amd64AllocatorEvidence, FreeLowBlocks) == 72U);
static_assert(__builtin_offsetof(Amd64AllocatorEvidence, ExcludedDmaPages) == 80U);
static_assert(__builtin_offsetof(Amd64AllocatorEvidence, ExcludedLoaderPages) == 88U);
static_assert(__builtin_offsetof(Amd64AllocatorEvidence, ExcludedFirmwarePages) == 96U);
static_assert(__builtin_offsetof(Amd64AllocatorEvidence, BootServicesImported) == 104U);
static_assert(__builtin_offsetof(Amd64AllocatorEvidence, ImportRefusal) == 112U);
static_assert(__builtin_offsetof(Amd64AllocatorEvidence, ZeroingSlots) == 120U);
static_assert(__builtin_offsetof(Amd64AllocatorEvidence, PeakZeroingSlots) == 128U);
static_assert(__builtin_offsetof(Amd64AllocatorEvidence, ZeroDeferrals) == 136U);
static_assert(__builtin_offsetof(Amd64AllocatorEvidence, ZeroFaults) == 144U);
static_assert(__builtin_offsetof(Amd64AllocatorEvidence, CreationZeroedPages) == 152U);
static_assert(__builtin_offsetof(Amd64AllocatorEvidence, CreationZeroRoot) == 160U);
static_assert(__builtin_offsetof(Amd64AllocatorEvidence, HandoffRecords) == 168U);
static_assert(sizeof(Amd64AllocatorEvidence) == 176U);
static_assert(__builtin_offsetof(Amd64EvidenceRecord, Allocator) == 6440U);
static_assert(__builtin_offsetof(Amd64EvidenceRecord, Smp) == 15976U);
static_assert(__builtin_offsetof(Amd64EvidenceRecord, MemoryImport) == 6616U);
static_assert(sizeof(Amd64EvidenceRecord) == 19160U);
static_assert(__builtin_offsetof(Amd64EvidenceRecord, ApplicationLaunch) == 16096U);
static_assert(__builtin_offsetof(Amd64EvidenceRecord, Watchdog) == 16376U);
static_assert(__builtin_offsetof(Amd64EvidenceRecord, Clocks) == 16600U);
static_assert(__builtin_offsetof(Amd64EvidenceRecord, UserProcessors) == 16760U);
static_assert(sizeof(Amd64EvidenceRecord) == __builtin_offsetof(Amd64EvidenceRecord, UserProcessors) +
                                                 sizeof(Kernel::Diagnostics::UserProcessorEvidence));
} // Namespace Asteria::Platform::Amd64::Diagnostics

#endif // ASTERIA_PLATFORM_AMD64_DIAGNOSTICS_AMD64_EVIDENCE_RECORD_HPP_INCLUDED
