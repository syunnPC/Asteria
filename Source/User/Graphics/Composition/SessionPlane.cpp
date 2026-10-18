// SPDX-License-Identifier: Apache-2.0

#include "User/Graphics/Composition/SessionPlane.hpp"

#include "User/Graphics/Blit.hpp"
#include "User/Session/Palette.hpp"

namespace Asteria::User::Graphics::Composition
{
Rect SessionPlane::PhysicalBounds(const Output& output) const noexcept
{
    return ProjectBounds(m_bounds, output);
}
bool SessionPlane::Intersects(const Rect rectangle, const Output& output) const noexcept
{
    return m_visible && !IsEmpty(Intersect(rectangle, PhysicalBounds(output)));
}
bool SessionPlane::Covers(const Rect rectangle, const Output& output) const noexcept
{
    const Rect area = Intersect(rectangle, PhysicalBounds(output));
    return m_visible && !IsEmpty(rectangle) && (area.Width == rectangle.Width) && (area.Height == rectangle.Height);
}
PixelSurface<const Foundation::UInt32> SessionPlane::Source(const Rect rectangle, const Output& output) const noexcept
{
    if ((output.Scale != 1U) || !Covers(rectangle, output) || !IsUsable(m_buffer) ||
        (m_buffer.Format != output.Scanout.Format))
    {
        return {};
    }
    const auto x = static_cast<Foundation::Int64>(rectangle.X) + output.Origin.X - m_bounds.X;
    const auto y = static_cast<Foundation::Int64>(rectangle.Y) + output.Origin.Y - m_bounds.Y;
    return {.Pixels =
                m_buffer.Pixels + static_cast<Foundation::Size>(y) * m_buffer.Stride + static_cast<Foundation::Size>(x),
            .Width = rectangle.Width,
            .Height = rectangle.Height,
            .Stride = m_buffer.Stride,
            .Format = m_buffer.Format};
}
void SessionPlane::Render(const MemorySurface& target, const Rect rectangle, const Output& output) const noexcept
{
    if (!m_visible)
    {
        return;
    }
    const Rect bounds = PhysicalBounds(output);
    const Rect area = Intersect(rectangle, bounds);
    if (IsEmpty(area))
    {
        return;
    }
    const Rect local{.X = area.X - rectangle.X, .Y = area.Y - rectangle.Y, .Width = area.Width, .Height = area.Height};
    if (!IsUsable(m_buffer) || m_buffer.Format != target.Format)
    {
        FillRect(target, local, Session::Desktop);
        return;
    }
    const Foundation::Int64 left = (static_cast<Foundation::Int64>(m_bounds.X) - output.Origin.X) * output.Scale;
    const Foundation::Int64 top = (static_cast<Foundation::Int64>(m_bounds.Y) - output.Origin.Y) * output.Scale;
    if (output.Scale == 1U)
    {
        static_cast<void>(CopyRect(target, m_buffer,
                                   {.X = static_cast<Foundation::Int32>(area.X - left),
                                    .Y = static_cast<Foundation::Int32>(area.Y - top),
                                    .Width = area.Width,
                                    .Height = area.Height},
                                   {.X = local.X, .Y = local.Y}));
        return;
    }
    for (Foundation::UInt32 y{0U}; y < area.Height; ++y)
    {
        const Foundation::UInt32 sourceY = Divide(static_cast<Foundation::UInt32>(area.Y - top) + y, output.Scale);
        for (Foundation::UInt32 x{0U}; x < area.Width; ++x)
        {
            const Foundation::UInt32 sourceX = Divide(static_cast<Foundation::UInt32>(area.X - left) + x, output.Scale);
            target.Pixels[(static_cast<Foundation::Size>(local.Y) + y) * target.Stride +
                          static_cast<Foundation::UInt32>(local.X) + x] =
                m_buffer.Pixels[static_cast<Foundation::Size>(sourceY) * m_buffer.Stride + sourceX];
        }
    }
}
}
