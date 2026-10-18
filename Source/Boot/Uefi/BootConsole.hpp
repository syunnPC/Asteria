// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_UEFI_BOOT_CONSOLE_HPP_INCLUDED
#define ASTERIA_BOOT_UEFI_BOOT_CONSOLE_HPP_INCLUDED

#include "Boot/Uefi/UefiTypes.hpp"

namespace Asteria::Boot::Uefi
{
/// BSP only, pre-exit. Uses a static 160-cell RAM line and GOP Blt on both
/// ISAs. Newlines/wraps publish one 16-pixel-high line, at most 20,480 device
/// pixels, independent of screen height. Wraps to the top without scrolling;
/// never reads the framebuffer. Invalid modes disable it. No allocation.
void ConfigureBootConsole(GraphicsOutputProtocol* graphics) noexcept;
[[nodiscard]] bool IsBootConsoleConfigured() noexcept;
/// Writes text through the boot line console when configured, otherwise
/// through the borrowed firmware console. No allocation; not interrupt-safe.
[[nodiscard]] bool WriteBootConsoleAscii(SimpleTextOutputProtocol& console, const char* text) noexcept;
[[nodiscard]] bool WriteBootConsoleUnicode(SimpleTextOutputProtocol& console, Foundation::UInt16* text) noexcept;
} // namespace Asteria::Boot::Uefi

#endif // ASTERIA_BOOT_UEFI_BOOT_CONSOLE_HPP_INCLUDED
