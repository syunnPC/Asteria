// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_BOOT_BOOT_INVENTORY_HPP_INCLUDED
#define ASTERIA_KERNEL_BOOT_BOOT_INVENTORY_HPP_INCLUDED

#include "Boot/BootInfo.hpp"
#include "Platform/Firmware/PciResource.hpp"

namespace Asteria::Boot::Bootstrap
{
/// Imports the loader inventory once into page-aligned kernel storage and
/// retargets the kernel's retained receipt. BSP only, no allocation or firmware
/// calls. Rejects inventory version and ABI count failures; the caller validates the
/// enclosing BootInfo CRC before import.
[[nodiscard]] bool ImportBootInventory(BootInfo& info) noexcept;

/// Appends native disk facts before pager admission. BSP only; bounded by the
/// inventory ABI's capacity. Model remains readable for modelBytes bytes.
void RecordBootInventoryDisk(Foundation::UInt8 port, Foundation::UInt64 sectors, const char* model,
                             Foundation::Size modelBytes) noexcept;

/// Records the kernel's final i8042 publication outcome (its declared-devices
/// word); BSP only, before the inventory becomes immutable.
void RecordBootInventoryI8042(Foundation::UInt64 declared) noexcept;

/// Marks a successfully published xHCI PCI function in the kernel's inventory
/// with its controller index. BSP only, bounded by the inventory capacity; no
/// allocation or firmware calls.
void RecordBootInventoryXhci(const Platform::Firmware::AhciPciResource& resource, Foundation::UInt8 index) noexcept;

/// A USB 1.1/2.0 host controller the kernel published at the one-based index
/// of the USB host table: the inventory names the usb host driver.
void RecordBootInventoryUsbHost(Foundation::UInt16 segment, Foundation::UInt8 bus, Foundation::UInt8 device,
                                Foundation::UInt8 function, Foundation::UInt8 index) noexcept;

/// The xHCI, EHCI or OHCI register resource the boot captured for a function
/// (its first BAR and the extent firmware reported); false when none was.
[[nodiscard]] bool GetBootInventoryXhciResource(Foundation::UInt16 segment, Foundation::UInt8 bus,
                                                Foundation::UInt8 device, Foundation::UInt8 function,
                                                Platform::Firmware::AhciPciResource& resource) noexcept;

/// Records why the kernel did not publish an xHCI function (step and error).
void RecordBootInventoryXhciFailure(Foundation::UInt16 segment, Foundation::UInt8 bus, Foundation::UInt8 device,
                                    Foundation::UInt8 function, Foundation::UInt8 step,
                                    Foundation::UInt8 error) noexcept;

/// Copies the BOOT.CFG trust entries into the inventory (each module's name from files), every
/// outcome Pending. BSP only, before the inventory becomes immutable.
void RecordBootInventoryTrust(const BootTrustPolicy& policy, const BootFileRegion& files) noexcept;

/// The outcome of the trust entry of one-based number; ignored for another number.
void RecordBootInventoryTrustOutcome(Foundation::Size number, Abi::InventoryTrustOutcome outcome) noexcept;

/// Marks the function a trusted DMA function under the entry of one-based number.
void RecordBootInventoryTrusted(Foundation::UInt16 segment, Foundation::UInt8 bus, Foundation::UInt8 device,
                                Foundation::UInt8 function, Foundation::UInt8 number) noexcept;

/// Resolves pending EDU entries after native capture, before pager admission.
/// BSP only, no allocation, blocking or firmware callbacks.
void FinalizeBootInventoryEduCapture(const Platform::Firmware::AhciPciResource* resource) noexcept;

/// Resolves pending e1000e entries (IA-64's NIC, Source/User/Network/NetworkStack.hpp ) the same
/// way.
void FinalizeBootInventoryNicCapture(const Platform::Firmware::AhciPciResource* resource) noexcept;
} // Namespace Asteria::Boot::Bootstrap

#endif // ASTERIA_KERNEL_BOOT_BOOT_INVENTORY_HPP_INCLUDED
