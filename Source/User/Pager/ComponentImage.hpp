// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_PAGER_COMPONENT_IMAGE_HPP_INCLUDED
#define ASTERIA_USER_PAGER_COMPONENT_IMAGE_HPP_INCLUDED

#include "Abi/ComponentImageTable.hpp"
#include "Foundation/Types.hpp"
#include "User/Loader/ExecutableImage.hpp"

namespace Asteria::User::Pager
{
/// The loadable segments a component image may have: its text, its read-only data and one
/// zero-initialized writable segment with no file bytes, its.bss. The root service provides the
/// stack and start block.
inline constexpr Foundation::Size MaximumComponentSegments{3U};

/// The largest segment, in 4 KiB pages, by platform: what one created Frame holds and the pager's
/// mapping window stages, so the root staging window has at least as many pages. report-protocol
/// input grows AMD64 text to 33 pages; allow 40. The build checks every module image against its
/// platform bound. IA-64 retains its 128-page bound: its bundles make the same component's text
/// about twice as large; with the AHCI driver and the filesystem service it no longer fitted 32
/// pages, and with the NIC driver and the network stack it no longer fits 64 (78 pages of text).
inline constexpr Foundation::UInt64 Amd64ComponentSegmentPages{40U};
inline constexpr Foundation::UInt64 Ia64ComponentSegmentPages{128U};

/// The rules a native component image is described under: the ISA's machine, user range and entry
/// naming, and the root service's limits.
[[nodiscard]] constexpr Loader::MachineRules MakeComponentRules(const Loader::ExecutableMachine machine,
                                                                const Foundation::UInt64 userStart,
                                                                const Foundation::UInt64 userLimit,
                                                                const bool entryDescriptor,
                                                                const Foundation::UInt64 maximumSegmentPages) noexcept
{
    return Loader::MachineRules{.Machine = machine,
                                .UserStart = userStart,
                                .UserLimit = userLimit,
                                .EntryDescriptor = entryDescriptor,
                                .MaximumSegments = MaximumComponentSegments,
                                .MaximumSegmentPages = maximumSegmentPages,
                                .WritableFileBytes = false};
}

/// The table of module files the boot application writes and the kernel maps read-only into the
/// pager: a count and a zero word, then per image ComponentImageNameBytes of zero-padded ASCII
/// name, its offset from the table's start and its size, each a little-endian word; the images
/// follow.
inline constexpr Foundation::Size ComponentImageNameBytes{16U};
inline constexpr Foundation::Size MaximumComponentImages{Abi::ComponentImageMaximumCount};

/// Finds the image name (at most ComponentImageNameBytes - 1 characters)
/// in the table of tableBytes at table: its bytes and size. False for a
/// malformed table, a table of more than MaximumComponentImages images, or
/// no such image.
[[nodiscard]] bool FindComponentImage(const Foundation::UInt8* table, Foundation::Size tableBytes, const char* name,
                                      const Foundation::UInt8*& image, Foundation::Size& imageBytes) noexcept;
} // Namespace Asteria::User::Pager

#endif // ASTERIA_USER_PAGER_COMPONENT_IMAGE_HPP_INCLUDED
