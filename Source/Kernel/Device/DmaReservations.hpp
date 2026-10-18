// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_DEVICE_DMA_RESERVATIONS_HPP_INCLUDED
#define ASTERIA_KERNEL_DEVICE_DMA_RESERVATIONS_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Kernel::Device
{
/// The device-address ranges firmware reserves for DMA requesters: the ranges of the VT-d RMRRs
/// whose scope names a requester, or of the AMD-Vi IVMD unity and exclusion blocks that cover it.
/// An IommuDomain never maps a device address overlapping a range reserved for a requester it
/// serves, whether firmware has released that requester or not : a schedule firmware left in that
/// memory must never reach a page a driver mapped there. Immutable once the boot configured it;
/// IommuDomain calls it with its own lock held, so an implementation takes no lock, allocates
/// nothing and is bounded.
class IDmaReservations
{
public:
    /// Whether the inclusive device-address range [first, last] overlaps a range reserved for
    /// requester (Hal::Device::IIommuDevice's identifier: segment in bits 16-31,
    /// bus/device/function in bits 0-15).
    [[nodiscard]] virtual bool Overlaps(Foundation::UInt32 requester, Foundation::UInt64 first,
                                        Foundation::UInt64 last) const noexcept = 0;

protected:
    constexpr IDmaReservations() noexcept = default;
    ~IDmaReservations() noexcept = default;
};
} // Namespace Asteria::Kernel::Device

#endif // ASTERIA_KERNEL_DEVICE_DMA_RESERVATIONS_HPP_INCLUDED
