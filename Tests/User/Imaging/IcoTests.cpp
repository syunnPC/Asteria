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
    const auto dib = F::Dib(40U, 24U, false, true);
    const auto png = F::Png(6U, 8U, 4U, true, false, 9U, 9U);
    auto file = F::Ico({dib, png.File}, {2U, 9U});
    F::V out(4096U);
    I::Image image{};
    ASTERIA_EXPECT(test, "ICO requested DIB",
                   I::DecodeIco(F::View(file), 2U, 2U, {out.data(), out.size()}, Work, image) == I::Status::Ok,
                   image.Width == 2U, out[0] == 255U, out[8] == 0U);
    ASTERIA_EXPECT(test, "ICO requested PNG",
                   I::DecodeIco(F::View(file), 8U, 8U, {out.data(), out.size()}, Work, image) == I::Status::Ok,
                   image.Width == 9U, std::memcmp(out.data(), png.Pixels.data(), png.Pixels.size()) == 0);
    auto mask = dib;
    mask[mask.size() - 8U] = 0x80U;
    auto masked = F::Ico({mask}, {2U});
    ASTERIA_EXPECT(test, "AND transparent black pixel",
                   I::DecodeIco(F::View(masked), 2U, 2U, {out.data(), out.size()}, Work, image) == I::Status::Ok,
                   out[11] == 0U);
    mask = dib;
    mask[mask.size() - 4U] = 0x80U;
    masked = F::Ico({mask}, {2U});
    ASTERIA_EXPECT(test, "destination inversion refused",
                   I::DecodeIco(F::View(masked), 2U, 2U, {out.data(), out.size()}, Work, image) ==
                       I::Status::Unsupported,
                   image.Pixels == nullptr);
    auto refusal = [&](const F::V& v, I::Status s)
    {
        ASTERIA_EXPECT(test, "typed ICO refusal",
                       I::DecodeIco(F::View(v), 2U, 2U, {out.data(), out.size()}, Work, image) == s,
                       image.Pixels == nullptr);
    };
    for (F::Size n{0U}; n < file.size(); ++n)
        ASTERIA_EXPECT(test, "ICO all truncations",
                       I::DecodeIco({file.data(), n}, 2U, 2U, {out.data(), out.size()}, Work, image) != I::Status::Ok);
    auto bad = file;
    F::Set(bad, 2U, 2U, 2U);
    refusal(bad, I::Status::Signature);
    bad = file;
    F::Set(bad, 4U, 0U, 2U);
    refusal(bad, I::Status::Header);
    bad = file;
    F::Set(bad, 4U, 65U, 2U);
    refusal(bad, I::Status::Limit);
    bad = file;
    bad[9] = 1U;
    refusal(bad, I::Status::Header);
    bad = file;
    F::Set(bad, 10U, 2U, 2U);
    refusal(bad, I::Status::Header);
    bad = file;
    F::Set(bad, 12U, 2U, 2U);
    refusal(bad, I::Status::Header);
    bad = file;
    F::Set(bad, 14U, 0U);
    refusal(bad, I::Status::Truncated);
    bad = file;
    F::Set(bad, 18U, 0U);
    refusal(bad, I::Status::Truncated);
    bad = file;
    F::Set(bad, 34U, 38U);
    refusal(bad, I::Status::Header);
    bad = file;
    bad[6] = 3U;
    refusal(bad, I::Status::SizeMismatch);
    auto d = dib;
    F::Set(d, 8U, 3U);
    refusal(F::Ico({d}, {2U}), I::Status::Header);
    d = dib;
    F::Set(d, 24U, 1U);
    refusal(F::Ico({d}, {2U}), I::Status::Header);
    d = dib;
    F::Set(d, 8U, 0xFFFFFFFCU);
    refusal(F::Ico({d}, {2U}), I::Status::Header);
    auto tied = F::Ico({dib, png.File}, {2U, 9U});
    ASTERIA_EXPECT(test, "best size tie prefers larger",
                   I::DecodeIco(F::View(tied), 5U, 6U, {out.data(), out.size()}, Work, image) == I::Status::Ok,
                   image.Width == 9U);
    auto big = F::Png(6U, 8U, 0U, false, false, 256U, 256U);
    auto zero = F::Ico({big.File}, {256U});
    F::V bigOut(256U * 256U * 4U);
    ASTERIA_EXPECT(test, "directory zero means 256",
                   I::DecodeIco(F::View(zero), 256U, 256U, {bigOut.data(), bigOut.size()}, Work, image) ==
                       I::Status::Ok,
                   image.Width == 256U, std::memcmp(bigOut.data(), big.Pixels.data(), big.Pixels.size()) == 0);
    ASTERIA_EXPECT(test, "ICO input and requested bounds",
                   I::DecodeIco(F::View(file), 0U, 1U, {out.data(), out.size()}, Work, image) == I::Status::Header,
                   I::DecodeIco(F::View(file), I::MaximumDimension + 1U, 1U, {out.data(), out.size()}, Work, image) ==
                       I::Status::Header,
                   I::DecodeIco({nullptr, 1U}, 2U, 2U, {out.data(), out.size()}, Work, image) ==
                       I::Status::InvalidBuffer,
                   I::DecodeIco({file.data(), I::MaximumInput + 1U}, 2U, 2U, {out.data(), out.size()}, Work, image) ==
                       I::Status::Limit);
}
} // namespace
} // namespace Asteria::Test
int main()
{
    Asteria::TestSupport::TestContext test{"Imaging ICO"};
    Asteria::Test::Cases(test);
    return test.Finish();
}
