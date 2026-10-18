// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Device/PciEndpointBarDecoder.hpp"

namespace Asteria::Kernel::Device
{
namespace
{
constexpr Foundation::UInt8 MaximumBarIndex{5U};
constexpr Foundation::UInt16 FirstBarRegisterOffset{0x10U};
constexpr Foundation::UInt16 RegisterWidthBytes{4U};
constexpr Foundation::UInt32 IoSpaceIndicatorMask{1U << 0U};
constexpr Foundation::UInt32 IoReservedMask{1U << 1U};
constexpr Foundation::UInt32 IoBaseMask{0xFFFF'FFFCU};
constexpr Foundation::UInt32 MemoryTypeMask{0b11U << 1U};
constexpr Foundation::UInt32 Memory32Type{0b00U << 1U};
constexpr Foundation::UInt32 Memory64Type{0b10U << 1U};
constexpr Foundation::UInt32 MemoryPrefetchableMask{1U << 3U};
constexpr Foundation::UInt32 MemoryBaseMask{0xFFFF'FFF0U};
constexpr Foundation::UInt64 MinimumIoExtentBytes{4U};
constexpr Foundation::UInt64 MinimumMemoryExtentBytes{16U};

[[nodiscard]] constexpr bool IsPowerOfTwo(const Foundation::UInt64 value) noexcept
{
    return (value != 0U) && ((value & (value - 1U)) == 0U);
}

[[nodiscard]] constexpr bool ExtentIsValid(const PciEndpointBarDecodeRequest& request) noexcept
{
    switch (request.ExtentState)
    {
    case PciEndpointBarExtentState::Unknown:
        return request.ExtentBytes == 0U;

    case PciEndpointBarExtentState::Authoritative:
        return IsPowerOfTwo(request.ExtentBytes);
    }
    return false;
}

[[nodiscard]] constexpr Foundation::UInt16 GetBarRegisterOffset(const Foundation::UInt8 barIndex) noexcept
{
    return static_cast<Foundation::UInt16>(FirstBarRegisterOffset +
                                           (static_cast<Foundation::UInt16>(barIndex) * RegisterWidthBytes));
}

[[nodiscard]] PciEndpointBarDecodeError ValidateDecodedRange(const PciEndpointBarDecodeRequest& request,
                                                             const PciEndpointBarKind kind,
                                                             const Foundation::UInt64 baseAddress) noexcept
{
    if (request.ExtentState == PciEndpointBarExtentState::Unknown)
    {
        if ((kind != PciEndpointBarKind::Io) && (baseAddress > request.MaximumPhysicalAddress))
        {
            return PciEndpointBarDecodeError::PhysicalAddressLimitExceeded;
        }
        return PciEndpointBarDecodeError::None;
    }

    const Foundation::UInt64 minimumExtent =
        (kind == PciEndpointBarKind::Io) ? MinimumIoExtentBytes : MinimumMemoryExtentBytes;
    if (request.ExtentBytes < minimumExtent)
    {
        return PciEndpointBarDecodeError::InvalidExtent;
    }

    const Foundation::UInt64 extentMinusOne = request.ExtentBytes - 1U;
    if ((baseAddress & extentMinusOne) != 0U)
    {
        return PciEndpointBarDecodeError::InvalidExtent;
    }
    if ((kind != PciEndpointBarKind::Io) && ((baseAddress > request.MaximumPhysicalAddress) ||
                                             (extentMinusOne > (request.MaximumPhysicalAddress - baseAddress))))
    {
        return PciEndpointBarDecodeError::PhysicalAddressLimitExceeded;
    }
    return PciEndpointBarDecodeError::None;
}
} // namespace

PciEndpointBarDecodeFailure
PciEndpointBarDecoder::MakeFailure(const PciEndpointBarDecodeError error,
                                   const Hal::Device::PciConfigurationReadError configurationRead) noexcept
{
    return PciEndpointBarDecodeFailure{
        .Error = error,
        .ConfigurationRead = configurationRead,
        .Reserved = {},
    };
}

PciEndpointBarDecodeFailure PciEndpointBarDecoder::Decode(const PciEndpointBarDecodeRequest& request,
                                                          PciEndpointBar& output) noexcept
{
    if (!request.Address.IsValid())
    {
        return MakeFailure(PciEndpointBarDecodeError::InvalidFunctionAddress);
    }
    if (request.BarIndex > MaximumBarIndex)
    {
        return MakeFailure(PciEndpointBarDecodeError::InvalidBarIndex);
    }
    if (!ExtentIsValid(request))
    {
        return MakeFailure(PciEndpointBarDecodeError::InvalidExtent);
    }

    Foundation::UInt32 lowerRegister{0U};
    const Hal::Device::PciConfigurationReadError lowerRead =
        m_configurationSpace.Read32(request.Address, GetBarRegisterOffset(request.BarIndex), lowerRegister);
    if (lowerRead != Hal::Device::PciConfigurationReadError::None)
    {
        return MakeFailure(PciEndpointBarDecodeError::ConfigurationReadFailed, lowerRead);
    }

    PciEndpointBarKind kind{PciEndpointBarKind::Io};
    Foundation::UInt64 baseAddress{0U};
    Foundation::UInt8 registerCount{1U};
    bool prefetchable{false};
    if ((lowerRegister & IoSpaceIndicatorMask) != 0U)
    {
        if ((lowerRegister & IoReservedMask) != 0U)
        {
            return MakeFailure(PciEndpointBarDecodeError::ReservedEncoding);
        }
        kind = PciEndpointBarKind::Io;
        baseAddress = static_cast<Foundation::UInt64>(lowerRegister & IoBaseMask);
    }
    else
    {
        const Foundation::UInt32 memoryType = lowerRegister & MemoryTypeMask;
        prefetchable = (lowerRegister & MemoryPrefetchableMask) != 0U;
        if (memoryType == Memory32Type)
        {
            kind = PciEndpointBarKind::Memory32;
            baseAddress = static_cast<Foundation::UInt64>(lowerRegister & MemoryBaseMask);
        }
        else if (memoryType == Memory64Type)
        {
            if (request.BarIndex == MaximumBarIndex)
            {
                return MakeFailure(PciEndpointBarDecodeError::MissingUpperBar);
            }
            Foundation::UInt32 upperRegister{0U};
            const Hal::Device::PciConfigurationReadError upperRead = m_configurationSpace.Read32(
                request.Address, GetBarRegisterOffset(static_cast<Foundation::UInt8>(request.BarIndex + 1U)),
                upperRegister);
            if (upperRead != Hal::Device::PciConfigurationReadError::None)
            {
                return MakeFailure(PciEndpointBarDecodeError::ConfigurationReadFailed, upperRead);
            }
            kind = PciEndpointBarKind::Memory64;
            registerCount = 2U;
            baseAddress = (static_cast<Foundation::UInt64>(upperRegister) << 32U) |
                          static_cast<Foundation::UInt64>(lowerRegister & MemoryBaseMask);
        }
        else
        {
            return MakeFailure(PciEndpointBarDecodeError::ReservedEncoding);
        }
    }

    if (baseAddress == 0U)
    {
        return MakeFailure(PciEndpointBarDecodeError::NoAssignedAddress);
    }
    const PciEndpointBarDecodeError rangeError = ValidateDecodedRange(request, kind, baseAddress);
    if (rangeError != PciEndpointBarDecodeError::None)
    {
        return MakeFailure(rangeError);
    }

    output = PciEndpointBar{
        .BaseAddress = baseAddress,
        .ExtentBytes = request.ExtentBytes,
        .Kind = kind,
        .ExtentState = request.ExtentState,
        .BarIndex = request.BarIndex,
        .RegisterCount = registerCount,
        .Prefetchable = prefetchable,
        .Reserved = {},
    };
    return MakeFailure(PciEndpointBarDecodeError::None);
}
} // namespace Asteria::Kernel::Device
