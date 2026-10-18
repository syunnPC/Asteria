// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_PROCESSOR_ADDRESS_SPACE_CONTROLLER_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_PROCESSOR_ADDRESS_SPACE_CONTROLLER_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Memory/AddressSpace.hpp"

namespace Asteria::Kernel::Memory
{
enum class ProcessorAddressSpaceControllerState : Foundation::UInt8
{
    Uninitialized,
    Ready,
    Faulted,
};

enum class ProcessorAddressSpaceControllerError : Foundation::UInt8
{
    None,
    AlreadyInitialized,
    InvalidConfiguration,
    NotReady,
    TransitionRejected,
    TransitionReleaseRejected,
    ActivationRejected,
    ActiveRootInvariantViolation,
    DestinationRootInvariantViolation,
    DetachRejected,
    Faulted,
};

struct ProcessorAddressSpaceControllerFailure
{
    ProcessorAddressSpaceControllerError Error;
    AddressSpaceActivationFailure Activation;
    AddressSpaceError Transition;
    AddressSpaceError Detach;
    bool AdmissionRollbackSafe;
    Foundation::UInt8 Reserved[4];
};

struct ProcessorAddressSpaceControllerReceipt
{
    AddressSpaceId Previous;
    AddressSpaceId Active;
    Scheduler::CpuId Processor;
    bool NativeActivationPerformed;
    Foundation::UInt8 Reserved[6];
};

struct ProcessorAddressSpaceControllerSnapshot
{
    AddressSpace* IdleStorage;
    AddressSpace* ActiveStorage;
    AddressSpace* PreviousStorage;
    AddressSpaceId Idle;
    AddressSpaceId Active;
    AddressSpaceId Previous;
    AddressSpaceTransitionReceipt SourceTransition;
    AddressSpaceTransitionReceipt DestinationTransition;
    Scheduler::CpuId Processor;
    ProcessorAddressSpaceControllerState State;
    Foundation::UInt8 Reserved[6];
};

/// Owns one processor's serialized native-root selection policy. Initialize adopts a dedicated idle
/// AddressSpace by activating it through the same backend transaction used for every later root.
/// SwitchTo activates the destination first and detaches the previous resident root only after that
/// commit. Source and destination reservations exclude mapping across the complete transaction. The
/// temporary dual-resident interval is conservative for TLB shootdown. No operation holds two
/// AddressSpace locks simultaneously. A failure with AdmissionRollbackSafe true proves that the
/// native root and this controller's active binding are unchanged. Every other failure is a
/// fail-stop outcome: scheduler admission must not be rolled back as if no architecture state
/// changed.
class ProcessorAddressSpaceController final
{
public:
    constexpr ProcessorAddressSpaceController() noexcept = default;

    /// Activates idleAddressSpace as this CPU's first tracked root.
    /// Preconditions:
    /// - local interrupts are disabled and same-CPU re-entry is excluded;
    /// - idleAddressSpace and its backend outlive this controller;
    /// - no other owner changes this CPU's native root.
    [[nodiscard]] Foundation::Result<ProcessorAddressSpaceControllerReceipt, ProcessorAddressSpaceControllerFailure>
    Initialize(Scheduler::CpuId processor, AddressSpace& idleAddressSpace) noexcept;

    /// Selects destination before any future native-context switch. Selecting
    /// the already active exact object is a validated no-op.
    [[nodiscard]] Foundation::Result<ProcessorAddressSpaceControllerReceipt, ProcessorAddressSpaceControllerFailure>
    SwitchTo(AddressSpace& destination) noexcept;

    [[nodiscard]] Foundation::Result<ProcessorAddressSpaceControllerReceipt, ProcessorAddressSpaceControllerFailure>
    SwitchToIdle() noexcept;

    // Owning idle CPU adopts its permanent private idle root after the shared-root boot proof,
    // before user dispatch.
    [[nodiscard]] bool AdoptIdleRoot(AddressSpace& destination) noexcept;

    /// Owning-CPU observation. Each valid transition receipt is non-owning
    /// authority for one reservation still known to be current. Previous is
    /// retained through source detach while that source receipt remains
    /// authoritative. The caller excludes same-CPU re-entry.
    [[nodiscard]] constexpr ProcessorAddressSpaceControllerSnapshot GetSnapshot() const noexcept
    {
        return ProcessorAddressSpaceControllerSnapshot{
            .IdleStorage = m_idle,
            .ActiveStorage = m_active,
            .PreviousStorage = m_previous,
            .Idle = m_idleIdentity,
            .Active = m_activeIdentity,
            .Previous = m_previousIdentity,
            .SourceTransition = m_sourceTransition,
            .DestinationTransition = m_destinationTransition,
            .Processor = m_processor,
            .State = m_state,
            .Reserved = {},
        };
    }

    /// Preconditions:
    /// - no operation or saved continuation can access this object;
    /// - idle and every active AddressSpace storage still outlive it.
    ~ProcessorAddressSpaceController() noexcept = default;

    ProcessorAddressSpaceController(const ProcessorAddressSpaceController&) = delete;
    ProcessorAddressSpaceController& operator=(const ProcessorAddressSpaceController&) = delete;
    ProcessorAddressSpaceController(ProcessorAddressSpaceController&&) = delete;
    ProcessorAddressSpaceController& operator=(ProcessorAddressSpaceController&&) = delete;

private:
    [[nodiscard]] static ProcessorAddressSpaceControllerFailure
    MakeFailure(ProcessorAddressSpaceControllerError error,
                const AddressSpaceActivationFailure& activation = AddressSpaceActivationFailure{},
                AddressSpaceError detach = AddressSpaceError::None,
                AddressSpaceError transition = AddressSpaceError::None, bool admissionRollbackSafe = false) noexcept;

    [[nodiscard]] ProcessorAddressSpaceControllerReceipt MakeReceipt(AddressSpaceId previous,
                                                                     bool nativeActivationPerformed) const noexcept;

    [[nodiscard]] Foundation::Result<ProcessorAddressSpaceControllerReceipt, ProcessorAddressSpaceControllerFailure>
    ActivateInitialRoot(AddressSpace& idleAddressSpace) noexcept;

    [[nodiscard]] Foundation::Result<ProcessorAddressSpaceControllerReceipt, ProcessorAddressSpaceControllerFailure>
    ReserveDestinationAndSwitch(AddressSpace& destination, AddressSpace& previousStorage,
                                AddressSpaceId previousIdentity) noexcept;

    [[nodiscard]] Foundation::Result<ProcessorAddressSpaceControllerReceipt, ProcessorAddressSpaceControllerFailure>
    ActivateDestinationAndFinish(AddressSpace& destination, AddressSpace& previousStorage,
                                 AddressSpaceId previousIdentity) noexcept;

    [[nodiscard]] bool ActiveInvariantIsValid(AddressSpaceTransitionReceipt transition) const noexcept;
    [[nodiscard]] bool DestinationInvariantIsValid(const AddressSpace& destination, AddressSpaceId identity,
                                                   AddressSpaceTransitionReceipt transition) const noexcept;
    void ClearPrevious() noexcept;
    void EnterFaulted() noexcept;

    AddressSpace* m_idle{nullptr};
    AddressSpace* m_active{nullptr};
    AddressSpace* m_previous{nullptr};
    AddressSpaceId m_idleIdentity{};
    AddressSpaceId m_activeIdentity{};
    AddressSpaceId m_previousIdentity{};
    AddressSpaceTransitionReceipt m_sourceTransition{};
    AddressSpaceTransitionReceipt m_destinationTransition{};
    Scheduler::CpuId m_processor{};
    ProcessorAddressSpaceControllerState m_state{ProcessorAddressSpaceControllerState::Uninitialized};
};

static_assert(sizeof(ProcessorAddressSpaceControllerState) == 1U);
static_assert(sizeof(ProcessorAddressSpaceControllerError) == 1U);
static_assert(sizeof(ProcessorAddressSpaceControllerFailure) == 16U);
static_assert(alignof(ProcessorAddressSpaceControllerFailure) == 1U);
static_assert(sizeof(ProcessorAddressSpaceControllerReceipt) == 40U);
static_assert(alignof(ProcessorAddressSpaceControllerReceipt) == 8U);
static_assert(sizeof(ProcessorAddressSpaceControllerSnapshot) == 112U);
static_assert(alignof(ProcessorAddressSpaceControllerSnapshot) == 8U);

} // Namespace Asteria::Kernel::Memory

#endif // ASTERIA_KERNEL_MEMORY_PROCESSOR_ADDRESS_SPACE_CONTROLLER_HPP_INCLUDED
