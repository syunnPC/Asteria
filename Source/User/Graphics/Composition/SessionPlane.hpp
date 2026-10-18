// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_GRAPHICS_COMPOSITION_SESSION_PLANE_HPP_INCLUDED
#define ASTERIA_USER_GRAPHICS_COMPOSITION_SESSION_PLANE_HPP_INCLUDED

#include "User/Graphics/Composition/Composition.hpp"

namespace Asteria::User::Graphics::Composition
{
/// One borrowed read-only output, under trusted planes. An absent buffer paints Desktop inside
/// Bounds; ownership stays with the broker.
class SessionPlane final : public IPlane
{
public:
    void Configure(Rect bounds) noexcept
    {
        m_bounds = bounds;
    }
    void SetBuffer(PixelSurface<const Foundation::UInt32> buffer) noexcept
    {
        m_buffer = IsUsable(buffer) && buffer.Width >= m_bounds.Width && buffer.Height >= m_bounds.Height
                       ? buffer
                       : PixelSurface<const Foundation::UInt32>{};
    }
    void Show(bool visible) noexcept
    {
        m_visible = visible;
    }
    [[nodiscard]] Rect Bounds() const noexcept
    {
        return m_bounds;
    }
    [[nodiscard]] bool Visible() const noexcept
    {
        return m_visible;
    }
    [[nodiscard]] bool Covers(Rect rectangle, const Output& output) const noexcept override;
    [[nodiscard]] bool Intersects(Rect rectangle, const Output& output) const noexcept override;
    [[nodiscard]] PixelSurface<const Foundation::UInt32> Source(Rect rectangle,
                                                                const Output& output) const noexcept override;
    void Render(const MemorySurface& target, Rect rectangle, const Output& output) const noexcept override;

private:
    [[nodiscard]] Rect PhysicalBounds(const Output& output) const noexcept;
    Rect m_bounds{};
    PixelSurface<const Foundation::UInt32> m_buffer{};
    bool m_visible{false};
};
} // Namespace Asteria::User::Graphics::Composition
#endif
