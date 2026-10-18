// SPDX-License-Identifier: Apache-2.0

#include "Frame.hpp"

#include "Kernel/Object/ObjectRegistry.hpp"

namespace Asteria::Kernel::Memory
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt32 MaximumUInt32{~Foundation::UInt32{0U}};

[[nodiscard]] constexpr bool IsKnownCachePolicy(const MemoryCachePolicy cachePolicy) noexcept
{
    switch (cachePolicy)
    {
    case MemoryCachePolicy::WriteBack:
    case MemoryCachePolicy::Uncached:
    case MemoryCachePolicy::WriteCombining:
    case MemoryCachePolicy::WriteThrough:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool IsKnownFrameKind(const FrameKind kind) noexcept
{
    switch (kind)
    {
    case FrameKind::Ram:
    case FrameKind::Device:
        return true;
    }
    return false;
}

[[nodiscard]] bool DescriptorIsValid(const FrameDescriptor& descriptor) noexcept
{
    const Foundation::UInt64 physicalStart = descriptor.PhysicalStart.GetValue();
    const Foundation::UInt64 pageCount = descriptor.Pages.GetValue();
    if ((physicalStart == 0U) || ((physicalStart % MappingPageSizeBytes) != 0U) || (pageCount == 0U) ||
        (pageCount > (MaximumUInt64 / MappingPageSizeBytes)))
    {
        return false;
    }

    const Foundation::UInt64 sizeBytes = pageCount * MappingPageSizeBytes;
    if (physicalStart > (MaximumUInt64 - sizeBytes))
    {
        return false;
    }
    if (!IsKnownCachePolicy(descriptor.CachePolicy) || !IsKnownFrameKind(descriptor.Kind))
    {
        return false;
    }
    if ((descriptor.Kind == FrameKind::Device) && (descriptor.CachePolicy != MemoryCachePolicy::Uncached) &&
        (descriptor.CachePolicy != MemoryCachePolicy::WriteCombining))
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < sizeof(descriptor.Reserved); ++index)
    {
        if (descriptor.Reserved[index] != 0U)
        {
            return false;
        }
    }
    return true;
}
} // namespace

FrameError Frame::Initialize(const Object::ResourceDomainId chargeOwner, const FrameDescriptor& descriptor) noexcept
{
    return InitializeWithAccess(chargeOwner, descriptor, false);
}

FrameError Frame::InitializeReadOnly(const Object::ResourceDomainId chargeOwner,
                                     const FrameDescriptor& descriptor) noexcept
{
    return InitializeWithAccess(chargeOwner, descriptor, true);
}

FrameError Frame::InitializeWithAccess(const Object::ResourceDomainId chargeOwner, const FrameDescriptor& descriptor,
                                       const bool readOnly) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state != FrameState::Uninitialized)
    {
        return FrameError::AlreadyInitialized;
    }
    if (!chargeOwner.IsValid())
    {
        return FrameError::InvalidConfiguration;
    }
    if (!DescriptorIsValid(descriptor))
    {
        return FrameError::InvalidDescriptor;
    }

    m_descriptor = descriptor;
    m_readOnly = readOnly;
    m_state = FrameState::Ready;
    m_chargeOwnerValue.StoreRelease(chargeOwner.GetValue());
    return FrameError::None;
}

bool Frame::IsInitialized() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return m_state == FrameState::Ready;
}

FrameSnapshot Frame::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return GetSnapshotLocked();
}

Object::ObjectType Frame::GetObjectType() const noexcept
{
    return Object::ObjectType::Frame;
}

Object::ResourceDomainId Frame::GetChargeOwner() const noexcept
{
    return Object::ResourceDomainId{m_chargeOwnerValue.LoadAcquire()};
}

FrameSnapshot Frame::GetSnapshotLocked() const noexcept
{
    return FrameSnapshot{
        .Descriptor = m_descriptor,
        .State = m_state,
        .ReadOnly = static_cast<Foundation::UInt8>(m_readOnly ? 1U : 0U),
        .Reserved = {},
    };
}

FrameMappingPolicyError Frame::FindRecordLocked(const PhysicalAddress physicalStart, const PageCount pages,
                                                const Foundation::UInt8 access, FrameMappingPolicyRecord*& matching,
                                                FrameMappingPolicyRecord*& empty) noexcept
{
    const Foundation::UInt64 requestedStart = physicalStart.GetValue();
    const Foundation::UInt64 requestedPages = pages.GetValue();
    const Foundation::UInt64 requestedEnd = requestedStart + (requestedPages * MappingPageSizeBytes);
    const bool writable = (access & 1U) != 0U;
    const bool executable = (access & 2U) != 0U;
    for (Foundation::Size index{0U}; index < MaximumFrameMappingPolicyRecords; ++index)
    {
        FrameMappingPolicyRecord& record = m_mappingPolicies[index];
        if (record.ReferenceCount == 0U)
        {
            if (empty == nullptr)
            {
                empty = &record;
            }
            continue;
        }
        const Foundation::UInt64 existingStart = record.PhysicalStart.GetValue();
        const Foundation::UInt64 existingEnd = existingStart + (record.Pages.GetValue() * MappingPageSizeBytes);
        if ((requestedStart < existingEnd) && (existingStart < requestedEnd) &&
            (((writable && ((record.Access & 2U) != 0U)) || (executable && ((record.Access & 1U) != 0U)))))
        {
            return FrameMappingPolicyError::WriteExecuteConflict;
        }
        if ((existingStart == requestedStart) && (record.Pages.GetValue() == requestedPages) &&
            (record.Access == access))
        {
            matching = &record;
        }
    }
    const bool exhausted = (matching != nullptr) ? (matching->ReferenceCount == MaximumUInt64) : (empty == nullptr);
    return exhausted ? FrameMappingPolicyError::ReferenceExhausted : FrameMappingPolicyError::None;
}

FrameMappingPolicyError Frame::ReserveMappingPolicy(const FrameMappingKey, Object::ObjectRegistry& registry,
                                                    const Object::ObjectId object, const PhysicalAddress physicalStart,
                                                    const PageCount pages, const PagePermissions permissions) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    const bool writable = permissions.Contains(PagePermission::Write);
    const bool executable = permissions.Contains(PagePermission::Execute);
    if ((m_state != FrameState::Ready) || !permissions.HasOnlyKnownBits() ||
        !permissions.Contains(PagePermission::Read) || (writable && executable) ||
        (m_readOnly && (writable || executable)))
    {
        return FrameMappingPolicyError::InvalidPermissions;
    }
    if (m_activeReservations == MaximumUInt32)
    {
        return FrameMappingPolicyError::ReferenceExhausted;
    }

    const Foundation::UInt8 requestedAccess =
        static_cast<Foundation::UInt8>((writable ? 1U : 0U) | (executable ? 2U : 0U));
    // Read-only access needs no record and no counter. A BootFrame's
    // writable and executable access conflict with any overlapping record
    // of the opposite access; a CreatedFrame's with any reservation of it.
    FrameMappingPolicyRecord* emptyRecord{nullptr};
    FrameMappingPolicyRecord* matchingRecord{nullptr};
    if ((requestedAccess != 0U) && (m_mappingPolicies != nullptr))
    {
        const FrameMappingPolicyError found =
            FindRecordLocked(physicalStart, pages, requestedAccess, matchingRecord, emptyRecord);
        if (found != FrameMappingPolicyError::None)
        {
            return found;
        }
    }
    else if ((writable && (m_executableReservations != 0U)) || (executable && (m_writableReservations != 0U)))
    {
        return FrameMappingPolicyError::WriteExecuteConflict;
    }

    // The first reservation claims every page of the Frame; any registry
    // refusal leaves the Frame unchanged.
    if (m_activeReservations == 0U)
    {
        const Foundation::UInt64 frameStart = m_descriptor.PhysicalStart.GetValue();
        const Object::ObjectExclusiveRange range{
            .Start = frameStart,
            .End = frameStart + (m_descriptor.Pages.GetValue() * MappingPageSizeBytes),
        };
        if (registry.ClaimExclusiveRange(object, Object::ObjectType::Frame, range) != Object::ObjectRegistryError::Ok)
        {
            return FrameMappingPolicyError::PhysicalOwnershipConflict;
        }
    }
    ++m_activeReservations;

    if (matchingRecord != nullptr)
    {
        ++matchingRecord->ReferenceCount;
    }
    else if (emptyRecord != nullptr)
    {
        *emptyRecord = FrameMappingPolicyRecord{
            .PhysicalStart = physicalStart,
            .Pages = pages,
            .ReferenceCount = 1U,
            .Access = requestedAccess,
            .Reserved = {},
        };
    }
    else if (m_mappingPolicies == nullptr)
    {
        // Each counter stays at most m_activeReservations, below UInt32's.
        m_writableReservations += writable ? 1U : 0U;
        m_executableReservations += executable ? 1U : 0U;
    }
    return FrameMappingPolicyError::None;
}

void Frame::ReleaseMappingPolicy(const FrameMappingKey, Object::ObjectRegistry& registry, const Object::ObjectId object,
                                 const PhysicalAddress physicalStart, const PageCount pages,
                                 const PagePermissions permissions) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_activeReservations == 0U)
    {
        return;
    }
    const bool writable = permissions.Contains(PagePermission::Write);
    const bool executable = permissions.Contains(PagePermission::Execute);
    if ((writable || executable) && (m_mappingPolicies != nullptr))
    {
        const Foundation::UInt8 access = static_cast<Foundation::UInt8>((writable ? 1U : 0U) | (executable ? 2U : 0U));
        for (Foundation::Size index{0U}; index < MaximumFrameMappingPolicyRecords; ++index)
        {
            FrameMappingPolicyRecord& record = m_mappingPolicies[index];
            if ((record.ReferenceCount != 0U) && (record.PhysicalStart.GetValue() == physicalStart.GetValue()) &&
                (record.Pages.GetValue() == pages.GetValue()) && (record.Access == access))
            {
                --record.ReferenceCount;
                if (record.ReferenceCount == 0U)
                {
                    record = FrameMappingPolicyRecord{};
                }
                break;
            }
        }
    }
    else if (m_mappingPolicies == nullptr)
    {
        m_writableReservations -= (writable && (m_writableReservations != 0U)) ? 1U : 0U;
        m_executableReservations -= (executable && (m_executableReservations != 0U)) ? 1U : 0U;
    }

    --m_activeReservations;
    if (m_activeReservations == 0U)
    {
        static_cast<void>(registry.ReleaseExclusiveRange(object));
    }
}
} // namespace Asteria::Kernel::Memory
