// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_SESSION_WINDOW_MANAGER_HPP_INCLUDED
#define ASTERIA_USER_SESSION_WINDOW_MANAGER_HPP_INCLUDED
#include "User/Session/Scene.hpp"
#include "User/Session/ClientEvents.hpp"
namespace Asteria::User::Session
{
// Runner-readable geometry and timing.
inline constexpr Foundation::UInt32 CascadeOrigin{32U};
inline constexpr Foundation::UInt32 CascadeStep{24U};
inline constexpr Foundation::UInt32 VisibleTitleWidth{32U};
inline constexpr Foundation::UInt64 SystemRaiseIgnoreUs{300000U};
inline constexpr Foundation::UInt64 DoubleClickUs{500000U};
inline constexpr Foundation::UInt32 DoubleClickDistance{4U};
inline constexpr Foundation::UInt64 OutlinePassUs{50000U};
struct SizeHints
{
    Foundation::UInt32 MinWidth{1U};
    Foundation::UInt32 MinHeight{1U};
    Foundation::UInt32 MaxWidth{8192U};
    Foundation::UInt32 MaxHeight{8192U};
    Foundation::UInt32 IncrementWidth{0U};
    Foundation::UInt32 IncrementHeight{0U};
    Foundation::UInt32 BaseWidth{0U};
    Foundation::UInt32 BaseHeight{0U};
};
/// Pure policy; caller owns Scene and four client states in static storage.
/// slot = client * 4 + surface - 1. Times are monotonic microseconds.
class WindowManager final
{
public:
    void Configure(Scene& scene, ClientEvents* clients) noexcept;
    void UserLaunch(Foundation::UInt64 serial) noexcept
    {
        if (serial != 0U)
        {
            m_launch = serial;
        }
    }
    [[nodiscard]] bool Map(Foundation::Size slot, Foundation::UInt32 width, Foundation::UInt32 height,
                           Foundation::UInt64 launchSerial, Foundation::UInt64 now) noexcept;
    void Unmap(Foundation::Size slot, Foundation::UInt64 now) noexcept;
    void ForgetClient(Foundation::Size client) noexcept
    {
        if (client < SessionMaximumClients)
        {
            m_firstWindow[client] = false;
        }
    }
    void Raise(Foundation::Size slot, bool user, Foundation::UInt64 now) noexcept;
    void Focus(Foundation::Size slot, Foundation::UInt64 now) noexcept;
    [[nodiscard]] bool SetHints(Foundation::Size slot, const SizeHints& hints) noexcept;
    void AcceptArrange(Foundation::Size slot, Foundation::UInt64 serial, Foundation::UInt64 now) noexcept;
    void Pointer(Graphics::Point position, Foundation::UInt8 buttons, Foundation::Int32 wheel,
                 Foundation::UInt64 now) noexcept;
    /// HID usage and Input Broker modifier bits. True means consumed.
    [[nodiscard]] bool Key(Foundation::UInt16 usage, Foundation::UInt8 modifiers, bool down,
                           Foundation::UInt64 now) noexcept;
    /// Local shortcuts remain available on a stale seat; ordinary keys still require the ordering
    /// fence.
    [[nodiscard]] bool Shortcut(Foundation::UInt16 usage, Foundation::UInt8 modifiers, bool down,
                                Foundation::UInt64 now) noexcept;
    void Tick(Foundation::UInt64 now) noexcept;
    void LastPass(Foundation::UInt64 microseconds) noexcept
    {
        m_outlineMove = microseconds > OutlinePassUs;
    }
    [[nodiscard]] Foundation::Size Focused() const noexcept
    {
        return m_focus;
    }
    [[nodiscard]] bool TakeNewTerminal() noexcept
    {
        const bool value = m_newTerminal;
        m_newTerminal = false;
        return value;
    }

private:
    struct Window
    {
        SizeHints Hints{};
        Graphics::Rect Restore{};
        Foundation::UInt64 IgnoreUntil{0U};
        Foundation::UInt64 Arrange{0U};
        bool PendingArrange{false};
    };
    void Event(Foundation::Size slot, SessionEventKind kind, Foundation::UInt64 a, Foundation::UInt64 b,
               Foundation::UInt64 c, Foundation::UInt64 now) noexcept;
    void Arrange(Foundation::Size slot, Foundation::UInt64 now) noexcept;
    void Maximize(Foundation::Size slot, Foundation::UInt64 now) noexcept;
    void Close(Foundation::Size slot, Foundation::UInt64 now) noexcept;
    void Move(Graphics::Point position, Foundation::UInt64 now) noexcept;
    void Switch(bool reverse) noexcept;
    Scene* m_scene{nullptr};
    ClientEvents* m_clients{nullptr};
    Window m_windows[Scene::MaximumWindows]{};
    Foundation::UInt8 m_mru[Scene::MaximumWindows]{};
    Foundation::UInt8 m_consumed[256U]{};
    Foundation::UInt8 m_keyOwner[256U]{};
    bool m_firstWindow[SessionMaximumClients]{};
    Foundation::Size m_mruCount{0U};
    Foundation::Size m_focus{Scene::NoWindow};
    Foundation::Size m_grab{Scene::NoWindow};
    Foundation::Size m_hover{Scene::NoWindow};
    Foundation::Size m_lastTitle{Scene::NoWindow};
    Foundation::Size m_selected{Scene::NoWindow};
    Foundation::UInt64 m_launch{0U};
    Foundation::UInt64 m_serial{0U};
    Foundation::UInt64 m_lastClick{0U};
    Foundation::UInt32 m_cascade{0U};
    Graphics::Point m_anchor{};
    Graphics::Point m_position{};
    Graphics::Point m_clickPoint{};
    Graphics::Rect m_start{};
    Graphics::Rect m_dragFrame{};
    FramePart m_part{FramePart::None};
    Foundation::UInt8 m_buttons{0U};
    Foundation::UInt8 m_ignoredButtons{0U};
    bool m_outlineMove{false};
    bool m_grabOutline{false};
    bool m_switching{false};
    bool m_newTerminal{false};
};
[[nodiscard]] Foundation::UInt32 SnapSize(Foundation::Int64 requested, Foundation::UInt32 minimum,
                                          Foundation::UInt32 maximum, Foundation::UInt32 increment,
                                          Foundation::UInt32 base) noexcept;
} // Namespace Asteria::User::Session
#endif
