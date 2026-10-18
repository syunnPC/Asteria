// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_DEVICE_DETAIL_AHCI_IDENTIFY_DEVICE_INFORMATION_PARSER_HPP_INCLUDED
#define ASTERIA_KERNEL_DEVICE_DETAIL_AHCI_IDENTIFY_DEVICE_INFORMATION_PARSER_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Device/AhciIdentifyCommandExecutor.hpp"

namespace Asteria::Kernel::Device::Detail
{
/// Parses the 512-byte ATA IDENTIFY payload after the executor has validated
/// transport completion. Failure leaves output unchanged.
[[nodiscard]] AhciIdentifyCommandError
ParseAhciIdentifyDeviceInformation(const Foundation::UInt8* identifyData,
                                   AhciIdentifyDeviceInformation& output) noexcept;
} // namespace Asteria::Kernel::Device::Detail

#endif // ASTERIA_KERNEL_DEVICE_DETAIL_AHCI_IDENTIFY_DEVICE_INFORMATION_PARSER_HPP_INCLUDED
