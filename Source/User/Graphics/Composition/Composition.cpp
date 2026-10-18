// SPDX-License-Identifier: Apache-2.0

#include "User/Graphics/Composition/Composition.hpp"

#include "User/Graphics/Blit.hpp"
#include "User/Graphics/ScanoutStorage.hpp"
#include "User/Graphics/Composition/Divide.hpp"

namespace Asteria::User::Graphics::Composition
{
namespace
{
[[nodiscard]] Rect Union(const Rect a, const Rect b) noexcept
{
    const Foundation::Int32 x = a.X < b.X ? a.X : b.X;
    const Foundation::Int32 y = a.Y < b.Y ? a.Y : b.Y;
    const Foundation::Int64 rightA = static_cast<Foundation::Int64>(a.X) + a.Width;
    const Foundation::Int64 rightB = static_cast<Foundation::Int64>(b.X) + b.Width;
    const Foundation::Int64 bottomA = static_cast<Foundation::Int64>(a.Y) + a.Height;
    const Foundation::Int64 bottomB = static_cast<Foundation::Int64>(b.Y) + b.Height;
    return Rect{.X = x,
                .Y = y,
                .Width = static_cast<Foundation::UInt32>((rightA > rightB ? rightA : rightB) - x),
                .Height = static_cast<Foundation::UInt32>((bottomA > bottomB ? bottomA : bottomB) - y)};
}
[[nodiscard]] bool Touches(const Rect a, const Rect b) noexcept
{
    return (static_cast<Foundation::Int64>(a.X) <= static_cast<Foundation::Int64>(b.X) + b.Width) &&
           (static_cast<Foundation::Int64>(b.X) <= static_cast<Foundation::Int64>(a.X) + a.Width) &&
           (static_cast<Foundation::Int64>(a.Y) <= static_cast<Foundation::Int64>(b.Y) + b.Height) &&
           (static_cast<Foundation::Int64>(b.Y) <= static_cast<Foundation::Int64>(a.Y) + a.Height);
}
[[nodiscard]] Foundation::UInt32 Blend(const Foundation::UInt32 source, const Foundation::UInt32 destination,
                                       const PixelFormat format) noexcept
{
    const Foundation::UInt32 alpha = source >> 24U;
    const Foundation::UInt32 packed = PackColor(format, Color{.Red = static_cast<Foundation::UInt8>(source >> 16U),
                                                              .Green = static_cast<Foundation::UInt8>(source >> 8U),
                                                              .Blue = static_cast<Foundation::UInt8>(source)});
    Foundation::UInt32 result{0U};
    for (Foundation::UInt32 shift{0U}; shift < 24U; shift += 8U)
    {
        const Foundation::UInt32 channel =
            (((packed >> shift) & 255U) * alpha + ((destination >> shift) & 255U) * (255U - alpha) + 127U) / 255U;
        result |= channel << shift;
    }
    return result;
}
} // Namespace

Rect ProjectBounds(const Rect bounds, const Output& output) noexcept
{
    const Foundation::Int64 left = (static_cast<Foundation::Int64>(bounds.X) - output.Origin.X) * output.Scale;
    const Foundation::Int64 top = (static_cast<Foundation::Int64>(bounds.Y) - output.Origin.Y) * output.Scale;
    const Foundation::Int64 right = left + static_cast<Foundation::Int64>(bounds.Width) * output.Scale;
    const Foundation::Int64 bottom = top + static_cast<Foundation::Int64>(bounds.Height) * output.Scale;
    const auto x = left > 0 ? left : 0;
    const auto y = top > 0 ? top : 0;
    const auto endX = right < output.Scanout.Width ? right : output.Scanout.Width;
    const auto endY = bottom < output.Scanout.Height ? bottom : output.Scanout.Height;
    if (endX <= x || endY <= y)
    {
        return {};
    }
    return {.X = static_cast<Foundation::Int32>(x),
            .Y = static_cast<Foundation::Int32>(y),
            .Width = static_cast<Foundation::UInt32>(endX - x),
            .Height = static_cast<Foundation::UInt32>(endY - y)};
}

void DamageList::Configure(const Rect bounds) noexcept
{
    const bool valid = (bounds.Width <= 0x7FFFFFFFU) && (bounds.Height <= 0x7FFFFFFFU) &&
                       (static_cast<Foundation::Int64>(bounds.X) + bounds.Width <= 0x7FFFFFFF) &&
                       (static_cast<Foundation::Int64>(bounds.Y) + bounds.Height <= 0x7FFFFFFF);
    m_bounds = valid ? bounds : Rect{};
    Clear();
}
void DamageList::Clear() noexcept
{
    m_count = 0U;
}
Foundation::Size DamageList::Count() const noexcept
{
    return m_count;
}
Rect DamageList::At(const Foundation::Size index) const noexcept
{
    return index < m_count ? m_rects[index] : Rect{};
}
void DamageList::Add(Rect rect) noexcept
{
    rect = Intersect(rect, m_bounds);
    if (IsEmpty(rect))
    {
        return;
    }
    Foundation::Size index{0U};
    while (index < m_count)
    {
        if (Touches(rect, m_rects[index]))
        {
            rect = Union(rect, m_rects[index]);
            m_rects[index] = m_rects[--m_count];
            index = 0U;
        }
        else
        {
            ++index;
        }
    }
    if (m_count == Capacity)
    {
        for (Foundation::Size merge{0U}; merge < m_count; ++merge)
        {
            rect = Union(rect, m_rects[merge]);
        }
        m_count = 0U;
    }
    m_rects[m_count++] = rect;
}
void BackgroundPlane::Render(const MemorySurface& target, const Rect, const Output&) const noexcept
{
    FillRect(target, Graphics::Bounds(target), m_color);
}
void ConsolePlane::Render(const MemorySurface& target, const Rect rectangle, const Output& output) const noexcept
{
    if (m_visible)
    {
        m_console.RenderRegion(target, rectangle, output.Origin, output.Scale);
    }
}
bool CursorPlane::SetImage(const CursorImage image) noexcept
{
    if ((image.Pixels == nullptr) || (image.Width == 0U) || (image.Height == 0U) || (image.Width > 64U) ||
        (image.Height > 64U) || (image.HotSpot.X < 0) || (image.HotSpot.Y < 0) ||
        (static_cast<Foundation::UInt32>(image.HotSpot.X) >= image.Width) ||
        (static_cast<Foundation::UInt32>(image.HotSpot.Y) >= image.Height))
    {
        return false;
    }
    m_image = image;
    return true;
}
void CursorPlane::Move(const Point position) noexcept
{
    m_position = position;
}
void CursorPlane::Show(const bool visible) noexcept
{
    m_visible = visible;
}
Point CursorPlane::Position() const noexcept
{
    return m_position;
}
Rect CursorPlane::Bounds() const noexcept
{
    const Foundation::Int64 left = static_cast<Foundation::Int64>(m_position.X) - m_image.HotSpot.X;
    const Foundation::Int64 top = static_cast<Foundation::Int64>(m_position.Y) - m_image.HotSpot.Y;
    constexpr Foundation::Int64 minimum{-0x7FFFFFFF - 1};
    const Foundation::UInt32 clippedX = left < minimum ? static_cast<Foundation::UInt32>(minimum - left) : 0U;
    const Foundation::UInt32 clippedY = top < minimum ? static_cast<Foundation::UInt32>(minimum - top) : 0U;
    return Rect{.X = static_cast<Foundation::Int32>(left < minimum ? minimum : left),
                .Y = static_cast<Foundation::Int32>(top < minimum ? minimum : top),
                .Width = m_visible ? m_image.Width - clippedX : 0U,
                .Height = m_visible ? m_image.Height - clippedY : 0U};
}
bool CursorPlane::Intersects(const Rect rectangle, const Output& output) const noexcept
{
    return !IsEmpty(Intersect(rectangle, ProjectBounds(Bounds(), output)));
}
void CursorPlane::Render(const MemorySurface& target, const Rect rectangle, const Output& output) const noexcept
{
    if (!m_visible)
    {
        return;
    }
    const Foundation::Int64 left = static_cast<Foundation::Int64>(m_position.X) - m_image.HotSpot.X;
    const Foundation::Int64 top = static_cast<Foundation::Int64>(m_position.Y) - m_image.HotSpot.Y;
    const Foundation::Int64 startX = (left - output.Origin.X) * output.Scale - rectangle.X;
    const Foundation::Int64 startY = (top - output.Origin.Y) * output.Scale - rectangle.Y;
    const Foundation::Int64 endX = startX + static_cast<Foundation::Int64>(m_image.Width) * output.Scale;
    const Foundation::Int64 endY = startY + static_cast<Foundation::Int64>(m_image.Height) * output.Scale;
    const Foundation::Int64 firstX = startX > 0 ? startX : 0;
    const Foundation::Int64 firstY = startY > 0 ? startY : 0;
    const Foundation::Int64 lastX = endX < target.Width ? endX : target.Width;
    const Foundation::Int64 lastY = endY < target.Height ? endY : target.Height;
    if ((lastX <= firstX) || (lastY <= firstY))
    {
        return;
    }
    const Foundation::UInt32 initialX = Divide(static_cast<Foundation::UInt32>(firstX - startX), output.Scale);
    const Foundation::UInt32 initialRepeat =
        output.Scale - (static_cast<Foundation::UInt32>(firstX - startX) - initialX * output.Scale);
    for (Foundation::UInt32 y{static_cast<Foundation::UInt32>(firstY)}; y < static_cast<Foundation::UInt32>(lastY); ++y)
    {
        const Foundation::UInt32 imageY = Divide(static_cast<Foundation::UInt32>(y - startY), output.Scale);
        const Foundation::UInt32* const sourceRow =
            m_image.Pixels + static_cast<Foundation::Size>(imageY) * m_image.Width;
        Foundation::UInt32* const line = target.Pixels + static_cast<Foundation::Size>(y) * target.Stride;
        Foundation::UInt32 imageX = initialX;
        Foundation::UInt32 repeat = initialRepeat;
        for (Foundation::UInt32 x{static_cast<Foundation::UInt32>(firstX)}; x < static_cast<Foundation::UInt32>(lastX);
             ++x)
        {
            const Foundation::UInt32 source = sourceRow[imageX];
            if ((source >> 24U) == 255U)
            {
                line[x] = PackColor(target.Format, Color{.Red = static_cast<Foundation::UInt8>(source >> 16U),
                                                         .Green = static_cast<Foundation::UInt8>(source >> 8U),
                                                         .Blue = static_cast<Foundation::UInt8>(source)});
            }
            else if ((source >> 24U) != 0U)
            {
                line[x] = Blend(source, line[x], target.Format);
            }
            if (--repeat == 0U)
            {
                repeat = output.Scale;
                ++imageX;
            }
        }
    }
}

void Composer::Reset() noexcept
{
    m_planeCount = 0U;
    m_outputCount = 0U;
}
bool Composer::AddPlane(const IPlane& plane) noexcept
{
    if (m_planeCount == MaximumPlanes)
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < m_planeCount; ++index)
    {
        if (m_planes[index] == &plane)
        {
            return false;
        }
    }
    m_planes[m_planeCount++] = &plane;
    return true;
}
bool Composer::AddOutput(const Output output) noexcept
{
    if ((m_outputCount == MaximumOutputs) || !IsUsable(output.Scanout) || (output.Scale == 0U) ||
        (output.Scanout.Width > 32768U) || (output.Scanout.Height > 32768U) ||
        (Divide(output.Scanout.Width, output.Scale) * output.Scale != output.Scanout.Width) ||
        (Divide(output.Scanout.Height, output.Scale) * output.Scale != output.Scanout.Height) ||
        (static_cast<Foundation::Int64>(output.Origin.X) + Divide(output.Scanout.Width, output.Scale) > 0x7FFFFFFF) ||
        (static_cast<Foundation::Int64>(output.Origin.Y) + Divide(output.Scanout.Height, output.Scale) > 0x7FFFFFFF))
    {
        return false;
    }
    m_outputs[m_outputCount++] = output;
    return true;
}
Foundation::Size Composer::OutputCount() const noexcept
{
    return m_outputCount;
}
Output Composer::OutputAt(const Foundation::Size index) const noexcept
{
    return index < m_outputCount ? m_outputs[index] : Output{};
}
RenderStatistics Composer::RenderRectangle(const Output& output, const Rect rectangle) noexcept
{
    RenderStatistics statistics{};
    for (Foundation::UInt32 y{0U}; y < rectangle.Height; y += TileExtent)
    {
        for (Foundation::UInt32 x{0U}; x < rectangle.Width; x += TileExtent)
        {
            const Foundation::UInt32 width = rectangle.Width - x;
            const Foundation::UInt32 height = rectangle.Height - y;
            const Rect piece{.X = rectangle.X + static_cast<Foundation::Int32>(x),
                             .Y = rectangle.Y + static_cast<Foundation::Int32>(y),
                             .Width = width < TileExtent ? width : TileExtent,
                             .Height = height < TileExtent ? height : TileExtent};
            const MemorySurface tile{.Pixels = m_tile,
                                     .Width = piece.Width,
                                     .Height = piece.Height,
                                     .Stride = TileExtent,
                                     .Format = output.Scanout.Format};
            // Start at the highest opaque plane. Its full coverage replaces both tile clearing and
            // every lower plane.
            Foundation::Size first{0U};
            bool opaque{false};
            for (Foundation::Size plane{0U}; plane < m_planeCount; ++plane)
            {
                if (m_planes[plane]->Covers(piece, output))
                {
                    first = plane;
                    opaque = true;
                }
            }
            bool overlay{false};
            for (Foundation::Size plane{first + 1U}; opaque && (plane < m_planeCount); ++plane)
            {
                overlay = overlay || m_planes[plane]->Intersects(piece, output);
            }
            const auto source =
                opaque && !overlay ? m_planes[first]->Source(piece, output) : PixelSurface<const Foundation::UInt32>{};
            const bool direct = IsUsable(source) && (source.Format == tile.Format) && (source.Width == piece.Width) &&
                                (source.Height == piece.Height);
            if (!direct)
            {
                if (!opaque)
                {
                    FillRect(tile, Graphics::Bounds(tile), Color{});
                }
                for (Foundation::Size plane{first}; plane < m_planeCount; ++plane)
                {
                    m_planes[plane]->Render(tile, piece, output);
                }
            }
            for (Foundation::UInt32 row{0U}; row < piece.Height; ++row)
            {
                const ScanoutStorage::Stores stores = ScanoutStorage::WriteRow(
                    output.Scanout.Pixels +
                        static_cast<Foundation::Size>(static_cast<Foundation::UInt32>(piece.Y) + row) *
                            output.Scanout.Stride +
                        static_cast<Foundation::UInt32>(piece.X),
                    direct ? source.Pixels + static_cast<Foundation::Size>(row) * source.Stride
                           : m_tile + row * TileExtent,
                    piece.Width);
                statistics.Stores32 += stores.Narrow;
                statistics.Stores64 += stores.Wide;
            }
            if (direct)
            {
                statistics.DirectPixels += static_cast<Foundation::UInt64>(piece.Width) * piece.Height;
            }
            statistics.Pixels += static_cast<Foundation::UInt64>(piece.Width) * piece.Height;
        }
    }
    return statistics;
}
RenderStatistics Composer::Render(DamageList& damage, const Order order) noexcept
{
    bool drawn{false};
    RenderStatistics statistics{};
    for (Foundation::Size index{0U}; index < m_outputCount; ++index)
    {
        const Output& output = m_outputs[index];
        const Rect bounds{.X = output.Origin.X,
                          .Y = output.Origin.Y,
                          .Width = Divide(output.Scanout.Width, output.Scale),
                          .Height = Divide(output.Scanout.Height, output.Scale)};
        for (Foundation::Size item{0U}; item < damage.Count(); ++item)
        {
            const Rect area = Intersect(damage.At(item), bounds);
            if (!IsEmpty(area))
            {
                const Rect physical{.X = (area.X - bounds.X) * static_cast<Foundation::Int32>(output.Scale),
                                    .Y = (area.Y - bounds.Y) * static_cast<Foundation::Int32>(output.Scale),
                                    .Width = area.Width * output.Scale,
                                    .Height = area.Height * output.Scale};
                const RenderStatistics piece = RenderRectangle(output, physical);
                statistics.Pixels += piece.Pixels;
                statistics.Stores32 += piece.Stores32;
                statistics.Stores64 += piece.Stores64;
                statistics.DirectPixels += piece.DirectPixels;
                drawn = true;
            }
        }
    }
    damage.Clear();
    if (drawn && (order != nullptr))
    {
        order();
    }
    return statistics;
}
} // Namespace Asteria::User::Graphics::Composition
