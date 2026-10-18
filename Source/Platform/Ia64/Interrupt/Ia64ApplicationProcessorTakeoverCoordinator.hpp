// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_INTERRUPT_IA64_APPLICATION_PROCESSOR_TAKEOVER_COORDINATOR_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_INTERRUPT_IA64_APPLICATION_PROCESSOR_TAKEOVER_COORDINATOR_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Scheduler/SchedulerTypes.hpp"
#include "Platform/Ia64/Firmware/PalBootstrapCapabilityCapture.hpp"
#include "Platform/Ia64/Memory/ApplicationProcessorExecutionStorageOwner.hpp"
#include "Platform/Ia64/Memory/InitialInterruptVectorTableInstaller.hpp"
#include "Platform/Ia64/Memory/InitialTranslationInstaller.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"

namespace Asteria::Platform::Ia64::Interrupt
{
enum class Ia64ApplicationProcessorTakeoverCoordinatorState : Foundation::UInt8
{
    Uninitialized,
    Ready,
    InitializationFailed,
};

enum class Ia64ApplicationProcessorTakeoverLifecycle : Foundation::UInt8
{
    Unused,
    Prepared,
    Claimed,
    OwnedPhysical,
    PalCapabilitiesPublished,
    InitialTranslationsInstalled,
    Failed,
};

enum class Ia64ApplicationProcessorTakeoverInitializationError : Foundation::UInt32
{
    None,
    AlreadyInitialized,
    OwnerNotReady,
    OwnerFailed,
    InvalidOwnerProcessorCount,
    InvalidOwnerProcessorMask,
    InvalidOwnerDescriptorView,
    InvalidOwnerDescriptor,
};

enum class Ia64ApplicationProcessorTakeoverClaimError : Foundation::UInt32
{
    None,
    CoordinatorNotReady,
    UnknownProcessor,
    BootstrapProcessor,
    DuplicateClaim,
};

enum class Ia64ApplicationProcessorTakeoverPublicationError : Foundation::UInt32
{
    None,
    CoordinatorNotReady,
    UnknownProcessor,
    BootstrapProcessor,
    InvalidPrecedingState,
    InvalidFailurePayload,
};

enum class Ia64ApplicationProcessorTakeoverSnapshotError : Foundation::UInt32
{
    None,
    CoordinatorNotReady,
    InvalidProcessorSlot,
};

enum class Ia64ApplicationProcessorTakeoverFailurePhase : Foundation::UInt32
{
    None,
    InterruptVectorTableInstallation,
    PalCapabilityCapture,
    InitialTranslationInstallation,
};

/// Immutable bridge operands retained in one naturally aligned coordinator
/// slot. ProcessorValue is the zero-extended logical CpuId. A returned pointer
/// is non-owning and remains stable for the lifetime of its coordinator.
struct Ia64ApplicationProcessorTakeoverRecord
{
    Foundation::UInt64 InitialStackPointer;
    Foundation::UInt64 RseBackingStorePhysicalStart;
    Foundation::UInt64 ProcessorValue;
};

/// Exact terminal error publication. Only the payload selected by Phase is
/// meaningful; the other payloads remain all-zero.
struct Ia64ApplicationProcessorTakeoverFailure
{
    Ia64ApplicationProcessorTakeoverFailurePhase Phase;
    Memory::InitialInterruptVectorTableApInstallStatus InterruptVectorTableInstallationStatus;
    Firmware::PalBootstrapCapabilityCaptureError PalCapabilityCapture;
    Memory::InitialTranslationInstallError InitialTranslationInstallationError;
};

/// One acquire-observed slot. PAL capabilities are present from
/// PalCapabilitiesPublished onward, including either initial-translation
/// terminal outcome. Failure remains zero until Failed is release-published.
struct Ia64ApplicationProcessorTakeoverProcessorSnapshot
{
    Ia64ApplicationProcessorTakeoverRecord Takeover;
    Firmware::PalBootstrapCapabilities PalCapabilities;
    Ia64ApplicationProcessorTakeoverFailure Failure;
    Ia64ApplicationProcessorTakeoverLifecycle Lifecycle;
    Foundation::UInt8 Reserved[7];
};

/// Compact bounded progress used by a BSP finite polling policy. TerminalMask
/// includes only InitialTranslationsInstalled and Failed; FailedMask is its
/// failed subset.
struct Ia64ApplicationProcessorTakeoverProgressSnapshot
{
    Ia64ApplicationProcessorTakeoverLifecycle Processors[Kernel::Scheduler::MaximumProcessorCount];
    Foundation::UInt8 ProcessorCount;
    Foundation::UInt8 EnabledProcessorMask;
    Foundation::UInt8 TerminalMask;
    Foundation::UInt8 FailedMask;
    Ia64ApplicationProcessorTakeoverCoordinatorState State;
    Foundation::UInt8 Reserved[7];
};

/// Pure-C++ publication state for one boot generation of IA-64 AP takeover.
///
/// Initialization is BSP-serialized and single-use. APs may independently
/// operate on distinct enabled slots. The component performs no PAL call,
/// privileged operation, stack switch, translation change, scheduler
/// admission, or parking action. OwnedPhysical records only the caller's
/// publication that the claim has reached its external physical-mode owner.
///
/// Allocation:
/// - Does not allocate.
/// Blocking:
/// - Every operation is bounded by the one-to-four processor target limit.
class Ia64ApplicationProcessorTakeoverCoordinator final
{
public:
    constexpr Ia64ApplicationProcessorTakeoverCoordinator() noexcept = default;

    ~Ia64ApplicationProcessorTakeoverCoordinator() noexcept = default;

    Ia64ApplicationProcessorTakeoverCoordinator(const Ia64ApplicationProcessorTakeoverCoordinator&) = delete;
    Ia64ApplicationProcessorTakeoverCoordinator& operator=(const Ia64ApplicationProcessorTakeoverCoordinator&) = delete;
    Ia64ApplicationProcessorTakeoverCoordinator(Ia64ApplicationProcessorTakeoverCoordinator&&) = delete;
    Ia64ApplicationProcessorTakeoverCoordinator& operator=(Ia64ApplicationProcessorTakeoverCoordinator&&) = delete;

    /// Validates a Ready execution-storage owner and copies exactly its
    /// enabled non-BSP records in ascending CpuId order. Any failure is
    /// terminal for this coordinator and publishes no Prepared slot.
    [[nodiscard]] Ia64ApplicationProcessorTakeoverInitializationError
    Initialize(const Memory::ApplicationProcessorExecutionStorageOwner& owner) noexcept;

    /// Atomically performs Prepared -> Claimed and returns the exact stable
    /// record for that AP. Unknown, BSP, and repeated claims mutate nothing.
    [[nodiscard]] Foundation::Result<const Ia64ApplicationProcessorTakeoverRecord*,
                                     Ia64ApplicationProcessorTakeoverClaimError>
    Claim(Kernel::Scheduler::CpuId processor) noexcept;

    /// Atomically records Claimed -> OwnedPhysical. The caller must already
    /// have completed the AP stack/RSE switch and shared fail-stop IVA
    /// installation. This component performs and independently attests none of
    /// those operations and establishes no translation or scheduler state.
    [[nodiscard]] Ia64ApplicationProcessorTakeoverPublicationError
    PublishOwnedPhysical(Kernel::Scheduler::CpuId processor) noexcept;

    /// Copies one PAL payload, then release-publishes
    /// OwnedPhysical -> PalCapabilitiesPublished.
    [[nodiscard]] Ia64ApplicationProcessorTakeoverPublicationError
    PublishPalCaptureSuccess(Kernel::Scheduler::CpuId processor,
                             const Firmware::PalBootstrapCapabilities& capabilities) noexcept;

    /// Copies the exact AP-local IVT installation failure status, then
    /// release-publishes Claimed -> Failed.
    [[nodiscard]] Ia64ApplicationProcessorTakeoverPublicationError
    PublishInterruptVectorTableFailure(Kernel::Scheduler::CpuId processor,
                                       Memory::InitialInterruptVectorTableApInstallStatus status) noexcept;

    /// Copies the exact PAL capture failure, then release-publishes
    /// OwnedPhysical -> Failed.
    [[nodiscard]] Ia64ApplicationProcessorTakeoverPublicationError
    PublishPalCaptureFailure(Kernel::Scheduler::CpuId processor,
                             const Firmware::PalBootstrapCapabilityCaptureError& failure) noexcept;

    /// Atomically release-publishes PalCapabilitiesPublished ->
    /// InitialTranslationsInstalled.
    [[nodiscard]] Ia64ApplicationProcessorTakeoverPublicationError
    PublishInitialTranslationInstallSuccess(Kernel::Scheduler::CpuId processor) noexcept;

    /// Copies the exact initial-translation installation error, then
    /// release-publishes PalCapabilitiesPublished -> Failed.
    [[nodiscard]] Ia64ApplicationProcessorTakeoverPublicationError
    PublishInitialTranslationInstallFailure(Kernel::Scheduler::CpuId processor,
                                            Memory::InitialTranslationInstallError error) noexcept;

    /// Acquire-loads every slot once and returns bounded progress and masks.
    [[nodiscard]] Ia64ApplicationProcessorTakeoverProgressSnapshot GetProgressSnapshot() const noexcept;

    /// Acquire-loads one valid physical slot. PAL capabilities are copied once
    /// their intermediate publication is visible, and failure is copied only
    /// after Failed is observed. BSP and absent slots return an Unused,
    /// zero-payload snapshot after successful initialization.
    [[nodiscard]] Foundation::Result<Ia64ApplicationProcessorTakeoverProcessorSnapshot,
                                     Ia64ApplicationProcessorTakeoverSnapshotError>
    GetProcessorSnapshot(Kernel::Scheduler::CpuId processor) const noexcept;

private:
    [[nodiscard]] Ia64ApplicationProcessorTakeoverPublicationError
    ValidatePublicationProcessor(Kernel::Scheduler::CpuId processor, Foundation::UInt8& processorValue) const noexcept;

    [[nodiscard]] Ia64ApplicationProcessorTakeoverPublicationError
    ReservePublication(Kernel::Scheduler::CpuId processor, Ia64ApplicationProcessorTakeoverLifecycle precedingLifecycle,
                       Foundation::UInt64 publishingState, Foundation::UInt8& processorValue) noexcept;

    Ia64ApplicationProcessorTakeoverRecord m_records[Kernel::Scheduler::MaximumProcessorCount]{};
    Firmware::PalBootstrapCapabilities m_palCapabilities[Kernel::Scheduler::MaximumProcessorCount]{};
    Ia64ApplicationProcessorTakeoverFailure m_failures[Kernel::Scheduler::MaximumProcessorCount]{};
    Synchronization::AtomicUInt64 m_lifecycles[Kernel::Scheduler::MaximumProcessorCount]{};
    Synchronization::AtomicUInt64 m_state{};
    Foundation::UInt8 m_processorCount{0U};
    Foundation::UInt8 m_enabledProcessorMask{0U};
    [[maybe_unused]] Foundation::UInt8 m_reserved[6]{};
};

/// Returns the constant-initialized boot-lifetime coordinator used by the
/// native AP takeover bridge. This accessor grants no ownership of the object.
[[nodiscard]] Ia64ApplicationProcessorTakeoverCoordinator&
GetNativeIa64ApplicationProcessorTakeoverCoordinator() noexcept;

static_assert(sizeof(Ia64ApplicationProcessorTakeoverCoordinatorState) == 1U);
static_assert(sizeof(Ia64ApplicationProcessorTakeoverLifecycle) == 1U);
static_assert(sizeof(Ia64ApplicationProcessorTakeoverInitializationError) == 4U);
static_assert(sizeof(Ia64ApplicationProcessorTakeoverClaimError) == 4U);
static_assert(sizeof(Ia64ApplicationProcessorTakeoverPublicationError) == 4U);
static_assert(sizeof(Ia64ApplicationProcessorTakeoverSnapshotError) == 4U);
static_assert(sizeof(Ia64ApplicationProcessorTakeoverFailurePhase) == 4U);
static_assert(sizeof(Ia64ApplicationProcessorTakeoverRecord) == 24U);
static_assert(alignof(Ia64ApplicationProcessorTakeoverRecord) == 8U);
static_assert(__is_aggregate(Ia64ApplicationProcessorTakeoverRecord));
static_assert(__is_standard_layout(Ia64ApplicationProcessorTakeoverRecord));
static_assert(__is_trivially_copyable(Ia64ApplicationProcessorTakeoverRecord));
static_assert(__builtin_offsetof(Ia64ApplicationProcessorTakeoverRecord, InitialStackPointer) == 0U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorTakeoverRecord, RseBackingStorePhysicalStart) == 8U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorTakeoverRecord, ProcessorValue) == 16U);
static_assert(sizeof(Ia64ApplicationProcessorTakeoverFailure) == 28U);
static_assert(alignof(Ia64ApplicationProcessorTakeoverFailure) == 4U);
static_assert(__is_aggregate(Ia64ApplicationProcessorTakeoverFailure));
static_assert(__is_standard_layout(Ia64ApplicationProcessorTakeoverFailure));
static_assert(__is_trivially_copyable(Ia64ApplicationProcessorTakeoverFailure));
static_assert(__builtin_offsetof(Ia64ApplicationProcessorTakeoverFailure, Phase) == 0U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorTakeoverFailure, InterruptVectorTableInstallationStatus) ==
              4U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorTakeoverFailure, PalCapabilityCapture) == 8U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorTakeoverFailure, InitialTranslationInstallationError) == 24U);
static_assert(sizeof(Ia64ApplicationProcessorTakeoverProcessorSnapshot) == 152U);
static_assert(alignof(Ia64ApplicationProcessorTakeoverProcessorSnapshot) == 8U);
static_assert(__is_aggregate(Ia64ApplicationProcessorTakeoverProcessorSnapshot));
static_assert(__is_standard_layout(Ia64ApplicationProcessorTakeoverProcessorSnapshot));
static_assert(__is_trivially_copyable(Ia64ApplicationProcessorTakeoverProcessorSnapshot));
static_assert(sizeof(Ia64ApplicationProcessorTakeoverProgressSnapshot) == 16U);
static_assert(alignof(Ia64ApplicationProcessorTakeoverProgressSnapshot) == 1U);
static_assert(__is_aggregate(Ia64ApplicationProcessorTakeoverProgressSnapshot));
static_assert(__is_standard_layout(Ia64ApplicationProcessorTakeoverProgressSnapshot));
static_assert(__is_trivially_copyable(Ia64ApplicationProcessorTakeoverProgressSnapshot));
static_assert(sizeof(Ia64ApplicationProcessorTakeoverCoordinator) == 608U);
static_assert(alignof(Ia64ApplicationProcessorTakeoverCoordinator) == 8U);
} // namespace Asteria::Platform::Ia64::Interrupt

#endif // ASTERIA_PLATFORM_IA64_INTERRUPT_IA64_APPLICATION_PROCESSOR_TAKEOVER_COORDINATOR_HPP_INCLUDED
