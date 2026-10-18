// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_MEMORY_IA64_RUNTIME_ADDRESS_SPACE_PAGE_TABLE_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_MEMORY_IA64_RUNTIME_ADDRESS_SPACE_PAGE_TABLE_HPP_INCLUDED

#include "Foundation/Compiler.hpp"
#include "Foundation/Types.hpp"
#include "Hal/Memory/IAddressSpacePageTable.hpp"
#include "Platform/Ia64/Memory/Ia64ActiveRuntimeTranslationBindings.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeProcessorCapabilities.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeRegionSet.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeTranslationLedger.hpp"
#include "Platform/Ia64/Memory/Ia64TranslationCachePurge.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Platform::Ia64::Memory
{
class InitialAddressSpacePreparation;

enum class Ia64RuntimeAddressSpacePageTableState : Foundation::UInt8
{
    Uninitialized,
    Ready,
    Claimed,
    Faulted,
    Retired,
};

enum class Ia64RuntimeAddressSpacePageTableInitializationError : Foundation::UInt8
{
    None,
    AlreadyInitialized,
    InvalidCapabilities,
    InvalidReceipt,
    InvalidPreparation,
    InvalidPlacement,
    RegionSetRejected,
    LedgerRejected,
};

/// Adapts one private IA-64 RID set and reusable-slot refill ledger to the architecture-neutral
/// AddressSpace page-table contract. The integration owner supplies the one authoritative monotonic
/// RID allocator and guarantees that each copied receipt successfully initializes at most one
/// backend. This object cannot detect receipt copying. A backend is permanently retired after
/// release, and its private RIDs are never reused. The backend, active binding table, preparation
/// and pending seed, later exact owner, and embedded ledger must be wholly in numerical IA-64
/// Region 0. That placement is necessary but does not prove pinned shared-Global coverage; the
/// alternate-refill-IVT and corridor owner retains that proof. The preparation and pending
/// seed outlive the initialized-but-unclaimed Ready state; Claim verifies identity and drops that
/// borrowed seed pointer. The backend, bindings, embedded ledger, and exact owner then outlive
/// every applicable call and borrowed refill reader. None may reside on a target stack. Lock order
/// is AddressSpace then backend; this backend never calls into an AddressSpace. Same-CPU activation
/// and clear remain externally serialized, with interrupts, preemption, migration, and re-entry
/// excluded. Release also requires proven native switch-away, zero AddressSpace residency,
/// uncertainty fail-stop handling, and refill-reader quiescence; IsBound is only an extra
/// post-quiescence guard. Switching region registers keeps the translation-cache entries of every
/// other RID, so a translation Unmap removed while this space was not resident on a processor can
/// still be cached there, and would match again once the space's RIDs are back in the region
/// registers. Each successful Unmap therefore advances an unmap epoch; Activate on a processor
/// where the space ran before and whose recorded epoch is older purges that processor's local
/// translation cache after binding (Ia64LocalTranslationCachePurge), or, with no purge configured
/// for it, rejects the activation before any native work. A purge after an unmap the shootdown
/// already covered is redundant, not wrong. A retired space needs none: its RIDs never return. This
/// backend never calls AdmitProcessor. A production-zero binding table rejects activation as
/// UnsupportedConfiguration before native activation. The physical-bootstrap entry assumes that the
/// alternate refill IVT and binding admission already succeeded; it establishes only the native
/// RRs, exact ledger, and pending controller acknowledgement described below. IVT installation,
/// pinned-corridor preparation, AP release, translated pivot, and IPI delivery are caller-owned.
class Ia64RuntimeAddressSpacePageTable final : public Hal::Memory::IAddressSpacePageTable
{
public:
    // The unmap epoch purges at reactivation.
    [[nodiscard]] bool DetachAcknowledgesShootdown(const bool unmapping) const noexcept override
    {
        return unmapping;
    }
    constexpr Ia64RuntimeAddressSpacePageTable() noexcept = default;
    ~Ia64RuntimeAddressSpacePageTable() noexcept = default;

    Ia64RuntimeAddressSpacePageTable(const Ia64RuntimeAddressSpacePageTable&) = delete;
    Ia64RuntimeAddressSpacePageTable& operator=(const Ia64RuntimeAddressSpacePageTable&) = delete;
    Ia64RuntimeAddressSpacePageTable(Ia64RuntimeAddressSpacePageTable&&) = delete;
    Ia64RuntimeAddressSpacePageTable& operator=(Ia64RuntimeAddressSpacePageTable&&) = delete;

    [[nodiscard]] Ia64RuntimeAddressSpacePageTableInitializationError
    Initialize(const Ia64RuntimeProcessorCapabilities& capabilities,
               const Ia64RegionIdentifierAllocationReceipt& receipt, const InitialAddressSpacePreparation& preparation,
               Ia64ActiveRuntimeTranslationBindings& activeBindings) noexcept;

    [[nodiscard]] bool IsReady() const noexcept override;

    [[nodiscard]] Ia64RuntimeAddressSpacePageTableState GetState() const noexcept;

    /// Whether plan is the seed Initialize accepts: sealed, the bootstrap's
    /// Global identity mappings of Region 0 and nothing else.
    [[nodiscard]] static bool IsCanonicalSeed(const Kernel::Memory::InitialMappingPlan& plan) noexcept;

    /// Names the purge Activate runs after an unmap (see the class comment);
    /// once, while Ready or Claimed. Until then an activation that needs a
    /// purge is rejected.
    [[nodiscard]] bool ConfigureTranslationCachePurge(Ia64LocalTranslationCachePurge& purge) noexcept;

    /// Bounded postcondition observer for the calling CPU's exact private
    /// ledger binding. This proves neither reader quiescence nor reclamation.
    [[nodiscard]] bool IsActiveLedgerBound(Kernel::Scheduler::CpuId processor) const noexcept;

    [[nodiscard]] Hal::Memory::AddressSpacePageTableOwnershipError
    Claim(const Kernel::Memory::AddressSpace& owner,
          const Kernel::Memory::InitialMappingPlan& seedPlan) noexcept override;

    [[nodiscard]] Hal::Memory::AddressSpacePageTableOwnershipError
    Release(const Kernel::Memory::AddressSpace& owner) noexcept override;

    /// Ordinary architecture-neutral activation. Its existing translated
    /// switch behavior is unchanged unless a successful physical bootstrap is
    /// awaiting controller admission. Only that exact-ledger acknowledgement
    /// branch may execute on the physical AP stack: it performs no native RR
    /// call and consumes the pending bit once. Every other successful branch
    /// retains the ordinary translated-runtime preconditions.
    [[nodiscard]] Hal::Memory::AddressSpacePageTableActivationError
    Activate(const Kernel::Memory::AddressSpace& owner, Kernel::Scheduler::CpuId processor) noexcept override;

    /// Establishes this already-claimed root's private RRs and active refill ledger while a
    /// post-seed AP still executes in physical Region 0 with PSR.it, PSR.dt, and PSR.rt clear. This
    /// IA-64-only bootstrap operation intentionally bypasses the architecture-neutral AddressSpace
    /// and ProcessorAddressSpaceController admission transaction. It does not publish processor
    /// residency or controller state. After exact physical success, and while still on the physical
    /// stack/RSE with translation disabled, the caller must perform ordinary controller
    /// Initialize/AddressSpace admission. That first ordinary Activate acknowledges only this
    /// backend's exact published ledger and makes no second native RR call. Only after controller
    /// residency succeeds may the caller pivot to translated execution. The binding slot must
    /// already be admitted with no ledger. Exact success makes this physical-bootstrap operation
    /// permanently one-shot for the selected backend/CPU. Calls repeated either before or after
    /// controller acknowledgement are rejected before native activation. Until the acknowledgement
    /// succeeds, a missing, malformed, or different active ledger is a conservative committed-state
    /// fault. The caller externally serializes the backend and selected binding slot against all
    /// translated users. The complete direct call chain, GP-addressed data, memory stack, RSE
    /// backing store, this backend, its exact owner, and every accessed operand remain resident at
    /// physical Region-0 addresses. Numerical placement checks cannot prove the code/GP/stack/RSE
    /// part of that contract.
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Hal::Memory::AddressSpacePageTableActivationError
    ActivatePhysicalBootstrap(const Kernel::Memory::AddressSpace& owner, Kernel::Scheduler::CpuId processor) noexcept;

    [[nodiscard]] Hal::Memory::AddressSpacePageTableError
    Map(const Kernel::Memory::AddressSpaceMappingRecord& mapping,
        Hal::Memory::AddressSpacePageTableMapResult& output) noexcept override;

    /// Clears the mapping's ledger record and returns its range for invalidation. The slot may be
    /// reused after the AddressSpace completes the unmap shootdown; a mapping absent from the
    /// ledger is CorruptState.
    [[nodiscard]] Hal::Memory::AddressSpacePageTableError
    Unmap(const Kernel::Memory::AddressSpaceMappingRecord& mapping,
          Hal::Memory::AddressSpacePageTableMapResult& output) noexcept override;

private:
    /// Whether activating on processor must purge first: the space ran there
    /// and an Unmap has happened since.
    [[nodiscard]] bool NeedsTranslationCachePurgeLocked(Kernel::Scheduler::CpuId processor) const noexcept;
    /// Records that the space is now current on processor at this epoch.
    void RecordRunLocked(Kernel::Scheduler::CpuId processor) noexcept;

    mutable Platform::Synchronization::SpinLock m_synchronization;
    Ia64RuntimeRegionSet m_regionSet;
    Ia64RuntimeTranslationLedger m_ledger;
    Ia64ActiveRuntimeTranslationBindings* m_activeBindings{nullptr};
    const Kernel::Memory::InitialMappingPlan* m_pendingSeedPlan{nullptr};
    const Kernel::Memory::AddressSpace* m_owner{nullptr};
    Ia64LocalTranslationCachePurge* m_translationCachePurge{nullptr};
    Foundation::UInt64 m_unmapEpoch{0U};
    Foundation::UInt64 m_ranUnmapEpoch[Kernel::Scheduler::MaximumProcessorCount]{};
    // Residency and epoch membership use the widened type.
    Foundation::ProcessorMask m_ranProcessorMask{0U};
    Foundation::ProcessorMask m_admittedProcessorMask{0U};
    Foundation::ProcessorMask m_physicalBootstrapCompletedProcessorMask{0U};
    Foundation::ProcessorMask m_physicalBootstrapAdmissionPendingProcessorMask{0U};
    Ia64RuntimeAddressSpacePageTableState m_state{Ia64RuntimeAddressSpacePageTableState::Uninitialized};
};

static_assert(sizeof(Ia64RuntimeAddressSpacePageTableState) == 1U);
static_assert(sizeof(Ia64RuntimeAddressSpacePageTableInitializationError) == 1U);
static_assert(sizeof(Ia64RuntimeAddressSpacePageTable) == 6'376U);
static_assert(alignof(Ia64RuntimeAddressSpacePageTable) == 8U);
} // Namespace Asteria::Platform::Ia64::Memory

#endif // ASTERIA_PLATFORM_IA64_MEMORY_IA64_RUNTIME_ADDRESS_SPACE_PAGE_TABLE_HPP_INCLUDED
