// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_SCHEDULER_IA64_INITIAL_USER_IMAGE_LAUNCH_OWNER_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_SCHEDULER_IA64_INITIAL_USER_IMAGE_LAUNCH_OWNER_HPP_INCLUDED

#include "Foundation/Compiler.hpp"
#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Object/CapabilitySpace.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Kernel/Object/RevocationRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Protection/ProtectionDomain.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Platform/Ia64/Executable/Ia64InitialUserImageMappingOwner.hpp"
#include "Platform/Ia64/Scheduler/Ia64UserModeLaunch.hpp"

namespace Asteria::Platform::Ia64::Scheduler
{
enum class Ia64InitialUserImageLaunchOwnerState : Foundation::UInt8
{
    Fresh,
    Failed,
    Ready,
};

enum class Ia64InitialUserImageLaunchOwnerError : Foundation::UInt32
{
    None,
    AlreadyAttempted,
    MemoryOwnerNotReady,
    MemoryOwnerAccessRejected,
    MappingOwnerNotReady,
    MappingOwnerAccessRejected,
    InvalidImplementedAddressWidth,
    InvalidMemorySummary,
    InvalidMappingSummary,
    OwnerSummaryMismatch,
    InvalidExpectedAddressSpace,
    IssuerRegistryMismatch,
    RevocationRegistryRejected,
    CapabilitySpaceRejected,
    ProtectionDomainRejected,
    ResourceDomainRejected,
    InvalidBootstrapGrant,
    InvalidDiagnosticGrant,
    BootstrapTargetRejected,
    DiagnosticTargetRejected,
    RegistryBindingRejected,
    AddressSpaceRejected,
    RangePlanMismatch,
    InvalidRange,
    InvalidPermissions,
    InvalidMappingReceipt,
    InvalidMappingRecord,
    GuardRangeOccupied,
    EntryMappingRejected,
    GlobalPointerMappingRejected,
    MemoryStackMappingRejected,
    RseBackingStoreMappingRejected,
    LaunchRequestRejected,
    AuthoritativeStateChanged,
    RevocationDomainCreationRejected,
    CapabilityMintRejected,
    CapabilityQueryRejected,
    DiagnosticRevocationDomainCreationRejected,
    DiagnosticCapabilityMintRejected,
    DiagnosticCapabilityQueryRejected,
    PostconditionRejected,
};

enum class Ia64InitialUserImageLaunchAccessError : Foundation::UInt32
{
    None,
    NotReady,
    NoFailure,
};

/// Immutable, caller-supplied authority description for one capability passed
/// to the initial user image: the bootstrap capability (r4/r5) or the
/// DiagnosticChannel capability (r6/r7). Registry is non-owning and must name
/// the same registry supplied to Prepare. The target object and registry
/// outlive this owner and the capability minted from the grant.
struct Ia64InitialUserImageBootstrapGrant
{
    const Kernel::Object::ObjectRegistry* Registry;
    Kernel::Object::ObjectId Target;
    Kernel::Object::ObjectType ExpectedType;
    Kernel::Object::CapabilityRights Rights;
    Kernel::Object::MonotonicTime LeaseDeadline;
    Foundation::UInt64 SealedObjectSelector;
};

struct Ia64InitialUserImageLaunchSummary
{
    Kernel::Memory::AddressSpaceId AddressSpaceIdentity;
    Kernel::Protection::ProtectionDomainId ProtectionDomainIdentity;
    Kernel::Resource::ResourceDomainObjectId ResourceDomainIdentity;
    Kernel::Object::CapabilitySpaceIdentifier CapabilitySpaceIdentity;
    Kernel::Object::CapabilityHandle BootstrapCapability;
    Kernel::Object::RevocationDomainId RevocationDomain;
    Kernel::Object::CapabilityHandle DiagnosticCapability;
    Kernel::Object::RevocationDomainId DiagnosticRevocationDomain;
    Foundation::Size RangeCount;
    Foundation::Size EntryMappingIndex;
    Foundation::Size GlobalPointerMappingIndex;
    Foundation::Size MemoryStackMappingIndex;
    Foundation::Size RseBackingStoreMappingIndex;
    Foundation::UInt64 FirstMappingSequence;
    Foundation::UInt64 LastMappingSequence;
};

struct Ia64InitialUserImageLaunchFailure
{
    Ia64InitialUserImageLaunchOwnerError Error;
    Executable::Ia64InitialUserImageMemoryAccessError MemoryAccess;
    Executable::Ia64InitialUserImageMappingAccessError MappingAccess;
    Kernel::Object::ObjectPinError Pin;
    Kernel::Protection::ProtectionDomainError ProtectionDomain;
    Kernel::Object::RevocationError Revocation;
    Kernel::Object::CapabilityError Capability;
    Ia64UserModeLaunchError Launch;
    Foundation::Size RangeIndex;
};

/// Owns the final allocation-free authority and launch record for the staged IA-64 initial user
/// image. Prepare is externally serialized on the non-preemptible BSP. Both image owners, the
/// complete runtime identity graph, the private AddressSpace, and this owner remain alive and
/// immovable through launch and supervisor resume. No collaborator lock may be held by the caller.
/// Complete image, mapping, guard, identity, and launch-record preflight occurs before authority
/// changes. A rejection in that phase leaves this owner Fresh and retryable. Immediately before
/// creating the first revocation root this owner becomes Failed. Every later rejection is terminal
/// and retains any issued revocation domain or capability. Both revocation roots are created before
/// either capability is minted. Success retains exactly two capabilities, each in its own new
/// revocation root: the one described by the bootstrap grant in initial r4/r5 and the one described
/// by the diagnostic grant in initial r6/r7, plus a fresh embedded supervisor continuation. The two
/// grants name distinct objects, and each grant must include Inspect so its complete descriptor can
/// be verified. The ProtectionDomain remains the execution identity and is independent from the
/// grant targets. Prepare never changes mapping, object-registry, resource, or scheduler state.
class alignas(16U) Ia64InitialUserImageLaunchOwner final
{
public:
    constexpr Ia64InitialUserImageLaunchOwner() noexcept = default;
    ~Ia64InitialUserImageLaunchOwner() noexcept = default;

    Ia64InitialUserImageLaunchOwner(const Ia64InitialUserImageLaunchOwner&) = delete;
    Ia64InitialUserImageLaunchOwner& operator=(const Ia64InitialUserImageLaunchOwner&) = delete;
    Ia64InitialUserImageLaunchOwner(Ia64InitialUserImageLaunchOwner&&) = delete;
    Ia64InitialUserImageLaunchOwner& operator=(Ia64InitialUserImageLaunchOwner&&) = delete;

    [[nodiscard]] Foundation::Result<Ia64InitialUserImageLaunchSummary, Ia64InitialUserImageLaunchFailure>
    Prepare(Executable::Ia64InitialUserImageMemoryOwner& memoryOwner,
            const Executable::Ia64InitialUserImageMappingOwner& mappingOwner,
            Kernel::Memory::AddressSpaceId expectedAddressSpaceIdentity, Kernel::Object::ObjectRegistry& registry,
            Kernel::Object::RootCapabilityIssuer& issuer, Kernel::Object::RevocationRegistry& revocationRegistry,
            Kernel::Object::CapabilitySpace& capabilitySpace, const Ia64InitialUserImageBootstrapGrant& bootstrapGrant,
            const Ia64InitialUserImageBootstrapGrant& diagnosticGrant,
            Kernel::Protection::ProtectionDomainId protectionDomainIdentity,
            Kernel::Protection::ProtectionDomain& protectionDomain,
            Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
            Kernel::Resource::ResourceDomain& resourceDomain, Ia64UserModeVirtualRange supervisorMemoryStack,
            Ia64UserModeVirtualRange supervisorRseBackingStore,
            Foundation::UInt8 implementedVirtualAddressMostSignificantBit) noexcept;

    [[nodiscard]] constexpr Ia64InitialUserImageLaunchOwnerState GetState() const noexcept
    {
        return m_state;
    }

    [[nodiscard]] Foundation::Result<Ia64InitialUserImageLaunchSummary, Ia64InitialUserImageLaunchAccessError>
    GetSummary() const noexcept;

    [[nodiscard]] Foundation::Result<Ia64UserModeLaunchRequest, Ia64InitialUserImageLaunchAccessError>
    GetLaunchRequest() const noexcept;

    /// A retryable preflight diagnostic is visible while State remains Fresh;
    /// a terminal diagnostic is visible while State is Failed.
    [[nodiscard]] Foundation::Result<Ia64InitialUserImageLaunchFailure, Ia64InitialUserImageLaunchAccessError>
    GetFailure() const noexcept;

private:
    using PrepareResult = Foundation::Result<Ia64InitialUserImageLaunchSummary, Ia64InitialUserImageLaunchFailure>;

    void ResetPreflightStorage() noexcept;

    [[nodiscard]] Ia64InitialUserImageLaunchOwnerError
    CaptureOwners(Executable::Ia64InitialUserImageMemoryOwner& memoryOwner,
                  const Executable::Ia64InitialUserImageMappingOwner& mappingOwner) noexcept;
    [[nodiscard]] Ia64InitialUserImageLaunchOwnerError CaptureRuntime(
        Kernel::Memory::AddressSpaceId expectedAddressSpaceIdentity, Kernel::Object::ObjectRegistry& registry,
        Kernel::Object::RootCapabilityIssuer& issuer, Kernel::Object::RevocationRegistry& revocationRegistry,
        Kernel::Object::CapabilitySpace& capabilitySpace, const Ia64InitialUserImageBootstrapGrant& bootstrapGrant,
        const Ia64InitialUserImageBootstrapGrant& diagnosticGrant,
        Kernel::Protection::ProtectionDomainId protectionDomainIdentity,
        Kernel::Protection::ProtectionDomain& protectionDomain,
        Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
        Kernel::Resource::ResourceDomain& resourceDomain) noexcept;
    [[nodiscard]] Ia64InitialUserImageLaunchOwnerError
    ValidateMappings(Executable::Ia64InitialUserImageMemoryOwner& memoryOwner,
                     const Executable::Ia64InitialUserImageMappingOwner& mappingOwner) noexcept;
    [[nodiscard]] Ia64InitialUserImageLaunchOwnerError
    ValidateMapping(Executable::Ia64InitialUserImageMemoryOwner& memoryOwner,
                    const Executable::Ia64InitialUserImageMappingOwner& mappingOwner, Foundation::Size index) noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64InitialUserImageLaunchOwnerError
    ValidateMappingStorage(Foundation::Size index) noexcept;
    [[nodiscard]] Ia64InitialUserImageLaunchOwnerError SelectLaunchRanges() noexcept;
    [[nodiscard]] Ia64InitialUserImageLaunchOwnerError ValidateGuards() noexcept;
    [[nodiscard]] Ia64InitialUserImageLaunchOwnerError
    BuildLaunchRequest(Ia64UserModeVirtualRange supervisorMemoryStack,
                       Ia64UserModeVirtualRange supervisorRseBackingStore,
                       Foundation::UInt8 implementedVirtualAddressMostSignificantBit) noexcept;
    [[nodiscard]] Ia64InitialUserImageLaunchOwnerError RevalidatePreflight(
        Executable::Ia64InitialUserImageMemoryOwner& memoryOwner,
        const Executable::Ia64InitialUserImageMappingOwner& mappingOwner, Kernel::Object::ObjectRegistry& registry,
        Kernel::Object::RootCapabilityIssuer& issuer, Kernel::Object::RevocationRegistry& revocationRegistry,
        Kernel::Object::CapabilitySpace& capabilitySpace, Kernel::Protection::ProtectionDomain& protectionDomain,
        Kernel::Resource::ResourceDomain& resourceDomain) noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64InitialUserImageLaunchOwnerError
    RevalidateMemoryOwner(Executable::Ia64InitialUserImageMemoryOwner& memoryOwner) noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64InitialUserImageLaunchOwnerError
    RevalidateMappingOwner(const Executable::Ia64InitialUserImageMappingOwner& mappingOwner) noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64InitialUserImageLaunchOwnerError
    RevalidateProtectionDomain(Kernel::Protection::ProtectionDomain& protectionDomain) noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64InitialUserImageLaunchOwnerError RevalidateRuntimeState(
        Kernel::Object::ObjectRegistry& registry, Kernel::Object::RootCapabilityIssuer& issuer,
        Kernel::Object::RevocationRegistry& revocationRegistry, Kernel::Object::CapabilitySpace& capabilitySpace,
        Kernel::Resource::ResourceDomain& resourceDomain) noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64InitialUserImageLaunchOwnerError
    RevalidateRanges(Executable::Ia64InitialUserImageMemoryOwner& memoryOwner,
                     const Executable::Ia64InitialUserImageMappingOwner& mappingOwner) noexcept;
    [[nodiscard]] Ia64InitialUserImageLaunchOwnerError
    CreateRevocationDomains(Kernel::Object::RevocationRegistry& revocationRegistry) noexcept;
    [[nodiscard]] Ia64InitialUserImageLaunchOwnerError
    CreateCapability(Kernel::Object::RootCapabilityIssuer& issuer,
                     Kernel::Object::CapabilitySpace& capabilitySpace) noexcept;
    [[nodiscard]] Ia64InitialUserImageLaunchOwnerError
    CreateDiagnosticCapability(Kernel::Object::RootCapabilityIssuer& issuer,
                               Kernel::Object::CapabilitySpace& capabilitySpace) noexcept;
    [[nodiscard]] Ia64InitialUserImageLaunchOwnerError VerifyPostconditions(
        Executable::Ia64InitialUserImageMemoryOwner& memoryOwner,
        const Executable::Ia64InitialUserImageMappingOwner& mappingOwner, Kernel::Object::ObjectRegistry& registry,
        Kernel::Object::RootCapabilityIssuer& issuer, Kernel::Object::RevocationRegistry& revocationRegistry,
        Kernel::Object::CapabilitySpace& capabilitySpace, Kernel::Protection::ProtectionDomain& protectionDomain,
        Kernel::Resource::ResourceDomain& resourceDomain) noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64InitialUserImageLaunchOwnerError
    VerifyMemoryOwnerPostconditions(Executable::Ia64InitialUserImageMemoryOwner& memoryOwner) noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64InitialUserImageLaunchOwnerError
    VerifyMappingOwnerPostconditions(const Executable::Ia64InitialUserImageMappingOwner& mappingOwner) noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64InitialUserImageLaunchOwnerError
    VerifyCapabilityPostconditions(Kernel::Object::RevocationRegistry& revocationRegistry,
                                   Kernel::Object::CapabilitySpace& capabilitySpace) noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64InitialUserImageLaunchOwnerError
    VerifyRuntimePostconditions(Kernel::Object::ObjectRegistry& registry, Kernel::Object::RootCapabilityIssuer& issuer,
                                Kernel::Protection::ProtectionDomain& protectionDomain,
                                Kernel::Resource::ResourceDomain& resourceDomain) noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64InitialUserImageLaunchOwnerError VerifyLaunchPostconditions() noexcept;

    [[nodiscard]] PrepareResult Fail(Ia64InitialUserImageLaunchOwnerError error) noexcept;

    Executable::Ia64InitialUserImageInstalledMapping
        m_mappings[Executable::Ia64InitialUserImageMaximumMemoryObjectCount]{};
    Executable::Ia64InitialUserImageMemoryRangeView
        m_ranges[Executable::Ia64InitialUserImageMaximumMemoryObjectCount]{};
    Executable::Ia64InitialUserImageMemorySummary m_memorySummary{};
    Executable::Ia64InitialUserImageMappingSummary m_mappingSummary{};
    Kernel::Protection::ProtectionDomainSnapshot m_protectionSnapshot{};
    Kernel::Protection::ProtectionDomainBinding m_protectionBinding{};
    Kernel::Resource::ResourceDomainSnapshot m_resourceSnapshot{};
    Kernel::Memory::AddressSpaceSnapshot m_addressSpaceSnapshot{};
    Ia64InitialUserImageBootstrapGrant m_bootstrapGrant{};
    Ia64InitialUserImageBootstrapGrant m_diagnosticGrant{};
    Ia64InitialUserImageLaunchSummary m_summary{};
    Ia64InitialUserImageLaunchFailure m_failure{};
    alignas(16U) Ia64UserModeSupervisorContinuation m_supervisorContinuation{};
    Ia64UserModeLaunchRequest m_launchRequest{};
    Kernel::Object::ObjectRegistry* m_registry{nullptr};
    Kernel::Object::CapabilitySpace* m_capabilitySpace{nullptr};
    Kernel::Protection::ProtectionDomain* m_protectionDomain{nullptr};
    Kernel::Resource::ResourceDomain* m_resourceDomain{nullptr};
    Kernel::Memory::AddressSpace* m_addressSpace{nullptr};
    Kernel::Object::ObjectPin m_bootstrapTargetPin{};
    Kernel::Object::ObjectPin m_diagnosticTargetPin{};
    Foundation::Size m_initialRegistryObjectCount{0U};
    Foundation::Size m_initialCapabilityCount{0U};
    bool m_hasFailure{false};
    Ia64InitialUserImageLaunchOwnerState m_state{Ia64InitialUserImageLaunchOwnerState::Fresh};
};

static_assert(sizeof(Ia64InitialUserImageLaunchOwnerState) == 1U);
static_assert(sizeof(Ia64InitialUserImageLaunchOwnerError) == 4U);
static_assert(sizeof(Ia64InitialUserImageLaunchAccessError) == 4U);
static_assert(__is_aggregate(Ia64InitialUserImageBootstrapGrant));
static_assert(__is_standard_layout(Ia64InitialUserImageBootstrapGrant));
static_assert(__is_trivially_copyable(Ia64InitialUserImageBootstrapGrant));
static_assert(__is_aggregate(Ia64InitialUserImageLaunchSummary));
static_assert(__is_standard_layout(Ia64InitialUserImageLaunchSummary));
static_assert(__is_trivially_copyable(Ia64InitialUserImageLaunchSummary));
static_assert(__is_aggregate(Ia64InitialUserImageLaunchFailure));
static_assert(__is_standard_layout(Ia64InitialUserImageLaunchFailure));
static_assert(__is_trivially_copyable(Ia64InitialUserImageLaunchFailure));
static_assert(alignof(Ia64InitialUserImageLaunchOwner) == 16U);
} // Namespace Asteria::Platform::Ia64::Scheduler

#endif // ASTERIA_PLATFORM_IA64_SCHEDULER_IA64_INITIAL_USER_IMAGE_LAUNCH_OWNER_HPP_INCLUDED
