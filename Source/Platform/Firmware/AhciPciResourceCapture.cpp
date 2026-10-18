// SPDX-License-Identifier: Apache-2.0

#include "Platform/Firmware/AhciPciResourceCapture.hpp"

#include "Boot/Uefi/PciIoProtocol.hpp"

namespace Asteria::Platform::Firmware
{
namespace
{
constexpr Foundation::UInt8 AbarIndex{5U};
constexpr Foundation::UInt8 QwordAddressSpaceDescriptor{0x8AU};
constexpr Foundation::UInt16 QwordAddressSpacePayloadBytes{0x2BU};
constexpr Foundation::UInt8 EndTagDescriptor{0x79U};
constexpr Foundation::Size ResourceTypeOffset{3U};
constexpr Foundation::Size GranularityOffset{6U};
constexpr Foundation::Size AddressMinimumOffset{14U};
constexpr Foundation::Size TranslationOffset{30U};
constexpr Foundation::Size AddressLengthOffset{38U};
constexpr Foundation::Size QwordDescriptorBytes{46U};
constexpr Foundation::Size ResourceListBytes{48U};
constexpr Foundation::UInt64 Memory32Granularity{32U};
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt8 MassStorageClass{0x01U};
constexpr Foundation::UInt8 SerialAtaSubclass{0x06U};
constexpr Foundation::UInt8 AhciProgrammingInterface{0x01U};
constexpr Foundation::UInt8 EndpointHeaderLayout{0x00U};

[[nodiscard]] constexpr Foundation::UInt16 ReadLittleEndian16(const Foundation::UInt8* const bytes) noexcept
{
    return static_cast<Foundation::UInt16>(bytes[0U]) |
           static_cast<Foundation::UInt16>(static_cast<Foundation::UInt16>(bytes[1U]) << 8U);
}

[[nodiscard]] constexpr Foundation::UInt64 ReadLittleEndian64(const Foundation::UInt8* const bytes) noexcept
{
    Foundation::UInt64 value{0U};
    for (Foundation::UInt32 index{0U}; index < 8U; ++index)
    {
        value |= static_cast<Foundation::UInt64>(bytes[index]) << (index * 8U);
    }
    return value;
}

/// Which endpoint a capture selects and which BAR it reads: the AHCI class
/// code (identifier zero) or exactly identifier.
struct Selector
{
    Foundation::UInt32 Identifier;
    Foundation::UInt8 BarIndex;
};

[[nodiscard]] constexpr AhciPciResourceCaptureFailure MakeFailure(const AhciPciResourceCaptureError error,
                                                                  const Foundation::UInt8 barIndex = AbarIndex) noexcept
{
    return AhciPciResourceCaptureFailure{
        .HandleIndex = 0U,
        .MatchingControllerCount = 0U,
        .LocateHandleBufferStatus = Boot::Uefi::Success,
        .HandleProtocolStatus = Boot::Uefi::Success,
        .GetLocationStatus = Boot::Uefi::Success,
        .ConfigurationReadStatus = Boot::Uefi::Success,
        .GetBarAttributesStatus = Boot::Uefi::Success,
        .DescriptorFreeStatus = Boot::Uefi::Success,
        .HandleBufferFreeStatus = Boot::Uefi::Success,
        .Error = error,
        .Descriptor = AhciPciResourceDescriptorError::None,
        .Cleanup = AhciPciResourceCleanupError::None,
        .ConfigurationRegister = AhciPciConfigurationRegister::None,
        .BarIndex = barIndex,
        .Reserved = {},
    };
}

[[nodiscard]] AhciPciResourceDescriptorError DecodeResource(const void* const resources,
                                                            AhciPciResource& candidate) noexcept
{
    const auto* const bytes = static_cast<const Foundation::UInt8*>(resources);
    if (bytes[0U] != QwordAddressSpaceDescriptor)
    {
        return AhciPciResourceDescriptorError::InvalidDescriptor;
    }
    if (ReadLittleEndian16(bytes + 1U) != QwordAddressSpacePayloadBytes)
    {
        return AhciPciResourceDescriptorError::InvalidDescriptorLength;
    }
    if (bytes[ResourceTypeOffset] != 0U)
    {
        return AhciPciResourceDescriptorError::NotMemory;
    }
    if (ReadLittleEndian64(bytes + GranularityOffset) != Memory32Granularity)
    {
        return AhciPciResourceDescriptorError::UnsupportedGranularity;
    }
    if (ReadLittleEndian64(bytes + TranslationOffset) != 0U)
    {
        return AhciPciResourceDescriptorError::UnsupportedTranslation;
    }

    const Foundation::UInt64 baseAddress = ReadLittleEndian64(bytes + AddressMinimumOffset);
    const Foundation::UInt64 lengthBytes = ReadLittleEndian64(bytes + AddressLengthOffset);
    if ((lengthBytes == 0U) || (baseAddress > (MaximumUInt64 - lengthBytes)))
    {
        return AhciPciResourceDescriptorError::InvalidExtent;
    }
    if (bytes[QwordDescriptorBytes] != EndTagDescriptor)
    {
        return AhciPciResourceDescriptorError::InvalidEndTag;
    }
    if (bytes[QwordDescriptorBytes + 1U] != 0U)
    {
        Foundation::UInt8 checksum{0U};
        for (Foundation::Size index{0U}; index < ResourceListBytes; ++index)
        {
            checksum = static_cast<Foundation::UInt8>(checksum + bytes[index]);
        }
        if (checksum != 0U)
        {
            return AhciPciResourceDescriptorError::InvalidChecksum;
        }
    }

    candidate.AbarBaseAddress = baseAddress;
    candidate.AbarLengthBytes = lengthBytes;
    return AhciPciResourceDescriptorError::None;
}

[[nodiscard]] bool IsSelectedEndpoint(const Selector& selector, const Foundation::UInt32 identifier,
                                      const Foundation::UInt32 classRegister,
                                      const Foundation::UInt32 headerRegister) noexcept
{
    const Foundation::UInt8 programmingInterface = static_cast<Foundation::UInt8>((classRegister >> 8U) & 0xFFU);
    const Foundation::UInt8 subclass = static_cast<Foundation::UInt8>((classRegister >> 16U) & 0xFFU);
    const Foundation::UInt8 classCode = static_cast<Foundation::UInt8>((classRegister >> 24U) & 0xFFU);
    const Foundation::UInt8 headerLayout = static_cast<Foundation::UInt8>((headerRegister >> 16U) & 0x7FU);
    if (selector.Identifier != 0U)
    {
        return (identifier == selector.Identifier) && (headerLayout == EndpointHeaderLayout);
    }
    return (classCode == MassStorageClass) && (subclass == SerialAtaSubclass) &&
           (programmingInterface == AhciProgrammingInterface) && (headerLayout == EndpointHeaderLayout);
}

/// The capture of the one endpoint selector names.
[[nodiscard]] AhciPciResourceCaptureFailure CaptureSelected(Boot::Uefi::BootServices& bootServices,
                                                            const Selector& selector, AhciPciResource& output) noexcept;

void RecordCleanup(Boot::Uefi::BootServices& bootServices, void* const resources, Boot::Uefi::Handle* const handles,
                   AhciPciResourceCaptureFailure& failure) noexcept
{
    if (resources != nullptr)
    {
        failure.DescriptorFreeStatus = bootServices.FreePoolFunction(resources);
    }
    if (handles != nullptr)
    {
        failure.HandleBufferFreeStatus = bootServices.FreePoolFunction(handles);
    }

    const bool descriptorFailed = failure.DescriptorFreeStatus != Boot::Uefi::Success;
    const bool handlesFailed = failure.HandleBufferFreeStatus != Boot::Uefi::Success;
    if (descriptorFailed && handlesFailed)
    {
        failure.Cleanup = AhciPciResourceCleanupError::DescriptorAndHandleBuffers;
    }
    else if (descriptorFailed)
    {
        failure.Cleanup = AhciPciResourceCleanupError::DescriptorBuffer;
    }
    else if (handlesFailed)
    {
        failure.Cleanup = AhciPciResourceCleanupError::HandleBuffer;
    }
    if ((failure.Cleanup != AhciPciResourceCleanupError::None) && (failure.Error == AhciPciResourceCaptureError::None))
    {
        failure.Error = AhciPciResourceCaptureError::CleanupFailed;
    }
}
} // namespace

AhciPciResourceCaptureFailure AhciPciResourceCapture::Capture(Boot::Uefi::BootServices& bootServices,
                                                              AhciPciResource& output) noexcept
{
    return CaptureSelected(bootServices, Selector{.Identifier = 0U, .BarIndex = AbarIndex}, output);
}

AhciPciResourceCaptureFailure AhciPciResourceCapture::CaptureByIdentifier(Boot::Uefi::BootServices& bootServices,
                                                                          const Foundation::UInt32 identifier,
                                                                          const Foundation::UInt8 barIndex,
                                                                          AhciPciResource& output) noexcept
{
    if ((identifier == 0U) || (barIndex > 5U))
    {
        return MakeFailure(AhciPciResourceCaptureError::InvalidConfiguration, barIndex);
    }
    return CaptureSelected(bootServices, Selector{.Identifier = identifier, .BarIndex = barIndex}, output);
}

namespace
{
AhciPciResourceCaptureFailure CaptureSelected(Boot::Uefi::BootServices& bootServices, const Selector& selector,
                                              AhciPciResource& output) noexcept
{
    AhciPciResourceCaptureFailure failure = MakeFailure(AhciPciResourceCaptureError::None, selector.BarIndex);
    if ((bootServices.LocateHandleBufferFunction == nullptr) || (bootServices.HandleProtocolFunction == nullptr) ||
        (bootServices.FreePoolFunction == nullptr))
    {
        failure.Error = AhciPciResourceCaptureError::InvalidConfiguration;
        return failure;
    }

    Foundation::Size handleCount{0U};
    Boot::Uefi::Handle* handles{nullptr};
    Boot::Uefi::Guid protocolGuid = Boot::Uefi::PciIoProtocolGuid;
    failure.LocateHandleBufferStatus = bootServices.LocateHandleBufferFunction(
        Boot::Uefi::LocateSearchType::ByProtocol, &protocolGuid, nullptr, &handleCount, &handles);
    if (failure.LocateHandleBufferStatus != Boot::Uefi::Success)
    {
        failure.Error = (failure.LocateHandleBufferStatus == Boot::Uefi::NotFound)
                            ? AhciPciResourceCaptureError::NoAhciController
                            : AhciPciResourceCaptureError::LocateHandleBufferFailed;
        RecordCleanup(bootServices, nullptr, handles, failure);
        return failure;
    }
    if ((handleCount == 0U) || (handles == nullptr))
    {
        failure.Error = AhciPciResourceCaptureError::InvalidHandleBuffer;
        RecordCleanup(bootServices, nullptr, handles, failure);
        return failure;
    }

    AhciPciResource candidate{};
    Boot::Uefi::PciIoProtocol* selectedProtocol{nullptr};
    for (Foundation::Size index{0U}; index < handleCount; ++index)
    {
        failure.HandleIndex = index;
        void* interface{nullptr};
        protocolGuid = Boot::Uefi::PciIoProtocolGuid;
        failure.HandleProtocolStatus = bootServices.HandleProtocolFunction(handles[index], &protocolGuid, &interface);
        if (failure.HandleProtocolStatus != Boot::Uefi::Success)
        {
            failure.Error = AhciPciResourceCaptureError::HandleProtocolFailed;
            break;
        }
        auto* const protocol = static_cast<Boot::Uefi::PciIoProtocol*>(interface);
        if ((protocol == nullptr) || (protocol->GetLocation == nullptr) || (protocol->Pci.Read == nullptr))
        {
            failure.Error = AhciPciResourceCaptureError::InvalidPciIoProtocol;
            break;
        }

        Foundation::Size segment{0U};
        Foundation::Size bus{0U};
        Foundation::Size device{0U};
        Foundation::Size function{0U};
        failure.GetLocationStatus = protocol->GetLocation(protocol, &segment, &bus, &device, &function);
        if (failure.GetLocationStatus != Boot::Uefi::Success)
        {
            failure.Error = AhciPciResourceCaptureError::GetLocationFailed;
            break;
        }
        if ((segment > 0xFFFFU) || (bus > 0xFFU) || (device > 31U) || (function > 7U))
        {
            failure.Error = AhciPciResourceCaptureError::InvalidLocation;
            break;
        }

        Foundation::UInt32 identifier{0U};
        failure.ConfigurationRegister = AhciPciConfigurationRegister::Identifier;
        failure.ConfigurationReadStatus =
            protocol->Pci.Read(protocol, Boot::Uefi::PciIoWidth::Uint32, 0x00U, 1U, &identifier);
        if (failure.ConfigurationReadStatus != Boot::Uefi::Success)
        {
            failure.Error = AhciPciResourceCaptureError::ConfigurationReadFailed;
            break;
        }
        failure.ConfigurationRegister = AhciPciConfigurationRegister::None;
        if ((identifier & 0xFFFFU) == 0xFFFFU)
        {
            continue;
        }

        Foundation::UInt32 classRegister{0U};
        failure.ConfigurationRegister = AhciPciConfigurationRegister::ClassCode;
        failure.ConfigurationReadStatus =
            protocol->Pci.Read(protocol, Boot::Uefi::PciIoWidth::Uint32, 0x08U, 1U, &classRegister);
        if (failure.ConfigurationReadStatus != Boot::Uefi::Success)
        {
            failure.Error = AhciPciResourceCaptureError::ConfigurationReadFailed;
            break;
        }

        Foundation::UInt32 headerRegister{0U};
        failure.ConfigurationRegister = AhciPciConfigurationRegister::HeaderType;
        failure.ConfigurationReadStatus =
            protocol->Pci.Read(protocol, Boot::Uefi::PciIoWidth::Uint32, 0x0CU, 1U, &headerRegister);
        if (failure.ConfigurationReadStatus != Boot::Uefi::Success)
        {
            failure.Error = AhciPciResourceCaptureError::ConfigurationReadFailed;
            break;
        }
        failure.ConfigurationRegister = AhciPciConfigurationRegister::None;
        if (!IsSelectedEndpoint(selector, identifier, classRegister, headerRegister))
        {
            continue;
        }

        ++failure.MatchingControllerCount;
        if (failure.MatchingControllerCount != 1U)
        {
            failure.Error = AhciPciResourceCaptureError::AmbiguousAhciController;
            break;
        }
        candidate = AhciPciResource{
            .AbarBaseAddress = 0U,
            .AbarLengthBytes = 0U,
            .Segment = static_cast<Foundation::UInt16>(segment),
            .Bus = static_cast<Foundation::UInt8>(bus),
            .Device = static_cast<Foundation::UInt8>(device),
            .Function = static_cast<Foundation::UInt8>(function),
            .BarIndex = selector.BarIndex,
            .Reserved = {},
        };
        selectedProtocol = protocol;
    }

    void* resources{nullptr};
    if (failure.Error == AhciPciResourceCaptureError::None)
    {
        if (failure.MatchingControllerCount == 0U)
        {
            failure.Error = AhciPciResourceCaptureError::NoAhciController;
        }
        else if ((selectedProtocol == nullptr) || (selectedProtocol->GetBarAttributes == nullptr))
        {
            failure.Error = AhciPciResourceCaptureError::InvalidPciIoProtocol;
        }
        else
        {
            failure.GetBarAttributesStatus =
                selectedProtocol->GetBarAttributes(selectedProtocol, selector.BarIndex, nullptr, &resources);
            if (failure.GetBarAttributesStatus != Boot::Uefi::Success)
            {
                failure.Error = AhciPciResourceCaptureError::GetBarAttributesFailed;
            }
            else if (resources == nullptr)
            {
                failure.Error = AhciPciResourceCaptureError::InvalidBarResourceBuffer;
            }
            else
            {
                failure.Descriptor = DecodeResource(resources, candidate);
                if (failure.Descriptor != AhciPciResourceDescriptorError::None)
                {
                    failure.Error = AhciPciResourceCaptureError::BarResourceDescriptorRejected;
                }
            }
        }
    }

    RecordCleanup(bootServices, resources, handles, failure);
    if (failure.Error == AhciPciResourceCaptureError::None)
    {
        output = candidate;
    }
    return failure;
}
} // namespace
} // namespace Asteria::Platform::Firmware
