// SPDX-License-Identifier: Apache-2.0

#include "User/Imaging/Internal.hpp"
namespace Asteria::User::Imaging
{
namespace
{
using Detail::Dib;
using Detail::Dimensions;
using Detail::Input;
using Detail::Le16;
using Detail::Le32;
using Detail::Range;
struct Bitmap
{
    UInt32 Header{};
    UInt32 Width{};
    UInt32 Height{};
    UInt32 Bpp{};
    UInt32 Compression{};
    UInt32 Colors{};
    UInt32 ColorBytes{};
    UInt32 ImageBytes{};
    UInt32 Masks[4]{};
    Size Palette{};
    Size Pixels{};
    Size Row{};
    Size MaskRow{};
    bool Top{};
    bool Icon{};
};
[[nodiscard]] bool MaskValid(UInt32 mask, UInt32 bpp) noexcept
{
    if (mask == 0U)
    {
        return false;
    }
    if (bpp < 32U && (mask >> bpp) != 0U)
    {
        return false;
    }
    while ((mask & 1U) == 0U)
    {
        mask >>= 1U;
    }
    return (mask & (mask + 1U)) == 0U;
}
[[nodiscard]] Status Header(Bytes input, Size offset, bool icon, Buffer output, Bitmap& b) noexcept
{
    if (input.Count < 4U)
    {
        return Status::Truncated;
    }
    b.Header = Le32(input.Data);
    b.Icon = icon;
    if (b.Header != 12U && b.Header != 40U && b.Header != 108U && b.Header != 124U)
    {
        return Status::Unsupported;
    }
    if (!Range(0U, b.Header, input.Count))
    {
        return Status::Truncated;
    }
    if (icon && b.Header != 40U)
    {
        return Status::Unsupported;
    }
    b.ColorBytes = b.Header == 12U ? 3U : 4U;
    if (b.Header == 12U)
    {
        b.Width = Le16(input.Data + 4U);
        b.Height = Le16(input.Data + 6U);
        b.Bpp = Le16(input.Data + 10U);
        if (Le16(input.Data + 8U) != 1U)
        {
            return Status::Header;
        }
        if (b.Bpp != 1U && b.Bpp != 4U && b.Bpp != 8U && b.Bpp != 24U)
        {
            return Status::Unsupported;
        }
    }
    else
    {
        b.Width = Le32(input.Data + 4U);
        const UInt32 height{Le32(input.Data + 8U)};
        b.Top = (height & 0x80000000U) != 0U;
        b.Height = b.Top ? (~height) + 1U : height;
        b.Bpp = Le16(input.Data + 14U);
        b.Compression = Le32(input.Data + 16U);
        b.ImageBytes = Le32(input.Data + 20U);
        b.Colors = Le32(input.Data + 32U);
        if ((b.Width & 0x80000000U) != 0U || Le16(input.Data + 12U) != 1U)
        {
            return Status::Header;
        }
        if (icon)
        {
            if (b.Top || b.Height % 2U != 0U)
            {
                return Status::Header;
            }
            // Icons in Win32, The ICO File: other INFO fields must be zero.
            if (b.Compression != 0U || Le32(input.Data + 24U) != 0U || Le32(input.Data + 28U) != 0U || b.Colors != 0U ||
                Le32(input.Data + 36U) != 0U)
            {
                return Status::Header;
            }
            b.Height /= 2U;
        }
        if (b.Header == 124U)
        {
            if (Le32(input.Data + 120U) != 0U)
            {
                return Status::Header;
            }
            // Profiles are ignored but any declared range must be bounded.
            const UInt32 profile{Le32(input.Data + 112U)};
            const UInt32 bytes{Le32(input.Data + 116U)};
            if (bytes != 0U && (profile < b.Header || !Range(profile, bytes, input.Count)))
            {
                return Status::Header;
            }
        }
    }
    const Status status{Dimensions(b.Width, b.Height, output)};
    if (status != Status::Ok)
    {
        return status;
    }
    if (b.Bpp != 1U && b.Bpp != 4U && b.Bpp != 8U && b.Bpp != 16U && b.Bpp != 24U && b.Bpp != 32U)
    {
        return Status::Unsupported;
    }
    if (b.Compression > 3U)
    {
        return Status::Unsupported;
    }
    if ((b.Compression == 1U && b.Bpp != 8U) || (b.Compression == 2U && b.Bpp != 4U) ||
        (b.Compression == 3U && b.Bpp != 16U && b.Bpp != 32U) ||
        (b.Top && (b.Compression == 1U || b.Compression == 2U)))
    {
        return Status::Header;
    }
    b.Palette = b.Header;
    // Microsoft BITMAPINFOHEADER/V4HEADER Members: RGB defaults vs bitfields.
    b.Masks[0] = b.Bpp == 16U ? 0x7C00U : 0xFF0000U;
    b.Masks[1] = b.Bpp == 16U ? 0x3E0U : 0xFF00U;
    b.Masks[2] = b.Bpp == 16U ? 0x1FU : 0xFFU;
    if (b.Compression == 3U)
    {
        const Size masks{b.Header == 40U ? b.Header : 40U};
        if (!Range(masks, 12U, input.Count))
        {
            return Status::Truncated;
        }
        for (UInt32 c{0U}; c < 3U; ++c)
        {
            b.Masks[c] = Le32(input.Data + masks + c * 4U);
        }
        if (b.Header == 40U)
        {
            b.Palette += 12U;
        }
        else
        {
            b.Masks[3] = Le32(input.Data + 52U);
        }
        UInt32 used{0U};
        for (UInt32 c{0U}; c < 4U; ++c)
        {
            if (c == 3U && b.Masks[c] == 0U)
            {
                continue;
            }
            if (!MaskValid(b.Masks[c], b.Bpp) || (used & b.Masks[c]) != 0U)
            {
                return Status::Mask;
            }
            used |= b.Masks[c];
        }
    }
    if (b.Bpp <= 8U)
    {
        if (b.Colors == 0U)
        {
            b.Colors = 1U << b.Bpp;
        }
        if (b.Colors > (1U << b.Bpp))
        {
            return Status::Palette;
        }
    }
    if (b.Colors > 256U)
    {
        return Status::Limit;
    }
    if (!Range(b.Palette, static_cast<Size>(b.Colors) * b.ColorBytes, input.Count))
    {
        return Status::Truncated;
    }
    if (b.ColorBytes == 4U)
    {
        for (UInt32 i{0U}; i < b.Colors; ++i)
        {
            if (input.Data[b.Palette + i * 4U + 3U] != 0U)
            {
                return Status::Palette;
            }
        }
    }
    const Size minimum{b.Palette + static_cast<Size>(b.Colors) * b.ColorBytes};
    b.Pixels = offset == 0U ? minimum : offset;
    if (b.Pixels < minimum || b.Pixels > input.Count)
    {
        return Status::Header;
    }
    b.Row = ((static_cast<Size>(b.Width) * b.Bpp + 31U) / 32U) * 4U;
    b.MaskRow = ((b.Width + 31U) / 32U) * 4U;
    if (b.Compression == 1U || b.Compression == 2U)
    {
        if (b.ImageBytes == 0U)
        {
            return Status::Header;
        }
        if (!Range(b.Pixels, b.ImageBytes, input.Count))
        {
            return Status::Truncated;
        }
    }
    else
    {
        const Size bytes{b.Row * b.Height + (icon ? b.MaskRow * b.Height : 0U)};
        if (!Range(b.Pixels, bytes, input.Count))
        {
            return Status::Truncated;
        }
        if (b.ImageBytes != 0U && (!Range(b.Pixels, b.ImageBytes, input.Count) || b.ImageBytes < b.Row * b.Height))
        {
            return Status::SizeMismatch;
        }
    }
    if (b.Header == 124U && Le32(input.Data + 116U) != 0U)
    {
        const Size bitmapBytes{b.Compression == 1U || b.Compression == 2U ? b.ImageBytes : b.Row * b.Height};
        // BITMAPV5HEADER Remarks: a packed DIB/file profile follows the bits.
        if (Le32(input.Data + 112U) < b.Pixels + bitmapBytes)
        {
            return Status::Header;
        }
    }
    return Status::Ok;
}
[[nodiscard]] UInt8 Channel(UInt32 pixel, UInt32 mask) noexcept
{
    if (mask == 0U)
    {
        return 255U;
    }
    while ((mask & 1U) == 0U)
    {
        pixel >>= 1U;
        mask >>= 1U;
    }
    const UInt64 scaled{static_cast<UInt64>(pixel & mask) * 255U + mask / 2U};
    return static_cast<UInt8>(scaled / mask);
}
[[nodiscard]] Status PalettePixel(Bytes input, const Bitmap& b, UInt32 index, UInt8* out) noexcept
{
    if (index >= b.Colors)
    {
        return Status::Palette;
    }
    const UInt8* color{input.Data + b.Palette + index * b.ColorBytes};
    out[0] = color[2];
    out[1] = color[1];
    out[2] = color[0];
    out[3] = 255U;
    return Status::Ok;
}
[[nodiscard]] Status Rows(Bytes input, const Bitmap& b, Buffer output) noexcept
{
    for (UInt32 y{0U}; y < b.Height; ++y)
    {
        const UInt32 sourceY{b.Top ? y : b.Height - 1U - y};
        const UInt8* row{input.Data + b.Pixels + sourceY * b.Row};
        if (b.Header == 12U)
        {
            // BITMAPCOREINFO Remarks: zero padding to a LONG boundary.
            const Size used{(static_cast<Size>(b.Width) * b.Bpp + 7U) / 8U};
            for (Size i{used}; i < b.Row; ++i)
            {
                if (row[i] != 0U)
                {
                    return Status::Header;
                }
            }
        }
        const UInt8* andRow{b.Icon ? input.Data + b.Pixels + b.Row * b.Height + sourceY * b.MaskRow : nullptr};
        for (UInt32 x{0U}; x < b.Width; ++x)
        {
            UInt8* out{output.Data + (static_cast<Size>(y) * b.Width + x) * 4U};
            if (b.Bpp <= 8U)
            {
                const UInt32 bit{x * b.Bpp};
                const UInt32 index{(static_cast<UInt32>(row[bit / 8U]) >> (8U - b.Bpp - bit % 8U)) &
                                   ((1U << b.Bpp) - 1U)};
                const Status status{PalettePixel(input, b, index, out)};
                if (status != Status::Ok)
                {
                    return status;
                }
            }
            else if (b.Bpp == 24U)
            {
                out[0] = row[x * 3U + 2U];
                out[1] = row[x * 3U + 1U];
                out[2] = row[x * 3U];
                out[3] = 255U;
            }
            else
            {
                const UInt32 pixel{b.Bpp == 16U ? Le16(row + x * 2U) : Le32(row + x * 4U)};
                for (UInt32 c{0U}; c < 4U; ++c)
                {
                    out[c] = Channel(pixel, b.Masks[c]);
                }
            }
            if (andRow != nullptr && (andRow[x / 8U] & (128U >> (x % 8U))) != 0U)
            {
                // AND keeps destination, XOR nonzero changes it. RGBA cannot invert.
                if (out[0] != 0U || out[1] != 0U || out[2] != 0U)
                {
                    return Status::Unsupported;
                }
                out[3] = 0U;
            }
        }
    }
    return Status::Ok;
}
[[nodiscard]] Status Rle(Bytes input, const Bitmap& b, Buffer output) noexcept
{
    // Bitmap Compression: initialize skipped pixels to palette zero, not garbage.
    for (UInt32 i{0U}; i < b.Width * b.Height; ++i)
    {
        const Status s{PalettePixel(input, b, 0U, output.Data + static_cast<Size>(i) * 4U)};
        if (s != Status::Ok)
        {
            return s;
        }
    }
    const UInt8* stream{input.Data + b.Pixels};
    Size pos{0U};
    UInt32 x{0U};
    UInt32 y{0U};
    while (pos < b.ImageBytes)
    {
        if (!Range(pos, 2U, b.ImageBytes))
        {
            return Status::Truncated;
        }
        const UInt32 count{stream[pos++]};
        const UInt32 value{stream[pos++]};
        if (count == 0U && value < 3U)
        {
            if (value == 1U)
            {
                return pos == b.ImageBytes ? Status::Ok : Status::SizeMismatch;
            }
            if (value == 0U)
            {
                if (y >= b.Height)
                {
                    return Status::Header;
                }
                x = 0U;
                ++y;
            }
            else
            {
                if (!Range(pos, 2U, b.ImageBytes))
                {
                    return Status::Truncated;
                }
                const UInt32 dx{stream[pos++]};
                const UInt32 dy{stream[pos++]};
                if (y >= b.Height || dx > b.Width - x || dy >= b.Height - y)
                {
                    return Status::Header;
                }
                x += dx;
                y += dy;
            }
            continue;
        }
        const UInt32 run{count == 0U ? value : count};
        if (y >= b.Height || run > b.Width - x)
        {
            return Status::Header;
        }
        const UInt32 bytes{b.Bpp == 8U ? run : (run + 1U) / 2U};
        const UInt32 padded{(bytes + 1U) & ~1U};
        if (count == 0U && !Range(pos, padded, b.ImageBytes))
        {
            return Status::Truncated;
        }
        for (UInt32 i{0U}; i < run; ++i)
        {
            const UInt32 packed{count == 0U ? stream[pos + (b.Bpp == 8U ? i : i / 2U)] : value};
            const UInt32 index{b.Bpp == 8U ? packed : ((packed >> (i % 2U == 0U ? 4U : 0U)) & 15U)};
            const Size pixel{(static_cast<Size>(b.Height - 1U - y) * b.Width + x + i) * 4U};
            const Status status{PalettePixel(input, b, index, output.Data + pixel)};
            if (status != Status::Ok)
            {
                return status;
            }
        }
        if (count == 0U)
        {
            if (b.Bpp == 8U && bytes != padded && stream[pos + bytes] != 0U)
            {
                return Status::Header;
            }
            pos += padded;
        }
        x += run;
    }
    return Status::Truncated;
}
} // namespace
Status Detail::Dib(Bytes input, Size pixelOffset, bool icon, Buffer output, Image& image) noexcept
{
    image = {};
    if (!Input(input))
    {
        return input.Count > MaximumInput ? Status::Limit : Status::InvalidBuffer;
    }
    Bitmap b{};
    Status status{Header(input, pixelOffset, icon, output, b)};
    if (status == Status::Ok)
    {
        status = b.Compression == 1U || b.Compression == 2U ? Rle(input, b, output) : Rows(input, b, output);
    }
    if (status == Status::Ok)
    {
        image = {output.Data, static_cast<Size>(b.Width) * b.Height * 4U, b.Width, b.Height};
    }
    return status;
}
Status DecodeDib(Bytes input, Buffer output, Image& image) noexcept
{
    return Detail::Dib(input, 0U, false, output, image);
}
Status DecodeBmp(Bytes input, Buffer output, Image& image) noexcept
{
    image = {};
    if (!Detail::Input(input))
    {
        return input.Count > MaximumInput ? Status::Limit : Status::InvalidBuffer;
    }
    if (input.Count < 14U)
    {
        return Status::Truncated;
    }
    if (input.Data[0] != 'B' || input.Data[1] != 'M')
    {
        return Status::Signature;
    }
    if (Detail::Le32(input.Data + 2U) != input.Count)
    {
        return Status::SizeMismatch;
    }
    if (Detail::Le16(input.Data + 6U) != 0U || Detail::Le16(input.Data + 8U) != 0U)
    {
        return Status::Header;
    }
    const UInt32 offset{Detail::Le32(input.Data + 10U)};
    if (offset <= 14U)
    {
        return Status::Header;
    }
    return Detail::Dib({input.Data + 14U, input.Count - 14U}, offset - 14U, false, output, image);
}
} // namespace Asteria::User::Imaging
