// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_DEVICE_PCI_ECAM_ADDRESS_HPP_INCLUDED
#define ASTERIA_KERNEL_DEVICE_PCI_ECAM_ADDRESS_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Device/PciTypes.hpp"

namespace Asteria::Kernel::Device
{
enum class PciEcamAddressError : Foundation::UInt8
{
    None,
    InvalidRegion,
    AddressOutsideRegion,
    InvalidFunctionAddress,
    InvalidRegisterOffset,
    AddressOverflow,
};

struct PciEcamRegion
{
    Foundation::UInt64 PhysicalStart;
    Foundation::UInt16 Segment;
    Foundation::UInt8 FirstBus;
    Foundation::UInt8 LastBus;
    Foundation::UInt8 Padding[4];
};

struct PciEcamAddressRequest
{
    PciEcamRegion Region;
    PciFunctionAddress Address;
    Foundation::UInt16 RegisterOffset;
};

/// Pure PCI Express ECAM DWORD-address calculation. This class does not map or
/// access MMIO and does not assume that an identity mapping remains installed.
class PciEcamAddressCalculator final
{
public:
    [[nodiscard]] static constexpr Foundation::Result<Foundation::UInt64, PciEcamAddressError>
    Calculate(const PciEcamAddressRequest& request) noexcept
    {
        using Result = Foundation::Result<Foundation::UInt64, PciEcamAddressError>;
        constexpr Foundation::UInt64 maximumUInt64{~Foundation::UInt64{0U}};
        constexpr Foundation::UInt64 busBytes{Foundation::UInt64{1U} << 20U};

        if (((request.Region.PhysicalStart & (busBytes - 1U)) != 0U) ||
            (request.Region.FirstBus > request.Region.LastBus))
        {
            return Result::Failure(PciEcamAddressError::InvalidRegion);
        }
        const Foundation::UInt64 busCount = static_cast<Foundation::UInt64>(request.Region.LastBus) -
                                            static_cast<Foundation::UInt64>(request.Region.FirstBus) + 1U;
        const Foundation::UInt64 regionLastOffset = (busCount << 20U) - 1U;
        if (request.Region.PhysicalStart > (maximumUInt64 - regionLastOffset))
        {
            return Result::Failure(PciEcamAddressError::AddressOverflow);
        }
        if (!request.Address.IsValid())
        {
            return Result::Failure(PciEcamAddressError::InvalidFunctionAddress);
        }
        if ((request.Address.GetSegment() != request.Region.Segment) ||
            (request.Address.GetBus() < request.Region.FirstBus) || (request.Address.GetBus() > request.Region.LastBus))
        {
            return Result::Failure(PciEcamAddressError::AddressOutsideRegion);
        }
        if ((request.RegisterOffset > 0x0FFCU) || ((request.RegisterOffset & 3U) != 0U))
        {
            return Result::Failure(PciEcamAddressError::InvalidRegisterOffset);
        }

        const Foundation::UInt64 relativeBus = static_cast<Foundation::UInt64>(request.Address.GetBus()) -
                                               static_cast<Foundation::UInt64>(request.Region.FirstBus);
        const Foundation::UInt64 configurationOffset =
            (relativeBus << 20U) | (static_cast<Foundation::UInt64>(request.Address.GetDevice()) << 15U) |
            (static_cast<Foundation::UInt64>(request.Address.GetFunction()) << 12U) | request.RegisterOffset;
        if (request.Region.PhysicalStart > (maximumUInt64 - configurationOffset))
        {
            return Result::Failure(PciEcamAddressError::AddressOverflow);
        }
        return Result::Success(request.Region.PhysicalStart + configurationOffset);
    }
};

static_assert(sizeof(PciEcamAddressError) == 1U);
static_assert(sizeof(PciEcamRegion) == 16U);
static_assert(alignof(PciEcamRegion) == 8U);
static_assert(sizeof(PciEcamAddressRequest) == 24U);
static_assert(alignof(PciEcamAddressRequest) == 8U);
} // namespace Asteria::Kernel::Device

#endif // ASTERIA_KERNEL_DEVICE_PCI_ECAM_ADDRESS_HPP_INCLUDED
