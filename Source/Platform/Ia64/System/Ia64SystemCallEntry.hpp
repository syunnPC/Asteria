// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_SYSTEM_IA64_SYSTEM_CALL_ENTRY_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_SYSTEM_IA64_SYSTEM_CALL_ENTRY_HPP_INCLUDED

#include "Foundation/Compiler.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Memory/IUserMemoryAccess.hpp"
#include "Kernel/System/SystemCallScratch.hpp"
#include "Platform/Ia64/Scheduler/Ia64ProcessorRunLoop.hpp"
#include "Platform/Ia64/System/Ia64SystemCallRegisterCodec.hpp"

namespace Asteria::Kernel::System
{
class IObjectManagement;
class WaitSetSystemCallDispatcher;
}

namespace Asteria::Kernel::Memory
{
class IPagerResolution;
}

namespace Asteria::Platform::Ia64::System
{
class Ia64SystemCallWaitContinuation;

/// A user thread whose kernel context runs on its own entry storage : its WaitSetWait blocks on
/// that storage.
class IIa64SystemCallThread
{
public:
    /// The running thread's WaitSetWait: dispatches it and, when it suspends, blocks the thread
    /// through ProcessorKernelRunLoop::FinishWaitAndDispatch, returning once the thread runs again
    /// with its entry contexts selected. False only on a kernel invariant failure; a rejected wait
    /// is a response.
    [[nodiscard]] virtual bool DispatchWait(const Kernel::System::SystemCallRequest& request,
                                            Kernel::System::SystemCallResponse& response) noexcept = 0;

    /// The running thread's ThreadExit: a created thread decodes it and, when valid, finishes its
    /// run Exited, which never returns; a boot thread answers it UnknownSystemCall. False only on a
    /// kernel invariant failure.
    [[nodiscard]] virtual bool DispatchExit(const Kernel::System::SystemCallRequest& request,
                                            Kernel::System::SystemCallResponse& response) noexcept = 0;

    /// Runs before the thread's ObjectCreate, ObjectDestroy, MemoryMap or
    /// MemoryUnmap: unbinds exited created threads, so the call's collection
    /// can free their slots. False only on a kernel invariant
    /// failure.
    [[nodiscard]] virtual bool PrepareManagement() noexcept = 0;

protected:
    constexpr IIa64SystemCallThread() noexcept = default;
    ~IIa64SystemCallThread() noexcept = default;
    IIa64SystemCallThread(const IIa64SystemCallThread&) = default;
    IIa64SystemCallThread& operator=(const IIa64SystemCallThread&) = default;
};

constexpr Foundation::UInt64 Ia64SystemCallBreakImmediate{0x10'0000U};
constexpr Foundation::UInt64 Ia64SystemCallFloatingPointDisableMask{0xC'0000U};

constexpr Foundation::UInt64 Ia64SystemCallCurrentProcessorStatusMask{0x0000'0010'080E'E000ULL};
constexpr Foundation::UInt64 Ia64SystemCallCurrentProcessorStatusValue{0x0000'0010'0802'0000ULL};
constexpr Foundation::UInt64 Ia64SystemCallInterruptedProcessorStatusMask{0x0000'5017'0802'A000ULL};
constexpr Foundation::UInt64 Ia64SystemCallInterruptedProcessorStatusValue{0x0000'1013'0802'2000ULL};
constexpr Foundation::UInt64 Ia64SystemCallIfsValidMask{0x8000'0000'0000'0000ULL};
constexpr Foundation::UInt64 Ia64SystemCallInstructionSlotMask{0x0000'0600'0000'0000ULL};
constexpr Foundation::UInt64 Ia64SystemCallInstructionSlotShift{41U};
constexpr Foundation::UInt64 Ia64SystemCallBundleSizeBytes{16U};
constexpr Foundation::UInt64 Ia64SystemCallFrameMarkerMask{0x0000'003F'FFFF'FFFFULL};
constexpr Foundation::UInt64 Ia64SystemCallMaximumStackedRegisters{96U};
constexpr Foundation::UInt64 Ia64SystemCallRequiredOutputRegisters{6U};
/// P64 syscall ABI range that callers must treat as floating-register clobbers.
constexpr Foundation::UInt64 Ia64SystemCallFirstClobberedFloatingRegister{6U};
constexpr Foundation::UInt64 Ia64SystemCallLastClobberedFloatingRegister{15U};
/// Refill-stable, non-arithmetic carrier subset used by the native restore tail.
constexpr Foundation::UInt64 Ia64SystemCallFirstRestoreCarrierRegister{6U};
constexpr Foundation::UInt64 Ia64SystemCallLastRestoreCarrierRegister{13U};
/// RSC.mode 11 (eager) and RSC.pl 00 (PL0).
constexpr Foundation::UInt64 Ia64SystemCallKernelRseConfiguration{0x3U};
/// RSC.mode 00 (enforced lazy) and RSC.pl 11 (PL3).
constexpr Foundation::UInt64 Ia64SystemCallUserRseConfiguration{0xCU};
constexpr Foundation::UInt64 Ia64SystemCallStoragePageSizeBytes{4096U};
constexpr Foundation::UInt64 Ia64SystemCallMemoryStackSizeBytes{16U * 1024U};
/// Maximum architected interrupted dirty partition carried onto the kernel RBS.
constexpr Foundation::UInt64 Ia64SystemCallRseTransitionSizeBytes{16U * 1024U};
constexpr Foundation::UInt64 Ia64SystemCallRseBackingStoreSizeBytes{Ia64SystemCallRseTransitionSizeBytes +
                                                                    (16U * 1024U)};
/// r16-r31 plus one software-spill NaT collection, rounded to 16 bytes.
constexpr Foundation::UInt64 Ia64SystemCallBankedRegisterSaveSizeBytes{144U};
constexpr Foundation::UInt64 MaximumIa64SystemCallProcessorCount{4U};
constexpr Foundation::UInt64 Ia64SystemCallFirstNonKernelRegionAddress{0x2000'0000'0000'0000ULL};
/// Complete immutable root object named by ar.k3; the publication word is at 16.
constexpr Foundation::UInt64 Ia64SystemCallRefillContextSizeBytes{48U};
constexpr Foundation::UInt64 Ia64SystemCallRefillContextPublicationOffsetBytes{16U};
constexpr Foundation::UInt64 Ia64SystemCallEntryContextSizeBytes{64U};
constexpr Foundation::UInt64 Ia64SystemCallBridgeContextSizeBytes{64U};

static_assert((Ia64SystemCallRseTransitionSizeBytes % Ia64SystemCallStoragePageSizeBytes) == 0U);
static_assert((Ia64SystemCallRseBackingStoreSizeBytes % Ia64SystemCallStoragePageSizeBytes) == 0U);
static_assert(Ia64SystemCallBankedRegisterSaveSizeBytes < Ia64SystemCallMemoryStackSizeBytes);
static_assert(Ia64SystemCallRefillContextSizeBytes <= Ia64SystemCallFirstNonKernelRegionAddress);
static_assert(Ia64SystemCallRefillContextPublicationOffsetBytes + sizeof(Foundation::UInt64) <=
              Ia64SystemCallRefillContextSizeBytes);
static_assert(Ia64SystemCallEntryContextSizeBytes <= Ia64SystemCallFirstNonKernelRegionAddress);
static_assert(Ia64SystemCallBridgeContextSizeBytes <= Ia64SystemCallFirstNonKernelRegionAddress);
static_assert(Ia64SystemCallFirstRestoreCarrierRegister >= Ia64SystemCallFirstClobberedFloatingRegister);
static_assert(Ia64SystemCallLastRestoreCarrierRegister <= Ia64SystemCallLastClobberedFloatingRegister);
// Intel SDM Vol. 2 Table 3-2 specifies zero as the interruption state of both
// PSR.dfl and PSR.dfh. The restore carriers are accessed only before rfi puts
// the interrupted (possibly disabled) state back into PSR.
static_assert((Ia64SystemCallCurrentProcessorStatusMask & Ia64SystemCallFloatingPointDisableMask) ==
              Ia64SystemCallFloatingPointDisableMask);
static_assert((Ia64SystemCallCurrentProcessorStatusValue & Ia64SystemCallFloatingPointDisableMask) == 0U);

enum class Ia64SystemCallEntryFrameState : Foundation::UInt64
{
    Inactive = 0U,
    Active = 1U,
};

/// Immutable per-processor inputs reached through offset 16 of the ar.k3
/// context. The stack and RSE ranges are retained kernel storage dedicated to
/// this transport and may not be the interrupted worker's storage. The first
/// Ia64SystemCallBankedRegisterSaveSizeBytes of the memory stack are reserved
/// for low-level bank-one capture; the P64 memory stack grows down from its
/// end. The RSE range reserves its first transition-size bytes for the dirty
/// interrupted partition carried by the architected backing-store switch.
/// BridgeContextAddress names the owning processor's trusted bridge
/// collaborators and is the sole source of syscall authority.
struct alignas(16U) Ia64SystemCallEntryContext
{
    Foundation::UInt64 FrameAddress;
    Foundation::UInt64 MemoryStackStart;
    Foundation::UInt64 MemoryStackEnd;
    Foundation::UInt64 RseBackingStoreStart;
    Foundation::UInt64 RseBackingStoreEnd;
    Foundation::UInt64 BridgeContextAddress;
    Foundation::UInt64 ProcessorIndex;
    Foundation::UInt64 Reserved;
};

/// Immutable processor-owned collaborators consumed by the native syscall
/// adapter. The owner retains every named object for the publication lifetime.
struct alignas(8U) Ia64SystemCallBridgeContext
{
    Scheduler::Ia64ProcessorRunLoop* RunLoop;
    Kernel::Memory::IUserMemoryAccess* UserMemory;
    Kernel::System::SystemCallScratch* Scratch;
    /// Optional processor-owned one-slot continuation. This and
    /// WaitSetDispatcher are either both null or both non-null.
    Ia64SystemCallWaitContinuation* WaitContinuation;
    /// Optional processor-owned portable WaitSet dispatcher. When both wait
    /// collaborators are null, syscall 8 completes with InvalidConfiguration.
    Kernel::System::WaitSetSystemCallDispatcher* WaitSetDispatcher;
    /// Optional thread that blocks its own WaitSetWait. Exclusive with the
    /// continuation pair above.
    IIa64SystemCallThread* Thread;
    /// The pager resolution FaultResolve may use, or null.
    Kernel::Memory::IPagerResolution* PagerResolution;
    /// The object management ObjectCreate and ObjectDestroy reach, or null.
    Kernel::System::IObjectManagement* ObjectManagement;
};

[[nodiscard]] constexpr bool IsIa64SystemCallRefillContextAddressValid(Foundation::UInt64 address) noexcept;

[[nodiscard]] constexpr bool IsIa64SystemCallEntryContextAddressValid(Foundation::UInt64 address) noexcept;

[[nodiscard]] constexpr bool IsIa64SystemCallBridgeContextAddressValid(Foundation::UInt64 address) noexcept;

/// The resumable WaitSet continuation and dispatcher form one optional unit.
/// An asymmetric pair is a malformed trusted bridge configuration.
[[nodiscard]] constexpr bool
IsIa64SystemCallWaitPairValid(const Ia64SystemCallWaitContinuation* waitContinuation,
                              const Kernel::System::WaitSetSystemCallDispatcher* waitSetDispatcher) noexcept;

/// Complete integer/application/RSE interruption record for the native syscall
/// break transport. The syscall ABI treats caller-saved floating registers
/// f6-f15 as clobbered. The native restore tail uses non-arithmetic bit transfers
/// through f6-f13 as refill-stable carriers after architectural interruption
/// entry has cleared PSR.dfl and PSR.dfh; P64 requires the bridge to preserve
/// f2-f5 and f16-f31. PreviousFunctionState is the user's pre-entry PFS;
/// CapturedUserInterruptionFunctionState is the interrupted CFM captured
/// directly by cover before the backing-store switch.
struct alignas(16U) Ia64SystemCallEntryFrame
{
    Ia64SystemCallEntryFrameState State;
    Foundation::UInt64 CurrentProcessorStatus;
    Foundation::UInt64 InterruptedInstructionPointer;
    Foundation::UInt64 InterruptedProcessorStatus;
    Foundation::UInt64 InterruptionStatus;
    Foundation::UInt64 BreakImmediate;
    Foundation::UInt64 HardwareInterruptionFunctionState;
    Foundation::UInt64 CapturedUserInterruptionFunctionState;
    Foundation::UInt64 PreviousFunctionState;
    Foundation::UInt64 UserRseConfiguration;
    Foundation::UInt64 UserRseBackingStoreStorePointer;
    Foundation::UInt64 KernelRseLoadConfiguration;
    Foundation::UInt64 UserRseNatCollection;
    Foundation::UInt64 OriginalUserNatCollection;
    Foundation::UInt64 SpillNatCollection;
    Foundation::UInt64 FloatingPointStatus;
    Foundation::UInt64 LoopCounter;
    Foundation::UInt64 CompareExchangeValue;
    Foundation::UInt64 EpilogCounter;
    Foundation::UInt64 Predicates;
    Foundation::UInt64 B0;
    Foundation::UInt64 B1;
    Foundation::UInt64 B2;
    Foundation::UInt64 B3;
    Foundation::UInt64 B4;
    Foundation::UInt64 B5;
    Foundation::UInt64 B6;
    Foundation::UInt64 B7;
    Foundation::UInt64 SpillR1;
    Foundation::UInt64 SpillR2;
    Foundation::UInt64 SpillR3;
    Foundation::UInt64 SpillR4;
    Foundation::UInt64 SpillR5;
    Foundation::UInt64 SpillR6;
    Foundation::UInt64 SpillR7;
    Foundation::UInt64 SpillR11;
    Foundation::UInt64 SpillR12;
    Foundation::UInt64 SpillR13;
    Foundation::UInt64 SpillR14;
    Foundation::UInt64 SpillR15;
    Ia64SystemCallRegisterImage Registers;
    Foundation::UInt64 Reserved;
};

enum class Ia64SystemCallContextValidationError : Foundation::UInt8
{
    None,
    InvalidReserved,
    InvalidProcessorIndex,
    InvalidFrame,
    InvalidMemoryStack,
    InvalidRseBackingStore,
    StorageRangesOverlap,
    FrameAliasesStorage,
    InvalidBridgeContextAddress,
};

enum class Ia64SystemCallFrameConfigurationError : Foundation::UInt8
{
    None,
    InvalidValidBit,
    ReservedBitsSet,
    InvalidFrameSize,
    InvalidLocalSize,
    RotatingFrame,
    InsufficientOutputRegisters,
};

enum class Ia64SystemCallInterruptedStateValidationError : Foundation::UInt8
{
    None,
    IncompatibleCurrentState,
    IncompatibleInterruptedState,
    WrongBreakImmediate,
    HardwareIfsIsValid,
    InvalidRseConfiguration,
    InvalidFrameConfiguration,
};

enum class Ia64SystemCallResumeAdvanceError : Foundation::UInt8
{
    None,
    InvalidBundleAddress,
    InvalidInstructionSlot,
    AddressOverflow,
};

struct Ia64SystemCallResumeTarget
{
    Foundation::UInt64 InstructionPointer;
    Foundation::UInt64 ProcessorStatus;
};

enum class Ia64SystemCallFrameTransitionError : Foundation::UInt8
{
    None,
    AlreadyActive,
    InvalidState,
    NotActive,
};

/// Nonzero values are fatal to the native entry and never become user-visible
/// SystemCallError values.
enum class Ia64SystemCallBridgeStatus : Foundation::UInt64
{
    None = 0U,
    InvalidFrameState = 1U,
    InvalidInterruptedState = 2U,
    InvalidResumeTarget = 3U,
    InvalidNatMetadata = 4U,
    NativeTransportUnavailable = 5U,
    IngressRejected = 6U,
    InvalidBridgeContext = 7U,
    WaitContinuationRejected = 8U,
    ThreadWaitRejected = 9U,
    ThreadExitRejected = 10U,
    ManagementPreparationRejected = 11U,
};

/// The fail-stop diagnostic of a PL3 break whose immediate is not
/// Ia64SystemCallBreakImmediate: "ASTERIA:IA64-USER-BREAK:FAIL:IMMEDIATE=0x"
/// followed by 16 lowercase hex digits of cr.iim, ":IIP=0x" and 16 of cr.iip,
/// then CR LF and a terminating NUL.
using Ia64UnknownBreakDiagnostic = char[83];

/// Formats the unknown-break diagnostic line into `line`.
constexpr void FormatIa64UnknownBreakDiagnostic(Foundation::UInt64 immediate, Foundation::UInt64 instructionPointer,
                                                Ia64UnknownBreakDiagnostic& line) noexcept;

[[nodiscard]] constexpr Ia64SystemCallContextValidationError
ValidateIa64SystemCallTrustedContext(const Ia64SystemCallEntryContext& context) noexcept;

[[nodiscard]] constexpr Ia64SystemCallFrameConfigurationError
ValidateIa64SystemCallFrameConfiguration(Foundation::UInt64 interruptionFunctionState) noexcept;

[[nodiscard]] constexpr Ia64SystemCallInterruptedStateValidationError
ValidateIa64SystemCallInterruptedState(const Ia64SystemCallEntryFrame& frame) noexcept;

[[nodiscard]] constexpr Ia64SystemCallResumeAdvanceError
AdvanceIa64SystemCallResumeTarget(Foundation::UInt64 instructionPointer, Foundation::UInt64 processorStatus,
                                  Ia64SystemCallResumeTarget& target) noexcept;

[[nodiscard]] constexpr Ia64SystemCallFrameTransitionError
ActivateIa64SystemCallEntryFrame(Ia64SystemCallEntryFrame& frame) noexcept;

[[nodiscard]] constexpr Ia64SystemCallFrameTransitionError
DeactivateIa64SystemCallEntryFrame(Ia64SystemCallEntryFrame& frame) noexcept;

/// Validates and dispatches one captured syscall through the owning processor
/// run loop. Success preserves Active for the native restore path. Fatal
/// failure leaves response registers and resume coordinates uncommitted.
[[nodiscard]] Ia64SystemCallBridgeStatus
DispatchIa64SystemCallBridge(Ia64SystemCallEntryFrame& frame, Scheduler::Ia64ProcessorRunLoop& runLoop,
                             Kernel::Memory::IUserMemoryAccess& userMemory,
                             Kernel::System::SystemCallScratch& scratch) noexcept;

/// Compatibility overload for callers without a portable wait dispatcher.
/// Null preserves completed-only behavior. A non-null continuation is an
/// incomplete wait pair and returns InvalidBridgeContext without mutation.
[[nodiscard]] Ia64SystemCallBridgeStatus
DispatchIa64SystemCallBridge(Ia64SystemCallEntryFrame& frame, Scheduler::Ia64ProcessorRunLoop& runLoop,
                             Kernel::Memory::IUserMemoryAccess& userMemory, Kernel::System::SystemCallScratch& scratch,
                             Ia64SystemCallWaitContinuation* waitContinuation) noexcept;

/// Fully configured resumable-ingress overload. The two optional wait
/// collaborators form one processor-owned pair. Both null keeps syscall
/// numbers 1-7 on the ordinary ingress path and makes syscall 8 complete with
/// the ordinary InvalidConfiguration ABI response. Exactly one null is a
/// malformed bridge context and fails without committing response or resume
/// coordinates.
[[nodiscard]] Ia64SystemCallBridgeStatus
DispatchIa64SystemCallBridge(Ia64SystemCallEntryFrame& frame, Scheduler::Ia64ProcessorRunLoop& runLoop,
                             Kernel::Memory::IUserMemoryAccess& userMemory, Kernel::System::SystemCallScratch& scratch,
                             Ia64SystemCallWaitContinuation* waitContinuation,
                             Kernel::System::WaitSetSystemCallDispatcher* waitSetDispatcher) noexcept;

/// The thread-owned overload: thread, when non-null, dispatches syscall 8
/// and may block (the continuation pair must then be null); pagerResolution
/// is passed to the ingress for FaultResolve, and objectManagement for
/// ObjectCreate and ObjectDestroy.
[[nodiscard]] Ia64SystemCallBridgeStatus
DispatchIa64SystemCallBridge(Ia64SystemCallEntryFrame& frame, Scheduler::Ia64ProcessorRunLoop& runLoop,
                             Kernel::Memory::IUserMemoryAccess& userMemory, Kernel::System::SystemCallScratch& scratch,
                             Ia64SystemCallWaitContinuation* waitContinuation,
                             Kernel::System::WaitSetSystemCallDispatcher* waitSetDispatcher,
                             IIa64SystemCallThread* thread, Kernel::Memory::IPagerResolution* pagerResolution,
                             Kernel::System::IObjectManagement* objectManagement = nullptr) noexcept;

constexpr void FormatIa64UnknownBreakDiagnostic(const Foundation::UInt64 immediate,
                                                const Foundation::UInt64 instructionPointer,
                                                Ia64UnknownBreakDiagnostic& line) noexcept
{
    constexpr char Prefix[]{"ASTERIA:IA64-USER-BREAK:FAIL:IMMEDIATE=0x"};
    constexpr char Separator[]{":IIP=0x"};
    constexpr char Digits[]{"0123456789abcdef"};
    constexpr Foundation::Size HexDigits{16U};
    static_assert((sizeof(Prefix) - 1U) + HexDigits + (sizeof(Separator) - 1U) + HexDigits + 3U ==
                  sizeof(Ia64UnknownBreakDiagnostic));
    Foundation::Size position{0U};
    for (Foundation::Size index{0U}; (index + 1U) < sizeof(Prefix); ++index)
    {
        line[position++] = Prefix[index];
    }
    for (Foundation::Size digit{0U}; digit < HexDigits; ++digit)
    {
        line[position++] = Digits[(immediate >> ((HexDigits - 1U - digit) * 4U)) & 0xFU];
    }
    for (Foundation::Size index{0U}; (index + 1U) < sizeof(Separator); ++index)
    {
        line[position++] = Separator[index];
    }
    for (Foundation::Size digit{0U}; digit < HexDigits; ++digit)
    {
        line[position++] = Digits[(instructionPointer >> ((HexDigits - 1U - digit) * 4U)) & 0xFU];
    }
    line[position++] = '\r';
    line[position++] = '\n';
    line[position] = '\0';
}

constexpr bool IsIa64SystemCallRefillContextAddressValid(const Foundation::UInt64 address) noexcept
{
    constexpr Foundation::UInt64 maximumAddress{Ia64SystemCallFirstNonKernelRegionAddress -
                                                Ia64SystemCallRefillContextSizeBytes};
    return (address != 0U) && ((address & 0xFU) == 0U) && (address <= maximumAddress);
}

constexpr bool IsIa64SystemCallEntryContextAddressValid(const Foundation::UInt64 address) noexcept
{
    constexpr Foundation::UInt64 maximumAddress{Ia64SystemCallFirstNonKernelRegionAddress -
                                                Ia64SystemCallEntryContextSizeBytes};
    return (address != 0U) && ((address & 0xFU) == 0U) && (address <= maximumAddress);
}

constexpr bool IsIa64SystemCallBridgeContextAddressValid(const Foundation::UInt64 address) noexcept
{
    constexpr Foundation::UInt64 maximumAddress{Ia64SystemCallFirstNonKernelRegionAddress -
                                                Ia64SystemCallBridgeContextSizeBytes};
    return (address != 0U) && ((address & 0x7U) == 0U) && (address <= maximumAddress);
}

constexpr bool
IsIa64SystemCallWaitPairValid(const Ia64SystemCallWaitContinuation* const waitContinuation,
                              const Kernel::System::WaitSetSystemCallDispatcher* const waitSetDispatcher) noexcept
{
    return (waitContinuation == nullptr) == (waitSetDispatcher == nullptr);
}

constexpr Ia64SystemCallContextValidationError
ValidateIa64SystemCallTrustedContext(const Ia64SystemCallEntryContext& context) noexcept
{
    const auto isPageAligned = [](const Foundation::UInt64 value) constexpr noexcept
    {
        return (value & (Ia64SystemCallStoragePageSizeBytes - 1U)) == 0U;
    };
    const auto rangesOverlap = [](const Foundation::UInt64 firstStart, const Foundation::UInt64 firstEnd,
                                  const Foundation::UInt64 secondStart,
                                  const Foundation::UInt64 secondEnd) constexpr noexcept
    {
        return (firstStart < secondEnd) && (secondStart < firstEnd);
    };

    if (context.Reserved != 0U)
    {
        return Ia64SystemCallContextValidationError::InvalidReserved;
    }
    if (context.ProcessorIndex >= MaximumIa64SystemCallProcessorCount)
    {
        return Ia64SystemCallContextValidationError::InvalidProcessorIndex;
    }
    if ((context.FrameAddress == 0U) || ((context.FrameAddress & 0xFU) != 0U) ||
        (context.FrameAddress > (Ia64SystemCallFirstNonKernelRegionAddress - sizeof(Ia64SystemCallEntryFrame))))
    {
        return Ia64SystemCallContextValidationError::InvalidFrame;
    }
    if ((context.MemoryStackStart == 0U) || !isPageAligned(context.MemoryStackStart) ||
        !isPageAligned(context.MemoryStackEnd) || (context.MemoryStackEnd <= context.MemoryStackStart) ||
        ((context.MemoryStackEnd - context.MemoryStackStart) != Ia64SystemCallMemoryStackSizeBytes) ||
        (context.MemoryStackEnd > Ia64SystemCallFirstNonKernelRegionAddress))
    {
        return Ia64SystemCallContextValidationError::InvalidMemoryStack;
    }
    if ((context.RseBackingStoreStart == 0U) || !isPageAligned(context.RseBackingStoreStart) ||
        !isPageAligned(context.RseBackingStoreEnd) || (context.RseBackingStoreEnd <= context.RseBackingStoreStart) ||
        ((context.RseBackingStoreEnd - context.RseBackingStoreStart) != Ia64SystemCallRseBackingStoreSizeBytes) ||
        (context.RseBackingStoreEnd > Ia64SystemCallFirstNonKernelRegionAddress))
    {
        return Ia64SystemCallContextValidationError::InvalidRseBackingStore;
    }
    if (rangesOverlap(context.MemoryStackStart, context.MemoryStackEnd, context.RseBackingStoreStart,
                      context.RseBackingStoreEnd))
    {
        return Ia64SystemCallContextValidationError::StorageRangesOverlap;
    }

    const Foundation::UInt64 frameEnd = context.FrameAddress + sizeof(Ia64SystemCallEntryFrame);
    if (rangesOverlap(context.FrameAddress, frameEnd, context.MemoryStackStart, context.MemoryStackEnd) ||
        rangesOverlap(context.FrameAddress, frameEnd, context.RseBackingStoreStart, context.RseBackingStoreEnd))
    {
        return Ia64SystemCallContextValidationError::FrameAliasesStorage;
    }
    if (!IsIa64SystemCallBridgeContextAddressValid(context.BridgeContextAddress))
    {
        return Ia64SystemCallContextValidationError::InvalidBridgeContextAddress;
    }
    return Ia64SystemCallContextValidationError::None;
}

constexpr Ia64SystemCallFrameConfigurationError
ValidateIa64SystemCallFrameConfiguration(const Foundation::UInt64 interruptionFunctionState) noexcept
{
    constexpr Foundation::UInt64 reservedIfsMask{~(Ia64SystemCallIfsValidMask | Ia64SystemCallFrameMarkerMask)};
    if ((interruptionFunctionState & Ia64SystemCallIfsValidMask) == 0U)
    {
        return Ia64SystemCallFrameConfigurationError::InvalidValidBit;
    }
    if ((interruptionFunctionState & reservedIfsMask) != 0U)
    {
        return Ia64SystemCallFrameConfigurationError::ReservedBitsSet;
    }

    const Foundation::UInt64 frameSize = interruptionFunctionState & 0x7FU;
    const Foundation::UInt64 localSize = (interruptionFunctionState >> 7U) & 0x7FU;
    const Foundation::UInt64 rotatingState = (interruptionFunctionState >> 14U) & 0x00FF'FFFFU;
    if (frameSize > Ia64SystemCallMaximumStackedRegisters)
    {
        return Ia64SystemCallFrameConfigurationError::InvalidFrameSize;
    }
    if (localSize != 0U)
    {
        return Ia64SystemCallFrameConfigurationError::InvalidLocalSize;
    }
    if (rotatingState != 0U)
    {
        return Ia64SystemCallFrameConfigurationError::RotatingFrame;
    }
    if ((frameSize - localSize) < Ia64SystemCallRequiredOutputRegisters)
    {
        return Ia64SystemCallFrameConfigurationError::InsufficientOutputRegisters;
    }
    return Ia64SystemCallFrameConfigurationError::None;
}

constexpr Ia64SystemCallInterruptedStateValidationError
ValidateIa64SystemCallInterruptedState(const Ia64SystemCallEntryFrame& frame) noexcept
{
    if ((frame.CurrentProcessorStatus & Ia64SystemCallCurrentProcessorStatusMask) !=
        Ia64SystemCallCurrentProcessorStatusValue)
    {
        return Ia64SystemCallInterruptedStateValidationError::IncompatibleCurrentState;
    }
    if ((frame.InterruptedProcessorStatus & Ia64SystemCallInterruptedProcessorStatusMask) !=
        Ia64SystemCallInterruptedProcessorStatusValue)
    {
        return Ia64SystemCallInterruptedStateValidationError::IncompatibleInterruptedState;
    }
    if (frame.BreakImmediate != Ia64SystemCallBreakImmediate)
    {
        return Ia64SystemCallInterruptedStateValidationError::WrongBreakImmediate;
    }
    if ((frame.HardwareInterruptionFunctionState & Ia64SystemCallIfsValidMask) != 0U)
    {
        return Ia64SystemCallInterruptedStateValidationError::HardwareIfsIsValid;
    }
    if (frame.UserRseConfiguration != Ia64SystemCallUserRseConfiguration)
    {
        return Ia64SystemCallInterruptedStateValidationError::InvalidRseConfiguration;
    }
    if (ValidateIa64SystemCallFrameConfiguration(frame.CapturedUserInterruptionFunctionState) !=
        Ia64SystemCallFrameConfigurationError::None)
    {
        return Ia64SystemCallInterruptedStateValidationError::InvalidFrameConfiguration;
    }
    return Ia64SystemCallInterruptedStateValidationError::None;
}

constexpr Ia64SystemCallResumeAdvanceError
AdvanceIa64SystemCallResumeTarget(const Foundation::UInt64 instructionPointer, const Foundation::UInt64 processorStatus,
                                  Ia64SystemCallResumeTarget& target) noexcept
{
    if ((instructionPointer & (Ia64SystemCallBundleSizeBytes - 1U)) != 0U)
    {
        return Ia64SystemCallResumeAdvanceError::InvalidBundleAddress;
    }

    const Foundation::UInt64 instructionSlot =
        (processorStatus & Ia64SystemCallInstructionSlotMask) >> Ia64SystemCallInstructionSlotShift;
    Foundation::UInt64 nextInstructionPointer{instructionPointer};
    Foundation::UInt64 nextProcessorStatus{processorStatus & ~Ia64SystemCallInstructionSlotMask};
    if (instructionSlot < 2U)
    {
        nextProcessorStatus |= (instructionSlot + 1U) << Ia64SystemCallInstructionSlotShift;
    }
    else if (instructionSlot == 2U)
    {
        constexpr Foundation::UInt64 maximumUInt64{~Foundation::UInt64{0U}};
        if (instructionPointer > (maximumUInt64 - Ia64SystemCallBundleSizeBytes))
        {
            return Ia64SystemCallResumeAdvanceError::AddressOverflow;
        }
        nextInstructionPointer = instructionPointer + Ia64SystemCallBundleSizeBytes;
    }
    else
    {
        return Ia64SystemCallResumeAdvanceError::InvalidInstructionSlot;
    }

    target = Ia64SystemCallResumeTarget{
        .InstructionPointer = nextInstructionPointer,
        .ProcessorStatus = nextProcessorStatus,
    };
    return Ia64SystemCallResumeAdvanceError::None;
}

constexpr Ia64SystemCallFrameTransitionError ActivateIa64SystemCallEntryFrame(Ia64SystemCallEntryFrame& frame) noexcept
{
    if (frame.State == Ia64SystemCallEntryFrameState::Active)
    {
        return Ia64SystemCallFrameTransitionError::AlreadyActive;
    }
    if (frame.State != Ia64SystemCallEntryFrameState::Inactive)
    {
        return Ia64SystemCallFrameTransitionError::InvalidState;
    }
    frame.State = Ia64SystemCallEntryFrameState::Active;
    return Ia64SystemCallFrameTransitionError::None;
}

constexpr Ia64SystemCallFrameTransitionError
DeactivateIa64SystemCallEntryFrame(Ia64SystemCallEntryFrame& frame) noexcept
{
    if (frame.State == Ia64SystemCallEntryFrameState::Inactive)
    {
        return Ia64SystemCallFrameTransitionError::NotActive;
    }
    if (frame.State != Ia64SystemCallEntryFrameState::Active)
    {
        return Ia64SystemCallFrameTransitionError::InvalidState;
    }
    frame.State = Ia64SystemCallEntryFrameState::Inactive;
    return Ia64SystemCallFrameTransitionError::None;
}

static_assert(sizeof(Ia64SystemCallEntryFrameState) == 8U);
static_assert(sizeof(Ia64SystemCallEntryContext) == Ia64SystemCallEntryContextSizeBytes);
static_assert(alignof(Ia64SystemCallEntryContext) == 16U);
static_assert(__is_aggregate(Ia64SystemCallEntryContext));
static_assert(__is_standard_layout(Ia64SystemCallEntryContext));
static_assert(__is_trivially_copyable(Ia64SystemCallEntryContext));
static_assert(__builtin_offsetof(Ia64SystemCallEntryContext, FrameAddress) == 0U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryContext, MemoryStackStart) == 8U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryContext, MemoryStackEnd) == 16U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryContext, RseBackingStoreStart) == 24U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryContext, RseBackingStoreEnd) == 32U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryContext, BridgeContextAddress) == 40U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryContext, ProcessorIndex) == 48U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryContext, Reserved) == 56U);

static_assert(sizeof(Ia64SystemCallBridgeContext) == Ia64SystemCallBridgeContextSizeBytes);
static_assert(alignof(Ia64SystemCallBridgeContext) == 8U);
static_assert(__is_aggregate(Ia64SystemCallBridgeContext));
static_assert(__is_standard_layout(Ia64SystemCallBridgeContext));
static_assert(__is_trivially_copyable(Ia64SystemCallBridgeContext));
static_assert(__builtin_offsetof(Ia64SystemCallBridgeContext, RunLoop) == 0U);
static_assert(__builtin_offsetof(Ia64SystemCallBridgeContext, UserMemory) == 8U);
static_assert(__builtin_offsetof(Ia64SystemCallBridgeContext, Scratch) == 16U);
static_assert(__builtin_offsetof(Ia64SystemCallBridgeContext, WaitContinuation) == 24U);
static_assert(__builtin_offsetof(Ia64SystemCallBridgeContext, WaitSetDispatcher) == 32U);
static_assert(__builtin_offsetof(Ia64SystemCallBridgeContext, Thread) == 40U);
static_assert(__builtin_offsetof(Ia64SystemCallBridgeContext, PagerResolution) == 48U);
static_assert(__builtin_offsetof(Ia64SystemCallBridgeContext, ObjectManagement) == 56U);

static_assert(sizeof(Ia64SystemCallEntryFrame) == 416U);
static_assert(alignof(Ia64SystemCallEntryFrame) == 16U);
static_assert(__is_aggregate(Ia64SystemCallEntryFrame));
static_assert(__is_standard_layout(Ia64SystemCallEntryFrame));
static_assert(__is_trivially_copyable(Ia64SystemCallEntryFrame));
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, State) == 0U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, CurrentProcessorStatus) == 8U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, InterruptedInstructionPointer) == 16U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, InterruptedProcessorStatus) == 24U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, InterruptionStatus) == 32U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, BreakImmediate) == 40U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, HardwareInterruptionFunctionState) == 48U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, CapturedUserInterruptionFunctionState) == 56U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, PreviousFunctionState) == 64U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, UserRseConfiguration) == 72U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, UserRseBackingStoreStorePointer) == 80U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, KernelRseLoadConfiguration) == 88U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, UserRseNatCollection) == 96U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, OriginalUserNatCollection) == 104U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, SpillNatCollection) == 112U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, FloatingPointStatus) == 120U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, LoopCounter) == 128U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, CompareExchangeValue) == 136U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, EpilogCounter) == 144U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, Predicates) == 152U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, B0) == 160U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, B1) == 168U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, B2) == 176U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, B3) == 184U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, B4) == 192U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, B5) == 200U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, B6) == 208U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, B7) == 216U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, SpillR1) == 224U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, SpillR2) == 232U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, SpillR3) == 240U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, SpillR4) == 248U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, SpillR5) == 256U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, SpillR6) == 264U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, SpillR7) == 272U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, SpillR11) == 280U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, SpillR12) == 288U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, SpillR13) == 296U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, SpillR14) == 304U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, SpillR15) == 312U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, Registers) == 320U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, Reserved) == 408U);
static_assert(sizeof(Ia64SystemCallContextValidationError) == 1U);
static_assert(sizeof(Ia64SystemCallFrameConfigurationError) == 1U);
static_assert(sizeof(Ia64SystemCallInterruptedStateValidationError) == 1U);
static_assert(sizeof(Ia64SystemCallResumeAdvanceError) == 1U);
static_assert(sizeof(Ia64SystemCallResumeTarget) == 16U);
static_assert(alignof(Ia64SystemCallResumeTarget) == 8U);
static_assert(__is_aggregate(Ia64SystemCallResumeTarget));
static_assert(__is_standard_layout(Ia64SystemCallResumeTarget));
static_assert(__is_trivially_copyable(Ia64SystemCallResumeTarget));
static_assert(__builtin_offsetof(Ia64SystemCallResumeTarget, InstructionPointer) == 0U);
static_assert(__builtin_offsetof(Ia64SystemCallResumeTarget, ProcessorStatus) == 8U);
static_assert(sizeof(Ia64SystemCallFrameTransitionError) == 1U);
static_assert(sizeof(Ia64SystemCallBridgeStatus) == 8U);
} // namespace Asteria::Platform::Ia64::System

extern "C" [[noreturn]] void AsteriaIa64SystemCallEntry() noexcept;

/// Called only by the entry's unknown-break leaf, on the admitted kernel stack
/// and RSE with collection on, bank one and the kernel GP: writes the
/// FormatIa64UnknownBreakDiagnostic line and stops this CPU. A PL3 break with another immediate is
/// an unsupported user fault; the ABI defines no response to it.
extern "C" [[noreturn]] ASTERIA_NOINLINE_NOIPA void
AsteriaIa64FailStopUnknownSystemCallBreak(Asteria::Foundation::UInt64 immediate,
                                          Asteria::Foundation::UInt64 instructionPointer) noexcept;

extern "C" ASTERIA_NOINLINE_NOIPA Asteria::Foundation::UInt64
AsteriaIa64SystemCallBridge(Asteria::Platform::Ia64::System::Ia64SystemCallEntryFrame* frame,
                            Asteria::Platform::Ia64::System::Ia64SystemCallBridgeContext* bridgeContext) noexcept;

#endif // ASTERIA_PLATFORM_IA64_SYSTEM_IA64_SYSTEM_CALL_ENTRY_HPP_INCLUDED
