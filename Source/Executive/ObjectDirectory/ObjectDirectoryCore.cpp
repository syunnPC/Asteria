// SPDX-License-Identifier: Apache-2.0

#include "Executive/ObjectDirectory/ObjectDirectoryCore.hpp"

namespace Asteria::Executive::ObjectDirectory
{
namespace
{
constexpr Foundation::Size NoSlot{ObjectDirectoryCore::MaximumBindings};
constexpr Foundation::UInt64 MaximumRevision{~Foundation::UInt64{0U}};
} // namespace

DirectoryCoreError ObjectDirectoryCore::ValidateCandidate(const DirectoryBindingCandidate& candidate) noexcept
{
    if (!candidate.Custody.IsValid() || (candidate.ServiceGeneration == 0U) ||
        (candidate.Type != Kernel::Object::ObjectType::Endpoint))
    {
        return DirectoryCoreError::InvalidCandidate;
    }

    for (const Foundation::UInt8 value : candidate.Reserved)
    {
        if (value != 0U)
        {
            return DirectoryCoreError::InvalidCandidate;
        }
    }

    if (!candidate.ServiceInterface.IsValid())
    {
        return DirectoryCoreError::InvalidInterface;
    }
    if ((candidate.Version.Major == 0U) || (candidate.Version.MinimumMinor > candidate.Version.MaximumMinor) ||
        (candidate.Version.Reserved != 0U))
    {
        return DirectoryCoreError::InvalidVersion;
    }
    if (!candidate.PublishedRights.IsValid() || (candidate.PublishedRights.GetRawValue() == 0U) ||
        !candidate.PublishedRights.ContainsAll(
            Kernel::Object::CapabilityRights{Kernel::Object::CapabilityRight::Invoke}))
    {
        return DirectoryCoreError::InvalidRights;
    }
    if (!candidate.CustodyRights.IsValid() ||
        !candidate.CustodyRights.IsEqualTo(candidate.PublishedRights.With(Kernel::Object::CapabilityRight::Duplicate)))
    {
        return DirectoryCoreError::InvalidRights;
    }
    return DirectoryCoreError::None;
}

bool ObjectDirectoryCore::IsMutationOutputOccupied(const DirectoryMutationReceipt& output) noexcept
{
    return (output.Version.DirectoryGeneration != 0U) || (output.Version.Revision != 0U) ||
           (output.DisplacedEndpoint.GetSelector() != 0U) || (output.DisplacedEndpoint.GetAuthenticator() != 0U);
}

bool ObjectDirectoryCore::IsResolutionOutputOccupied(const DirectoryResolution& output) noexcept
{
    return (output.Version.DirectoryGeneration != 0U) || (output.Version.Revision != 0U) ||
           (output.ServiceInterface.GetLow() != 0U) || (output.ServiceInterface.GetHigh() != 0U) ||
           (output.ServiceGeneration != 0U) || (output.GrantedDeadline.GetTicks() != 0U) ||
           (output.GrantedRights.GetRawValue() != 0U) || (output.Major != 0U) || (output.SelectedMinor != 0U) ||
           (output.Reserved != 0U) || (output.TargetEndpoint.GetSelector() != 0U) ||
           (output.TargetEndpoint.GetAuthenticator() != 0U);
}

bool ObjectDirectoryCore::HandlesAreEqual(const Kernel::Object::CapabilityHandle left,
                                          const Kernel::Object::CapabilityHandle right) noexcept
{
    return (left.GetSelector() == right.GetSelector()) && (left.GetAuthenticator() == right.GetAuthenticator());
}

bool ObjectDirectoryCore::InterfacesAreEqual(const Kernel::Ipc::InterfaceIdentifier left,
                                             const Kernel::Ipc::InterfaceIdentifier right) noexcept
{
    return (left.GetLow() == right.GetLow()) && (left.GetHigh() == right.GetHigh());
}

Foundation::Size ObjectDirectoryCore::FindName(const DirectoryName& name) const noexcept
{
    for (Foundation::Size index{0U}; index < MaximumBindings; ++index)
    {
        if (m_entries[index].IsOccupied && m_entries[index].Name.IsEqualTo(name))
        {
            return index;
        }
    }
    return NoSlot;
}

Foundation::Size ObjectDirectoryCore::FindFreeSlot() const noexcept
{
    for (Foundation::Size index{0U}; index < MaximumBindings; ++index)
    {
        if (!m_entries[index].IsOccupied)
        {
            return index;
        }
    }
    return NoSlot;
}

bool ObjectDirectoryCore::IsCustodyAlreadyOwned(const Kernel::Object::CapabilityHandle custody) const noexcept
{
    for (Foundation::Size index{0U}; index < MaximumBindings; ++index)
    {
        if (m_entries[index].IsOccupied && HandlesAreEqual(m_entries[index].Custody, custody))
        {
            return true;
        }
    }
    return false;
}

void ObjectDirectoryCore::StoreBinding(BindingEntry& entry, const DirectoryName& name,
                                       const DirectoryBindingCandidate& candidate,
                                       const Foundation::UInt64 revision) noexcept
{
    entry.Name = name;
    entry.Custody = candidate.Custody;
    entry.ServiceInterface = candidate.ServiceInterface;
    entry.ServiceGeneration = candidate.ServiceGeneration;
    entry.TargetDeadline = candidate.TargetDeadline;
    entry.PublishedRights = candidate.PublishedRights;
    entry.CustodyRights = candidate.CustodyRights;
    entry.Type = candidate.Type;
    entry.Version = candidate.Version;
    entry.Revision = revision;
    entry.IsOccupied = true;
}

DirectoryCoreError ObjectDirectoryCore::BindIfAbsent(const DirectoryName& name, DirectoryBindingCandidate& candidate,
                                                     DirectoryMutationReceipt& output) noexcept
{
    if (m_state == DirectoryCoreState::InvalidConfiguration)
    {
        return DirectoryCoreError::InvalidConfiguration;
    }
    if (m_state != DirectoryCoreState::Ready)
    {
        return DirectoryCoreError::NotReady;
    }
    if (!name.IsValid())
    {
        return DirectoryCoreError::InvalidName;
    }
    const DirectoryCoreError candidateError = ValidateCandidate(candidate);
    if (candidateError != DirectoryCoreError::None)
    {
        return candidateError;
    }
    if (IsMutationOutputOccupied(output))
    {
        return DirectoryCoreError::OutputOccupied;
    }
    if (FindName(name) != NoSlot)
    {
        return DirectoryCoreError::AlreadyBound;
    }
    if (IsCustodyAlreadyOwned(candidate.Custody))
    {
        return DirectoryCoreError::CapabilityAlreadyOwned;
    }

    const Foundation::Size freeIndex = FindFreeSlot();
    if (freeIndex == NoSlot)
    {
        return DirectoryCoreError::CapacityExceeded;
    }
    if (m_lastIssuedRevision == MaximumRevision)
    {
        return DirectoryCoreError::RevisionExhausted;
    }

    const Foundation::UInt64 revision = m_lastIssuedRevision + 1U;
    StoreBinding(m_entries[freeIndex], name, candidate, revision);
    ++m_bindingCount;
    m_lastIssuedRevision = revision;
    candidate.Custody = {};
    output = DirectoryMutationReceipt{
        .Version =
            BindingVersion{
                .DirectoryGeneration = m_directoryGeneration,
                .Revision = revision,
            },
        .DisplacedEndpoint = {},
    };
    return DirectoryCoreError::None;
}

DirectoryCoreError ObjectDirectoryCore::ReplaceIfVersion(const DirectoryName& name,
                                                         const BindingVersion expectedVersion,
                                                         DirectoryBindingCandidate& candidate,
                                                         DirectoryMutationReceipt& output) noexcept
{
    if (m_state == DirectoryCoreState::InvalidConfiguration)
    {
        return DirectoryCoreError::InvalidConfiguration;
    }
    if (m_state != DirectoryCoreState::Ready)
    {
        return DirectoryCoreError::NotReady;
    }
    if (!name.IsValid())
    {
        return DirectoryCoreError::InvalidName;
    }
    if ((expectedVersion.DirectoryGeneration == 0U) || (expectedVersion.Revision == 0U))
    {
        return DirectoryCoreError::InvalidVersion;
    }
    const DirectoryCoreError candidateError = ValidateCandidate(candidate);
    if (candidateError != DirectoryCoreError::None)
    {
        return candidateError;
    }
    if (IsMutationOutputOccupied(output))
    {
        return DirectoryCoreError::OutputOccupied;
    }

    const Foundation::Size index = FindName(name);
    if (index == NoSlot)
    {
        return DirectoryCoreError::NotFound;
    }
    const BindingEntry& current = m_entries[index];
    if ((expectedVersion.DirectoryGeneration != m_directoryGeneration) ||
        (expectedVersion.Revision != current.Revision))
    {
        return DirectoryCoreError::BindingChanged;
    }
    if (IsCustodyAlreadyOwned(candidate.Custody))
    {
        return DirectoryCoreError::CapabilityAlreadyOwned;
    }
    if (m_lastIssuedRevision == MaximumRevision)
    {
        return DirectoryCoreError::RevisionExhausted;
    }

    const Kernel::Object::CapabilityHandle displacedEndpoint = current.Custody;
    const Foundation::UInt64 revision = m_lastIssuedRevision + 1U;
    StoreBinding(m_entries[index], name, candidate, revision);
    m_lastIssuedRevision = revision;
    candidate.Custody = {};
    output = DirectoryMutationReceipt{
        .Version =
            BindingVersion{
                .DirectoryGeneration = m_directoryGeneration,
                .Revision = revision,
            },
        .DisplacedEndpoint = displacedEndpoint,
    };
    return DirectoryCoreError::None;
}

DirectoryCoreError ObjectDirectoryCore::RemoveIfVersion(const DirectoryName& name, const BindingVersion expectedVersion,
                                                        DirectoryMutationReceipt& output) noexcept
{
    if (m_state == DirectoryCoreState::InvalidConfiguration)
    {
        return DirectoryCoreError::InvalidConfiguration;
    }
    if (m_state != DirectoryCoreState::Ready)
    {
        return DirectoryCoreError::NotReady;
    }
    if (!name.IsValid())
    {
        return DirectoryCoreError::InvalidName;
    }
    if ((expectedVersion.DirectoryGeneration == 0U) || (expectedVersion.Revision == 0U))
    {
        return DirectoryCoreError::InvalidVersion;
    }
    if (IsMutationOutputOccupied(output))
    {
        return DirectoryCoreError::OutputOccupied;
    }

    const Foundation::Size index = FindName(name);
    if (index == NoSlot)
    {
        return DirectoryCoreError::NotFound;
    }
    const BindingEntry& current = m_entries[index];
    if ((expectedVersion.DirectoryGeneration != m_directoryGeneration) ||
        (expectedVersion.Revision != current.Revision))
    {
        return DirectoryCoreError::BindingChanged;
    }
    if (m_lastIssuedRevision == MaximumRevision)
    {
        return DirectoryCoreError::RevisionExhausted;
    }

    const Kernel::Object::CapabilityHandle displacedEndpoint = current.Custody;
    const Foundation::UInt64 revision = m_lastIssuedRevision + 1U;
    m_entries[index] = BindingEntry{};
    --m_bindingCount;
    m_lastIssuedRevision = revision;
    output = DirectoryMutationReceipt{
        .Version =
            BindingVersion{
                .DirectoryGeneration = m_directoryGeneration,
                .Revision = revision,
            },
        .DisplacedEndpoint = displacedEndpoint,
    };
    return DirectoryCoreError::None;
}

DirectoryCoreError ObjectDirectoryCore::Resolve(const DirectoryName& name, const DirectoryResolveQuery& query,
                                                DirectoryResolution& output) const noexcept
{
    if (m_state == DirectoryCoreState::InvalidConfiguration)
    {
        return DirectoryCoreError::InvalidConfiguration;
    }
    if (m_state != DirectoryCoreState::Ready)
    {
        return DirectoryCoreError::NotReady;
    }
    if (!name.IsValid())
    {
        return DirectoryCoreError::InvalidName;
    }
    if (!query.ServiceInterface.IsValid())
    {
        return DirectoryCoreError::InvalidInterface;
    }
    if ((query.RequiredVersion.Major == 0U) ||
        (query.RequiredVersion.MinimumMinor > query.RequiredVersion.MaximumMinor) ||
        (query.RequiredVersion.Reserved != 0U))
    {
        return DirectoryCoreError::InvalidVersion;
    }
    if (!query.RequiredRights.IsValid() || (query.RequiredRights.GetRawValue() == 0U) ||
        !query.RequiredRights.ContainsAll(Kernel::Object::CapabilityRights{Kernel::Object::CapabilityRight::Invoke}))
    {
        return DirectoryCoreError::InvalidRights;
    }
    for (const Foundation::UInt8 value : query.Reserved)
    {
        if (value != 0U)
        {
            return DirectoryCoreError::InvalidVersion;
        }
    }
    if (IsResolutionOutputOccupied(output))
    {
        return DirectoryCoreError::OutputOccupied;
    }

    const Foundation::Size index = FindName(name);
    if (index == NoSlot)
    {
        return DirectoryCoreError::NotFound;
    }
    const BindingEntry& entry = m_entries[index];
    if (!InterfacesAreEqual(query.ServiceInterface, entry.ServiceInterface))
    {
        return DirectoryCoreError::InvalidInterface;
    }
    if (query.RequiredVersion.Major != entry.Version.Major)
    {
        return DirectoryCoreError::InvalidVersion;
    }

    const Foundation::UInt16 minimumMinor = query.RequiredVersion.MinimumMinor > entry.Version.MinimumMinor
                                                ? query.RequiredVersion.MinimumMinor
                                                : entry.Version.MinimumMinor;
    const Foundation::UInt16 maximumMinor = query.RequiredVersion.MaximumMinor < entry.Version.MaximumMinor
                                                ? query.RequiredVersion.MaximumMinor
                                                : entry.Version.MaximumMinor;
    if (minimumMinor > maximumMinor)
    {
        return DirectoryCoreError::InvalidVersion;
    }
    if (!query.RequiredRights.IsSubsetOf(entry.PublishedRights))
    {
        return DirectoryCoreError::InvalidRights;
    }
    if (!entry.TargetDeadline.IsInfinite() && (query.RequestedDeadline.IsInfinite() ||
                                               (query.RequestedDeadline.GetTicks() > entry.TargetDeadline.GetTicks())))
    {
        return DirectoryCoreError::LeaseUnavailable;
    }

    output = DirectoryResolution{
        .Version =
            BindingVersion{
                .DirectoryGeneration = m_directoryGeneration,
                .Revision = entry.Revision,
            },
        .ServiceInterface = entry.ServiceInterface,
        .ServiceGeneration = entry.ServiceGeneration,
        .GrantedDeadline = query.RequestedDeadline,
        .GrantedRights = query.RequiredRights,
        .Major = entry.Version.Major,
        .SelectedMinor = maximumMinor,
        .Reserved = 0U,
        .TargetEndpoint = entry.Custody,
    };
    return DirectoryCoreError::None;
}

DirectoryCoreError ObjectDirectoryCore::BeginQuiesce() noexcept
{
    if (m_state == DirectoryCoreState::InvalidConfiguration)
    {
        return DirectoryCoreError::InvalidConfiguration;
    }
    if (m_state != DirectoryCoreState::Ready)
    {
        return DirectoryCoreError::NotReady;
    }

    m_state = m_bindingCount == 0U ? DirectoryCoreState::Quiesced : DirectoryCoreState::Quiescing;
    return DirectoryCoreError::None;
}

DirectoryCoreError ObjectDirectoryCore::TakeNextOwnedEndpoint(Kernel::Object::CapabilityHandle& output) noexcept
{
    if (m_state == DirectoryCoreState::InvalidConfiguration)
    {
        return DirectoryCoreError::InvalidConfiguration;
    }
    if (m_state == DirectoryCoreState::Ready)
    {
        return DirectoryCoreError::NotReady;
    }
    if ((output.GetSelector() != 0U) || (output.GetAuthenticator() != 0U))
    {
        return DirectoryCoreError::OutputOccupied;
    }
    if (m_state == DirectoryCoreState::Quiesced)
    {
        return DirectoryCoreError::NoOwnedCapability;
    }

    for (Foundation::Size index{0U}; index < MaximumBindings; ++index)
    {
        BindingEntry& entry = m_entries[index];
        if (!entry.IsOccupied)
        {
            continue;
        }

        output = entry.Custody;
        entry = BindingEntry{};
        --m_bindingCount;
        if (m_bindingCount == 0U)
        {
            m_state = DirectoryCoreState::Quiesced;
        }
        return DirectoryCoreError::None;
    }

    m_state = DirectoryCoreState::Quiesced;
    return DirectoryCoreError::NoOwnedCapability;
}

DirectoryCoreSnapshot ObjectDirectoryCore::GetSnapshot() const noexcept
{
    return DirectoryCoreSnapshot{
        .BindingCount = m_bindingCount,
        .FreeSlotCount = MaximumBindings - m_bindingCount,
        .DirectoryGeneration = m_directoryGeneration,
        .LastIssuedRevision = m_lastIssuedRevision,
        .State = m_state,
        .Reserved = {},
    };
}
} // namespace Asteria::Executive::ObjectDirectory
