// SPDX-License-Identifier: Apache-2.0

#include "User/Font/Draw.hpp"
namespace Asteria::User::Font
{
// Compile both Graphics surface variants on each target, including stack gates.
template Error DrawMask<UInt32>(const Graphics::PixelSurface<UInt32>&, const Mask&, Graphics::Point, Graphics::Color,
                                UInt8) noexcept;
template Error DrawMask<volatile UInt32>(const Graphics::PixelSurface<volatile UInt32>&, const Mask&, Graphics::Point,
                                         Graphics::Color, UInt8) noexcept;
}
