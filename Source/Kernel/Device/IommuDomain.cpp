// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Device/IommuDomain.hpp"

#include "Kernel/Object/ObjectRegistry.hpp"

namespace Asteria::Kernel::Device
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};

[[nodiscard]] constexpr Resource::ResourceReservation GetMetadataReservation() noexcept
{
    return Resource::ResourceReservation{
        .CommittedMemoryPages = 0U,
        .ResidentMemoryPages = 0U,
        .KernelMetadataBytes = sizeof(IommuDomain),
        .CapabilityHandles = 0U,
        .IpcQueueBytes = 0U,
        .Threads = 0U,
    };
}
} // Namespace

IommuDomainFailure
IommuDomain::MakeFailure(const IommuDomainError error, const Hal::Device::IommuBackendOwnershipError ownership,
                         const Hal::Device::IommuBackendError backend, const Hal::Device::IommuDeviceError device,
                         const Object::ObjectPinError pin, const Memory::FrameMappingPolicyError framePolicy,
                         const Resource::ResourceDomainError resource) noexcept
{
    return IommuDomainFailure{
        .Ownership = ownership,
        .Backend = backend,
        .Device = device,
        .Pin = pin,
        .FramePolicy = framePolicy,
        .Resource = resource,
        .Error = error,
        .Reserved = 0U,
    };
}

bool IommuDomain::ConfigurationIsValid(const IommuDomainConfiguration& configuration) noexcept
{
    if ((configuration.InputAddressBits < 32U) || (configuration.InputAddressBits > 64U))
    {
        return false;
    }
    for (const Foundation::UInt8 value : configuration.Reserved)
    {
        if (value != 0U)
        {
            return false;
        }
    }
    return true;
}

bool IommuDomain::RequestReservedIsZero(const IommuMappingRequest& request) noexcept
{
    for (const Foundation::UInt8 value : request.Reserved)
    {
        if (value != 0U)
        {
            return false;
        }
    }
    return true;
}

Memory::PagePermissions IommuDomain::GetFramePolicyPermissions(const DmaPermissions permissions) noexcept
{
    Memory::PagePermissions policy{Memory::PagePermission::Read};
    if (permissions.Contains(DmaPermission::Write))
    {
        policy = policy.With(Memory::PagePermission::Write);
    }
    return policy;
}

bool IommuDomain::BackendFailureIsOrdinary(const Hal::Device::IommuBackendError error) noexcept
{
    return (error == Hal::Device::IommuBackendError::UnsupportedConfiguration) ||
           (error == Hal::Device::IommuBackendError::CapacityExceeded);
}

IommuDomainFailure IommuDomain::Initialize(const Resource::ResourceDomainObjectId resourceDomainIdentity,
                                           Resource::ResourceDomain& resourceDomain,
                                           const IommuDomainConfiguration& configuration) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state != IommuDomainState::Uninitialized)
    {
        return MakeFailure(IommuDomainError::AlreadyInitialized);
    }
    if (!ConfigurationIsValid(configuration) || !resourceDomainIdentity.IsValid())
    {
        return MakeFailure(IommuDomainError::InvalidConfiguration);
    }
    if (!resourceDomainIdentity.IsFromRegistry(m_registry))
    {
        return MakeFailure(IommuDomainError::ResourceDomainRegistryMismatch);
    }
    if (!m_backend.IsReady())
    {
        return MakeFailure(IommuDomainError::BackendRejected, Hal::Device::IommuBackendOwnershipError::NotReady);
    }

    const Object::ObjectPinError pinError = m_registry.PinObject(
        resourceDomainIdentity.GetObjectId(), Object::ObjectType::ResourceDomain, m_resourceDomainPin);
    if (pinError != Object::ObjectPinError::Ok)
    {
        return MakeFailure(IommuDomainError::ResourceDomainPinRejected, Hal::Device::IommuBackendOwnershipError::None,
                           Hal::Device::IommuBackendError::None, Hal::Device::IommuDeviceError::None, pinError);
    }
    if (&m_resourceDomainPin.GetObject() != static_cast<Object::IKernelObject*>(&resourceDomain))
    {
        m_resourceDomainPin.Reset();
        return MakeFailure(IommuDomainError::ResourceDomainInstanceMismatch);
    }

    const Resource::ResourceDomainError reservationError = resourceDomain.TryReserve(GetMetadataReservation());
    if (reservationError != Resource::ResourceDomainError::None)
    {
        m_resourceDomainPin.Reset();
        return MakeFailure(IommuDomainError::ResourceDomainRejected, Hal::Device::IommuBackendOwnershipError::None,
                           Hal::Device::IommuBackendError::None, Hal::Device::IommuDeviceError::None,
                           Object::ObjectPinError::Ok, Memory::FrameMappingPolicyError::None, reservationError);
    }

    m_resourceDomain = &resourceDomain;
    m_resourceDomainIdentity = resourceDomainIdentity;
    m_resourceDomainIdentifier = resourceDomain.GetIdentifier();
    m_resourceReserved = true;

    const Hal::Device::IommuBackendOwnershipError ownershipError =
        m_backend.Claim(*this, configuration.InputAddressBits);
    if (ownershipError != Hal::Device::IommuBackendOwnershipError::None)
    {
        const Resource::ResourceDomainError rollbackError = resourceDomain.Release(GetMetadataReservation());
        if (rollbackError != Resource::ResourceDomainError::None)
        {
            m_configuration = configuration;
            m_state = IommuDomainState::Faulted;
            m_chargeOwnerValue.StoreRelease(m_resourceDomainIdentifier.GetValue());
            return MakeFailure(IommuDomainError::RollbackRejected, ownershipError, Hal::Device::IommuBackendError::None,
                               Hal::Device::IommuDeviceError::None, Object::ObjectPinError::Ok,
                               Memory::FrameMappingPolicyError::None, rollbackError);
        }

        m_resourceReserved = false;
        m_resourceDomain = nullptr;
        m_resourceDomainIdentity = Resource::ResourceDomainObjectId{};
        m_resourceDomainIdentifier = Object::ResourceDomainId{};
        m_resourceDomainPin.Reset();
        return MakeFailure(IommuDomainError::BackendRejected, ownershipError);
    }

    m_configuration = configuration;
    m_backendClaimed = true;
    m_state = IommuDomainState::Ready;
    m_chargeOwnerValue.StoreRelease(m_resourceDomainIdentifier.GetValue());
    return MakeFailure(IommuDomainError::None);
}

bool IommuDomain::DmaRangeIsValidLocked(const DmaAddress start, const Memory::PageCount pages) const noexcept
{
    const Foundation::UInt64 startValue = start.GetValue();
    const Foundation::UInt64 pageCount = pages.GetValue();
    if ((pageCount == 0U) || ((startValue & (Memory::MappingPageSizeBytes - 1U)) != 0U) ||
        (pageCount > (MaximumUInt64 / Memory::MappingPageSizeBytes)))
    {
        return false;
    }

    const Foundation::UInt64 sizeBytes = pageCount * Memory::MappingPageSizeBytes;
    if (startValue > (MaximumUInt64 - (sizeBytes - 1U)))
    {
        return false;
    }
    const Foundation::UInt64 lastAddress = startValue + sizeBytes - 1U;
    if (m_configuration.InputAddressBits == 64U)
    {
        return true;
    }
    const Foundation::UInt64 maximumAddress = (Foundation::UInt64{1U} << m_configuration.InputAddressBits) - 1U;
    return lastAddress <= maximumAddress;
}

bool IommuDomain::DmaRangeOverlapsLocked(const DmaAddress start, const Memory::PageCount pages) const noexcept
{
    const Foundation::UInt64 requestedStart = start.GetValue();
    const Foundation::UInt64 requestedLast = requestedStart + (pages.GetValue() * Memory::MappingPageSizeBytes) - 1U;
    for (const MappingRecord& record : m_mappings)
    {
        if ((record.Sequence == 0U) || record.Retired)
        {
            continue;
        }
        const Foundation::UInt64 existingStart = record.DmaStart.GetValue();
        const Foundation::UInt64 existingLast =
            existingStart + (record.Pages.GetValue() * Memory::MappingPageSizeBytes) - 1U;
        if ((requestedStart <= existingLast) && (existingStart <= requestedLast))
        {
            return true;
        }
    }
    return false;
}

bool IommuDomain::ReservationOverlapsLocked(const DmaAddress start, const Memory::PageCount pages) const noexcept
{
    if (m_reservations == nullptr)
    {
        return false;
    }
    // DmaRangeIsValidLocked proved the range does not wrap.
    const Foundation::UInt64 first = start.GetValue();
    const Foundation::UInt64 last = first + (pages.GetValue() * Memory::MappingPageSizeBytes) - 1U;
    for (const AttachmentRecord& record : m_attachments)
    {
        // The endpoint's identifier is fixed; its lock ranks below this one.
        if ((record.Device != nullptr) && m_reservations->Overlaps(record.Device->GetRequesterId(), first, last))
        {
            return true;
        }
    }
    return false;
}

bool IommuDomain::MappingOverlapsReservationLocked(const Foundation::UInt32 requester) const noexcept
{
    if (m_reservations == nullptr)
    {
        return false;
    }
    for (const MappingRecord& record : m_mappings)
    {
        if (record.Sequence == 0U)
        {
            continue;
        }
        const Foundation::UInt64 first = record.DmaStart.GetValue();
        const Foundation::UInt64 last = first + (record.Pages.GetValue() * Memory::MappingPageSizeBytes) - 1U;
        if (m_reservations->Overlaps(requester, first, last))
        {
            return true;
        }
    }
    return false;
}

IommuDomain::MappingRecord* IommuDomain::FindFreeMappingLocked() noexcept
{
    for (MappingRecord& record : m_mappings)
    {
        if ((record.Sequence == 0U) && !record.FramePin.IsValid())
        {
            return &record;
        }
    }
    return nullptr;
}

IommuDomain::MappingRecord* IommuDomain::FindRetiredMappingLocked(const IommuMappingRequest& request) noexcept
{
    for (MappingRecord& record : m_mappings)
    {
        if (!record.Retired || (record.FrameStorage != request.FrameStorage) ||
            !record.FrameIdentity.IsEqualTo(request.FrameIdentity) ||
            (record.Pages.GetValue() != request.Pages.GetValue()) ||
            (record.Permissions.GetBits() != request.Permissions.GetBits()))
        {
            continue;
        }
        const auto frame = record.FrameStorage->GetSnapshot();
        const Foundation::UInt64 offset = request.FrameOffsetPages.GetValue();
        if ((offset <= frame.Descriptor.Pages.GetValue()) &&
            (record.PhysicalStart.GetValue() ==
             frame.Descriptor.PhysicalStart.GetValue() + offset * Memory::MappingPageSizeBytes) &&
            (record.DmaStart.GetValue() ==
             m_backend.ChooseDeviceAddress(request.DmaStart, record.PhysicalStart).GetValue()))
        {
            return &record;
        }
    }
    return nullptr;
}

IommuDomain::MappingRecord* IommuDomain::FindMappingLocked(const IommuMappingToken mapping) noexcept
{
    for (MappingRecord& record : m_mappings)
    {
        if (!record.Retired && (record.Sequence == mapping.GetSequence()))
        {
            return &record;
        }
    }
    return nullptr;
}

IommuDomain::AttachmentRecord* IommuDomain::FindFreeAttachmentLocked() noexcept
{
    for (AttachmentRecord& record : m_attachments)
    {
        if ((record.Sequence == 0U) && (record.Device == nullptr))
        {
            return &record;
        }
    }
    return nullptr;
}

IommuDomain::AttachmentRecord* IommuDomain::FindAttachmentLocked(const IommuAttachmentToken attachment) noexcept
{
    for (AttachmentRecord& record : m_attachments)
    {
        if (record.Sequence == attachment.GetSequence())
        {
            return &record;
        }
    }
    return nullptr;
}

bool IommuDomain::DeviceIsAttachedLocked(const Hal::Device::IIommuDevice& device) const noexcept
{
    for (const AttachmentRecord& record : m_attachments)
    {
        if ((record.Sequence != 0U) && (record.Device == &device))
        {
            return true;
        }
    }
    return false;
}

void IommuDomain::PublishMappingLocked(MappingRecord& record, const IommuMappingRequest& request,
                                       const DmaAddress deviceStart, const Memory::PhysicalAddress physicalStart,
                                       const Foundation::UInt64 sequence) noexcept
{
    if (record.Sequence == 0U)
    {
        ++m_mappingCount;
    }
    record.Retired = false;
    record.FrameStorage = request.FrameStorage;
    record.FrameIdentity = request.FrameIdentity;
    record.DmaStart = deviceStart;
    record.PhysicalStart = physicalStart;
    record.Pages = request.Pages;
    record.Permissions = request.Permissions;
    record.Sequence = sequence;
    m_lastMappingSequence = sequence;
}

void IommuDomain::ReleaseMappingLocked(MappingRecord& record) noexcept
{
    record.FrameStorage->ReleaseMappingPolicy(Memory::FrameMappingKey{}, m_registry, record.FrameIdentity.GetObjectId(),
                                              record.PhysicalStart, record.Pages,
                                              GetFramePolicyPermissions(record.Permissions));
    record.FramePin.Reset();
    record.FrameStorage = nullptr;
    record.FrameIdentity = Memory::FrameId{};
    record.DmaStart = DmaAddress{};
    record.PhysicalStart = Memory::PhysicalAddress{};
    record.Pages = Memory::PageCount{};
    record.Permissions = DmaPermissions{};
    record.Sequence = 0U;
    record.Retired = false;
    --m_mappingCount;
}

// A parker took the record's pin and its Frame policy: only the record goes.
void IommuDomain::ForgetMappingLocked(MappingRecord& record) noexcept
{
    record.FramePin.Reset();
    record.FrameStorage = nullptr;
    record.FrameIdentity = Memory::FrameId{};
    record.DmaStart = DmaAddress{};
    record.PhysicalStart = Memory::PhysicalAddress{};
    record.Pages = Memory::PageCount{};
    record.Permissions = DmaPermissions{};
    record.Sequence = 0U;
    record.Retired = false;
    --m_mappingCount;
}

void IommuDomain::ReleaseParkedMapping(Object::ObjectPin& pin, Memory::Frame& frame, Object::ObjectRegistry& registry,
                                       const Object::ObjectId frameObject, const Memory::PhysicalAddress physicalStart,
                                       const Memory::PageCount pages,
                                       const Memory::PagePermissions permissions) noexcept
{
    frame.ReleaseMappingPolicy(Memory::FrameMappingKey{}, registry, frameObject, physicalStart, pages, permissions);
    pin.Reset();
}

void IommuDomain::PublishAttachmentLocked(AttachmentRecord& record, Hal::Device::IIommuDevice& device,
                                          const Foundation::UInt64 sequence) noexcept
{
    record.Device = &device;
    record.Sequence = sequence;
    m_lastAttachmentSequence = sequence;
    ++m_attachmentCount;
}

Hal::Device::IommuDeviceError IommuDomain::ReleaseAttachmentLocked(AttachmentRecord& record) noexcept
{
    const Hal::Device::IommuDeviceError error = record.Device->Release(*this);
    if (error == Hal::Device::IommuDeviceError::None)
    {
        record.Device = nullptr;
        record.Sequence = 0U;
        --m_attachmentCount;
    }
    return error;
}

Foundation::Result<IommuMappingReceipt, IommuDomainFailure>
IommuDomain::Map(const IommuMappingRequest& request) noexcept
{
    using ResultType = Foundation::Result<IommuMappingReceipt, IommuDomainFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == IommuDomainState::Faulted)
    {
        return ResultType::Failure(MakeFailure(IommuDomainError::Faulted));
    }
    if (m_state != IommuDomainState::Ready)
    {
        return ResultType::Failure(MakeFailure(IommuDomainError::NotReady));
    }
    if ((request.FrameStorage == nullptr) || !request.FrameIdentity.IsValid() ||
        !request.Permissions.HasOnlyKnownBits() || request.Permissions.IsEmpty() || !RequestReservedIsZero(request))
    {
        return ResultType::Failure(MakeFailure(IommuDomainError::InvalidMappingRequest));
    }
    if (!request.FrameIdentity.IsFromRegistry(m_registry))
    {
        return ResultType::Failure(MakeFailure(IommuDomainError::FrameRegistryMismatch));
    }
    if (!DmaRangeIsValidLocked(request.DmaStart, request.Pages))
    {
        return ResultType::Failure(MakeFailure(IommuDomainError::DmaRangeExceeded));
    }
    if (DmaRangeOverlapsLocked(request.DmaStart, request.Pages))
    {
        return ResultType::Failure(MakeFailure(IommuDomainError::DmaRangeOverlap));
    }
    if (ReservationOverlapsLocked(request.DmaStart, request.Pages))
    {
        return ResultType::Failure(MakeFailure(IommuDomainError::ReservedDmaRange));
    }

    MappingRecord* record = FindRetiredMappingLocked(request);
    const bool retained = record != nullptr;
    if (!retained)
    {
        record = FindFreeMappingLocked();
    }
    if (record == nullptr)
    {
        return ResultType::Failure(MakeFailure(IommuDomainError::MappingCapacityExceeded));
    }
    Foundation::UInt64 nextSequence{m_lastMappingSequence};
    if (!IommuSequencePolicy::TryAdvance(m_lastMappingSequence, nextSequence))
    {
        return ResultType::Failure(MakeFailure(IommuDomainError::MappingSequenceExhausted));
    }

    if (!retained)
    {
        const Object::ObjectPinError pinError =
            m_registry.PinObject(request.FrameIdentity.GetObjectId(), Object::ObjectType::Frame, record->FramePin);
        if (pinError != Object::ObjectPinError::Ok)
        {
            return ResultType::Failure(
                MakeFailure(IommuDomainError::FramePinRejected, Hal::Device::IommuBackendOwnershipError::None,
                            Hal::Device::IommuBackendError::None, Hal::Device::IommuDeviceError::None, pinError));
        }
    }
    if (retained)
    {
        Object::ObjectPin live;
        const auto pinError =
            m_registry.PinObject(request.FrameIdentity.GetObjectId(), Object::ObjectType::Frame, live);
        if (pinError != Object::ObjectPinError::Ok)
        {
            return ResultType::Failure(
                MakeFailure(IommuDomainError::FramePinRejected, Hal::Device::IommuBackendOwnershipError::None,
                            Hal::Device::IommuBackendError::None, Hal::Device::IommuDeviceError::None, pinError));
        }
    }
    if (&record->FramePin.GetObject() != static_cast<Object::IKernelObject*>(request.FrameStorage))
    {
        if (!retained)
        {
            record->FramePin.Reset();
        }
        return ResultType::Failure(MakeFailure(IommuDomainError::FrameInstanceMismatch));
    }

    const Memory::FrameSnapshot frame = request.FrameStorage->GetSnapshot();
    if (frame.State != Memory::FrameState::Ready)
    {
        if (!retained)
        {
            record->FramePin.Reset();
        }
        return ResultType::Failure(MakeFailure(IommuDomainError::FrameNotReady));
    }
    if (frame.Descriptor.Kind != Memory::FrameKind::Ram)
    {
        if (!retained)
        {
            record->FramePin.Reset();
        }
        return ResultType::Failure(MakeFailure(IommuDomainError::RamFrameRequired));
    }

    const Foundation::UInt64 offsetPages = request.FrameOffsetPages.GetValue();
    const Foundation::UInt64 requestedPages = request.Pages.GetValue();
    const Foundation::UInt64 framePages = frame.Descriptor.Pages.GetValue();
    if ((requestedPages == 0U) || (offsetPages > framePages) || (requestedPages > (framePages - offsetPages)))
    {
        if (!retained)
        {
            record->FramePin.Reset();
        }
        return ResultType::Failure(MakeFailure(IommuDomainError::FrameRangeExceeded));
    }
    const Foundation::UInt64 offsetBytes = offsetPages * Memory::MappingPageSizeBytes;
    const Memory::PhysicalAddress physicalStart{frame.Descriptor.PhysicalStart.GetValue() + offsetBytes};
    // The backend names the device address the driver programs: the one requested, or the physical
    // address on an identity backend, which is checked against the domain's width and its mappings
    // as a requested one was above.
    const DmaAddress deviceStart = m_backend.ChooseDeviceAddress(request.DmaStart, physicalStart);
    IommuDomainError refused{IommuDomainError::None};
    if (deviceStart.GetValue() != request.DmaStart.GetValue())
    {
        if (!DmaRangeIsValidLocked(deviceStart, request.Pages))
        {
            refused = IommuDomainError::DmaRangeExceeded;
        }
        else if (DmaRangeOverlapsLocked(deviceStart, request.Pages))
        {
            refused = IommuDomainError::DmaRangeOverlap;
        }
        else if (ReservationOverlapsLocked(deviceStart, request.Pages))
        {
            refused = IommuDomainError::ReservedDmaRange;
        }
    }
    if (refused != IommuDomainError::None)
    {
        if (!retained)
        {
            record->FramePin.Reset();
        }
        return ResultType::Failure(MakeFailure(refused));
    }
    const Memory::PagePermissions framePermissions = GetFramePolicyPermissions(request.Permissions);
    const Memory::FrameMappingPolicyError framePolicyError =
        retained ? Memory::FrameMappingPolicyError::None
                 : request.FrameStorage->ReserveMappingPolicy(Memory::FrameMappingKey{}, m_registry,
                                                              request.FrameIdentity.GetObjectId(), physicalStart,
                                                              request.Pages, framePermissions);
    if (framePolicyError != Memory::FrameMappingPolicyError::None)
    {
        if (!retained)
        {
            record->FramePin.Reset();
        }
        return ResultType::Failure(
            MakeFailure(IommuDomainError::FramePolicyRejected, Hal::Device::IommuBackendOwnershipError::None,
                        Hal::Device::IommuBackendError::None, Hal::Device::IommuDeviceError::None,
                        Object::ObjectPinError::Ok, framePolicyError));
    }

    const Hal::Device::IommuBackendMapRequest backendRequest{
        .DmaStart = deviceStart,
        .PhysicalStart = physicalStart,
        .Pages = request.Pages,
        .Permissions = request.Permissions,
        .Reserved = {},
    };
    const Hal::Device::IommuBackendError backendError = m_backend.Map(backendRequest);
    if (backendError != Hal::Device::IommuBackendError::None)
    {
        if (BackendFailureIsOrdinary(backendError))
        {
            if (!retained)
            {
                request.FrameStorage->ReleaseMappingPolicy(Memory::FrameMappingKey{}, m_registry,
                                                           request.FrameIdentity.GetObjectId(), physicalStart,
                                                           request.Pages, framePermissions);
                record->FramePin.Reset();
            }
        }
        else
        {
            PublishMappingLocked(*record, request, deviceStart, physicalStart, nextSequence);
            EnterFaultedLocked();
        }
        return ResultType::Failure(MakeFailure(IommuDomainError::BackendRejected,
                                               Hal::Device::IommuBackendOwnershipError::None, backendError));
    }

    PublishMappingLocked(*record, request, deviceStart, physicalStart, nextSequence);
    return ResultType::Success(IommuMappingReceipt{
        .Mapping = IommuMappingToken{*this, nextSequence},
        .Frame = request.FrameIdentity,
        .DmaStart = deviceStart,
        .PhysicalStart = physicalStart,
        .Pages = request.Pages,
        .Permissions = request.Permissions,
        .Reserved = {},
    });
}

IommuDomainFailure IommuDomain::Unmap(const IommuMappingToken mapping) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == IommuDomainState::Faulted)
    {
        return MakeFailure(IommuDomainError::Faulted);
    }
    if (m_state != IommuDomainState::Ready)
    {
        return MakeFailure(IommuDomainError::NotReady);
    }
    if (!mapping.IsValid() || !mapping.IsFromDomain(*this))
    {
        return MakeFailure(IommuDomainError::InvalidMapping);
    }
    MappingRecord* const record = FindMappingLocked(mapping);
    if (record == nullptr)
    {
        return MakeFailure(IommuDomainError::StaleMapping);
    }
    return UnmapLocked(*record);
}

void IommuDomain::RequireDmaQuiescence() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    m_dmaQuiescenceRequired = true;
}

bool IommuDomain::RequiresDmaQuiescence() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return m_dmaQuiescenceRequired;
}

IommuDomainFailure IommuDomain::UnmapAt(const DmaAddress start) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == IommuDomainState::Faulted)
    {
        return MakeFailure(IommuDomainError::Faulted);
    }
    if (m_state != IommuDomainState::Ready)
    {
        return MakeFailure(IommuDomainError::NotReady);
    }
    for (MappingRecord& record : m_mappings)
    {
        if (!record.Retired && (record.Sequence != 0U) && (record.DmaStart.GetValue() == start.GetValue()))
        {
            return UnmapLocked(record);
        }
    }
    return MakeFailure(IommuDomainError::InvalidMapping);
}

IommuDomainFailure IommuDomain::UnmapLocked(MappingRecord& record) noexcept
{
    const Hal::Device::IommuBackendMapRequest backendRequest{
        .DmaStart = record.DmaStart,
        .PhysicalStart = record.PhysicalStart,
        .Pages = record.Pages,
        .Permissions = record.Permissions,
        .Reserved = {},
    };
    const Hal::Device::IommuBackendError backendError = m_backend.Unmap(backendRequest);
    if (backendError != Hal::Device::IommuBackendError::None)
    {
        EnterFaultedLocked();
        return MakeFailure(IommuDomainError::BackendRejected, Hal::Device::IommuBackendOwnershipError::None,
                           backendError);
    }

    if (m_dmaQuiescenceRequired)
    {
        record.Retired = true;
    }
    else
    {
        ReleaseMappingLocked(record);
    }
    return MakeFailure(IommuDomainError::None);
}

Hal::Device::IommuBackendFaults IommuDomain::GetFaults() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return m_backendClaimed ? m_backend.GetFaults() : Hal::Device::IommuBackendFaults{};
}

Foundation::Result<IommuAttachmentReceipt, IommuDomainFailure>
IommuDomain::Attach(Hal::Device::IIommuDevice& device) noexcept
{
    using ResultType = Foundation::Result<IommuAttachmentReceipt, IommuDomainFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == IommuDomainState::Faulted)
    {
        return ResultType::Failure(MakeFailure(IommuDomainError::Faulted));
    }
    if (m_state != IommuDomainState::Ready)
    {
        return ResultType::Failure(MakeFailure(IommuDomainError::NotReady));
    }
    if (DeviceIsAttachedLocked(device))
    {
        return ResultType::Failure(MakeFailure(IommuDomainError::DeviceAlreadyAttached));
    }
    if (MappingOverlapsReservationLocked(device.GetRequesterId()))
    {
        return ResultType::Failure(MakeFailure(IommuDomainError::ReservedDmaRange));
    }

    AttachmentRecord* const record = FindFreeAttachmentLocked();
    if (record == nullptr)
    {
        return ResultType::Failure(MakeFailure(IommuDomainError::AttachmentCapacityExceeded));
    }
    Foundation::UInt64 nextSequence{m_lastAttachmentSequence};
    if (!IommuSequencePolicy::TryAdvance(m_lastAttachmentSequence, nextSequence))
    {
        return ResultType::Failure(MakeFailure(IommuDomainError::AttachmentSequenceExhausted));
    }

    const Hal::Device::IommuDeviceError deviceError = device.Claim(*this);
    if (deviceError != Hal::Device::IommuDeviceError::None)
    {
        return ResultType::Failure(MakeFailure(IommuDomainError::DeviceRejected,
                                               Hal::Device::IommuBackendOwnershipError::None,
                                               Hal::Device::IommuBackendError::None, deviceError));
    }

    // A failed native attachment may already have exposed every mapping.
    m_dmaQuiescenceRequired = m_dmaQuiescenceRequired || !m_backend.DrainsDma();
    const Hal::Device::IommuBackendError backendError = m_backend.Attach(device);
    if (backendError != Hal::Device::IommuBackendError::None)
    {
        if (BackendFailureIsOrdinary(backendError))
        {
            const Hal::Device::IommuDeviceError rollbackError = device.Release(*this);
            if (rollbackError == Hal::Device::IommuDeviceError::None)
            {
                return ResultType::Failure(MakeFailure(IommuDomainError::BackendRejected,
                                                       Hal::Device::IommuBackendOwnershipError::None, backendError));
            }
            PublishAttachmentLocked(*record, device, nextSequence);
            EnterFaultedLocked();
            return ResultType::Failure(MakeFailure(IommuDomainError::RollbackRejected,
                                                   Hal::Device::IommuBackendOwnershipError::None, backendError,
                                                   rollbackError));
        }

        PublishAttachmentLocked(*record, device, nextSequence);
        EnterFaultedLocked();
        return ResultType::Failure(MakeFailure(IommuDomainError::BackendRejected,
                                               Hal::Device::IommuBackendOwnershipError::None, backendError));
    }

    PublishAttachmentLocked(*record, device, nextSequence);
    return ResultType::Success(IommuAttachmentReceipt{
        .Attachment = IommuAttachmentToken{*this, nextSequence},
    });
}

IommuDomainFailure IommuDomain::Detach(const IommuAttachmentToken attachment) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == IommuDomainState::Faulted)
    {
        return MakeFailure(IommuDomainError::Faulted);
    }
    if (m_state != IommuDomainState::Ready)
    {
        return MakeFailure(IommuDomainError::NotReady);
    }
    if (!attachment.IsValid() || !attachment.IsFromDomain(*this))
    {
        return MakeFailure(IommuDomainError::InvalidAttachment);
    }
    AttachmentRecord* const record = FindAttachmentLocked(attachment);
    if (record == nullptr)
    {
        return MakeFailure(IommuDomainError::StaleAttachment);
    }

    // Keep the requester association for the storage parker. Translation detachment alone cannot
    // revoke cached DMA addresses (AHCI block I/O #2).
    if (m_dmaQuiescenceRequired && (m_mappingCount != 0U))
    {
        return MakeFailure(IommuDomainError::ParkingRefused);
    }

    const Hal::Device::IommuBackendError backendError = m_backend.Detach(*record->Device);
    if (backendError != Hal::Device::IommuBackendError::None)
    {
        EnterFaultedLocked();
        return MakeFailure(IommuDomainError::BackendRejected, Hal::Device::IommuBackendOwnershipError::None,
                           backendError);
    }

    const Hal::Device::IommuDeviceError deviceError = ReleaseAttachmentLocked(*record);
    if (deviceError != Hal::Device::IommuDeviceError::None)
    {
        EnterFaultedLocked();
        return MakeFailure(IommuDomainError::DeviceRejected, Hal::Device::IommuBackendOwnershipError::None,
                           Hal::Device::IommuBackendError::None, deviceError);
    }
    return MakeFailure(IommuDomainError::None);
}

IommuDomainFailure IommuDomain::ReleaseResourceDomainLocked() noexcept
{
    if (!m_resourceReserved)
    {
        return MakeFailure(IommuDomainError::None);
    }
    const Resource::ResourceDomainError resourceError = m_resourceDomain->Release(GetMetadataReservation());
    if (resourceError != Resource::ResourceDomainError::None)
    {
        EnterFaultedLocked();
        return MakeFailure(IommuDomainError::RollbackRejected, Hal::Device::IommuBackendOwnershipError::None,
                           Hal::Device::IommuBackendError::None, Hal::Device::IommuDeviceError::None,
                           Object::ObjectPinError::Ok, Memory::FrameMappingPolicyError::None, resourceError);
    }

    m_resourceReserved = false;
    m_resourceDomain = nullptr;
    m_resourceDomainIdentity = Resource::ResourceDomainObjectId{};
    m_resourceDomainIdentifier = Object::ResourceDomainId{};
    m_resourceDomainPin.Reset();
    return MakeFailure(IommuDomainError::None);
}

IommuDomainFailure IommuDomain::Quiesce(IIommuStorageParker* const parker) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == IommuDomainState::Uninitialized)
    {
        return MakeFailure(IommuDomainError::NotReady);
    }
    if (m_state == IommuDomainState::Quiesced)
    {
        return MakeFailure(IommuDomainError::AlreadyQuiesced);
    }
    if ((m_state != IommuDomainState::Ready) && (m_state != IommuDomainState::Faulted))
    {
        EnterFaultedLocked();
        return MakeFailure(IommuDomainError::CorruptState);
    }
    // AHCI block I/O #2: no attachment means these pages were never exposed. Ambiguous Attach
    // retains an attachment record and still needs quiet.
    if (m_dmaQuiescenceRequired && (m_mappingCount != 0U) && (m_attachmentCount != 0U) && (parker == nullptr))
    {
        return MakeFailure(IommuDomainError::ParkingRefused);
    }
    if ((parker != nullptr) && !parker->CanPark(static_cast<Foundation::Size>(m_mappingCount)))
    {
        return MakeFailure(IommuDomainError::ParkingRefused);
    }

    if (m_backendClaimed)
    {
        const Hal::Device::IommuBackendOwnershipError ownershipError = m_backend.Release(*this);
        if (ownershipError != Hal::Device::IommuBackendOwnershipError::None)
        {
            EnterFaultedLocked();
            return MakeFailure(IommuDomainError::BackendRejected, ownershipError);
        }
        m_backendClaimed = false;
    }

    for (MappingRecord& record : m_mappings)
    {
        if (record.Sequence == 0U)
        {
            continue;
        }
        if (parker == nullptr)
        {
            ReleaseMappingLocked(record);
            continue;
        }
        parker->Park(IommuParkedMapping{.Pin = &record.FramePin,
                                        .FrameStorage = record.FrameStorage,
                                        .Registry = &m_registry,
                                        .FrameObject = record.FrameIdentity.GetObjectId(),
                                        .PhysicalStart = record.PhysicalStart,
                                        .Pages = record.Pages,
                                        .PolicyPermissions = GetFramePolicyPermissions(record.Permissions)});
        ForgetMappingLocked(record);
    }

    Hal::Device::IommuDeviceError firstDeviceError = Hal::Device::IommuDeviceError::None;
    for (AttachmentRecord& record : m_attachments)
    {
        if (record.Sequence == 0U)
        {
            continue;
        }
        const Hal::Device::IommuDeviceError deviceError = ReleaseAttachmentLocked(record);
        if ((firstDeviceError == Hal::Device::IommuDeviceError::None) &&
            (deviceError != Hal::Device::IommuDeviceError::None))
        {
            firstDeviceError = deviceError;
        }
    }
    if (firstDeviceError != Hal::Device::IommuDeviceError::None)
    {
        EnterFaultedLocked();
        return MakeFailure(IommuDomainError::DeviceRejected, Hal::Device::IommuBackendOwnershipError::None,
                           Hal::Device::IommuBackendError::None, firstDeviceError);
    }

    const IommuDomainFailure resourceRelease = ReleaseResourceDomainLocked();
    if (resourceRelease.Error != IommuDomainError::None)
    {
        return resourceRelease;
    }

    m_configuration = IommuDomainConfiguration{};
    m_chargeOwnerValue.StoreRelease(0U);
    m_state = IommuDomainState::Quiesced;
    return MakeFailure(IommuDomainError::None);
}

bool IommuDomain::IsInitialized() const noexcept
{
    return m_chargeOwnerValue.LoadAcquire() != 0U;
}

IommuDomainSnapshot IommuDomain::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return GetSnapshotLocked();
}

IommuDomainSnapshot IommuDomain::GetSnapshotLocked() const noexcept
{
    return IommuDomainSnapshot{
        .ResourceDomainIdentity = m_resourceDomainIdentity,
        .ResourceDomain = m_resourceDomainIdentifier,
        .MappingCount = m_mappingCount,
        .AttachmentCount = m_attachmentCount,
        .LastMappingSequence = m_lastMappingSequence,
        .LastAttachmentSequence = m_lastAttachmentSequence,
        .Configuration = m_configuration,
        .State = m_state,
        .BackendClaimed = m_backendClaimed,
        .Reserved = {},
    };
}

Object::ObjectType IommuDomain::GetObjectType() const noexcept
{
    return Object::ObjectType::IommuDomain;
}

Object::ResourceDomainId IommuDomain::GetChargeOwner() const noexcept
{
    return Object::ResourceDomainId{m_chargeOwnerValue.LoadAcquire()};
}

void IommuDomain::EnterFaultedLocked() noexcept
{
    m_state = IommuDomainState::Faulted;
}
} // Namespace Asteria::Kernel::Device
