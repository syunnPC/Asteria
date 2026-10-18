// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_SCHEDULER_IA64_USER_EXECUTION_PROBE_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_SCHEDULER_IA64_USER_EXECUTION_PROBE_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Memory/BootstrapPageAllocator.hpp"
#include "Kernel/Memory/FaultFrameMapping.hpp"
#include "Kernel/Memory/MemoryObject.hpp"
#include "Kernel/Memory/TlbShootdownDispatcher.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Kernel/Scheduler/SchedulingContext.hpp"
#include "Platform/Ia64/Ia64UserVirtualLayout.hpp"
#include "Platform/Ia64/Memory/Ia64BspRuntimeMemory.hpp"
#include "Platform/Ia64/Scheduler/Ia64UserModeLaunch.hpp"

namespace Asteria::Platform::Ia64::Scheduler
{
constexpr Foundation::UInt64 Ia64UserExecutionProbeCodePageCount{
    ::Asteria::Platform::Ia64::Ia64UserExecutionProbeCodePageCount};
constexpr Foundation::UInt64 Ia64UserExecutionProbeMemoryStackPageCount{
    ::Asteria::Platform::Ia64::Ia64UserExecutionProbeMemoryStackPageCount};
constexpr Foundation::UInt64 Ia64UserExecutionProbeRseBackingStorePageCount{
    ::Asteria::Platform::Ia64::Ia64UserExecutionProbeRseBackingStorePageCount};
constexpr Foundation::UInt64 Ia64UserExecutionProbeCodeSizeBytes{
    ::Asteria::Platform::Ia64::Ia64UserExecutionProbeCodeSizeBytes};
constexpr Foundation::UInt64 Ia64UserExecutionProbeMemoryStackSizeBytes{
    ::Asteria::Platform::Ia64::Ia64UserExecutionProbeMemoryStackSizeBytes};
constexpr Foundation::UInt64 Ia64UserExecutionProbeRseBackingStoreSizeBytes{
    ::Asteria::Platform::Ia64::Ia64UserExecutionProbeRseBackingStoreSizeBytes};

constexpr Foundation::UInt64 Ia64UserExecutionProbeCodeVirtualStart{
    ::Asteria::Platform::Ia64::Ia64UserExecutionProbeCodeVirtualStart};
constexpr Foundation::UInt64 Ia64UserExecutionProbeMemoryStackVirtualStart{
    ::Asteria::Platform::Ia64::Ia64UserExecutionProbeMemoryStackVirtualStart};
constexpr Foundation::UInt64 Ia64UserExecutionProbeRseBackingStoreVirtualStart{
    ::Asteria::Platform::Ia64::Ia64UserExecutionProbeRseBackingStoreVirtualStart};

enum class Ia64UserExecutionProbeState : Foundation::UInt8
{
    Fresh,
    Staged,
    Failed,
    Ready,
};

enum class Ia64UserExecutionProbeMapping : Foundation::UInt8
{
    None,
    Code,
    MemoryStack,
    RseBackingStore,
};

enum class Ia64UserExecutionProbeStageError : Foundation::UInt32
{
    None,
    AlreadyStaged,
    InvalidCodeAllocation,
    InvalidMemoryStackAllocation,
    InvalidRseBackingStoreAllocation,
    InvalidAllocationAttributes,
    PhysicalAllocationsOverlap,
    InvalidCodeAlias,
    InvalidMemoryStackAlias,
    InvalidRseBackingStoreAlias,
    AliasPhysicalAddressMismatch,
    WritableAliasesOverlap,
    WritableAliasAliasesControlStorage,
    InvalidCodeImage,
    CodeImageAliasesWritableStorage,
};

enum class Ia64UserExecutionProbeError : Foundation::UInt32
{
    None,
    NotStaged,
    AlreadyPrepared,
    RootBorrowRejected,
    InvalidRoot,
    InvalidDispatcher,
    InvalidRegistryBinding,
    InvalidResourceDomain,
    InvalidMappingThread,
    MappingCapacityExceeded,
    GuardedVirtualRangeOccupied,
    PhysicalAllocationAlreadyOwned,
    FrameInitializationRejected,
    MemoryObjectInitializationRejected,
    ObjectRegistrationRejected,
    FaultTokenRejected,
    FaultClaimRejected,
    FaultMappingPreparationRejected,
    AddressSpaceMappingRejected,
    ShootdownRejected,
    MappingCompletionRejected,
    FaultCommitRejected,
    LaunchRecordRejected,
    PostconditionRejected,
};

struct Ia64UserExecutionProbeWritableAlias
{
    Foundation::UInt8* Data;
    Foundation::Size SizeBytes;
    Foundation::UInt64 PhysicalStart;
};

struct Ia64UserExecutionProbeStorageRegion
{
    Kernel::Memory::PhysicalPageRange Allocation;
    Ia64UserExecutionProbeWritableAlias WritableAlias;
};

struct Ia64UserExecutionProbeStorage
{
    Ia64UserExecutionProbeStorageRegion Code;
    Ia64UserExecutionProbeStorageRegion MemoryStack;
    Ia64UserExecutionProbeStorageRegion RseBackingStore;
};

struct Ia64UserExecutionProbeCodeView
{
    const Foundation::UInt8* Data;
    Foundation::Size SizeBytes;
};

struct Ia64UserExecutionProbeStageSummary
{
    Kernel::Memory::PhysicalPageRange Code;
    Kernel::Memory::PhysicalPageRange MemoryStack;
    Kernel::Memory::PhysicalPageRange RseBackingStore;
    Foundation::Size CodeImageSizeBytes;
};

struct Ia64UserExecutionProbeFailure
{
    Ia64UserExecutionProbeError Error;
    Ia64UserExecutionProbeMapping Mapping;
    Memory::Ia64BspRuntimeRootBorrowError RootBorrow;
    Kernel::Memory::FrameError Frame;
    Kernel::Memory::MemoryObjectFailure MemoryObject;
    Kernel::Object::ObjectRegistryError Registry;
    Kernel::Object::ObjectPinError Pin;
    Kernel::Memory::FaultTokenError FaultToken;
    Kernel::Memory::FaultFrameMappingError FaultMapping;
    Kernel::Memory::AddressSpaceFailure AddressSpace;
    Kernel::Memory::TlbShootdownFailure Shootdown;
    Ia64UserModeLaunchError Launch;
    Foundation::UInt8 Reserved[4U];
};

/// Fixed, boot-lifetime ownership for one deliberately bounded IA-64 CPL3
/// execution probe.
///
/// StageStorage is called before native translation takeover while each exact
/// allocator receipt still has a proven writable alias. PrepareLaunch is
/// called later on the admitted BSP root. Both calls are externally serialized
/// with migration, preemption, interrupt re-entry, and concurrent root mutation
/// excluded. The owner, all supplied collaborators, and every accepted page
/// remain alive and immovable for the complete boot lifetime.
class alignas(16U) Ia64UserExecutionProbe final
{
public:
    constexpr Ia64UserExecutionProbe() noexcept = default;
    ~Ia64UserExecutionProbe() noexcept = default;

    Ia64UserExecutionProbe(const Ia64UserExecutionProbe&) = delete;
    Ia64UserExecutionProbe& operator=(const Ia64UserExecutionProbe&) = delete;
    Ia64UserExecutionProbe(Ia64UserExecutionProbe&&) = delete;
    Ia64UserExecutionProbe& operator=(Ia64UserExecutionProbe&&) = delete;

    /// Validates every receipt, alias, and input interval before writing. On
    /// success all pages are initialized, instruction bytes are published,
    /// only pointer-free receipt metadata is retained, and the aliases must no
    /// longer be used after translation takeover. Validation failure is inert
    /// and retryable.
    [[nodiscard]] Foundation::Result<Ia64UserExecutionProbeStageSummary, Ia64UserExecutionProbeStageError>
    StageStorage(const Ia64UserExecutionProbeStorage& storage, Ia64UserExecutionProbeCodeView codeImage) noexcept;

    /// Registers the three fixed Frame/MemoryObject pairs and installs their
    /// mappings through the common fault-mapping and shootdown transaction.
    /// Preflight rejection leaves Staged retryable. Once initialization starts,
    /// every rejection is terminal and all committed state remains retained.
    [[nodiscard]] Foundation::Result<Ia64UserModeLaunchRequest, Ia64UserExecutionProbeFailure>
    PrepareLaunch(Memory::Ia64BspRuntimeMemory& runtimeMemory, Kernel::Object::ObjectRegistry& registry,
                  Kernel::Object::RootCapabilityIssuer& issuer,
                  Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
                  Kernel::Resource::ResourceDomain& resourceDomain, Kernel::Scheduler::ThreadId mappingThread,
                  Kernel::Memory::TlbShootdownDispatcher& shootdownDispatcher,
                  Ia64UserModeVirtualRange supervisorMemoryStack, Ia64UserModeVirtualRange supervisorRseBackingStore,
                  Foundation::UInt8 implementedVirtualAddressMostSignificantBit) noexcept;

    [[nodiscard]] constexpr Ia64UserExecutionProbeState GetState() const noexcept
    {
        return m_state;
    }

    [[nodiscard]] Foundation::Result<Ia64UserModeLaunchRequest, Ia64UserExecutionProbeError>
    GetLaunchRequest() const noexcept;

private:
    static constexpr Foundation::Size MappingCount{3U};

    [[nodiscard]] Ia64UserExecutionProbeFailure
    PrepareMappingObjects(Kernel::Object::ObjectRegistry& registry, Kernel::Object::RootCapabilityIssuer& issuer,
                          Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
                          Kernel::Resource::ResourceDomain& resourceDomain) noexcept;

    [[nodiscard]] Ia64UserExecutionProbeFailure
    InstallMapping(Foundation::Size index, Kernel::Object::ObjectRegistry& registry,
                   Kernel::Scheduler::ThreadId mappingThread, Memory::Ia64BspRuntimeRootView root,
                   Kernel::Memory::TlbShootdownDispatcher& shootdownDispatcher) noexcept;

    [[nodiscard]] Ia64UserExecutionProbeFailure PrepareFaultMapping(Foundation::Size index,
                                                                    Kernel::Object::ObjectRegistry& registry,
                                                                    Kernel::Scheduler::ThreadId mappingThread,
                                                                    Memory::Ia64BspRuntimeRootView root) noexcept;

    [[nodiscard]] Ia64UserExecutionProbeFailure BeginFaultMapping(Foundation::Size index,
                                                                  Memory::Ia64BspRuntimeRootView root) noexcept;

    [[nodiscard]] Ia64UserExecutionProbeFailure
    DispatchShootdown(Foundation::Size index, Memory::Ia64BspRuntimeRootView root,
                      Kernel::Memory::TlbShootdownDispatcher& shootdownDispatcher) noexcept;

    [[nodiscard]] Ia64UserExecutionProbeFailure CompleteFaultMapping(Foundation::Size index,
                                                                     Memory::Ia64BspRuntimeRootView root) noexcept;

    [[nodiscard]] Ia64UserExecutionProbeFailure
    BorrowAndValidateRoot(Memory::Ia64BspRuntimeMemory& runtimeMemory, Kernel::Object::ObjectRegistry& registry,
                          Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
                          Kernel::Resource::ResourceDomain& resourceDomain, Kernel::Scheduler::ThreadId mappingThread,
                          Kernel::Memory::TlbShootdownDispatcher& shootdownDispatcher,
                          Memory::Ia64BspRuntimeRootView& root) noexcept;

    [[nodiscard]] Ia64UserExecutionProbeFailure
    ValidateMappingVacancy(Memory::Ia64BspRuntimeRootView root, Foundation::Size& initialMappingCount) const noexcept;

    [[nodiscard]] Ia64UserExecutionProbeFailure
    BuildLaunchRecord(Ia64UserModeVirtualRange supervisorMemoryStack,
                      Ia64UserModeVirtualRange supervisorRseBackingStore,
                      Foundation::UInt8 implementedVirtualAddressMostSignificantBit) noexcept;

    [[nodiscard]] Ia64UserExecutionProbeFailure
    ValidatePostconditions(Memory::Ia64BspRuntimeRootView root, Foundation::Size initialMappingCount) const noexcept;

    [[nodiscard]] Ia64UserExecutionProbeFailure
    ValidateMappingPostcondition(Foundation::Size index, Memory::Ia64BspRuntimeRootView root,
                                 Foundation::Size initialMappingCount) const noexcept;

    Kernel::Memory::PhysicalPageRange m_allocations[MappingCount]{};
    Kernel::Memory::BootFrame m_frames[MappingCount];
    Kernel::Memory::MemoryObject m_memoryObjects[MappingCount];
    Kernel::Memory::FaultToken m_faultTokens[MappingCount];
    Kernel::Memory::FaultResolutionClaimToken m_faultClaims[MappingCount]{};
    Kernel::Memory::FaultFrameMappingPreparation m_mappingPreparations[MappingCount];
    Kernel::Memory::AddressSpaceShootdownRequest m_shootdownRequests[MappingCount]{};
    Kernel::Memory::FrameId m_frameIdentities[MappingCount]{};
    Kernel::Memory::MemoryObjectId m_memoryObjectIdentities[MappingCount]{};
    Kernel::Memory::AddressSpaceMappingReceipt m_mappingReceipts[MappingCount]{};
    alignas(16U) Ia64UserModeSupervisorContinuation m_supervisorContinuation{};
    Ia64UserModeLaunchRequest m_launchRequest{};
    Foundation::Size m_codeImageSizeBytes{0U};
    Ia64UserExecutionProbeState m_state{Ia64UserExecutionProbeState::Fresh};
};

static_assert(Ia64UserExecutionProbeCodePageCount == 1U);
static_assert(Ia64UserExecutionProbeMemoryStackPageCount == 4U);
static_assert(Ia64UserExecutionProbeRseBackingStorePageCount == 4U);
static_assert(Ia64UserExecutionProbeCodeSizeBytes == 4U * 1024U);
static_assert(Ia64UserExecutionProbeMemoryStackSizeBytes == 16U * 1024U);
static_assert(Ia64UserExecutionProbeRseBackingStoreSizeBytes == 16U * 1024U);
static_assert((Ia64UserExecutionProbeCodeVirtualStart >> 61U) == 1U);
static_assert((Ia64UserExecutionProbeMemoryStackVirtualStart >> 61U) == 1U);
static_assert((Ia64UserExecutionProbeRseBackingStoreVirtualStart >> 61U) == 1U);
static_assert(sizeof(Ia64UserExecutionProbeState) == 1U);
static_assert(sizeof(Ia64UserExecutionProbeMapping) == 1U);
static_assert(sizeof(Ia64UserExecutionProbeStageError) == 4U);
static_assert(sizeof(Ia64UserExecutionProbeError) == 4U);
static_assert(sizeof(Ia64UserExecutionProbeWritableAlias) == 24U);
static_assert(alignof(Ia64UserExecutionProbeWritableAlias) == 8U);
static_assert(__is_aggregate(Ia64UserExecutionProbeWritableAlias));
static_assert(__is_standard_layout(Ia64UserExecutionProbeWritableAlias));
static_assert(__is_trivially_copyable(Ia64UserExecutionProbeWritableAlias));
static_assert(sizeof(Ia64UserExecutionProbeStorageRegion) == 56U);
static_assert(alignof(Ia64UserExecutionProbeStorageRegion) == 8U);
static_assert(__is_aggregate(Ia64UserExecutionProbeStorageRegion));
static_assert(__is_standard_layout(Ia64UserExecutionProbeStorageRegion));
static_assert(__is_trivially_copyable(Ia64UserExecutionProbeStorageRegion));
static_assert(sizeof(Ia64UserExecutionProbeStorage) == 168U);
static_assert(alignof(Ia64UserExecutionProbeStorage) == 8U);
static_assert(__is_aggregate(Ia64UserExecutionProbeStorage));
static_assert(__is_standard_layout(Ia64UserExecutionProbeStorage));
static_assert(__is_trivially_copyable(Ia64UserExecutionProbeStorage));
static_assert(sizeof(Ia64UserExecutionProbeCodeView) == 16U);
static_assert(alignof(Ia64UserExecutionProbeCodeView) == 8U);
static_assert(__is_aggregate(Ia64UserExecutionProbeCodeView));
static_assert(__is_standard_layout(Ia64UserExecutionProbeCodeView));
static_assert(__is_trivially_copyable(Ia64UserExecutionProbeCodeView));
static_assert(sizeof(Ia64UserExecutionProbeStageSummary) == 104U);
static_assert(alignof(Ia64UserExecutionProbeStageSummary) == 8U);
static_assert(__is_aggregate(Ia64UserExecutionProbeStageSummary));
static_assert(__is_standard_layout(Ia64UserExecutionProbeStageSummary));
static_assert(__is_trivially_copyable(Ia64UserExecutionProbeStageSummary));
static_assert(sizeof(Ia64UserExecutionProbeFailure) == 44U);
static_assert(alignof(Ia64UserExecutionProbeFailure) == 4U);
static_assert(__is_aggregate(Ia64UserExecutionProbeFailure));
static_assert(__is_standard_layout(Ia64UserExecutionProbeFailure));
static_assert(__is_trivially_copyable(Ia64UserExecutionProbeFailure));
static_assert(alignof(Ia64UserExecutionProbe) == 16U);
} // namespace Asteria::Platform::Ia64::Scheduler

#endif // ASTERIA_PLATFORM_IA64_SCHEDULER_IA64_USER_EXECUTION_PROBE_HPP_INCLUDED
