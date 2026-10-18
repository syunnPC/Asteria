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
static I::CursorSet Set;
void Png(TestContext& test)
{
    F::UInt8 output[16]{};
    I::Image image{};
    const auto valid = F::Png(3U, 1U, 0U, false, false, 2U, 2U);
    const F::V start(valid.File.begin(), valid.File.begin() + 33);
    auto check = [&](const F::V& v, I::Status s)
    {
        ASTERIA_CHECK_EQ(test, static_cast<unsigned>(I::DecodePng(F::View(v), {output, sizeof(output)}, Work, image)),
                         static_cast<unsigned>(s));
        ASTERIA_EXPECT(test, "refused image unpublished", !image.Valid());
    };
    auto v = start;
    F::Chunk(v, "IDAT", {});
    check(v, I::Status::Order);
    v = start;
    F::Chunk(v, "tRNS", {});
    check(v, I::Status::Order);
    v = start;
    F::Chunk(v, "PLTE", {});
    check(v, I::Status::Palette);
    v = start;
    F::Chunk(v, "PLTE", F::V(9U));
    check(v, I::Status::Palette);
    v = start;
    F::Chunk(v, "PLTE", {0U, 0U, 0U});
    F::Chunk(v, "PLTE", {0U, 0U, 0U});
    check(v, I::Status::Order);
    v = start;
    F::Chunk(v, "PLTE", {0U, 0U, 0U});
    F::Chunk(v, "tRNS", {});
    check(v, I::Status::Palette);
    v = start;
    F::Chunk(v, "PLTE", {0U, 0U, 0U});
    F::Chunk(v, "tRNS", {1U, 2U});
    check(v, I::Status::Palette);
    v = start;
    F::Chunk(v, "PLTE", {0U, 0U, 0U});
    F::Chunk(v, "tRNS", {255U});
    F::Chunk(v, "tRNS", {255U});
    check(v, I::Status::Order);
    const auto rgba = F::Png(6U, 8U, 0U, false, false, 1U, 1U);
    const F::V rstart(rgba.File.begin(), rgba.File.begin() + 33);
    v = rstart;
    F::Chunk(v, "IDAT", F::Zlib(rgba.Raw));
    F::Chunk(v, "PLTE", {0U, 0U, 0U});
    check(v, I::Status::Order);
    v = rstart;
    F::Chunk(v, "IDAT", F::Zlib(rgba.Raw));
    F::Chunk(v, "tRNS", {0U});
    check(v, I::Status::Order);
    v = rstart;
    F::Chunk(v, "IDAT", F::Zlib(rgba.Raw));
    F::Chunk(v, "IEND", {0U});
    check(v, I::Status::Header);
    v = rstart;
    for (F::UInt32 i{0U}; i < I::MaximumChunks; ++i)
        F::Chunk(v, "tEXt", {});
    check(v, I::Status::Limit);
    auto bad = rstart;
    bad[11] = 12U;
    F::RepairCrc(bad, 8U);
    check(bad, I::Status::Header);
    const auto grey = F::Png(0U, 8U, 0U, false, false, 1U, 1U);
    v = F::V(grey.File.begin(), grey.File.begin() + 33);
    F::Chunk(v, "PLTE", {0U, 0U, 0U});
    check(v, I::Status::Palette);
    v = F::V(grey.File.begin(), grey.File.begin() + 33);
    F::Chunk(v, "tRNS", {0U});
    check(v, I::Status::Header);
    v = F::V(grey.File.begin(), grey.File.begin() + 33);
    F::Chunk(v, "tRNS", {0U, 0U});
    F::Chunk(v, "PLTE", {0U, 0U, 0U});
    check(v, I::Status::Order);
    v = rstart;
    F::Set(v, 16U, 0U); // write BE dimensions below
    v = rstart;
    v[19] = 0U;
    F::RepairCrc(v, 8U);
    check(v, I::Status::Header);
    v = rstart;
    v[23] = 0U;
    F::RepairCrc(v, 8U);
    check(v, I::Status::Header);
    v = rstart;
    v[16] = 0U;
    v[17] = 0U;
    v[18] = 0x10U;
    v[19] = 0U;
    v[20] = 0U;
    v[21] = 0U;
    v[22] = 0x10U;
    v[23] = 0U;
    F::RepairCrc(v, 8U);
    check(v, I::Status::Limit);
    ASTERIA_EXPECT(test, "PNG bounded input and output",
                   I::DecodePng({start.data(), I::MaximumInput + 1U}, {output, sizeof(output)}, Work, image) ==
                       I::Status::Limit,
                   I::DecodePng(F::View(rgba.File), {output, 3U}, Work, image) == I::Status::Limit);
}
void Bmp(TestContext& test)
{
    F::UInt8 output[32]{};
    I::Image image{};
    auto check = [&](const F::V& v, I::Status s)
    {
        ASTERIA_CHECK_EQ(test, static_cast<unsigned>(I::DecodeDib(F::View(v), {output, sizeof(output)}, image)),
                         static_cast<unsigned>(s));
    };
    auto v = F::Dib(12U, 24U);
    F::Set(v, 8U, 0U, 2U);
    check(v, I::Status::Header);
    v = F::Dib(12U, 24U);
    F::Set(v, 10U, 16U, 2U);
    check(v, I::Status::Unsupported);
    v = F::Dib(12U, 24U);
    v[18U] = 1U;
    check(v, I::Status::Header);
    v = F::Dib(40U, 24U);
    F::Set(v, 16U, 3U);
    check(v, I::Status::Header);
    v = F::Dib(40U, 8U, true);
    F::Set(v, 16U, 1U);
    check(v, I::Status::Header);
    v = F::Dib(40U, 8U);
    F::Set(v, 16U, 2U);
    check(v, I::Status::Header);
    v = F::Dib(40U, 24U);
    F::Set(v, 32U, 257U);
    check(v, I::Status::Limit);
    v = F::Dib(40U, 8U);
    v[43] = 1U;
    check(v, I::Status::Palette);
    v = F::Dib(124U, 24U);
    F::Set(v, 112U, 124U);
    F::Set(v, 116U, 1U);
    check(v, I::Status::Header);
    v = F::Dib(124U, 24U);
    F::Set(v, 112U, 1U);
    F::Set(v, 116U, 1U);
    check(v, I::Status::Header);
    v = F::Dib(108U, 32U, false, false, 3U);
    F::Set(v, 52U, 0x80000100U);
    check(v, I::Status::Mask);
    v = F::Dib(40U, 16U, false, false, 3U);
    v.resize(48U);
    check(v, I::Status::Truncated);
    v = F::Dib(40U, 8U);
    v.resize(40U + 1023U);
    check(v, I::Status::Truncated);
    for (F::UInt32 mode : {1U, 2U})
    {
        auto base = F::Dib(40U, mode == 1U ? 8U : 4U);
        base.resize(40U + (mode == 1U ? 256U : 16U) * 4U);
        F::Set(base, 16U, mode);
        check(base, I::Status::Header);
        auto rle = [&](const F::V& data, I::Status s)
        {
            auto b = base;
            F::Set(b, 20U, static_cast<F::UInt32>(data.size()));
            b.insert(b.end(), data.begin(), data.end());
            check(b, s);
        };
        rle({0U}, I::Status::Truncated);
        rle({3U, 0U, 0U, 1U}, I::Status::Header);
        rle({0U, 2U, 3U, 0U}, I::Status::Header);
        rle({0U, 2U, 0U, 2U}, I::Status::Header);
        rle({0U, 2U, 0U}, I::Status::Truncated);
        rle({0U, 0U, 0U, 0U, 0U, 0U}, I::Status::Header);
        rle({0U, 0U, 0U, 0U, 1U, 0U}, I::Status::Header);
        rle({0U, 1U, 0U}, I::Status::SizeMismatch);
        rle({2U, 0U}, I::Status::Truncated);
        auto b = base;
        F::Set(b, 20U, 100U);
        check(b, I::Status::Truncated);
        F::Set(base, 4U, 4U);
        rle({0U, 3U, 0U}, I::Status::Truncated);
        if (mode == 1U)
            rle({0U, 3U, 0U, 0U, 0U, 1U, 0U, 1U}, I::Status::Header);
        F::Set(base, 32U, 1U);
        base.resize(44U);
        rle({1U, mode == 1U ? F::UInt8{1U} : F::UInt8{0x10U}, 0U, 1U}, I::Status::Palette);
    }
    ASTERIA_EXPECT(test, "BMP invalid buffers and bounded input",
                   I::DecodeDib({nullptr, 1U}, {output, sizeof(output)}, image) == I::Status::InvalidBuffer,
                   I::DecodeBmp({output, I::MaximumInput + 1U}, {output, sizeof(output)}, image) == I::Status::Limit,
                   I::DecodeDib({output, I::MaximumInput + 1U}, {output, sizeof(output)}, image) == I::Status::Limit);
}
void Inflate(TestContext& test)
{
    F::UInt8 output[32]{};
    F::Size written{};
    auto check = [&](const F::V& v, I::Status s)
    {
        const auto in = F::View(v);
        ASTERIA_CHECK_EQ(
            test, static_cast<unsigned>(I::Inflate(&in, 1U, {output, sizeof(output)}, Work.Inflate, written, false)),
            static_cast<unsigned>(s));
    };
    const auto lit = F::LiteralLengths(false);
    const F::V dist(32U, 5U);
    F::Bits invalid;
    invalid.Put(3U, 3U);
    invalid.Code(284U, lit);
    invalid.Put(31U, 5U);
    check(invalid.Data, I::Status::Huffman);
    invalid = {};
    invalid.Put(5U, 3U);
    invalid.Put(30U, 5U);
    invalid.Put(0U, 5U);
    invalid.Put(0U, 4U);
    check(invalid.Data, I::Status::Huffman);
    invalid = {};
    invalid.Put(5U, 3U);
    F::V ll(257U, 0U);
    ll[0] = 1U;
    ll[256] = 1U;
    F::DynamicHeader(invalid, ll, F::V{5U});
    check(invalid.Data, I::Status::Huffman);
    invalid = {};
    invalid.Put(5U, 3U);
    F::V codes(19U, 0U);
    codes[16] = 1U;
    codes[0] = 1U;
    invalid.Put(0U, 5U);
    invalid.Put(0U, 5U);
    invalid.Put(0U, 4U);
    for (auto n : {1U, 0U, 0U, 1U})
        invalid.Put(n, 3U);
    invalid.Code(16U, codes);
    check(invalid.Data, I::Status::Huffman);
    // Repeat 18 three times: the third overruns the 258 expected lengths.
    invalid = {};
    invalid.Put(5U, 3U);
    invalid.Put(0U, 5U);
    invalid.Put(0U, 5U);
    invalid.Put(0U, 4U);
    for (auto n : {0U, 0U, 1U, 1U})
        invalid.Put(n, 3U);
    codes = F::V(19U, 0U);
    codes[18] = 1U;
    codes[0] = 1U;
    for (F::UInt32 i{0U}; i < 2U; ++i)
    {
        invalid.Code(18U, codes);
        invalid.Put(127U, 7U);
    }
    check(invalid.Data, I::Status::Huffman);
    I::Bytes in{output, I::MaximumInput + 1U};
    ASTERIA_EXPECT(test, "inflate input limit", Work.Inflate.Begin(&in, 1U) == I::Status::Limit);
    in = {nullptr, 1U};
    ASTERIA_EXPECT(test, "inflate null segment", Work.Inflate.Begin(&in, 1U) == I::Status::InvalidBuffer);
    auto z = F::Zlib({0U});
    in = F::View(z);
    ASTERIA_EXPECT(test, "inflate invalid output",
                   I::Inflate(&in, 1U, {nullptr, 1U}, Work.Inflate, written) == I::Status::InvalidBuffer);
}
} // namespace
} // namespace Asteria::Test
int main()
{
    Asteria::TestSupport::TestContext test{"Imaging refusals"};
    Asteria::Test::Png(test);
    Asteria::Test::Bmp(test);
    Asteria::Test::Inflate(test);
    return test.Finish();
}
