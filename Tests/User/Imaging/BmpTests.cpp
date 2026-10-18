// SPDX-License-Identifier: Apache-2.0
#include "User/Imaging/Fixture.hpp"
namespace Asteria::Test
{
namespace
{
namespace F = ImagingFixture;
namespace I = User::Imaging;
using TestSupport::TestContext;
void Cases(TestContext& test)
{
    F::UInt8 out[16]{};
    I::Image image{};
    for (F::UInt32 header : {12U, 40U, 108U, 124U})
        for (F::UInt32 bpp : {1U, 4U, 8U, 16U, 24U, 32U})
            for (bool top : {false, true})
            {
                if (header == 12U && (top || bpp == 16U || bpp == 32U))
                    continue;
                for (F::UInt32 compression : {0U, 3U})
                {
                    if (compression == 3U && (header == 12U || (bpp != 16U && bpp != 32U) ||
                                              (header == 40U && bpp != 16U) || (header != 40U && bpp != 32U)))
                        continue;
                    const auto dib = F::Dib(header, bpp, top, false, compression);
                    const auto bmp = F::Bmp(dib);
                    for (bool file : {false, true})
                    {
                        const auto status = file ? I::DecodeBmp(F::View(bmp), {out, sizeof(out)}, image)
                                                 : I::DecodeDib(F::View(dib), {out, sizeof(out)}, image);
                        const F::UInt8 alpha{static_cast<F::UInt8>(compression == 3U && header >= 108U ? 128U : 255U)};
                        const F::UInt8 expected[]{255U, 0U, 0U, alpha, 255U, 0U, 0U, alpha,
                                                  0U,   0U, 0U, alpha, 0U,   0U, 0U, alpha};
                        ASTERIA_EXPECT(test, "BMP/DIB headers depths row orders masks", status == I::Status::Ok,
                                       image.Valid(), std::memcmp(out, expected, 16U) == 0);
                    }
                }
            }
    for (F::UInt32 mode : {1U, 2U})
    {
        auto dib = F::Dib(40U, mode == 1U ? 8U : 4U);
        const F::Size start{40U + (mode == 1U ? 256U : 16U) * 4U};
        dib.resize(start);
        const F::V rle{2U, static_cast<F::UInt8>(mode == 1U ? 0U : 0U),    0U, 0U,
                       2U, static_cast<F::UInt8>(mode == 1U ? 1U : 0x11U), 0U, 1U};
        F::Set(dib, 16U, mode);
        F::Set(dib, 20U, static_cast<F::UInt32>(rle.size()));
        dib.insert(dib.end(), rle.begin(), rle.end());
        ASTERIA_EXPECT(test, "RLE encoded runs", I::DecodeDib(F::View(dib), {out, sizeof(out)}, image) == I::Status::Ok,
                       out[0] == 255U, out[8] == 0U);
        // 4x2: absolute run, word pad, delta, encoded tail and end.
        auto absolute = F::Dib(40U, mode == 1U ? 8U : 4U);
        absolute.resize(start);
        F::Set(absolute, 4U, 4U);
        F::Set(absolute, 16U, mode);
        const F::V stream{mode == 1U ? F::V{0U, 3U, 1U, 0U, 1U, 0U, 0U, 2U, 1U, 0U, 0U, 0U, 4U, 1U, 0U, 1U}
                                     : F::V{0U, 3U, 0x10U, 0x10U, 0U, 2U, 1U, 0U, 0U, 0U, 4U, 0x11U, 0U, 1U}};
        F::Set(absolute, 20U, static_cast<F::UInt32>(stream.size()));
        absolute.insert(absolute.end(), stream.begin(), stream.end());
        F::UInt8 larger[32]{};
        ASTERIA_EXPECT(test, "RLE absolute word pad delta EOL EOB",
                       I::DecodeDib(F::View(absolute), {larger, sizeof(larger)}, image) == I::Status::Ok,
                       larger[0] == 255U, larger[16] == 255U, larger[20] == 0U, larger[24] == 255U, larger[28] == 0U);
    }
    for (F::UInt32 header : {108U, 124U})
    {
        auto d16 = F::Dib(header, 16U);
        F::Set(d16, 16U, 3U);
        F::Set(d16, 40U, 0x0F00U);
        F::Set(d16, 44U, 0x00F0U);
        F::Set(d16, 48U, 0x000FU);
        F::Set(d16, 52U, 0xF000U);
        F::Set(d16, header, 0x80F0U, 2U);
        F::Set(d16, header + 2U, 0x800FU, 2U);
        F::Set(d16, header + 4U, 0x8F00U, 2U);
        F::Set(d16, header + 6U, 0x8FFFU, 2U);
        const F::UInt8 expected[]{255U, 0U, 0U, 136U, 255U, 255U, 255U, 136U, 0U, 255U, 0U, 136U, 0U, 0U, 255U, 136U};
        ASTERIA_EXPECT(test, "V4/V5 sixteen bit RGBA masks",
                       I::DecodeDib(F::View(d16), {out, sizeof(out)}, image) == I::Status::Ok,
                       std::memcmp(out, expected, sizeof(out)) == 0);
    }
    auto profile = F::Dib(124U, 24U);
    const auto profileOffset = profile.size();
    profile.push_back(7U);
    F::Set(profile, 112U, static_cast<F::UInt32>(profileOffset));
    F::Set(profile, 116U, 1U);
    ASTERIA_EXPECT(test, "bounded trailing profile ignored",
                   I::DecodeDib(F::View(profile), {out, sizeof(out)}, image) == I::Status::Ok, out[0] == 255U);
    const auto good = F::Bmp(F::Dib(40U, 24U));
    auto refusal = [&](const F::V& v, I::Status s, bool file = true)
    {
        const auto actual = file ? I::DecodeBmp(F::View(v), {out, sizeof(out)}, image)
                                 : I::DecodeDib(F::View(v), {out, sizeof(out)}, image);
        static_cast<void>(test.ExpectEqual(static_cast<unsigned>(actual), static_cast<unsigned>(s), "actual",
                                           "expected", __FILE__, __LINE__));
        ASTERIA_EXPECT(test, "refusal clears view", image.Pixels == nullptr);
    };
    for (F::Size n{0U}; n < good.size(); ++n)
        ASTERIA_EXPECT(test, "BMP truncation refused",
                       I::DecodeBmp({good.data(), n}, {out, sizeof(out)}, image) != I::Status::Ok);
    for (F::Size n{0U}; n < good.size() - 14U; ++n)
        ASTERIA_EXPECT(test, "DIB truncation refused",
                       I::DecodeDib({good.data() + 14U, n}, {out, sizeof(out)}, image) != I::Status::Ok);
    auto bad = good;
    bad[0] = 'X';
    refusal(bad, I::Status::Signature);
    bad = good;
    F::Set(bad, 2U, 0U);
    refusal(bad, I::Status::SizeMismatch);
    for (F::Size p : {6U, 8U})
    {
        bad = good;
        bad[p] = 1U;
        refusal(bad, I::Status::Header);
    }
    for (F::UInt32 offset : {0U, 14U, 100000U})
    {
        bad = good;
        F::Set(bad, 10U, offset);
        refusal(bad, I::Status::Header);
    }
    auto d = F::Dib(40U, 24U);
    for (F::Size p : {4U, 8U, 12U})
    {
        bad = d;
        F::Set(bad, p, 0U, p == 12U ? 2U : 4U);
        refusal(bad, I::Status::Header, false);
    }
    bad = d;
    F::Set(bad, 4U, 4097U);
    refusal(bad, I::Status::Limit, false);
    bad = d;
    F::Set(bad, 4U, 0xFFFFFFFFU);
    refusal(bad, I::Status::Header, false);
    bad = d;
    F::Set(bad, 0U, 64U);
    refusal(bad, I::Status::Unsupported, false);
    bad = d;
    F::Set(bad, 14U, 2U, 2U);
    refusal(bad, I::Status::Unsupported, false);
    bad = d;
    F::Set(bad, 16U, 5U);
    refusal(bad, I::Status::Unsupported, false);
    bad = d;
    F::Set(bad, 16U, 1U);
    refusal(bad, I::Status::Header, false);
    bad = d;
    F::Set(bad, 20U, 1U);
    refusal(bad, I::Status::SizeMismatch, false);
    bad = F::Dib(124U, 24U);
    F::Set(bad, 120U, 1U);
    refusal(bad, I::Status::Header, false);
    bad = F::Dib(124U, 24U);
    F::Set(bad, 112U, 124U);
    F::Set(bad, 116U, 100000U);
    refusal(bad, I::Status::Header, false);
    for (F::UInt32 mask : {0U, 0xF0F0U, 0x10000U, 0x7E0U})
    {
        bad = F::Dib(40U, 16U, false, false, 3U);
        F::Set(bad, 40U, mask);
        refusal(bad, I::Status::Mask, false);
    }
    bad = F::Dib(40U, 8U);
    F::Set(bad, 32U, 257U);
    refusal(bad, I::Status::Palette, false);
    bad = F::Dib(40U, 8U);
    F::Set(bad, 32U, 1U);
    bad[44U] = 1U;
    refusal(bad, I::Status::Palette, false);
}
} // namespace
} // namespace Asteria::Test
int main()
{
    Asteria::TestSupport::TestContext test{"Imaging BMP"};
    Asteria::Test::Cases(test);
    return test.Finish();
}
