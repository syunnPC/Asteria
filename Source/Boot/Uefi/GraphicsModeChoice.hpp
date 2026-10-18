// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_UEFI_GRAPHICS_MODE_CHOICE_HPP_INCLUDED
#define ASTERIA_BOOT_UEFI_GRAPHICS_MODE_CHOICE_HPP_INCLUDED

#include "Boot/DisplayPolicy.hpp"
#include "Boot/Uefi/UefiTypes.hpp"
#include "Inventory/ReportText.hpp"

namespace Asteria::Boot::Uefi
{
inline constexpr Foundation::UInt64 ScanoutCeilingBytes{32U * 1024U * 1024U};
inline constexpr Foundation::UInt32 MaximumReportedGraphicsModes{256U};

/// Checks dimensions, 32-bit format, stride and the scanout ceiling; no allocation.
[[nodiscard]] bool IsScanoutInformationUsable(const GraphicsOutputModeInformation& information) noexcept;
/// Also checks the current mode's page-aligned framebuffer and its byte extent.
[[nodiscard]] bool IsScanoutModeUsable(const GraphicsOutputProtocolMode* mode) noexcept;
/// BSP only, once, before display capture and ExitBootServices. Queries at most
/// 256 modes, frees each QueryMode allocation and the handle array. EDID is
/// borrowed on the selected GOP handle. Mode failures are recorded, never fatal.
/// Returns the selected GOP, or nullptr if none is available.
[[nodiscard]] GraphicsOutputProtocol* ChooseGraphicsMode(SystemTable& system, const DisplayPolicy& policy) noexcept;
/// Pre-exit failure return only: restore the original firmware mode once the
/// readable timed failure screen has finished. Disables the boot console.
void RestoreOriginalGraphicsMode() noexcept;
/// Emits the selected-mode console line after the desktop, before the banner.
void AnnounceGraphicsMode(SystemTable& system, const GraphicsOutputProtocol* graphics) noexcept;
/// Appends the retained bounded selection report; no firmware calls or allocation.
void FormatGraphicsModeChoice(Inventory::ReportText& text) noexcept;
} // namespace Asteria::Boot::Uefi

#endif // ASTERIA_BOOT_UEFI_GRAPHICS_MODE_CHOICE_HPP_INCLUDED
