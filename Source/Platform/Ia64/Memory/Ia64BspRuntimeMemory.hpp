// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_MEMORY_IA64_BSP_RUNTIME_MEMORY_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_MEMORY_IA64_BSP_RUNTIME_MEMORY_HPP_INCLUDED

#include "Foundation/Compiler.hpp"
#include "Platform/Ia64/Diagnostics/Ia64Stop.hpp"
#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Memory/ProcessorAddressSpaceController.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Scheduler/SchedulerTypes.hpp"
#include "Kernel/Scheduler/ThreadPool.hpp"
#include "Platform/Ia64/Interrupt/Ia64ExternalInterruptEntry.hpp"
#include "Platform/Ia64/Interrupt/Ia64ExternalPreemptionEntry.hpp"
#include "Platform/Ia64/Interrupt/Ia64PageFaultEntryContract.hpp"
#include "Platform/Ia64/Interrupt/Ia64UserFaultEntry.hpp"
#include "Platform/Ia64/Memory/Ia64ActiveRuntimeTranslationBindings.hpp"
#include "Platform/Ia64/Memory/Ia64RegionIdentifierAllocator.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeAddressSpacePageTable.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeAddressSpaceOwner.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeAddressSpaceSlots.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeInterruptVectorTable.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeProcessorCapabilities.hpp"
#include "Platform/Ia64/System/Ia64SystemCallEntry.hpp"

namespace Asteria::Platform::Ia64::Memory
{
class InitialAddressSpacePreparation;
enum class InitialAddressSpacePreparationError : Foundation::UInt32;

} // Namespace Asteria::Platform::Ia64::Memory

namespace Asteria::Kernel::Object
{
class RootCapabilityIssuer;
}

namespace Asteria::Platform::Ia64::Memory
{

enum class Ia64BspRuntimeMemoryState : Foundation::UInt8
{
    Fresh,
    Failed,
    Ready,
};

enum class Ia64BspRuntimeMemoryError : Foundation::UInt8
{
    None,
    AlreadyPrepared,
    PreviousPreparationFailed,
    InvalidPlacement,
    InterruptVectorTablePreflightRejected,
    ProcessorCapabilitiesRejected,
    RegionIdentifierAllocatorRejected,
    RegionIdentifierAllocationRejected,
    AddressSpaceBackendRejected,
    MappingPlanRetrievalRejected,
    AddressSpaceInitializationRejected,
    InterruptVectorTableInstallRejected,
    ObjectRegistrationRejected,
    AddressSpaceIdentityBindingRejected,
    ProcessorControllerActivationRejected,
    PostconditionRejected,
    LedgerPostconditionRejected,
    InitialComponentAddressSpaceRejected,
};

enum class Ia64BspRuntimeBindingState : Foundation::UInt8
{
    Unadmitted,
    AdmittedWithoutLedger,
    Active,
    Invalid,
};

enum class Ia64BspRuntimeMemoryPostconditionError : Foundation::UInt8
{
    None,
    RegionIdentifierReceiptMismatch,
    RegionIdentifierAllocatorSnapshotMismatch,
    AddressSpaceSnapshotMismatch,
    AddressSpaceRegistryBindingMismatch,
    ProcessorControllerReceiptMismatch,
    ProcessorControllerSnapshotMismatch,
    ProcessorProfileMismatch,
    ActiveLedgerMismatch,
};

enum class Ia64BspRuntimeRootBorrowError : Foundation::UInt8
{
    None,
    NotReady,
    InvariantMismatch,
};

inline constexpr Foundation::Size Ia64BspRuntimeMaximumApplicationProcessorCount{
    Kernel::Scheduler::MaximumProcessorCount - 1U};

enum class Ia64BspRuntimeApplicationProcessorAdmissionState : Foundation::UInt8
{
    Unadmitted,
    Admitting,
    Failed,
    Ready,
};

enum class Ia64BspRuntimeApplicationProcessorAdmissionError : Foundation::UInt8
{
    None,
    NotReady,
    InvalidProcessor,
    BspProcessor,
    ProcessorNotProfiled,
    OutOfOrder,
    AlreadyAdmitted,
    AdmissionInProgress,
    PreviousAdmissionFailed,
    RetainedPreparationMissing,
    RuntimeObjectRangeMismatch,
    RootInvariantMismatch,
    InterruptVectorTablePreflightRejected,
    InterruptVectorTableInstallRejected,
    PhysicalBootstrapActivationRejected,
    ProcessorControllerActivationRejected,
    PostconditionRejected,
    LedgerPostconditionRejected,
};

enum class Ia64BspRuntimeApplicationProcessorPostconditionError : Foundation::UInt8
{
    None,
    InterruptVectorTableSummaryMismatch,
    AddressSpaceSnapshotMismatch,
    BindingMismatch,
    ControllerReceiptMismatch,
    ControllerSnapshotMismatch,
};

enum class Ia64BspRuntimeSystemCallPublicationError : Foundation::UInt8
{
    None,
    NotReady,
    InvalidProcessor,
    ProcessorMismatch,
    InvalidBridgeContextAddress,
    ContextValidationRejected,
    ContextOutsideRuntimeObjectRange,
    ReferencedStorageOutsideRuntimeObjectRange,
    AlreadyPublished,
    InterruptVectorTableRejected,
};

enum class Ia64BspRuntimeExternalInterruptPublicationError : Foundation::UInt8
{
    None,
    NotReady,
    InvalidProcessor,
    ProcessorMismatch,
    ContextOutsideRuntimeObjectRange,
    EvidenceOutsideRuntimeObjectRange,
    AlreadyPublished,
    InterruptVectorTableRejected,
};

enum class Ia64BspRuntimeExternalInterruptAcknowledgementError : Foundation::UInt8
{
    None,
    NotReady,
    InvalidProcessor,
    NotProfiled,
    NotResident,
    NotPublished,
    PublicationMismatch,
    InvalidSequence,
    StaleSequence,
    ForeignSequence,
    AlreadyAcknowledged,
    SequenceExhausted,
    InvalidMailboxState,
};

enum class Ia64BspRuntimeExternalInterruptAccessError : Foundation::UInt8
{
    None,
    NotReady,
    InvalidProcessor,
    NotProfiled,
    NotResident,
    NotPublished,
    PublicationMismatch,
};

enum class Ia64BspRuntimeExternalPreemptionError : Foundation::UInt8
{
    None,
    NotReady,
    InvalidProcessor,
    NotBspProcessor,
    ExternalInterruptNotPublished,
    PublicationMismatch,
    StorageAlreadyBorrowed,
    ControlNotConfigured,
    ControlAlreadyPublished,
};

enum class Ia64BspRuntimePageFaultEntryState : Foundation::UInt8
{
    Fresh,
    Borrowed,
    Published,
    Faulted,
};

enum class Ia64BspRuntimePageFaultEntryError : Foundation::UInt8
{
    None,
    NotReady,
    InvalidProcessor,
    NotBspProcessor,
    StorageAlreadyBorrowed,
    StorageNotBorrowed,
    StorageInvariantMismatch,
    SubstitutedStorage,
    FrameNotPristine,
    ControlNotConfigured,
    ConfigurationMismatch,
    AlreadyPublished,
    InterruptVectorTableRejected,
    PublicationStateMismatch,
};

/// Exclusive view of the BSP's reusable entry corridor. The dedicated memory
/// stack and RBS are disjoint from system-call storage, every Thread, and every
/// retained architecture frame. The owner retains every byte.
struct Ia64BspRuntimeExternalPreemptionStorageView
{
    Interrupt::Ia64ExternalPreemptionControl* Control;
    Foundation::UInt8* CoordinatorMemoryStack;
    Foundation::UInt8* CoordinatorRseBackingStore;
    Foundation::UInt64 CoordinatorMemoryStackAddress;
    Foundation::UInt64 CoordinatorMemoryStackSizeBytes;
    Foundation::UInt64 CoordinatorRseBackingStoreAddress;
    Foundation::UInt64 CoordinatorRseBackingStoreSizeBytes;
};

/// Exclusive view of the BSP-owned one-shot page-fault entry storage. Every
/// returned pointer is non-owning and remains stable for the owner's complete
/// boot lifetime. The coordinator RSE range begins with the mandatory 16-KiB
/// transition headroom; ordinary coordinator frames use only its upper half.
struct Ia64BspRuntimePageFaultEntryStorageView
{
    Interrupt::Ia64PageFaultEntryControl* Control;
    Interrupt::Ia64PageFaultEntryContext* Context;
    Interrupt::Ia64PageFaultFrame* Frame;
    Foundation::UInt8* CoordinatorMemoryStack;
    Foundation::UInt8* CoordinatorRseBackingStore;
    Foundation::UInt64 ControlAddress;
    Foundation::UInt64 ContextAddress;
    Foundation::UInt64 FrameAddress;
    Foundation::UInt64 CoordinatorMemoryStackAddress;
    Foundation::UInt64 CoordinatorMemoryStackSizeBytes;
    Foundation::UInt64 CoordinatorRseBackingStoreAddress;
    Foundation::UInt64 CoordinatorRseTransitionSizeBytes;
    Foundation::UInt64 CoordinatorRseBackingStoreSizeBytes;
};

/// The user domains of the static boot manifest: the initial component and the boot pager. Each has
/// its own private root and its own kernel entry storage.
enum class Ia64BspRuntimeUserDomain : Foundation::UInt8
{
    InitialComponent,
    Pager,
};

inline constexpr Foundation::Size Ia64BspRuntimeUserDomainCount{2U};

/// A user thread's kernel entry storage: the initial component's, the boot pager's, and one for
/// each created-thread slot, matching the kernel's thirty-two thread slots. Created-thread slot n is Thread0 + n
/// (Ia64BspRuntimeThreadEntrySlot).
enum class Ia64BspRuntimeEntrySlot : Foundation::UInt8
{
    InitialComponent,
    Pager,
    Thread0,
};

inline constexpr Foundation::Size Ia64BspRuntimeThreadEntrySlots{32U};
static_assert(Ia64BspRuntimeThreadEntrySlots == Kernel::Scheduler::ThreadPool::Capacity);
inline constexpr Foundation::Size Ia64BspRuntimeEntrySlotCount{
    static_cast<Foundation::Size>(Ia64BspRuntimeEntrySlot::Thread0) + Ia64BspRuntimeThreadEntrySlots};
static_assert(Ia64BspRuntimeEntrySlotCount == 34U);
// Every slot's system-call and user-fault contexts are admitted at boot.
static_assert(Ia64RuntimeInterruptVectorTable::MaximumSelectableEntryContexts == Ia64BspRuntimeEntrySlotCount);

/// The entry slot of a boot domain's thread.
[[nodiscard]] constexpr Ia64BspRuntimeEntrySlot
Ia64BspRuntimeEntrySlotOf(const Ia64BspRuntimeUserDomain domain) noexcept
{
    return (domain == Ia64BspRuntimeUserDomain::Pager) ? Ia64BspRuntimeEntrySlot::Pager
                                                       : Ia64BspRuntimeEntrySlot::InitialComponent;
}

/// The entry slot of created-thread slot `slot` (below
/// Ia64BspRuntimeThreadEntrySlots).
[[nodiscard]] constexpr Ia64BspRuntimeEntrySlot Ia64BspRuntimeThreadEntrySlot(const Foundation::Size slot) noexcept
{
    return static_cast<Ia64BspRuntimeEntrySlot>(static_cast<Foundation::Size>(Ia64BspRuntimeEntrySlot::Thread0) + slot);
}

/// One user domain's kernel entry storage: the memory stack and RSE backing
/// store its system-call and user-fault entries switch to, and its reusable
/// user-fault frame. Every byte lies in the owner's locked corridor.
struct Ia64BspRuntimeUserEntryStorageView
{
    Foundation::UInt8* MemoryStack;
    Foundation::UInt64 MemoryStackStart;
    Foundation::UInt64 MemoryStackSizeBytes;
    Foundation::UInt8* RseBackingStore;
    Foundation::UInt64 RseBackingStoreStart;
    Foundation::UInt64 RseBackingStoreSizeBytes;
    Interrupt::Ia64PageFaultFrame* FaultFrame;
};

struct Ia64BspRuntimeRootView
{
    Kernel::Memory::AddressSpace* AddressSpaceStorage;
    Kernel::Memory::AddressSpaceId AddressSpaceIdentity;
    Kernel::Memory::ProcessorAddressSpaceController* ControllerStorage;
    Kernel::Scheduler::CpuId Processor;
};

struct Ia64BspRuntimeApplicationProcessorAdmissionSummary
{
    Ia64RuntimeInterruptVectorTableInstallSummary InterruptVectorTable;
    Kernel::Memory::AddressSpaceId AddressSpaceIdentity;
    Kernel::Memory::ProcessorAddressSpaceControllerReceipt ControllerReceipt;
    Kernel::Scheduler::CpuId Processor;
    Foundation::ProcessorMask ResidentProcessorMask;
    Foundation::UInt8 InstalledProcessorMask;
    Kernel::Memory::ProcessorAddressSpaceControllerState ControllerState;
    bool PhysicalBootstrapActivationPerformed;
    bool ControllerAcknowledgementPerformed;
    bool ExactLedgerVerified;
    Foundation::UInt8 Reserved;
};

struct Ia64BspRuntimeApplicationProcessorAdmissionFailure
{
    Ia64BspRuntimeApplicationProcessorAdmissionError Error;
    Ia64RuntimeInterruptVectorTableInstallError InterruptVectorTableError;
    Hal::Memory::AddressSpacePageTableActivationError PhysicalBootstrapActivationError;
    Ia64BspRuntimeApplicationProcessorPostconditionError PostconditionError;
    Kernel::Memory::ProcessorAddressSpaceControllerFailure ControllerFailure;
    bool InterruptVectorTableCommitted;
    bool PhysicalRootMayBeActive;
    Foundation::UInt8 Reserved[2U];
};

struct Ia64BspRuntimeApplicationProcessorAdmissionSnapshot
{
    Ia64BspRuntimeApplicationProcessorAdmissionSummary Summary;
    Ia64BspRuntimeApplicationProcessorAdmissionFailure Failure;
    Kernel::Memory::ProcessorAddressSpaceControllerSnapshot Controller;
    Ia64BspRuntimeApplicationProcessorAdmissionState State;
    Foundation::UInt8 Reserved[7U];
};

struct Ia64BspRuntimeMemorySummary
{
    Ia64RuntimeObjectRange RuntimeObjects;
    Ia64RuntimeInterruptVectorTableInstallSummary InterruptVectorTable;
    Ia64CommonRuntimeProcessorCapabilities CommonCapabilities;
    Kernel::Memory::AddressSpaceId AddressSpaceIdentity;
    Kernel::Memory::ProcessorAddressSpaceControllerReceipt ControllerReceipt;
    /// Mapping count captured during preparation. The
    /// AddressSpace snapshot exposes the live mapping count.
    Foundation::Size MappingCount;
    Foundation::UInt32 FirstRegionIdentifier;
    Foundation::UInt32 LastRegionIdentifier;
    Foundation::UInt8 BspProcessorIndex;
    Foundation::UInt8 ProfiledProcessorMask;
    Foundation::ProcessorMask ResidentProcessorMask;
    Kernel::Memory::ProcessorAddressSpaceControllerState ControllerState;
    bool BackendClaimed;
    bool ExactLedgerVerified;
    Foundation::UInt8 Reserved[2U];
};

struct Ia64BspRuntimeMemoryFailure
{
    Ia64BspRuntimeMemoryError Error;
    Ia64RuntimeInterruptVectorTableInstallError InterruptVectorTableError;
    Ia64RuntimeProcessorCapabilitiesError ProcessorCapabilitiesError;
    Ia64RegionIdentifierAllocatorError RegionIdentifierAllocatorError;
    Ia64RuntimeAddressSpacePageTableInitializationError AddressSpaceBackendError;
    Kernel::Memory::AddressSpaceError AddressSpaceInitializationError;
    Kernel::Object::ObjectRegistryError ObjectRegistryError;
    Ia64BspRuntimeMemoryPostconditionError PostconditionError;
    InitialAddressSpacePreparationError MappingPlanError;
    Kernel::Memory::AddressSpaceFailure AddressSpaceIdentityBindingFailure;
    Kernel::Memory::ProcessorAddressSpaceControllerFailure ControllerFailure;
};

struct Ia64BspRuntimeSystemCallPublicationSnapshot
{
    System::Ia64SystemCallEntryContext Context;
    Foundation::UInt64 ContextAddress;
    Ia64BspRuntimeSystemCallPublicationError LastError;
    System::Ia64SystemCallContextValidationError LastContextValidationError;
    Ia64RuntimeInterruptVectorTablePublicationError LastInterruptVectorTableError;
    bool Published;
    Foundation::UInt8 Reserved[4U];
};

struct Ia64BspRuntimeExternalInterruptPublicationSnapshot
{
    Interrupt::Ia64ExternalInterruptEntryContext Context;
    Interrupt::Ia64LocalTimerInterruptEvidence Evidence;
    Foundation::UInt64 ContextAddress;
    Foundation::UInt64 EvidenceAddress;
    Ia64BspRuntimeExternalInterruptPublicationError LastError;
    Ia64RuntimeExternalInterruptPublicationError LastInterruptVectorTableError;
    Ia64BspRuntimeExternalInterruptAcknowledgementError LastAcknowledgementError;
    bool Published;
    Foundation::UInt8 Reserved[4U];
};

struct Ia64BspRuntimePageFaultEntrySnapshot
{
    Interrupt::Ia64PageFaultEntryContext Context;
    Foundation::UInt64 ContextAddress;
    Foundation::UInt64 ControlAddress;
    Foundation::UInt64 FrameAddress;
    Ia64BspRuntimePageFaultEntryError LastError;
    Ia64RuntimePageFaultPublicationError LastInterruptVectorTableError;
    Ia64BspRuntimePageFaultEntryState State;
    Foundation::UInt8 Reserved[5U];
};

/// One processor-local immutable entry context, mutable ISR evidence mailbox,
/// and normal-context diagnostics. Context and Published become immutable once
/// publication succeeds. Reserved remains zero for the complete owner lifetime.
struct alignas(16U) Ia64BspRuntimeExternalInterruptTransportSlot
{
    Interrupt::Ia64ExternalInterruptEntryContext Context;
    Interrupt::Ia64LocalTimerInterruptEvidence Evidence;
    Ia64BspRuntimeExternalInterruptPublicationError LastError;
    Ia64RuntimeExternalInterruptPublicationError LastInterruptVectorTableError;
    Ia64BspRuntimeExternalInterruptAcknowledgementError LastAcknowledgementError;
    bool Published;
    Foundation::UInt8 Reserved[12U];
};

struct Ia64BspRuntimeMemorySnapshot
{
    Ia64BspRuntimeMemorySummary Summary;
    Ia64BspRuntimeMemoryFailure Failure;
    Ia64BspRuntimeSystemCallPublicationSnapshot SystemCallPublication;
    Ia64BspRuntimeExternalInterruptPublicationSnapshot ExternalInterruptPublication;
    Ia64BspRuntimePageFaultEntrySnapshot PageFaultEntry;
    Ia64RegionIdentifierAllocatorSnapshot RegionIdentifierAllocator;
    const Kernel::Memory::AddressSpace* AddressSpaceStorage;
    Kernel::Memory::AddressSpaceSnapshot AddressSpace;
    Kernel::Memory::ProcessorAddressSpaceControllerSnapshot Controller;
    Ia64BspRuntimeBindingState Bindings[Kernel::Scheduler::MaximumProcessorCount];
    Ia64BspRuntimeMemoryState State;
    Foundation::UInt8 Reserved[3U];
};

/// Owns the BSP bootstrap/idle root and its capability, RID, binding, backend, controller, and IVT
/// state. Placement and lifetime preconditions: - The complete object starts on a 4-KiB page
/// boundary, remains immovable, and occupies boot-lifetime storage wholly in numerical IA-64 Region
/// 0. - Its exact complete object range has writable identity-Global mapping and initial
/// data-translation-register coverage supplied by preparation. - The object, preparation,
/// preparation's sealed plans, supplied registry, and issuer remain in boot-lifetime storage. The
/// issuer belongs to that exact registry. None may reside on a target stack. - The supplied
/// registry and its backing synchronization remain resident in writable identity-Global Region-0
/// bootstrap coverage. A physical AP may acquire that registry lock while validating the retained
/// root identity. Serialization and publication: - The boot coordinator externally serializes
/// PrepareAndAdmit, calls it on the non-preemptible BSP after the final SAL handoff, and excludes
/// ISR use, migration, and same-CPU re-entry for the complete transaction. - Once this owner
/// registers its AddressSpace generation, the coordinator excludes DestroyObject, quiescence, and
/// collection for that exact generation throughout this owner's boot lifetime. The registry lock is
/// never held by a CPU that calls into this owner or waits for a physical AP executing
/// AdmitApplicationProcessorPhysicalRoot. Liveness probes are diagnostics under this lifecycle
/// contract, not transaction guards. - Exactly one Ia64BspRuntimeMemory transaction per boot may
/// pass processor- capability initialization and become the authoritative RID namespace for that
/// complete boot lifetime. This class has no singleton/global enforcement. - A failed
/// side-effect-free IVT preflight leaves this object Fresh and retryable. Once that preflight
/// succeeds, every failure is terminal and retains all possibly committed state without rollback,
/// destruction, quiescence, or reclamation. - The coordinator publishes the object only after Ready
/// is returned. On the same serialized BSP path, it may then bind the dedicated break-entry
/// transport exactly once before any break consumer can execute. - Later, enabled non-BSP
/// processors are admitted in increasing CpuId order by AdmitApplicationProcessorPhysicalRoot while
/// each selected AP still executes physically. Calls for all processors are externally serialized.
/// This owner performs no translated pivot, firmware release, or IPI work. Allocation: - Does not
/// dynamically allocate. The dedicated RSE range contains the shared-contract 16-KiB transition
/// workspace followed by 16 KiB of kernel backing-store capacity. All loops are bounded by four
/// processors.
class alignas(Kernel::Memory::MappingPageSizeBytes) Ia64BspRuntimeMemory final
{
public:
    constexpr Ia64BspRuntimeMemory() noexcept = default;
    [[nodiscard]] Diagnostics::Ia64StopStorage& StopStorage() noexcept
    {
        return m_stopStorage;
    }
    ~Ia64BspRuntimeMemory() noexcept = default;

    Ia64BspRuntimeMemory(const Ia64BspRuntimeMemory&) = delete;
    Ia64BspRuntimeMemory& operator=(const Ia64BspRuntimeMemory&) = delete;
    Ia64BspRuntimeMemory(Ia64BspRuntimeMemory&&) = delete;
    Ia64BspRuntimeMemory& operator=(Ia64BspRuntimeMemory&&) = delete;

    /// Builds one immutable capability profile from exactly the processors in
    /// profiledProcessorMask. The mask must contain bspProcessor; disabled
    /// array entries are ignored. Common capability fields are intersections
    /// or minima. Only bspProcessor is admitted to the active translation
    /// binding, controller, and root residency during this transaction.
    [[nodiscard]] Foundation::Result<Ia64BspRuntimeMemorySummary, Ia64BspRuntimeMemoryError> PrepareAndAdmit(
        Kernel::Scheduler::CpuId bspProcessor, Foundation::UInt8 profiledProcessorMask,
        const PalVirtualMemoryCapabilities (&processorCapabilities)[Kernel::Scheduler::MaximumProcessorCount],
        const InitialAddressSpacePreparation& preparation, Kernel::Object::ResourceDomainId chargeOwner,
        Kernel::Object::ObjectRegistry& registry, Kernel::Object::RootCapabilityIssuer& issuer) noexcept;

    /// Compatibility entry point for a BSP-only runtime. The synthesized
    /// profile contains exactly bspProcessor and therefore preserves the SMP1
    /// mask contract. Multi-processor boot paths pass the explicit mask and
    /// fixed-capacity capability array to the primary overload.
    [[nodiscard]] Foundation::Result<Ia64BspRuntimeMemorySummary, Ia64BspRuntimeMemoryError>
    PrepareAndAdmit(Kernel::Scheduler::CpuId bspProcessor, const PalVirtualMemoryCapabilities& bspCapabilities,
                    const InitialAddressSpacePreparation& preparation, Kernel::Object::ResourceDomainId chargeOwner,
                    Kernel::Object::ObjectRegistry& registry, Kernel::Object::RootCapabilityIssuer& issuer) noexcept;

    /// Admits exactly the next profiled non-BSP processor to the already- claimed root while that
    /// AP still executes in physical Region 0. The transaction strictly preflights and installs
    /// that AP's runtime IVT, performs one physical-bootstrap RR activation, and then initializes
    /// the AP's dedicated ordinary controller on the same AddressSpace. The latter consumes the
    /// backend's pending acknowledgement without a second native RR activation. Exact root
    /// residency, ledger binding, and controller Ready state are validated before success is
    /// published. Preconditions: - The retained preparation and this complete owner still satisfy
    /// the PrepareAndAdmit boot-lifetime and locked Region-0 contract. - The selected AP has local
    /// interrupts, translation, preemption, migration, and same-CPU re-entry disabled for the
    /// complete call. - The complete direct call chain, GP data, physical stack, and physical RSE
    /// backing store are resident in numerical Region 0. - The retained registry and its backing
    /// lock satisfy the class-level Region-0 residency and no-lock-held/no-wait-cycle contract. -
    /// Destruction or quiescence of the retained AddressSpace generation is excluded for the
    /// complete transaction and subsequent boot lifetime. - Required mappings and page-table
    /// updates are complete and published before admission begins. - The caller excludes external
    /// concurrent root, page-table, active- binding, IVT, controller, mapping, and shootdown
    /// mutation from the pre-admission snapshot through the postcondition snapshot. - Calls are
    /// externally serialized and use increasing enabled CpuId. A failed side-effect-free preflight
    /// remains retryable. Once native IVT installation begins, a failure is retained for that AP
    /// and repeated or later out-of-order calls perform no native work. State committed before a
    /// failure is retained conservatively without rollback or reclamation.
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Foundation::Result<Ia64BspRuntimeApplicationProcessorAdmissionSummary,
                                                            Ia64BspRuntimeApplicationProcessorAdmissionError>
    AdmitApplicationProcessorPhysicalRoot(Kernel::Scheduler::CpuId processor) noexcept;

    /// Returns the retained state for one valid non-BSP processor without
    /// performing native work. The returned controller pointers borrow storage
    /// owned by this boot-lifetime object.
    [[nodiscard]] Foundation::Result<Ia64BspRuntimeApplicationProcessorAdmissionSnapshot,
                                     Ia64BspRuntimeApplicationProcessorAdmissionError>
    GetApplicationProcessorAdmissionSnapshot(Kernel::Scheduler::CpuId processor) const noexcept;

    /// Returns mutable, non-owning access to the exact shared root and the
    /// selected admitted AP's dedicated processor-local controller. Success is
    /// available only after that AP slot has committed Ready and the shared
    /// residency, registry, ledger, root, and selected-controller invariants
    /// still match the requested CpuId. A later AP's side-effect-free failed
    /// admission does not invalidate an earlier Ready AP.
    /// Preconditions:
    /// - The call executes on processor with preemption, migration, and
    /// processor-local controller re-entry excluded.
    /// - The caller excludes concurrent owner, root, controller, mapping,
    /// active-binding, IVT, and admission mutation.
    /// - The caller excludes destruction or quiescence of the retained root
    /// generation for the complete borrowed-view lifetime.
    /// - Later use obeys the returned controller's owning-CPU contract.
    /// Blocking:
    /// - May spin while observing the embedded AddressSpace, page-table ledger,
    /// active bindings, or supplied registry. The caller satisfies the
    /// class-level no-lock-held contract.
    /// Allocation:
    /// - Does not allocate, register, or transfer ownership.
    /// Returns:
    /// - InvalidProcessor, BspProcessor, or ProcessorNotProfiled for a request
    /// outside one exact profiled AP slot.
    /// - NotReady when the requested AP is Unadmitted or the complete owner is
    /// unavailable without a failure attributed to that AP.
    /// - AdmissionInProgress or PreviousAdmissionFailed only when the requested
    /// AP slot retains that exact state.
    /// - RootInvariantMismatch when retained shared or selected Ready state is
    /// contradictory.
    [[nodiscard]] Foundation::Result<Ia64BspRuntimeRootView, Ia64BspRuntimeApplicationProcessorAdmissionError>
    BorrowReadyApplicationProcessorRoot(Kernel::Scheduler::CpuId processor) noexcept;

    /// Returns mutable, non-owning access to the exact admitted BSP root and
    /// its processor-local controller only while the initial Ready invariant
    /// still holds. The owner and its embedded objects outlive the view.
    /// Preconditions:
    /// - The caller excludes concurrent owner, root, and controller mutation.
    /// - The caller excludes destruction or quiescence of the retained root
    /// generation for the complete borrowed-view lifetime.
    /// - Later use obeys the returned controller's owning-CPU contract.
    /// Blocking:
    /// - May spin while observing the embedded AddressSpace or supplied
    /// registry. The caller satisfies the class-level no-lock-held contract.
    /// Allocation:
    /// - Does not allocate, register, pin, or transfer ownership.
    /// Returns:
    /// - NotReady before the Ready commit or after terminal preparation failure.
    /// - InvariantMismatch when Ready no longer proves the exact initial root.
    [[nodiscard]] Foundation::Result<Ia64BspRuntimeRootView, Ia64BspRuntimeRootBorrowError> BorrowReadyRoot() noexcept;

    /// Returns the one fixed private initial-component AddressSpace together
    /// with the BSP controller that owns all switches between it and the
    /// common idle root. The private root may be either nonresident while the
    /// common root is active or BSP-resident while it is selected.
    [[nodiscard]] Foundation::Result<Ia64BspRuntimeRootView, Ia64BspRuntimeRootBorrowError>
    BorrowReadyInitialComponentRoot() noexcept;

    [[nodiscard]] Ia64RuntimeAddressSpaceOwnerSnapshot GetInitialComponentAddressSpaceSnapshot() const noexcept;

    /// Returns one user domain's private root with the BSP controller, like
    /// BorrowReadyInitialComponentRoot. The root may be nonresident while the
    /// common root or another user root is active, or BSP-resident while it
    /// is selected.
    [[nodiscard]] Foundation::Result<Ia64BspRuntimeRootView, Ia64BspRuntimeRootBorrowError>
    BorrowReadyUserRoot(Ia64BspRuntimeUserDomain domain) noexcept;

    [[nodiscard]] Ia64RuntimeAddressSpaceOwnerSnapshot
    GetUserAddressSpaceSnapshot(Ia64BspRuntimeUserDomain domain) const noexcept;

    /// One entry slot's storage, for the kernel context its thread runs on.
    /// Ready only.
    [[nodiscard]] Foundation::Result<Ia64BspRuntimeUserEntryStorageView, Ia64BspRuntimeRootBorrowError>
    GetEntryStorage(Ia64BspRuntimeEntrySlot slot) noexcept;

    /// A boot domain's thread's entry storage.
    [[nodiscard]] Foundation::Result<Ia64BspRuntimeUserEntryStorageView, Ia64BspRuntimeRootBorrowError>
    GetUserEntryStorage(const Ia64BspRuntimeUserDomain domain) noexcept
    {
        return GetEntryStorage(Ia64BspRuntimeEntrySlotOf(domain));
    }

    /// Validates the system-call entry context of slot (the pager's or a
    /// created thread's), which names its bridge and entry storage, and
    /// admits it for selection on the BSP. The initial component's context is
    /// the published one.
    [[nodiscard]] Ia64BspRuntimeSystemCallPublicationError
    AdmitSystemCallEntryContext(Kernel::Scheduler::CpuId processor, Ia64BspRuntimeEntrySlot slot,
                                Foundation::UInt64 bridgeContextAddress) noexcept;

    [[nodiscard]] Ia64BspRuntimeSystemCallPublicationError
    AdmitPagerSystemCallEntryContext(const Kernel::Scheduler::CpuId processor,
                                     const Foundation::UInt64 bridgeContextAddress) noexcept
    {
        return AdmitSystemCallEntryContext(processor, Ia64BspRuntimeEntrySlot::Pager, bridgeContextAddress);
    }

    /// Validates one slot's user-fault entry context (its fault frame, its
    /// entry storage and handlerAddress, an IIa64UserFaultHandler) and admits
    /// it for selection on the BSP.
    [[nodiscard]] Ia64RuntimeInterruptVectorTablePublicationError
    AdmitUserFaultEntryContext(Kernel::Scheduler::CpuId processor, Ia64BspRuntimeEntrySlot slot,
                               Foundation::UInt64 handlerAddress) noexcept;

    [[nodiscard]] Ia64RuntimeInterruptVectorTablePublicationError
    AdmitUserFaultEntryContext(const Kernel::Scheduler::CpuId processor, const Ia64BspRuntimeUserDomain domain,
                               const Foundation::UInt64 handlerAddress) noexcept
    {
        return AdmitUserFaultEntryContext(processor, Ia64BspRuntimeEntrySlotOf(domain), handlerAddress);
    }

    /// Selects the slot's admitted system-call context, and its user-fault
    /// context once admitted, on the BSP: the owning processor calls this
    /// with PSR.i clear before that slot's thread enters PL3.
    [[nodiscard]] bool SelectEntryContexts(Kernel::Scheduler::CpuId processor, Ia64BspRuntimeEntrySlot slot) noexcept;

    [[nodiscard]] bool SelectUserEntryContexts(const Kernel::Scheduler::CpuId processor,
                                               const Ia64BspRuntimeUserDomain domain) noexcept
    {
        return SelectEntryContexts(processor, Ia64BspRuntimeEntrySlotOf(domain));
    }

    /// Returns a created thread's slot frames to Inactive once its thread
    /// exited and was unbound, so a later thread's entries can claim them: a
    /// thread that exits in a system call leaves its system-call frame
    /// Active, and one terminated in a fault leaves its fault frame Retained.
    [[nodiscard]] bool ResetEntryFrames(Ia64BspRuntimeEntrySlot slot) noexcept;

    /// Initializes and release-publishes the dedicated BSP break-entry
    /// transport after the root is Ready. The processor must be the admitted
    /// and resident BSP; other profiled processors may already be resident.
    /// bridgeContextAddress must name the complete aligned Region-0 processor
    /// bridge context. The caller proves that separately owned context and
    /// collaborators are retained before this call. Entry frame, stack, and
    /// RSE storage remain inside this owner's exact RuntimeObjects corridor.
    /// Rejection changes diagnostics only.
    [[nodiscard]] Ia64BspRuntimeSystemCallPublicationError
    PublishSystemCallEntryContext(Kernel::Scheduler::CpuId processor, Foundation::UInt64 bridgeContextAddress) noexcept;

    [[nodiscard]] Ia64BspRuntimeSystemCallPublicationSnapshot GetSystemCallPublicationSnapshot() const noexcept;

    /// Initializes and release-publishes one processor-local external-interrupt
    /// context and its initially zero evidence storage. The processor must be
    /// valid, profiled, resident, and have an installed IVT context. The caller
    /// keeps external interrupt delivery disabled on that processor until this
    /// returns None. Calls for all processor slots and IVT publications are
    /// externally serialized. A valid rejection changes only that processor's
    /// diagnostics; an invalid CpuId changes no slot. Successful publication is
    /// one-shot and both objects remain in the locked corridor for boot lifetime.
    [[nodiscard]] Ia64BspRuntimeExternalInterruptPublicationError
    PublishExternalInterruptEntryContext(Kernel::Scheduler::CpuId processor) noexcept;

    /// Borrows the one reusable BSP coordinator stack/RBS and its separate
    /// control word for the boot lifetime; no per-context coordinator storage
    /// is created.
    [[nodiscard]] Foundation::Result<Ia64BspRuntimeExternalPreemptionStorageView, Ia64BspRuntimeExternalPreemptionError>
    BorrowExternalPreemptionStorage(Kernel::Scheduler::CpuId processor) noexcept;

    /// Release-publishes this owner's exact configured control through the
    /// already-published external-entry context. The caller has proved locked
    /// DTR/ITR coverage for every configured range and keeps PSR.i clear.
    [[nodiscard]] Ia64BspRuntimeExternalPreemptionError
    PublishExternalPreemptionControl(Kernel::Scheduler::CpuId processor,
                                     Interrupt::Ia64ExternalPreemptionControl& control) noexcept;

    /// Borrows exactly one BSP-owned page-fault context, control, architecture
    /// frame, memory stack, and transition-capable RSE range. Success is
    /// one-shot. Every returned byte is retained in this owner's exact locked
    /// RuntimeObjects corridor. A non-pristine frame rejects without changing
    /// the Fresh storage state and may be retried after restoring every byte
    /// to the public pristine contract. No pager transaction identity is
    /// created.
    [[nodiscard]] Foundation::Result<Ia64BspRuntimePageFaultEntryStorageView, Ia64BspRuntimePageFaultEntryError>
    BorrowPageFaultEntryStorage(Kernel::Scheduler::CpuId processor) noexcept;

    /// Release-publishes the exact three objects returned by the successful
    /// borrow after the caller configures control against that exact frame and
    /// coordinator geometry. The owner supplies the immutable vector context;
    /// alternate caller-controlled storage is rejected. Rejections while the
    /// control remains Fresh, and non-pristine-frame rejections, retain the
    /// Borrowed state for repair and retry. Once a valid configured control's
    /// immutable geometry contradicts this owner, the storage becomes Faulted
    /// and cannot be reset or republished. Observing any later control state
    /// before publication is likewise terminal.
    [[nodiscard]] Ia64BspRuntimePageFaultEntryError
    PublishPageFaultEntryContext(Kernel::Scheduler::CpuId processor, Interrupt::Ia64PageFaultEntryControl& control,
                                 Interrupt::Ia64PageFaultEntryContext& context,
                                 Interrupt::Ia64PageFaultFrame& frame) noexcept;

    [[nodiscard]] Ia64BspRuntimePageFaultEntrySnapshot GetPageFaultEntrySnapshot() const noexcept;

    /// Exact query against the immutable translation-resource plan installed
    /// for this owner. These calls have no side effect and reject wrap/Region-0
    /// violations. Native entry activation must cover every IC-off access.
    [[nodiscard]] bool HasLockedDataTranslationCoverage(Foundation::UInt64 rangeStart,
                                                        Foundation::UInt64 sizeBytes) const noexcept;

    [[nodiscard]] bool HasLockedInstructionTranslationCoverage(Foundation::UInt64 rangeStart,
                                                               Foundation::UInt64 sizeBytes) const noexcept;

    /// Acquire-observes the stored BSP slot's evidence sequence. Non-complete
    /// sequence values return zero non-sequence fields and therefore never
    /// expose partial ISR writes. Context and address diagnostics are retained
    /// after publication.
    [[nodiscard]] Ia64BspRuntimeExternalInterruptPublicationSnapshot
    GetExternalInterruptPublicationSnapshot() const noexcept;

    /// Returns one acquired processor-local publication snapshot. Calls are
    /// side-effect free. The caller excludes concurrent normal-context
    /// publication or acknowledgement for this slot; ISR evidence writes are
    /// safely observed through the mailbox sequence protocol.
    [[nodiscard]] Foundation::Result<Ia64BspRuntimeExternalInterruptPublicationSnapshot,
                                     Ia64BspRuntimeExternalInterruptAccessError>
    GetExternalInterruptPublicationSnapshot(Kernel::Scheduler::CpuId processor) const noexcept;

    /// Returns a read-only boot-lifetime view of one successfully published
    /// processor-local evidence mailbox. The caller uses the evidence acquire
    /// helpers and obeys the same interrupt-delivery/consumer serialization as
    /// acknowledgement. This owner never grants a mutable borrow.
    [[nodiscard]] Foundation::Result<const Interrupt::Ia64LocalTimerInterruptEvidence*,
                                     Ia64BspRuntimeExternalInterruptAccessError>
    GetPublishedExternalInterruptEvidence(Kernel::Scheduler::CpuId processor) const noexcept;

    /// Release-acknowledges one exact acquired generation in the stored BSP
    /// slot. The local timer remains masked and the caller excludes ISR delivery
    /// and concurrent consumers for the complete operation.
    [[nodiscard]] Ia64BspRuntimeExternalInterruptAcknowledgementError
    AcknowledgeExternalInterruptEvidence(Foundation::UInt64 sequence) noexcept;

    /// Release-acknowledges one exact acquired processor-local mailbox
    /// generation. The selected processor's local timer remains masked and the
    /// caller excludes ISR delivery and concurrent consumers for that mailbox.
    /// Calls across slots are externally serialized with publication. Invalid
    /// CpuId changes no slot; every valid failure changes only the selected
    /// slot's acknowledgement diagnostic.
    [[nodiscard]] Ia64BspRuntimeExternalInterruptAcknowledgementError
    AcknowledgeExternalInterruptEvidence(Kernel::Scheduler::CpuId processor, Foundation::UInt64 sequence) noexcept;

    [[nodiscard]] Ia64BspRuntimeMemorySnapshot GetSnapshot() const noexcept;

    /// Retains the BSP's validated PAL_PTCE_INFO loop and names it as the
    /// translation-cache purge of every runtime root this object owns
    /// (Ia64RuntimeAddressSpacePageTable: a root that unmapped while it was
    /// not current purges when it next activates). Once, after
    /// PrepareAndAdmit returned Ready and before any unmap, on the BSP.
    [[nodiscard]] bool ConfigureTranslationCachePurge(Kernel::Scheduler::CpuId bspProcessor,
                                                      const PalTranslationCachePurgePlan& plan) noexcept;

    [[nodiscard]] Ia64LocalTranslationCachePurgeSnapshot GetTranslationCachePurgeSnapshot() const noexcept;

    /// The run-time AddressSpaces of ObjectCreate type 2: slots embedded here, so their backends
    /// lie in the runtime-object corridor, built from the boot's seed, profile, RID allocator,
    /// bindings and purge. Configured once, after ConfigureTranslationCachePurge; null otherwise.
    [[nodiscard]] Kernel::Memory::IAddressSpaceProvider* ConfigureRuntimeAddressSpaces() noexcept;

    [[nodiscard]] Ia64RuntimeAddressSpaceSlotsSnapshot GetRuntimeAddressSpaceSnapshot() const noexcept;

    /// The allocator's AddressSpace count: the boot's three roots and every
    /// run-time incarnation.
    [[nodiscard]] Foundation::UInt32 GetAllocatedAddressSpaceCount() const noexcept;

private:
    [[nodiscard]] Ia64BspRuntimeMemoryError InitializeRuntimeBackend(
        Kernel::Scheduler::CpuId bspProcessor, Foundation::UInt8 profiledProcessorMask,
        const PalVirtualMemoryCapabilities (&processorCapabilities)[Kernel::Scheduler::MaximumProcessorCount],
        const InitialAddressSpacePreparation& preparation) noexcept;

    [[nodiscard]] Ia64BspRuntimeMemoryError
    VerifyPostconditions(Kernel::Scheduler::CpuId bspProcessor, const Kernel::Memory::InitialMappingPlan& mappingPlan,
                         Kernel::Memory::AddressSpaceId identity,
                         const Kernel::Memory::ProcessorAddressSpaceControllerReceipt& controllerReceipt) noexcept;

    [[nodiscard]] Ia64BspRuntimeMemoryError
    PreparePagerAddressSpace(const InitialAddressSpacePreparation& preparation,
                             Kernel::Object::ResourceDomainId chargeOwner, Kernel::Object::ObjectRegistry& registry,
                             Kernel::Object::RootCapabilityIssuer& issuer) noexcept;

    [[nodiscard, gnu::noinline]] bool
    OtherUserRootIsActive(Ia64BspRuntimeUserDomain domain,
                          const Kernel::Memory::ProcessorAddressSpaceControllerSnapshot& controller) const noexcept;
    [[nodiscard]] Ia64RuntimeAddressSpaceOwner& UserRootOwner(Ia64BspRuntimeUserDomain domain) noexcept;

    [[nodiscard]] const Ia64RuntimeAddressSpaceOwner& UserRootOwner(Ia64BspRuntimeUserDomain domain) const noexcept;

    [[nodiscard]] Ia64BspRuntimeMemoryError PrepareInitialComponentAddressSpace(
        const InitialAddressSpacePreparation& preparation, Kernel::Object::ResourceDomainId chargeOwner,
        Kernel::Object::ObjectRegistry& registry, Kernel::Object::RootCapabilityIssuer& issuer) noexcept;

    [[nodiscard]] bool HasLiveAddressSpaceRegistryBinding() const noexcept;

    [[nodiscard]] bool PageFaultEntryStorageInvariantHolds() const noexcept;

    [[nodiscard]] Ia64BspRuntimeApplicationProcessorAdmissionError
    ValidateApplicationProcessorAdmission(Kernel::Scheduler::CpuId processor,
                                          Foundation::Size controllerIndex) const noexcept;

    [[nodiscard]] Ia64BspRuntimeApplicationProcessorAdmissionError VerifyApplicationProcessorPostconditions(
        Kernel::Scheduler::CpuId processor, Foundation::Size controllerIndex,
        Foundation::ProcessorMask expectedResidentProcessorMask, Foundation::Size preAdmissionRootMappingCount,
        const Ia64RuntimeInterruptVectorTableInstallSummary& interruptVectorTable,
        const Kernel::Memory::ProcessorAddressSpaceControllerReceipt& controllerReceipt) noexcept;

    [[nodiscard]] Ia64BspRuntimeApplicationProcessorAdmissionError FailApplicationProcessorAdmission(
        Foundation::Size controllerIndex, Ia64BspRuntimeApplicationProcessorAdmissionError error,
        Ia64RuntimeInterruptVectorTableInstallError interruptVectorTableError,
        Hal::Memory::AddressSpacePageTableActivationError physicalBootstrapActivationError,
        const Kernel::Memory::ProcessorAddressSpaceControllerFailure& controllerFailure,
        Ia64BspRuntimeApplicationProcessorPostconditionError postconditionError, bool interruptVectorTableCommitted,
        bool physicalRootMayBeActive) noexcept;

    Diagnostics::Ia64StopStorage m_stopStorage{};
    Ia64RuntimeProcessorCapabilities m_processorCapabilities;
    Ia64RegionIdentifierAllocator m_regionIdentifierAllocator;
    Ia64RegionIdentifierAllocationReceipt m_regionIdentifierReceipt;
    Ia64ActiveRuntimeTranslationBindings m_activeBindings;
    Ia64RuntimeAddressSpacePageTable m_addressSpacePageTable;
    Kernel::Memory::AddressSpace m_addressSpace;
    Kernel::Object::ObjectPin m_addressSpacePin;
    Ia64RuntimeAddressSpaceOwner m_initialComponentAddressSpaceOwner;
    Ia64RuntimeAddressSpaceOwner m_pagerAddressSpaceOwner;
    Ia64LocalTranslationCachePurge m_translationCachePurge;
    Ia64RuntimeAddressSpaceSlots m_runtimeAddressSpaces;
    bool m_runtimeAddressSpacesConfigured{false};
    Kernel::Object::ObjectRegistry* m_registry{nullptr};
    Kernel::Memory::ProcessorAddressSpaceController m_addressSpaceController;
    Kernel::Memory::ProcessorAddressSpaceController
        m_applicationProcessorControllers[Ia64BspRuntimeMaximumApplicationProcessorCount];
    Ia64RuntimeInterruptVectorTable m_interruptVectorTable;
    System::Ia64SystemCallEntryContext m_systemCallEntryContext{};
    System::Ia64SystemCallEntryFrame m_systemCallEntryFrame{};
    alignas(Kernel::Memory::MappingPageSizeBytes)
        Foundation::UInt8 m_systemCallMemoryStack[System::Ia64SystemCallMemoryStackSizeBytes]{};
    alignas(Kernel::Memory::MappingPageSizeBytes)
        Foundation::UInt8 m_systemCallRseBackingStore[System::Ia64SystemCallRseBackingStoreSizeBytes]{};
    // The admitted slots' entry storage (the pager's and the created
    // threads'; the initial component's is the system-call storage above)
    // and every slot's user-fault context and frame.
    static constexpr Foundation::Size AdmittedEntrySlots{Ia64BspRuntimeEntrySlotCount - 1U};
    System::Ia64SystemCallEntryContext m_slotSystemCallEntryContexts[AdmittedEntrySlots]{};
    System::Ia64SystemCallEntryFrame m_slotSystemCallEntryFrames[AdmittedEntrySlots]{};
    alignas(Kernel::Memory::MappingPageSizeBytes) Foundation::UInt8
        m_slotSystemCallMemoryStacks[AdmittedEntrySlots][System::Ia64SystemCallMemoryStackSizeBytes]{};
    alignas(Kernel::Memory::MappingPageSizeBytes) Foundation::UInt8
        m_slotSystemCallRseBackingStores[AdmittedEntrySlots][System::Ia64SystemCallRseBackingStoreSizeBytes]{};
    Interrupt::Ia64UserFaultEntryContext m_userFaultEntryContexts[Ia64BspRuntimeEntrySlotCount]{};
    alignas(Kernel::Memory::MappingPageSizeBytes) Interrupt::Ia64PageFaultFrame
        m_userFaultFrames[Ia64BspRuntimeEntrySlotCount]{};
    bool m_slotSystemCallEntryAdmitted[AdmittedEntrySlots]{};
    bool m_userFaultEntryAdmitted[Ia64BspRuntimeEntrySlotCount]{};
    Ia64BspRuntimeMemorySummary m_summary{};
    Ia64BspRuntimeMemoryFailure m_failure{};
    Ia64BspRuntimeApplicationProcessorAdmissionSummary
        m_applicationProcessorAdmissionSummaries[Ia64BspRuntimeMaximumApplicationProcessorCount]{};
    Ia64BspRuntimeApplicationProcessorAdmissionFailure
        m_applicationProcessorAdmissionFailures[Ia64BspRuntimeMaximumApplicationProcessorCount]{};
    Ia64BspRuntimeApplicationProcessorAdmissionState
        m_applicationProcessorAdmissionStates[Ia64BspRuntimeMaximumApplicationProcessorCount]{};
    const InitialAddressSpacePreparation* m_preparation{nullptr};
    Ia64BspRuntimeSystemCallPublicationError m_systemCallPublicationError{
        Ia64BspRuntimeSystemCallPublicationError::None};
    System::Ia64SystemCallContextValidationError m_systemCallContextValidationError{
        System::Ia64SystemCallContextValidationError::None};
    Ia64RuntimeInterruptVectorTablePublicationError m_systemCallInterruptVectorTableError{
        Ia64RuntimeInterruptVectorTablePublicationError::None};
    bool m_systemCallEntryPublished{false};
    Ia64BspRuntimeMemoryState m_state{Ia64BspRuntimeMemoryState::Fresh};
    Ia64BspRuntimeExternalInterruptTransportSlot
        m_externalInterruptTransportSlots[Kernel::Scheduler::MaximumProcessorCount]{};
    alignas(Interrupt::Ia64ExternalPreemptionControl) Interrupt::Ia64ExternalPreemptionControl
        m_externalPreemptionControl{};
    bool m_externalPreemptionStorageBorrowed{false};
    alignas(Kernel::Memory::MappingPageSizeBytes) Foundation::UInt8
        m_externalPreemptionCoordinatorMemoryStack[Interrupt::Ia64ExternalPreemptionCoordinatorStackSizeBytes]{};
    alignas(Kernel::Memory::MappingPageSizeBytes) Foundation::UInt8 m_externalPreemptionCoordinatorRseBackingStore
        [Interrupt::Ia64ExternalPreemptionCoordinatorRseBackingStoreSizeBytes]{};
    alignas(Interrupt::Ia64PageFaultEntryContext) Interrupt::Ia64PageFaultEntryContext m_pageFaultEntryContext{};
    alignas(Interrupt::Ia64PageFaultEntryControl) Interrupt::Ia64PageFaultEntryControl m_pageFaultEntryControl{};
    alignas(Kernel::Memory::MappingPageSizeBytes) Interrupt::Ia64PageFaultFrame m_pageFaultFrame{};
    alignas(Kernel::Memory::MappingPageSizeBytes)
        Foundation::UInt8 m_pageFaultCoordinatorMemoryStack[Interrupt::Ia64PageFaultCoordinatorStackSizeBytes]{};
    alignas(Kernel::Memory::MappingPageSizeBytes) Foundation::UInt8
        m_pageFaultCoordinatorRseBackingStore[Interrupt::Ia64PageFaultCoordinatorRseBackingStoreSizeBytes]{};
    Ia64BspRuntimePageFaultEntryError m_pageFaultEntryError{Ia64BspRuntimePageFaultEntryError::None};
    Ia64RuntimePageFaultPublicationError m_pageFaultInterruptVectorTableError{
        Ia64RuntimePageFaultPublicationError::None};
    Ia64BspRuntimePageFaultEntryState m_pageFaultEntryState{Ia64BspRuntimePageFaultEntryState::Fresh};
};

static_assert(Kernel::Memory::MappingPageSizeBytes == 4096U);
static_assert(Kernel::Memory::MappingPageSizeBytes == System::Ia64SystemCallStoragePageSizeBytes);
static_assert(Kernel::Scheduler::MaximumProcessorCount == System::MaximumIa64SystemCallProcessorCount);
static_assert(System::Ia64SystemCallMemoryStackSizeBytes == 16U * 1024U);
static_assert(System::Ia64SystemCallRseTransitionSizeBytes == 16U * 1024U);
static_assert(System::Ia64SystemCallRseBackingStoreSizeBytes == 32U * 1024U);
static_assert(Kernel::Memory::MappingPageSizeBytes == Interrupt::Ia64PageFaultStoragePageSizeBytes);
static_assert(Kernel::Scheduler::MaximumProcessorCount == Interrupt::Ia64PageFaultMaximumProcessorCount);
static_assert(Interrupt::Ia64PageFaultCoordinatorStackSizeBytes == 16U * 1024U);
static_assert(Interrupt::Ia64PageFaultCoordinatorRseTransitionSizeBytes == 16U * 1024U);
static_assert(Interrupt::Ia64PageFaultCoordinatorRseBackingStoreSizeBytes == 32U * 1024U);
static_assert(sizeof(Ia64BspRuntimeMemoryState) == 1U);
static_assert(sizeof(Ia64BspRuntimeMemoryError) == 1U);
static_assert(sizeof(Ia64BspRuntimeBindingState) == 1U);
static_assert(sizeof(Ia64BspRuntimeMemoryPostconditionError) == 1U);
static_assert(sizeof(Ia64BspRuntimeRootBorrowError) == 1U);
static_assert(Ia64BspRuntimeMaximumApplicationProcessorCount == 3U);
static_assert(sizeof(Ia64BspRuntimeApplicationProcessorAdmissionState) == 1U);
static_assert(sizeof(Ia64BspRuntimeApplicationProcessorAdmissionError) == 1U);
static_assert(sizeof(Ia64BspRuntimeApplicationProcessorPostconditionError) == 1U);
static_assert(sizeof(Ia64BspRuntimeSystemCallPublicationError) == 1U);
static_assert(sizeof(Ia64BspRuntimeExternalInterruptPublicationError) == 1U);
static_assert(sizeof(Ia64BspRuntimeExternalInterruptAcknowledgementError) == 1U);
static_assert(sizeof(Ia64BspRuntimeExternalInterruptAccessError) == 1U);
static_assert(sizeof(Ia64BspRuntimeExternalPreemptionError) == 1U);
static_assert(sizeof(Ia64BspRuntimePageFaultEntryState) == 1U);
static_assert(sizeof(Ia64BspRuntimePageFaultEntryError) == 1U);
static_assert(sizeof(Ia64BspRuntimeExternalPreemptionStorageView) == 56U);
static_assert(alignof(Ia64BspRuntimeExternalPreemptionStorageView) == 8U);
static_assert(__is_aggregate(Ia64BspRuntimeExternalPreemptionStorageView));
static_assert(__is_standard_layout(Ia64BspRuntimeExternalPreemptionStorageView));
static_assert(__is_trivially_copyable(Ia64BspRuntimeExternalPreemptionStorageView));
static_assert(sizeof(Ia64BspRuntimePageFaultEntryStorageView) == 104U);
static_assert(alignof(Ia64BspRuntimePageFaultEntryStorageView) == 8U);
static_assert(__is_aggregate(Ia64BspRuntimePageFaultEntryStorageView));
static_assert(__is_standard_layout(Ia64BspRuntimePageFaultEntryStorageView));
static_assert(__is_trivially_copyable(Ia64BspRuntimePageFaultEntryStorageView));
static_assert(__builtin_offsetof(Ia64BspRuntimePageFaultEntryStorageView, Control) == 0U);
static_assert(__builtin_offsetof(Ia64BspRuntimePageFaultEntryStorageView, Context) == 8U);
static_assert(__builtin_offsetof(Ia64BspRuntimePageFaultEntryStorageView, Frame) == 16U);
static_assert(__builtin_offsetof(Ia64BspRuntimePageFaultEntryStorageView, CoordinatorMemoryStack) == 24U);
static_assert(__builtin_offsetof(Ia64BspRuntimePageFaultEntryStorageView, CoordinatorRseBackingStore) == 32U);
static_assert(__builtin_offsetof(Ia64BspRuntimePageFaultEntryStorageView, ControlAddress) == 40U);
static_assert(__builtin_offsetof(Ia64BspRuntimePageFaultEntryStorageView, ContextAddress) == 48U);
static_assert(__builtin_offsetof(Ia64BspRuntimePageFaultEntryStorageView, FrameAddress) == 56U);
static_assert(__builtin_offsetof(Ia64BspRuntimePageFaultEntryStorageView, CoordinatorMemoryStackAddress) == 64U);
static_assert(__builtin_offsetof(Ia64BspRuntimePageFaultEntryStorageView, CoordinatorMemoryStackSizeBytes) == 72U);
static_assert(__builtin_offsetof(Ia64BspRuntimePageFaultEntryStorageView, CoordinatorRseBackingStoreAddress) == 80U);
static_assert(__builtin_offsetof(Ia64BspRuntimePageFaultEntryStorageView, CoordinatorRseTransitionSizeBytes) == 88U);
static_assert(__builtin_offsetof(Ia64BspRuntimePageFaultEntryStorageView, CoordinatorRseBackingStoreSizeBytes) == 96U);
static_assert(sizeof(Ia64BspRuntimeRootView) == 40U);
static_assert(alignof(Ia64BspRuntimeRootView) == 8U);
static_assert(__is_aggregate(Ia64BspRuntimeRootView));
static_assert(__is_standard_layout(Ia64BspRuntimeRootView));
static_assert(__is_trivially_copyable(Ia64BspRuntimeRootView));
static_assert(__builtin_offsetof(Ia64BspRuntimeRootView, AddressSpaceStorage) == 0U);
static_assert(__builtin_offsetof(Ia64BspRuntimeRootView, AddressSpaceIdentity) == 8U);
static_assert(__builtin_offsetof(Ia64BspRuntimeRootView, ControllerStorage) == 24U);
static_assert(__builtin_offsetof(Ia64BspRuntimeRootView, Processor) == 32U);
static_assert(sizeof(Ia64BspRuntimeApplicationProcessorAdmissionSummary) == 104U);
static_assert(alignof(Ia64BspRuntimeApplicationProcessorAdmissionSummary) == 8U);
static_assert(__is_aggregate(Ia64BspRuntimeApplicationProcessorAdmissionSummary));
static_assert(__is_standard_layout(Ia64BspRuntimeApplicationProcessorAdmissionSummary));
static_assert(__is_trivially_copyable(Ia64BspRuntimeApplicationProcessorAdmissionSummary));
static_assert(__builtin_offsetof(Ia64BspRuntimeApplicationProcessorAdmissionSummary, InterruptVectorTable) == 0U);
static_assert(__builtin_offsetof(Ia64BspRuntimeApplicationProcessorAdmissionSummary, AddressSpaceIdentity) == 24U);
static_assert(__builtin_offsetof(Ia64BspRuntimeApplicationProcessorAdmissionSummary, ControllerReceipt) == 40U);
static_assert(__builtin_offsetof(Ia64BspRuntimeApplicationProcessorAdmissionSummary, Processor) == 80U);
static_assert(__builtin_offsetof(Ia64BspRuntimeApplicationProcessorAdmissionSummary, ResidentProcessorMask) == 88U);
static_assert(__builtin_offsetof(Ia64BspRuntimeApplicationProcessorAdmissionSummary, InstalledProcessorMask) == 96U);
static_assert(__builtin_offsetof(Ia64BspRuntimeApplicationProcessorAdmissionSummary, ControllerState) == 97U);
static_assert(__builtin_offsetof(Ia64BspRuntimeApplicationProcessorAdmissionSummary,
                                 PhysicalBootstrapActivationPerformed) == 98U);
static_assert(__builtin_offsetof(Ia64BspRuntimeApplicationProcessorAdmissionSummary,
                                 ControllerAcknowledgementPerformed) == 99U);
static_assert(__builtin_offsetof(Ia64BspRuntimeApplicationProcessorAdmissionSummary, ExactLedgerVerified) == 100U);
static_assert(__builtin_offsetof(Ia64BspRuntimeApplicationProcessorAdmissionSummary, Reserved) == 101U);
static_assert(sizeof(Ia64BspRuntimeApplicationProcessorAdmissionFailure) == 24U);
static_assert(alignof(Ia64BspRuntimeApplicationProcessorAdmissionFailure) == 1U);
static_assert(__is_aggregate(Ia64BspRuntimeApplicationProcessorAdmissionFailure));
static_assert(__is_standard_layout(Ia64BspRuntimeApplicationProcessorAdmissionFailure));
static_assert(__is_trivially_copyable(Ia64BspRuntimeApplicationProcessorAdmissionFailure));
static_assert(__builtin_offsetof(Ia64BspRuntimeApplicationProcessorAdmissionFailure, Error) == 0U);
static_assert(__builtin_offsetof(Ia64BspRuntimeApplicationProcessorAdmissionFailure, InterruptVectorTableError) == 1U);
static_assert(__builtin_offsetof(Ia64BspRuntimeApplicationProcessorAdmissionFailure,
                                 PhysicalBootstrapActivationError) == 2U);
static_assert(__builtin_offsetof(Ia64BspRuntimeApplicationProcessorAdmissionFailure, PostconditionError) == 3U);
static_assert(__builtin_offsetof(Ia64BspRuntimeApplicationProcessorAdmissionFailure, ControllerFailure) == 4U);
static_assert(__builtin_offsetof(Ia64BspRuntimeApplicationProcessorAdmissionFailure, InterruptVectorTableCommitted) ==
              20U);
static_assert(__builtin_offsetof(Ia64BspRuntimeApplicationProcessorAdmissionFailure, PhysicalRootMayBeActive) == 21U);
static_assert(__builtin_offsetof(Ia64BspRuntimeApplicationProcessorAdmissionFailure, Reserved) == 22U);
static_assert(sizeof(Ia64BspRuntimeApplicationProcessorAdmissionSnapshot) == 248U);
static_assert(alignof(Ia64BspRuntimeApplicationProcessorAdmissionSnapshot) == 8U);
static_assert(__is_aggregate(Ia64BspRuntimeApplicationProcessorAdmissionSnapshot));
static_assert(__is_standard_layout(Ia64BspRuntimeApplicationProcessorAdmissionSnapshot));
static_assert(__is_trivially_copyable(Ia64BspRuntimeApplicationProcessorAdmissionSnapshot));
static_assert(__builtin_offsetof(Ia64BspRuntimeApplicationProcessorAdmissionSnapshot, Summary) == 0U);
static_assert(__builtin_offsetof(Ia64BspRuntimeApplicationProcessorAdmissionSnapshot, Failure) == 104U);
static_assert(__builtin_offsetof(Ia64BspRuntimeApplicationProcessorAdmissionSnapshot, Controller) == 128U);
static_assert(__builtin_offsetof(Ia64BspRuntimeApplicationProcessorAdmissionSnapshot, State) == 240U);
static_assert(__builtin_offsetof(Ia64BspRuntimeApplicationProcessorAdmissionSnapshot, Reserved) == 241U);
static_assert(sizeof(Ia64BspRuntimeMemorySummary) == 160U);
static_assert(alignof(Ia64BspRuntimeMemorySummary) == 8U);
static_assert(__is_aggregate(Ia64BspRuntimeMemorySummary));
static_assert(__is_standard_layout(Ia64BspRuntimeMemorySummary));
static_assert(__is_trivially_copyable(Ia64BspRuntimeMemorySummary));
static_assert(__builtin_offsetof(Ia64BspRuntimeMemorySummary, RuntimeObjects) == 0U);
static_assert(__builtin_offsetof(Ia64BspRuntimeMemorySummary, InterruptVectorTable) == 16U);
static_assert(__builtin_offsetof(Ia64BspRuntimeMemorySummary, CommonCapabilities) == 40U);
static_assert(__builtin_offsetof(Ia64BspRuntimeMemorySummary, AddressSpaceIdentity) == 64U);
static_assert(__builtin_offsetof(Ia64BspRuntimeMemorySummary, ControllerReceipt) == 80U);
static_assert(__builtin_offsetof(Ia64BspRuntimeMemorySummary, MappingCount) == 120U);
static_assert(__builtin_offsetof(Ia64BspRuntimeMemorySummary, FirstRegionIdentifier) == 128U);
static_assert(__builtin_offsetof(Ia64BspRuntimeMemorySummary, LastRegionIdentifier) == 132U);
static_assert(__builtin_offsetof(Ia64BspRuntimeMemorySummary, BspProcessorIndex) == 136U);
static_assert(__builtin_offsetof(Ia64BspRuntimeMemorySummary, ProfiledProcessorMask) == 137U);
static_assert(__builtin_offsetof(Ia64BspRuntimeMemorySummary, ResidentProcessorMask) == 144U);
static_assert(__builtin_offsetof(Ia64BspRuntimeMemorySummary, ControllerState) == 152U);
static_assert(__builtin_offsetof(Ia64BspRuntimeMemorySummary, BackendClaimed) == 153U);
static_assert(__builtin_offsetof(Ia64BspRuntimeMemorySummary, ExactLedgerVerified) == 154U);
static_assert(__builtin_offsetof(Ia64BspRuntimeMemorySummary, Reserved) == 155U);
static_assert(sizeof(Ia64BspRuntimeMemoryFailure) == 36U);
static_assert(alignof(Ia64BspRuntimeMemoryFailure) == 4U);
static_assert(__is_aggregate(Ia64BspRuntimeMemoryFailure));
static_assert(__is_standard_layout(Ia64BspRuntimeMemoryFailure));
static_assert(__is_trivially_copyable(Ia64BspRuntimeMemoryFailure));
static_assert(__builtin_offsetof(Ia64BspRuntimeMemoryFailure, Error) == 0U);
static_assert(__builtin_offsetof(Ia64BspRuntimeMemoryFailure, InterruptVectorTableError) == 1U);
static_assert(__builtin_offsetof(Ia64BspRuntimeMemoryFailure, ProcessorCapabilitiesError) == 2U);
static_assert(__builtin_offsetof(Ia64BspRuntimeMemoryFailure, RegionIdentifierAllocatorError) == 3U);
static_assert(__builtin_offsetof(Ia64BspRuntimeMemoryFailure, AddressSpaceBackendError) == 4U);
static_assert(__builtin_offsetof(Ia64BspRuntimeMemoryFailure, AddressSpaceInitializationError) == 5U);
static_assert(__builtin_offsetof(Ia64BspRuntimeMemoryFailure, ObjectRegistryError) == 6U);
static_assert(__builtin_offsetof(Ia64BspRuntimeMemoryFailure, PostconditionError) == 7U);
static_assert(__builtin_offsetof(Ia64BspRuntimeMemoryFailure, MappingPlanError) == 8U);
static_assert(__builtin_offsetof(Ia64BspRuntimeMemoryFailure, AddressSpaceIdentityBindingFailure) == 12U);
static_assert(__builtin_offsetof(Ia64BspRuntimeMemoryFailure, ControllerFailure) == 20U);
static_assert(sizeof(Ia64BspRuntimeSystemCallPublicationSnapshot) == 80U);
static_assert(alignof(Ia64BspRuntimeSystemCallPublicationSnapshot) == 16U);
static_assert(__is_aggregate(Ia64BspRuntimeSystemCallPublicationSnapshot));
static_assert(__is_standard_layout(Ia64BspRuntimeSystemCallPublicationSnapshot));
static_assert(__is_trivially_copyable(Ia64BspRuntimeSystemCallPublicationSnapshot));
static_assert(__builtin_offsetof(Ia64BspRuntimeSystemCallPublicationSnapshot, Context) == 0U);
static_assert(__builtin_offsetof(Ia64BspRuntimeSystemCallPublicationSnapshot, ContextAddress) == 64U);
static_assert(__builtin_offsetof(Ia64BspRuntimeSystemCallPublicationSnapshot, LastError) == 72U);
static_assert(__builtin_offsetof(Ia64BspRuntimeSystemCallPublicationSnapshot, LastContextValidationError) == 73U);
static_assert(__builtin_offsetof(Ia64BspRuntimeSystemCallPublicationSnapshot, LastInterruptVectorTableError) == 74U);
static_assert(__builtin_offsetof(Ia64BspRuntimeSystemCallPublicationSnapshot, Published) == 75U);
static_assert(__builtin_offsetof(Ia64BspRuntimeSystemCallPublicationSnapshot, Reserved) == 76U);
static_assert(sizeof(Ia64BspRuntimeExternalInterruptPublicationSnapshot) == 128U);
static_assert(alignof(Ia64BspRuntimeExternalInterruptPublicationSnapshot) == 16U);
static_assert(__is_aggregate(Ia64BspRuntimeExternalInterruptPublicationSnapshot));
static_assert(__is_standard_layout(Ia64BspRuntimeExternalInterruptPublicationSnapshot));
static_assert(__is_trivially_copyable(Ia64BspRuntimeExternalInterruptPublicationSnapshot));
static_assert(__builtin_offsetof(Ia64BspRuntimeExternalInterruptPublicationSnapshot, Context) == 0U);
static_assert(__builtin_offsetof(Ia64BspRuntimeExternalInterruptPublicationSnapshot, Evidence) == 32U);
static_assert(__builtin_offsetof(Ia64BspRuntimeExternalInterruptPublicationSnapshot, ContextAddress) == 96U);
static_assert(__builtin_offsetof(Ia64BspRuntimeExternalInterruptPublicationSnapshot, EvidenceAddress) == 104U);
static_assert(__builtin_offsetof(Ia64BspRuntimeExternalInterruptPublicationSnapshot, LastError) == 112U);
static_assert(__builtin_offsetof(Ia64BspRuntimeExternalInterruptPublicationSnapshot, LastInterruptVectorTableError) ==
              113U);
static_assert(__builtin_offsetof(Ia64BspRuntimeExternalInterruptPublicationSnapshot, LastAcknowledgementError) == 114U);
static_assert(__builtin_offsetof(Ia64BspRuntimeExternalInterruptPublicationSnapshot, Published) == 115U);
static_assert(__builtin_offsetof(Ia64BspRuntimeExternalInterruptPublicationSnapshot, Reserved) == 116U);
static_assert(sizeof(Ia64BspRuntimePageFaultEntrySnapshot) == 96U);
static_assert(alignof(Ia64BspRuntimePageFaultEntrySnapshot) == 16U);
static_assert(__is_aggregate(Ia64BspRuntimePageFaultEntrySnapshot));
static_assert(__is_standard_layout(Ia64BspRuntimePageFaultEntrySnapshot));
static_assert(__is_trivially_copyable(Ia64BspRuntimePageFaultEntrySnapshot));
static_assert(__builtin_offsetof(Ia64BspRuntimePageFaultEntrySnapshot, Context) == 0U);
static_assert(__builtin_offsetof(Ia64BspRuntimePageFaultEntrySnapshot, ContextAddress) == 64U);
static_assert(__builtin_offsetof(Ia64BspRuntimePageFaultEntrySnapshot, ControlAddress) == 72U);
static_assert(__builtin_offsetof(Ia64BspRuntimePageFaultEntrySnapshot, FrameAddress) == 80U);
static_assert(__builtin_offsetof(Ia64BspRuntimePageFaultEntrySnapshot, LastError) == 88U);
static_assert(__builtin_offsetof(Ia64BspRuntimePageFaultEntrySnapshot, LastInterruptVectorTableError) == 89U);
static_assert(__builtin_offsetof(Ia64BspRuntimePageFaultEntrySnapshot, State) == 90U);
static_assert(__builtin_offsetof(Ia64BspRuntimePageFaultEntrySnapshot, Reserved) == 91U);
static_assert(sizeof(Ia64BspRuntimeExternalInterruptTransportSlot) == 0x70U);
static_assert(alignof(Ia64BspRuntimeExternalInterruptTransportSlot) == 16U);
static_assert(__is_aggregate(Ia64BspRuntimeExternalInterruptTransportSlot));
static_assert(__is_standard_layout(Ia64BspRuntimeExternalInterruptTransportSlot));
static_assert(__is_trivially_copyable(Ia64BspRuntimeExternalInterruptTransportSlot));
static_assert(__builtin_offsetof(Ia64BspRuntimeExternalInterruptTransportSlot, Context) == 0x00U);
static_assert(__builtin_offsetof(Ia64BspRuntimeExternalInterruptTransportSlot, Evidence) == 0x20U);
static_assert(__builtin_offsetof(Ia64BspRuntimeExternalInterruptTransportSlot, LastError) == 0x60U);
static_assert(__builtin_offsetof(Ia64BspRuntimeExternalInterruptTransportSlot, LastInterruptVectorTableError) == 0x61U);
static_assert(__builtin_offsetof(Ia64BspRuntimeExternalInterruptTransportSlot, LastAcknowledgementError) == 0x62U);
static_assert(__builtin_offsetof(Ia64BspRuntimeExternalInterruptTransportSlot, Published) == 0x63U);
static_assert(__builtin_offsetof(Ia64BspRuntimeExternalInterruptTransportSlot, Reserved) == 0x64U);
static_assert(sizeof(Ia64BspRuntimeMemorySnapshot) == 752U);
static_assert(alignof(Ia64BspRuntimeMemorySnapshot) == 16U);
static_assert(__is_aggregate(Ia64BspRuntimeMemorySnapshot));
static_assert(__is_standard_layout(Ia64BspRuntimeMemorySnapshot));
static_assert(__is_trivially_copyable(Ia64BspRuntimeMemorySnapshot));
static_assert(__builtin_offsetof(Ia64BspRuntimeMemorySnapshot, Summary) == 0U);
static_assert(__builtin_offsetof(Ia64BspRuntimeMemorySnapshot, Failure) == 160U);
static_assert(__builtin_offsetof(Ia64BspRuntimeMemorySnapshot, SystemCallPublication) == 208U);
static_assert(__builtin_offsetof(Ia64BspRuntimeMemorySnapshot, ExternalInterruptPublication) == 288U);
static_assert(__builtin_offsetof(Ia64BspRuntimeMemorySnapshot, PageFaultEntry) == 416U);
static_assert(__builtin_offsetof(Ia64BspRuntimeMemorySnapshot, RegionIdentifierAllocator) == 512U);
static_assert(__builtin_offsetof(Ia64BspRuntimeMemorySnapshot, AddressSpaceStorage) == 528U);
static_assert(__builtin_offsetof(Ia64BspRuntimeMemorySnapshot, AddressSpace) == 536U);
static_assert(__builtin_offsetof(Ia64BspRuntimeMemorySnapshot, Controller) == 624U);
static_assert(__builtin_offsetof(Ia64BspRuntimeMemorySnapshot, Bindings) == 736U);
static_assert(__builtin_offsetof(Ia64BspRuntimeMemorySnapshot, State) == 740U);
static_assert(__builtin_offsetof(Ia64BspRuntimeMemorySnapshot, Reserved) == 741U);
static_assert(alignof(Ia64BspRuntimeMemory) == Kernel::Memory::MappingPageSizeBytes);
static_assert((sizeof(Ia64BspRuntimeMemory) % Kernel::Memory::MappingPageSizeBytes) == 0U);
} // Namespace Asteria::Platform::Ia64::Memory

#endif // ASTERIA_PLATFORM_IA64_MEMORY_IA64_BSP_RUNTIME_MEMORY_HPP_INCLUDED
