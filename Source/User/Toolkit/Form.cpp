// SPDX-License-Identifier: Apache-2.0

#include "User/Toolkit/Form.hpp"

#include "User/Graphics/Blit.hpp"

namespace Asteria::User::Toolkit
{
void Damage::Add(Rect rectangle) noexcept
{
    if (User::Graphics::IsEmpty(rectangle))
    {
        return;
    }
    if (m_count < MaximumDamage)
    {
        m_rectangles[m_count++] = rectangle;
        return;
    }
    Foundation::Int64 left{rectangle.X};
    Foundation::Int64 top{rectangle.Y};
    Foundation::Int64 right{left + rectangle.Width};
    Foundation::Int64 bottom{top + rectangle.Height};
    for (UInt32 i{}; i < m_count; ++i)
    {
        const Rect r{m_rectangles[i]};
        if (r.X < left)
        {
            left = r.X;
        }
        if (r.Y < top)
        {
            top = r.Y;
        }
        if (static_cast<Foundation::Int64>(r.X) + r.Width > right)
        {
            right = static_cast<Foundation::Int64>(r.X) + r.Width;
        }
        if (static_cast<Foundation::Int64>(r.Y) + r.Height > bottom)
        {
            bottom = static_cast<Foundation::Int64>(r.Y) + r.Height;
        }
    }
    m_rectangles[0U] = {static_cast<Int32>(left), static_cast<Int32>(top), static_cast<UInt32>(right - left),
                        static_cast<UInt32>(bottom - top)};
    m_count = 1U;
}
Status Form::Bind(User::Graphics::MemorySurface first, User::Graphics::MemorySurface second,
                  IPresenter& presenter) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            if (m_rendering || m_lent[0U] || m_lent[1U])
            {
                return Status::Busy;
            }
            if (!User::Graphics::IsUsable(first) || !User::Graphics::IsUsable(second) || first.Width != second.Width ||
                first.Height != second.Height || first.Format != second.Format || first.Width > MaximumAxis ||
                first.Height > MaximumAxis || first.Stride > MaximumAxis || second.Stride > MaximumAxis)
            {
                return Status::Invalid;
            }
            const auto firstStart{reinterpret_cast<Foundation::UIntPointer>(first.Pixels)};
            const auto secondStart{reinterpret_cast<Foundation::UIntPointer>(second.Pixels)};
            const Size firstBytes{static_cast<Size>(first.Stride) * first.Height * sizeof(UInt32)};
            const Size secondBytes{static_cast<Size>(second.Stride) * second.Height * sizeof(UInt32)};
            if (firstStart < secondStart + secondBytes && secondStart < firstStart + firstBytes)
            {
                return Status::Invalid;
            }
            m_images[0U] = first;
            m_images[1U] = second;
            m_capacityWidth = first.Width;
            m_capacityHeight = first.Height;
            m_presenter = &presenter;
            m_last = -1;
            m_damage.Clear();
            m_carry.Clear();
            UiSetBounds({0, 0, first.Width, first.Height}, true);
            UiDamage(User::Graphics::Bounds(first));
            return Status::Ok;
        });
}
Status Form::Resize(const UInt32 width, const UInt32 height) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            if (m_closed || m_rendering || m_presenter == nullptr)
            {
                return Status::Busy;
            }
            if (width == 0U || height == 0U || width > m_capacityWidth || height > m_capacityHeight)
            {
                return Status::Limit;
            }
            for (auto& image : m_images)
            {
                image.Width = width;
                image.Height = height;
            }
            m_damage.Clear();
            UiSetBounds({0, 0, width, height}, true);
            m_damage.Clear();
            UiDamage({0, 0, width, height});
            return Status::Ok;
        });
}
Status Form::Close() noexcept
{
    return m_application.Call(
        [this]() noexcept
        {
            if (m_closed)
            {
                return Status::Ok;
            }
            if (m_closing || m_rendering)
            {
                return Status::Busy;
            }
            m_closing = true;
            Event closing{EventKind::Closing};
            UiEmit(closing);
            m_closing = false;
            if (closing.Cancel)
            {
                return Status::Busy;
            }
            static_cast<void>(m_application.ReleaseInput(*this));
            m_closed = true;
            m_visible = false;
            Event closed{EventKind::Closed};
            UiEmit(closed);
            return Status::Ok;
        });
}
Status Form::ReturnImage(UInt32 image) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            if (image >= 2U || !m_lent[image])
            {
                return Status::Invalid;
            }
            m_lent[image] = false;
            return Status::Ok;
        });
}
void Form::UiDamage(Rect rect) noexcept
{
    if (m_presenter != nullptr)
    {
        m_damage.Add(User::Graphics::ClipToSurface(m_images[0U], rect));
    }
}
Status Form::Render() noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            return UiRender();
        });
}
Status Form::UiRender() noexcept
{
    if (m_rendering)
    {
        return Status::Busy;
    }
    if (m_presenter == nullptr || m_damage.Count() == 0U || !m_visible)
    {
        return Status::Ok;
    }
    UInt32 image{m_last == 0 ? 1U : 0U};
    if (m_lent[image])
    {
        image = 1U - image;
    }
    if (m_lent[image])
    {
        return Status::Busy;
    }
    m_rendering = true;
    // Copy all earlier damage forward; do not write lent images.
    if (m_last >= 0 && image != static_cast<UInt32>(m_last))
    {
        for (UInt32 i{}; i < m_carry.Count(); ++i)
        {
            const Rect rect{m_carry.Rectangles()[i]};
            static_cast<void>(User::Graphics::CopyRect(m_images[image], m_images[static_cast<UInt32>(m_last)], rect,
                                                       {rect.X, rect.Y}));
        }
    }
    m_frameDamage = m_damage;
    m_damage.Clear();
    for (UInt32 i{}; i < m_frameDamage.Count(); ++i)
    {
        UiPaint(m_images[image], m_frameDamage.Rectangles()[i]);
    }
    if (!m_presenter->Present(image, m_frameDamage.Rectangles(), m_frameDamage.Count()))
    {
        for (UInt32 i{}; i < m_frameDamage.Count(); ++i)
        {
            m_damage.Add(m_frameDamage.Rectangles()[i]);
        }
        m_rendering = false;
        return Status::Busy;
    }
    if (m_last < 0 || image != static_cast<UInt32>(m_last))
    {
        m_carry.Clear();
    }
    for (UInt32 i{}; i < m_frameDamage.Count(); ++i)
    {
        m_carry.Add(m_frameDamage.Rectangles()[i]);
    }
    m_lent[image] = true;
    m_last = static_cast<Int32>(image);
    m_rendering = false;
    return Status::Ok;
}
Status Form::SetDefaultButtons(Control* accept, Control* cancel) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            if ((accept != nullptr && accept->UiForm() != this) || (cancel != nullptr && cancel->UiForm() != this))
            {
                return Status::Invalid;
            }
            m_accept = accept;
            m_cancel = cancel;
            return Status::Ok;
        });
}
} // Namespace Asteria::User::Toolkit
