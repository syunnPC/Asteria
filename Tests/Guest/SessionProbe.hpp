// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_GUEST_SESSION_PROBE_HPP_INCLUDED
#define ASTERIA_TESTS_GUEST_SESSION_PROBE_HPP_INCLUDED
#include "Abi/ComponentBootstrap.hpp"
namespace Asteria::Test
{
// Deliberately below either current pager grant.
inline constexpr Foundation::UInt32 ProbeWidth{320U};
inline constexpr Foundation::UInt32 ProbeHeight{200U};
inline constexpr Foundation::UInt64 ProbePages{(ProbeWidth * ProbeHeight * 4U + 4095U) / 4096U};
inline constexpr Foundation::UInt32 ProbeWindowWidth{160U};
inline constexpr Foundation::UInt32 ProbeWindowHeight{100U};
static_assert(ProbePages == Abi::ComponentSessionProbePages);
} // Namespace Asteria::Test
#endif
