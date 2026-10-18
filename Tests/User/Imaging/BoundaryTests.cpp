// SPDX-License-Identifier: Apache-2.0
#include "User/Imaging/Fixture.hpp"
namespace Asteria::Test
{
namespace
{
namespace F = ImagingFixture;
namespace I = User::Imaging;
using TestSupport::TestContext;
static I::Workspace Work;
F::V Wrap(F::UInt32 w, F::UInt32 h, F::UInt32 depth, F::UInt32 type, const F::V& compressed,
          const F::V& transparent = {})
{
    F::V file{137U, 80U, 78U, 71U, 13U, 10U, 26U, 10U};
    F::V hdr{};
    F::Be(hdr, w);
    F::Be(hdr, h);
    hdr.insert(hdr.end(), {static_cast<F::UInt8>(depth), static_cast<F::UInt8>(type), 0U, 0U, 0U});
    F::Chunk(file, "IHDR", hdr);
    if (!transparent.empty())
        F::Chunk(file, "tRNS", transparent);
    for (F::Size i{0U}; i < compressed.size(); i += 47U)
    {
        const F::Size end{std::min(i + 47U, compressed.size())};
        F::Chunk(file, "IDAT",
                 F::V(compressed.begin() + static_cast<std::ptrdiff_t>(i),
                      compressed.begin() + static_cast<std::ptrdiff_t>(end)));
    }
    F::Chunk(file, "IEND", {});
    return file;
}
void Cases(TestContext& test)
{
    F::V output(131072U);
    I::Image image{};
    // Two RGBA16 rows of zeros: full-window match straddles scanline and IDAT.
    const F::V raw(65538U, 0U);
    F::V stream{0x78U, 0x01U};
    F::V stored{F::Deflate(F::V(32768U, 0U), 0U)};
    stored[0] = 0U;
    stream.insert(stream.end(), stored.begin(), stored.end());
    F::Bits bits;
    bits.Put(3U, 3U);
    const auto lit = F::LiteralLengths(false);
    const F::V dist(32U, 5U);
    bits.Code(257U, lit);
    bits.Code(29U, dist);
    bits.Put(8191U, 13U);
    F::Size rest{raw.size() - 32771U};
    while (rest >= 258U)
    {
        bits.Code(285U, lit);
        bits.Code(0U, dist);
        rest -= 258U;
    }
    while (rest != 0U)
    {
        bits.Code(0U, lit);
        --rest;
    }
    bits.Code(256U, lit);
    stream.insert(stream.end(), bits.Data.begin(), bits.Data.end());
    F::Be(stream, F::Adler(raw));
    const auto file = Wrap(4096U, 2U, 16U, 6U, stream);
    ASTERIA_EXPECT(test, "PNG max distance and block ends across IDAT",
                   I::DecodePng(F::View(file), {output.data(), output.size()}, Work, image) == I::Status::Ok,
                   image.Bytes == 32768U,
                   std::all_of(output.begin(), output.begin() + 32768,
                               [](auto v)
                               {
                                   return v == 0U;
                               }));
    const auto transparency = Wrap(2U, 1U, 16U, 0U, F::Zlib({0U, 1U, 0U, 1U, 1U}), {1U, 0U});
    const F::UInt8 expected[]{1U, 1U, 1U, 0U, 1U, 1U, 1U, 255U};
    ASTERIA_EXPECT(test, "tRNS compare before sixteen-bit reduction",
                   I::DecodePng(F::View(transparency), {output.data(), output.size()}, Work, image) == I::Status::Ok,
                   std::memcmp(output.data(), expected, 8U) == 0);
    // Each length code and each distance code, both interval endpoints.
    const F::UInt32 lengths[]{3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                              31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    const F::UInt32 lextra[]{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    F::V seed(32768U);
    for (F::Size i{0U}; i < seed.size(); ++i)
        seed[i] = static_cast<F::UInt8>(i ^ (i / 257U));
    for (bool dynamic : {false, true})
        for (bool high : {false, true})
            for (F::UInt32 code{0U}; code < 30U; ++code)
            {
                F::V s{F::Deflate(seed, 0U)};
                s[0] = 0U;
                F::Bits b;
                b.Put(dynamic ? 5U : 3U, 3U);
                const auto ll = F::LiteralLengths(dynamic);
                if (dynamic)
                    F::DynamicHeader(b, ll, dist);
                const F::UInt32 lc{dynamic ? 28U : code % 29U}; // dynamic dictionary includes length 258
                const F::UInt32 le{high ? (lc == 27U ? 30U : (1U << lextra[lc]) - 1U) : 0U};
                const F::UInt32 deBits{code < 4U ? 0U : (code / 2U) - 1U};
                const F::UInt32 base{code < 4U ? code + 1U : ((2U + (code % 2U)) << deBits) + 1U};
                const F::UInt32 de{high ? (1U << deBits) - 1U : 0U};
                b.Code(257U + lc, ll);
                b.Put(le, lextra[lc]);
                b.Code(code, dist);
                b.Put(de, deBits);
                b.Code(256U, ll);
                s.insert(s.end(), b.Data.begin(), b.Data.end());
                F::V want{seed};
                for (F::UInt32 i{0U}; i < lengths[lc] + le; ++i)
                    want.push_back(want[want.size() - base - de]);
                const auto in = F::View(s);
                F::Size written{};
                ASTERIA_EXPECT(test, "length/distance interval endpoints",
                               I::Inflate(&in, 1U, {output.data(), output.size()}, Work.Inflate, written, false) ==
                                   I::Status::Ok,
                               written == want.size(), std::memcmp(output.data(), want.data(), want.size()) == 0);
            }
    // Work limits and semantic window limit from CMF, not merely 32 KiB.
    F::V blocks;
    for (F::UInt32 n{0U}; n <= I::MaximumBlocks; ++n)
        blocks.insert(blocks.end(), {0U, 0U, 0U, 255U, 255U});
    blocks[blocks.size() - 5U] = 1U;
    auto in = F::View(blocks);
    F::Size written{};
    ASTERIA_EXPECT(test, "DEFLATE block limit",
                   I::Inflate(&in, 1U, {output.data(), output.size()}, Work.Inflate, written, false) ==
                       I::Status::Limit);
    F::V small{8U, 29U};
    stored = F::Deflate(F::V(257U, 0U), 0U);
    stored[0] = 0U;
    small.insert(small.end(), stored.begin(), stored.end());
    F::Bits back;
    back.Put(3U, 3U);
    back.Code(257U, lit);
    back.Code(16U, dist);
    back.Put(0U, 7U);
    back.Code(256U, lit);
    small.insert(small.end(), back.Data.begin(), back.Data.end());
    F::Be(small, F::Adler(F::V(260U, 0U)));
    in = F::View(small);
    ASTERIA_EXPECT(test, "zlib advertised window enforced",
                   I::Inflate(&in, 1U, {output.data(), output.size()}, Work.Inflate, written) == I::Status::Distance);
    F::Bits bomb;
    bomb.Put(3U, 3U);
    bomb.Code(0U, lit);
    for (F::Size n{0U}; n <= I::MaximumInflated / 258U; ++n)
    {
        bomb.Code(285U, lit);
        bomb.Code(0U, dist);
    }
    bomb.Code(256U, lit);
    in = F::View(bomb.Data);
    auto status = Work.Inflate.Begin(&in, 1U, false);
    F::UInt8 byte{};
    while (status == I::Status::Ok)
        status = Work.Inflate.Next(byte);
    ASTERIA_EXPECT(test, "decompressed byte limit", status == I::Status::Limit,
                   Work.Inflate.Produced() == I::MaximumInflated);
}
} // namespace
} // namespace Asteria::Test
int main()
{
    Asteria::TestSupport::TestContext test{"Imaging boundaries"};
    Asteria::Test::Cases(test);
    return test.Finish();
}
