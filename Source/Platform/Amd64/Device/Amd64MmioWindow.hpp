// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_DEVICE_AMD64_MMIO_WINDOW_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_DEVICE_AMD64_MMIO_WINDOW_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Platform::Amd64::Device
{
inline constexpr Foundation::UInt64 MmioWindowVirtualAddress{0x7000'0000U};

/// Selects a supervisor RW/UC/NX leaf wholly owned by the supplied identity-
/// mapped arena. Failure leaves output unchanged. Used before firmware exit.
[[nodiscard]] bool FindAmd64MmioWindowLeaf(Foundation::UInt64 root, Foundation::Size arenaPages,
                                           Foundation::UInt64*& output) noexcept;

/// Bootstrap BSP-only aperture. Prepare before any access; Close before APs
/// join the root. Every access runs with interrupts off, outside an ISR and
/// outside AddressSpace transactions, and restores the original leaf before
/// returning. The 256-bus ECAM aperture therefore needs only one PTE, without
/// permanently mapping device pages or exceeding the 64-page root arena.
[[nodiscard]] bool PrepareAmd64MmioWindow(Foundation::UInt64 root) noexcept;
[[nodiscard]] bool CloseAmd64MmioWindow() noexcept;
} // namespace Asteria::Platform::Amd64::Device
#endif
