// SPDX-License-Identifier: Apache-2.0

#include "RootCapabilityIssuer.hpp"

#include "Kernel/Object/CapabilitySpace.hpp"

namespace Asteria::Kernel::Object
{
Foundation::Result<ObjectId, ObjectRegistryError> RootCapabilityIssuer::CreateObject(IKernelObject& instance) noexcept
{
    return m_objectRegistry.RegisterObject(instance);
}

ObjectRegistryError RootCapabilityIssuer::DestroyObject(const ObjectId object) noexcept
{
    return m_objectRegistry.DestroyObject(object);
}

Foundation::Result<ObjectDestructionRecord, ObjectRegistryError> RootCapabilityIssuer::CollectDestroyedObject() noexcept
{
    return m_objectRegistry.CollectDestroyedObject();
}

Foundation::Result<ObjectDestructionRecord, ObjectRegistryError>
RootCapabilityIssuer::CollectDestroyedObject(const ObjectId object) noexcept
{
    return m_objectRegistry.CollectDestroyedObject(object);
}

Foundation::Result<CapabilityHandle, CapabilityError>
RootCapabilityIssuer::Mint(CapabilitySpace& destination, const ObjectId object, const CapabilityRights rights,
                           const RevocationDomainId revocationDomain, const MonotonicTime leaseDeadline,
                           const Foundation::UInt64 sealedObjectSelector) noexcept
{
    return destination.MintFromRootIssuer(m_objectRegistry, object, rights, revocationDomain, leaseDeadline,
                                          sealedObjectSelector);
}

Foundation::Result<CapabilityHandle, CapabilityError> RootCapabilityIssuer::MintUnderAuthority(
    CapabilitySpace& destination, const ObjectId object, const CapabilityRights rights,
    const CapabilityHandle authority, const ObjectType authorityType, const CapabilityOperation operation,
    const IKernelObject& authorityObject, const Foundation::UInt64 sealedObjectSelector) noexcept
{
    return destination.MintUnderAuthorityFromRootIssuer(m_objectRegistry, object, rights, authority, authorityType,
                                                        operation, authorityObject, sealedObjectSelector);
}
} // namespace Asteria::Kernel::Object
