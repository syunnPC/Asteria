// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_TOOLKIT_SESSION_ADAPTER_HPP_INCLUDED
#define ASTERIA_USER_TOOLKIT_SESSION_ADAPTER_HPP_INCLUDED
#include "User/Toolkit/Form.hpp"
#include "User/Session/ClientConnection.hpp"
#include "User/Input/Keymap.hpp"
#include "User/Input/KeyRepeat.hpp"

namespace Asteria::User::Toolkit
{
// One Form per application connection/domain.
class SessionAdapter final : public IEventSource, public IPresenter
{
public:
    explicit SessionAdapter(Session::IClientConnection& connection,
                            Input::KeyboardLayout layout = Input::KeyboardLayout::Us) noexcept
        : m_connection{connection}, m_layout{layout}
    {
    }
    [[nodiscard]] Status Bind(Form& form, UInt32 width, UInt32 height) noexcept;
    [[nodiscard]] bool Poll(SourceEvent& event) noexcept override;
    void WaitUntil(UInt64 deadline) noexcept override;
    [[nodiscard]] UInt64 Now() noexcept override
    {
        return m_connection.Now();
    }
    [[nodiscard]] bool Present(UInt32 image, const Rect* damage, UInt32 count) noexcept override;
    [[nodiscard]] bool Healthy() const noexcept
    {
        return m_healthy && m_connection.Healthy();
    }
    [[nodiscard]] UInt64 Serial() const noexcept
    {
        return m_serial;
    }
    [[nodiscard]] UInt64 Shown() const noexcept
    {
        return m_shown;
    }
    [[nodiscard]] UInt64 ArrangeFlags() const noexcept
    {
        return m_flags;
    }
    void Observe(void* context, void (*observer)(void*, const Session::SessionEvent&) noexcept) noexcept
    {
        m_observerContext = context;
        m_observer = observer;
    }

private:
    void Handle(const Session::SessionEvent& event) noexcept;
    void KeyEvent(Foundation::UInt16 usage, UInt8 modifiers, bool down) noexcept;
    void Pointer(EventKind kind, Point position, UInt32 buttons, UInt32 button = 1U) noexcept;
    void ClearWords(UInt64 first = 1U) noexcept;
    [[nodiscard]] bool Send(UInt32 method) noexcept;
    Session::IClientConnection& m_connection;
    Input::KeyboardLayout m_layout;
    Input::KeyRepeat m_repeat{};
    Form* m_form{};
    Control* m_savedFocus{};
    User::Graphics::MemorySurface m_images[2U]{};
    UInt64 m_words[Session::SessionMaximumWords]{};
    UInt64 m_serial{};
    UInt64 m_shown{};
    UInt64 m_arrange{};
    UInt64 m_flags{};
    UInt32 m_lent{};
    UInt32 m_buttons{};
    Point m_position{};
    bool m_healthy{true};
    bool m_focused{};
    bool m_destroyed{};
    bool m_caps{};
    void* m_observerContext{};
    void (*m_observer)(void*, const Session::SessionEvent&) noexcept {};
};
} // Namespace Asteria::User::Toolkit
#endif
