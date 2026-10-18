// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_INTERRUPT_IA64_USER_FAULT_ENTRY_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_INTERRUPT_IA64_USER_FAULT_ENTRY_HPP_INCLUDED

#include "Foundation/Compiler.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Memory/FaultToken.hpp"
#include "Platform/Ia64/Interrupt/IIa64DeviceInterruptDispatch.hpp"
#include "Platform/Ia64/Interrupt/Ia64PageFaultEntryContract.hpp"
#include "Platform/Ia64/System/Ia64SystemCallEntry.hpp"

namespace Asteria::Platform::Ia64::Interrupt
{
/// The immutable selector the alternate data-TLB refill handler passes in
/// bank-zero r16 when it routes a ledger miss to AsteriaIa64UserFaultEntry;
/// it continues the page-fault selectors 0 to 3 of the heavyweight slots.
inline constexpr Foundation::UInt64 Ia64UserFaultAlternateDataTlbSelector{4U};

/// Current-PSR admission of the entry, as observed by mov-from-PSR: dt, rt
/// and it set; ic, i, dfl and dfh clear. Inherited pk is disabled before
/// kernel memory access and restored from IPSR at return.
inline constexpr Foundation::UInt64 Ia64UserFaultCurrentProcessorStatusMask{
    System::Ia64SystemCallCurrentProcessorStatusMask & ~(Foundation::UInt64{1U} << 15U)};
inline constexpr Foundation::UInt64 Ia64UserFaultCurrentProcessorStatusValue{
    System::Ia64SystemCallCurrentProcessorStatusValue};
/// Interrupted-PSR admission: cpl 3, bn, ic, dt, rt and it set; is and vm
/// clear; pk either. psr.i is either: the boot domains run with external
/// interrupts enabled, the synthetic user probe with them masked.
inline constexpr Foundation::UInt64 Ia64UserFaultInterruptedProcessorStatusMask{
    System::Ia64SystemCallInterruptedProcessorStatusMask & ~(Foundation::UInt64{1U} << 15U)};
inline constexpr Foundation::UInt64 Ia64UserFaultInterruptedProcessorStatusValue{
    System::Ia64SystemCallInterruptedProcessorStatusValue};
/// The selector the PL3 interrupt claim (AsteriaIa64UserInterruptClaim) passes in bank-zero r16 to
/// AsteriaIa64UserInterruptEntry, which captures the interrupted state like the user-fault entry.
inline constexpr Foundation::UInt64 Ia64UserInterruptSelector{5U};
/// The External Interrupt vector offset the user interrupt entry records in the frame's
/// VectorOffset; the bridge hands such a frame to IIa64UserFaultHandler::HandleUserInterrupt.
inline constexpr Foundation::UInt64 Ia64UserInterruptVectorOffset{0x3000U};
/// The device vectors the interrupt claim captures: the claim records Ia64UserInterruptVectorOffset
/// with the vector in its low byte, and the bridge dispatches it.
inline constexpr Foundation::UInt64 Ia64UserInterruptFirstDeviceVector{0x40U};
inline constexpr Foundation::UInt64 Ia64UserInterruptDeviceVectorCount{16U};
/// The selector the General Exception, Floating-point Fault and Floating-point Trap vectors pass in
/// bank-zero r16 to AsteriaIa64UserExceptionEntry, which captures a PL3 exception like a fault for
/// the thread's supervisor. Each trampoline also leaves its vector offset, shifted right by
/// Ia64UserExceptionVectorShift, in ar.k4 (Ia64UserExceptionVectorRegister), which PL3 cannot
/// write; the entry records that offset in the frame's VectorOffset. A Floating-point Trap's IIP is
/// the instruction after the trapping one, which completed.
inline constexpr Foundation::UInt64 Ia64UserExceptionSelector{6U};
inline constexpr Foundation::UInt64 Ia64UserGeneralExceptionVectorOffset{0x5400U};
inline constexpr Foundation::UInt64 Ia64UserFloatingPointFaultVectorOffset{0x5C00U};
inline constexpr Foundation::UInt64 Ia64UserFloatingPointTrapVectorOffset{0x5D00U};
inline constexpr Foundation::UInt64 Ia64UserExceptionVectorRegister{4U};
inline constexpr Foundation::UInt64 Ia64UserExceptionVectorShift{8U};

/// These PL3-writable admission violations must reach the supervisor as thread Exceptions.
[[nodiscard]] bool IsIa64UserEntryStateFault(const Ia64PageFaultFrame& frame) noexcept;

/// Native IVA offsets, Intel SDM Vol. 2, table 8-4. Undefined/reserved,
/// nested, VHPT, virtualized and IA-32 vectors stay outside this contract.
enum class Ia64UserFaultVector : Foundation::UInt16
{
    InstructionTlb = 0x0400U,
    DataTlb = 0x0800U,
    AlternateInstructionTlb = 0x0C00U,
    AlternateDataTlb = 0x1000U,
    InstructionKeyMiss = 0x1800U,
    DataKeyMiss = 0x1C00U,
    DirtyBit = 0x2000U,
    InstructionAccessBit = 0x2400U,
    DataAccessBit = 0x2800U,
    Break = 0x2C00U,
    PageNotPresent = 0x5000U,
    KeyPermission = 0x5100U,
    InstructionAccessRights = 0x5200U,
    DataAccessRights = 0x5300U,
    GeneralException = 0x5400U,
    DisabledFloatingPointRegister = 0x5500U,
    NatConsumption = 0x5600U,
    Speculation = 0x5700U,
    Debug = 0x5900U,
    UnalignedReference = 0x5A00U,
    UnsupportedDataReference = 0x5B00U,
    FloatingPointFault = 0x5C00U,
    FloatingPointTrap = 0x5D00U,
    LowerPrivilegeTransfer = 0x5E00U,
    TakenBranch = 0x5F00U,
    SingleStep = 0x6000U,
};

/// Interrupted-PSR admission of the user interrupt entry: the user-fault
/// entry's, with i required set, since only a PL3 thread with external
/// interrupts enabled takes an external interrupt.
inline constexpr Foundation::UInt64 Ia64UserInterruptInterruptedProcessorStatusMask{
    Ia64UserFaultInterruptedProcessorStatusMask | (Foundation::UInt64{1U} << 14U)};
inline constexpr Foundation::UInt64 Ia64UserInterruptInterruptedProcessorStatusValue{
    Ia64UserFaultInterruptedProcessorStatusValue | (Foundation::UInt64{1U} << 14U)};
/// The ar.rsc bits the entries compare with the launch's enforced-lazy PL3
/// configuration: all but loadrs (bits 16-29), which the thread may have
/// set for its own loadrs. A mandatory RSE fault interrupts with that value.
inline constexpr Foundation::UInt64 Ia64UserFaultRseConfigurationMask{~(Foundation::UInt64{0x3FFFU} << 16U)};

/// ISR fields the entries reject: ni. A nested interruption has no
/// independently collected restart tuple. RSE-origin (rs) and
/// incomplete-frame (ir) faults are admitted: cover completes the frame
/// before any alloc, loadrs or BSPSTORE write (Intel SDM Vol. 2, §6.8).
inline constexpr Foundation::UInt64 Ia64UserFaultRejectedInterruptionStatusMask{Foundation::UInt64{1U} << 39U};

/// The user-fault entry context of one user thread. The refill handler routes a PL3 Alternate Data
/// TLB miss that exhausts the active ledger to the user-fault entry, which reads the context
/// selected at offset 40 of the processor's ar.k3 root. The frame is the thread's reusable fault
/// frame; the memory stack and RSE backing store are the thread's entry storage, the same ranges
/// its system-call entry context names (a thread is never in a system call and a fault at once).
/// HandlerAddress names the thread's IIa64UserFaultHandler.
struct alignas(16U) Ia64UserFaultEntryContext
{
    Foundation::UInt64 FrameAddress;
    Foundation::UInt64 MemoryStackStart;
    Foundation::UInt64 MemoryStackEnd;
    Foundation::UInt64 RseBackingStoreStart;
    Foundation::UInt64 RseBackingStoreEnd;
    Foundation::UInt64 HandlerAddress;
    Foundation::UInt64 ProcessorIndex;
    Foundation::UInt64 Reserved;
};

inline constexpr Foundation::UInt64 Ia64UserFaultEntryContextSizeBytes{64U};

enum class Ia64UserFaultContextValidationError : Foundation::UInt8
{
    None,
    InvalidReserved,
    InvalidProcessorIndex,
    InvalidFrame,
    InvalidMemoryStack,
    InvalidRseBackingStore,
    StorageRangesOverlap,
    FrameAliasesStorage,
    InvalidHandlerAddress,
};

/// The trusted shape of one user-fault entry context: a nonzero frame aligned
/// as Ia64PageFaultFrame, the exact 16-KiB memory stack and 32-KiB RSE backing
/// store of the system-call entry storage, page aligned and disjoint, every
/// range and the handler in Region 0, and a zero Reserved word.
[[nodiscard]] constexpr Ia64UserFaultContextValidationError
ValidateIa64UserFaultTrustedContext(const Ia64UserFaultEntryContext& context) noexcept
{
    constexpr Foundation::UInt64 regionLimit{System::Ia64SystemCallFirstNonKernelRegionAddress};
    constexpr Foundation::UInt64 pageMask{System::Ia64SystemCallStoragePageSizeBytes - 1U};
    const auto rangesOverlap = [](const Foundation::UInt64 firstStart, const Foundation::UInt64 firstEnd,
                                  const Foundation::UInt64 secondStart,
                                  const Foundation::UInt64 secondEnd) constexpr noexcept
    {
        return (firstStart < secondEnd) && (secondStart < firstEnd);
    };
    const auto validStorage = [](const Foundation::UInt64 start, const Foundation::UInt64 end,
                                 const Foundation::UInt64 size) constexpr noexcept
    {
        return (start != 0U) && ((start & pageMask) == 0U) && ((end & pageMask) == 0U) && (end > start) &&
               ((end - start) == size) && (end <= regionLimit);
    };

    if (context.Reserved != 0U)
    {
        return Ia64UserFaultContextValidationError::InvalidReserved;
    }
    if (context.ProcessorIndex >= System::MaximumIa64SystemCallProcessorCount)
    {
        return Ia64UserFaultContextValidationError::InvalidProcessorIndex;
    }
    if ((context.FrameAddress == 0U) || ((context.FrameAddress & (alignof(Ia64PageFaultFrame) - 1U)) != 0U) ||
        (context.FrameAddress > (regionLimit - sizeof(Ia64PageFaultFrame))))
    {
        return Ia64UserFaultContextValidationError::InvalidFrame;
    }
    if (!validStorage(context.MemoryStackStart, context.MemoryStackEnd, System::Ia64SystemCallMemoryStackSizeBytes))
    {
        return Ia64UserFaultContextValidationError::InvalidMemoryStack;
    }
    if (!validStorage(context.RseBackingStoreStart, context.RseBackingStoreEnd,
                      System::Ia64SystemCallRseBackingStoreSizeBytes))
    {
        return Ia64UserFaultContextValidationError::InvalidRseBackingStore;
    }
    if (rangesOverlap(context.MemoryStackStart, context.MemoryStackEnd, context.RseBackingStoreStart,
                      context.RseBackingStoreEnd))
    {
        return Ia64UserFaultContextValidationError::StorageRangesOverlap;
    }
    const Foundation::UInt64 frameEnd = context.FrameAddress + sizeof(Ia64PageFaultFrame);
    if (rangesOverlap(context.FrameAddress, frameEnd, context.MemoryStackStart, context.MemoryStackEnd) ||
        rangesOverlap(context.FrameAddress, frameEnd, context.RseBackingStoreStart, context.RseBackingStoreEnd))
    {
        return Ia64UserFaultContextValidationError::FrameAliasesStorage;
    }
    if ((context.HandlerAddress == 0U) || ((context.HandlerAddress & 7U) != 0U) ||
        (context.HandlerAddress >= regionLimit))
    {
        return Ia64UserFaultContextValidationError::InvalidHandlerAddress;
    }
    return Ia64UserFaultContextValidationError::None;
}

/// What the user-fault handler decided for one captured fault.
enum class Ia64UserFaultDisposition : Foundation::UInt8
{
    /// The faulting access may run again: the bridge commits the resume and
    /// the entry returns to the exact faulting instruction slot.
    Resume,
    /// The fault cannot be resolved; the entry parks this processor.
    FailStop,
};

/// The kernel side of one user thread's faults and interrupts, named by the
/// HandlerAddress of its Ia64UserFaultEntryContext.
/// AsteriaIa64UserFaultBridge calls it on the thread's own entry stack and RSE
/// backing store, with PSR.ic set, PSR.i clear, bank one and the kernel GP,
/// after the frame was moved to Retained. The handler may block the thread
/// (switch away through the run loop) and returns once the fault is resolved
/// or found unresolvable, or once the thread runs again after an interrupt.
/// It must not change the frame's State or its resume controls (IIP, IPSR
/// and covered IFS); the bridge rejects either change.
class IIa64UserFaultHandler
{
public:
    [[nodiscard]] virtual Ia64UserFaultDisposition
    HandleUserFault(Ia64PageFaultFrame& frame, const Kernel::Memory::FaultRecord& fault) noexcept = 0;

    /// An external interrupt the thread took at PL3, which
    /// AsteriaIa64UserInterruptClaim already claimed: the local timer vector,
    /// with the timer masked and read back and the vector's EOI written.
    /// Resume returns to the interrupted instruction slot.
    [[nodiscard]] virtual Ia64UserFaultDisposition HandleUserInterrupt(Ia64PageFaultFrame& frame) noexcept = 0;

protected:
    constexpr IIa64UserFaultHandler() noexcept = default;
    ~IIa64UserFaultHandler() noexcept = default;
};

/// Nonzero bridge results make the entry fail-stop.
enum class Ia64UserFaultBridgeStatus : Foundation::UInt64
{
    None = 0U,
    InvalidFrame = 1U,
    InvalidHandler = 2U,
    InvalidFrameState = 3U,
    DecodeRejected = 4U,
    HandlerFailStop = 5U,
    InvalidDisposition = 6U,
    FrameStateChanged = 7U,
    ResumeTargetChanged = 8U,
};

/// Decodes the fault or trap a user entry captured into frame: the vector must be one of
/// Ia64UserFaultVector, the post-capture ISR must equal the pre-cover one and hold only native
/// fields, the interrupted PSR must be a native PL3 one, a fault's ISR.ei must equal IPSR.ri and
/// the covered IFS must be a valid frame. The FaultRecord's cause is MissingMapping for translation
/// and page-not-present faults, ProtectionViolation for key, access-bit, dirty-bit and
/// access-rights faults and Exception otherwise; its address is IFA where the vector defines one,
/// else the instruction's bundle; its instruction is IIP for a fault and IIPA for a trap; its
/// syndrome is the vector offset in bits 48-63 over the ISR, with a break's IIM in place of the
/// zero ISR.code. The resume target is the unchanged IIP, IPSR and covered IFS. Failure leaves
/// output unchanged.
[[nodiscard]] Ia64PageFaultEntryContractError DecodeIa64UserFault(const Ia64PageFaultFrame& frame,
                                                                  Ia64PageFaultEntryOutput& output) noexcept;

/// The bridge's semantics, callable on the host. frame must be Capturing (as the entry leaves it)
/// and is moved to Retained. A frame whose VectorOffset is Ia64UserInterruptVectorOffset goes to
/// the handler's HandleUserInterrupt; one whose VectorOffset carries a device vector in its low
/// byte goes to the device dispatch and resumes the thread; any other is decoded as a fault into
/// CapturedEntry and handed to HandleUserFault. Resume moves the frame to ResumeCommitted and
/// returns None; every failure returns its status and leaves the frame Capturing (invalid input) or
/// Retained.
[[nodiscard]] Ia64UserFaultBridgeStatus RunIa64UserFaultBridge(Ia64PageFaultFrame* frame,
                                                               IIa64UserFaultHandler* handler) noexcept;

/// Gives the bridge where device vectors go; once, before a device line is
/// unmasked. Until then a device vector fails the bridge.
[[nodiscard]] bool ConfigureIa64UserDeviceInterrupts(IIa64DeviceInterruptDispatch& dispatch) noexcept;

static_assert(sizeof(Ia64UserFaultEntryContext) == Ia64UserFaultEntryContextSizeBytes);
static_assert(alignof(Ia64UserFaultEntryContext) == 16U);
static_assert(__is_aggregate(Ia64UserFaultEntryContext));
static_assert(__is_standard_layout(Ia64UserFaultEntryContext));
static_assert(__is_trivially_copyable(Ia64UserFaultEntryContext));
static_assert(__builtin_offsetof(Ia64UserFaultEntryContext, FrameAddress) == 0U);
static_assert(__builtin_offsetof(Ia64UserFaultEntryContext, MemoryStackStart) == 8U);
static_assert(__builtin_offsetof(Ia64UserFaultEntryContext, MemoryStackEnd) == 16U);
static_assert(__builtin_offsetof(Ia64UserFaultEntryContext, RseBackingStoreStart) == 24U);
static_assert(__builtin_offsetof(Ia64UserFaultEntryContext, RseBackingStoreEnd) == 32U);
static_assert(__builtin_offsetof(Ia64UserFaultEntryContext, HandlerAddress) == 40U);
static_assert(__builtin_offsetof(Ia64UserFaultEntryContext, ProcessorIndex) == 48U);
static_assert(__builtin_offsetof(Ia64UserFaultEntryContext, Reserved) == 56U);
static_assert(sizeof(Ia64UserFaultDisposition) == 1U);
static_assert(sizeof(Ia64UserFaultBridgeStatus) == 8U);
static_assert(Ia64UserFaultAlternateDataTlbSelector == 4U);
static_assert(Ia64UserInterruptSelector == 5U);
static_assert((Ia64UserInterruptInterruptedProcessorStatusValue & ~Ia64UserInterruptInterruptedProcessorStatusMask) ==
              0U);
static_assert((Ia64UserFaultInterruptedProcessorStatusValue & ~Ia64UserFaultInterruptedProcessorStatusMask) == 0U);
static_assert((Ia64UserFaultCurrentProcessorStatusValue & ~Ia64UserFaultCurrentProcessorStatusMask) == 0U);
} // Namespace Asteria::Platform::Ia64::Interrupt

/// Raw bank-zero target of the alternate data-TLB refill handler's ledger
/// miss escape. r16
/// carries Ia64UserFaultAlternateDataTlbSelector and PR the interrupted
/// predicates. This is not a C++ ABI entry: it captures the complete PL3
/// state onto the selected thread's frame and entry storage, calls
/// AsteriaIa64UserFaultBridge and returns to the faulting instruction with
/// rfi, or parks the processor.
extern "C" [[noreturn]] void AsteriaIa64UserFaultEntry() noexcept;

/// Raw bank-zero continuation of the external-interrupt dispatcher for an
/// interrupt taken at PL3. It reads cr.ivr: the spurious vector returns with
/// rfi; a timer interrupt whose deadline (cr.itm) is not due yet, pended
/// before the deadline was reprogrammed, is acknowledged with EOI and
/// returns; a due one masks the timer with readback, writes EOI and enters
/// AsteriaIa64UserInterruptEntry with Ia64UserInterruptSelector in r16. Any
/// other vector parks the processor.
extern "C" [[noreturn]] void AsteriaIa64UserInterruptClaim() noexcept;

/// The user-fault entry's admission, capture, bridge call and restore for a
/// claimed PL3 interrupt (r16 Ia64UserInterruptSelector); it records
/// Ia64UserInterruptVectorOffset and returns to the interrupted slot.
extern "C" [[noreturn]] void AsteriaIa64UserInterruptEntry() noexcept;

/// The user-fault entry's admission, capture, bridge call and restore for a
/// General Exception or Floating-point Fault or Trap taken at PL3 (r16
/// Ia64UserExceptionSelector, from the vectors' trampolines); it records the
/// vector offset its trampoline left in ar.k4. A PL0 occurrence fails its
/// admission and parks the processor.
extern "C" [[noreturn]] void AsteriaIa64UserExceptionEntry() noexcept;

/// Stackless break demultiplexer. Only the ABI immediate enters the system
/// call entry; every other immediate retains the complete interruption tuple
/// and goes to the user exception entry, before any system-call admission.
extern "C" [[noreturn]] void AsteriaIa64UserBreakEntry() noexcept;

/// Called only by AsteriaIa64UserFaultEntry, AsteriaIa64UserExceptionEntry and
/// AsteriaIa64UserInterruptEntry,
/// on the thread's entry stack and RSE backing store with PSR.ic set, PSR.i
/// clear, bank one and the kernel GP: RunIa64UserFaultBridge. handler is the
/// context's HandlerAddress, the IIa64UserFaultHandler subobject the runtime
/// admitted, passed opaquely so the C boundary carries no virtual type. A
/// nonzero result parks the processor.
extern "C" ASTERIA_NOINLINE_NOIPA Asteria::Foundation::UInt64
AsteriaIa64UserFaultBridge(Asteria::Platform::Ia64::Interrupt::Ia64PageFaultFrame* frame, void* handler) noexcept;

#endif // ASTERIA_PLATFORM_IA64_INTERRUPT_IA64_USER_FAULT_ENTRY_HPP_INCLUDED
