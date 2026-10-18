// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_INPUT_PS2MOUSE_HPP_INCLUDED
#define ASTERIA_USER_INPUT_PS2MOUSE_HPP_INCLUDED

#include "User/Input/InputEvent.hpp"

namespace Asteria::User::Input
{
enum class MouseMode : UInt8
{
    Standard,
    IntelliMouse
};
enum class MouseResult : UInt8
{
    Pending,
    Packet,
    Unsynchronised,
    Overflow
};
/// Standard stream packets; ID 3 must be confirmed before selecting IntelliMouse.
/// PS/2 wheel positive down is converted to event positive up.
/// Overflow discards motion (not buttons/wheel). Only packet starts are synced:
/// payload bytes may have any bit pattern. Serialized, bounded, allocation-free.
class Ps2Mouse final
{
public:
    explicit constexpr Ps2Mouse(UInt32 source = 0U) noexcept : m_source{source}
    {
    }
    void Reset(MouseMode mode = MouseMode::Standard) noexcept;
    [[nodiscard]] MouseResult Feed(UInt8 byte, EventBatch& output) noexcept;

private:
    UInt32 m_source;
    MouseMode m_mode{MouseMode::Standard};
    UInt8 m_bytes[4U]{};
    UInt8 m_at{0U};
    UInt8 m_buttons{0U};
};
} // namespace Asteria::User::Input

#endif
