// SPDX-License-Identifier: Apache-2.0

#include "CapabilitySpace.hpp"
#include "Kernel/Device/DeviceRegion.hpp"

namespace Asteria::Kernel::Object
{
namespace
{
constexpr Foundation::UInt64 CapabilityAuthenticationDomain{0x3150'4143'5254'5341ULL};
constexpr Foundation::UInt64 AuthenticationLengthBlock{Foundation::UInt64{24U} << 56U};
constinit Platform::Synchronization::SpinLock g_spaceIdentifierSynchronization;
constinit Foundation::UInt64 g_nextSpaceIdentifier{1U};

[[nodiscard]] Foundation::UInt64 AllocateSpaceIdentifier() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{g_spaceIdentifierSynchronization};
    if (g_nextSpaceIdentifier == 0U)
    {
        return 0U;
    }

    const Foundation::UInt64 identifier = g_nextSpaceIdentifier;
    if (g_nextSpaceIdentifier == ~Foundation::UInt64{0U})
    {
        g_nextSpaceIdentifier = 0U;
    }
    else
    {
        ++g_nextSpaceIdentifier;
    }
    return identifier;
}

[[nodiscard]] constexpr Foundation::UInt64 RotateLeft64(const Foundation::UInt64 value,
                                                        const Foundation::UInt32 distance) noexcept
{
    if (distance == 0U)
    {
        return value;
    }

    return (value << distance) | (value >> (64U - distance));
}

void SipRound(Foundation::UInt64& value0, Foundation::UInt64& value1, Foundation::UInt64& value2,
              Foundation::UInt64& value3) noexcept
{
    value0 += value1;
    value1 = RotateLeft64(value1, 13U);
    value1 ^= value0;
    value0 = RotateLeft64(value0, 32U);
    value2 += value3;
    value3 = RotateLeft64(value3, 16U);
    value3 ^= value2;
    value0 += value3;
    value3 = RotateLeft64(value3, 21U);
    value3 ^= value0;
    value2 += value1;
    value1 = RotateLeft64(value1, 17U);
    value1 ^= value2;
    value2 = RotateLeft64(value2, 32U);
}

void CompressSipBlock(const Foundation::UInt64 message, Foundation::UInt64& value0, Foundation::UInt64& value1,
                      Foundation::UInt64& value2, Foundation::UInt64& value3) noexcept
{
    value3 ^= message;
    SipRound(value0, value1, value2, value3);
    SipRound(value0, value1, value2, value3);
    value0 ^= message;
}

[[nodiscard]] CapabilityError MapRevocationError(const RevocationError error) noexcept
{
    switch (error)
    {
    case RevocationError::Ok:
        return CapabilityError::Ok;

    case RevocationError::InvalidDomain:
        return CapabilityError::InvalidArgument;

    case RevocationError::DomainExhausted:
        return CapabilityError::RevocationDomainExhausted;

    case RevocationError::DepthExceeded:
        return CapabilityError::InvalidArgument;

    case RevocationError::Revoked:
        return CapabilityError::Revoked;
    }

    return CapabilityError::InvalidArgument;
}
} // namespace

CapabilityLease::CapabilityLease(ObjectRegistry& objectRegistry, IKernelObject& object,
                                 const CapabilityDescriptor& descriptor) noexcept
    : m_objectRegistry{&objectRegistry}, m_object{&object}, m_descriptor{descriptor}
{
}

CapabilityLease::~CapabilityLease() noexcept
{
    if (m_objectRegistry != nullptr)
    {
        m_objectRegistry->ReleaseReference(m_descriptor.Object);
    }
}

CapabilityLease::CapabilityLease(CapabilityLease&& other) noexcept
    : m_objectRegistry{other.m_objectRegistry}, m_object{other.m_object}, m_descriptor{other.m_descriptor}
{
    other.m_objectRegistry = nullptr;
    other.m_object = nullptr;
    other.m_descriptor = CapabilityDescriptor{};
}

bool CapabilityLease::IsValid() const noexcept
{
    return (m_objectRegistry != nullptr) && (m_object != nullptr);
}

const CapabilityDescriptor& CapabilityLease::GetDescriptor() const noexcept
{
    return m_descriptor;
}

IKernelObject& CapabilityLease::GetObject() const noexcept
{
    return *m_object;
}

CapabilityLookupResult::CapabilityLookupResult(CapabilityLookupResult&& other) noexcept
    : m_lease{static_cast<CapabilityLease&&>(other.m_lease)}, m_error{other.m_error}, m_hasValue{other.m_hasValue}
{
    other.m_error = CapabilityError::InvalidHandle;
    other.m_hasValue = false;
}

CapabilityLookupResult CapabilityLookupResult::Success(CapabilityLease&& lease) noexcept
{
    return CapabilityLookupResult{
        static_cast<CapabilityLease&&>(lease),
        CapabilityError::Ok,
        true,
    };
}

CapabilityLookupResult CapabilityLookupResult::Failure(const CapabilityError error) noexcept
{
    CapabilityLease emptyLease;
    return CapabilityLookupResult{
        static_cast<CapabilityLease&&>(emptyLease),
        error,
        false,
    };
}

bool CapabilityLookupResult::HasValue() const noexcept
{
    return m_hasValue;
}

const CapabilityLease& CapabilityLookupResult::GetValue() const noexcept
{
    return m_lease;
}

CapabilityError CapabilityLookupResult::GetError() const noexcept
{
    return m_error;
}

CapabilityLookupResult::CapabilityLookupResult(CapabilityLease&& lease, const CapabilityError error,
                                               const bool hasValue) noexcept
    : m_lease{static_cast<CapabilityLease&&>(lease)}, m_error{error}, m_hasValue{hasValue}
{
}

CapabilitySpace::CapabilitySpace(ObjectRegistry& objectRegistry, RevocationRegistry& revocationRegistry,
                                 Time::IMonotonicClock& clock, const Foundation::UInt64 validationKey0,
                                 const Foundation::UInt64 validationKey1) noexcept
    : m_objectRegistry{objectRegistry}, m_revocationRegistry{revocationRegistry}, m_clock{clock},
      m_synchronization{revocationRegistry.GetSynchronization()}, m_spaceIdentifier{AllocateSpaceIdentifier()},
      m_validationKey0{validationKey0}, m_validationKey1{validationKey1},
      m_isConfigurationValid{(&objectRegistry.GetSynchronization() == &revocationRegistry.GetSynchronization()) &&
                             revocationRegistry.IsReady() && m_spaceIdentifier.IsValid() &&
                             ((validationKey0 != 0U) || (validationKey1 != 0U))}
{
}

CapabilityLookupResult CapabilitySpace::Lookup(const CapabilityHandle handle, const ObjectType expectedType,
                                               const CapabilityOperation operation) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};

    if (!m_isConfigurationValid)
    {
        return CapabilityLookupResult::Failure(CapabilityError::InvalidConfiguration);
    }
    if (!IsKnownObjectType(expectedType))
    {
        return CapabilityLookupResult::Failure(CapabilityError::InvalidArgument);
    }

    const Foundation::Result<CapabilityRights, CapabilityError> rightsResult = GetRequiredRights(operation);
    if (!rightsResult.HasValue())
    {
        return CapabilityLookupResult::Failure(rightsResult.GetError());
    }

    const Foundation::Result<MonotonicTime, CapabilityError> timeResult = GetCurrentTimeLocked();
    if (!timeResult.HasValue())
    {
        return CapabilityLookupResult::Failure(timeResult.GetError());
    }

    const Foundation::Result<CapabilityDescriptor, CapabilityError> descriptorResult =
        GetUsableDescriptorLocked(handle, rightsResult.GetValue(), timeResult.GetValue());
    if (!descriptorResult.HasValue())
    {
        return CapabilityLookupResult::Failure(descriptorResult.GetError());
    }

    if (descriptorResult.GetValue().Type != expectedType)
    {
        return CapabilityLookupResult::Failure(CapabilityError::TypeMismatch);
    }

    const Foundation::Result<IKernelObject*, ObjectRegistryError> referenceResult =
        m_objectRegistry.AcquireReferenceLocked(descriptorResult.GetValue().Object);
    if (!referenceResult.HasValue() && (referenceResult.GetError() == ObjectRegistryError::ReferenceExhausted))
    {
        return CapabilityLookupResult::Failure(CapabilityError::ResourceExhausted);
    }
    if (!referenceResult.HasValue())
    {
        return CapabilityLookupResult::Failure(CapabilityError::ObjectNotFound);
    }

    CapabilityLease lease{m_objectRegistry, *referenceResult.GetValue(), descriptorResult.GetValue()};
    return CapabilityLookupResult::Success(static_cast<CapabilityLease&&>(lease));
}

Foundation::Result<CapabilityHandle, CapabilityError>
CapabilitySpace::Derive(const CapabilityHandle source, const CapabilityRights requestedRights,
                        const MonotonicTime requestedDeadline, const DerivedRevocationDomain revocationDomain) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};

    if (!m_isConfigurationValid)
    {
        return Foundation::Result<CapabilityHandle, CapabilityError>::Failure(CapabilityError::InvalidConfiguration);
    }
    const bool childDomain = revocationDomain == DerivedRevocationDomain::Child;
    if (!requestedRights.IsValid() || (!childDomain && (revocationDomain != DerivedRevocationDomain::Source)))
    {
        return Foundation::Result<CapabilityHandle, CapabilityError>::Failure(CapabilityError::InvalidArgument);
    }

    const Foundation::Result<MonotonicTime, CapabilityError> timeResult = GetCurrentTimeLocked();
    if (!timeResult.HasValue())
    {
        return Foundation::Result<CapabilityHandle, CapabilityError>::Failure(timeResult.GetError());
    }

    const CapabilityRights deriveRight{CapabilityRight::Derive};
    const Foundation::Result<CapabilityDescriptor, CapabilityError> sourceResult =
        GetUsableDescriptorLocked(source, deriveRight, timeResult.GetValue());
    if (!sourceResult.HasValue())
    {
        return Foundation::Result<CapabilityHandle, CapabilityError>::Failure(sourceResult.GetError());
    }

    const CapabilityDescriptor sourceDescriptor = sourceResult.GetValue();
    if (!requestedRights.IsSubsetOf(sourceDescriptor.Rights) ||
        (!childDomain && requestedRights.ContainsAll(CapabilityRights{CapabilityRight::Revoke})))
    {
        return Foundation::Result<CapabilityHandle, CapabilityError>::Failure(CapabilityError::RightsAmplification);
    }

    if (IsExpired(requestedDeadline, timeResult.GetValue()))
    {
        return Foundation::Result<CapabilityHandle, CapabilityError>::Failure(CapabilityError::Expired);
    }

    if (!sourceDescriptor.LeaseDeadline.IsInfinite() &&
        (requestedDeadline.IsInfinite() || (requestedDeadline.GetTicks() > sourceDescriptor.LeaseDeadline.GetTicks())))
    {
        return Foundation::Result<CapabilityHandle, CapabilityError>::Failure(CapabilityError::LeaseExtension);
    }

    // A shorter lease alone does not attenuate: any far-future deadline is as
    // good as an infinite one, and Duplicate already grants equal leases.
    if (!childDomain && requestedRights.IsEqualTo(sourceDescriptor.Rights))
    {
        return Foundation::Result<CapabilityHandle, CapabilityError>::Failure(CapabilityError::NoAttenuation);
    }

    CapabilityDescriptor derivedDescriptor = sourceDescriptor;
    derivedDescriptor.Rights = requestedRights;
    derivedDescriptor.LeaseDeadline = requestedDeadline;

    const Foundation::Result<Foundation::Size, CapabilityError> sourceIndexResult = ResolveHandleLocked(source);
    if (!sourceIndexResult.HasValue())
    {
        return Foundation::Result<CapabilityHandle, CapabilityError>::Failure(CapabilityError::InvalidHandle);
    }

    const Foundation::UInt32 sourceEpoch = m_entries[sourceIndexResult.GetValue()].RevocationEpoch;
    if (!m_revocationRegistry.IsCurrentLocked(sourceDescriptor.RevocationDomain, sourceEpoch))
    {
        return Foundation::Result<CapabilityHandle, CapabilityError>::Failure(CapabilityError::Revoked);
    }

    if (!childDomain)
    {
        return InsertDescriptorLocked(derivedDescriptor, sourceEpoch);
    }
    if (!HasFreeSlotLocked())
    {
        return Foundation::Result<CapabilityHandle, CapabilityError>::Failure(CapabilityError::SpaceExhausted);
    }
    const Foundation::Result<RevocationDomainId, RevocationError> child =
        m_revocationRegistry.CreateChildLocked(sourceDescriptor.RevocationDomain);
    if (!child.HasValue())
    {
        const RevocationError error = child.GetError();
        return Foundation::Result<CapabilityHandle, CapabilityError>::Failure(
            ((error == RevocationError::DomainExhausted) || (error == RevocationError::DepthExceeded))
                ? CapabilityError::RevocationDomainExhausted
                : MapRevocationError(error));
    }
    const Foundation::Result<Foundation::UInt32, RevocationError> childEpoch =
        m_revocationRegistry.GetCurrentEpochLocked(child.GetValue());
    if (!childEpoch.HasValue())
    {
        return Foundation::Result<CapabilityHandle, CapabilityError>::Failure(CapabilityError::InvalidConfiguration);
    }
    derivedDescriptor.RevocationDomain = child.GetValue();
    return InsertDescriptorLocked(derivedDescriptor, childEpoch.GetValue());
}

Foundation::Result<CapabilityDescriptor, CapabilityError> CapabilitySpace::Query(const CapabilityHandle handle) noexcept
{
    using ResultType = Foundation::Result<CapabilityDescriptor, CapabilityError>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!m_isConfigurationValid)
    {
        return ResultType::Failure(CapabilityError::InvalidConfiguration);
    }

    const Foundation::Result<MonotonicTime, CapabilityError> timeResult = GetCurrentTimeLocked();
    if (!timeResult.HasValue())
    {
        return ResultType::Failure(timeResult.GetError());
    }

    return GetUsableDescriptorLocked(handle, CapabilityRights{CapabilityRight::Inspect}, timeResult.GetValue());
}

CapabilityError CapabilitySpace::Revoke(const CapabilityHandle handle) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!m_isConfigurationValid)
    {
        return CapabilityError::InvalidConfiguration;
    }

    const Foundation::Result<MonotonicTime, CapabilityError> timeResult = GetCurrentTimeLocked();
    if (!timeResult.HasValue())
    {
        return timeResult.GetError();
    }
    const Foundation::Result<CapabilityDescriptor, CapabilityError> descriptorResult =
        GetUsableDescriptorLocked(handle, CapabilityRights{CapabilityRight::Revoke}, timeResult.GetValue());
    if (!descriptorResult.HasValue())
    {
        return descriptorResult.GetError();
    }

    switch (m_revocationRegistry.RevokeLocked(descriptorResult.GetValue().RevocationDomain))
    {
    case RevocationError::Ok:
        return CapabilityError::Ok;
    case RevocationError::Revoked:
        return CapabilityError::Revoked;
    case RevocationError::InvalidDomain:
    case RevocationError::DomainExhausted:
    case RevocationError::DepthExceeded:
        return CapabilityError::InvalidConfiguration;
    }
    return CapabilityError::InvalidConfiguration;
}

Foundation::Result<CapabilityHandle, CapabilityError>
CapabilitySpace::Duplicate(const CapabilityHandle source, const CapabilityRights requestedRights,
                           const MonotonicTime requestedDeadline) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};

    if (!m_isConfigurationValid)
    {
        return Foundation::Result<CapabilityHandle, CapabilityError>::Failure(CapabilityError::InvalidConfiguration);
    }
    if (!requestedRights.IsValid())
    {
        return Foundation::Result<CapabilityHandle, CapabilityError>::Failure(CapabilityError::InvalidArgument);
    }

    const Foundation::Result<MonotonicTime, CapabilityError> timeResult = GetCurrentTimeLocked();
    if (!timeResult.HasValue())
    {
        return Foundation::Result<CapabilityHandle, CapabilityError>::Failure(timeResult.GetError());
    }

    const CapabilityRights duplicateRight{CapabilityRight::Duplicate};
    const Foundation::Result<CapabilityDescriptor, CapabilityError> sourceResult =
        GetUsableDescriptorLocked(source, duplicateRight, timeResult.GetValue());
    if (!sourceResult.HasValue())
    {
        return Foundation::Result<CapabilityHandle, CapabilityError>::Failure(sourceResult.GetError());
    }

    const CapabilityDescriptor sourceDescriptor = sourceResult.GetValue();
    if (!requestedRights.IsSubsetOf(sourceDescriptor.Rights) ||
        requestedRights.ContainsAll(CapabilityRights{CapabilityRight::Revoke}))
    {
        return Foundation::Result<CapabilityHandle, CapabilityError>::Failure(CapabilityError::RightsAmplification);
    }

    if (IsExpired(requestedDeadline, timeResult.GetValue()))
    {
        return Foundation::Result<CapabilityHandle, CapabilityError>::Failure(CapabilityError::Expired);
    }

    if (!sourceDescriptor.LeaseDeadline.IsInfinite() &&
        (requestedDeadline.IsInfinite() || (requestedDeadline.GetTicks() > sourceDescriptor.LeaseDeadline.GetTicks())))
    {
        return Foundation::Result<CapabilityHandle, CapabilityError>::Failure(CapabilityError::LeaseExtension);
    }

    CapabilityDescriptor duplicateDescriptor = sourceDescriptor;
    duplicateDescriptor.Rights = requestedRights;
    duplicateDescriptor.LeaseDeadline = requestedDeadline;

    const Foundation::Result<Foundation::Size, CapabilityError> sourceIndexResult = ResolveHandleLocked(source);
    if (!sourceIndexResult.HasValue())
    {
        return Foundation::Result<CapabilityHandle, CapabilityError>::Failure(CapabilityError::InvalidHandle);
    }

    const Foundation::UInt32 sourceEpoch = m_entries[sourceIndexResult.GetValue()].RevocationEpoch;
    if (!m_revocationRegistry.IsCurrentLocked(sourceDescriptor.RevocationDomain, sourceEpoch))
    {
        return Foundation::Result<CapabilityHandle, CapabilityError>::Failure(CapabilityError::Revoked);
    }

    return InsertDescriptorLocked(duplicateDescriptor, sourceEpoch);
}

CapabilityError CapabilitySpace::Close(const CapabilityHandle handle) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};

    return CloseLocked(handle);
}

CapabilityError CapabilitySpace::CloseLocked(const CapabilityHandle handle) noexcept
{

    if (!m_isConfigurationValid)
    {
        return CapabilityError::InvalidConfiguration;
    }

    const Foundation::Result<Foundation::Size, CapabilityError> indexResult = ResolveHandleLocked(handle);
    if (!indexResult.HasValue())
    {
        return CapabilityError::InvalidHandle;
    }

    CloseIndexLocked(indexResult.GetValue());
    return CapabilityError::Ok;
}

void CapabilitySpace::CloseIndexLocked(const Foundation::Size index) noexcept
{
    SlotEntry& entry = m_entries[index];
    entry.IsOccupied = false;
    entry.RevocationEpoch = 0U;
    entry.Descriptor = CapabilityDescriptor{};
    --m_activeCount;

    if (!CapabilityGenerationPolicy::CanAdvance(entry.Generation))
    {
        entry.IsRetired = true;
    }
    else
    {
        entry.Generation = CapabilityGenerationPolicy::Advance(entry.Generation);
    }
}

Foundation::Size CapabilitySpace::GetActiveCount() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return m_activeCount;
}

Foundation::Size CapabilitySpace::GetHeldCount() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    Foundation::Size held{0U};
    for (const SlotEntry& entry : m_entries)
    {
        held += (entry.IsOccupied && entry.IsHeld) ? 1U : 0U;
    }
    return held;
}

CapabilityLookupResult CapabilitySpace::PlacementAuthority(const Foundation::Size index) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    const auto& entry = m_entries[index];
    if (!m_isConfigurationValid || !entry.IsOccupied || entry.IsHeld ||
        (entry.Descriptor.Type != ObjectType::Interrupt && entry.Descriptor.Type != ObjectType::DeviceRegion &&
         entry.Descriptor.Type != ObjectType::InterruptLine && entry.Descriptor.Type != ObjectType::PciFunction))
    {
        return CapabilityLookupResult::Failure(CapabilityError::ObjectNotFound);
    }
    const auto now = GetCurrentTimeLocked();
    if (!now.HasValue())
    {
        return CapabilityLookupResult::Failure(now.GetError());
    }
    if (!m_revocationRegistry.IsCurrentLocked(entry.Descriptor.RevocationDomain, entry.RevocationEpoch) ||
        IsExpired(entry.Descriptor.LeaseDeadline, now.GetValue()))
    {
        return CapabilityLookupResult::Failure(CapabilityError::ObjectNotFound);
    }
    const auto reference = m_objectRegistry.AcquireReferenceLocked(entry.Descriptor.Object);
    if (!reference.HasValue())
    {
        return CapabilityLookupResult::Failure(CapabilityError::ObjectNotFound);
    }
    CapabilityLease lease{m_objectRegistry, *reference.GetValue(), entry.Descriptor};
    return CapabilityLookupResult::Success(static_cast<CapabilityLease&&>(lease));
}

bool CapabilitySpace::HasProcessorDeviceAuthority() noexcept
{
    for (Foundation::Size index{0U}; index < MaximumSlots; ++index)
    {
        const auto lease = PlacementAuthority(index);
        // A failed clock/configuration check cannot establish that this
        // domain is free of device authority. Place it conservatively on 0.
        if (!lease.HasValue())
        {
            if (lease.GetError() == CapabilityError::InvalidConfiguration)
            {
                return true;
            }
            continue;
        }
        const auto type = lease.GetValue().GetDescriptor().Type;
        if (type == ObjectType::Interrupt ||
            ((type == ObjectType::InterruptLine || type == ObjectType::PciFunction) &&
             lease.GetValue().GetDescriptor().Rights.ContainsAll(CapabilityRights{CapabilityRight::Invoke})) ||
            (type == ObjectType::DeviceRegion &&
             static_cast<Device::DeviceRegion&>(lease.GetValue().GetObject()).GetSnapshot().Descriptor.Kind ==
                 Device::DeviceRegionKind::PortIo))
        {
            return true;
        }
    }
    return false;
}

bool CapabilitySpace::IsReady() const noexcept
{
    return m_isConfigurationValid;
}

CapabilitySpaceIdentifier CapabilitySpace::GetIdentifier() const noexcept
{
    return m_spaceIdentifier;
}

bool CapabilitySpace::IsFromRegistry(const ObjectRegistry& objectRegistry) const noexcept
{
    return &m_objectRegistry == &objectRegistry;
}

bool CapabilitySpace::IsFromRevocationRegistry(const RevocationRegistry& revocationRegistry) const noexcept
{
    return &m_revocationRegistry == &revocationRegistry;
}

Foundation::Result<CapabilityHandle, CapabilityError>
CapabilitySpace::MintFromRootIssuer(const ObjectRegistry& issuerRegistry, const ObjectId object,
                                    const CapabilityRights rights, const RevocationDomainId revocationDomain,
                                    const MonotonicTime leaseDeadline,
                                    const Foundation::UInt64 sealedObjectSelector) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};

    if (!m_isConfigurationValid || (&issuerRegistry != &m_objectRegistry))
    {
        return Foundation::Result<CapabilityHandle, CapabilityError>::Failure(CapabilityError::InvalidConfiguration);
    }
    if (!rights.IsValid() || !revocationDomain.IsValid())
    {
        return Foundation::Result<CapabilityHandle, CapabilityError>::Failure(CapabilityError::InvalidArgument);
    }

    const Foundation::Result<MonotonicTime, CapabilityError> timeResult = GetCurrentTimeLocked();
    if (!timeResult.HasValue())
    {
        return Foundation::Result<CapabilityHandle, CapabilityError>::Failure(timeResult.GetError());
    }

    if (IsExpired(leaseDeadline, timeResult.GetValue()))
    {
        return Foundation::Result<CapabilityHandle, CapabilityError>::Failure(CapabilityError::Expired);
    }

    const Foundation::Result<ObjectInformation, ObjectRegistryError> objectResult =
        m_objectRegistry.GetInformationLocked(object);
    if (!objectResult.HasValue())
    {
        return Foundation::Result<CapabilityHandle, CapabilityError>::Failure(CapabilityError::ObjectNotFound);
    }

    const Foundation::Result<Foundation::UInt32, RevocationError> epochResult =
        m_revocationRegistry.GetCurrentEpochLocked(revocationDomain);
    if (!epochResult.HasValue())
    {
        return Foundation::Result<CapabilityHandle, CapabilityError>::Failure(
            MapRevocationError(epochResult.GetError()));
    }

    const CapabilityDescriptor descriptor{
        .Object = object,
        .Type = objectResult.GetValue().Type,
        .Rights = rights,
        .RevocationDomain = revocationDomain,
        .LeaseDeadline = leaseDeadline,
        .SealedObjectSelector = sealedObjectSelector,
        .ChargeOwner = objectResult.GetValue().ChargeOwner,
    };
    return InsertDescriptorLocked(descriptor, epochResult.GetValue());
}

Foundation::Result<CapabilityHandle, CapabilityError> CapabilitySpace::MintUnderAuthorityFromRootIssuer(
    const ObjectRegistry& issuerRegistry, const ObjectId object, const CapabilityRights rights,
    const CapabilityHandle authority, const ObjectType authorityType, const CapabilityOperation operation,
    const IKernelObject& authorityObject, const Foundation::UInt64 sealedObjectSelector) noexcept
{
    using ResultType = Foundation::Result<CapabilityHandle, CapabilityError>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};

    if (!m_isConfigurationValid || (&issuerRegistry != &m_objectRegistry))
    {
        return ResultType::Failure(CapabilityError::InvalidConfiguration);
    }
    if (!rights.IsValid())
    {
        return ResultType::Failure(CapabilityError::InvalidArgument);
    }

    const Foundation::Result<MonotonicTime, CapabilityError> timeResult = GetCurrentTimeLocked();
    if (!timeResult.HasValue())
    {
        return ResultType::Failure(timeResult.GetError());
    }
    const Foundation::Result<CapabilityDescriptor, CapabilityError> authorized =
        AuthorizeInvocationLocked(authority, authorityType, operation, authorityObject, timeResult.GetValue());
    if (!authorized.HasValue())
    {
        return ResultType::Failure(authorized.GetError());
    }

    const Foundation::Result<ObjectInformation, ObjectRegistryError> objectResult =
        m_objectRegistry.GetInformationLocked(object);
    if (!objectResult.HasValue())
    {
        return ResultType::Failure(CapabilityError::ObjectNotFound);
    }

    // The authority's slot epoch is current, so this is the epoch it was
    // authorized under.
    const RevocationDomainId revocationDomain = authorized.GetValue().RevocationDomain;
    const Foundation::Result<Foundation::UInt32, RevocationError> epochResult =
        m_revocationRegistry.GetCurrentEpochLocked(revocationDomain);
    if (!epochResult.HasValue())
    {
        return ResultType::Failure(MapRevocationError(epochResult.GetError()));
    }

    const CapabilityDescriptor descriptor{
        .Object = object,
        .Type = objectResult.GetValue().Type,
        .Rights = rights,
        .RevocationDomain = revocationDomain,
        .LeaseDeadline = authorized.GetValue().LeaseDeadline,
        .SealedObjectSelector = sealedObjectSelector,
        .ChargeOwner = objectResult.GetValue().ChargeOwner,
    };
    return InsertDescriptorLocked(descriptor, epochResult.GetValue());
}

Foundation::Result<CapabilityHandle, CapabilityError>
CapabilitySpace::InsertDescriptorLocked(const CapabilityDescriptor& descriptor,
                                        const Foundation::UInt32 revocationEpoch) noexcept
{
    for (Foundation::Size index{0U}; index < MaximumSlots; ++index)
    {
        SlotEntry& entry = m_entries[index];
        if (entry.IsOccupied || entry.IsRetired)
        {
            continue;
        }

        if (entry.Generation == 0U)
        {
            entry.Generation = 1U;
        }

        entry.IsOccupied = true;
        entry.RevocationEpoch = revocationEpoch;
        entry.Descriptor = descriptor;
        ++m_activeCount;

        return Foundation::Result<CapabilityHandle, CapabilityError>::Success(
            EncodeHandleLocked(index, entry.Generation));
    }

    return Foundation::Result<CapabilityHandle, CapabilityError>::Failure(CapabilityError::SpaceExhausted);
}

Foundation::Result<CapabilityDescriptor, CapabilityError>
CapabilitySpace::GetUsableDescriptorLocked(const CapabilityHandle handle, const CapabilityRights requiredRights,
                                           const MonotonicTime now) const noexcept
{
    const Foundation::Result<Foundation::Size, CapabilityError> indexResult = ResolveHandleLocked(handle);
    if (!indexResult.HasValue())
    {
        return Foundation::Result<CapabilityDescriptor, CapabilityError>::Failure(CapabilityError::InvalidHandle);
    }

    const SlotEntry& entry = m_entries[indexResult.GetValue()];
    if (!m_revocationRegistry.IsCurrentLocked(entry.Descriptor.RevocationDomain, entry.RevocationEpoch))
    {
        return Foundation::Result<CapabilityDescriptor, CapabilityError>::Failure(CapabilityError::Revoked);
    }

    if (IsExpired(entry.Descriptor.LeaseDeadline, now))
    {
        return Foundation::Result<CapabilityDescriptor, CapabilityError>::Failure(CapabilityError::Expired);
    }

    if (!entry.Descriptor.Rights.ContainsAll(requiredRights))
    {
        return Foundation::Result<CapabilityDescriptor, CapabilityError>::Failure(CapabilityError::RightsDenied);
    }

    const Foundation::Result<ObjectInformation, ObjectRegistryError> objectResult =
        m_objectRegistry.GetInformationLocked(entry.Descriptor.Object);
    if (!objectResult.HasValue() || (objectResult.GetValue().Type != entry.Descriptor.Type) ||
        (objectResult.GetValue().ChargeOwner.GetValue() != entry.Descriptor.ChargeOwner.GetValue()))
    {
        return Foundation::Result<CapabilityDescriptor, CapabilityError>::Failure(CapabilityError::ObjectNotFound);
    }

    return Foundation::Result<CapabilityDescriptor, CapabilityError>::Success(entry.Descriptor);
}

Foundation::Result<Foundation::Size, CapabilityError>
CapabilitySpace::ResolveHandleLocked(const CapabilityHandle handle) const noexcept
{
    if (!handle.IsValid())
    {
        return Foundation::Result<Foundation::Size, CapabilityError>::Failure(CapabilityError::InvalidHandle);
    }

    const Foundation::UInt64 selector = handle.GetSelector();
    const Foundation::Size index =
        static_cast<Foundation::Size>(selector & CapabilityGenerationPolicy::SelectorIndexMask);
    const Foundation::UInt64 generation = selector >> CapabilityGenerationPolicy::SelectorIndexBits;
    if ((generation == 0U) || (handle.GetAuthenticator() != CalculateAuthenticator(selector)))
    {
        return Foundation::Result<Foundation::Size, CapabilityError>::Failure(CapabilityError::InvalidHandle);
    }

    const SlotEntry& entry = m_entries[index];
    if (!entry.IsOccupied || entry.IsHeld || (entry.Generation != generation))
    {
        return Foundation::Result<Foundation::Size, CapabilityError>::Failure(CapabilityError::InvalidHandle);
    }

    return Foundation::Result<Foundation::Size, CapabilityError>::Success(index);
}

Foundation::Result<Foundation::Size, CapabilityError>
CapabilitySpace::ResolveHeldHandleLocked(const CapabilityHandle handle) const noexcept
{
    const Foundation::UInt64 selector = handle.GetSelector();
    const Foundation::Size index =
        static_cast<Foundation::Size>(selector & CapabilityGenerationPolicy::SelectorIndexMask);
    const Foundation::UInt64 generation = selector >> CapabilityGenerationPolicy::SelectorIndexBits;
    if (!handle.IsValid() || (generation == 0U) || (handle.GetAuthenticator() != CalculateAuthenticator(selector)) ||
        !m_entries[index].IsOccupied || !m_entries[index].IsHeld || (m_entries[index].Generation != generation))
    {
        return Foundation::Result<Foundation::Size, CapabilityError>::Failure(CapabilityError::InvalidHandle);
    }
    return Foundation::Result<Foundation::Size, CapabilityError>::Success(index);
}

void CapabilitySpace::ReleaseHeldLocked(const Foundation::Size index) noexcept
{
    m_entries[index].IsHeld = false;
    CloseIndexLocked(index);
}

void CapabilitySpace::RestoreHeldLocked(const Foundation::Size index) noexcept
{
    m_entries[index].IsHeld = false;
}

Foundation::Result<CapabilityDescriptor, CapabilityError>
CapabilitySpace::AuthorizeInvocationLocked(const CapabilityHandle handle, const ObjectType expectedType,
                                           const CapabilityOperation operation, const IKernelObject& expectedObject,
                                           const MonotonicTime now) const noexcept
{
    using ResultType = Foundation::Result<CapabilityDescriptor, CapabilityError>;

    const Foundation::Result<CapabilityRights, CapabilityError> rightsResult = GetRequiredRights(operation);
    if (!rightsResult.HasValue())
    {
        return ResultType::Failure(rightsResult.GetError());
    }
    const ResultType descriptorResult = GetUsableDescriptorLocked(handle, rightsResult.GetValue(), now);
    if (!descriptorResult.HasValue())
    {
        return descriptorResult;
    }
    if (descriptorResult.GetValue().Type != expectedType)
    {
        return ResultType::Failure(CapabilityError::TypeMismatch);
    }
    if (m_objectRegistry.GetInstanceLocked(descriptorResult.GetValue().Object) != &expectedObject)
    {
        return ResultType::Failure(CapabilityError::InvalidArgument);
    }
    return descriptorResult;
}

Foundation::Result<MonotonicTime, CapabilityError> CapabilitySpace::GetCurrentTimeLocked() noexcept
{
    const auto processor = ClockProcessor();
    const MonotonicTime currentTime{m_clock.GetTicks()};
    if (currentTime.IsInfinite() || (currentTime.GetTicks() < m_lastObservedTime[processor].GetTicks()))
    {
        return Foundation::Result<MonotonicTime, CapabilityError>::Failure(CapabilityError::InvalidConfiguration);
    }

    m_lastObservedTime[processor] = currentTime;
    return Foundation::Result<MonotonicTime, CapabilityError>::Success(currentTime);
}

CapabilityHandle CapabilitySpace::EncodeHandleLocked(const Foundation::Size index,
                                                     const Foundation::UInt64 generation) const noexcept
{
    const Foundation::UInt64 selector =
        (generation << CapabilityGenerationPolicy::SelectorIndexBits) | static_cast<Foundation::UInt64>(index);
    return CapabilityHandle{selector, CalculateAuthenticator(selector)};
}

Foundation::UInt64 CapabilitySpace::CalculateAuthenticator(const Foundation::UInt64 selector) const noexcept
{
    Foundation::UInt64 value0 = Foundation::UInt64{0x736F6D6570736575ULL} ^ m_validationKey0;
    Foundation::UInt64 value1 = Foundation::UInt64{0x646F72616E646F6DULL} ^ m_validationKey1;
    Foundation::UInt64 value2 = Foundation::UInt64{0x6C7967656E657261ULL} ^ m_validationKey0;
    Foundation::UInt64 value3 = Foundation::UInt64{0x7465646279746573ULL} ^ m_validationKey1;

    CompressSipBlock(selector, value0, value1, value2, value3);
    CompressSipBlock(m_spaceIdentifier.GetValue(), value0, value1, value2, value3);
    CompressSipBlock(CapabilityAuthenticationDomain, value0, value1, value2, value3);
    CompressSipBlock(AuthenticationLengthBlock, value0, value1, value2, value3);
    value2 ^= Foundation::UInt64{0xFFU};
    SipRound(value0, value1, value2, value3);
    SipRound(value0, value1, value2, value3);
    SipRound(value0, value1, value2, value3);
    SipRound(value0, value1, value2, value3);

    return value0 ^ value1 ^ value2 ^ value3;
}

Foundation::Result<CapabilityRights, CapabilityError>
CapabilitySpace::GetRequiredRights(const CapabilityOperation operation) noexcept
{
    switch (operation)
    {
    case CapabilityOperation::Inspect:
        return Foundation::Result<CapabilityRights, CapabilityError>::Success(
            CapabilityRights{CapabilityRight::Inspect});

    case CapabilityOperation::Invoke:
        return Foundation::Result<CapabilityRights, CapabilityError>::Success(
            CapabilityRights{CapabilityRight::Invoke});

    case CapabilityOperation::Read:
        return Foundation::Result<CapabilityRights, CapabilityError>::Success(CapabilityRights{CapabilityRight::Read});

    case CapabilityOperation::Write:
        return Foundation::Result<CapabilityRights, CapabilityError>::Success(CapabilityRights{CapabilityRight::Write});

    case CapabilityOperation::Map:
        return Foundation::Result<CapabilityRights, CapabilityError>::Success(CapabilityRights{CapabilityRight::Map});

    case CapabilityOperation::Wait:
        return Foundation::Result<CapabilityRights, CapabilityError>::Success(CapabilityRights{CapabilityRight::Wait});

    case CapabilityOperation::Signal:
        return Foundation::Result<CapabilityRights, CapabilityError>::Success(
            CapabilityRights{CapabilityRight::Signal});

    case CapabilityOperation::Transfer:
        return Foundation::Result<CapabilityRights, CapabilityError>::Success(
            CapabilityRights{CapabilityRight::Transfer});

    case CapabilityOperation::Duplicate:
        return Foundation::Result<CapabilityRights, CapabilityError>::Success(
            CapabilityRights{CapabilityRight::Duplicate});

    case CapabilityOperation::Derive:
        return Foundation::Result<CapabilityRights, CapabilityError>::Success(
            CapabilityRights{CapabilityRight::Derive});

    case CapabilityOperation::Revoke:
        return Foundation::Result<CapabilityRights, CapabilityError>::Success(
            CapabilityRights{CapabilityRight::Revoke});

    case CapabilityOperation::Manage:
        return Foundation::Result<CapabilityRights, CapabilityError>::Success(
            CapabilityRights{CapabilityRight::Manage});

    case CapabilityOperation::Debug:
        return Foundation::Result<CapabilityRights, CapabilityError>::Success(CapabilityRights{CapabilityRight::Debug});
    }

    return Foundation::Result<CapabilityRights, CapabilityError>::Failure(CapabilityError::InvalidArgument);
}

bool CapabilitySpace::IsExpired(const MonotonicTime deadline, const MonotonicTime now) noexcept
{
    return !deadline.IsInfinite() && (now.GetTicks() >= deadline.GetTicks());
}

bool CapabilitySpace::HasFreeSlotLocked() const noexcept
{
    for (const SlotEntry& entry : m_entries)
    {
        if (!entry.IsOccupied && !entry.IsRetired)
        {
            return true;
        }
    }
    return false;
}
} // namespace Asteria::Kernel::Object
