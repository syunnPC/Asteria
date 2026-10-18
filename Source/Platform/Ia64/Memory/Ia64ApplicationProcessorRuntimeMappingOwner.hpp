// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_MEMORY_IA64_APPLICATION_PROCESSOR_RUNTIME_MAPPING_OWNER_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_MEMORY_IA64_APPLICATION_PROCESSOR_RUNTIME_MAPPING_OWNER_HPP_INCLUDED

#include "Foundation/Compiler.hpp"
#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Memory/AddressSpace.hpp"
#include "Kernel/Memory/Frame.hpp"
#include "Kernel/Memory/MemoryObject.hpp"
#include "Kernel/Memory/PinnedFrameMapping.hpp"
#include "Kernel/Memory/TlbShootdownDispatcher.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Platform/Ia64/Memory/ApplicationProcessorExecutionStorageOwner.hpp"
#include "Platform/Ia64/Memory/Ia64ApplicationProcessorRuntimeLayout.hpp"
#include "Platform/Ia64/Memory/Ia64ApplicationProcessorRuntimeWorkerStorageOwner.hpp"
#include "Platform/Ia64/Memory/Ia64BspRuntimeMemory.hpp"

namespace Asteria::Platform::Ia64::Memory
{
inline constexpr Foundation::Size Ia64ApplicationProcessorRuntimeMaximumMappingCount{
    2U * Ia64ApplicationProcessorRuntimeMaximumProcessorCount};
inline constexpr Foundation::Size Ia64ApplicationProcessorRuntimeMaximumOwnedFrameCount{
    Ia64ApplicationProcessorRuntimeMaximumProcessorCount + 1U};
inline constexpr Foundation::Size Ia64ApplicationProcessorRuntimeMaximumOwnedMemoryObjectCount{
    2U * Ia64ApplicationProcessorRuntimeMaximumProcessorCount};

enum class Ia64ApplicationProcessorRuntimeMappingOwnerState : Foundation::UInt8
{
    Fresh,
    Failed,
    RootFaulted,
    Ready,
};

enum class Ia64ApplicationProcessorRuntimeMappingOwnerError : Foundation::UInt32
{
    None,
    AlreadyPrepared,
    BspRuntimeMemoryNotReady,
    BspRootBorrowRejected,
    InvalidBspRoot,
    ApplicationProcessorAlreadyAdmitted,
    ExecutionStorageOwnerNotReady,
    InvalidExecutionStorageOwner,
    WorkerStorageOwnerNotReady,
    InvalidWorkerStorageOwner,
    LayoutRejected,
    InvalidDispatcher,
    InvalidResourceDomain,
    RegistryCapacityExceeded,
    ResourceCapacityExceeded,
    MappingCapacityExceeded,
    InvalidExistingMapping,
    GuardedVirtualRangeOccupied,
    PhysicalStorageAlreadyMapped,
    FrameInitializationRejected,
    MemoryObjectInitializationRejected,
    ObjectRegistrationRejected,
    RegistryBindingRejected,
    MappingPreparationRejected,
    AddressSpaceMappingRejected,
    ShootdownRejected,
    ShootdownReceiptMismatch,
    MappingCompletionRejected,
    PostconditionRejected,
    IssuerRegistryMismatch,
};

enum class Ia64ApplicationProcessorRuntimeMappingKind : Foundation::UInt8
{
    None,
    Idle,
    Worker,
};

enum class Ia64ApplicationProcessorRuntimeMappingAccessError : Foundation::UInt32
{
    None,
    NotReady,
    InvalidProcessor,
};

struct Ia64ApplicationProcessorRuntimeMappingOwnership
{
    Kernel::Memory::FrameId IdleFrame;
    Kernel::Memory::MemoryObjectId IdleMemoryObject;
    Kernel::Memory::MemoryObjectId WorkerMemoryObject;
    Kernel::Memory::AddressSpaceMappingReceipt IdleMapping;
    Kernel::Memory::AddressSpaceMappingReceipt WorkerMapping;
    Kernel::Scheduler::CpuId Processor;
    Foundation::UInt8 Reserved[7];
};

struct Ia64ApplicationProcessorRuntimeMappingSummary
{
    Kernel::Memory::AddressSpaceId AddressSpaceIdentity;
    Kernel::Memory::FrameId WorkerFrame;
    Foundation::Size InitialMappingCount;
    Foundation::Size FinalMappingCount;
    Foundation::UInt8 ProcessorCount;
    Foundation::UInt8 EnabledProcessorMask;
    Foundation::UInt8 ApplicationProcessorCount;
    Foundation::UInt8 OwnedFrameCount;
    Foundation::UInt8 OwnedMemoryObjectCount;
    Foundation::UInt8 InstalledMappingCount;
    Foundation::ProcessorMask ResidentProcessorMask;
    Foundation::UInt8 Reserved;
};

struct Ia64ApplicationProcessorRuntimeMappingFailure
{
    Ia64ApplicationProcessorRuntimeMappingOwnerError Error;
    Ia64ApplicationProcessorRuntimeLayoutError LayoutError;
    Kernel::Memory::AddressSpaceFailure AddressSpaceFailure;
    Kernel::Memory::TlbShootdownFailure ShootdownFailure;
    Kernel::Memory::MemoryObjectFailure MemoryObjectFailure;
    Kernel::Object::ObjectRegistryError RegistryError;
    Kernel::Object::ObjectPinError PinError;
    Kernel::Memory::FrameError FrameError;
    Kernel::Memory::PinnedFrameMappingError MappingPreparationError;
    Ia64BspRuntimeRootBorrowError RootBorrowError;
    Kernel::Resource::ResourceDomainError ResourceDomainError;
    Kernel::Scheduler::CpuId FailingProcessor;
    Ia64ApplicationProcessorRuntimeMappingKind MappingKind;
    bool RootMayBeFaulted;
    Foundation::UInt8 Reserved[3];
};

struct Ia64ApplicationProcessorRuntimeMappingSnapshot
{
    Ia64ApplicationProcessorRuntimeMappingSummary Summary;
    Ia64ApplicationProcessorRuntimeMappingFailure Failure;
    Kernel::Memory::AddressSpaceSnapshot InitialRoot;
    Kernel::Memory::AddressSpaceSnapshot FinalRoot;
    Kernel::Resource::ResourceDomainSnapshot InitialResourceDomain;
    Kernel::Resource::ResourceDomainSnapshot FinalResourceDomain;
    Foundation::Size InitialRegistryObjectCount;
    Foundation::Size FinalRegistryObjectCount;
    Foundation::UInt8 InitializedFrameCount;
    Foundation::UInt8 InitializedMemoryObjectCount;
    Foundation::UInt8 RegisteredObjectCount;
    Foundation::UInt8 RemotePublishAttemptCount;
    Ia64ApplicationProcessorRuntimeMappingOwnerState State;
    Foundation::UInt8 Reserved[3];
};

/// Boot-lifetime owner of every proactive AP idle and worker alias in the BSP root. Prepare is
/// BSP-serialized, single-use, allocation-free, and never admits, releases, wakes, transitions, or
/// activates an AP. Caller contract: - the exact registry and its backing lock, issuer bound to
/// that registry, BSP runtime/root generation, resource domain generation, both storage owners and
/// their physical storage, every owned Frame and MemoryObject generation, and this owner all have
/// boot lifetime; - throughout Prepare, the non-preemptible BSP does not migrate, permits no
/// same-CPU re-entry (including Prepare or dispatcher re-entry), holds no collaborator lock,
/// creates no lock wait cycle, and excludes registry mutation, object
/// destruction/quiescence/collection, root mutation, AP admission, and collaborator mutation; - no
/// accessor runs concurrently with Prepare. Before AP-side readers use a successful result or root
/// mappings, the caller publishes completion with release ordering and those readers observe it
/// with acquire ordering. Prepare may spin on collaborator locks and is not interrupt-context safe.
/// Topology, identity, residency, layout, virtual-vacancy, mapping-capacity, resource-capacity, and
/// the necessary active-object-count bound are checked before persistent effects. Active count is
/// diagnostic, not a free-slot reservation: quiescing or retired occupied registry slots can still
/// make registration fail after Frame or MemoryObject initialization. Every Prepare failure is
/// terminal. Failures after the first persistent effect retain all initialized objects,
/// registrations, pins, resource reservations, and append-only mappings. Terminal return clears
/// transaction-only raw borrow slots; it does not release retained ownership or relax boot-lifetime
/// rules.
class Ia64ApplicationProcessorRuntimeMappingOwner final
{
public:
    constexpr Ia64ApplicationProcessorRuntimeMappingOwner() noexcept = default;
    ~Ia64ApplicationProcessorRuntimeMappingOwner() noexcept = default;

    Ia64ApplicationProcessorRuntimeMappingOwner(const Ia64ApplicationProcessorRuntimeMappingOwner&) = delete;
    Ia64ApplicationProcessorRuntimeMappingOwner& operator=(const Ia64ApplicationProcessorRuntimeMappingOwner&) = delete;
    Ia64ApplicationProcessorRuntimeMappingOwner(Ia64ApplicationProcessorRuntimeMappingOwner&&) = delete;
    Ia64ApplicationProcessorRuntimeMappingOwner& operator=(Ia64ApplicationProcessorRuntimeMappingOwner&&) = delete;

    [[nodiscard]] Foundation::Result<Ia64ApplicationProcessorRuntimeMappingSummary,
                                     Ia64ApplicationProcessorRuntimeMappingFailure>
    Prepare(Ia64BspRuntimeMemory& bspRuntimeMemory,
            const ApplicationProcessorExecutionStorageOwner& executionStorageOwner,
            const Ia64ApplicationProcessorRuntimeWorkerStorageOwner& workerStorageOwner,
            Kernel::Object::ObjectRegistry& registry, Kernel::Object::RootCapabilityIssuer& issuer,
            Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
            Kernel::Resource::ResourceDomain& resourceDomain,
            Kernel::Memory::TlbShootdownDispatcher& shootdownDispatcher) noexcept;

    [[nodiscard]] constexpr Ia64ApplicationProcessorRuntimeMappingOwnerState GetState() const noexcept
    {
        return m_state;
    }

    [[nodiscard]] Foundation::Result<Ia64ApplicationProcessorRuntimeLayoutPlan,
                                     Ia64ApplicationProcessorRuntimeMappingAccessError>
    GetPlan() const noexcept;

    [[nodiscard]] Foundation::Result<Ia64ApplicationProcessorRuntimeMappingSummary,
                                     Ia64ApplicationProcessorRuntimeMappingAccessError>
    GetSummary() const noexcept;

    [[nodiscard]] Foundation::Result<Ia64ApplicationProcessorRuntimeMappingOwnership,
                                     Ia64ApplicationProcessorRuntimeMappingAccessError>
    GetOwnership(Kernel::Scheduler::CpuId processor) const noexcept;

    /// Diagnostics remain available after terminal failure. Summary fields and
    /// identities are authoritative for consumption only when State is Ready.
    [[nodiscard]] Ia64ApplicationProcessorRuntimeMappingSnapshot GetSnapshot() const noexcept;

private:
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorRuntimeMappingOwnerError
    PreflightBspAndTopology() noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorRuntimeMappingOwnerError
    CaptureBspRuntimeSnapshot() noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorRuntimeMappingOwnerError
    BorrowAndValidateBspRoot() noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorRuntimeMappingOwnerError
    ValidateStorageTopology() noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorRuntimeMappingOwnerError
    ValidateApplicationProcessorState() noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorRuntimeMappingOwnerError
    PreflightResourceDomain() noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorRuntimeMappingOwnerError
    PreflightLayoutAndCapacity() noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorRuntimeMappingOwnerError
    PreflightExistingMappings() noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorRuntimeMappingOwnerError
    FinishZeroApplicationProcessorResult() noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorRuntimeMappingOwnerError InitializeObjects() noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorRuntimeMappingOwnerError
    RegisterAndVerify(Kernel::Object::IKernelObject& object, Kernel::Object::ObjectType expectedType,
                      Kernel::Object::ObjectId& identity) noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorRuntimeMappingOwnerError RegisterObjects() noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorRuntimeMappingOwnerError
    InstallMapping(Foundation::Size index, Ia64ApplicationProcessorRuntimeMappingKind kind) noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorRuntimeMappingOwnerError InstallMappings() noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorRuntimeMappingOwnerError
    ValidateOwnedObjects(Foundation::Size index) noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorRuntimeMappingOwnerError
    ValidateCommittedMapping(Foundation::Size index, Ia64ApplicationProcessorRuntimeMappingKind kind,
                             Foundation::Size installedIndex, Foundation::UInt64 priorSequence) noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorRuntimeMappingOwnerError
    ValidateGuardVacancy() noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorRuntimeMappingOwnerError
    VerifyFinalPostconditions() noexcept;

    void CaptureFinalDiagnostics() noexcept;
    void ClearPrepareInputs() noexcept;

    [[nodiscard]] Foundation::Result<Ia64ApplicationProcessorRuntimeMappingSummary,
                                     Ia64ApplicationProcessorRuntimeMappingFailure>
    Fail(Ia64ApplicationProcessorRuntimeMappingOwnerError error) noexcept;

    Kernel::Memory::BootFrame m_idleFrames[Ia64ApplicationProcessorRuntimeMaximumProcessorCount];
    Kernel::Memory::BootFrame m_workerFrame;
    Kernel::Memory::MemoryObject m_idleMemoryObjects[Ia64ApplicationProcessorRuntimeMaximumProcessorCount];
    Kernel::Memory::MemoryObject m_workerMemoryObjects[Ia64ApplicationProcessorRuntimeMaximumProcessorCount];
    Kernel::Memory::PinnedFrameMappingPreparation
        m_mappingPreparations[Ia64ApplicationProcessorRuntimeMaximumMappingCount];
    Ia64ApplicationProcessorRuntimeMappingOwnership m_ownership[Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Ia64ApplicationProcessorRuntimeLayoutPlan m_plan{};
    Ia64BspRuntimeMemorySnapshot m_bspSnapshot{};
    Kernel::Memory::AddressSpaceSnapshot m_initialRootSnapshot{};
    Kernel::Memory::AddressSpaceSnapshot m_finalRootSnapshot{};
    Kernel::Resource::ResourceDomainSnapshot m_initialResourceDomainSnapshot{};
    Kernel::Resource::ResourceDomainSnapshot m_finalResourceDomainSnapshot{};
    Ia64ApplicationProcessorRuntimeMappingSummary m_summary{};
    Ia64ApplicationProcessorRuntimeMappingFailure m_failure{};
    Ia64BspRuntimeMemory* m_bspRuntimeMemory{nullptr};
    const ApplicationProcessorExecutionStorageOwner* m_executionStorageOwner{nullptr};
    const Ia64ApplicationProcessorRuntimeWorkerStorageOwner* m_workerStorageOwner{nullptr};
    Kernel::Object::ObjectRegistry* m_registry{nullptr};
    Kernel::Object::RootCapabilityIssuer* m_issuer{nullptr};
    Kernel::Resource::ResourceDomain* m_resourceDomain{nullptr};
    Kernel::Memory::TlbShootdownDispatcher* m_shootdownDispatcher{nullptr};
    Kernel::Memory::AddressSpace* m_rootAddressSpace{nullptr};
    Kernel::Resource::ResourceDomainObjectId m_resourceDomainIdentity{};
    Kernel::Object::ResourceDomainId m_resourceDomainIdentifier{};
    Kernel::Memory::PhysicalPageRange m_workerPool{};
    Kernel::Scheduler::CpuId m_bspProcessor{};
    Foundation::Size m_initialRegistryObjectCount{0U};
    Foundation::Size m_finalRegistryObjectCount{0U};
    Foundation::UInt8 m_initializedFrameCount{0U};
    Foundation::UInt8 m_initializedMemoryObjectCount{0U};
    Foundation::UInt8 m_registeredObjectCount{0U};
    Foundation::UInt8 m_remotePublishAttemptCount{0U};
    Ia64ApplicationProcessorRuntimeMappingOwnerState m_state{Ia64ApplicationProcessorRuntimeMappingOwnerState::Fresh};
    [[maybe_unused]] Foundation::UInt8 m_reserved[3]{};
};

static_assert(Ia64ApplicationProcessorRuntimeMaximumMappingCount == 6U);
static_assert(Ia64ApplicationProcessorRuntimeMaximumOwnedFrameCount == 4U);
static_assert(Ia64ApplicationProcessorRuntimeMaximumOwnedMemoryObjectCount == 6U);
static_assert(sizeof(Ia64ApplicationProcessorRuntimeMappingOwnerState) == 1U);
static_assert(sizeof(Ia64ApplicationProcessorRuntimeMappingOwnerError) == 4U);
static_assert(sizeof(Ia64ApplicationProcessorRuntimeMappingKind) == 1U);
static_assert(sizeof(Ia64ApplicationProcessorRuntimeMappingAccessError) == 4U);
static_assert(__is_aggregate(Ia64ApplicationProcessorRuntimeMappingOwnership));
static_assert(__is_standard_layout(Ia64ApplicationProcessorRuntimeMappingOwnership));
static_assert(__is_trivially_copyable(Ia64ApplicationProcessorRuntimeMappingOwnership));
static_assert(sizeof(Ia64ApplicationProcessorRuntimeMappingOwnership) == 120U);
static_assert(alignof(Ia64ApplicationProcessorRuntimeMappingOwnership) == 8U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeMappingOwnership, IdleFrame) == 0U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeMappingOwnership, IdleMemoryObject) == 16U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeMappingOwnership, WorkerMemoryObject) == 32U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeMappingOwnership, IdleMapping) == 48U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeMappingOwnership, WorkerMapping) == 80U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeMappingOwnership, Processor) == 112U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeMappingOwnership, Reserved) == 113U);
static_assert(__is_aggregate(Ia64ApplicationProcessorRuntimeMappingSummary));
static_assert(__is_standard_layout(Ia64ApplicationProcessorRuntimeMappingSummary));
static_assert(__is_trivially_copyable(Ia64ApplicationProcessorRuntimeMappingSummary));
static_assert(sizeof(Ia64ApplicationProcessorRuntimeMappingSummary) == 72U);
static_assert(alignof(Ia64ApplicationProcessorRuntimeMappingSummary) == 8U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeMappingSummary, AddressSpaceIdentity) == 0U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeMappingSummary, WorkerFrame) == 16U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeMappingSummary, InitialMappingCount) == 32U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeMappingSummary, FinalMappingCount) == 40U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeMappingSummary, ProcessorCount) == 48U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeMappingSummary, Reserved) == 64U);
static_assert(__is_aggregate(Ia64ApplicationProcessorRuntimeMappingFailure));
static_assert(__is_standard_layout(Ia64ApplicationProcessorRuntimeMappingFailure));
static_assert(__is_trivially_copyable(Ia64ApplicationProcessorRuntimeMappingFailure));
static_assert(sizeof(Ia64ApplicationProcessorRuntimeMappingFailure) == 44U);
static_assert(alignof(Ia64ApplicationProcessorRuntimeMappingFailure) == 4U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeMappingFailure, Error) == 0U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeMappingFailure, AddressSpaceFailure) == 8U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeMappingFailure, ShootdownFailure) == 16U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeMappingFailure, MemoryObjectFailure) == 24U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeMappingFailure, RegistryError) == 32U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeMappingFailure, FailingProcessor) == 38U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeMappingFailure, MappingKind) == 39U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeMappingFailure, RootMayBeFaulted) == 40U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeMappingFailure, Reserved) == 41U);
static_assert(__is_aggregate(Ia64ApplicationProcessorRuntimeMappingSnapshot));
static_assert(__is_standard_layout(Ia64ApplicationProcessorRuntimeMappingSnapshot));
static_assert(__is_trivially_copyable(Ia64ApplicationProcessorRuntimeMappingSnapshot));
static_assert(sizeof(Ia64ApplicationProcessorRuntimeMappingSnapshot) == 704U);
static_assert(alignof(Ia64ApplicationProcessorRuntimeMappingSnapshot) == 8U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeMappingSnapshot, Summary) == 0U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeMappingSnapshot, Failure) == 72U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeMappingSnapshot, InitialRoot) == 120U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeMappingSnapshot, FinalRoot) == 208U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeMappingSnapshot, InitialResourceDomain) == 296U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeMappingSnapshot, FinalResourceDomain) == 488U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeMappingSnapshot, InitialRegistryObjectCount) == 680U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeMappingSnapshot, FinalRegistryObjectCount) == 688U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeMappingSnapshot, InitializedFrameCount) == 696U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeMappingSnapshot, State) == 700U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeMappingSnapshot, Reserved) == 701U);
static_assert(sizeof(Ia64ApplicationProcessorRuntimeMappingOwner) == 36816U);
static_assert(alignof(Ia64ApplicationProcessorRuntimeMappingOwner) == 16U);
} // Namespace Asteria::Platform::Ia64::Memory

#endif // ASTERIA_PLATFORM_IA64_MEMORY_IA64_APPLICATION_PROCESSOR_RUNTIME_MAPPING_OWNER_HPP_INCLUDED
