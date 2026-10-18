// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_TOOLKIT_TYPES_HPP_INCLUDED
#define ASTERIA_USER_TOOLKIT_TYPES_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "User/Graphics/Surface.hpp"
#include "User/Text/Utf8.hpp"

namespace Asteria::User::Toolkit
{
using Foundation::Int32;
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::UInt8;
using Graphics::Color;
using Graphics::Point;
using Graphics::Rect;

// Caller-owned bounded storage; no heap allocation.
inline constexpr UInt32 MaximumControls{128U};
inline constexpr UInt32 MaximumChildren{32U};
inline constexpr UInt32 MaximumDepth{16U};
inline constexpr UInt32 MaximumText{512U};
inline constexpr UInt32 MaximumItems{32U};
inline constexpr UInt32 MaximumItemText{64U};
inline constexpr UInt32 MaximumQueue{64U};
inline constexpr UInt32 MaximumTimers{16U};
inline constexpr UInt32 MaximumDamage{16U};
inline constexpr UInt32 MaximumCells{8U};
inline constexpr UInt32 MaximumAxis{4096U};
inline constexpr UInt32 CaptureBytes{32U};
static_assert(MaximumControls <= MaximumChildren * MaximumDepth);
static_assert(MaximumAxis * MaximumAxis * sizeof(UInt32) <= 64U * 1024U * 1024U);

enum class Status : UInt8
{
    Ok,
    Invalid,
    Limit,
    QueueFull,
    Stopped,
    Busy,
    WrongThread
};
enum class Dock : UInt8
{
    None,
    Top,
    Bottom,
    Left,
    Right,
    Fill
};
enum class Anchor : UInt8
{
    None = 0,
    Left = 1,
    Top = 2,
    Right = 4,
    Bottom = 8
};
[[nodiscard]] constexpr Anchor operator|(Anchor a, Anchor b) noexcept
{
    return static_cast<Anchor>(static_cast<UInt8>(a) | static_cast<UInt8>(b));
}
[[nodiscard]] constexpr bool Has(Anchor value, Anchor flag) noexcept
{
    return (static_cast<UInt8>(value) & static_cast<UInt8>(flag)) != 0U;
}
struct Insets
{
    UInt32 Left{};
    UInt32 Top{};
    UInt32 Right{};
    UInt32 Bottom{};
};
enum class Align : UInt8
{
    Start,
    Center,
    End
};
enum class Key : UInt8
{
    None,
    Tab,
    Left,
    Right,
    Up,
    Down,
    Home,
    End,
    Backspace,
    Delete,
    Space,
    Enter,
    Escape,
    A
};
enum class EventKind : UInt8
{
    Click,
    CheckedChanged,
    SelectedIndexChanged,
    TextChanged,
    KeyDown,
    KeyUp,
    KeyPress,
    MouseDown,
    MouseUp,
    MouseMove,
    MouseEnter,
    MouseLeave,
    MouseWheel,
    Paint,
    Resize,
    GotFocus,
    LostFocus,
    ValueChanged,
    Tick,
    DoubleClick,
    Closing,
    Closed,
    Count
};
class Control;
class Graphics;
struct Event
{
    EventKind Kind{};
    Point Position{};
    Key Code{};
    UInt32 Scalar{};
    Int32 Wheel{};
    bool Shift{};
    bool ControlKey{};
    bool Alt{};
    bool Handled{};
    Graphics* Drawing{};
    Int32 Item{-1};
    UInt32 Buttons{};
    UInt32 Button{1U};
    bool Cancel{};
};

// Small lambdas are copied into aligned implicit-lifetime storage, without new.
class Action final
{
public:
    template <typename T> [[nodiscard]] static Action Bind(const T& function) noexcept
    {
        static_assert(__is_trivially_copyable(T));
        static_assert(sizeof(T) <= CaptureBytes && alignof(T) <= alignof(UInt64));
        Action result{};
        // Empty closures carry no state; their padding need not be read.
        if constexpr (!__is_empty(T))
        {
            __builtin_memcpy(result.m_capture, &function, sizeof(T));
        }
        result.m_call = &Execute<T>;
        return result;
    }
    void operator()() noexcept
    {
        if (m_call != nullptr)
        {
            m_call(m_capture);
        }
    }

private:
    template <typename T> static void Execute(void* data) noexcept
    {
        if (data != nullptr)
        {
            (*static_cast<T*>(data))();
        }
    }
    alignas(UInt64) UInt8 m_capture[CaptureBytes]{};
    void (*m_call)(void*) noexcept {};
};
class Handler final
{
public:
    template <typename T> [[nodiscard]] static Handler Bind(const T& function) noexcept
    {
        static_assert(__is_trivially_copyable(T));
        static_assert(sizeof(T) <= CaptureBytes && alignof(T) <= alignof(UInt64));
        Handler result{};
        // Empty closures carry no state; their padding need not be read.
        if constexpr (!__is_empty(T))
        {
            __builtin_memcpy(result.m_capture, &function, sizeof(T));
        }
        result.m_call = &Execute<T>;
        return result;
    }
    void operator()(Control& control, Event& event) noexcept
    {
        if (m_call != nullptr)
        {
            m_call(m_capture, control, event);
        }
    }

private:
    template <typename T> static void Execute(void* data, Control& control, Event& event) noexcept
    {
        if (data != nullptr)
        {
            (*static_cast<T*>(data))(control, event);
        }
    }
    alignas(UInt64) UInt8 m_capture[CaptureBytes]{};
    void (*m_call)(void*, Control&, Event&) noexcept {};
};
struct TextView
{
    const char* Data{};
    UInt32 Length{};
};
template <Size N> [[nodiscard]] constexpr TextView Utf8(const char (&value)[N]) noexcept
{
    return {value, static_cast<UInt32>(N - 1U)};
}
[[nodiscard]] inline bool ValidText(TextView text, UInt32 bound) noexcept
{
    return text.Length <= bound && Text::ValidUtf8(reinterpret_cast<const UInt8*>(text.Data), text.Length);
}
[[nodiscard]] constexpr UInt32 Min(UInt32 a, UInt32 b) noexcept
{
    return a < b ? a : b;
}
[[nodiscard]] constexpr Int32 Clamp(Int32 v, Int32 lo, Int32 hi) noexcept
{
    return v < lo ? lo : (v > hi ? hi : v);
}
[[nodiscard]] constexpr bool Contains(Rect r, Point p) noexcept
{
    return p.X >= r.X && p.Y >= r.Y &&
           static_cast<Foundation::Int64>(p.X) < r.X + static_cast<Foundation::Int64>(r.Width) &&
           static_cast<Foundation::Int64>(p.Y) < r.Y + static_cast<Foundation::Int64>(r.Height);
}
[[nodiscard]] constexpr Rect Inset(Rect r, UInt32 amount) noexcept
{
    const UInt32 x{Min(amount, r.Width / 2U)};
    const UInt32 y{Min(amount, r.Height / 2U)};
    return {r.X + static_cast<Int32>(x), r.Y + static_cast<Int32>(y), r.Width - 2U * x, r.Height - 2U * y};
}
} // namespace Asteria::User::Toolkit
#endif
