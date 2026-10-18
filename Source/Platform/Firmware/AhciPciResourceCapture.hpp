// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_FIRMWARE_AHCI_PCI_RESOURCE_CAPTURE_HPP_INCLUDED
#define ASTERIA_PLATFORM_FIRMWARE_AHCI_PCI_RESOURCE_CAPTURE_HPP_INCLUDED

#include "Boot/Uefi/UefiTypes.hpp"
#include "Foundation/Types.hpp"
#include "Platform/Firmware/PciResource.hpp"

namespace Asteria::Platform::Firmware
{
enum class AhciPciResourceCaptureError : Foundation::UInt8
{
    None,
    InvalidConfiguration,
    LocateHandleBufferFailed,
    InvalidHandleBuffer,
    HandleProtocolFailed,
    InvalidPciIoProtocol,
    GetLocationFailed,
    InvalidLocation,
    ConfigurationReadFailed,
    NoAhciController,
    AmbiguousAhciController,
    GetBarAttributesFailed,
    InvalidBarResourceBuffer,
    BarResourceDescriptorRejected,
    CleanupFailed,
};

enum class AhciPciResourceDescriptorError : Foundation::UInt8
{
    None,
    InvalidDescriptor,
    InvalidDescriptorLength,
    NotMemory,
    UnsupportedGranularity,
    UnsupportedTranslation,
    InvalidExtent,
    InvalidEndTag,
    InvalidChecksum,
};

enum class AhciPciResourceCleanupError : Foundation::UInt8
{
    None,
    DescriptorBuffer,
    HandleBuffer,
    DescriptorAndHandleBuffers,
};

enum class AhciPciConfigurationRegister : Foundation::UInt8
{
    None,
    Identifier,
    ClassCode,
    HeaderType,
};

struct AhciPciResourceCaptureFailure
{
    Foundation::Size HandleIndex;
    Foundation::Size MatchingControllerCount;
    Boot::Uefi::Status LocateHandleBufferStatus;
    Boot::Uefi::Status HandleProtocolStatus;
    Boot::Uefi::Status GetLocationStatus;
    Boot::Uefi::Status ConfigurationReadStatus;
    Boot::Uefi::Status GetBarAttributesStatus;
    Boot::Uefi::Status DescriptorFreeStatus;
    Boot::Uefi::Status HandleBufferFreeStatus;
    AhciPciResourceCaptureError Error;
    AhciPciResourceDescriptorError Descriptor;
    AhciPciResourceCleanupError Cleanup;
    AhciPciConfigurationRegister ConfigurationRegister;
    Foundation::UInt8 BarIndex;
    Foundation::UInt8 Reserved[3];
};

/// Captures the one allocated 32-bit AHCI BAR5 resource before firmware exit. LocateHandleBuffer
/// supplies the complete protocol-handle array; this code imposes no handle-count or resource-size
/// policy. It reads only standard PCI configuration fields and calls GetBarAttributes only after
/// exactly one present type-zero 01/06/01 endpoint has been identified. The resource list must
/// contain one QWORD memory descriptor followed immediately by an End Tag. Address Range Minimum
/// and Address Length become the scalar result; Address Range Maximum is deliberately not an
/// authority input. Both firmware buffers are freed, descriptor first, whenever their pointers are
/// non-null. No firmware pointer survives this call. Failure, including a cleanup failure, leaves
/// output unchanged. This BSP boot-services operation is externally serialized, may allocate
/// through firmware, and must never be called from an ISR or from the final memory-map preparation
/// callback.
class AhciPciResourceCapture final
{
public:
    [[nodiscard]] static AhciPciResourceCaptureFailure Capture(Boot::Uefi::BootServices& bootServices,
                                                               AhciPciResource& output) noexcept;

    /// As Capture, for the one type-zero endpoint whose identifier register (device << 16 | vendor)
    /// is identifier, and its 32-bit memory BAR barIndex: a device the kernel may assign to a
    /// driver. NoAhciController and AmbiguousAhciController name no or several such endpoints.
    [[nodiscard]] static AhciPciResourceCaptureFailure CaptureByIdentifier(Boot::Uefi::BootServices& bootServices,
                                                                           Foundation::UInt32 identifier,
                                                                           Foundation::UInt8 barIndex,
                                                                           AhciPciResource& output) noexcept;

private:
    AhciPciResourceCapture() = delete;
};

static_assert(sizeof(AhciPciResourceCaptureError) == 1U);
static_assert(sizeof(AhciPciResourceDescriptorError) == 1U);
static_assert(sizeof(AhciPciResourceCleanupError) == 1U);
static_assert(sizeof(AhciPciConfigurationRegister) == 1U);
static_assert(sizeof(AhciPciResource) == 24U);
static_assert(alignof(AhciPciResource) == 8U);
static_assert(sizeof(AhciPciResourceCaptureFailure) == 80U);
static_assert(alignof(AhciPciResourceCaptureFailure) == 8U);
} // Namespace Asteria::Platform::Firmware

#endif // ASTERIA_PLATFORM_FIRMWARE_AHCI_PCI_RESOURCE_CAPTURE_HPP_INCLUDED
