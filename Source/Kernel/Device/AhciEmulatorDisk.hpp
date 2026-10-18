// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_DEVICE_AHCI_EMULATOR_DISK_HPP_INCLUDED
#define ASTERIA_KERNEL_DEVICE_AHCI_EMULATOR_DISK_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Device/AhciIdentifyCommandExecutor.hpp"

namespace Asteria::Kernel::Device
{
/// Whether IDENTIFY DEVICE names one of the emulator's disks: QEMU's default
/// model observed by VM tests ("QEMU HARDDISK") or the
/// model the VM runner gives its disks ("ASTERIA AHCI DISK",
/// Tools/Vm/AsteriaVm/Machine.py). No physical drive reports either. Only
/// such a disk may be written or formatted by the boot's demonstrations; any
/// other disk, a real one above all, is left alone. The model is compared
/// exactly, as the parser trims it (trailing spaces removed).
[[nodiscard]] constexpr bool IsEmulatorDisk(const AhciIdentifyDeviceInformation& device) noexcept
{
    constexpr const char* Models[]{"QEMU HARDDISK", "ASTERIA AHCI DISK"};
    for (const char* const model : Models)
    {
        Foundation::Size length{0U};
        while (model[length] != '\0')
        {
            ++length;
        }
        bool equal = length == device.ModelNumberLength;
        for (Foundation::Size index{0U}; equal && (index < length); ++index)
        {
            equal = device.ModelNumber[index] == model[index];
        }
        if (equal)
        {
            return true;
        }
    }
    return false;
}
} // namespace Asteria::Kernel::Device

#endif // ASTERIA_KERNEL_DEVICE_AHCI_EMULATOR_DISK_HPP_INCLUDED
