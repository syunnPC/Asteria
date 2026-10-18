// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_IMAGING_CURSOR_SET_HPP_INCLUDED
#define ASTERIA_USER_IMAGING_CURSOR_SET_HPP_INCLUDED
#include "User/Imaging/Imaging.hpp"
namespace Asteria::User::Imaging
{
// Ten cursor roles with fixed ACSR v1 identifiers.
enum class CursorRole : UInt16
{
    Arrow,
    Text,
    Busy,
    ResizeHorizontal,
    ResizeVertical,
    ResizeNorthWestSouthEast,
    ResizeNorthEastSouthWest,
    Move,
    Link,
    Unavailable
};
inline constexpr UInt32 CursorRoleCount = 10U;
inline constexpr UInt32 MaximumCursorDimension = 128U;
inline constexpr UInt32 MaximumFramesPerRole = 32U;
inline constexpr UInt32 MaximumCursorFrames = CursorRoleCount * MaximumFramesPerRole;
inline constexpr UInt32 MaximumFrameDelay = 60000U;
inline constexpr UInt32 CursorVersion = 1U;
inline constexpr UInt32 CursorHeaderBytes = 16U;
inline constexpr UInt32 CursorRoleBytes = 4U;
inline constexpr UInt32 CursorFrameBytes = 20U;
static_assert(MaximumCursorFrames == 320U);
struct CursorFrame
{
    Image Pixels{};
    UInt16 HotspotX{};
    UInt16 HotspotY{};
    UInt32 DelayMs{};
    CursorRole Role{};
};
struct CursorSet
{
    CursorFrame Frames[MaximumCursorFrames]{};
    UInt32 First[CursorRoleCount]{};
    UInt32 Length[CursorRoleCount]{};
    UInt32 Count{};
};
[[nodiscard]] Status DecodeCursorSet(Bytes input, Buffer arena, Workspace& work, CursorSet& set) noexcept;
} // Namespace Asteria::User::Imaging
#endif
