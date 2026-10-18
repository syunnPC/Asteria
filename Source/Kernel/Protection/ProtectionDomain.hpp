// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_PROTECTION_PROTECTION_DOMAIN_HPP_INCLUDED
#define ASTERIA_KERNEL_PROTECTION_PROTECTION_DOMAIN_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Memory/FaultToken.hpp"
#include "Kernel/Object/CapabilitySpace.hpp"
#include "Kernel/Object/IKernelObject.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Device
{
class PciFunction;
}

namespace Asteria::Kernel::Memory
{
class AddressSpace;
}

namespace Asteria::Kernel::Protection
{
/// Identifies one ProtectionDomain generation within one ObjectRegistry.
/// The originating registry outlives every copy and use of this value.
class ProtectionDomainId final
{
public:
    constexpr ProtectionDomainId() noexcept = default;

    constexpr ProtectionDomainId(const Object::ObjectRegistry& registry, const Object::ObjectId object) noexcept
        : m_registry{&registry}, m_object{object}
    {
    }

    [[nodiscard]] constexpr Foundation::UInt64 GetValue() const noexcept
    {
        return m_object.GetValue();
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return (m_registry != nullptr) && m_object.IsValid();
    }

    [[nodiscard]] constexpr Object::ObjectId GetObjectId() const noexcept
    {
        return m_object;
    }

    [[nodiscard]] constexpr bool IsFromRegistry(const Object::ObjectRegistry& registry) const noexcept
    {
        return m_registry == &registry;
    }

    [[nodiscard]] constexpr bool IsEqualTo(const ProtectionDomainId other) const noexcept
    {
        return (m_registry == other.m_registry) && (m_object.GetValue() == other.m_object.GetValue());
    }

private:
    const Object::ObjectRegistry* m_registry{nullptr};
    Object::ObjectId m_object{};
};

enum class ProtectionDomainState : Foundation::UInt8
{
    Uninitialized,
    Unbound,
    Ready,
    Quiesced,
};

enum class ProtectionDomainKind : Foundation::UInt8
{
    Uninitialized,
    UserCapable,
    KernelOnly,
};

enum class ProtectionDomainError : Foundation::UInt8
{
    None,
    InvalidConfiguration,
    AlreadyInitialized,
    NotInitialized,
    AlreadyBound,
    InvalidIdentity,
    RegistryMismatch,
    AddressSpacePinRejected,
    ResourceDomainPinRejected,
    ResourceDomainInstanceMismatch,
    ResourceDomainRejected,
    ObjectInstanceMismatch,
    NotReady,
    Quiesced,
    InUse,
    DeviceAssignmentReferenceExhausted,
    DeviceAssignmentReferenceUnderflow,
};

struct ProtectionDomainFailure
{
    ProtectionDomainError Error;
    Object::ObjectPinError Pin;
    Resource::ResourceDomainError Resource;
    Foundation::UInt8 Reserved[5];
};

struct ProtectionDomainBinding
{
    ProtectionDomainId Identity;
    Memory::AddressSpaceId AddressSpace;
    Memory::AddressSpace* AddressSpaceStorage;
    Object::CapabilitySpace* Capabilities;
    Object::CapabilitySpaceIdentifier CapabilitySpace;
    Resource::ResourceDomainObjectId ResourceDomainIdentity;
    Resource::ResourceDomain* ResourceDomainStorage;
    Object::ResourceDomainId ResourceDomain;
};

struct ProtectionDomainSnapshot
{
    ProtectionDomainId Identity;
    Memory::AddressSpaceId AddressSpace;
    Object::CapabilitySpaceIdentifier CapabilitySpace;
    Resource::ResourceDomainObjectId ResourceDomainIdentity;
    Object::ResourceDomainId ResourceDomain;
    ProtectionDomainKind Kind;
    ProtectionDomainState State;
    Foundation::UInt8 Reserved[6];
};

/// Owns one immutable association between an AddressSpace generation and one
/// resource domain. A user-capable association also names one capability-space
/// instance; an explicitly kernel-only association has no CapabilitySpace and
/// cannot provide user syscall authority.
///
/// The AddressSpace is retained with a generation-safe pin. When present, the
/// CapabilitySpace is immovable and identified by its boot-unique identifier;
/// its owner keeps it alive through Quiesce. The association grants no
/// capability authority by itself. A caller must already be authorized before
/// installing or consuming the relationship. PciFunction nests this domain's
/// lock below its own while retaining or releasing an internal
/// device-assignment reference; this domain never calls back into a
/// PciFunction.
class ProtectionDomain final : public Object::IKernelObject
{
public:
    /// Immutable after publication; terminal diagnostics read it without locks.
    void SetDiagnosticComponentKind(Foundation::UInt64 kind) noexcept
    {
        m_diagnosticComponentKind = kind;
    }
    [[nodiscard]] Foundation::UInt64 DiagnosticComponentKind() const noexcept
    {
        return m_diagnosticComponentKind;
    }

    constexpr ProtectionDomain() noexcept = default;

    /// Preconditions:
    /// - State is Uninitialized or Quiesced;
    /// - no binding view or operation remains live.
    ~ProtectionDomain() noexcept = default;

    ProtectionDomain(const ProtectionDomain&) = delete;
    ProtectionDomain& operator=(const ProtectionDomain&) = delete;
    ProtectionDomain(ProtectionDomain&&) = delete;
    ProtectionDomain& operator=(ProtectionDomain&&) = delete;

    /// Installs the immutable association before this object is registered.
    /// If initialization begins in State Uninitialized, failure leaves State
    /// Uninitialized and retains no object pin.
    [[nodiscard]] ProtectionDomainFailure Initialize(Object::ObjectRegistry& registry,
                                                     Resource::ResourceDomainObjectId resourceDomainIdentity,
                                                     Resource::ResourceDomain& resourceDomain,
                                                     Memory::AddressSpaceId addressSpace,
                                                     Object::CapabilitySpace& capabilities) noexcept;

    /// Installs a kernel-only association before this object is registered.
    /// The resulting binding retains the AddressSpace and ResourceDomain but
    /// deliberately contains no CapabilitySpace or capability identifier.
    /// If initialization begins in State Uninitialized, failure leaves State
    /// Uninitialized and retains no object pin.
    [[nodiscard]] ProtectionDomainFailure InitializeKernelOnly(Object::ObjectRegistry& registry,
                                                               Resource::ResourceDomainObjectId resourceDomainIdentity,
                                                               Resource::ResourceDomain& resourceDomain,
                                                               Memory::AddressSpaceId addressSpace) noexcept;

    /// Binds the exact identity returned by ObjectRegistry registration.
    [[nodiscard]] ProtectionDomainId GetIdentity() const noexcept
    {
        return GetSnapshot().Identity;
    }

    [[nodiscard]] ProtectionDomainFailure BindIdentity(Object::ObjectRegistry& registry,
                                                       ProtectionDomainId identity) noexcept;

    /// Returns a stable view while the caller retains this ProtectionDomain
    /// and prevents Quiesce until it has finished using the referenced space.
    [[nodiscard]] Foundation::Result<ProtectionDomainBinding, ProtectionDomainError> GetBinding() const noexcept;

    /// Releases the AddressSpace pin after every consumer stopped using the
    /// binding. Quiesced is terminal for this storage instance.
    [[nodiscard]] ProtectionDomainError Quiesce() noexcept;

    [[nodiscard]] ProtectionDomainSnapshot GetSnapshot() const noexcept;
    [[nodiscard]] Object::ObjectType GetObjectType() const noexcept override;
    [[nodiscard]] Object::ResourceDomainId GetChargeOwner() const noexcept override;

private:
    Foundation::UInt64 m_diagnosticComponentKind{};
    friend class Device::PciFunction;

    [[nodiscard]] static ProtectionDomainFailure
    MakeFailure(ProtectionDomainError error, Object::ObjectPinError pin = Object::ObjectPinError::Ok,
                Resource::ResourceDomainError resource = Resource::ResourceDomainError::None) noexcept;
    [[nodiscard]] static constexpr Resource::ResourceReservation GetResourceReservation() noexcept;
    [[nodiscard]] ProtectionDomainFailure
    InitializeAssociation(Object::ObjectRegistry& registry, Resource::ResourceDomainObjectId resourceDomainIdentity,
                          Resource::ResourceDomain& resourceDomain, Memory::AddressSpaceId addressSpace,
                          Object::CapabilitySpace* capabilities, ProtectionDomainKind kind) noexcept;
    [[nodiscard]] bool IsCapabilityBindingValid() const noexcept;
    [[nodiscard]] ProtectionDomainError RetainDeviceAssignment() noexcept;
    [[nodiscard]] ProtectionDomainError ReleaseDeviceAssignment() noexcept;

    mutable Platform::Synchronization::SpinLock m_synchronization;
    Platform::Synchronization::AtomicUInt64 m_chargeOwnerValue;
    Object::ObjectRegistry* m_registry{nullptr};
    Object::ObjectPin m_addressSpacePin;
    Object::ObjectPin m_resourceDomainPin;
    Memory::AddressSpace* m_addressSpaceStorage{nullptr};
    Object::CapabilitySpace* m_capabilities{nullptr};
    Resource::ResourceDomain* m_resourceDomain{nullptr};
    ProtectionDomainId m_identity{};
    Memory::AddressSpaceId m_addressSpace{};
    Object::CapabilitySpaceIdentifier m_capabilitySpace{};
    Resource::ResourceDomainObjectId m_resourceDomainIdentity{};
    Object::ResourceDomainId m_resourceDomainIdentifier{};
    Foundation::UInt64 m_activeDeviceAssignments{0U};
    ProtectionDomainKind m_kind{ProtectionDomainKind::Uninitialized};
    ProtectionDomainState m_state{ProtectionDomainState::Uninitialized};
};

static_assert(sizeof(ProtectionDomainId) == 16U);
static_assert(alignof(ProtectionDomainId) == 8U);
static_assert(sizeof(ProtectionDomainKind) == 1U);
static_assert(sizeof(ProtectionDomainState) == 1U);
static_assert(sizeof(ProtectionDomainError) == 1U);
static_assert(sizeof(ProtectionDomainFailure) == 8U);
static_assert(alignof(ProtectionDomainFailure) == 1U);
static_assert(sizeof(ProtectionDomainBinding) == 88U);
static_assert(alignof(ProtectionDomainBinding) == 8U);
static_assert(sizeof(ProtectionDomainSnapshot) == 72U);
static_assert(alignof(ProtectionDomainSnapshot) == 8U);
} // namespace Asteria::Kernel::Protection

#endif // ASTERIA_KERNEL_PROTECTION_PROTECTION_DOMAIN_HPP_INCLUDED
