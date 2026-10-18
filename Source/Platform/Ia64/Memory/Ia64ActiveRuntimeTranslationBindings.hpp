// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_MEMORY_IA64_ACTIVE_RUNTIME_TRANSLATION_BINDINGS_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_MEMORY_IA64_ACTIVE_RUNTIME_TRANSLATION_BINDINGS_HPP_INCLUDED

#include "Foundation/Compiler.hpp"
#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Scheduler/SchedulerTypes.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"

namespace Asteria::Platform::Ia64::Memory
{
class Ia64RuntimeRegionSet;
class Ia64RuntimeInterruptVectorTable;
class Ia64RuntimeTranslationLedger;

enum class Ia64ActiveRuntimeTranslationBindingError : Foundation::UInt8
{
    None,
    InvalidProcessor,
    ProcessorAlreadyAdmitted,
    ProcessorNotAdmitted,
    NoActiveLedger,
    MalformedBindingEncoding,
    BindingTableOutsideRegionZero,
    RegionSetOutsideRegionZero,
    LedgerOutsideRegionZero,
    RuntimeObjectRangeNotConfigured,
    RuntimeObjectRangeMismatch,
    AdmissionOwnerMismatch,
    RegionSetOutsideRuntimeObjectRange,
    LedgerOutsideRuntimeObjectRange,
    InvalidRegionSet,
    ActivationRejectedBeforeCommit,
    ActivationStateUnknown,
    ActiveLedgerMismatch,
};

/// Stable layout consumed by the alternate-refill IVT. Slots are first so a
/// context needs only this wire base and a validated CPU index to derive one
/// exact slot address. The remaining words are immutable after first admission.
struct Ia64ActiveRuntimeTranslationBindingsWireStorage
{
    Synchronization::AtomicUInt64 Slots[Kernel::Scheduler::MaximumProcessorCount];
    Foundation::UInt64 RuntimeObjectStart;
    Foundation::UInt64 RuntimeObjectEnd;
    Foundation::UInt64 AdmissionOwnerAddress;
};

/// Owns one active runtime-translation ledger binding for each possible CPU. Each atomic slot has
/// this exact encoding: - zero: the CPU's refill path and shared corridor are not admitted; - one:
/// the CPU is admitted but has no active ledger; and - any other valid word: a naturally aligned
/// Region-0 pointer to an active const Ia64RuntimeTranslationLedger. Admission is private to
/// Ia64RuntimeInterruptVectorTable. Its exact install owner configures one immutable Region-0
/// runtime-object corridor before the first slot is release-published. Region-set and ledger
/// operands must remain wholly within that corridor. The table and every published ledger outlive
/// all readers. AcquireActiveLedger returns a borrowed const pointer. Boot-only admission precedes
/// runtime use. InstallAndAdmit/admission calls for all CPUs are externally serialized; the first
/// admitted slot release-publishes the immutable shared configuration. ActivateAndBind,
/// ActivateAndBindPhysicalBootstrap, and ClearActiveLedger mutations of one CPU slot are externally
/// serialized single-writer operations with preemption, migration, and processor-local re-entry
/// excluded. Cross-CPU slots are independent. After ActivationStateUnknown, the AddressSpace
/// fail-stop/residency contract retains both the target and any prior ledger; IsBound cannot
/// release either lifetime. IVT installation and backend ownership are caller-owned.
class Ia64ActiveRuntimeTranslationBindings final
{
public:
    constexpr Ia64ActiveRuntimeTranslationBindings() noexcept = default;
    ~Ia64ActiveRuntimeTranslationBindings() noexcept = default;

    Ia64ActiveRuntimeTranslationBindings(const Ia64ActiveRuntimeTranslationBindings&) = delete;
    Ia64ActiveRuntimeTranslationBindings& operator=(const Ia64ActiveRuntimeTranslationBindings&) = delete;
    Ia64ActiveRuntimeTranslationBindings(Ia64ActiveRuntimeTranslationBindings&&) = delete;
    Ia64ActiveRuntimeTranslationBindings& operator=(Ia64ActiveRuntimeTranslationBindings&&) = delete;

    /// Validates the complete Region-0 operand corridor, activates regionSet, and publishes ledger
    /// only after exact native success. A committed-state uncertainty clears any prior pointer to
    /// admitted sentinel one. CPL0, PSR.vm=0, software exclusion, pinned shared-Global coverage,
    /// and operand lifetime are caller-owned preconditions described by the region activator.
    /// Numerical Region-0 placement checked here is necessary but cannot prove that coverage.
    [[nodiscard]] Ia64ActiveRuntimeTranslationBindingError
    ActivateAndBind(Kernel::Scheduler::CpuId processor, const Ia64RuntimeRegionSet& regionSet,
                    const Ia64RuntimeTranslationLedger& ledger) noexcept;

    /// Physical Region-0 counterpart used before a post-seed AP enables translation or enters
    /// architecture-neutral AddressSpace admission. The selected processor must already be admitted
    /// with exact sentinel one. A valid existing ledger returns ActiveLedgerMismatch without a
    /// native call; this bootstrap entry never replaces a prior publication. The complete
    /// configured object corridor is validated before calling Ia64RuntimeRegionActivator:
    /// ActivatePhysicalBootstrap. Exact native success release-publishes ledger. Precommit
    /// rejection preserves sentinel one, and committed uncertainty conservatively republishes
    /// sentinel one. This method records no AddressSpace residency. It owns only the strict
    /// admitted-empty slot transition; the calling runtime backend owns the lifetime one-shot and
    /// controller-acknowledgement state. In addition to the ordinary software-exclusion and
    /// lifetime rules, the complete direct call chain, GP-addressed data, memory stack, RSE backing
    /// store, this table, regionSet, ledger, and every accessed operand must remain resident at
    /// physical Region-0 addresses while PSR.it, PSR.dt, and PSR.rt are clear. Numerical object
    /// checks cannot prove the caller-owned code/GP/stack/RSE portion of that contract.
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ActiveRuntimeTranslationBindingError
    ActivateAndBindPhysicalBootstrap(Kernel::Scheduler::CpuId processor, const Ia64RuntimeRegionSet& regionSet,
                                     const Ia64RuntimeTranslationLedger& ledger) noexcept;

    /// Performs one acquire load and returns one borrowed immutable pointer.
    [[nodiscard]] Foundation::Result<const Ia64RuntimeTranslationLedger*, Ia64ActiveRuntimeTranslationBindingError>
    AcquireActiveLedger(Kernel::Scheduler::CpuId processor) const noexcept;

    /// Replaces exactly expected with admitted sentinel one. The caller must
    /// first quiesce every same-CPU handler/refill reader that could retain the
    /// borrowed pointer.
    [[nodiscard]] Ia64ActiveRuntimeTranslationBindingError
    ClearActiveLedger(Kernel::Scheduler::CpuId processor, const Ia64RuntimeTranslationLedger& expected) noexcept;

    /// Bounded post-quiescence consistency guard. This is not reader
    /// reclamation and is not sufficient proof that ledger may be destroyed.
    [[nodiscard]] bool IsBound(const Ia64RuntimeTranslationLedger& ledger) const noexcept;

private:
    friend class Ia64RuntimeInterruptVectorTable;

    [[nodiscard]] Ia64ActiveRuntimeTranslationBindingError
    ValidateAdmission(Kernel::Scheduler::CpuId processor, Foundation::UInt64 runtimeObjectStart,
                      Foundation::UInt64 runtimeObjectEnd, const Ia64RuntimeInterruptVectorTable& owner) const noexcept;

    [[nodiscard]] Ia64ActiveRuntimeTranslationBindingError
    AdmitProcessor(Kernel::Scheduler::CpuId processor, Foundation::UInt64 runtimeObjectStart,
                   Foundation::UInt64 runtimeObjectEnd, const Ia64RuntimeInterruptVectorTable& owner) noexcept;

    Ia64ActiveRuntimeTranslationBindingsWireStorage m_wire{};
};

static_assert(sizeof(Foundation::UIntPointer) <= sizeof(Foundation::UInt64));
static_assert(sizeof(Ia64ActiveRuntimeTranslationBindingError) == 1U);
static_assert(sizeof(Ia64ActiveRuntimeTranslationBindingsWireStorage) == 56U);
static_assert(alignof(Ia64ActiveRuntimeTranslationBindingsWireStorage) == 8U);
static_assert(__is_aggregate(Ia64ActiveRuntimeTranslationBindingsWireStorage));
static_assert(__is_standard_layout(Ia64ActiveRuntimeTranslationBindingsWireStorage));
static_assert(__is_trivially_copyable(Ia64ActiveRuntimeTranslationBindingsWireStorage));
static_assert(__builtin_offsetof(Ia64ActiveRuntimeTranslationBindingsWireStorage, Slots) == 0U);
static_assert(__builtin_offsetof(Ia64ActiveRuntimeTranslationBindingsWireStorage, RuntimeObjectStart) == 32U);
static_assert(__builtin_offsetof(Ia64ActiveRuntimeTranslationBindingsWireStorage, RuntimeObjectEnd) == 40U);
static_assert(__builtin_offsetof(Ia64ActiveRuntimeTranslationBindingsWireStorage, AdmissionOwnerAddress) == 48U);
static_assert(sizeof(Ia64ActiveRuntimeTranslationBindings) == 56U);
static_assert(alignof(Ia64ActiveRuntimeTranslationBindings) == 8U);
static_assert(__is_standard_layout(Ia64ActiveRuntimeTranslationBindings));
} // Namespace Asteria::Platform::Ia64::Memory

#endif // ASTERIA_PLATFORM_IA64_MEMORY_IA64_ACTIVE_RUNTIME_TRANSLATION_BINDINGS_HPP_INCLUDED
