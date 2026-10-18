// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_TOOLKIT_SESSION_COMPONENT_HPP_INCLUDED
#define ASTERIA_USER_TOOLKIT_SESSION_COMPONENT_HPP_INCLUDED
#include "User/Toolkit/Theme.hpp"
#include "User/Session/ClientConnection.hpp"
namespace Asteria::User::Toolkit
{
struct SessionFontWorkspace
{
    FontContext Context{};
    Font::Font Face{};
    Font::CacheEntry Entries[64U]{};
    UInt8 Cache[64U * 1024U]{};
};
inline constexpr UInt64 SessionWorkspacePages{(sizeof(SessionFontWorkspace) + Session::SessionPageBytes - 1U) /
                                              Session::SessionPageBytes};
static_assert(SessionWorkspacePages <= Session::SessionMaximumFramePages);
// Four 150-pixel columns of 38-pixel rows: the 28 sample controls, four of
// them two rows tall, fill eight rows inside a 10-pixel margin. With its
// frame at the 32-pixel cascade origin, 600x314 fits a 640x400 output.
inline constexpr UInt32 DemoColumns{4U};
inline constexpr UInt32 DemoCellWidth{150U};
inline constexpr UInt32 DemoRowHeight{38U};
inline constexpr UInt32 DemoRows{8U};
inline constexpr UInt32 DemoInitialWidth{DemoColumns * DemoCellWidth};
inline constexpr UInt32 DemoInitialHeight{DemoRows * DemoRowHeight + 10U};
static_assert(DemoColumns * DemoRows == 32U);
// One UI thread, reply, timer WaitSet; two pools of at most
// 16 Frames, private state/stack and font workspace.
inline constexpr Foundation::UInt16 SessionFrameLimit{35U};
inline constexpr Foundation::UInt16 SessionMemoryLimit{5U};
static_assert(SessionFrameLimit == 2U * Session::SessionMaximumPoolFrames + 3U);
[[nodiscard]] UInt64 RunSessionDemo(const System::SystemCallClient& client,
                                    const UInt64 (&block)[Abi::ComponentBlockWords],
                                    Session::ClientClock clock) noexcept;
} // Namespace Asteria::User::Toolkit
#endif
