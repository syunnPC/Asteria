// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_ABI_DESKTOP_SESSION_HPP_INCLUDED
#define ASTERIA_ABI_DESKTOP_SESSION_HPP_INCLUDED
#include "Foundation/Types.hpp"
namespace Asteria::Abi
{
// BOOT.CFG session modes.
inline constexpr Foundation::UInt64 SessionConsole{0U};
inline constexpr Foundation::UInt64 SessionDesktop{1U};
inline constexpr Foundation::UInt64 ConsoleInputSources{3U};
// Return to the console after three deaths.
inline constexpr Foundation::UInt64 DesktopCompositorDeathLimit{3U};
// Root channels, real driver seats and native image pools.
inline constexpr Foundation::UInt64 DesktopGrantEndpoints{48U};
inline constexpr Foundation::UInt64 DesktopGrantWaitSets{16U};
inline constexpr Foundation::UInt64 DesktopGrantMemoryPairs{128U};
}
#endif
