// SPDX-License-Identifier: Apache-2.0

#include "User/Imaging/Internal.hpp"
namespace Asteria::User::Imaging
{
namespace
{
using Detail::Be16;
using Detail::Be32;
using Detail::Dimensions;
using Detail::Input;
using Detail::Range;
struct Png
{
    UInt32 Width{};
    UInt32 Height{};
    UInt32 Depth{};
    UInt32 Type{};
    UInt32 Channels{};
    UInt32 Interlace{};
    UInt32 PaletteCount{};
    UInt32 Segments{};
    UInt16 Transparent[3]{};
    bool Transparency{};
};
constexpr UInt8 Signature[]{137U, 80U, 78U, 71U, 13U, 10U, 26U, 10U};
// Derived from PNG Third Edition section 8's eight-square pass pattern.
constexpr UInt8 StartX[]{0, 4, 0, 2, 0, 1, 0};
constexpr UInt8 StartY[]{0, 0, 4, 0, 2, 0, 1};
constexpr UInt8 StepX[]{8, 8, 4, 4, 2, 2, 1};
constexpr UInt8 StepY[]{8, 8, 8, 4, 4, 2, 2};
[[nodiscard]] bool Depth(UInt32 type, UInt32 depth) noexcept
{
    if (type == 0U)
    {
        return depth == 1U || depth == 2U || depth == 4U || depth == 8U || depth == 16U;
    }
    if (type == 3U)
    {
        return depth == 1U || depth == 2U || depth == 4U || depth == 8U;
    }
    return (type == 2U || type == 4U || type == 6U) && (depth == 8U || depth == 16U);
}
[[nodiscard]] bool Letter(UInt8 byte) noexcept
{
    return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
}
[[nodiscard]] Status Chunks(Bytes input, Buffer output, Workspace& work, Png& png) noexcept
{
    if (input.Count < sizeof(Signature))
    {
        return Status::Truncated;
    }
    for (UInt32 i{0U}; i < sizeof(Signature); ++i)
    {
        if (input.Data[i] != Signature[i])
        {
            return Status::Signature;
        }
    }
    Size pos{sizeof(Signature)};
    UInt32 count{0U};
    bool header{false};
    bool palette{false};
    bool trans{false};
    bool data{false};
    bool dataEnded{false};
    for (; pos < input.Count;)
    {
        if (++count > MaximumChunks)
        {
            return Status::Limit;
        }
        if (!Range(pos, 12U, input.Count))
        {
            return Status::Truncated;
        }
        const UInt32 size{Be32(input.Data + pos)};
        if (size > 0x7FFFFFFFU)
        {
            return Status::Header;
        }
        if (!Range(pos + 8U, static_cast<Size>(size) + 4U, input.Count))
        {
            return Status::Truncated;
        }
        const UInt8* name{input.Data + pos + 4U};
        const UInt8* payload{name + 4U};
        for (UInt32 i{0U}; i < 4U; ++i)
        {
            if (!Letter(name[i]))
            {
                return Status::Header;
            }
        }
        if ((name[2] & 32U) != 0U)
        {
            return Status::Header;
        }
        if (Crc32({name, static_cast<Size>(size) + 4U}) != Be32(payload + size))
        {
            return Status::Crc;
        }
        const UInt32 tag{Be32(name)};
        if (!header && tag != 0x49484452U)
        {
            return Status::Order;
        }
        if (data && tag != 0x49444154U)
        {
            dataEnded = true;
        }
        switch (tag)
        {
        case 0x49484452U: // IHDR, PNG 11.2.2
        {
            if (header || count != 1U)
            {
                return Status::Order;
            }
            if (size != 13U)
            {
                return Status::Header;
            }
            png.Width = Be32(payload);
            png.Height = Be32(payload + 4U);
            png.Depth = payload[8];
            png.Type = payload[9];
            png.Interlace = payload[12];
            if (!Depth(png.Type, png.Depth) || payload[10] != 0U || payload[11] != 0U || png.Interlace > 1U)
            {
                return Status::Header;
            }
            const Status status{Dimensions(png.Width, png.Height, output)};
            if (status != Status::Ok)
            {
                return status;
            }
            png.Channels = png.Type == 2U ? 3U : (png.Type == 4U ? 2U : (png.Type == 6U ? 4U : 1U));
            header = true;
            break;
        }
        case 0x504C5445U: // PLTE, 11.2.3
            if (palette || trans || data)
            {
                return Status::Order;
            }
            if (png.Type == 0U || png.Type == 4U || size == 0U || size % 3U != 0U || size > 768U)
            {
                return Status::Palette;
            }
            png.PaletteCount = size / 3U;
            if (png.Type == 3U && png.PaletteCount > (1U << png.Depth))
            {
                return Status::Palette;
            }
            for (UInt32 i{0U}; i < png.PaletteCount; ++i)
            {
                for (UInt32 c{0U}; c < 3U; ++c)
                {
                    work.Palette[i][c] = payload[i * 3U + c];
                }
                work.Palette[i][3] = 255U;
            }
            palette = true;
            break;
        case 0x74524E53U: // tRNS, 11.3.1.1
            if (trans || data)
            {
                return Status::Order;
            }
            if (png.Type == 3U)
            {
                if (!palette)
                {
                    return Status::Order;
                }
                if (size == 0U || size > png.PaletteCount)
                {
                    return Status::Palette;
                }
                for (UInt32 i{0U}; i < size; ++i)
                {
                    work.Palette[i][3] = payload[i];
                }
            }
            else
            {
                if ((png.Type != 0U && png.Type != 2U) || size != png.Channels * 2U)
                {
                    return Status::Header;
                }
                for (UInt32 i{0U}; i < png.Channels; ++i)
                {
                    const UInt32 v{Be16(payload + i * 2U)};
                    if (v >= (1U << png.Depth))
                    {
                        return Status::Header;
                    }
                    png.Transparent[i] = static_cast<UInt16>(v);
                }
                png.Transparency = true;
            }
            trans = true;
            break;
        case 0x49444154U: // IDAT, 11.2.4
            if (dataEnded || (png.Type == 3U && !palette))
            {
                return Status::Order;
            }
            work.Segments[png.Segments++] = {payload, size};
            data = true;
            break;
        case 0x49454E44U: // IEND, 11.2.5
            if (!data)
            {
                return Status::Order;
            }
            if (size != 0U)
            {
                return Status::Header;
            }
            return pos + 12U == input.Count ? Status::Ok : Status::SizeMismatch;
        default:
            if ((name[0] & 32U) == 0U)
            {
                return Status::Unsupported;
            }
            break; // ancillary data already CRC checked, including APNG metadata
        }
        pos += static_cast<Size>(size) + 12U;
    }
    return Status::Truncated;
}
[[nodiscard]] UInt8 Paeth(UInt32 left, UInt32 up, UInt32 corner) noexcept
{
    // Section 9.4: choose neighbour nearest left+up-corner; ties L, U, C.
    const Int32 p{static_cast<Int32>(left + up) - static_cast<Int32>(corner)};
    const Int32 dl{p - static_cast<Int32>(left)};
    const Int32 du{p - static_cast<Int32>(up)};
    const Int32 dc{p - static_cast<Int32>(corner)};
    const Int32 al{dl < 0 ? -dl : dl};
    const Int32 au{du < 0 ? -du : du};
    const Int32 ac{dc < 0 ? -dc : dc};
    return static_cast<UInt8>(al <= au && al <= ac ? left : (au <= ac ? up : corner));
}
[[nodiscard]] Status Row(Workspace& work, UInt32 index, UInt32 bytes, UInt32 bpp) noexcept
{
    UInt8 filter{};
    Status status{work.Inflate.Next(filter)};
    if (status != Status::Ok)
    {
        return status == Status::End ? Status::SizeMismatch : status;
    }
    if (filter > 4U)
    {
        return Status::Filter;
    }
    UInt8* row{work.Rows[index]};
    const UInt8* prior{work.Rows[1U - index]};
    for (UInt32 i{0U}; i < bytes; ++i)
    {
        UInt8 value{};
        status = work.Inflate.Next(value);
        if (status != Status::Ok)
        {
            return status == Status::End ? Status::SizeMismatch : status;
        }
        const UInt32 left{i < bpp ? 0U : row[i - bpp]};
        const UInt32 up{prior[i]};
        const UInt32 corner{i < bpp ? 0U : prior[i - bpp]};
        UInt32 prediction{0U};
        if (filter == 1U)
        {
            prediction = left;
        }
        if (filter == 2U)
        {
            prediction = up;
        }
        if (filter == 3U)
        {
            prediction = (left + up) / 2U;
        }
        if (filter == 4U)
        {
            prediction = Paeth(left, up, corner);
        }
        row[i] = static_cast<UInt8>(value + prediction);
    }
    return Status::Ok;
}
[[nodiscard]] UInt32 Sample(const UInt8* row, UInt32 sample, UInt32 depth) noexcept
{
    if (depth == 16U)
    {
        return Be16(row + sample * 2U);
    }
    if (depth == 8U)
    {
        return row[sample];
    }
    const UInt32 bit{sample * depth};
    return (static_cast<UInt32>(row[bit / 8U]) >> (8U - depth - bit % 8U)) & ((1U << depth) - 1U);
}
[[nodiscard]] UInt8 Scale(UInt32 sample, UInt32 depth) noexcept
{
    const UInt32 maximum{(1U << depth) - 1U};
    return static_cast<UInt8>((sample * 255U + maximum / 2U) / maximum);
}
[[nodiscard]] Status Pixel(const Png& png, const Workspace& work, const UInt8* row, UInt32 x, UInt8* out) noexcept
{
    UInt32 values[4]{0U, 0U, 0U, 0U};
    for (UInt32 i{0U}; i < png.Channels; ++i)
    {
        values[i] = Sample(row, x * png.Channels + i, png.Depth);
    }
    if (png.Type == 3U)
    {
        if (values[0] >= png.PaletteCount)
        {
            return Status::Palette;
        }
        for (UInt32 c{0U}; c < 4U; ++c)
        {
            out[c] = work.Palette[values[0]][c];
        }
        return Status::Ok;
    }
    const bool grey{png.Type == 0U || png.Type == 4U};
    for (UInt32 c{0U}; c < 3U; ++c)
    {
        out[c] = Scale(values[grey ? 0U : c], png.Depth);
    }
    out[3] = png.Type == 4U || png.Type == 6U ? Scale(values[png.Channels - 1U], png.Depth) : UInt8{255U};
    if (png.Transparency)
    {
        bool match{true};
        for (UInt32 c{0U}; c < png.Channels; ++c)
        {
            if (values[c] != png.Transparent[c])
            {
                match = false;
            }
        }
        if (match)
        {
            out[3] = 0U;
        }
    }
    return Status::Ok;
}
[[nodiscard]] Status Pixels(const Png& png, Buffer output, Workspace& work) noexcept
{
    Status status{work.Inflate.Begin(work.Segments, png.Segments)};
    if (status != Status::Ok)
    {
        return status;
    }
    const UInt32 bits{png.Depth * png.Channels};
    const UInt32 bpp{(bits + 7U) / 8U};
    for (UInt32 pass{0U}; pass < (png.Interlace == 0U ? 1U : 7U); ++pass)
    {
        const UInt32 sx{png.Interlace == 0U ? 0U : StartX[pass]};
        const UInt32 sy{png.Interlace == 0U ? 0U : StartY[pass]};
        const UInt32 dx{png.Interlace == 0U ? 1U : StepX[pass]};
        const UInt32 dy{png.Interlace == 0U ? 1U : StepY[pass]};
        if (sx >= png.Width || sy >= png.Height)
        {
            continue;
        }
        const UInt32 width{(png.Width - sx + dx - 1U) / dx};
        const UInt32 height{(png.Height - sy + dy - 1U) / dy};
        const UInt32 bytes{(width * bits + 7U) / 8U};
        for (UInt32 i{0U}; i < bytes; ++i)
        {
            work.Rows[1][i] = 0U;
        }
        for (UInt32 y{0U}; y < height; ++y)
        {
            const UInt32 index{y % 2U};
            status = Row(work, index, bytes, bpp);
            if (status != Status::Ok)
            {
                return status;
            }
            for (UInt32 x{0U}; x < width; ++x)
            {
                const Size offset{(static_cast<Size>(sy + y * dy) * png.Width + sx + x * dx) * 4U};
                status = Pixel(png, work, work.Rows[index], x, output.Data + offset);
                if (status != Status::Ok)
                {
                    return status;
                }
            }
        }
    }
    UInt8 extra{};
    status = work.Inflate.Next(extra);
    return status == Status::End ? Status::Ok : (status == Status::Ok ? Status::SizeMismatch : status);
}
} // namespace
Status DecodePng(Bytes input, Buffer output, Workspace& work, Image& image) noexcept
{
    image = {};
    if (!Detail::Input(input))
    {
        return input.Count > MaximumInput ? Status::Limit : Status::InvalidBuffer;
    }
    Png png{};
    Status status{Chunks(input, output, work, png)};
    if (status == Status::Ok)
    {
        status = Pixels(png, output, work);
    }
    if (status == Status::Ok)
    {
        image = {output.Data, static_cast<Size>(png.Width) * png.Height * 4U, png.Width, png.Height};
    }
    return status;
}
} // namespace Asteria::User::Imaging
