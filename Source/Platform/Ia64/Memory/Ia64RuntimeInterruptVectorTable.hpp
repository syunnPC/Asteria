// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_MEMORY_IA64_RUNTIME_INTERRUPT_VECTOR_TABLE_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_MEMORY_IA64_RUNTIME_INTERRUPT_VECTOR_TABLE_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Scheduler/SchedulerTypes.hpp"
#include "Platform/Ia64/Memory/Ia64ActiveRuntimeTranslationBindings.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeTranslationLedger.hpp"

namespace Asteria::Platform::Ia64::System
{
struct Ia64SystemCallEntryContext;
}

namespace Asteria::Platform::Ia64::Interrupt
{
struct Ia64ExternalInterruptEntryContext;
struct Ia64PageFaultEntryContext;
struct Ia64UserFaultEntryContext;
}

namespace Asteria::Platform::Ia64::Memory
{
class InitialAddressSpacePreparation;

constexpr Foundation::Size Ia64RuntimeInterruptVectorTableSizeBytes{32U * 1024U};
/// Architected size of the early lightweight IVT vector slots.
constexpr Foundation::Size Ia64RuntimeInterruptVectorSlotSizeBytes{0x400U};
/// Architected size of the later heavyweight IVT vector slots.
constexpr Foundation::Size Ia64RuntimeHeavyweightInterruptVectorSlotSizeBytes{0x100U};
constexpr Foundation::Size Ia64RuntimeAlternateInstructionTlbOffset{0x0C00U};
constexpr Foundation::Size Ia64RuntimeAlternateDataTlbOffset{0x1000U};
constexpr Foundation::Size Ia64RuntimeBreakOffset{0x2C00U};
constexpr Foundation::Size Ia64RuntimeExternalInterruptOffset{0x3000U};
constexpr Foundation::Size Ia64RuntimePageNotPresentOffset{0x5000U};
constexpr Foundation::Size Ia64RuntimeKeyPermissionOffset{0x5100U};
constexpr Foundation::Size Ia64RuntimeInstructionAccessRightsOffset{0x5200U};
constexpr Foundation::Size Ia64RuntimeDataAccessRightsOffset{0x5300U};
/// The General Exception vector: a PL3 occurrence (an illegal or privileged operation, a reserved
/// register or field) goes to the user exception entry; any other fail-stops there.
constexpr Foundation::Size Ia64RuntimeGeneralExceptionOffset{0x5400U};
/// The Floating-point Fault and Trap vectors: a PL3 occurrence (an unmasked IEEE exception, or one
/// that would need a software assist) goes to the same user exception entry; any other fail-stops
/// there.
constexpr Foundation::Size Ia64RuntimeFloatingPointFaultOffset{0x5C00U};
constexpr Foundation::Size Ia64RuntimeFloatingPointTrapOffset{0x5D00U};
constexpr Foundation::UInt64 Ia64RuntimeTranslationInsertion{0x30U};

struct Ia64RuntimeInterruptVectorTableImage
{
    Foundation::UInt8 Bytes[Ia64RuntimeInterruptVectorTableSizeBytes];
};

/// One bank-0 context selected through ar.k3. The binding address and processor
/// index are immutable after installation. The system-call and external-entry
/// addresses are the currently release-published words; the system-call word
/// and the user-fault word name the running user thread's contexts and are
/// reselected among admitted contexts whenever another thread is about to
/// enter PL3. The page-fault entry address remains zero until the complete
/// one-shot control and retained storage corridor pass publication
/// validation. Each native page-fault slot contains one immutable selector
/// trampoline followed by a fail-stop tail.
/// SlotAddress is deliberately absent: the refill
/// handler derives it from the binding address and processor index so there is
/// one slot-selection authority.
struct alignas(16U) Ia64RuntimeInterruptVectorTableContext
{
    Foundation::UInt64 BindingsWireAddress;
    Foundation::UInt64 ProcessorIndex;
    Foundation::UInt64 SystemCallEntryContextAddress;
    Foundation::UInt64 ExternalInterruptEntryContextAddress;
    Foundation::UInt64 PageFaultEntryContextAddress;
    Foundation::UInt64 UserFaultEntryContextAddress;
    Foundation::UInt64 StopStorageAddress;
};

struct Ia64RuntimeObjectRange
{
    Foundation::UInt64 VirtualStart;
    Foundation::UInt64 SizeBytes;
};

enum class Ia64RuntimeRefillKind : Foundation::UInt8
{
    Instruction,
    Data,
};

/// Pure-model input corresponding to the three architected status resources
/// read by the real low-level handler. CurrentProcessorStatus contains only
/// the fields observable through mov-from-PSR; InterruptedProcessorStatus is
/// the complete captured IPSR. This type is not an interruption entry ABI and
/// grants no installation or insertion authority.
struct Ia64RuntimeRefillProcessorState
{
    Foundation::UInt64 CurrentProcessorStatus;
    Foundation::UInt64 InterruptedProcessorStatus;
    Foundation::UInt64 InterruptionStatus;
};

enum class Ia64RuntimeRefillError : Foundation::UInt8
{
    None,
    IncompatibleCurrentState,
    IncompatibleInterruptedState,
    NestedInterruption,
    IncompleteRegisterFrame,
    RseReference,
    InvalidKind,
    InvalidContext,
    InvalidProcessor,
    InvalidBindingsPointer,
    InvalidBindingSlot,
    NoActiveLedger,
    InvalidLedgerPointer,
    InvalidPublication,
    MappingNotFound,
};

/// What a refill does when it does not stop.
enum class Ia64RuntimeRefillOutcome : Foundation::UInt8
{
    /// The ledger maps the page: insert TranslationWord with
    /// TranslationInsertion at FaultingAddress (the page) and return with rfi.
    Insert,
    /// A PL3 data access that is not an RSE reference and finds no ledger
    /// record: the data handler restores PR and branches to
    /// AsteriaIa64UserFaultEntry with the user-fault selector. Nothing is
    /// inserted; FaultingAddress is the unchanged cr.ifa.
    UserFault,
};

struct Ia64RuntimeRefillAction
{
    Foundation::UInt64 TranslationWord;
    Foundation::UInt64 TranslationInsertion;
    Foundation::UInt64 FaultingAddress;
    Ia64RuntimeRefillKind Kind;
    Ia64RuntimeRefillOutcome Outcome;
    Foundation::UInt8 Reserved[6U];
};

enum class Ia64RuntimeInterruptVectorTableInstallError : Foundation::UInt8
{
    None,
    InvalidProcessor,
    PreparationNotReady,
    InvalidTableAddress,
    InvalidTableLayout,
    InvalidRuntimeObjectRange,
    RuntimeObjectRangeMismatch,
    OwnerOutsideRuntimeObjectRange,
    BindingsOutsideRuntimeObjectRange,
    TableMappingMissing,
    TableInstructionCoverageMissing,
    RuntimeObjectMappingMissing,
    RuntimeObjectDataCoverageMissing,
    BindingAdmissionRejected,
    ContextAlreadyConfigured,
    ProcessorAlreadyInstalled,
    UnsupportedProcessorState,
    StopStorageOutsideRuntimeObjectRange,
};

enum class Ia64RuntimeInterruptVectorTablePublicationError : Foundation::UInt8
{
    None,
    InvalidProcessor,
    ProcessorNotInstalled,
    NullContext,
    MisalignedContext,
    ContextOutsideRegionZero,
    ContextOutsideRuntimeObjectRange,
    ProcessorMismatch,
    ReservedNotZero,
    InvalidSystemCallContext,
    ReferencedStorageOutsideRuntimeObjectRange,
    AlreadyPublished,
    ReferencedStorageOverlapsContext,
    InvalidUserFaultContext,
    AdmissionCapacityExceeded,
    NotAdmitted,
};

enum class Ia64RuntimeExternalInterruptPublicationError : Foundation::UInt8
{
    None,
    InvalidProcessor,
    ProcessorNotInstalled,
    NullContext,
    MisalignedContext,
    ContextOutsideRegionZero,
    ContextOutsideRuntimeObjectRange,
    ProcessorMismatch,
    InvalidVector,
    ReservedNotZero,
    InvalidEvidenceAddress,
    EvidenceOutsideRegionZero,
    EvidenceOutsideRuntimeObjectRange,
    EvidenceNotZero,
    EvidenceOverlapsContext,
    AlreadyPublished,
};

enum class Ia64RuntimePageFaultPublicationError : Foundation::UInt8
{
    None,
    InvalidProcessor,
    ProcessorNotInstalled,
    NullContext,
    MisalignedContext,
    ContextOutsideRegionZero,
    ContextOutsideRuntimeObjectRange,
    ProcessorMismatch,
    InvalidVectorIdentity,
    ReservedNotZero,
    InvalidControlAddress,
    ControlOutsideRuntimeObjectRange,
    ControlNotConfigured,
    InvalidFrameAddress,
    FrameOutsideRuntimeObjectRange,
    FrameNotPristine,
    InvalidCoordinatorMemoryStack,
    CoordinatorMemoryStackOutsideRuntimeObjectRange,
    InvalidCoordinatorRseBackingStore,
    CoordinatorRseBackingStoreOutsideRuntimeObjectRange,
    ReferencedStorageOverlaps,
    AlreadyPublished,
};

struct Ia64RuntimeInterruptVectorTableInstallSummary
{
    Foundation::UInt64 TableAddress;
    Foundation::UInt64 ContextAddress;
    Foundation::UInt32 SizeBytes;
    Foundation::UInt8 ProcessorIndex;
    Foundation::UInt8 InstalledProcessorMask;
    Foundation::UInt16 Reserved;
};

extern "C" const Ia64RuntimeInterruptVectorTableImage AsteriaIa64RuntimeInterruptVectorTableImage;

/// Owns the four ar.k3 contexts and the only admission path for alternate refill readers.
/// Installation occurs after the final SAL handoff because a SAL OS_BOOT_RENDEZ implementation may
/// use kernel registers as scratch. This class installs only ar.k3 and cr.iva. It does not enable
/// PSR.it, PSR.dt, PSR.rt, PSR.ic, or PSR.i and does not establish a runnable root. The caller
/// guarantees CPL0 and PSR.vm=0; mov-from-PSR cannot validate those privileged high fields in the
/// stack-free native leaf.
class Ia64RuntimeInterruptVectorTable final
{
public:
    /// Boot-only: address of the stop owner in the pinned runtime corridor.
    void ConfigureStopStorage(const Foundation::UInt64 address) noexcept
    {
        if (m_installedProcessorMask == 0U || m_stopStorageAddress == address)
        {
            m_stopStorageAddress = address;
        }
    }
    constexpr Ia64RuntimeInterruptVectorTable() noexcept = default;
    ~Ia64RuntimeInterruptVectorTable() noexcept = default;

    Ia64RuntimeInterruptVectorTable(const Ia64RuntimeInterruptVectorTable&) = delete;
    Ia64RuntimeInterruptVectorTable& operator=(const Ia64RuntimeInterruptVectorTable&) = delete;
    Ia64RuntimeInterruptVectorTable(Ia64RuntimeInterruptVectorTable&&) = delete;
    Ia64RuntimeInterruptVectorTable& operator=(Ia64RuntimeInterruptVectorTable&&) = delete;

    /// Performs every preparation, mapping, coverage, placement, slot, and
    /// repeat-state check without changing this owner, bindings, or CPU state.
    [[nodiscard]] Foundation::Result<Ia64RuntimeInterruptVectorTableInstallSummary,
                                     Ia64RuntimeInterruptVectorTableInstallError>
    ValidateInstall(Kernel::Scheduler::CpuId processor, const InitialAddressSpacePreparation& preparation,
                    Ia64ActiveRuntimeTranslationBindings& bindings,
                    Ia64RuntimeObjectRange runtimeObjects) const noexcept;

    /// Fills one unpublished context, performs exactly one native install, then
    /// release-admits the slot and sets the installed bit last. A post-native
    /// admission contradiction is unrecoverable and enters a local fail-stop.
    /// A recoverable native rejection rolls back a context created by that
    /// attempt so a different validated binding identity can be retried.
    /// Calls for every CPU sharing bindings are externally serialized.
    [[nodiscard]] Foundation::Result<Ia64RuntimeInterruptVectorTableInstallSummary,
                                     Ia64RuntimeInterruptVectorTableInstallError>
    InstallAndAdmit(Kernel::Scheduler::CpuId processor, const InitialAddressSpacePreparation& preparation,
                    Ia64ActiveRuntimeTranslationBindings& bindings, Ia64RuntimeObjectRange runtimeObjects) noexcept;

    /// Release-publishes one fully initialized system-call entry context to an
    /// already-installed CPU. The context must remain in the exact retained
    /// runtime-object range for every future break reader. Its frame and exact
    /// stack/RSE ranges are validated and retained in that same corridor, and
    /// none of those writable ranges may overlap the immutable context.
    /// Calls that mutate this owner are externally serialized.
    [[nodiscard]] Ia64RuntimeInterruptVectorTablePublicationError
    PublishSystemCallEntryContext(Kernel::Scheduler::CpuId processor,
                                  const System::Ia64SystemCallEntryContext* context) noexcept;

    /// Validates one system-call entry context exactly as
    /// PublishSystemCallEntryContext does and admits it for later selection
    /// on that processor without changing the published word. Admitting an
    /// admitted context again succeeds and changes nothing; at most
    /// MaximumSelectableEntryContexts contexts are admitted per processor.
    [[nodiscard]] Ia64RuntimeInterruptVectorTablePublicationError
    AdmitSystemCallEntryContext(Kernel::Scheduler::CpuId processor,
                                const System::Ia64SystemCallEntryContext* context) noexcept;

    /// Release-publishes one admitted (or published) system-call entry
    /// context as the processor's current one: the running user thread's.
    /// The owning processor calls it with PSR.i clear before that thread
    /// enters PL3, so no break observes a half-selected thread.
    [[nodiscard]] Ia64RuntimeInterruptVectorTablePublicationError
    SelectSystemCallEntryContext(Kernel::Scheduler::CpuId processor,
                                 const System::Ia64SystemCallEntryContext* context) noexcept;

    /// The user-fault counterparts at offset 40: validation is
    /// ValidateIa64UserFaultTrustedContext plus the same corridor, placement
    /// and disjointness rules as a system-call entry context.
    [[nodiscard]] Ia64RuntimeInterruptVectorTablePublicationError
    AdmitUserFaultEntryContext(Kernel::Scheduler::CpuId processor,
                               const Interrupt::Ia64UserFaultEntryContext* context) noexcept;

    [[nodiscard]] Ia64RuntimeInterruptVectorTablePublicationError
    SelectUserFaultEntryContext(Kernel::Scheduler::CpuId processor,
                                const Interrupt::Ia64UserFaultEntryContext* context) noexcept;

    /// User threads whose entry contexts one processor can select: the BSP runtime memory's
    /// thirty-four entry slots (two boot threads, thirty-two created;
    /// Documentation/Architecture/BootPager.md).
    static constexpr Foundation::Size MaximumSelectableEntryContexts{34U};

    /// Release-publishes one immutable external-interrupt context to offset 24
    /// of an installed processor's ar.k3 root. Delivery must remain disabled
    /// until this succeeds. The complete context and initially zero evidence
    /// must both remain in the exact locked runtime-object corridor for every
    /// future bank-zero access; they may not overlap. Calls are externally
    /// serialized and publication is one-shot.
    [[nodiscard]] Ia64RuntimeExternalInterruptPublicationError
    PublishExternalInterruptEntryContext(Kernel::Scheduler::CpuId processor,
                                         const Interrupt::Ia64ExternalInterruptEntryContext* context) noexcept;

    /// Release-publishes one immutable page-fault context to offset 32 of an
    /// installed processor's ar.k3 root. The context, configured control,
    /// frame, and coordinator stack/RBS must all reside in this owner's exact
    /// retained Region-0 runtime-object corridor and be pairwise disjoint. The
    /// one-shot frame must retain its exact never-used pristine state.
    /// The interrupted user RBS is separately validated prepared geometry and
    /// is intentionally not required to lie in this corridor, but cannot
    /// overlap the immutable context. Rejection is failure-atomic and
    /// publication is one-shot. The four heavyweight IVT trampolines consume
    /// this publication only for the capture-only native checkpoint; that
    /// checkpoint deliberately fail-stops before cover, user-RBS access,
    /// coordinator dispatch, or restore. Calls that mutate this owner are
    /// externally serialized by BSP bootstrap. The context, control, frame,
    /// and referenced ranges remain stable throughout validation and for their
    /// documented lifetime after publication.
    [[nodiscard]] Ia64RuntimePageFaultPublicationError
    PublishPageFaultEntryContext(Kernel::Scheduler::CpuId processor,
                                 const Interrupt::Ia64PageFaultEntryContext* context) noexcept;

    /// Production-shaped, side-effect-free verifier for hosted testing and
    /// review. Data refill admits an RSE reference, including its architected
    /// incomplete-register-frame companion state, but rejects an isolated
    /// incomplete frame. Instruction refill rejects either state, and nested
    /// interruption is always rejected. A data miss the ledger does not map
    /// is the UserFault outcome when the interrupted CPL is 3 and ISR.rs and
    /// ISR.ir are clear, and MappingNotFound (a stop) otherwise; every
    /// instruction miss stops. This model performs no ITC insertion and is
    /// never an IVT entry point.
    [[nodiscard]] static Foundation::Result<Ia64RuntimeRefillAction, Ia64RuntimeRefillError>
    ModelRefill(const Ia64RuntimeInterruptVectorTableContext& context, Foundation::UInt64 faultAddress,
                Ia64RuntimeRefillKind kind, Ia64RuntimeRefillProcessorState processorState) noexcept;

private:
    [[nodiscard]] Ia64RuntimeInterruptVectorTablePublicationError
    ValidateEntryContextPlacement(Kernel::Scheduler::CpuId processor, Foundation::UInt64 contextAddress,
                                  Foundation::UInt64 contextSizeBytes) const noexcept;

    [[nodiscard]] Ia64RuntimeInterruptVectorTablePublicationError
    ValidateEntryStorage(Foundation::UInt64 contextAddress, Foundation::UInt64 contextSizeBytes,
                         Foundation::UInt64 frameAddress, Foundation::UInt64 frameSizeBytes,
                         Foundation::UInt64 memoryStackStart, Foundation::UInt64 memoryStackEnd,
                         Foundation::UInt64 rseBackingStoreStart, Foundation::UInt64 rseBackingStoreEnd) const noexcept;

    [[nodiscard]] Ia64RuntimeInterruptVectorTablePublicationError
    ValidateSystemCallEntryContext(Kernel::Scheduler::CpuId processor,
                                   const System::Ia64SystemCallEntryContext* context) const noexcept;

    [[nodiscard]] Ia64RuntimeInterruptVectorTablePublicationError
    ValidateUserFaultEntryContext(Kernel::Scheduler::CpuId processor,
                                  const Interrupt::Ia64UserFaultEntryContext* context) const noexcept;

    [[nodiscard]] static Ia64RuntimeInterruptVectorTablePublicationError
    Admit(Foundation::UInt64 (&admitted)[MaximumSelectableEntryContexts], Foundation::UInt64 contextAddress) noexcept;

    [[nodiscard]] static bool IsAdmitted(const Foundation::UInt64 (&admitted)[MaximumSelectableEntryContexts],
                                         Foundation::UInt64 contextAddress) noexcept;

    Foundation::UInt64 m_stopStorageAddress{0U};
    Ia64RuntimeInterruptVectorTableContext m_contexts[Kernel::Scheduler::MaximumProcessorCount]{};
    Foundation::UInt64 m_admittedSystemCallContexts[Kernel::Scheduler::MaximumProcessorCount]
                                                   [MaximumSelectableEntryContexts]{};
    Foundation::UInt64 m_admittedUserFaultContexts[Kernel::Scheduler::MaximumProcessorCount]
                                                  [MaximumSelectableEntryContexts]{};
    Foundation::UInt64 m_installedProcessorMask{0U};
    Foundation::UInt64 m_runtimeObjectStart{0U};
    Foundation::UInt64 m_runtimeObjectEnd{0U};
};

static_assert(Ia64RuntimeInterruptVectorTableSizeBytes == 32U * 1024U);
static_assert(Ia64RuntimeInterruptVectorSlotSizeBytes == 0x400U);
static_assert(Ia64RuntimeHeavyweightInterruptVectorSlotSizeBytes == 0x100U);
static_assert(Ia64RuntimeAlternateInstructionTlbOffset == 0x0C00U);
static_assert(Ia64RuntimeAlternateDataTlbOffset == 0x1000U);
static_assert(Ia64RuntimeBreakOffset == 0x2C00U);
static_assert(Ia64RuntimeExternalInterruptOffset == 0x3000U);
static_assert(Ia64RuntimePageNotPresentOffset == 0x5000U);
static_assert(Ia64RuntimeKeyPermissionOffset == 0x5100U);
static_assert(Ia64RuntimeInstructionAccessRightsOffset == 0x5200U);
static_assert(Ia64RuntimeDataAccessRightsOffset == 0x5300U);
static_assert(Ia64RuntimeGeneralExceptionOffset == 0x5400U);
static_assert(Ia64RuntimeFloatingPointFaultOffset == 0x5C00U);
static_assert(Ia64RuntimeFloatingPointTrapOffset == 0x5D00U);
static_assert(sizeof(Ia64RuntimeInterruptVectorTableImage) == 32U * 1024U);
static_assert(alignof(Ia64RuntimeInterruptVectorTableImage) == 1U);
static_assert(__is_aggregate(Ia64RuntimeInterruptVectorTableImage));
static_assert(__is_standard_layout(Ia64RuntimeInterruptVectorTableImage));
static_assert(__is_trivially_copyable(Ia64RuntimeInterruptVectorTableImage));
static_assert(sizeof(Ia64RuntimeInterruptVectorTableContext) == 64U);
static_assert(alignof(Ia64RuntimeInterruptVectorTableContext) == 16U);
static_assert(__is_aggregate(Ia64RuntimeInterruptVectorTableContext));
static_assert(__is_standard_layout(Ia64RuntimeInterruptVectorTableContext));
static_assert(__is_trivially_copyable(Ia64RuntimeInterruptVectorTableContext));
static_assert(__builtin_offsetof(Ia64RuntimeInterruptVectorTableContext, BindingsWireAddress) == 0U);
static_assert(__builtin_offsetof(Ia64RuntimeInterruptVectorTableContext, ProcessorIndex) == 8U);
static_assert(__builtin_offsetof(Ia64RuntimeInterruptVectorTableContext, SystemCallEntryContextAddress) == 16U);
static_assert(__builtin_offsetof(Ia64RuntimeInterruptVectorTableContext, ExternalInterruptEntryContextAddress) == 24U);
static_assert(__builtin_offsetof(Ia64RuntimeInterruptVectorTableContext, PageFaultEntryContextAddress) == 32U);
static_assert(__builtin_offsetof(Ia64RuntimeInterruptVectorTableContext, UserFaultEntryContextAddress) == 40U);
static_assert(sizeof(Ia64RuntimeObjectRange) == 16U);
static_assert(alignof(Ia64RuntimeObjectRange) == 8U);
static_assert(__is_aggregate(Ia64RuntimeObjectRange));
static_assert(__is_standard_layout(Ia64RuntimeObjectRange));
static_assert(__is_trivially_copyable(Ia64RuntimeObjectRange));
static_assert(sizeof(Ia64RuntimeRefillProcessorState) == 24U);
static_assert(sizeof(Ia64RuntimeRefillOutcome) == 1U);
static_assert(sizeof(Ia64RuntimeRefillAction) == 32U);
static_assert(alignof(Ia64RuntimeRefillAction) == 8U);
static_assert(__is_aggregate(Ia64RuntimeRefillAction));
static_assert(__is_standard_layout(Ia64RuntimeRefillAction));
static_assert(__is_trivially_copyable(Ia64RuntimeRefillAction));
static_assert(sizeof(Ia64RuntimeInterruptVectorTableInstallSummary) == 24U);
static_assert(alignof(Ia64RuntimeInterruptVectorTableInstallSummary) == 8U);
static_assert(__is_aggregate(Ia64RuntimeInterruptVectorTableInstallSummary));
static_assert(__is_standard_layout(Ia64RuntimeInterruptVectorTableInstallSummary));
static_assert(__is_trivially_copyable(Ia64RuntimeInterruptVectorTableInstallSummary));
static_assert(sizeof(Ia64RuntimeInterruptVectorTablePublicationError) == 1U);
static_assert(sizeof(Ia64RuntimeExternalInterruptPublicationError) == 1U);
static_assert(sizeof(Ia64RuntimePageFaultPublicationError) == 1U);
static_assert(sizeof(Ia64RuntimeInterruptVectorTable) == 2480U);
static_assert(alignof(Ia64RuntimeInterruptVectorTable) == 16U);
} // Namespace Asteria::Platform::Ia64::Memory

#endif // ASTERIA_PLATFORM_IA64_MEMORY_IA64_RUNTIME_INTERRUPT_VECTOR_TABLE_HPP_INCLUDED
