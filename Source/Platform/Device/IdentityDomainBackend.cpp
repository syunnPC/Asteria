// SPDX-License-Identifier: Apache-2.0

#include "Platform/Device/IdentityDomainBackend.hpp"

namespace Asteria::Platform::Device
{
using Hal::Device::IommuBackendError;
using Hal::Device::IommuBackendOwnershipError;

bool IdentityDomainBackend::IsReady() const noexcept
{
    return true;
}

IommuBackendOwnershipError IdentityDomainBackend::Claim(const Kernel::Device::IommuDomain& owner,
                                                        const Foundation::UInt8 inputAddressBits) noexcept
{
    Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_owner != nullptr)
    {
        return IommuBackendOwnershipError::AlreadyOwned;
    }
    if ((inputAddressBits < MinimumInputAddressBits) || (inputAddressBits > MaximumInputAddressBits))
    {
        return IommuBackendOwnershipError::UnsupportedConfiguration;
    }
    m_owner = &owner;
    return IommuBackendOwnershipError::None;
}

IommuBackendOwnershipError IdentityDomainBackend::Release(const Kernel::Device::IommuDomain& owner) noexcept
{
    Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_owner != &owner)
    {
        return IommuBackendOwnershipError::InvalidOwner;
    }
    m_owner = nullptr;
    return IommuBackendOwnershipError::None;
}

Kernel::Device::DmaAddress
IdentityDomainBackend::ChooseDeviceAddress(const Kernel::Device::DmaAddress,
                                           const Kernel::Memory::PhysicalAddress physicalStart) const noexcept
{
    return Kernel::Device::DmaAddress{physicalStart.GetValue()};
}

bool IdentityDomainBackend::IsIdentity(const Hal::Device::IommuBackendMapRequest& request) noexcept
{
    return (request.DmaStart.GetValue() == request.PhysicalStart.GetValue()) && (request.Pages.GetValue() != 0U);
}

IommuBackendError IdentityDomainBackend::Map(const Hal::Device::IommuBackendMapRequest& request) noexcept
{
    return IsIdentity(request) ? IommuBackendError::None : IommuBackendError::InvalidRequest;
}

IommuBackendError IdentityDomainBackend::Unmap(const Hal::Device::IommuBackendMapRequest& request) noexcept
{
    return IsIdentity(request) ? IommuBackendError::None : IommuBackendError::InvalidRequest;
}

IommuBackendError IdentityDomainBackend::Attach(Hal::Device::IIommuDevice& device) noexcept
{
    return device.HasTrustedDma() ? IommuBackendError::None : IommuBackendError::UnsupportedConfiguration;
}

IommuBackendError IdentityDomainBackend::Detach(Hal::Device::IIommuDevice&) noexcept
{
    return IommuBackendError::None;
}

Hal::Device::IommuBackendFaults IdentityDomainBackend::GetFaults() noexcept
{
    return Hal::Device::IommuBackendFaults{};
}
} // namespace Asteria::Platform::Device
