// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_DEVICE_USB_HOST_PUBLICATION_HPP_INCLUDED
#define ASTERIA_PLATFORM_DEVICE_USB_HOST_PUBLICATION_HPP_INCLUDED

#include "Abi/PagerBootstrap.hpp"
#include "Platform/Device/PciEcamRuntime.hpp"

namespace Asteria::Platform::Device
{
/// What the boot of one ISA decides beside the USB host publication: the
/// firmware's captured BAR extent of a function, whether a refused IOMMU
/// handoff still lets a function be published (a BOOT.CFG trust entry names
/// it for the USB host driver), and where each outcome is recorded (the boot
/// inventory). Called on the BSP during boot only.
class IUsbHostPublicationSite
{
public:
    /// The extent firmware reported for function's memory BAR 0, if any.
    [[nodiscard]] virtual bool FirmwareBar(Kernel::Device::PciFunctionAddress function,
                                           Firmware::AhciPciResource& resource) noexcept = 0;
    /// Whether the function may be published; false withholds it (step 4).
    [[nodiscard]] virtual bool Permits(const Kernel::Device::PciFunctionDescriptor& function) noexcept = 0;
    /// The function's outcome: its one-based table index, or zero with the
    /// step (1 capture/publication, 3 capacity, 4 withheld, 5 restricted) and
    /// that step's error.
    virtual void Record(Kernel::Device::PciFunctionAddress function, Foundation::UInt8 index, Foundation::UInt8 step,
                        Foundation::UInt8 error) noexcept = 0;

protected:
    constexpr IUsbHostPublicationSite() noexcept = default;
    ~IUsbHostPublicationSite() = default;
};

struct UsbHostPublicationInput
{
    PciEcamRuntime* Runtime;
    IUsbHostPublicationSite* Site;
    Foundation::UInt8 PhysicalAddressBits;
    Kernel::Resource::ResourceDomainObjectId ResourceIdentity;
    Kernel::Resource::ResourceDomain* Resources;
};

/// Publishes the machine's USB 1.1/2.0 host controllers (PCI class 0C/03,
/// programming interfaces 00 UHCI, 10 OHCI, 20 EHCI) for the USB host group
/// driver, in PCI catalog
/// order and by whole PCI devices: an EHCI and the companion UHCI or OHCI
/// functions of the same device go together, and a device whose functions do
/// not all fit the table's remaining capacity is skipped whole, so no EHCI
/// hands a full-speed device to a companion nobody drives. A function the
/// platform restricts (a firmware DMA reservation such as a VT-d RMRR, until
/// an RMRR release exists) and one the site withholds are skipped and
/// recorded, as is one the runtime refuses; its device's other functions are
/// still published. Each outcome is written to the diagnostic channel as
/// `ASTERIA:USB:PUBLISHED:<bbddf>` or `ASTERIA:USB:SKIPPED:<bbddf>:<step>:<err>`.
/// Returns how many entries of output it filled, from the front.
[[nodiscard]] Foundation::Size PublishUsbHosts(const UsbHostPublicationInput& input,
                                               AssignableDevice (&output)[Abi::PagerUsbHostControllers]) noexcept;
} // Namespace Asteria::Platform::Device

#endif // ASTERIA_PLATFORM_DEVICE_USB_HOST_PUBLICATION_HPP_INCLUDED
