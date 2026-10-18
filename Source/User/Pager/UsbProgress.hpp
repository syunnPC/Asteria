// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_PAGER_USB_PROGRESS_HPP_INCLUDED
#define ASTERIA_USER_PAGER_USB_PROGRESS_HPP_INCLUDED

#include "User/Input/UsbDiagnostic.hpp"
#include "User/Pager/RootContext.hpp"

namespace Asteria::User::Pager
{
/// The longest line FormatUsbProgress writes, newline included.
inline constexpr Foundation::Size UsbProgressLineBytes{120U};

/// Formats one xHCI driver diagnostic as a `# usb: xhciN port P...` line
/// ending in a newline. The
/// record comes from a driver: every field is printed, never trusted, and
/// unknown codes print as numbers. Returns the byte count; no allocation.
[[nodiscard]] Foundation::Size FormatUsbProgress(Foundation::UInt8 controller, const Input::UsbDiagnostic& record,
                                                 char (&line)[UsbProgressLineBytes]) noexcept;
/// Prints the delivery of bytes in context.Delivery when it is an xHCI
/// driver's diagnostic (User/Component/UsbProgressFrame.hpp), between the
/// terminal's edited lines (WriteBetweenLines); false for any other delivery.
/// IA-64 links a stand-in returning false.
[[nodiscard]] bool PrintUsbProgress(ScenarioContext& context, Foundation::UInt64 bytes) noexcept;
} // Namespace Asteria::User::Pager

#endif // ASTERIA_USER_PAGER_USB_PROGRESS_HPP_INCLUDED
