// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Resource/ResourceDomain.hpp"

#include "Kernel/Object/ObjectRegistry.hpp"

namespace Asteria::Kernel::Resource
{
namespace
{
// A domain that still holds charges: Ready, or Draining once its subtree's
// drain began.
[[nodiscard]] constexpr bool IsLive(const ResourceDomainState state) noexcept
{
    return (state == ResourceDomainState::Ready) || (state == ResourceDomainState::Draining);
}

constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};

[[nodiscard]] constexpr bool LimitsAreValid(const ResourceDomainLimits& limits) noexcept
{
    return limits.ResidentMemoryPages <= limits.CommittedMemoryPages;
}

[[nodiscard]] constexpr bool LimitsFitWithin(const ResourceDomainLimits& child,
                                             const ResourceDomainLimits& parent) noexcept
{
    for (Foundation::Size i{0U}; i < Abi::LimitedObjectCount; ++i)
    {
        if (child.Objects.Counts[i] > parent.Objects.Counts[i])
        {
            return false;
        }
    }
    if (child.Objects.LargePageTableArenas > parent.Objects.LargePageTableArenas)
    {
        return false;
    }
    return (child.CommittedMemoryPages <= parent.CommittedMemoryPages) &&
           (child.ResidentMemoryPages <= parent.ResidentMemoryPages) &&
           (child.KernelMetadataBytes <= parent.KernelMetadataBytes) &&
           (child.CapabilityHandles <= parent.CapabilityHandles) && (child.IpcQueueBytes <= parent.IpcQueueBytes) &&
           (child.Threads <= parent.Threads) && (child.ChildDomains <= parent.ChildDomains);
}

[[nodiscard]] constexpr bool ReservationIsZero(const ResourceReservation& reservation) noexcept
{
    for (const Foundation::UInt16 count : reservation.Objects)
    {
        if (count != 0U)
        {
            return false;
        }
    }
    return (reservation.LargePageTableArenas == 0U) && (reservation.CommittedMemoryPages == 0U) &&
           (reservation.ResidentMemoryPages == 0U) && (reservation.KernelMetadataBytes == 0U) &&
           (reservation.CapabilityHandles == 0U) && (reservation.IpcQueueBytes == 0U) && (reservation.Threads == 0U);
}

[[nodiscard]] constexpr bool UsageIsZero(const ResourceDomainUsage& usage) noexcept
{
    for (const Foundation::UInt16 count : usage.Objects)
    {
        if (count != 0U)
        {
            return false;
        }
    }
    return (usage.LargePageTableArenas == 0U) && (usage.CommittedMemoryPages == 0U) &&
           (usage.ResidentMemoryPages == 0U) && (usage.KernelMetadataBytes == 0U) && (usage.CapabilityHandles == 0U) &&
           (usage.IpcQueueBytes == 0U) && (usage.Threads == 0U) && (usage.ChildDomains == 0U);
}

[[nodiscard]] constexpr bool AdditionOverflows(const Foundation::UInt64 current,
                                               const Foundation::UInt64 addition) noexcept
{
    return addition > (MaximumUInt64 - current);
}

[[nodiscard]] constexpr bool ReservationOverflows(const ResourceDomainUsage& usage,
                                                  const ResourceReservation& reservation) noexcept
{
    return AdditionOverflows(usage.CommittedMemoryPages, reservation.CommittedMemoryPages) ||
           AdditionOverflows(usage.ResidentMemoryPages, reservation.ResidentMemoryPages) ||
           AdditionOverflows(usage.KernelMetadataBytes, reservation.KernelMetadataBytes) ||
           AdditionOverflows(usage.CapabilityHandles, reservation.CapabilityHandles) ||
           AdditionOverflows(usage.IpcQueueBytes, reservation.IpcQueueBytes) ||
           AdditionOverflows(usage.Threads, reservation.Threads);
}

[[nodiscard]] constexpr bool ReservationFits(const ResourceDomainUsage& usage, const ResourceDomainLimits& limits,
                                             const ResourceReservation& reservation) noexcept
{
    const Foundation::UInt64 committed = usage.CommittedMemoryPages + reservation.CommittedMemoryPages;
    const Foundation::UInt64 resident = usage.ResidentMemoryPages + reservation.ResidentMemoryPages;
    return (committed <= limits.CommittedMemoryPages) && (resident <= limits.ResidentMemoryPages) &&
           (resident <= committed) &&
           ((usage.KernelMetadataBytes + reservation.KernelMetadataBytes) <= limits.KernelMetadataBytes) &&
           ((usage.CapabilityHandles + reservation.CapabilityHandles) <= limits.CapabilityHandles) &&
           ((usage.IpcQueueBytes + reservation.IpcQueueBytes) <= limits.IpcQueueBytes) &&
           ((usage.Threads + reservation.Threads) <= limits.Threads);
}

void AddReservation(ResourceDomainUsage& usage, const ResourceReservation& reservation) noexcept
{
    usage.CommittedMemoryPages += reservation.CommittedMemoryPages;
    usage.ResidentMemoryPages += reservation.ResidentMemoryPages;
    usage.KernelMetadataBytes += reservation.KernelMetadataBytes;
    usage.CapabilityHandles += reservation.CapabilityHandles;
    usage.IpcQueueBytes += reservation.IpcQueueBytes;
    usage.Threads += reservation.Threads;
    usage.LargePageTableArenas =
        static_cast<Foundation::UInt16>(usage.LargePageTableArenas + reservation.LargePageTableArenas);
    for (Foundation::Size i{0U}; i < Abi::LimitedObjectCount; ++i)
    {
        usage.Objects[i] = static_cast<Foundation::UInt16>(usage.Objects[i] + reservation.Objects[i]);
    }
}

[[nodiscard]] constexpr bool CanReleaseReservation(const ResourceDomainUsage& usage,
                                                   const ResourceReservation& reservation) noexcept
{
    for (Foundation::Size i{0U}; i < Abi::LimitedObjectCount; ++i)
    {
        if (reservation.Objects[i] > usage.Objects[i])
        {
            return false;
        }
    }
    if ((reservation.LargePageTableArenas > usage.LargePageTableArenas) ||
        (reservation.CommittedMemoryPages > usage.CommittedMemoryPages) ||
        (reservation.ResidentMemoryPages > usage.ResidentMemoryPages) ||
        (reservation.KernelMetadataBytes > usage.KernelMetadataBytes) ||
        (reservation.CapabilityHandles > usage.CapabilityHandles) ||
        (reservation.IpcQueueBytes > usage.IpcQueueBytes) || (reservation.Threads > usage.Threads))
    {
        return false;
    }
    const Foundation::UInt64 committed = usage.CommittedMemoryPages - reservation.CommittedMemoryPages;
    const Foundation::UInt64 resident = usage.ResidentMemoryPages - reservation.ResidentMemoryPages;
    return resident <= committed;
}

void SubtractReservation(ResourceDomainUsage& usage, const ResourceReservation& reservation) noexcept
{
    usage.CommittedMemoryPages -= reservation.CommittedMemoryPages;
    usage.ResidentMemoryPages -= reservation.ResidentMemoryPages;
    usage.KernelMetadataBytes -= reservation.KernelMetadataBytes;
    usage.CapabilityHandles -= reservation.CapabilityHandles;
    usage.IpcQueueBytes -= reservation.IpcQueueBytes;
    usage.Threads -= reservation.Threads;
    usage.LargePageTableArenas =
        static_cast<Foundation::UInt16>(usage.LargePageTableArenas - reservation.LargePageTableArenas);
    for (Foundation::Size i{0U}; i < Abi::LimitedObjectCount; ++i)
    {
        usage.Objects[i] = static_cast<Foundation::UInt16>(usage.Objects[i] - reservation.Objects[i]);
    }
}
} // Namespace

ResourceDomainFailure ResourceDomain::MakeFailure(const ResourceDomainError error,
                                                  const Object::ObjectPinError pin) noexcept
{
    return ResourceDomainFailure{
        .Error = error,
        .Pin = pin,
        .Reserved = {},
    };
}

bool ResourceDomain::FinalizeMemoryImport(const Foundation::UInt64 pages) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_ledger.m_synchronization};
    if (m_state != ResourceDomainState::Ready || m_parent != nullptr || m_memoryImported ||
        pages < m_usage.CommittedMemoryPages || pages < m_usage.ResidentMemoryPages)
    {
        return false;
    }
    m_limits.CommittedMemoryPages = pages;
    m_limits.ResidentMemoryPages = pages;
    m_memoryImported = true;
    return true;
}

ResourceDomainFailure ResourceDomain::InitializeRoot(const Object::ResourceDomainId identity,
                                                     const ResourceDomainLimits& limits) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_ledger.m_synchronization};
    if (m_state != ResourceDomainState::Uninitialized)
    {
        return MakeFailure(ResourceDomainError::AlreadyInitialized);
    }
    if (!identity.IsValid() || !LimitsAreValid(limits))
    {
        return MakeFailure(ResourceDomainError::InvalidConfiguration);
    }

    m_parent = nullptr;
    m_parentIdentity = ResourceDomainObjectId{};
    m_identity = identity;
    m_limits = limits;
    m_usage = ResourceDomainUsage{};
    m_depth = 0U;
    m_state = ResourceDomainState::Ready;
    m_chargeOwnerValue.StoreRelease(identity.GetValue());
    return MakeFailure(ResourceDomainError::None);
}

ResourceDomainFailure ResourceDomain::InitializeChild(Object::ObjectRegistry& registry,
                                                      const Object::ResourceDomainId identity,
                                                      const ResourceDomainObjectId parentIdentity,
                                                      ResourceDomain& parent,
                                                      const ResourceDomainLimits& limits) noexcept
{
    {
        Platform::Synchronization::SpinLockGuard guard{m_ledger.m_synchronization};
        if (m_state != ResourceDomainState::Uninitialized)
        {
            return MakeFailure(ResourceDomainError::AlreadyInitialized);
        }
        if (!identity.IsValid() || !parentIdentity.IsValid() || !parentIdentity.IsFromRegistry(registry) ||
            (&parent == this) || !LimitsAreValid(limits))
        {
            return MakeFailure(ResourceDomainError::InvalidConfiguration);
        }
        m_state = ResourceDomainState::Initializing;
    }

    const Object::ObjectPinError pinError =
        registry.PinObject(parentIdentity.GetObjectId(), Object::ObjectType::ResourceDomain, m_parentPin);
    if (pinError != Object::ObjectPinError::Ok)
    {
        Platform::Synchronization::SpinLockGuard guard{m_ledger.m_synchronization};
        m_state = ResourceDomainState::Uninitialized;
        return MakeFailure(ResourceDomainError::ParentPinRejected, pinError);
    }
    if (&m_parentPin.GetObject() != static_cast<Object::IKernelObject*>(&parent))
    {
        m_parentPin.Reset();
        Platform::Synchronization::SpinLockGuard guard{m_ledger.m_synchronization};
        m_state = ResourceDomainState::Uninitialized;
        return MakeFailure(ResourceDomainError::ParentInstanceMismatch);
    }

    ResourceDomainError validationError{ResourceDomainError::None};
    {
        Platform::Synchronization::SpinLockGuard guard{m_ledger.m_synchronization};
        if ((&parent.m_ledger != &m_ledger) || (parent.m_state != ResourceDomainState::Ready))
        {
            validationError = (parent.m_state == ResourceDomainState::Draining) ? ResourceDomainError::Draining
                                                                                : ResourceDomainError::ParentNotReady;
        }
        else if ((identity.GetValue() == parent.m_identity.GetValue()) || !LimitsFitWithin(limits, parent.m_limits))
        {
            validationError = ResourceDomainError::InvalidConfiguration;
        }
        else if (parent.m_depth >= MaximumResourceDomainDepth)
        {
            validationError = ResourceDomainError::DepthExceeded;
        }
        else
        {
            Foundation::UInt8 traversed{0U};
            for (ResourceDomain* current{&parent}; current != nullptr; current = current->m_parent)
            {
                if (current->m_state == ResourceDomainState::Draining)
                {
                    validationError = ResourceDomainError::Draining;
                    break;
                }
                if ((current->m_state != ResourceDomainState::Ready) || (traversed > MaximumResourceDomainDepth))
                {
                    validationError = ResourceDomainError::InvariantViolation;
                    break;
                }
                if (current->m_usage.ChildDomains == MaximumUInt64)
                {
                    validationError = ResourceDomainError::ArithmeticOverflow;
                    break;
                }
                if (current->m_usage.ChildDomains >= current->m_limits.ChildDomains)
                {
                    validationError = ResourceDomainError::LimitExceeded;
                    break;
                }
                ++traversed;
            }
            if ((validationError == ResourceDomainError::None) &&
                (traversed != static_cast<Foundation::UInt8>(parent.m_depth + 1U)))
            {
                validationError = ResourceDomainError::InvariantViolation;
            }
            if (validationError == ResourceDomainError::None)
            {
                for (ResourceDomain* current{&parent}; current != nullptr; current = current->m_parent)
                {
                    ++current->m_usage.ChildDomains;
                }
                m_parent = &parent;
                m_parentIdentity = parentIdentity;
                m_identity = identity;
                m_limits = limits;
                m_usage = ResourceDomainUsage{};
                m_depth = static_cast<Foundation::UInt8>(parent.m_depth + 1U);
                m_state = ResourceDomainState::Ready;
                m_chargeOwnerValue.StoreRelease(parent.m_identity.GetValue());
            }
        }
    }

    if (validationError != ResourceDomainError::None)
    {
        m_parentPin.Reset();
        Platform::Synchronization::SpinLockGuard guard{m_ledger.m_synchronization};
        m_state = ResourceDomainState::Uninitialized;
        return MakeFailure(validationError);
    }
    return MakeFailure(ResourceDomainError::None);
}

ResourceDomainError ResourceDomain::TryReserve(const ResourceReservation& reservation) noexcept
{
    if (ReservationIsZero(reservation))
    {
        return ResourceDomainError::InvalidReservation;
    }

    Platform::Synchronization::SpinLockGuard guard{m_ledger.m_synchronization};
    if (m_state == ResourceDomainState::Quiesced)
    {
        return ResourceDomainError::Quiesced;
    }
    if (m_state == ResourceDomainState::Draining)
    {
        return ResourceDomainError::Draining;
    }
    if (m_state != ResourceDomainState::Ready)
    {
        return ResourceDomainError::NotReady;
    }

    Foundation::UInt8 traversed{0U};
    for (ResourceDomain* current{this}; current != nullptr; current = current->m_parent)
    {
        // A drain above refuses every new charge below it.
        if (current->m_state == ResourceDomainState::Draining)
        {
            return ResourceDomainError::Draining;
        }
        if ((current->m_state != ResourceDomainState::Ready) || (traversed > MaximumResourceDomainDepth))
        {
            return ResourceDomainError::InvariantViolation;
        }
        if (ReservationOverflows(current->m_usage, reservation))
        {
            return ResourceDomainError::ArithmeticOverflow;
        }
        for (Foundation::Size type{0U}; type < Abi::LimitedObjectCount; ++type)
        {
            if (static_cast<Foundation::UInt64>(current->m_usage.Objects[type]) + reservation.Objects[type] >
                current->m_limits.Objects.Counts[type])
            {
                return ResourceDomainError::ObjectLimitExceeded;
            }
        }
        // Reserve through every ancestor, like object counts.
        if (static_cast<Foundation::UInt64>(current->m_usage.LargePageTableArenas) + reservation.LargePageTableArenas >
            current->m_limits.Objects.LargePageTableArenas)
        {
            return ResourceDomainError::ObjectLimitExceeded;
        }
        if (!ReservationFits(current->m_usage, current->m_limits, reservation))
        {
            return ResourceDomainError::LimitExceeded;
        }
        ++traversed;
    }
    if (traversed != static_cast<Foundation::UInt8>(m_depth + 1U))
    {
        return ResourceDomainError::InvariantViolation;
    }

    for (ResourceDomain* current{this}; current != nullptr; current = current->m_parent)
    {
        AddReservation(current->m_usage, reservation);
    }
    return ResourceDomainError::None;
}

ResourceDomainError ResourceDomain::Release(const ResourceReservation& reservation) noexcept
{
    if (ReservationIsZero(reservation))
    {
        return ResourceDomainError::InvalidReservation;
    }

    Platform::Synchronization::SpinLockGuard guard{m_ledger.m_synchronization};
    if (m_state == ResourceDomainState::Quiesced)
    {
        return ResourceDomainError::Quiesced;
    }
    if (!IsLive(m_state))
    {
        return ResourceDomainError::NotReady;
    }

    Foundation::UInt8 traversed{0U};
    for (ResourceDomain* current{this}; current != nullptr; current = current->m_parent)
    {
        if (!IsLive(current->m_state) || (traversed > MaximumResourceDomainDepth))
        {
            return ResourceDomainError::InvariantViolation;
        }
        if (!CanReleaseReservation(current->m_usage, reservation))
        {
            return ResourceDomainError::ReservationUnderflow;
        }
        ++traversed;
    }
    if (traversed != static_cast<Foundation::UInt8>(m_depth + 1U))
    {
        return ResourceDomainError::InvariantViolation;
    }

    for (ResourceDomain* current{this}; current != nullptr; current = current->m_parent)
    {
        SubtractReservation(current->m_usage, reservation);
    }
    return ResourceDomainError::None;
}

ResourceDomainError ResourceDomain::Quiesce() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_ledger.m_synchronization};
    if (m_state == ResourceDomainState::Quiesced)
    {
        return ResourceDomainError::Quiesced;
    }
    if (!IsLive(m_state))
    {
        return ResourceDomainError::NotReady;
    }
    if (!UsageIsZero(m_usage))
    {
        return ResourceDomainError::InUse;
    }

    ResourceDomain* const parent = m_parent;
    if (parent != nullptr)
    {
        Foundation::UInt8 traversed{0U};
        for (ResourceDomain* current{parent}; current != nullptr; current = current->m_parent)
        {
            if (!IsLive(current->m_state) || (current->m_usage.ChildDomains == 0U) ||
                (traversed > MaximumResourceDomainDepth))
            {
                return ResourceDomainError::InvariantViolation;
            }
            ++traversed;
        }
        if (traversed != m_depth)
        {
            return ResourceDomainError::InvariantViolation;
        }
        for (ResourceDomain* current{parent}; current != nullptr; current = current->m_parent)
        {
            --current->m_usage.ChildDomains;
        }
    }

    m_parent = nullptr;
    m_parentPin.Reset();
    m_state = ResourceDomainState::Quiesced;
    return ResourceDomainError::None;
}

bool ResourceDomain::IsDescendantOf(const ResourceDomain& ancestor) const noexcept
{
    if (&m_ledger != &ancestor.m_ledger)
    {
        return false;
    }
    Platform::Synchronization::SpinLockGuard guard{m_ledger.m_synchronization};
    const ResourceDomain* current{this};
    for (Foundation::Size depth{0U}; current != nullptr && depth <= MaximumResourceDomainDepth; ++depth)
    {
        if (current == &ancestor)
        {
            return true;
        }
        current = current->m_parent;
    }
    return false;
}

bool ResourceDomain::IsReady() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_ledger.m_synchronization};
    return m_state == ResourceDomainState::Ready;
}

ResourceDomainError ResourceDomain::BeginDrain() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_ledger.m_synchronization};
    if (m_state == ResourceDomainState::Quiesced)
    {
        return ResourceDomainError::Quiesced;
    }
    if (!IsLive(m_state))
    {
        return ResourceDomainError::NotReady;
    }
    m_state = ResourceDomainState::Draining;
    return ResourceDomainError::None;
}

Object::ResourceDomainId ResourceDomain::GetIdentifier() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_ledger.m_synchronization};
    if (!IsLive(m_state))
    {
        return Object::ResourceDomainId{};
    }
    return m_identity;
}

ResourceDomainUsage ResourceDomain::GetUsage() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_ledger.m_synchronization};
    return m_usage;
}

ResourceDomainSnapshot ResourceDomain::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_ledger.m_synchronization};
    return ResourceDomainSnapshot{
        .Identity = m_identity,
        .Parent = m_parentIdentity,
        .Limits = m_limits,
        .Usage = m_usage,
        .Depth = m_depth,
        .State = m_state,
        .Reserved = {},
    };
}

Object::ObjectType ResourceDomain::GetObjectType() const noexcept
{
    return Object::ObjectType::ResourceDomain;
}

Object::ResourceDomainId ResourceDomain::GetChargeOwner() const noexcept
{
    return Object::ResourceDomainId{m_chargeOwnerValue.LoadAcquire()};
}
} // Namespace Asteria::Kernel::Resource
