// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_TOOLKIT_FORM_HPP_INCLUDED
#define ASTERIA_USER_TOOLKIT_FORM_HPP_INCLUDED

#include "User/Toolkit/Control.hpp"

namespace Asteria::User::Toolkit
{
class IPresenter
{
public:
    // A success lends image until ReturnImage. Failure lends nothing.
    [[nodiscard]] virtual bool Present(UInt32 image, const Rect* damage, UInt32 count) noexcept = 0;

protected:
    ~IPresenter() = default;
};
class Damage final
{
public:
    void Add(Rect rectangle) noexcept;
    void Clear() noexcept
    {
        m_count = 0U;
    }
    [[nodiscard]] UInt32 Count() const noexcept
    {
        return m_count;
    }
    [[nodiscard]] const Rect* Rectangles() const noexcept
    {
        return m_rectangles;
    }

private:
    Rect m_rectangles[MaximumDamage]{};
    UInt32 m_count{};
};
class Form final : public Control
{
public:
    explicit Form(Application& application) noexcept : Control(application, Kind::Form)
    {
    }
    [[nodiscard]] Status Bind(User::Graphics::MemorySurface first, User::Graphics::MemorySurface second,
                              IPresenter& presenter) noexcept;
    [[nodiscard]] Status ReturnImage(UInt32 image) noexcept;
    [[nodiscard]] Status Render() noexcept;
    // Resize changes views only; lent pixels remain untouched.
    [[nodiscard]] Status Resize(UInt32 width, UInt32 height) noexcept;
    [[nodiscard]] Status Close() noexcept;
    [[nodiscard]] bool UiClosed() const noexcept
    {
        return m_closed;
    }
    [[nodiscard]] Status SetDefaultButtons(Control* accept, Control* cancel) noexcept;
    void UiDamage(Rect rect) noexcept;
    [[nodiscard]] Control* UiDefault(bool cancel) const noexcept
    {
        return cancel ? m_cancel : m_accept;
    }

private:
    [[nodiscard]] Status UiRender() noexcept;
    User::Graphics::MemorySurface m_images[2U]{};
    IPresenter* m_presenter{};
    Damage m_damage{};
    Damage m_carry{};
    Damage m_frameDamage{};
    bool m_lent[2U]{};
    Int32 m_last{-1};
    bool m_rendering{};
    UInt32 m_capacityWidth{};
    UInt32 m_capacityHeight{};
    bool m_closed{};
    bool m_closing{};
    Control* m_accept{};
    Control* m_cancel{};
};
} // Namespace Asteria::User::Toolkit
#endif
