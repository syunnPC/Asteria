// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_IDENTITY_TRUSTED_PLANE_HPP_INCLUDED
#define ASTERIA_USER_IDENTITY_TRUSTED_PLANE_HPP_INCLUDED
#include "User/Graphics/Composition/Composition.hpp"
#include "User/Identity/Budget.hpp"
namespace Asteria::User::Identity
{
// Opaque seat-sized trusted plane above every ordinary plane, including the pointer. Only public,
// fixed-size pixels.
class TrustedPlane final : public Graphics::Composition::IPlane
{
public:
    void Configure(Graphics::Rect bounds) noexcept
    {
        m_bounds = bounds;
    }
    void SetBuffer(Graphics::PixelSurface<const UInt32> pixels) noexcept
    {
        m_pixels = pixels;
    }
    void Show(bool visible) noexcept
    {
        m_visible = visible;
    }
    [[nodiscard]] bool Visible() const noexcept
    {
        return m_visible;
    }
    [[nodiscard]] bool Covers(Graphics::Rect, const Graphics::Composition::Output&) const noexcept override;
    [[nodiscard]] bool Intersects(Graphics::Rect, const Graphics::Composition::Output&) const noexcept override;
    void Render(const Graphics::MemorySurface&, Graphics::Rect,
                const Graphics::Composition::Output&) const noexcept override;

private:
    Graphics::Rect m_bounds{};
    Graphics::PixelSurface<const UInt32> m_pixels{};
    bool m_visible{false};
};
} // Namespace Asteria::User::Identity
#endif
