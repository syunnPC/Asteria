// SPDX-License-Identifier: Apache-2.0

#include "User/Identity/TrustedPlane.hpp"
#include "User/Identity/LoginRenderer.hpp"
#include "User/Graphics/Blit.hpp"
namespace Asteria::User::Identity
{
bool TrustedPlane::Intersects(Graphics::Rect r, const Graphics::Composition::Output& output) const noexcept
{
    return m_visible &&
           !Graphics::IsEmpty(Graphics::Intersect(r, Graphics::Composition::ProjectBounds(m_bounds, output)));
}
bool TrustedPlane::Covers(Graphics::Rect r, const Graphics::Composition::Output& output) const noexcept
{
    const auto area = Graphics::Intersect(r, Graphics::Composition::ProjectBounds(m_bounds, output));
    return m_visible && !Graphics::IsEmpty(r) && area.Width == r.Width && area.Height == r.Height;
}
void TrustedPlane::Render(const Graphics::MemorySurface& target, Graphics::Rect r,
                          const Graphics::Composition::Output& output) const noexcept
{
    if (!m_visible)
    {
        return;
    }
    Graphics::FillRect(target, Graphics::Bounds(target), LoginBackground);
    if (!Graphics::IsUsable(m_pixels) || output.Scale != 1U)
    {
        return;
    }
    const Foundation::Int32 left =
        m_bounds.Width >= LoginWidth ? static_cast<Foundation::Int32>((m_bounds.Width - LoginWidth) / 2U) : 0;
    const Foundation::Int32 top =
        m_bounds.Height >= LoginHeight ? static_cast<Foundation::Int32>((m_bounds.Height - LoginHeight) / 2U) : 0;
    const Graphics::Rect panel{.X = left, .Y = top, .Width = LoginWidth, .Height = LoginHeight};
    const auto area = Graphics::Intersect(r, panel);
    if (Graphics::IsEmpty(area))
    {
        return;
    }
    static_cast<void>(Graphics::CopyRect(
        target, m_pixels, {.X = area.X - left, .Y = area.Y - top, .Width = area.Width, .Height = area.Height},
        {.X = area.X - r.X, .Y = area.Y - r.Y}));
}
} // namespace Asteria::User::Identity
