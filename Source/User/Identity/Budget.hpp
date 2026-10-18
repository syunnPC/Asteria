// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_IDENTITY_BUDGET_HPP_INCLUDED
#define ASTERIA_USER_IDENTITY_BUDGET_HPP_INCLUDED
#include "User/Identity/Identity.hpp"
namespace Asteria::User::Identity
{
// Runtime pages are separate from executable segments; both ISAs use ordinary roots, 512-page runs
// and two MemoryObjects.
inline constexpr UInt64 SeatPageBytes{4096U};
inline constexpr UInt64 AuthRunPages{512U};
inline constexpr UInt64 AuthArenaPages{ArenaBytes / SeatPageBytes};
inline constexpr Size AuthRuns{AuthArenaPages / AuthRunPages};
inline constexpr Size AuthMemoryObjects{2U};
inline constexpr UInt64 AuthMemoryPages{AuthArenaPages / AuthMemoryObjects};
inline constexpr UInt64 AuthRuntimePages{AuthArenaPages};
inline constexpr UInt64 AuthFixturePages{StorageBytes / SeatPageBytes};
inline constexpr Size AuthFixtureFrameWord{45U};
inline constexpr Size AuthFixtureOffsetWord{49U};
inline constexpr Size AuthLimiterFrameWord{50U};
static_assert(sizeof(LimiterState) <= SeatPageBytes);
inline constexpr UInt64 AuthEndpoints{3U};
inline constexpr UInt64 AuthFrames{AuthRuns + 4U};
inline constexpr UInt64 AuthMemories{8U};
inline constexpr UInt32 LoginWidth{640U};
inline constexpr UInt32 LoginHeight{400U};
inline constexpr UInt64 LoginPixelsPages{static_cast<UInt64>(LoginWidth) * LoginHeight * 4U / SeatPageBytes};
inline constexpr UInt64 LoginRuntimePages{LoginPixelsPages + 1U};
inline constexpr UInt64 LoginEndpoints{1U};
inline constexpr UInt64 LoginFrames{6U};
inline constexpr UInt64 LoginMemories{6U};
inline constexpr UInt64 AuthArenaAddress{0x6000'0000U};
inline constexpr UInt64 AuthPasswordAddress{0x6400'0000U};
inline constexpr UInt64 AuthFixtureAddress{0x6420'0000U};
inline constexpr UInt64 LoginPixelsAddress{0x6000'0000U};
inline constexpr UInt64 LoginPasswordAddress{0x6020'0000U};
static_assert(AuthArenaPages == 16384U && AuthRuns == 32U && AuthMemoryPages == 8192U);
static_assert(AuthArenaPages * SeatPageBytes == ArenaBytes && AuthRuntimePages == 16384U);
static_assert(LoginPixelsPages == 250U &&
              LoginPixelsPages * SeatPageBytes == static_cast<UInt64>(LoginWidth) * LoginHeight * 4U);
static_assert(AuthArenaAddress + ArenaBytes == AuthPasswordAddress);
} // Namespace Asteria::User::Identity
#endif
