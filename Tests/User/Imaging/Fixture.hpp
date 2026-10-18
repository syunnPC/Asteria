// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TEST_USER_IMAGING_FIXTURE_HPP_INCLUDED
#define ASTERIA_TEST_USER_IMAGING_FIXTURE_HPP_INCLUDED
#include "Support/TestContext.hpp"
#include "User/Imaging/CursorSet.hpp"
#include <vector>
#include <algorithm>
#include <cstring>
namespace Asteria::Test::ImagingFixture
{
namespace I = User::Imaging;
using I::UInt8;
using I::UInt16;
using I::UInt32;
using I::Size;
// Byte vectors deliberately copy on assignment. GCC 15's analyzer cannot
// model the empty allocator temporary in vector move assignment; keeping
// this test helper on the ordinary initialized range-assignment path avoids
// that diagnostic without suppressing analysis of any test or decoder.
class V final : public std::vector<UInt8>
{
public:
    using std::vector<UInt8>::vector;
    V() = default;
    V(const V&) = default;
    V& operator=(const V& other)
    {
        if (this != &other)
            assign(other.begin(), other.end());
        return *this;
    }
};
inline void Le(V& v, UInt32 n, UInt32 bytes = 4U)
{
    for (UInt32 i{0U}; i < bytes; ++i)
        v.push_back(static_cast<UInt8>(n >> (i * 8U)));
}
inline void Be(V& v, UInt32 n, UInt32 bytes = 4U)
{
    for (UInt32 i{0U}; i < bytes; ++i)
        v.push_back(static_cast<UInt8>(n >> ((bytes - 1U - i) * 8U)));
}
inline void Set(V& v, Size pos, UInt32 n, UInt32 bytes = 4U)
{
    for (UInt32 i{0U}; i < bytes; ++i)
        v[pos + i] = static_cast<UInt8>(n >> (i * 8U));
}
inline UInt32 Adler(const V& v)
{
    UInt32 a{1U};
    UInt32 b{0U};
    for (auto c : v)
    {
        a = (a + c) % 65521U;
        b = (b + a) % 65521U;
    }
    return b * 65536U + a;
}
inline UInt32 Crc(const UInt8* p, Size n)
{
    // Independent bit polynomial oracle using the forward polynomial and
    // reversed octets (PNG 5.5), rather than the production reflected step.
    UInt32 crc{0xFFFFFFFFU};
    for (Size i{0U}; i < n; ++i)
        for (UInt32 b{0U}; b < 8U; ++b)
        {
            const bool carry{((crc >> 31U) ^ (static_cast<UInt32>(p[i]) >> b)) % 2U != 0U};
            crc <<= 1U;
            if (carry)
                crc ^= 0x04C11DB7U;
        }
    UInt32 reverse{0U};
    for (UInt32 b{0U}; b < 32U; ++b)
        reverse = reverse * 2U + ((crc >> b) & 1U);
    return ~reverse;
}
inline void Chunk(V& v, const char* tag, const V& payload)
{
    Be(v, static_cast<UInt32>(payload.size()));
    const Size start{v.size()};
    for (UInt32 i{0U}; i < 4U; ++i)
        v.push_back(static_cast<UInt8>(tag[i]));
    v.insert(v.end(), payload.begin(), payload.end());
    Be(v, Crc(v.data() + start, v.size() - start));
}
inline void RepairCrc(V& v, Size chunk)
{
    UInt32 n{0U};
    for (UInt32 i{0U}; i < 4U; ++i)
        n = n * 256U + v[chunk + i];
    const UInt32 crc{Crc(v.data() + chunk + 4U, n + 4U)};
    for (UInt32 i{0U}; i < 4U; ++i)
        v[chunk + 8U + n + i] = static_cast<UInt8>(crc >> ((3U - i) * 8U));
}
struct Bits
{
    V Data{};
    UInt32 Bit{};
    void Put(UInt32 value, UInt32 count)
    {
        for (UInt32 i{0U}; i < count; ++i)
        {
            if (Bit % 8U == 0U)
                Data.push_back(0U);
            Data.back() |= static_cast<UInt8>(((value >> i) & 1U) << (Bit % 8U));
            ++Bit;
        }
    }
    void Code(UInt32 symbol, const V& lengths)
    {
        const UInt32 depth{lengths[symbol]};
        UInt32 code{0U};
        for (UInt32 d{1U}; d <= depth; ++d)
        {
            UInt32 count{0U};
            for (auto l : lengths)
                if (l == d)
                    ++count;
            if (d < depth)
                code = (code + count) * 2U;
        }
        for (UInt32 s{0U}; s < symbol; ++s)
            if (lengths[s] == depth)
                ++code;
        for (UInt32 bit{0U}; bit < depth; ++bit)
            Put((code >> (depth - 1U - bit)) & 1U, 1U);
    }
};
inline V LiteralLengths(bool dynamic)
{
    V v(dynamic ? 286U : 288U, 0U);
    for (UInt32 i{0U}; i < v.size(); ++i)
        v[i] = static_cast<UInt8>(dynamic ? (i < 256U ? 9U : (i <= 270U || i == 285U ? 5U : 0U))
                                          : (i <= 143U ? 8U : (i <= 255U ? 9U : (i <= 279U ? 7U : 8U))));
    return v;
}
inline void DynamicHeader(Bits& b, const V& literal, const V& distance, bool repeats = true, bool shortZeros = false)
{
    b.Put(static_cast<UInt32>(literal.size() - 257U), 5U);
    b.Put(static_cast<UInt32>(distance.size() - 1U), 5U);
    b.Put(15U, 4U);
    const UInt8 order[]{16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    V codes(19U, 0U);
    for (UInt32 s : {0U, 1U, 5U, 8U, 9U, 16U, 17U, 18U})
        codes[s] = 3U;
    for (auto s : order)
        b.Put(codes[s], 3U);
    V joined{literal};
    joined.insert(joined.end(), distance.begin(), distance.end());
    for (Size i{0U}; i < joined.size();)
    {
        Size run{1U};
        while (i + run < joined.size() && joined[i + run] == joined[i])
            ++run;
        if (!repeats)
            run = 1U;
        const UInt32 value{joined[i]};
        if (value == 0U && run >= 11U && !shortZeros)
        {
            const Size n{std::min<Size>(run, 138U)};
            b.Code(18U, codes);
            b.Put(static_cast<UInt32>(n - 11U), 7U);
            i += n;
        }
        else if (value == 0U && run >= 3U)
        {
            const Size n{std::min<Size>(run, 10U)};
            b.Code(17U, codes);
            b.Put(static_cast<UInt32>(n - 3U), 3U);
            i += n;
        }
        else
        {
            b.Code(value, codes);
            ++i;
            --run;
            while (run >= 3U)
            {
                const Size n{std::min<Size>(run, 6U)};
                b.Code(16U, codes);
                b.Put(static_cast<UInt32>(n - 3U), 2U);
                i += n;
                run -= n;
            }
        }
    }
}
inline V Deflate(const V& raw, UInt32 mode)
{
    if (mode == 0U)
    {
        V result{};
        for (Size pos{0U}; pos < raw.size() || pos == 0U;)
        {
            const Size n{std::min<Size>(65535U, raw.size() - pos)};
            result.push_back(pos + n == raw.size() ? 1U : 0U);
            Le(result, static_cast<UInt32>(n), 2U);
            Le(result, 65535U - static_cast<UInt32>(n), 2U);
            result.insert(result.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos),
                          raw.begin() + static_cast<std::ptrdiff_t>(pos + n));
            pos += n;
            if (pos == raw.size())
                break;
        }
        return result;
    }
    Bits bits;
    bits.Put(mode == 1U ? 3U : 5U, 3U);
    const V lengths{LiteralLengths(mode == 2U)};
    if (mode == 2U)
        DynamicHeader(bits, lengths, V(32U, 5U));
    for (auto c : raw)
    {
        bits.Code(c, lengths);
    }
    bits.Code(256U, lengths);
    return bits.Data;
}
inline V Zlib(const V& raw, UInt32 mode = 0U)
{
    V v{0x78U, 0x01U};
    V d{Deflate(raw, mode)};
    v.insert(v.end(), d.begin(), d.end());
    Be(v, Adler(raw));
    return v;
}
inline I::Bytes View(const V& v)
{
    return {v.data(), v.size()};
}
inline UInt32 Channels(UInt32 type)
{
    return type == 2U ? 3U : (type == 4U ? 2U : (type == 6U ? 4U : 1U));
}
inline UInt32 Value(UInt32 x, UInt32 y, UInt32 c, UInt32 depth)
{
    return ((x * 7199U + y * 3571U + c * 11117U) ^ (x * 31U + y * 13U + c * 7U)) & ((1U << depth) - 1U);
}
inline UInt8 Scale(UInt32 v, UInt32 d)
{
    const UInt32 m{(1U << d) - 1U};
    return static_cast<UInt8>((v * 255U + m / 2U) / m);
}
inline UInt32 Predict(UInt32 a, UInt32 b, UInt32 c, UInt32 filter)
{
    if (filter == 0U)
        return 0U;
    if (filter == 1U)
        return a;
    if (filter == 2U)
        return b;
    if (filter == 3U)
        return (a + b) / 2U;
    const int target{static_cast<int>(a + b) - static_cast<int>(c)};
    UInt32 winner{a};
    int best{std::abs(target - static_cast<int>(a))};
    for (UInt32 v : {b, c})
    {
        const int error{std::abs(target - static_cast<int>(v))};
        if (error < best)
        {
            best = error;
            winner = v;
        }
    }
    return winner;
}
struct PngVector
{
    V File{};
    V Pixels{};
    V Raw{};
};
inline PngVector Png(UInt32 type, UInt32 depth, UInt32 filter, bool interlace = false, bool trans = false,
                     UInt32 width = 9U, UInt32 height = 9U, UInt32 mode = 0U, Size split = 0U)
{
    PngVector v;
    v.File = {137U, 80U, 78U, 71U, 13U, 10U, 26U, 10U};
    V hdr{};
    Be(hdr, width);
    Be(hdr, height);
    hdr.insert(hdr.end(), {static_cast<UInt8>(depth), static_cast<UInt8>(type), 0U, 0U, static_cast<UInt8>(interlace)});
    Chunk(v.File, "IHDR", hdr);
    V palette{};
    V alpha{};
    if (type == 3U)
    {
        for (UInt32 i{0U}; i < (1U << depth); ++i)
        {
            palette.insert(palette.end(),
                           {static_cast<UInt8>(i), static_cast<UInt8>(255U - i), static_cast<UInt8>(i ^ 85U)});
            alpha.push_back(static_cast<UInt8>(i * 17U));
        }
        Chunk(v.File, "PLTE", palette);
        if (trans)
            Chunk(v.File, "tRNS", alpha);
    }
    else if (trans && (type == 0U || type == 2U))
    {
        V t{};
        for (UInt32 c{0U}; c < Channels(type); ++c)
            Be(t, Value(0, 0, c, depth), 2U);
        Chunk(v.File, "tRNS", t);
    }
    v.Pixels.resize(static_cast<Size>(width) * height * 4U);
    for (UInt32 y{0U}; y < height; ++y)
        for (UInt32 x{0U}; x < width; ++x)
        {
            UInt8* p{v.Pixels.data() + (static_cast<Size>(y) * width + x) * 4U};
            if (type == 3U)
            {
                const UInt32 i{Value(x, y, 0, depth)};
                std::memcpy(p, palette.data() + i * 3U, 3U);
                p[3] = trans ? alpha[i] : UInt8{255U};
            }
            else
            {
                for (UInt32 c{0U}; c < 3U; ++c)
                    p[c] = Scale(Value(x, y, type == 0U || type == 4U ? 0U : c, depth), depth);
                p[3] = type == 4U || type == 6U ? Scale(Value(x, y, Channels(type) - 1U, depth), depth) : UInt8{255U};
                if (trans && (type == 0U || type == 2U))
                {
                    bool transparent = true;
                    for (UInt32 c{0U}; c < Channels(type); ++c)
                        if (Value(x, y, c, depth) != Value(0, 0, c, depth))
                            transparent = false;
                    if (transparent)
                        p[3] = 0U;
                }
            }
        }
    const UInt32 pattern[8][8]{{1, 6, 4, 6, 2, 6, 4, 6}, {7, 7, 7, 7, 7, 7, 7, 7}, {5, 6, 5, 6, 5, 6, 5, 6},
                               {7, 7, 7, 7, 7, 7, 7, 7}, {3, 6, 4, 6, 3, 6, 4, 6}, {7, 7, 7, 7, 7, 7, 7, 7},
                               {5, 6, 5, 6, 5, 6, 5, 6}, {7, 7, 7, 7, 7, 7, 7, 7}};
    for (UInt32 pass{1U}; pass <= (interlace ? 7U : 1U); ++pass)
    {
        V previous{};
        for (UInt32 y{0U}; y < height; ++y)
        {
            V row{};
            UInt32 bit{0U};
            for (UInt32 x{0U}; x < width; ++x)
            {
                if (interlace && pattern[y % 8U][x % 8U] != pass)
                    continue;
                for (UInt32 c{0U}; c < Channels(type); ++c)
                {
                    const UInt32 value{Value(x, y, c, depth)};
                    if (depth >= 8U)
                        Be(row, value, depth / 8U);
                    else
                    {
                        if (bit % 8U == 0U)
                            row.push_back(0U);
                        row.back() |= static_cast<UInt8>(value << (8U - depth - bit % 8U));
                        bit += depth;
                    }
                }
            }
            if (row.empty())
                continue;
            if (previous.empty())
                previous.resize(row.size(), 0U);
            v.Raw.push_back(static_cast<UInt8>(filter));
            const UInt32 bpp{(Channels(type) * depth + 7U) / 8U};
            for (Size i{0U}; i < row.size(); ++i)
                v.Raw.push_back(static_cast<UInt8>(row[i] - Predict(i < bpp ? 0U : row[i - bpp], previous[i],
                                                                    i < bpp ? 0U : previous[i - bpp], filter)));
            previous = row;
        }
    }
    const V compressed{Zlib(v.Raw, mode)};
    for (Size pos{0U}; pos < compressed.size();)
    {
        const Size n{split == 0U ? compressed.size() : std::min(split, compressed.size() - pos)};
        Chunk(v.File, "IDAT",
              V(compressed.begin() + static_cast<std::ptrdiff_t>(pos),
                compressed.begin() + static_cast<std::ptrdiff_t>(pos + n)));
        pos += n;
    }
    Chunk(v.File, "IEND", {});
    return v;
}
inline V Dib(UInt32 header, UInt32 bpp, bool top = false, bool icon = false, UInt32 compression = 0U)
{
    V v(header, 0U);
    Set(v, 0U, header);
    const UInt32 h{icon ? 4U : 2U};
    if (header == 12U)
    {
        Set(v, 4U, 2U, 2U);
        Set(v, 6U, h, 2U);
        Set(v, 8U, 1U, 2U);
        Set(v, 10U, bpp, 2U);
    }
    else
    {
        Set(v, 4U, 2U);
        Set(v, 8U, top ? 0U - h : h);
        Set(v, 12U, 1U, 2U);
        Set(v, 14U, bpp, 2U);
        Set(v, 16U, compression);
    }
    if (compression == 3U)
    {
        if (header == 40U)
        {
            Le(v, 0xF800U);
            Le(v, 0x7E0U);
            Le(v, 0x1FU);
        }
        else
        {
            Set(v, 40U, 0xFF0000U);
            Set(v, 44U, 0xFF00U);
            Set(v, 48U, 0xFFU);
            Set(v, 52U, 0xFF000000U);
        }
    }
    if (bpp <= 8U)
        for (UInt32 i{0U}; i < (1U << bpp); ++i)
        {
            v.insert(v.end(), {static_cast<UInt8>(i == 1U ? 0U : i), static_cast<UInt8>(i == 1U ? 0U : i),
                               static_cast<UInt8>(i == 1U ? 255U : i)});
            if (header != 12U)
                v.push_back(0U);
        }
    const Size stride{((2U * bpp + 31U) / 32U) * 4U};
    for (UInt32 y{0U}; y < 2U; ++y)
    {
        const Size start{v.size()};
        for (UInt32 x{0U}; x < 2U; ++x)
        {
            const bool red{(top ? y : 1U - y) == 0U};
            if (bpp == 1U)
            {
                if (x == 0U)
                    v.push_back(red ? 0xC0U : 0U);
            }
            else if (bpp == 4U)
            {
                if (x == 0U)
                    v.push_back(red ? 0x11U : 0U);
            }
            else if (bpp == 8U)
                v.push_back(red ? 1U : 0U);
            else if (bpp == 16U)
                Le(v, red ? (compression == 3U ? 0xF800U : 0x7C00U) : 0U, 2U);
            else if (bpp == 24U)
                v.insert(v.end(), {0U, 0U, static_cast<UInt8>(red ? 255U : 0U)});
            else
                Le(v, (red ? 0xFF0000U : 0U) | (compression == 3U ? 0x80000000U : 0U));
        }
        v.resize(start + stride, 0U);
    }
    if (icon)
        v.resize(v.size() + 8U, 0U);
    return v;
}
inline V Bmp(const V& dib)
{
    V v{'B', 'M'};
    Le(v, static_cast<UInt32>(dib.size() + 14U));
    Le(v, 0U);
    const UInt32 header{static_cast<UInt32>(dib[0])};
    UInt32 offset{header};
    const UInt32 bpp{dib[header == 12U ? 10U : 14U]};
    if (bpp <= 8U)
        offset += (1U << bpp) * (header == 12U ? 3U : 4U);
    if (header == 40U && dib[16] == 3U)
        offset += 12U;
    Le(v, offset + 14U);
    v.insert(v.end(), dib.begin(), dib.end());
    return v;
}
inline V Ico(const std::vector<V>& images, const std::vector<UInt32>& dimensions)
{
    V v{};
    Le(v, 0U, 2U);
    Le(v, 1U, 2U);
    Le(v, static_cast<UInt32>(images.size()), 2U);
    UInt32 pos{6U + 16U * static_cast<UInt32>(images.size())};
    for (Size i{0U}; i < images.size(); ++i)
    {
        v.insert(v.end(), {static_cast<UInt8>(dimensions[i]), static_cast<UInt8>(dimensions[i]), 0U, 0U});
        Le(v, 1U, 2U);
        Le(v, 32U, 2U);
        Le(v, static_cast<UInt32>(images[i].size()));
        Le(v, pos);
        pos += static_cast<UInt32>(images[i].size());
    }
    for (const auto& image : images)
    {
        v.insert(v.end(), image.begin(), image.end());
    }
    return v;
}
} // namespace Asteria::Test::ImagingFixture
#endif
