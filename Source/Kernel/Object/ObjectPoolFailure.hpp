// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_OBJECT_OBJECT_POOL_FAILURE_HPP_INCLUDED
#define ASTERIA_KERNEL_OBJECT_OBJECT_POOL_FAILURE_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Object/CapabilityTypes.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"

namespace Asteria::Kernel::Object
{
/// Why a pool of user-creatable objects refused a creation or destruction.
enum class ObjectPoolError : Foundation::UInt8
{
    None,
    /// The request names no ready capability space of the pool's registry.
    InvalidConfiguration,
    /// The object rejects the requested parameters.
    InvalidRequest,
    /// The authorizing or destroyed capability did not resolve; Capability
    /// says why.
    CapabilityRejected,
    /// The ResourceDomain could not be pinned for the charge.
    ResourceDomainRejected,
    /// The ResourceDomain refused the charge; Resource says why.
    ChargeRejected,
    /// The pool, or the memory it draws from, is exhausted.
    CapacityExceeded,
    RegistrationRejected,
    /// The mint under authority failed; Capability says why. The creation
    /// rolled back completely.
    MintRejected,
    /// The capability names an object this pool does not own.
    NotOwned,
    DestroyRejected,
    /// The object is not in a state the operation accepts yet, such as a
    /// thread that has not exited at its ObjectDestroy; retryable.
    Busy,
    /// A rollback or reclamation step failed; the pool is Faulted.
    InvariantViolation,
    Faulted,
};

struct ObjectPoolFailure
{
    ObjectPoolError Error;
    CapabilityError Capability;
    Resource::ResourceDomainError Resource;
    ObjectRegistryError Registry;
    /// The object type's own error, such as an EndpointErrorCode.
    Foundation::UInt8 Detail;
    Foundation::UInt8 Reserved[3];
};

[[nodiscard]] constexpr ObjectPoolFailure
MakeObjectPoolFailure(const ObjectPoolError error, const CapabilityError capability = CapabilityError::Ok,
                      const Resource::ResourceDomainError resource = Resource::ResourceDomainError::None,
                      const ObjectRegistryError registry = ObjectRegistryError::Ok,
                      const Foundation::UInt8 detail = 0U) noexcept
{
    return ObjectPoolFailure{
        .Error = error,
        .Capability = capability,
        .Resource = resource,
        .Registry = registry,
        .Detail = detail,
        .Reserved = {0U, 0U, 0U},
    };
}

static_assert(sizeof(ObjectPoolError) == 1U);
static_assert(sizeof(ObjectPoolFailure) == 8U);
static_assert(alignof(ObjectPoolFailure) == 1U);
} // Namespace Asteria::Kernel::Object

#endif // ASTERIA_KERNEL_OBJECT_OBJECT_POOL_FAILURE_HPP_INCLUDED
