// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_EXECUTABLE_IA64_INITIAL_USER_IMAGE_MEMORY_OWNER_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_EXECUTABLE_IA64_INITIAL_USER_IMAGE_MEMORY_OWNER_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Memory/Frame.hpp"
#include "Kernel/Memory/MemoryObject.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Platform/Ia64/Executable/Ia64InitialUserImageOwner.hpp"

namespace Asteria::Platform::Ia64::Executable
{
inline constexpr Foundation::Size Ia64InitialUserImageMaximumMemoryObjectCount{Ia64InitialUserImageMaximumSegmentCount +
                                                                               2U};
inline constexpr Foundation::Size Ia64InitialUserImageMaximumObjectRegistrationCount{
    Ia64InitialUserImageMaximumMemoryObjectCount + 1U};
inline constexpr Foundation::UInt64 Ia64InitialUserImageMaximumMaterializedPageCount{
    Ia64InitialUserImageMaximumLoadPages + 8U};

enum class Ia64InitialUserImageMemoryOwnerState : Foundation::UInt8
{
    Fresh,
    Failed,
    Ready,
};

enum class Ia64InitialUserImageMemoryPurpose : Foundation::UInt8
{
    LoadSegment,
    MemoryStack,
    RseBackingStore,
};

enum class Ia64InitialUserImageGuardPolicy : Foundation::UInt8
{
    None,
    OnePageBeforeAndAfter,
};

enum class Ia64InitialUserImageMemoryOwnerError : Foundation::UInt32
{
    None,
    AlreadyAttempted,
    SourceNotStaged,
    SourceAccessRejected,
    InvalidStageSummary,
    SegmentPlanRejected,
    InvalidAggregateArena,
    InvalidPhysicalPacking,
    InvalidVirtualRange,
    VirtualRangeOrderInvalid,
    PermissionRejected,
    RangeCapacityExceeded,
    IssuerRegistryMismatch,
    InvalidResourceDomain,
    RegistryCapacityExceeded,
    ResourceCapacityExceeded,
    StagedOwnerChanged,
    FrameInitializationRejected,
    MemoryObjectInitializationRejected,
    ObjectRegistrationRejected,
    RegistryBindingRejected,
    PostconditionRejected,
};

enum class Ia64InitialUserImageMemoryAccessError : Foundation::UInt32
{
    None,
    NotReady,
    IndexOutOfBounds,
    NoFailure,
};

struct Ia64InitialUserImageMemoryRangePlan
{
    Ia64InitialUserImageMemoryPurpose Purpose;
    Ia64InitialUserImageGuardPolicy GuardPolicy;
    Kernel::Memory::PagePermissions Permissions;
    Foundation::UInt8 Reserved[5];
    Foundation::Size OriginalSegmentIndex;
    Foundation::UInt64 VirtualStart;
    Kernel::Memory::PageCount Pages;
    Foundation::UInt64 FrameOffsetPages;
    Kernel::Memory::MemoryObjectId MemoryObject;
};

struct Ia64InitialUserImageMemoryRangeView
{
    Ia64InitialUserImageMemoryRangePlan Plan;
    Kernel::Memory::Frame* FrameStorage;
    Kernel::Memory::MemoryObject* MemoryObjectStorage;
};

struct Ia64InitialUserImageMemorySummary
{
    Ia64P64ImageInformation Image;
    Kernel::Memory::PhysicalPageRange AggregateArena;
    Kernel::Memory::FrameId AggregateFrame;
    Kernel::Resource::ResourceDomainObjectId ResourceDomainGeneration;
    Foundation::Size RangeCount;
};

struct Ia64InitialUserImageMemoryFailure
{
    Ia64InitialUserImageMemoryOwnerError Error;
    Ia64InitialUserImageOwnerAccessError SourceAccess;
    Kernel::Memory::MemoryObjectFailure MemoryObject;
    Foundation::Size RangeIndex;
    Foundation::Size OriginalSegmentIndex;
    Kernel::Memory::FrameError Frame;
    Kernel::Object::ObjectRegistryError Registry;
    Kernel::Object::ObjectPinError Pin;
    Kernel::Resource::ResourceDomainError ResourceDomain;
    Foundation::UInt8 Reserved[4];
};

/// Materializes one staged IA-64 initial-user arena as kernel object metadata.
///
/// The staging owner remains the sole owner of the allocator receipt and all
/// physical pages. This owner keeps one aggregate Frame, one MemoryObject for
/// each nonempty logical range, and their registered identities alive for the
/// boot lifetime. It creates no mapping, capability, protection domain,
/// capability space, thread, scheduling context, or launch record.
///
/// Materialize is externally serialized on the non-preemptible BSP. The
/// staging owner, registry, issuer, root ResourceDomain, this owner, and all
/// staged pages remain alive and immovable for the complete boot lifetime.
/// Concurrent external registry or ResourceDomain mutation, object
/// destruction, quiescence, and collection are excluded for the call.
/// Materialize itself changes the staging owner from Staged to Claimed at its
/// commit cut and then performs the described registrations and reservations.
class alignas(16U) Ia64InitialUserImageMemoryOwner final
{
public:
    constexpr Ia64InitialUserImageMemoryOwner() noexcept = default;
    ~Ia64InitialUserImageMemoryOwner() noexcept = default;

    Ia64InitialUserImageMemoryOwner(const Ia64InitialUserImageMemoryOwner&) = delete;
    Ia64InitialUserImageMemoryOwner& operator=(const Ia64InitialUserImageMemoryOwner&) = delete;
    Ia64InitialUserImageMemoryOwner(Ia64InitialUserImageMemoryOwner&&) = delete;
    Ia64InitialUserImageMemoryOwner& operator=(Ia64InitialUserImageMemoryOwner&&) = delete;

    /// Performs a preflight free of retained or persistent effects first; its
    /// ResourceDomain identity check takes and immediately resets a temporary
    /// pin. A preflight rejection leaves this owner Fresh and the staging owner
    /// Staged, so the call may be retried and its diagnostic remains available.
    /// The staging owner is claimed exactly once at the commit cut, then this
    /// owner becomes Failed before object initialization. Every later failure
    /// is terminal and all committed state is retained.
    ///
    /// GetActiveCount provides only a necessary registry-capacity diagnostic:
    /// quiescing occupied slots or retired slots can still cause a terminal
    /// RegistryExhausted result after the commit cut. Boot excludes object
    /// destruction, so those hidden occupied-slot states cannot occur.
    [[nodiscard]] Foundation::Result<Ia64InitialUserImageMemorySummary, Ia64InitialUserImageMemoryFailure>
    Materialize(Ia64InitialUserImageOwner& stagedOwner, Kernel::Object::ObjectRegistry& registry,
                Kernel::Object::RootCapabilityIssuer& issuer,
                Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
                Kernel::Resource::ResourceDomain& resourceDomain) noexcept;

    [[nodiscard]] constexpr Ia64InitialUserImageMemoryOwnerState GetState() const noexcept
    {
        return m_state;
    }

    [[nodiscard]] Foundation::Result<Ia64InitialUserImageMemorySummary, Ia64InitialUserImageMemoryAccessError>
    GetSummary() const noexcept;

    [[nodiscard]] Foundation::Result<Ia64InitialUserImageMemoryRangePlan, Ia64InitialUserImageMemoryAccessError>
    GetRangePlan(Foundation::Size index) const noexcept;

    /// Returns non-owning pointers to the exact embedded objects required by
    /// a later mapping transaction. The returned pointers remain valid only
    /// while this ready owner remains alive and immovable.
    [[nodiscard]] Foundation::Result<Ia64InitialUserImageMemoryRangeView, Ia64InitialUserImageMemoryAccessError>
    BorrowRange(Foundation::Size index) noexcept;

    /// A retryable preflight diagnostic is visible while State remains Fresh;
    /// a terminal diagnostic is visible while State is Failed.
    [[nodiscard]] Foundation::Result<Ia64InitialUserImageMemoryFailure, Ia64InitialUserImageMemoryAccessError>
    GetFailure() const noexcept;

private:
    using MaterializeResult = Foundation::Result<Ia64InitialUserImageMemorySummary, Ia64InitialUserImageMemoryFailure>;

    void ResetPreflightStorage() noexcept;

    [[nodiscard]] Ia64InitialUserImageMemoryOwnerError
    PreflightSource(const Ia64InitialUserImageOwner& stagedOwner) noexcept;
    [[nodiscard, gnu::noinline]]
    Ia64InitialUserImageMemoryOwnerError
    PreflightSegments(const Ia64InitialUserImageOwner& stagedOwner, const Kernel::Memory::PhysicalPageRange& arena,
                      Foundation::UInt64& packedPages, Foundation::Size& rangeCount) noexcept;
    [[nodiscard, gnu::noinline]]
    Ia64InitialUserImageMemoryOwnerError PreflightFixedRanges(const Kernel::Memory::PhysicalPageRange& arena,
                                                              Foundation::UInt64 packedPages,
                                                              Foundation::Size& rangeCount) noexcept;
    [[nodiscard]] Ia64InitialUserImageMemoryOwnerError
    PreflightObjects(Kernel::Object::ObjectRegistry& registry, Kernel::Object::RootCapabilityIssuer& issuer,
                     Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
                     Kernel::Resource::ResourceDomain& resourceDomain) noexcept;
    [[nodiscard]] Ia64InitialUserImageMemoryOwnerError
    InitializeObjects(Kernel::Object::ObjectRegistry& registry,
                      Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
                      Kernel::Resource::ResourceDomain& resourceDomain) noexcept;
    [[nodiscard]] Ia64InitialUserImageMemoryOwnerError
    RegisterObjects(Kernel::Object::ObjectRegistry& registry, Kernel::Object::RootCapabilityIssuer& issuer) noexcept;
    [[nodiscard]] Ia64InitialUserImageMemoryOwnerError RegisterAndVerify(Kernel::Object::ObjectRegistry& registry,
                                                                         Kernel::Object::RootCapabilityIssuer& issuer,
                                                                         Kernel::Object::IKernelObject& object,
                                                                         Kernel::Object::ObjectType expectedType,
                                                                         Kernel::Object::ObjectId& identity) noexcept;
    [[nodiscard]] Ia64InitialUserImageMemoryOwnerError
    VerifyPostconditions(const Ia64InitialUserImageOwner& stagedOwner, Kernel::Object::ObjectRegistry& registry,
                         Kernel::Resource::ResourceDomain& resourceDomain) noexcept;
    [[nodiscard]] bool StagedOwnerIsUnchanged(const Ia64InitialUserImageOwner& stagedOwner,
                                              Ia64InitialUserImageOwnerState expectedState) const noexcept;

    [[nodiscard]] MaterializeResult Fail(Ia64InitialUserImageMemoryOwnerError error) noexcept;

    Kernel::Memory::BootFrame m_aggregateFrame;
    Kernel::Memory::MemoryObject m_memoryObjects[Ia64InitialUserImageMaximumMemoryObjectCount];
    Ia64InitialUserImageMemoryRangePlan m_rangePlans[Ia64InitialUserImageMaximumMemoryObjectCount]{};
    Ia64InitialUserImageSegmentPlan m_stagedSegments[Ia64InitialUserImageMaximumSegmentCount]{};
    Ia64InitialUserImageStageSummary m_stagedSummary{};
    Ia64InitialUserImageMemorySummary m_summary{};
    Ia64InitialUserImageMemoryFailure m_failure{};
    Kernel::Resource::ResourceDomainSnapshot m_initialResourceDomain{};
    Foundation::Size m_initialRegistryObjectCount{0U};
    Foundation::UInt8 m_initializedFrameCount{0U};
    Foundation::UInt8 m_initializedMemoryObjectCount{0U};
    Foundation::UInt8 m_registeredObjectCount{0U};
    bool m_hasFailure{false};
    Ia64InitialUserImageMemoryOwnerState m_state{Ia64InitialUserImageMemoryOwnerState::Fresh};
};

static_assert(Ia64InitialUserImageMaximumMemoryObjectCount == 10U);
static_assert(Ia64InitialUserImageMaximumObjectRegistrationCount == 11U);
static_assert(Ia64InitialUserImageMaximumMaterializedPageCount == 360U);
static_assert(sizeof(Ia64InitialUserImageMemoryOwnerState) == 1U);
static_assert(sizeof(Ia64InitialUserImageMemoryPurpose) == 1U);
static_assert(sizeof(Ia64InitialUserImageGuardPolicy) == 1U);
static_assert(sizeof(Ia64InitialUserImageMemoryOwnerError) == 4U);
static_assert(sizeof(Ia64InitialUserImageMemoryAccessError) == 4U);
static_assert(__is_aggregate(Ia64InitialUserImageMemoryRangePlan));
static_assert(__is_standard_layout(Ia64InitialUserImageMemoryRangePlan));
static_assert(__is_trivially_copyable(Ia64InitialUserImageMemoryRangePlan));
static_assert(sizeof(Ia64InitialUserImageMemoryRangePlan) == 56U);
static_assert(alignof(Ia64InitialUserImageMemoryRangePlan) == 8U);
static_assert(__is_aggregate(Ia64InitialUserImageMemoryRangeView));
static_assert(__is_standard_layout(Ia64InitialUserImageMemoryRangeView));
static_assert(__is_trivially_copyable(Ia64InitialUserImageMemoryRangeView));
static_assert(sizeof(Ia64InitialUserImageMemoryRangeView) == 72U);
static_assert(alignof(Ia64InitialUserImageMemoryRangeView) == 8U);
static_assert(__is_aggregate(Ia64InitialUserImageMemorySummary));
static_assert(__is_standard_layout(Ia64InitialUserImageMemorySummary));
static_assert(__is_trivially_copyable(Ia64InitialUserImageMemorySummary));
static_assert(sizeof(Ia64InitialUserImageMemorySummary) == 104U);
static_assert(alignof(Ia64InitialUserImageMemorySummary) == 8U);
static_assert(__is_aggregate(Ia64InitialUserImageMemoryFailure));
static_assert(__is_standard_layout(Ia64InitialUserImageMemoryFailure));
static_assert(__is_trivially_copyable(Ia64InitialUserImageMemoryFailure));
static_assert(sizeof(Ia64InitialUserImageMemoryFailure) == 40U);
static_assert(alignof(Ia64InitialUserImageMemoryFailure) == 8U);
static_assert(alignof(Ia64InitialUserImageMemoryOwner) == 16U);
} // namespace Asteria::Platform::Ia64::Executable

#endif // ASTERIA_PLATFORM_IA64_EXECUTABLE_IA64_INITIAL_USER_IMAGE_MEMORY_OWNER_HPP_INCLUDED
