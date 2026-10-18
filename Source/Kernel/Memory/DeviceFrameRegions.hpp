// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_DEVICE_FRAME_REGIONS_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_DEVICE_FRAME_REGIONS_HPP_INCLUDED

#include "Kernel/Device/DeviceRegion.hpp"
#include "Kernel/Memory/Frame.hpp"

namespace Asteria::Kernel::Memory
{
/// Names the DeviceRegion whose pages a device Frame holds, which a mapping of the Frame retains.
/// The pool that created the Frame answers; a RAM Frame, or one it did not create, has none.
class IDeviceFrameRegions
{
public:
    [[nodiscard]] virtual Device::DeviceRegionId GetDeviceRegion(const Frame& frame) const noexcept = 0;

protected:
    constexpr IDeviceFrameRegions() noexcept = default;
    ~IDeviceFrameRegions() noexcept = default;
};
} // Namespace Asteria::Kernel::Memory

#endif // ASTERIA_KERNEL_MEMORY_DEVICE_FRAME_REGIONS_HPP_INCLUDED
