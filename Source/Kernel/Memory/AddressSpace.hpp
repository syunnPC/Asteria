// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_ADDRESS_SPACE_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_ADDRESS_SPACE_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Hal/Memory/IAddressSpacePageTable.hpp"
#include "Kernel/Memory/AddressSpaceMappingRecord.hpp"
#include "Kernel/Memory/FaultFrameMapping.hpp"
#include "Kernel/Memory/InitialMappingPlan.hpp"
#include "Kernel/Memory/PinnedFrameMapping.hpp"
#include "Kernel/Object/IKernelObject.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Kernel/Scheduler/SchedulerTypes.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Object
{
class ObjectRegistry;
}

namespace Asteria::Kernel::Memory
{
constexpr Foundation::Size MaximumAddressSpaceMappings{MaximumInitialMappings + MaximumRuntimeAddressSpaceMappings};

enum class AddressSpaceState : Foundation::UInt8
{
    Uninitialized,
    Unbound,
    Ready,
    ActivationPending,
    ShootdownPending,
    Faulted,
    Quiesced,
};

enum class AddressSpaceError : Foundation::UInt8
{
    None,
    InvalidConfiguration,
    AlreadyInitialized,
    NotInitialized,
    InvalidIdentity,
    RegistryMismatch,
    ObjectNotFound,
    ObjectTypeMismatch,
    ObjectPinUnavailable,
    ObjectInstanceMismatch,
    AlreadyBound,
    NotReady,
    Faulted,
    Quiesced,
    InvalidProcessor,
    ProcessorAlreadyResident,
    ProcessorNotResident,
    ActivationPending,
    ActivationSequenceExhausted,
    ProcessorTransitionPending,
    UserMappingInUse,
    ProcessorTransitionSequenceExhausted,
    InvalidProcessorTransitionReceipt,
    PageTableActivationRejected,
    PageTableActivationCommittedStateUnknown,
    ShootdownPending,
    MappingSequenceExhausted,
    InvalidMappingPlan,
    AddressSpaceMismatch,
    MemoryObjectRegistryMismatch,
    MemoryObjectNotFound,
    MemoryObjectTypeMismatch,
    MemoryObjectPinUnavailable,
    MemoryObjectRejected,
    DeviceRegionRegistryMismatch,
    DeviceRegionNotFound,
    DeviceRegionTypeMismatch,
    DeviceRegionPinUnavailable,
    DeviceRegionRejected,
    FrameRegistryMismatch,
    FrameNotFound,
    FrameTypeMismatch,
    FramePinUnavailable,
    VirtualOverlap,
    PhysicalOwnershipConflict,
    PhysicalWriteExecuteAlias,
    MappingCapacityExceeded,
    PageTableOwnershipRejected,
    PageTableRejected,
    PageTableProtocolViolation,
    InvalidShootdownToken,
    ProcessorNotTargeted,
    ShootdownContention,
    ShootdownIncomplete,
    ShootdownFailed,
    ProcessorsStillResident,
    /// BeginUnmap names no completed runtime mapping of exactly that range.
    MappingNotFound,
};

class AddressSpaceMappingSequencePolicy final
{
public:
    static constexpr Foundation::UInt64 Maximum{(Foundation::UInt64{1U} << 55U) - 1U};

    AddressSpaceMappingSequencePolicy() = delete;

    [[nodiscard]] static constexpr bool TryAdvance(const Foundation::UInt64 current, Foundation::UInt64& next) noexcept
    {
        if (current >= Maximum)
        {
            return false;
        }
        next = current + 1U;
        return true;
    }
};

class AddressSpace;

/// Authorizes one CPU's bounded root transition while mapping is excluded.
/// The AddressSpace outlives every copy and use of this receipt.
class AddressSpaceTransitionReceipt final
{
public:
    constexpr AddressSpaceTransitionReceipt() noexcept = default;

    [[nodiscard]] constexpr Foundation::UInt64 GetSequence() const noexcept
    {
        return m_sequenceAndProcessor >> ProcessorBits;
    }

    [[nodiscard]] constexpr Scheduler::CpuId GetProcessor() const noexcept
    {
        return Scheduler::CpuId{static_cast<Foundation::UInt8>(m_sequenceAndProcessor & ProcessorMask)};
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return (m_addressSpace != nullptr) && (GetSequence() != 0U) && GetProcessor().IsValid();
    }

    [[nodiscard]] constexpr bool IsFor(const AddressSpace& addressSpace) const noexcept
    {
        return m_addressSpace == &addressSpace;
    }

private:
    friend class AddressSpace;

    static constexpr Foundation::UInt8 ProcessorBits{2U};
    static constexpr Foundation::UInt64 ProcessorMask{0x03U};

    constexpr AddressSpaceTransitionReceipt(const AddressSpace& addressSpace, const Foundation::UInt64 sequence,
                                            const Scheduler::CpuId processor) noexcept
        : m_addressSpace{&addressSpace},
          m_sequenceAndProcessor{(sequence << ProcessorBits) | static_cast<Foundation::UInt64>(processor.GetValue())}
    {
    }

    const AddressSpace* m_addressSpace{nullptr};
    Foundation::UInt64 m_sequenceAndProcessor{0U};
};

struct AddressSpaceActivationReceipt
{
    Foundation::UInt64 Sequence;
    Scheduler::CpuId Processor;
    Foundation::UInt8 Reserved[7];
};

struct AddressSpaceActivationFailure
{
    AddressSpaceError Error;
    Hal::Memory::AddressSpacePageTableActivationError PageTable;
    bool ProcessorMayBeResident;
    Foundation::UInt8 Reserved[5];
};

/// Names one in-flight page-table update and its exact shootdown generation.
/// The AddressSpace outlives every copy and use of this token.
class AddressSpaceMappingToken final
{
public:
    constexpr AddressSpaceMappingToken() noexcept = default;

    [[nodiscard]] constexpr Foundation::UInt64 GetSequence() const noexcept
    {
        return m_sequence;
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return (m_addressSpace != nullptr) && (m_sequence != 0U);
    }

    [[nodiscard]] constexpr bool IsEqualTo(const AddressSpaceMappingToken other) const noexcept
    {
        return (m_addressSpace == other.m_addressSpace) && (m_sequence == other.m_sequence);
    }

    [[nodiscard]] constexpr bool IsFor(const AddressSpace& addressSpace) const noexcept
    {
        return m_addressSpace == &addressSpace;
    }

private:
    friend class AddressSpace;

    constexpr AddressSpaceMappingToken(const AddressSpace& addressSpace, const Foundation::UInt64 sequence) noexcept
        : m_addressSpace{&addressSpace}, m_sequence{sequence}
    {
    }

    const AddressSpace* m_addressSpace{nullptr};
    Foundation::UInt64 m_sequence{0U};
};

class AddressSpaceShootdownRequest final
{
public:
    constexpr AddressSpaceShootdownRequest() noexcept = default;

    [[nodiscard]] constexpr AddressSpaceMappingToken GetToken() const noexcept
    {
        return m_token;
    }

    [[nodiscard]] constexpr VirtualAddress GetInvalidationStart() const noexcept
    {
        return m_invalidationStart;
    }

    [[nodiscard]] constexpr PageCount GetInvalidationPages() const noexcept
    {
        return m_invalidationPages;
    }

private:
    friend class AddressSpace;

    constexpr AddressSpaceShootdownRequest(const AddressSpaceMappingToken token, const VirtualAddress invalidationStart,
                                           const PageCount invalidationPages) noexcept
        : m_token{token}, m_invalidationStart{invalidationStart}, m_invalidationPages{invalidationPages}
    {
    }

    AddressSpaceMappingToken m_token{};
    VirtualAddress m_invalidationStart{};
    PageCount m_invalidationPages{};
};

struct AddressSpaceMappingReceipt
{
    Foundation::UInt64 MappingSequence;
    Foundation::Size MappingCount;
    VirtualAddress VirtualStart;
    PageCount Pages;
};

/// A bounded range spans at most two mappings. Owned by a serialized thread accessor; released
/// before that accessor resets.
struct UserMappingUse
{
    Foundation::Size Slots[2U]{};
    Foundation::Size Count{0U};
};

struct AddressSpaceSnapshot
{
    AddressSpaceId Identity;
    Foundation::Size MappingCount;
    Foundation::UInt64 ShootdownSequence;
    Foundation::UInt64 TransitionSequence;
    AddressSpaceState State;
    Foundation::ProcessorMask ResidentProcessorMask;
    Scheduler::CpuId ActivatingProcessor;
    Scheduler::CpuId TransitionProcessor;
    Foundation::ProcessorMask TargetProcessorMask;
    Foundation::ProcessorMask AcknowledgedProcessorMask;
    Foundation::UInt8 Reserved[2];
};

struct AddressSpaceFailure
{
    AddressSpaceError Error;
    Object::ObjectPinError PinError;
    Hal::Memory::AddressSpacePageTableError PageTableError;
    Foundation::UInt8 Reserved[5];
};

/// Owns one architecture page-table backend, its physical-alias ledger, and the bounded 1-4 CPU
/// TLB-shootdown transaction for each new or removed runtime mapping. Initialize imports the
/// verified bootstrap plan. BindIdentity is completed immediately after ObjectRegistry publication
/// and before the identity is distributed. Runtime mappings retain their exact MemoryObject and
/// Frame generations, plus the exact DeviceRegion for device memory, until Quiesce.
/// TlbShootdownDispatcher performs the current CPU's invalidation and publishes one all-or-none
/// batch of opaque work for the other target CPUs; each CPU acknowledges only after local
/// invalidation. CompleteMapping exposes the ledger entry only after every target has completed.
/// Bootstrap ownership boundary: - every seed is produced by the platform's verified canonical
/// bootstrap profile; - seed-owned physical pages are reserved for that profile and are never
/// issued as Frames; - the memory-manager factory preserves this invariant across all roots.
/// Runtime Frame mappings are additionally checked through Frame's shared interval policy. A Frame
/// claims its pages in the registry while any of its reservations is held, so independently owned
/// roots and IOMMU domains cannot create writable versus executable aliases, or differently cached
/// aliases, of the same pages through different Frames.
class AddressSpace final : public Object::IKernelObject
{
public:
    constexpr AddressSpace() noexcept = default;

    /// Preconditions:
    /// - State is Uninitialized or Quiesced.
    /// - No processor can activate the backend root and no operation is live.
    ~AddressSpace() noexcept = default;

    AddressSpace(const AddressSpace&) = delete;
    AddressSpace& operator=(const AddressSpace&) = delete;
    AddressSpace(AddressSpace&&) = delete;
    AddressSpace& operator=(AddressSpace&&) = delete;

    [[nodiscard]] AddressSpaceError Initialize(Object::ResourceDomainId chargeOwner,
                                               Hal::Memory::IAddressSpacePageTable& pageTable,
                                               const InitialMappingPlan& seedPlan) noexcept;

    [[nodiscard]] AddressSpaceFailure BindIdentity(Object::ObjectRegistry& registry, AddressSpaceId identity) noexcept;

    /// Excludes mappings and other processor transitions until the exact
    /// receipt is released. Acquisition never waits for another transition.
    /// ProcessorTransitionPending, ActivationPending, and ShootdownPending
    /// are retryable after the conflicting operation completes.
    [[nodiscard]] Foundation::Result<AddressSpaceTransitionReceipt, AddressSpaceError>
    ReserveProcessorTransition(Scheduler::CpuId processor) noexcept;

    /// Activates this root for the CPU named by the current reservation.
    [[nodiscard]] Foundation::Result<AddressSpaceActivationReceipt, AddressSpaceActivationFailure>
    ActivateReservedProcessor(AddressSpaceTransitionReceipt transition) noexcept;

    /// Detaches the exact reserved CPU after its native root changed.
    [[nodiscard]] AddressSpaceError DetachReservedProcessor(AddressSpaceTransitionReceipt transition) noexcept;

    /// Ends one exact pre-commit rollback or completed root transition.
    [[nodiscard]] AddressSpaceError ReleaseProcessorTransition(AddressSpaceTransitionReceipt transition) noexcept;

    /// Serializes one complete backend root activation against mapping and
    /// quiescence. Success publishes processor residency only after the native
    /// root switch completes. A failure with ProcessorMayBeResident false
    /// leaves the prior root authoritative and is safe for scheduler-admission
    /// rollback. A true value is terminal and retains conservative residency.
    /// Preconditions:
    /// - processor is the calling CPU;
    /// - local interrupts are disabled and same-CPU re-entry is excluded;
    /// - the claimed page-table backend outlives this AddressSpace.
    [[nodiscard]] Foundation::Result<AddressSpaceActivationReceipt, AddressSpaceActivationFailure>
    ActivateProcessor(Scheduler::CpuId processor) noexcept;

    /// Removes a processor after it has switched to another root or performed
    /// an equivalent complete local flush.
    [[nodiscard]] AddressSpaceError DetachProcessor(Scheduler::CpuId processor) noexcept;

    [[nodiscard]] Foundation::Result<AddressSpaceShootdownRequest, AddressSpaceFailure>
    BeginFaultMapping(const FaultFrameMappingPreparation& preparation) noexcept;

    /// Begins one proactive supervisor or user mapping after the caller has
    /// prepared exact pinned logical and physical ownership. target identifies
    /// this exact root but grants no authority. The common transaction retains
    /// its own object generations through completion and later quiescence.
    [[nodiscard]] Foundation::Result<AddressSpaceShootdownRequest, AddressSpaceFailure>
    BeginPinnedFrameMapping(AddressSpaceId target, const PinnedFrameMappingPreparation& preparation) noexcept;

    /// Begins removing the one completed runtime mapping whose range is exactly [virtualStart,
    /// virtualStart + pages). The backend clears its translations now; the record, its pins, the
    /// Frame's W^X reservation and the MemoryObject's mapping stay until CompleteMapping observes
    /// every resident processor's acknowledgement of the same shootdown protocol as a mapping, and
    /// then free its slot. The mapping is not visible to TryGetMapping while the unmap is in
    /// flight. A rejected unmap leaves the mapping in place.
    [[nodiscard]] Foundation::Result<AddressSpaceShootdownRequest, AddressSpaceFailure>
    BeginUnmap(VirtualAddress virtualStart, PageCount pages) noexcept;

    /// Interrupt-safe, wait-free acknowledgement after local invalidation.
    /// The first acknowledgement or rejection for one CPU is final; later
    /// duplicate reports cannot change that CPU's outcome.
    [[nodiscard]] AddressSpaceError AcknowledgeShootdown(AddressSpaceMappingToken token,
                                                         Scheduler::CpuId processor) noexcept;

    /// Interrupt-safe, wait-free rejection when local invalidation cannot be
    /// completed. CompleteMapping observes the marker and makes the
    /// AddressSpace terminal from its serialized caller context.
    [[nodiscard]] AddressSpaceError RejectShootdown(AddressSpaceMappingToken token,
                                                    Scheduler::CpuId processor) noexcept;

    [[nodiscard]] Foundation::Result<AddressSpaceMappingReceipt, AddressSpaceFailure>
    CompleteMapping(AddressSpaceMappingToken token) noexcept;

    /// Compatibility entry for fault-resolution callers. It completes the
    /// same architecture-neutral mapping transaction as CompleteMapping.
    [[nodiscard]] Foundation::Result<AddressSpaceMappingReceipt, AddressSpaceFailure>
    CompleteFaultMapping(AddressSpaceMappingToken token) noexcept;

    /// Makes the object terminal after a shootdown transport failure. Native
    /// mappings and retained pins remain owned until platform quiescence.
    [[nodiscard]] AddressSpaceError FailShootdown(AddressSpaceMappingToken token) noexcept;

    /// Releases runtime logical, device, and physical mapping references after
    /// every CPU has stopped using this root.
    [[nodiscard]] AddressSpaceError Quiesce() noexcept;

    [[nodiscard]] AddressSpaceSnapshot GetSnapshot() const noexcept;
    // Narrow observations avoid copying the widened mask snapshot onto the target's 512-byte stack.
    [[nodiscard]] AddressSpaceId GetIdentity() const noexcept;
    /// Validate this CPU's receipt, not the snapshot's diagnostic aggregate of outstanding
    /// reservations.
    [[nodiscard]] bool IsCurrentProcessorTransition(AddressSpaceTransitionReceipt transition) const noexcept;
    [[nodiscard]] AddressSpaceState GetState() const noexcept;
    [[nodiscard]] Foundation::ProcessorMask GetShootdownTargets() const noexcept;

    [[nodiscard, gnu::noinline]] Foundation::Result<AddressSpaceShootdownRequest, AddressSpaceFailure>
    WaitForFaultMapping(const FaultFrameMappingPreparation& preparation) noexcept;
    [[nodiscard, gnu::noinline]] Foundation::Result<AddressSpaceShootdownRequest, AddressSpaceFailure>
    WaitForPinnedFrameMapping(AddressSpaceId target, const PinnedFrameMappingPreparation& preparation) noexcept;
    [[nodiscard, gnu::noinline]] Foundation::Result<AddressSpaceShootdownRequest, AddressSpaceFailure>
    WaitForUnmap(VirtualAddress start, PageCount pages) noexcept;
    // Intent spans retries with no lock held.
    void BeginMappingIntent() noexcept;
    void EndMappingIntent() noexcept;
    [[nodiscard]] Foundation::Result<UserMappingUse, AddressSpaceError>
    PinUserMapping(Foundation::UInt64 start, Foundation::UInt64 sizeBytes, bool write,
                   bool includeSeed = false) const noexcept;
    void ReleaseUserMapping(UserMappingUse use) const noexcept;

    [[nodiscard]] Foundation::Result<AddressSpaceMappingRecord, AddressSpaceError>
    TryGetMapping(Foundation::Size index) const noexcept;

    /// The one completed runtime mapping that holds all of [start, start + sizeBytes): a user,
    /// readable RAM mapping, writable too when write is set. Seed mappings never qualify, and
    /// neither does the slot of a transaction in flight. AccessDenied-like failures report
    /// ObjectNotFound.
    [[nodiscard]] Foundation::Result<AddressSpaceMappingRecord, AddressSpaceError>
    FindUserMapping(Foundation::UInt64 start, Foundation::UInt64 sizeBytes, bool write) const noexcept;

    [[nodiscard]] Object::ObjectType GetObjectType() const noexcept override;
    [[nodiscard]] Object::ResourceDomainId GetChargeOwner() const noexcept override;

private:
    [[nodiscard]] static AddressSpaceFailure MakeFailure(AddressSpaceError error,
                                                         Object::ObjectPinError pinError = Object::ObjectPinError::Ok,
                                                         Hal::Memory::AddressSpacePageTableError pageTableError =
                                                             Hal::Memory::AddressSpacePageTableError::None) noexcept;

    [[nodiscard]] AddressSpaceError ValidateMappingLocked(const AddressSpaceMappingRecord& mapping,
                                                          bool readOnlyBootstrapAlias = false) const noexcept;

    [[nodiscard]] AddressSpaceError ValidateBeginMappingStateLocked() const noexcept;

    /// The first runtime slot whose record is empty, or
    /// MaximumRuntimeAddressSpaceMappings when every slot holds a mapping.
    [[nodiscard]] Foundation::Size FindFreeRuntimeSlotLocked() const noexcept;

    /// Releases slot's Frame W^X reservation, DeviceRegion and MemoryObject
    /// mappings and pins, and empties its record.
    void ReleaseRuntimeSlotLocked(Foundation::Size slot) noexcept;

    [[nodiscard]] Foundation::Result<AddressSpaceMappingReceipt, AddressSpaceFailure> CompleteUnmapLocked() noexcept;

    [[nodiscard]] Foundation::Result<AddressSpaceShootdownRequest, AddressSpaceFailure>
    BeginMappingLocked(const AddressSpaceMappingRecord& mapping) noexcept;

    [[nodiscard]] static AddressSpaceMappingRecord ConvertFaultMapping(const FaultFrameMappingPlan& plan) noexcept;

    [[nodiscard]] static AddressSpaceMappingRecord ConvertSeedMapping(const InitialMapping& mapping) noexcept;

    [[nodiscard]] static Foundation::UInt64 PackShootdownState(Foundation::UInt64 sequence,
                                                               Foundation::ProcessorMask targetMask,
                                                               Foundation::ProcessorMask acknowledgedMask) noexcept;

    [[nodiscard]] bool IsCurrentTransitionLocked(AddressSpaceTransitionReceipt transition) const noexcept;

    [[nodiscard]] Foundation::Result<AddressSpaceActivationReceipt, AddressSpaceActivationFailure>
    ActivateProcessorLocked(Scheduler::CpuId processor) noexcept;

    [[nodiscard]] AddressSpaceError DetachProcessorLocked(Scheduler::CpuId processor) noexcept;

    mutable Platform::Synchronization::SpinLock m_synchronization;
    Platform::Synchronization::AtomicUInt64 m_chargeOwnerValue;
    Platform::Synchronization::AtomicUInt64 m_shootdownState;
    Object::ObjectRegistry* m_registry{nullptr};
    Hal::Memory::IAddressSpacePageTable* m_pageTable{nullptr};
    Hal::Memory::AddressSpacePageTableMapResult m_pageTableMapResult{};
    Foundation::UInt64 m_pendingMappingSequence{0U};
    AddressSpaceId m_identity{};
    AddressSpaceMappingRecord m_mappings[MaximumAddressSpaceMappings]{};
    Object::ObjectPin m_runtimeMemoryObjectPins[MaximumRuntimeAddressSpaceMappings]{};
    Object::ObjectPin m_runtimeDeviceRegionPins[MaximumRuntimeAddressSpaceMappings]{};
    Object::ObjectPin m_runtimeFramePins[MaximumRuntimeAddressSpaceMappings]{};
    Foundation::Size m_seedMappingCount{0U};
    /// Runtime mappings whose transaction completed. A runtime record lives
    /// in slot m_seedMappingCount + slot of m_mappings, its pins at slot;
    /// a slot is free while its record is empty (zero pages).
    Foundation::Size m_runtimeMappingCount{0U};
    /// The slot of the mapping transaction in flight, or
    /// MaximumRuntimeAddressSpaceMappings.
    Foundation::Size m_pendingRuntimeSlot{MaximumRuntimeAddressSpaceMappings};
    /// Whether the transaction in flight removes m_pendingRuntimeSlot.
    bool m_pendingUnmap{false};
    Foundation::UInt64 m_mappingSequence{0U};
    Foundation::UInt64 m_activationSequence{0U};
    Foundation::UInt64 m_transitionSequence{0U};
    Foundation::UInt64 m_reservedTransitionSequence{0U};
    Foundation::UInt64 m_transitionReservations[Scheduler::MaximumProcessorCount]{};
    Foundation::ProcessorMask m_transitionMask{0U};
    Foundation::UInt64 m_mappingIntents{0U};
    mutable Foundation::UInt32 m_mappingUses[MaximumAddressSpaceMappings]{};
    Foundation::ProcessorMask m_residentProcessorMask{0U};
    Scheduler::CpuId m_activatingProcessor{};
    Scheduler::CpuId m_transitionProcessor{};
    AddressSpaceState m_state{AddressSpaceState::Uninitialized};
};

class MappingIntentGuard final
{
public:
    explicit MappingIntentGuard(AddressSpace& space) noexcept : m_space{space}
    {
        m_space.BeginMappingIntent();
    }
    ~MappingIntentGuard() noexcept
    {
        m_space.EndMappingIntent();
    }
    MappingIntentGuard(const MappingIntentGuard&) = delete;
    MappingIntentGuard& operator=(const MappingIntentGuard&) = delete;

private:
    AddressSpace& m_space;
};

static_assert(AddressSpaceMappingSequencePolicy::Maximum == 0x007F'FFFF'FFFF'FFFFULL);
static_assert(sizeof(AddressSpaceState) == 1U);
static_assert(sizeof(AddressSpaceError) == 1U);
static_assert(sizeof(AddressSpaceTransitionReceipt) == 16U);
static_assert(alignof(AddressSpaceTransitionReceipt) == 8U);
static_assert(sizeof(AddressSpaceActivationReceipt) == 16U);
static_assert(alignof(AddressSpaceActivationReceipt) == 8U);
static_assert(sizeof(AddressSpaceActivationFailure) == 8U);
static_assert(alignof(AddressSpaceActivationFailure) == 1U);
static_assert(sizeof(AddressSpaceMappingToken) == 16U);
static_assert(alignof(AddressSpaceMappingToken) == 8U);
static_assert(sizeof(AddressSpaceShootdownRequest) == 32U);
static_assert(alignof(AddressSpaceShootdownRequest) == 8U);
static_assert(sizeof(AddressSpaceMappingReceipt) == 32U);
static_assert(alignof(AddressSpaceMappingReceipt) == 8U);
static_assert(sizeof(AddressSpaceSnapshot) == 88U);
static_assert(alignof(AddressSpaceSnapshot) == 8U);
static_assert(sizeof(AddressSpaceFailure) == 8U);
} // Namespace Asteria::Kernel::Memory

#endif // ASTERIA_KERNEL_MEMORY_ADDRESS_SPACE_HPP_INCLUDED
