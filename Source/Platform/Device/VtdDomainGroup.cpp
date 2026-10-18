// SPDX-License-Identifier: Apache-2.0

#include "Platform/Device/VtdDomainGroup.hpp"

#include "Hal/Device/IIommuDevice.hpp"

namespace Asteria::Platform::Device
{
namespace
{
using Foundation::Size;
using Hal::Device::IommuBackendError;
using Hal::Device::IommuBackendOwnershipError;
}

VtdDomainGroup::VtdDomainGroup(VtdDomainBackend* const (&backends)[MaximumUnits], VtdUnit* const (&units)[MaximumUnits],
                               const Size count) noexcept
{
    if (count > MaximumUnits)
    {
        return;
    }
    for (Size index{0U}; index < count; ++index)
    {
        if ((backends[index] == nullptr) || (units[index] == nullptr))
        {
            return;
        }
        m_backends[index] = backends[index];
        m_units[index] = units[index];
    }
    m_count = count;
}

bool VtdDomainGroup::IsReady() const noexcept
{
    if (m_count == 0U)
    {
        return false;
    }
    for (Size index{0U}; index < m_count; ++index)
    {
        if (!m_backends[index]->IsReady())
        {
            return false;
        }
    }
    return true;
}

IommuBackendOwnershipError VtdDomainGroup::Claim(const Kernel::Device::IommuDomain& owner,
                                                 const Foundation::UInt8 inputAddressBits) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_ownership};
    if (!IsReady())
    {
        return IommuBackendOwnershipError::NotReady;
    }
    for (Size index{0U}; index < m_count; ++index)
    {
        if (m_claimed[index])
        {
            return IommuBackendOwnershipError::AlreadyOwned;
        }
        if ((inputAddressBits < 12U) || (inputAddressBits > 64U))
        {
            return IommuBackendOwnershipError::UnsupportedConfiguration;
        }
    }
    for (Size index{0U}; index < m_count; ++index)
    {
        const auto result = m_backends[index]->Claim(owner, inputAddressBits);
        if (result != IommuBackendOwnershipError::None)
        {
            for (Size prior{0U}; prior < index; ++prior)
            {
                if (!m_backends[prior]->CancelClaim(owner))
                {
                    return IommuBackendOwnershipError::HardwareFailure;
                }
                m_claimed[prior] = false;
            }
            return result;
        }
        m_claimed[index] = true;
    }
    m_owner = &owner;
    return IommuBackendOwnershipError::None;
}

IommuBackendOwnershipError VtdDomainGroup::Release(const Kernel::Device::IommuDomain& owner) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_ownership};
    if (m_owner != &owner)
    {
        return IommuBackendOwnershipError::InvalidOwner;
    }
    for (Size index{0U}; index < m_count; ++index)
    {
        if (!m_claimed[index])
        {
            continue;
        }
        const auto result = m_backends[index]->Release(owner);
        if (result != IommuBackendOwnershipError::None)
        {
            return result;
        }
        m_claimed[index] = false;
    }
    m_owner = nullptr;
    return IommuBackendOwnershipError::None;
}

IommuBackendError VtdDomainGroup::Map(const Hal::Device::IommuBackendMapRequest& request) noexcept
{
    if (m_owner == nullptr)
    {
        return IommuBackendError::NotReady;
    }
    for (Size index{0U}; index < m_count; ++index)
    {
        const auto result = m_backends[index]->CheckMap(request);
        if (result != IommuBackendError::None)
        {
            return result;
        }
    }
    for (Size index{0U}; index < m_count; ++index)
    {
        if (m_backends[index]->Map(request) != IommuBackendError::None)
        {
            return IommuBackendError::CommittedStateUnknown;
        }
    }
    return IommuBackendError::None;
}

IommuBackendError VtdDomainGroup::Unmap(const Hal::Device::IommuBackendMapRequest& request) noexcept
{
    if (m_owner == nullptr)
    {
        return IommuBackendError::NotReady;
    }
    for (Size index{0U}; index < m_count; ++index)
    {
        if (m_backends[index]->Unmap(request) != IommuBackendError::None)
        {
            return IommuBackendError::CommittedStateUnknown;
        }
    }
    return IommuBackendError::None;
}

VtdDomainBackend* VtdDomainGroup::Route(Hal::Device::IIommuDevice& device) const noexcept
{
    const Foundation::UInt32 requester = device.GetRequesterId();
    if ((requester >> 16U) != 0U)
    {
        return nullptr;
    }
    for (Size index{0U}; index < m_count; ++index)
    {
        if (m_units[index]->Serves(static_cast<Foundation::UInt16>(requester)))
        {
            return m_backends[index];
        }
    }
    return nullptr;
}

IommuBackendError VtdDomainGroup::Attach(Hal::Device::IIommuDevice& device) noexcept
{
    VtdDomainBackend* const backend = Route(device);
    return (backend == nullptr) ? IommuBackendError::UnsupportedConfiguration : backend->Attach(device);
}

IommuBackendError VtdDomainGroup::Detach(Hal::Device::IIommuDevice& device) noexcept
{
    VtdDomainBackend* const backend = Route(device);
    return (backend == nullptr) ? IommuBackendError::InvalidRequest : backend->Detach(device);
}

Hal::Device::IommuBackendFaults VtdDomainGroup::GetFaults() noexcept
{
    Hal::Device::IommuBackendFaults output{};
    for (Size index{0U}; index < m_count; ++index)
    {
        const auto faults = m_backends[index]->GetFaults();
        output.Count += faults.Count;
        if (faults.Count != 0U)
        {
            output.LastSource = faults.LastSource;
            output.LastReason = faults.LastReason;
            output.LastAddress = faults.LastAddress;
        }
    }
    return output;
}
} // namespace Asteria::Platform::Device
