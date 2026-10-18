// SPDX-License-Identifier: Apache-2.0

#include "RevocationRegistry.hpp"

namespace Asteria::Kernel::Object
{
namespace
{
constexpr Foundation::UInt32 DomainSlotMask{0xFFFFU};
constexpr Foundation::UInt32 DomainGenerationShift{16U};
constexpr Foundation::UInt32 MaximumEpoch{~Foundation::UInt32{0U}};
constexpr Foundation::UInt16 MaximumGeneration{0xFFFFU};
constinit Platform::Synchronization::SpinLock g_registryIdentifierSynchronization;
constinit Foundation::UInt32 g_nextRegistryIdentifier{1U};

[[nodiscard]] Foundation::UInt32 AllocateRegistryIdentifier() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{g_registryIdentifierSynchronization};
    if (g_nextRegistryIdentifier == 0U)
    {
        return 0U;
    }

    const Foundation::UInt32 identifier = g_nextRegistryIdentifier;
    if (g_nextRegistryIdentifier == ~Foundation::UInt32{0U})
    {
        g_nextRegistryIdentifier = 0U;
    }
    else
    {
        ++g_nextRegistryIdentifier;
    }
    return identifier;
}
}

RevocationRegistry::RevocationRegistry(Platform::Synchronization::SpinLock& synchronization) noexcept
    : m_synchronization{synchronization}, m_registryIdentifier{AllocateRegistryIdentifier()}
{
}

Foundation::Result<RevocationDomainId, RevocationError> RevocationRegistry::CreateRoot() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return CreateDomainLocked(RevocationDomainId{}, 0U, 0U);
}

Foundation::Result<RevocationDomainId, RevocationError>
RevocationRegistry::CreateChild(const RevocationDomainId parent) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return CreateChildLocked(parent);
}

Foundation::Result<RevocationDomainId, RevocationError>
RevocationRegistry::CreateChildLocked(const RevocationDomainId parent) noexcept
{
    const DomainEntry* const parentEntry = FindEntryLocked(parent);
    if (parentEntry == nullptr)
    {
        return Foundation::Result<RevocationDomainId, RevocationError>::Failure(RevocationError::InvalidDomain);
    }

    if (!IsLineageCurrentLocked(*parentEntry))
    {
        return Foundation::Result<RevocationDomainId, RevocationError>::Failure(RevocationError::Revoked);
    }

    if (parentEntry->Depth >= MaximumDepth)
    {
        return Foundation::Result<RevocationDomainId, RevocationError>::Failure(RevocationError::DepthExceeded);
    }

    return CreateDomainLocked(parent, parentEntry->Epoch, static_cast<Foundation::UInt8>(parentEntry->Depth + 1U));
}

Foundation::Result<Foundation::UInt32, RevocationError>
RevocationRegistry::GetCurrentEpoch(const RevocationDomainId domain) const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return GetCurrentEpochLocked(domain);
}

Foundation::Result<Foundation::UInt32, RevocationError>
RevocationRegistry::GetCurrentEpochLocked(const RevocationDomainId domain) const noexcept
{
    const DomainEntry* const entry = FindEntryLocked(domain);
    if (entry == nullptr)
    {
        return Foundation::Result<Foundation::UInt32, RevocationError>::Failure(RevocationError::InvalidDomain);
    }

    if (!IsLineageCurrentLocked(*entry))
    {
        return Foundation::Result<Foundation::UInt32, RevocationError>::Failure(RevocationError::Revoked);
    }

    return Foundation::Result<Foundation::UInt32, RevocationError>::Success(entry->Epoch);
}

RevocationError RevocationRegistry::Revoke(const RevocationDomainId domain) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return RevokeLocked(domain);
}

RevocationError RevocationRegistry::RevokeLocked(const RevocationDomainId domain) noexcept
{
    DomainEntry* const entry = FindEntryLocked(domain);
    if (entry == nullptr)
    {
        return RevocationError::InvalidDomain;
    }

    if (!IsLineageCurrentLocked(*entry))
    {
        return RevocationError::Revoked;
    }

    if (entry->Epoch == MaximumEpoch)
    {
        entry->IsPermanentlyRevoked = true;
        return RevocationError::Ok;
    }

    ++entry->Epoch;
    return RevocationError::Ok;
}

RevocationError RevocationRegistry::Destroy(const RevocationDomainId domain) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    const DomainEntry* const root = FindEntryLocked(domain);
    if (root == nullptr)
    {
        return RevocationError::InvalidDomain;
    }

    // Mark the whole subtree first: releasing an ancestor would hide the
    // lineage of its descendants from the walk.
    Foundation::UInt64 subtree{0U};
    for (Foundation::Size index{0U}; index < MaximumDomains; ++index)
    {
        if (IsInSubtreeLocked(m_entries[index], *root))
        {
            subtree |= Foundation::UInt64{1U} << index;
        }
    }
    for (Foundation::Size index{0U}; index < MaximumDomains; ++index)
    {
        if ((subtree & (Foundation::UInt64{1U} << index)) == 0U)
        {
            continue;
        }
        DomainEntry& entry = m_entries[index];
        entry.IsOccupied = false;
        entry.IsPermanentlyRevoked = false;
        entry.Epoch = 0U;
        entry.Parent = RevocationDomainId{};
        entry.ParentEpochAtCreation = 0U;
        entry.Depth = 0U;
        if (entry.Generation == MaximumGeneration)
        {
            entry.IsRetired = true;
        }
        else
        {
            ++entry.Generation;
        }
    }
    return RevocationError::Ok;
}

bool RevocationRegistry::IsInSubtreeLocked(const DomainEntry& entry, const DomainEntry& root) const noexcept
{
    if (!entry.IsOccupied)
    {
        return false;
    }
    const DomainEntry* current = &entry;
    for (Foundation::UInt8 traversed{0U}; traversed <= MaximumDepth; ++traversed)
    {
        if (current == &root)
        {
            return true;
        }
        current = FindEntryLocked(current->Parent);
        if (current == nullptr)
        {
            return false;
        }
    }
    return false;
}

bool RevocationRegistry::IsCurrent(const RevocationDomainId domain,
                                   const Foundation::UInt32 expectedEpoch) const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return IsCurrentLocked(domain, expectedEpoch);
}

bool RevocationRegistry::IsReady() const noexcept
{
    return m_registryIdentifier != 0U;
}

bool RevocationRegistry::IsCurrentLocked(const RevocationDomainId domain,
                                         const Foundation::UInt32 expectedEpoch) const noexcept
{
    const DomainEntry* const entry = FindEntryLocked(domain);
    if (entry == nullptr)
    {
        return false;
    }

    if (entry->Epoch != expectedEpoch)
    {
        return false;
    }

    return IsLineageCurrentLocked(*entry);
}

Foundation::Result<RevocationDomainId, RevocationError>
RevocationRegistry::CreateDomainLocked(const RevocationDomainId parent, const Foundation::UInt32 parentEpoch,
                                       const Foundation::UInt8 depth) noexcept
{
    if (m_registryIdentifier == 0U)
    {
        return Foundation::Result<RevocationDomainId, RevocationError>::Failure(RevocationError::DomainExhausted);
    }

    for (Foundation::Size index{0U}; index < MaximumDomains; ++index)
    {
        DomainEntry& entry = m_entries[index];
        if (entry.IsOccupied || entry.IsRetired)
        {
            continue;
        }

        if (entry.Generation == 0U)
        {
            entry.Generation = 1U;
        }

        entry.IsOccupied = true;
        entry.IsPermanentlyRevoked = false;
        entry.Epoch = 1U;
        entry.Parent = parent;
        entry.ParentEpochAtCreation = parentEpoch;
        entry.Depth = depth;

        const Foundation::UInt32 slotValue = static_cast<Foundation::UInt32>(index + 1U);
        const Foundation::UInt32 localValue =
            slotValue | (static_cast<Foundation::UInt32>(entry.Generation) << DomainGenerationShift);
        const Foundation::UInt64 rawValue = (static_cast<Foundation::UInt64>(m_registryIdentifier) << 32U) |
                                            static_cast<Foundation::UInt64>(localValue);

        return Foundation::Result<RevocationDomainId, RevocationError>::Success(RevocationDomainId{rawValue});
    }

    return Foundation::Result<RevocationDomainId, RevocationError>::Failure(RevocationError::DomainExhausted);
}

const RevocationRegistry::DomainEntry*
RevocationRegistry::FindEntryLocked(const RevocationDomainId domain) const noexcept
{
    const Foundation::UInt64 rawValue = domain.GetValue();
    const Foundation::UInt32 registryIdentifier = static_cast<Foundation::UInt32>(rawValue >> 32U);
    const Foundation::UInt32 localValue = static_cast<Foundation::UInt32>(rawValue);
    const Foundation::UInt32 slotValue = localValue & DomainSlotMask;
    const Foundation::UInt16 generation = static_cast<Foundation::UInt16>(localValue >> DomainGenerationShift);

    if ((registryIdentifier != m_registryIdentifier) || (slotValue == 0U) || (slotValue > MaximumDomains) ||
        (generation == 0U))
    {
        return nullptr;
    }

    const Foundation::Size index = static_cast<Foundation::Size>(slotValue - 1U);
    const DomainEntry& entry = m_entries[index];
    if (!entry.IsOccupied || (entry.Generation != generation))
    {
        return nullptr;
    }

    return &entry;
}

RevocationRegistry::DomainEntry* RevocationRegistry::FindEntryLocked(const RevocationDomainId domain) noexcept
{
    const Foundation::UInt64 rawValue = domain.GetValue();
    const Foundation::UInt32 registryIdentifier = static_cast<Foundation::UInt32>(rawValue >> 32U);
    const Foundation::UInt32 localValue = static_cast<Foundation::UInt32>(rawValue);
    const Foundation::UInt32 slotValue = localValue & DomainSlotMask;
    const Foundation::UInt16 generation = static_cast<Foundation::UInt16>(localValue >> DomainGenerationShift);

    if ((registryIdentifier != m_registryIdentifier) || (slotValue == 0U) || (slotValue > MaximumDomains) ||
        (generation == 0U))
    {
        return nullptr;
    }

    const Foundation::Size index = static_cast<Foundation::Size>(slotValue - 1U);
    DomainEntry& entry = m_entries[index];
    if (!entry.IsOccupied || (entry.Generation != generation))
    {
        return nullptr;
    }

    return &entry;
}

bool RevocationRegistry::IsLineageCurrentLocked(const DomainEntry& entry) const noexcept
{
    if (entry.IsPermanentlyRevoked)
    {
        return false;
    }

    const DomainEntry* child = &entry;
    Foundation::UInt8 traversedDepth{0U};

    while (child->Parent.IsValid())
    {
        if (traversedDepth >= MaximumDepth)
        {
            return false;
        }

        const DomainEntry* const parent = FindEntryLocked(child->Parent);
        if (parent == nullptr)
        {
            return false;
        }

        if (parent->Epoch != child->ParentEpochAtCreation)
        {
            return false;
        }

        if (parent->IsPermanentlyRevoked)
        {
            return false;
        }

        child = parent;
        ++traversedDepth;
    }

    return traversedDepth == entry.Depth;
}

Platform::Synchronization::SpinLock& RevocationRegistry::GetSynchronization() const noexcept
{
    return m_synchronization;
}
} // namespace Asteria::Kernel::Object
