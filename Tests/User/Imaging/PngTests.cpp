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
void Cases(TestContext& test)
{
    F::V out(4096U);
    I::Image image{};
    for (F::UInt32 type : {0U, 2U, 3U, 4U, 6U})
        for (F::UInt32 depth : {1U, 2U, 4U, 8U, 16U})
        {
            if ((type == 3U && depth == 16U) || (type != 0U && type != 3U && depth < 8U))
                continue;
            for (F::UInt32 filter{0U}; filter < 5U; ++filter)
                for (bool interlace : {false, true})
                {
                    auto v = F::Png(type, depth, filter, interlace, type == 0U || type == 2U || type == 3U, 9U, 9U,
                                    filter % 3U, 1U);
                    ASTERIA_EXPECT(test, "all PNG types depths filters Adam7 byte IDAT split",
                                   I::DecodePng(F::View(v.File), {out.data(), out.size()}, Work, image) ==
                                       I::Status::Ok,
                                   image.Valid(), image.Width == 9U, image.Height == 9U,
                                   std::memcmp(out.data(), v.Pixels.data(), v.Pixels.size()) == 0);
                }
        }
    for (F::UInt32 w : {1U, 2U, 3U, 4U, 5U})
    {
        auto v = F::Png(6U, 16U, 4U, true, false, w, w);
        ASTERIA_EXPECT(test, "empty Adam7 passes",
                       I::DecodePng(F::View(v.File), {out.data(), out.size()}, Work, image) == I::Status::Ok,
                       std::memcmp(out.data(), v.Pixels.data(), v.Pixels.size()) == 0);
    }
    const auto good = F::Png(6U, 8U, 0U);
    auto refusal = [&](const F::V& file, I::Status s)
    {
        ASTERIA_EXPECT(test, "typed PNG refusal",
                       I::DecodePng(F::View(file), {out.data(), out.size()}, Work, image) == s,
                       image.Pixels == nullptr);
    };
    for (F::Size n{0U}; n < good.File.size(); ++n)
    {
        ASTERIA_EXPECT(test, "every PNG truncation refused",
                       I::DecodePng({good.File.data(), n}, {out.data(), out.size()}, Work, image) != I::Status::Ok,
                       image.Pixels == nullptr);
    }
    auto bad = good.File;
    bad[0] = 0U;
    refusal(bad, I::Status::Signature);
    bad = good.File;
    bad[29] ^= 1U;
    refusal(bad, I::Status::Crc);
    bad = good.File;
    bad[16] = 0x7FU;
    F::RepairCrc(bad, 8U);
    refusal(bad, I::Status::Limit);
    bad = good.File;
    bad[16] = bad[17] = bad[18] = bad[19] = 0U;
    F::RepairCrc(bad, 8U);
    refusal(bad, I::Status::Header);
    for (F::Size field : {24U, 25U, 26U, 27U, 28U})
    {
        bad = good.File;
        bad[field] = 7U;
        F::RepairCrc(bad, 8U);
        refusal(bad, I::Status::Header);
    }
    bad = good.File;
    bad.push_back(0U);
    refusal(bad, I::Status::SizeMismatch);
    bad = good.File;
    bad[12] = 'J';
    F::RepairCrc(bad, 8U);
    refusal(bad, I::Status::Order);
    bad = good.File;
    bad[14] = 'd';
    F::RepairCrc(bad, 8U);
    refusal(bad, I::Status::Header);
    bad = good.File;
    bad[12] = '1';
    F::RepairCrc(bad, 8U);
    refusal(bad, I::Status::Header);
    bad = good.File;
    bad[8] = 0x80U;
    refusal(bad, I::Status::Header);
    // Build independently ordered chunk sequences and decompressed failures.
    const F::V ihdr(good.File.begin(), good.File.begin() + 33);
    auto file = [&](const F::V& raw)
    {
        F::V v{ihdr};
        F::Chunk(v, "IDAT", F::Zlib(raw));
        F::Chunk(v, "IEND", {});
        return v;
    };
    auto raw = good.Raw;
    raw[0] = 5U;
    refusal(file(raw), I::Status::Filter);
    raw = good.Raw;
    raw.pop_back();
    refusal(file(raw), I::Status::SizeMismatch);
    raw = good.Raw;
    raw.push_back(0U);
    refusal(file(raw), I::Status::SizeMismatch);
    bad = ihdr;
    F::Chunk(bad, "IEND", {});
    refusal(bad, I::Status::Order);
    bad = ihdr;
    F::Chunk(bad, "IHDR", F::V(good.File.begin() + 16, good.File.begin() + 29));
    refusal(bad, I::Status::Order);
    bad = ihdr;
    F::Chunk(bad, "JUNK", {});
    refusal(bad, I::Status::Unsupported);
    bad = ihdr;
    F::Chunk(bad, "tRNS", {});
    refusal(bad, I::Status::Header);
    bad = ihdr;
    F::Chunk(bad, "IDAT", F::Zlib(good.Raw));
    F::Chunk(bad, "tEXt", {});
    F::Chunk(bad, "IDAT", {});
    F::Chunk(bad, "IEND", {});
    refusal(bad, I::Status::Order);
    bad = ihdr;
    F::Chunk(bad, "PLTE", {0U});
    refusal(bad, I::Status::Palette);
    bad = ihdr;
    F::Chunk(bad, "gAMA", {0U, 0U, 0U, 0U});
    F::Chunk(bad, "IDAT", F::Zlib(good.Raw));
    F::Chunk(bad, "IEND", {});
    ASTERIA_EXPECT(test, "ancillary metadata ignored after CRC",
                   I::DecodePng(F::View(bad), {out.data(), out.size()}, Work, image) == I::Status::Ok);
    auto indexed = F::Png(3U, 8U, 0U, false, false, 1U, 1U);
    bad = F::V(indexed.File.begin(), indexed.File.begin() + 33);
    F::Chunk(bad, "PLTE", {0U, 0U, 0U});
    F::Chunk(bad, "IDAT", F::Zlib({0U, 1U}));
    F::Chunk(bad, "IEND", {});
    refusal(bad, I::Status::Palette);
    auto grey = F::Png(0U, 1U, 0U);
    bad = F::V(grey.File.begin(), grey.File.begin() + 33);
    F::Chunk(bad, "tRNS", {0U, 2U});
    refusal(bad, I::Status::Header);
    ASTERIA_EXPECT(test, "null input and output",
                   I::DecodePng({nullptr, 1U}, {out.data(), out.size()}, Work, image) == I::Status::InvalidBuffer,
                   I::DecodePng(F::View(good.File), {nullptr, out.size()}, Work, image) == I::Status::InvalidBuffer);
}
} // namespace
} // namespace Asteria::Test
int main()
{
    Asteria::TestSupport::TestContext test{"Imaging PNG"};
    Asteria::Test::Cases(test);
    return test.Finish();
}
