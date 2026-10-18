// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Device/AhciHbaRegisterSnapshotReader.hpp"

namespace Asteria::Kernel::Device
{
namespace
{
constexpr Foundation::UInt64 CapabilitiesOffset{0x00U};
constexpr Foundation::UInt64 GlobalHostControlOffset{0x04U};
constexpr Foundation::UInt64 InterruptStatusOffset{0x08U};
constexpr Foundation::UInt64 PortsImplementedOffset{0x0CU};
constexpr Foundation::UInt64 VersionOffset{0x10U};
constexpr Foundation::UInt64 RegisterWidthBytes{4U};
constexpr Foundation::UInt64 RequiredRangeBytes{VersionOffset + RegisterWidthBytes};
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 MaximumUInt32{0xFFFF'FFFFULL};

constexpr Foundation::UInt32 PortCountMask{0x1FU};
constexpr Foundation::UInt32 CommandSlotCountMask{0x1FU << 8U};
constexpr Foundation::UInt32 CommandSlotCountShift{8U};
constexpr Foundation::UInt32 InterfaceSpeedMask{0x0FU << 20U};
constexpr Foundation::UInt32 InterfaceSpeedShift{20U};
constexpr Foundation::UInt32 SupportsPortMultiplierMask{1U << 17U};
constexpr Foundation::UInt32 SupportsAggressiveLinkPowerManagementMask{1U << 26U};
constexpr Foundation::UInt32 SupportsStaggeredSpinUpMask{1U << 27U};
constexpr Foundation::UInt32 SupportsNativeCommandQueuingMask{1U << 30U};
constexpr Foundation::UInt32 Supports64BitAddressingMask{1U << 31U};
constexpr Foundation::UInt32 ResetMask{1U << 0U};
constexpr Foundation::UInt32 InterruptEnableMask{1U << 1U};
constexpr Foundation::UInt32 AhciEnableMask{1U << 31U};

[[nodiscard]] constexpr bool HasBit(const Foundation::UInt32 value, const Foundation::UInt32 mask) noexcept
{
    return (value & mask) != 0U;
}
} // namespace

AhciHbaSnapshotFailure
AhciHbaRegisterSnapshotReader::MakeFailure(const AhciHbaSnapshotError error,
                                           const Hal::Device::MemoryMappedIoReadError memoryMappedIoRead,
                                           const AhciHbaGlobalRegister reg) noexcept
{
    return AhciHbaSnapshotFailure{
        .Error = error,
        .MemoryMappedIoRead = memoryMappedIoRead,
        .Register = reg,
        .Reserved = {},
    };
}

AhciHbaSnapshotFailure AhciHbaRegisterSnapshotReader::ReadRegister(const Foundation::UInt64 physicalAddress,
                                                                   const AhciHbaGlobalRegister reg,
                                                                   Foundation::UInt32& output) noexcept
{
    Foundation::UInt64 value{0U};
    const Hal::Device::MemoryMappedIoReadError readError =
        m_memoryMappedIo.Read(physicalAddress, Hal::Device::MemoryMappedIoWidth::DoubleWord, value);
    if (readError != Hal::Device::MemoryMappedIoReadError::None)
    {
        return MakeFailure(AhciHbaSnapshotError::MemoryMappedIoReadFailed, readError, reg);
    }
    if (value > MaximumUInt32)
    {
        return MakeFailure(AhciHbaSnapshotError::InvalidRegisterValue, Hal::Device::MemoryMappedIoReadError::None, reg);
    }
    output = static_cast<Foundation::UInt32>(value);
    return MakeFailure(AhciHbaSnapshotError::None);
}

AhciHbaSnapshotFailure AhciHbaRegisterSnapshotReader::Read(const AhciHbaRegisterRange range,
                                                           AhciHbaRegisterSnapshot& output) noexcept
{
    if ((range.BaseAddress == 0U) || ((range.BaseAddress & (RegisterWidthBytes - 1U)) != 0U) ||
        (range.LengthBytes < RequiredRangeBytes) || (range.BaseAddress > (MaximumUInt64 - range.LengthBytes)))
    {
        return MakeFailure(AhciHbaSnapshotError::InvalidRange);
    }

    Foundation::UInt32 capabilities{0U};
    Foundation::UInt32 globalHostControl{0U};
    Foundation::UInt32 interruptStatus{0U};
    Foundation::UInt32 portsImplemented{0U};
    Foundation::UInt32 version{0U};

    AhciHbaSnapshotFailure failure =
        ReadRegister(range.BaseAddress + CapabilitiesOffset, AhciHbaGlobalRegister::Capabilities, capabilities);
    if (failure.Error != AhciHbaSnapshotError::None)
    {
        return failure;
    }
    failure = ReadRegister(range.BaseAddress + GlobalHostControlOffset, AhciHbaGlobalRegister::GlobalHostControl,
                           globalHostControl);
    if (failure.Error != AhciHbaSnapshotError::None)
    {
        return failure;
    }
    failure = ReadRegister(range.BaseAddress + InterruptStatusOffset, AhciHbaGlobalRegister::InterruptStatus,
                           interruptStatus);
    if (failure.Error != AhciHbaSnapshotError::None)
    {
        return failure;
    }
    failure = ReadRegister(range.BaseAddress + PortsImplementedOffset, AhciHbaGlobalRegister::PortsImplemented,
                           portsImplemented);
    if (failure.Error != AhciHbaSnapshotError::None)
    {
        return failure;
    }
    failure = ReadRegister(range.BaseAddress + VersionOffset, AhciHbaGlobalRegister::Version, version);
    if (failure.Error != AhciHbaSnapshotError::None)
    {
        return failure;
    }

    output = AhciHbaRegisterSnapshot{
        .Capabilities = capabilities,
        .GlobalHostControl = globalHostControl,
        .InterruptStatus = interruptStatus,
        .PortsImplemented = portsImplemented,
        .Version = version,
        .DecodedCapabilities =
            AhciHbaCapabilities{
                .PortCount = static_cast<Foundation::UInt8>((capabilities & PortCountMask) + 1U),
                .CommandSlotCount = static_cast<Foundation::UInt8>(
                    ((capabilities & CommandSlotCountMask) >> CommandSlotCountShift) + 1U),
                .InterfaceSpeedSupport =
                    static_cast<Foundation::UInt8>((capabilities & InterfaceSpeedMask) >> InterfaceSpeedShift),
                .Supports64BitAddressing = HasBit(capabilities, Supports64BitAddressingMask),
                .SupportsNativeCommandQueuing = HasBit(capabilities, SupportsNativeCommandQueuingMask),
                .SupportsPortMultiplier = HasBit(capabilities, SupportsPortMultiplierMask),
                .SupportsStaggeredSpinUp = HasBit(capabilities, SupportsStaggeredSpinUpMask),
                .SupportsAggressiveLinkPowerManagement =
                    HasBit(capabilities, SupportsAggressiveLinkPowerManagementMask),
            },
        .DecodedControl =
            AhciHbaControl{
                .AhciModeEnabled = HasBit(globalHostControl, AhciEnableMask),
                .InterruptsEnabled = HasBit(globalHostControl, InterruptEnableMask),
                .ResetRequested = HasBit(globalHostControl, ResetMask),
                .Reserved = {},
            },
        .DecodedVersion =
            AhciHbaVersion{
                .Major = static_cast<Foundation::UInt16>(version >> 16U),
                .Minor = static_cast<Foundation::UInt16>(version & 0xFFFFU),
            },
    };
    return MakeFailure(AhciHbaSnapshotError::None);
}
} // namespace Asteria::Kernel::Device
