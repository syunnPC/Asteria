// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_OBJECT_ROOT_CAPABILITY_ISSUER_HPP_INCLUDED
#define ASTERIA_KERNEL_OBJECT_ROOT_CAPABILITY_ISSUER_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Object/CapabilityTypes.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"

namespace Asteria::Kernel::Object
{
class CapabilitySpace;

/// Owns the privileged object-registration and initial-capability minting boundary.
/// This object is retained only by the kernel bootstrap path and is never exposed through a syscall.
/// The referenced object registry must outlive this issuer and every capability minted through it.
/// Callers prevent same-CPU preemption or reentry by code using the shared registry lock.
class RootCapabilityIssuer final
{
public:
    explicit constexpr RootCapabilityIssuer(ObjectRegistry& objectRegistry) noexcept : m_objectRegistry{objectRegistry}
    {
    }

    RootCapabilityIssuer(const RootCapabilityIssuer&) = delete;
    RootCapabilityIssuer& operator=(const RootCapabilityIssuer&) = delete;
    RootCapabilityIssuer(RootCapabilityIssuer&&) = delete;
    RootCapabilityIssuer& operator=(RootCapabilityIssuer&&) = delete;

    /// Non-mutating identity query. The registry reference is immutable, so
    /// this does not acquire the registry lock or inspect registry state.
    [[nodiscard]] constexpr bool IsFromRegistry(const ObjectRegistry& objectRegistry) const noexcept
    {
        return &m_objectRegistry == &objectRegistry;
    }

    /// Registers externally owned, immovable object storage and reads its immutable metadata.
    /// Preconditions:
    /// - instance is not registered in any object registry.
    /// - instance remains alive until CollectDestroyedObject returns it.
    [[nodiscard]] Foundation::Result<ObjectId, ObjectRegistryError> CreateObject(IKernelObject& instance) noexcept;

    /// Starts asynchronous destruction. New lookups and pins fail immediately, while existing
    /// leases and pins remain valid and the registry slot cannot be reused.
    [[nodiscard]] ObjectRegistryError DestroyObject(ObjectId object) noexcept;

    /// Returns one quiescing object after its final lease or persistent pin has released.
    /// The owner may destroy or reuse Instance only after receiving this record.
    [[nodiscard]] Foundation::Result<ObjectDestructionRecord, ObjectRegistryError> CollectDestroyedObject() noexcept;

    /// Returns exactly object once it is quiescing and its final lease or pin
    /// has released, so owners of destroyable storage never receive another
    /// owner's record. An identity that names no quiescing entry of that exact
    /// generation is an InvalidObject; one still referenced is
    /// NoDestructionReady and stays quiescing.
    [[nodiscard]] Foundation::Result<ObjectDestructionRecord, ObjectRegistryError>
    CollectDestroyedObject(ObjectId object) noexcept;

    /// Mints an initial capability into a space using a domain issued by that space's
    /// revocation registry.
    [[nodiscard]] Foundation::Result<CapabilityHandle, CapabilityError>
    Mint(CapabilitySpace& destination, ObjectId object, CapabilityRights rights, RevocationDomainId revocationDomain,
         MonotonicTime leaseDeadline, Foundation::UInt64 sealedObjectSelector) noexcept;

    /// Mints object into destination under authority, a capability in that space that must
    /// authorize operation on exactly authorityObject of authorityType in the same critical section
    /// as the insertion. The new capability takes authority's revocation domain and lease deadline
    /// and a zero sealed selector, so revoking or expiring authority also ends it.
    [[nodiscard]] Foundation::Result<CapabilityHandle, CapabilityError>
    MintUnderAuthority(CapabilitySpace& destination, ObjectId object, CapabilityRights rights,
                       CapabilityHandle authority, ObjectType authorityType, CapabilityOperation operation,
                       const IKernelObject& authorityObject, Foundation::UInt64 sealedObjectSelector = 0U) noexcept;

private:
    ObjectRegistry& m_objectRegistry;
};
} // Namespace Asteria::Kernel::Object

#endif // ASTERIA_KERNEL_OBJECT_ROOT_CAPABILITY_ISSUER_HPP_INCLUDED
