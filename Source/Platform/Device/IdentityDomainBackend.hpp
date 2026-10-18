// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_DEVICE_IDENTITY_DOMAIN_BACKEND_HPP_INCLUDED
#define ASTERIA_PLATFORM_DEVICE_IDENTITY_DOMAIN_BACKEND_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Hal/Device/IIommuDevice.hpp"
#include "Hal/Device/IIommuDomainBackend.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Platform::Device
{
/// The trusted-driver profile's IommuDomain backend, for machines where no usable IOMMU serves a
/// function BOOT.CFG trusts. No hardware translates: a mapping's device address is its physical
/// address (ChooseDeviceAddress), which the IommuDomain still checks against the domain's input
/// width, so a driver of a device limited to 32-bit DMA is never handed an address its device would
/// truncate. Each domain's width follows its device, including a
/// 64-bit device whose identity mapping may name high RAM. This backend confines nothing. It keeps
/// the IommuDomain's bookkeeping (which Frames the driver named, pinned while mapped and parked
/// with a quarantined function after a drain) and the attachment rule: only an endpoint the boot
/// granted trusted DMA (IIommuDevice::HasTrustedDma) attaches, so bus mastering stays refused for
/// every other function. The device itself can read and write any physical address while it masters
/// the bus: unlike the IIommuDomainBackend contract, Unmap and Release cannot prove that a transfer
/// which used a range ended. That is why a trusted driver and its device join the trusted computing
/// base, and why the profile is never called isolation. Ownership transitions take the backend's
/// leaf lock; every other method is lock-free and touches only constant state. Bounded,
/// allocation-free, callback-free; forbidden from an ISR, as the interface requires.
class IdentityDomainBackend final : public Hal::Device::IIommuDomainBackend
{
public:
    /// The narrowest and widest domains: the ABI's 32-bit minimum, and the
    /// full 64-bit physical space.
    static constexpr Foundation::UInt8 MinimumInputAddressBits{32U};
    static constexpr Foundation::UInt8 MaximumInputAddressBits{64U};

    constexpr IdentityDomainBackend() noexcept = default;
    ~IdentityDomainBackend() noexcept = default;

    IdentityDomainBackend(const IdentityDomainBackend&) = delete;
    IdentityDomainBackend& operator=(const IdentityDomainBackend&) = delete;

    [[nodiscard]] bool IsReady() const noexcept override;
    [[nodiscard]] Hal::Device::IommuBackendOwnershipError Claim(const Kernel::Device::IommuDomain& owner,
                                                                Foundation::UInt8 inputAddressBits) noexcept override;
    [[nodiscard]] Hal::Device::IommuBackendOwnershipError
    Release(const Kernel::Device::IommuDomain& owner) noexcept override;
    /// The physical address: device address equals physical address.
    [[nodiscard]] Kernel::Device::DmaAddress
    ChooseDeviceAddress(Kernel::Device::DmaAddress requested,
                        Kernel::Memory::PhysicalAddress physicalStart) const noexcept override;
    /// InvalidRequest unless the device address is the physical one; nothing
    /// to program otherwise.
    [[nodiscard]] Hal::Device::IommuBackendError
    Map(const Hal::Device::IommuBackendMapRequest& request) noexcept override;
    [[nodiscard]] Hal::Device::IommuBackendError
    Unmap(const Hal::Device::IommuBackendMapRequest& request) noexcept override;
    /// UnsupportedConfiguration, an ordinary refusal, for an endpoint without
    /// trusted DMA.
    [[nodiscard]] Hal::Device::IommuBackendError Attach(Hal::Device::IIommuDevice& device) noexcept override;
    [[nodiscard]] Hal::Device::IommuBackendError Detach(Hal::Device::IIommuDevice& device) noexcept override;
    /// Always zero: nothing records a fault.
    [[nodiscard]] Hal::Device::IommuBackendFaults GetFaults() noexcept override;

private:
    [[nodiscard]] static bool IsIdentity(const Hal::Device::IommuBackendMapRequest& request) noexcept;

    mutable Synchronization::SpinLock m_synchronization;
    const Kernel::Device::IommuDomain* m_owner{nullptr};
};
} // Namespace Asteria::Platform::Device

#endif // ASTERIA_PLATFORM_DEVICE_IDENTITY_DOMAIN_BACKEND_HPP_INCLUDED
