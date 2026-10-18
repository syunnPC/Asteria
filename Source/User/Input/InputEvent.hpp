// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_INPUT_INPUT_EVENT_HPP_INCLUDED
#define ASTERIA_USER_INPUT_INPUT_EVENT_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::User::Input
{
using Foundation::Int32;
using Foundation::Size;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt8;

enum class EventKind : UInt8
{
    KeyDown = 1U,
    KeyUp = 2U,
    Motion = 3U,
    Buttons = 4U,
    Wheel = 5U,
};

/// Little-endian frame ABI, 24 bytes, alignment 4. Usage is keyboard page 0x07. Modifiers is the
/// HID LCtrl..RGui byte. Dx/Dy are relative screen coordinates (+x right, +y down); wheel is
/// positive up (away from the user). Buttons: left=1,right=2,middle=4. Fields irrelevant to Kind
/// and Reserved are zero. Source is a broker device tag. Frame adapters must encode/decode
/// fields and validate Kind explicitly.
struct InputEvent
{
    EventKind Kind;
    UInt8 Modifiers;
    UInt16 Usage;
    UInt32 Source;
    Int32 Dx;
    Int32 Dy;
    UInt8 Buttons;
    UInt8 Reserved[3U];
    Int32 Wheel;
};
static_assert(sizeof(InputEvent) == 24U);
static_assert(alignof(InputEvent) == 4U);
static_assert(__builtin_offsetof(InputEvent, Kind) == 0U);
static_assert(__builtin_offsetof(InputEvent, Modifiers) == 1U);
static_assert(__builtin_offsetof(InputEvent, Usage) == 2U);
static_assert(__builtin_offsetof(InputEvent, Source) == 4U);
static_assert(__builtin_offsetof(InputEvent, Dx) == 8U);
static_assert(__builtin_offsetof(InputEvent, Dy) == 12U);
static_assert(__builtin_offsetof(InputEvent, Buttons) == 16U);
static_assert(__builtin_offsetof(InputEvent, Reserved) == 17U);
static_assert(__builtin_offsetof(InputEvent, Wheel) == 20U);

/// Maximum keyboard diff: six releases, six makes, eight modifier changes.
/// Caller-owned output; no callbacks, allocation, blocking or system calls.
struct EventBatch
{
    InputEvent Events[20U];
    Size Count;
};

inline void AppendKey(EventBatch& output, const UInt32 source, const UInt16 usage, const UInt8 modifiers,
                      const EventKind kind) noexcept
{
    if (output.Count < 20U)
    {
        output.Events[output.Count++] = InputEvent{
            .Kind = kind,
            .Modifiers = modifiers,
            .Usage = usage,
            .Source = source,
            .Dx = 0,
            .Dy = 0,
            .Buttons = 0U,
            .Reserved = {},
            .Wheel = 0,
        };
    }
}

/// Shared packet conversion; output is replaced, buttons state updated.
inline void PointerEvents(EventBatch& output, const UInt32 source, const Int32 dx, const Int32 dy, const UInt8 buttons,
                          const Int32 wheel, UInt8& previousButtons) noexcept
{
    output.Count = 0U;
    if ((dx != 0) || (dy != 0))
    {
        output.Events[output.Count++] = InputEvent{
            .Kind = EventKind::Motion,
            .Modifiers = 0U,
            .Usage = 0U,
            .Source = source,
            .Dx = dx,
            .Dy = dy,
            .Buttons = 0U,
            .Reserved = {},
            .Wheel = 0,
        };
    }
    if (buttons != previousButtons)
    {
        output.Events[output.Count++] = InputEvent{
            .Kind = EventKind::Buttons,
            .Modifiers = 0U,
            .Usage = 0U,
            .Source = source,
            .Dx = 0,
            .Dy = 0,
            .Buttons = buttons,
            .Reserved = {},
            .Wheel = 0,
        };
    }
    if (wheel != 0)
    {
        output.Events[output.Count++] = InputEvent{
            .Kind = EventKind::Wheel,
            .Modifiers = 0U,
            .Usage = 0U,
            .Source = source,
            .Dx = 0,
            .Dy = 0,
            .Buttons = 0U,
            .Reserved = {},
            .Wheel = wheel,
        };
    }
    previousButtons = buttons;
}

[[nodiscard]] constexpr Int32 SignedByte(const UInt8 byte) noexcept
{
    return (byte < 128U) ? static_cast<Int32>(byte) : static_cast<Int32>(byte) - 256;
}
} // Namespace Asteria::User::Input

#endif
