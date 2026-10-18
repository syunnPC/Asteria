// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_GRAPHICS_COMPOSITION_COMPOSITION_HPP_INCLUDED
#define ASTERIA_USER_GRAPHICS_COMPOSITION_COMPOSITION_HPP_INCLUDED

#include "User/Console/TextConsole.hpp"
#include "User/Graphics/Composition/CursorImages.hpp"

namespace Asteria::User::Graphics::Composition
{
using Graphics::Rect;

/// Physical scanout, logical desktop origin, integer physical pixels per logical pixel.
/// Dimensions must be divisible by Scale; desktop bounds must fit signed 32 bits.
struct Output
{
    Surface Scanout;
    Point Origin;
    Foundation::UInt32 Scale;
};

/// Single-threaded damage in logical desktop pixels. Configure bounds before Add.
/// Overlap and adjacency merge; overflow collapses to the union of all damage.
/// No allocation; rectangles are clipped before arithmetic. Bounds are at most
/// INT32_MAX pixels per axis and their far edges must fit signed 32 bits.
/// Invalid Configure bounds disable damage until the next valid configuration.
class DamageList final
{
public:
    static constexpr Foundation::Size Capacity{16U};
    void Configure(Rect bounds) noexcept;
    void Add(Rect rect) noexcept;
    void Clear() noexcept;
    [[nodiscard]] Foundation::Size Count() const noexcept;
    [[nodiscard]] Rect At(Foundation::Size index) const noexcept;

private:
    Rect m_bounds{};
    Rect m_rects[Capacity]{};
    Foundation::Size m_count{0U};
};

/// Clip a logical rectangle to one output using wide transform arithmetic.
[[nodiscard]] Rect ProjectBounds(Rect bounds, const Output& output) noexcept;

/// Renders physical rectangle into a tile whose (0,0) is rectangle's top left.
/// Output supplies the transform to logical desktop pixels. Implementations may
/// read ordinary memory only, never scanout; calls are serialized, non-blocking,
/// bounded by the tile size, and allocation-free. Borrowed planes outlive passes.
class IPlane
{
public:
    virtual void Render(const MemorySurface& target, Rect rectangle, const Output& output) const noexcept = 0;
    // Conservative defaults for arbitrary planes.
    [[nodiscard]] virtual bool Covers(Rect, const Output&) const noexcept
    {
        return false;
    }
    [[nodiscard]] virtual bool Intersects(Rect, const Output&) const noexcept
    {
        return true;
    }
    [[nodiscard]] virtual PixelSurface<const Foundation::UInt32> Source(Rect, const Output&) const noexcept
    {
        return {};
    }

protected:
    ~IPlane() = default;
};

/// Backend interface for plane composition.
/// Image storage remains borrowed; operations are serialized by the broker.
class ICursorBackend
{
public:
    [[nodiscard]] virtual bool SetImage(CursorImage image) noexcept = 0;
    virtual void Move(Point position) noexcept = 0;
    virtual void Show(bool visible) noexcept = 0;

protected:
    ~ICursorBackend() = default;
};

class BackgroundPlane final : public IPlane
{
public:
    explicit constexpr BackgroundPlane(Color color) noexcept : m_color{color}
    {
    }
    void Render(const MemorySurface& target, Rect rectangle, const Output& output) const noexcept override;

private:
    Color m_color;
};

class ConsolePlane final : public IPlane
{
public:
    explicit constexpr ConsolePlane(const Console::TextConsole& console) noexcept : m_console{console}
    {
    }
    void Render(const MemorySurface& target, Rect rectangle, const Output& output) const noexcept override;
    void Show(bool visible) noexcept
    {
        m_visible = visible;
    }
    [[nodiscard]] bool Visible() const noexcept
    {
        return m_visible;
    }
    [[nodiscard]] bool Covers(Rect, const Output&) const noexcept override
    {
        return m_visible;
    }
    [[nodiscard]] bool Intersects(Rect, const Output&) const noexcept override
    {
        return m_visible;
    }

private:
    bool m_visible{true};
    const Console::TextConsole& m_console;
};

/// Software backend and last plane. Callers damage Bounds before and after changes.
/// Bounds clips the image to representable signed top-left coordinates.
class CursorPlane final : public IPlane, public ICursorBackend
{
public:
    [[nodiscard]] bool SetImage(CursorImage image) noexcept override;
    void Move(Point position) noexcept override;
    void Show(bool visible) noexcept override;
    [[nodiscard]] Point Position() const noexcept;
    [[nodiscard]] Rect Bounds() const noexcept;
    [[nodiscard]] bool Intersects(Rect rectangle, const Output& output) const noexcept override;
    void Render(const MemorySurface& target, Rect rectangle, const Output& output) const noexcept override;

private:
    CursorImage m_image{Arrow};
    Point m_position{};
    bool m_visible{true};
};

/// Counts actual write-only scanout stores in one pass, across all outputs.
struct RenderStatistics
{
    Foundation::UInt64 Pixels{0U};
    Foundation::UInt64 Stores32{0U};
    Foundation::UInt64 Stores64{0U};
    Foundation::UInt64 DirectPixels{0U};
};

/// Caller-owned static storage, including the 16 KiB tile. Ordered planes and
/// outputs are borrowed. Render copies each finished tile once; Order is called
/// exactly once after a nonempty pass, never once per tile or rectangle.
/// Caller serializes mutations and passes; no allocation or framebuffer reads.
class Composer final
{
public:
    static constexpr Foundation::UInt32 TileExtent{64U};
    static constexpr Foundation::Size MaximumPlanes{8U};
    static constexpr Foundation::Size MaximumOutputs{4U};
    using Order = void (*)() noexcept;
    void Reset() noexcept;
    [[nodiscard]] bool AddPlane(const IPlane& plane) noexcept;
    [[nodiscard]] bool AddOutput(Output output) noexcept;
    [[nodiscard]] Foundation::Size OutputCount() const noexcept;
    [[nodiscard]] Output OutputAt(Foundation::Size index) const noexcept;
    RenderStatistics Render(DamageList& damage, Order order) noexcept;

private:
    RenderStatistics RenderRectangle(const Output& output, Rect rectangle) noexcept;
    const IPlane* m_planes[MaximumPlanes]{};
    Output m_outputs[MaximumOutputs]{};
    Foundation::Size m_planeCount{0U};
    Foundation::Size m_outputCount{0U};
    Foundation::UInt32 m_tile[TileExtent * TileExtent]{};
};
} // Namespace Asteria::User::Graphics::Composition
#endif // ASTERIA_USER_GRAPHICS_COMPOSITION_COMPOSITION_HPP_INCLUDED
