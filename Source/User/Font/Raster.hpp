// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_FONT_RASTER_HPP_INCLUDED
#define ASTERIA_USER_FONT_RASTER_HPP_INCLUDED
#include "User/Font/Outline.hpp"
namespace Asteria::User::Font
{
struct Edge
{
    Point A{};
    Point B{};
};
struct Crossing
{
    Int64 X{};
    Int32 Winding{};
};
struct CurveFrame
{
    Point P[4U]{};
    UInt32 Depth{};
};
// Caller-sized edge storage lets a small UI owner retain the same rasterizer without reserving the
// general engine's maximum.
struct RasterScratch
{
    Edge* Edges{};
    Crossing* Crossings{};
    UInt32 EdgeCapacity{};
    UInt32 CrossingCapacity{};
    UInt16* Samples{};
    UInt32 SampleCapacity{};
    CurveFrame Curves[MaximumCurveDepth + 1U]{};
    UInt32 EdgeCount{};
    UInt32 CurveCount{};
    UInt32 Work{};
};
struct RasterWorkspace : RasterScratch
{
    Edge EdgeStorage[MaximumEdges]{};
    Crossing CrossingStorage[MaximumEdges]{};
    UInt16 SampleStorage[MaximumMaskAxis]{};
    RasterWorkspace(const RasterWorkspace&) = delete;
    RasterWorkspace& operator=(const RasterWorkspace&) = delete;
    constexpr RasterWorkspace() noexcept
    {
        Edges = EdgeStorage;
        Crossings = CrossingStorage;
        EdgeCapacity = MaximumEdges;
        CrossingCapacity = MaximumEdges;
        Samples = SampleStorage;
        SampleCapacity = MaximumMaskAxis;
    }
};
struct RasterOptions
{
    UInt32 PixelSize{16U * static_cast<UInt32>(One)};
    Int32 OffsetX{};
    Int32 OffsetY{}; // 16.16 fractional pixels in [-1,1]. y down.
};
struct Mask
{
    const UInt8* Pixels{};
    Size Length{};
    UInt32 Width{};
    UInt32 Height{};
    UInt32 Stride{};
    Int32 Left{};
    Int32 Top{}; // Pixel bearings from baseline; y down.
};
[[nodiscard]] Error Rasterize(const Outline& outline, UInt16 unitsPerEm, const RasterOptions& options,
                              RasterScratch& workspace, UInt8* pixels, Size capacity, Mask& mask) noexcept;
} // Namespace Asteria::User::Font
#endif
