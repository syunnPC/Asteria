// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_HAL_DEVICE_I_IOMMU_DEVICE_HPP_INCLUDED
#define ASTERIA_HAL_DEVICE_I_IOMMU_DEVICE_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Kernel::Device
{
class IommuDomain;
}

namespace Asteria::Hal::Device
{
enum class IommuDeviceError : Foundation::UInt8
{
    None,
    NotReady,
    AlreadyClaimed,
    InvalidOwner,
    CorruptState,
};

/// Unique platform endpoint for one DMA requester. The platform owner supplies exactly one endpoint
/// for each hardware requester. Claim and Release are internally serialized, bounded,
/// allocation-free, callback-free, never enter IommuDomain, and change no device registers. Calls
/// are forbidden from an ISR. Bus mastering is disabled before Claim, Attach, Detach, Release, or
/// domain recovery. A failed Claim transfers no ownership and changes no endpoint state. A
/// successful claim remains with the exact domain until Release succeeds; a failed Release retains
/// that claim.
class IIommuDevice
{
public:
    [[nodiscard]] virtual IommuDeviceError Claim(const Kernel::Device::IommuDomain& owner) noexcept = 0;

    [[nodiscard]] virtual IommuDeviceError Release(const Kernel::Device::IommuDomain& owner) noexcept = 0;

    /// The requester's PCI segment in bits 16-31 and its bus/device/function
    /// requester identifier in bits 0-15, fixed for the endpoint's lifetime;
    /// the backend programs its translation by it.
    [[nodiscard]] virtual Foundation::UInt32 GetRequesterId() const noexcept = 0;

    /// Whether the boot granted the endpoint DMA without isolation under a BOOT.CFG trust entry;
    /// fixed before any claim. Only an identity backend asks: it attaches no other endpoint.
    [[nodiscard]] virtual bool HasTrustedDma() const noexcept = 0;

protected:
    constexpr IIommuDevice() noexcept = default;
    ~IIommuDevice() noexcept = default;
};

static_assert(sizeof(IommuDeviceError) == 1U);
} // Namespace Asteria::Hal::Device

#endif // ASTERIA_HAL_DEVICE_I_IOMMU_DEVICE_HPP_INCLUDED
