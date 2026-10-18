// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_GUEST_PATTERN_CLIENT_HPP_INCLUDED
#define ASTERIA_TESTS_GUEST_PATTERN_CLIENT_HPP_INCLUDED
#include "Abi/ComponentBootstrap.hpp"
namespace Asteria::Test
{
// Distinct images in one client-owned pool. The second image is smaller than the first; the scene
// exercises output clipping.
inline constexpr Foundation::UInt32 PatternWidth{160U};
inline constexpr Foundation::UInt32 PatternHeight{64U};
inline constexpr Foundation::UInt32 PatternSecondaryHeight{48U};
// Native captures expose the full shaped title.
inline constexpr Foundation::UInt32 PatternNativeInitialWidth{400U};
inline constexpr Foundation::UInt32 PatternNativeInitialHeight{128U};
inline constexpr Foundation::UInt32 PatternPatchWidth{24U};
inline constexpr Foundation::UInt32 PatternPatchHeight{18U};
inline constexpr Foundation::UInt64 PatternImagePages{(PatternWidth * PatternHeight * 4U + 4095U) / 4096U};
inline constexpr Foundation::UInt64 PatternSecondaryImagePages{(PatternWidth * PatternSecondaryHeight * 4U + 4095U) /
                                                               4096U};
static_assert(PatternImagePages * 2U <= Abi::ComponentPatternPages);
static_assert(PatternSecondaryImagePages * 2U <= Abi::ComponentPatternSecondaryPages);
static_assert(Abi::ComponentPatternPages == 2U * Abi::ComponentCompositorPages);
} // Namespace Asteria::Test
#endif
