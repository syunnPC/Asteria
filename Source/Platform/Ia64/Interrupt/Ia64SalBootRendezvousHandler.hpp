// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_INTERRUPT_IA64_SAL_BOOT_RENDEZVOUS_HANDLER_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_INTERRUPT_IA64_SAL_BOOT_RENDEZVOUS_HANDLER_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Scheduler/SchedulerTypes.hpp"
#include "Platform/Ia64/Firmware/SalSystemTableParser.hpp"
#include "Platform/Ia64/Interrupt/Ia64LocalSapicIpiController.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"

namespace Asteria::Platform::Ia64::Interrupt
{
class Ia64ApplicationProcessorTakeoverCoordinator;

enum class Ia64SalBootRendezvousState : Foundation::UInt8
{
    Uninitialized,
    Configured,
    Armed,
};

enum class Ia64SalBootRendezvousInitializationError : Foundation::UInt8
{
    None,
    AlreadyInitialized,
    IpiControllerNotReady,
    InvalidProcessorCount,
    InvalidProcessorMask,
    InvalidProcessorRecord,
    InvalidBspIdentity,
    DuplicateProcessorIdentity,
    InvalidSalInformation,
};

enum class Ia64SalBootRendezvousArmError : Foundation::UInt8
{
    None,
    NotConfigured,
    AlreadyArmed,
};

enum class Ia64SalBootRendezvousArrivalStatus : Foundation::UInt8
{
    Arrived,
    NotArmed,
    UnknownProcessor,
    BootstrapProcessor,
    Duplicate,
};

enum class Ia64SalBootRendezvousDisposition : Foundation::UInt64
{
    ReturnToSal = 0U,
    TakeOver = 1U,
    FailStop = 2U,
};

enum class Ia64SalBootRendezvousTakeoverState : Foundation::UInt64
{
    Invalid = 0U,
    Valid = 1U,
};

struct Ia64SalBootRendezvousProcessorContext
{
    Foundation::UInt32 AcpiUid;
    Kernel::Scheduler::CpuId Processor;
    Foundation::UInt8 HardwareId;
    Foundation::UInt8 HardwareEid;
    Foundation::UInt8 Reserved;
};

struct Ia64SalBootRendezvousArrivalResult
{
    Kernel::Scheduler::CpuId Processor;
    Ia64SalBootRendezvousArrivalStatus Status;
    Foundation::UInt8 Reserved[6];
};

/// The exact ptr.i operands required before returning through SAL's BR0.
/// An all-zero value is the native bridge's unconfigured fail-stop sentinel.
struct Ia64SalBootRendezvousReturnContext
{
    Foundation::UInt64 TranslationRegisterVirtualAddress;
    Foundation::UInt64 TranslationRegisterEncodedPageSize;
};

/// The exact four-word result returned in r8 through r11 to the raw entry.
/// A TakeOver result names the coordinator's immutable physical-identity
/// record. Every other disposition carries a zero record address.
struct Ia64SalBootRendezvousAction
{
    Foundation::UInt64 TranslationRegisterVirtualAddress;
    Foundation::UInt64 TranslationRegisterEncodedPageSize;
    Foundation::UInt64 Disposition;
    Foundation::UInt64 TakeoverRecordPhysicalAddress;
};

struct Ia64SalBootRendezvousSnapshot
{
    Ia64SalBootRendezvousProcessorContext Processors[Kernel::Scheduler::MaximumProcessorCount];
    Foundation::UInt64 TranslationRegisterVirtualAddress;
    Foundation::UInt64 TranslationRegisterEncodedPageSize;
    Foundation::UInt8 ProcessorCount;
    Foundation::UInt8 ExpectedApMask;
    Foundation::UInt8 ArrivedApMask;
    Ia64SalBootRendezvousState State;
    Foundation::UInt8 Reserved[4];
};

/// Allocation-free shared state behind the physical OS_BOOT_RENDEZ entry. The BSP configures one
/// immutable BSP-first logical map and arms it once before sending wake IPIs. Each AP
/// release-publishes only its own arrival slot. SAL serializes repeated OS_BOOT_RENDEZ calls on one
/// AP, so duplicate detection needs no contended shared read-modify-write operation. The BSP
/// observes the slots with acquire loads through GetSnapshot. This boot-lifetime state has no
/// reset, re-arm, or generation protocol. The architected IPI carries no generation token, and
/// production teardown and CPU hotplug are unsupported. No allocation.
/// Blocking: - Every operation is bounded by the one-to-four processor limit. Arrival publication
/// uses no shared read-modify-write operation.
class Ia64SalBootRendezvousHandler final
{
public:
    constexpr Ia64SalBootRendezvousHandler() noexcept = default;

    ~Ia64SalBootRendezvousHandler() noexcept = default;

    Ia64SalBootRendezvousHandler(const Ia64SalBootRendezvousHandler&) = delete;
    Ia64SalBootRendezvousHandler& operator=(const Ia64SalBootRendezvousHandler&) = delete;
    Ia64SalBootRendezvousHandler(Ia64SalBootRendezvousHandler&&) = delete;
    Ia64SalBootRendezvousHandler& operator=(Ia64SalBootRendezvousHandler&&) = delete;

    /// Retains the exact Local SAPIC map already admitted by the IPI
    /// controller. currentBspCrLid must be the same value passed to that
    /// controller's Initialize call. Failure publishes nothing and is
    /// retryable. Calls require BSP-side external serialization.
    [[nodiscard]] Ia64SalBootRendezvousInitializationError
    Initialize(const Ia64LocalSapicIpiControllerSnapshot& ipiController, Foundation::UInt64 currentBspCrLid,
               const Firmware::SalSystemTableInformation& salInformation) noexcept;

    /// Release-publishes the immutable logical map. This succeeds at most once
    /// during the object's boot lifetime and must precede every wake IPI.
    [[nodiscard]] Ia64SalBootRendezvousArmError Arm() noexcept;

    /// Records one AP arrival by exact cr.lid ID/EID. CPU0, unknown, not-armed, and duplicate calls
    /// leave all arrival slots unchanged. Arrived means only that the AP release-published its slot
    /// inside this physical handler. The required ptr.i, serialization, epilogue, and BR0 return
    /// may still be in flight; no nucleus stack, RSE, PSR, IVA, RR/TR, translation, or scheduler
    /// context is active. Concurrency: - SAL must serialize handler invocations on each individual
    /// AP. Calls on distinct APs touch distinct release-published slots.
    [[nodiscard]] Ia64SalBootRendezvousArrivalResult RecordArrival(Foundation::UInt64 crLid) noexcept;

    /// Records one arrival and selects either the legacy SAL return or the
    /// first successful coordinator claim. An Uninitialized coordinator keeps
    /// arrival-only compatibility. InitializationFailed and every impossible
    /// claim mismatch fail-stop. Exact purge operands are retained in every
    /// result, including failure results. Invalid inherited takeover state is
    /// rejected before Claim, while ordinary SAL-return dispositions ignore
    /// it. After Claim, only immutable coordinator/action corruption can fail
    /// raw validation; such a fail-stop deliberately leaves the slot Claimed
    /// because no owned context exists from which to publish a later phase.
    [[nodiscard]] Ia64SalBootRendezvousAction Dispatch(Foundation::UInt64 crLid,
                                                       Ia64ApplicationProcessorTakeoverCoordinator& coordinator,
                                                       Ia64SalBootRendezvousTakeoverState takeoverState) noexcept;

    /// Captures cr.lid on the current non-migrating processor through the
    /// target adapter. The BSP must capture once and pass that exact value to
    /// both the IPI controller and this object's Initialize operation.
    [[nodiscard]] static Foundation::UInt64 ReadCurrentCrLid() noexcept;

    /// Returns the raw, link-time address of the physical assembly entry. It
    /// is not a C++ function descriptor. The caller may describe it as a
    /// physical address only while the image retains identity addressing.
    [[nodiscard]] static Foundation::UInt64 GetPhysicalEntryAddress() noexcept;

    /// Returns the current image GP for SAL registration. The caller must use
    /// it in the same physical identity environment as GetPhysicalEntryAddress.
    [[nodiscard]] static Foundation::UInt64 GetPhysicalGlobalPointerAddress() noexcept;

    /// Returns validated ITR(0) purge operands after Configured publication.
    /// The physical bridge fail-stops if invoked before these are available.
    [[nodiscard]] Ia64SalBootRendezvousReturnContext GetReturnContext() const noexcept;

    /// Snapshot calls may race AP arrivals. Initialize and Arm remain
    /// BSP-serialized against snapshot calls.
    [[nodiscard]] Ia64SalBootRendezvousSnapshot GetSnapshot() const noexcept;

private:
    Ia64SalBootRendezvousProcessorContext m_processors[Kernel::Scheduler::MaximumProcessorCount]{};
    Synchronization::AtomicUInt64 m_arrivals[Kernel::Scheduler::MaximumProcessorCount]{};
    Synchronization::AtomicUInt64 m_state{};
    Foundation::UInt64 m_translationRegisterVirtualAddress{0U};
    Foundation::UInt64 m_translationRegisterEncodedPageSize{0U};
    Foundation::UInt8 m_processorCount{0U};
    Foundation::UInt8 m_expectedApMask{0U};
    [[maybe_unused]] Foundation::UInt8 m_reserved[6]{};
};

/// Returns the constant-initialized singleton reached by the native entry.
/// Production boot wiring is deliberately outside this component.
[[nodiscard]] Ia64SalBootRendezvousHandler& GetNativeIa64SalBootRendezvousHandler() noexcept;

static_assert(sizeof(Ia64SalBootRendezvousState) == 1U);
static_assert(sizeof(Ia64SalBootRendezvousInitializationError) == 1U);
static_assert(sizeof(Ia64SalBootRendezvousArmError) == 1U);
static_assert(sizeof(Ia64SalBootRendezvousArrivalStatus) == 1U);
static_assert(sizeof(Ia64SalBootRendezvousDisposition) == 8U);
static_assert(sizeof(Ia64SalBootRendezvousTakeoverState) == 8U);
static_assert(sizeof(Ia64SalBootRendezvousProcessorContext) == 8U);
static_assert(alignof(Ia64SalBootRendezvousProcessorContext) == 4U);
static_assert(sizeof(Ia64SalBootRendezvousArrivalResult) == 8U);
static_assert(alignof(Ia64SalBootRendezvousArrivalResult) == 1U);
static_assert(sizeof(Ia64SalBootRendezvousReturnContext) == 16U);
static_assert(alignof(Ia64SalBootRendezvousReturnContext) == 8U);
static_assert(sizeof(Ia64SalBootRendezvousAction) == 32U);
static_assert(alignof(Ia64SalBootRendezvousAction) == 8U);
static_assert(__is_aggregate(Ia64SalBootRendezvousAction));
static_assert(__is_standard_layout(Ia64SalBootRendezvousAction));
static_assert(__is_trivially_copyable(Ia64SalBootRendezvousAction));
static_assert(__builtin_offsetof(Ia64SalBootRendezvousAction, TranslationRegisterVirtualAddress) == 0U);
static_assert(__builtin_offsetof(Ia64SalBootRendezvousAction, TranslationRegisterEncodedPageSize) == 8U);
static_assert(__builtin_offsetof(Ia64SalBootRendezvousAction, Disposition) == 16U);
static_assert(__builtin_offsetof(Ia64SalBootRendezvousAction, TakeoverRecordPhysicalAddress) == 24U);
static_assert(sizeof(Ia64SalBootRendezvousSnapshot) == 56U);
static_assert(alignof(Ia64SalBootRendezvousSnapshot) == 8U);
static_assert(sizeof(Ia64SalBootRendezvousHandler) == 96U);
static_assert(alignof(Ia64SalBootRendezvousHandler) == 8U);
} // Namespace Asteria::Platform::Ia64::Interrupt

#endif // ASTERIA_PLATFORM_IA64_INTERRUPT_IA64_SAL_BOOT_RENDEZVOUS_HANDLER_HPP_INCLUDED
