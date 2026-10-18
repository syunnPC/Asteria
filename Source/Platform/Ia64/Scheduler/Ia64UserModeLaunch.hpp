// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_SCHEDULER_IA64_USER_MODE_LAUNCH_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_SCHEDULER_IA64_USER_MODE_LAUNCH_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Platform/Ia64/Scheduler/Ia64KernelContext.hpp"

namespace Asteria::Platform::Ia64::Scheduler
{
constexpr Foundation::UInt64 Ia64UserModePageSizeBytes{4096U};
constexpr Foundation::UInt64 Ia64UserModeEntryBundleSizeBytes{16U};
constexpr Foundation::UInt64 Ia64UserModeGlobalPointerAlignmentBytes{8U};
constexpr Foundation::UInt64 Ia64UserModeInitialStackScratchBytes{16U};
constexpr Foundation::UInt64 MinimumIa64UserModeStackSizeBytes{16U * 1024U};
constexpr Foundation::UInt64 MaximumIa64UserModeStackSizeBytes{64U * 1024U};
constexpr Foundation::UInt8 MinimumIa64UserModeImplementedAddressBit{50U};
constexpr Foundation::UInt8 MaximumIa64UserModeImplementedAddressBit{60U};

/// Fields that the native leaf can observe with mov-from-PSR before commit.
constexpr Foundation::UInt64 Ia64UserModeCurrentProcessorStatusMask{0x0000'0010'080E'E002ULL};
constexpr Foundation::UInt64 Ia64UserModeCurrentProcessorStatusValue{0x0000'0010'0802'2000ULL};

// Native-only PL3 must not enter unsupported IA-32 state. PSR.di raises a native Disabled
// Instruction Set Transition fault on br.ia (SDM rev. 1.1 Vol. 2 §3.1; Vol. 3 "br").
inline constexpr Foundation::UInt64 Ia64UserModeDisableInstructionSetTransitionBit{0x40'0000U};

/// A translated, collecting PL3/bank-one state with external interruption
/// delivery and both floating-point halves disabled: the synthetic user
/// probe's, whose continuation model admits no PL3 interrupt.
constexpr Foundation::UInt64 Ia64UserModeInitialProcessorStatus{0x0000'1013'084E'2000ULL};
/// The same state with external interruption delivery and both floating-point halves enabled: every
/// other user thread's, whose timer ticks preempt them at PL3 and whose f2-f127 the kernel keeps:
/// It runs under its own fpsr at every entry and hands a PL3 floating-point fault or trap to the
/// thread's supervisor.
constexpr Foundation::UInt64 Ia64UserModePreemptibleProcessorStatus{0x0000'1013'0842'6000ULL};
static_assert(Ia64UserModePreemptibleProcessorStatus ==
              ((Ia64UserModeInitialProcessorStatus | (Foundation::UInt64{1U} << 14U)) &
               ~((Foundation::UInt64{1U} << 18U) | (Foundation::UInt64{1U} << 19U))));
static_assert((Ia64UserModeInitialProcessorStatus & Ia64UserModeDisableInstructionSetTransitionBit) != 0U);
static_assert((Ia64UserModePreemptibleProcessorStatus & Ia64UserModeDisableInstructionSetTransitionBit) != 0U);
constexpr Foundation::UInt64 Ia64UserModeInitialRseConfiguration{0xCU};
constexpr Foundation::UInt64 Ia64UserModeInitialFloatingPointStatus{Ia64KernelDefaultFloatingPointStatus};

struct Ia64UserModeVirtualRange
{
    Foundation::UInt64 VirtualStart;
    Foundation::UInt64 SizeBytes;
};

/// Four IA-64 static-register arguments delivered to user r4 through r7. A
/// fresh launch has cr.ifs equal to zero, so no stacked input register exists
/// yet. The words are opaque to the launch; a word the caller does not use is
/// zero.
struct Ia64UserModeInitialArguments
{
    Foundation::UInt64 Register4;
    Foundation::UInt64 Register5;
    Foundation::UInt64 Register6;
    Foundation::UInt64 Register7;
};

/// The caller states both the PAL-derived address width and the exact fixed
/// processor-state contract. CurrentPrivilegeLevel and
/// CurrentVirtualMachineMode, CurrentInstructionSet, and CurrentRegisterBank
/// are trusted integration preconditions because mov-from-PSR cannot observe
/// those privileged/high fields at the native boundary.
struct Ia64UserModeLaunchRequirements
{
    Foundation::UInt64 CurrentProcessorStatusMask;
    Foundation::UInt64 CurrentProcessorStatusValue;
    Foundation::UInt64 UserProcessorStatus;
    Foundation::UInt8 ImplementedVirtualAddressMostSignificantBit;
    Foundation::UInt8 CurrentPrivilegeLevel;
    Foundation::UInt8 CurrentVirtualMachineMode;
    Foundation::UInt8 CurrentInstructionSet;
    Foundation::UInt8 CurrentRegisterBank;
    Foundation::UInt8 Reserved[3U];
};

enum class Ia64UserModeSupervisorContinuationState : Foundation::UInt64
{
    Fresh = 0U,
    Retained = 1U,
    ResumePending = 2U,
    Active = 3U,
    Faulted = 4U,
};

enum class Ia64UserModeSupervisorContinuationOwner : Foundation::UInt64
{
    None = 0U,
    SystemCallWait = 1U,
    PageFault = 2U,
};

/// Caller-owned supervisor state retained across the initial PL3 launch.
/// Registers is a complete P64 synchronous continuation. The supplied stack
/// and RSE ranges describe the currently active supervisor storage and remain
/// mapped until the privilege-entry continuation resumes or retires it. Owner
/// serializes the system-call and page-fault transports without changing the
/// native launch record's size or field offsets.
struct alignas(16U) Ia64UserModeSupervisorContinuation
{
    Ia64KernelRegisterState Registers;
    Ia64UserModeVirtualRange MemoryStack;
    Ia64UserModeVirtualRange RseBackingStore;
    Ia64UserModeSupervisorContinuationState State;
    Ia64UserModeSupervisorContinuationOwner Owner;
};

struct Ia64UserModeLaunchRequest
{
    Ia64UserModeVirtualRange Code;
    Ia64UserModeVirtualRange MemoryStack;
    Ia64UserModeVirtualRange RseBackingStore;
    Foundation::UInt64 EntryBundleAddress;
    Foundation::UInt64 InitialGlobalPointer;
    Ia64UserModeInitialArguments InitialArguments;
    Ia64UserModeLaunchRequirements Requirements;
    Ia64UserModeSupervisorContinuation* SupervisorContinuation;
    Foundation::UInt64 Reserved;
};

/// Exact fixed-layout input copied into static registers before the native
/// leaf covers and drains the supervisor RSE frame. InitialGlobalPointer is
/// delivered to the P64 global-pointer register r1.
struct Ia64UserModeLaunchPlan
{
    Foundation::UInt64 EntryBundleAddress;
    Foundation::UInt64 UserStackPointer;
    Foundation::UInt64 UserRseBackingStorePointer;
    Foundation::UInt64 UserProcessorStatus;
    Foundation::UInt64 UserRseConfiguration;
    Foundation::UInt64 CurrentProcessorStatusMask;
    Foundation::UInt64 CurrentProcessorStatusValue;
    Foundation::UInt64 SupervisorContinuationAddress;
    Ia64UserModeInitialArguments InitialArguments;
    Foundation::UInt64 InitialGlobalPointer;
};

enum class Ia64UserModeLaunchError : Foundation::UInt32
{
    None,
    InvalidAddressProfile,
    InvalidProcessorStatusRequirements,
    InvalidCodeRange,
    InvalidEntryBundle,
    InvalidGlobalPointer,
    InvalidMemoryStack,
    InvalidRseBackingStore,
    UserRangesOverlap,
    InvalidSupervisorContinuation,
    InvalidSupervisorMemoryStack,
    InvalidSupervisorRseBackingStore,
    SupervisorRangesOverlap,
    SupervisorContinuationAliasesStorage,
    UnsupportedProcessorState,
    InvalidActiveSupervisorState,
    NativeTransitionUnavailable,
    UnexpectedAdapterReturn,
};

enum class Ia64UserModeLaunchDisposition : Foundation::UInt8
{
    SupervisorResumed,
};

using Ia64UserModeLaunchResult = Foundation::Result<Ia64UserModeLaunchDisposition, Ia64UserModeLaunchError>;

/// Allocation-free IA-64 CPL3 launch and supervisor-resume boundary.
class Ia64UserModeLaunch final
{
public:
    /// Validates every caller-supplied byte and returns the exact native plan.
    /// Failure does not change the request, continuation, or processor state.
    [[nodiscard]] static Foundation::Result<Ia64UserModeLaunchPlan, Ia64UserModeLaunchError>
    Validate(const Ia64UserModeLaunchRequest& request) noexcept;

    /// Revalidates the request and enters the target-only native leaf. A
    /// successful IA-64 transition never returns. Hosted execution reports
    /// NativeTransitionUnavailable and cannot report transition success.
    [[nodiscard]] static Ia64UserModeLaunchError Launch(const Ia64UserModeLaunchRequest& request) noexcept;

    /// Enters the same native launch boundary, but recognizes a later raw
    /// restoration of the retained supervisor call chain. The resumed path is
    /// selected exclusively by SupervisorContinuation.State before the
    /// unspecified adapter return register is interpreted.
    [[nodiscard]] static Ia64UserModeLaunchResult LaunchResumable(const Ia64UserModeLaunchRequest& request) noexcept;

private:
    Ia64UserModeLaunch() = delete;
};

static_assert(Ia64UserModeInitialStackScratchBytes == 16U);
static_assert(Ia64UserModeGlobalPointerAlignmentBytes == 8U);
static_assert(Ia64UserModeInitialRseConfiguration == 0xCU);
static_assert(Ia64UserModeInitialFloatingPointStatus == 0x0009'804C'0270'033FULL);
static_assert(sizeof(Ia64UserModeVirtualRange) == 16U);
static_assert(alignof(Ia64UserModeVirtualRange) == 8U);
static_assert(__is_aggregate(Ia64UserModeVirtualRange));
static_assert(__is_standard_layout(Ia64UserModeVirtualRange));
static_assert(__is_trivially_copyable(Ia64UserModeVirtualRange));
static_assert(__builtin_offsetof(Ia64UserModeVirtualRange, VirtualStart) == 0U);
static_assert(__builtin_offsetof(Ia64UserModeVirtualRange, SizeBytes) == 8U);
static_assert(sizeof(Ia64UserModeInitialArguments) == 32U);
static_assert(alignof(Ia64UserModeInitialArguments) == 8U);
static_assert(__is_aggregate(Ia64UserModeInitialArguments));
static_assert(__is_standard_layout(Ia64UserModeInitialArguments));
static_assert(__is_trivially_copyable(Ia64UserModeInitialArguments));
static_assert(__builtin_offsetof(Ia64UserModeInitialArguments, Register4) == 0U);
static_assert(__builtin_offsetof(Ia64UserModeInitialArguments, Register5) == 8U);
static_assert(__builtin_offsetof(Ia64UserModeInitialArguments, Register6) == 16U);
static_assert(__builtin_offsetof(Ia64UserModeInitialArguments, Register7) == 24U);
static_assert(sizeof(Ia64UserModeLaunchRequirements) == 32U);
static_assert(alignof(Ia64UserModeLaunchRequirements) == 8U);
static_assert(__is_aggregate(Ia64UserModeLaunchRequirements));
static_assert(__is_standard_layout(Ia64UserModeLaunchRequirements));
static_assert(__is_trivially_copyable(Ia64UserModeLaunchRequirements));
static_assert(__builtin_offsetof(Ia64UserModeLaunchRequirements, CurrentProcessorStatusMask) == 0U);
static_assert(__builtin_offsetof(Ia64UserModeLaunchRequirements, CurrentProcessorStatusValue) == 8U);
static_assert(__builtin_offsetof(Ia64UserModeLaunchRequirements, UserProcessorStatus) == 16U);
static_assert(__builtin_offsetof(Ia64UserModeLaunchRequirements, ImplementedVirtualAddressMostSignificantBit) == 24U);
static_assert(__builtin_offsetof(Ia64UserModeLaunchRequirements, CurrentPrivilegeLevel) == 25U);
static_assert(__builtin_offsetof(Ia64UserModeLaunchRequirements, CurrentVirtualMachineMode) == 26U);
static_assert(__builtin_offsetof(Ia64UserModeLaunchRequirements, CurrentInstructionSet) == 27U);
static_assert(__builtin_offsetof(Ia64UserModeLaunchRequirements, CurrentRegisterBank) == 28U);
static_assert(__builtin_offsetof(Ia64UserModeLaunchRequirements, Reserved) == 29U);
static_assert(sizeof(Ia64UserModeSupervisorContinuationState) == 8U);
static_assert(sizeof(Ia64UserModeSupervisorContinuationOwner) == 8U);
static_assert(static_cast<Foundation::UInt64>(Ia64UserModeSupervisorContinuationState::Fresh) == 0U);
static_assert(static_cast<Foundation::UInt64>(Ia64UserModeSupervisorContinuationState::Retained) == 1U);
static_assert(static_cast<Foundation::UInt64>(Ia64UserModeSupervisorContinuationOwner::None) == 0U);
static_assert(static_cast<Foundation::UInt64>(Ia64UserModeSupervisorContinuationOwner::SystemCallWait) == 1U);
static_assert(static_cast<Foundation::UInt64>(Ia64UserModeSupervisorContinuationOwner::PageFault) == 2U);
static_assert(sizeof(Ia64UserModeSupervisorContinuation) == 544U);
static_assert(alignof(Ia64UserModeSupervisorContinuation) == 16U);
static_assert(__is_aggregate(Ia64UserModeSupervisorContinuation));
static_assert(__is_standard_layout(Ia64UserModeSupervisorContinuation));
static_assert(__is_trivially_copyable(Ia64UserModeSupervisorContinuation));
static_assert(__builtin_offsetof(Ia64UserModeSupervisorContinuation, Registers) == 0U);
static_assert(__builtin_offsetof(Ia64UserModeSupervisorContinuation, MemoryStack) == 496U);
static_assert(__builtin_offsetof(Ia64UserModeSupervisorContinuation, RseBackingStore) == 512U);
static_assert(__builtin_offsetof(Ia64UserModeSupervisorContinuation, State) == 528U);
static_assert(__builtin_offsetof(Ia64UserModeSupervisorContinuation, Owner) == 536U);
static_assert(sizeof(Ia64UserModeLaunchRequest) == 144U);
static_assert(alignof(Ia64UserModeLaunchRequest) == 8U);
static_assert(__is_aggregate(Ia64UserModeLaunchRequest));
static_assert(__is_standard_layout(Ia64UserModeLaunchRequest));
static_assert(__is_trivially_copyable(Ia64UserModeLaunchRequest));
static_assert(__builtin_offsetof(Ia64UserModeLaunchRequest, Code) == 0U);
static_assert(__builtin_offsetof(Ia64UserModeLaunchRequest, MemoryStack) == 16U);
static_assert(__builtin_offsetof(Ia64UserModeLaunchRequest, RseBackingStore) == 32U);
static_assert(__builtin_offsetof(Ia64UserModeLaunchRequest, EntryBundleAddress) == 48U);
static_assert(__builtin_offsetof(Ia64UserModeLaunchRequest, InitialGlobalPointer) == 56U);
static_assert(__builtin_offsetof(Ia64UserModeLaunchRequest, InitialArguments) == 64U);
static_assert(__builtin_offsetof(Ia64UserModeLaunchRequest, Requirements) == 96U);
static_assert(__builtin_offsetof(Ia64UserModeLaunchRequest, SupervisorContinuation) == 128U);
static_assert(__builtin_offsetof(Ia64UserModeLaunchRequest, Reserved) == 136U);
static_assert(sizeof(Ia64UserModeLaunchPlan) == 104U);
static_assert(alignof(Ia64UserModeLaunchPlan) == 8U);
static_assert(__is_aggregate(Ia64UserModeLaunchPlan));
static_assert(__is_standard_layout(Ia64UserModeLaunchPlan));
static_assert(__is_trivially_copyable(Ia64UserModeLaunchPlan));
static_assert(__builtin_offsetof(Ia64UserModeLaunchPlan, EntryBundleAddress) == 0U);
static_assert(__builtin_offsetof(Ia64UserModeLaunchPlan, UserStackPointer) == 8U);
static_assert(__builtin_offsetof(Ia64UserModeLaunchPlan, UserRseBackingStorePointer) == 16U);
static_assert(__builtin_offsetof(Ia64UserModeLaunchPlan, UserProcessorStatus) == 24U);
static_assert(__builtin_offsetof(Ia64UserModeLaunchPlan, UserRseConfiguration) == 32U);
static_assert(__builtin_offsetof(Ia64UserModeLaunchPlan, CurrentProcessorStatusMask) == 40U);
static_assert(__builtin_offsetof(Ia64UserModeLaunchPlan, CurrentProcessorStatusValue) == 48U);
static_assert(__builtin_offsetof(Ia64UserModeLaunchPlan, SupervisorContinuationAddress) == 56U);
static_assert(__builtin_offsetof(Ia64UserModeLaunchPlan, InitialArguments) == 64U);
static_assert(__builtin_offsetof(Ia64UserModeLaunchPlan, InitialGlobalPointer) == 96U);
static_assert(sizeof(Ia64UserModeLaunchError) == 4U);
static_assert(sizeof(Ia64UserModeLaunchDisposition) == 1U);
static_assert(sizeof(Foundation::Result<Ia64UserModeLaunchPlan, Ia64UserModeLaunchError>) == 112U);
static_assert(alignof(Foundation::Result<Ia64UserModeLaunchPlan, Ia64UserModeLaunchError>) == 8U);
} // Namespace Asteria::Platform::Ia64::Scheduler

#endif // ASTERIA_PLATFORM_IA64_SCHEDULER_IA64_USER_MODE_LAUNCH_HPP_INCLUDED
