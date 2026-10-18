// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_FIRMWARE_FIRMWARE_PORT_IO_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_FIRMWARE_FIRMWARE_PORT_IO_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Platform::Amd64::Firmware
{
/// One 32-bit port read (`in` of a doubleword) by the boot application before ExitBootServices, for
/// registers whose read has no side effect (the PCH's SMI_EN). The caller
/// names a port the platform decodes.
[[nodiscard]] Foundation::UInt32 ReadFirmwarePort32(Foundation::UInt16 port) noexcept;
} // Namespace Asteria::Platform::Amd64::Firmware

#endif // ASTERIA_PLATFORM_AMD64_FIRMWARE_FIRMWARE_PORT_IO_HPP_INCLUDED
