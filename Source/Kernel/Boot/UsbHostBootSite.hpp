// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_BOOT_USB_HOST_BOOT_SITE_HPP_INCLUDED
#define ASTERIA_KERNEL_BOOT_USB_HOST_BOOT_SITE_HPP_INCLUDED

#include "Kernel/Boot/BootInventory.hpp"
#include "Kernel/Device/TrustedDmaPolicy.hpp"
#include "Platform/Device/UsbHostPublication.hpp"
#include "Platform/Firmware/Diagnostic.hpp"

namespace Asteria::Boot::Bootstrap
{
/// Both ISAs' boot beside the USB host publication: firmware's BAR extents and the outcomes come
/// from and go to the boot inventory; after a refused IOMMU handoff only a function a verified
/// BOOT.CFG trust entry names for the USB host driver is published. Lives for one PublishUsbHosts
/// call on the BSP.
class UsbHostBootSite final : public Platform::Device::IUsbHostPublicationSite
{
public:
    /// TrustOnly null publishes every function; otherwise only trusted ones. With lines, each
    /// published function's INTx line is isolated as the assignable device's : a line that cannot
    /// be leaves the controller to its driver's polling, never stopping the boot.
    explicit constexpr UsbHostBootSite(const BootTrustPolicy* const trustOnly,
                                       Platform::Device::PciEcamRuntime* const lines = nullptr) noexcept
        : m_trustOnly{trustOnly}, m_lines{lines}
    {
    }

    [[nodiscard]] bool FirmwareBar(const Kernel::Device::PciFunctionAddress function,
                                   Platform::Firmware::AhciPciResource& resource) noexcept override
    {
        return GetBootInventoryXhciResource(function.GetSegment(), function.GetBus(), function.GetDevice(),
                                            function.GetFunction(), resource);
    }

    [[nodiscard]] bool Permits(const Kernel::Device::PciFunctionDescriptor& function) noexcept override
    {
        if (m_trustOnly == nullptr)
        {
            return true;
        }
        const Kernel::Device::TrustedDmaFunction trusted{.Address = function.Address,
                                                         .Vendor = function.VendorId,
                                                         .Product = function.DeviceId,
                                                         .Driver = BootTrustDriver::UsbHost,
                                                         .Reserved = 0U};
        return Kernel::Device::DecideTrustedDma(*m_trustOnly, trusted, Kernel::Device::IommuAvailability::Refused)
                   .Outcome == Kernel::Device::TrustedDmaOutcome::Applied;
    }

    void Record(const Kernel::Device::PciFunctionAddress function, const Foundation::UInt8 index,
                const Foundation::UInt8 step, const Foundation::UInt8 error) noexcept override
    {
        if (index != 0U)
        {
            RecordBootInventoryUsbHost(function.GetSegment(), function.GetBus(), function.GetDevice(),
                                       function.GetFunction(), index);
            if ((m_lines != nullptr) && !m_lines->IsolateInterruptLine(function).HasValue())
            {
                Platform::Firmware::WriteDiagnosticAscii("ASTERIA:USB:INTX-NOT-ISOLATED\r\n");
            }
            return;
        }
        RecordBootInventoryXhciFailure(function.GetSegment(), function.GetBus(), function.GetDevice(),
                                       function.GetFunction(), step, error);
    }

private:
    const BootTrustPolicy* m_trustOnly;
    Platform::Device::PciEcamRuntime* m_lines;
};
} // Namespace Asteria::Boot::Bootstrap

#endif // ASTERIA_KERNEL_BOOT_USB_HOST_BOOT_SITE_HPP_INCLUDED
