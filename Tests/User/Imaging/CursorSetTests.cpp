// SPDX-License-Identifier: Apache-2.0
#include "User/Imaging/Fixture.hpp"
#include <fstream>
#include <iterator>
namespace Asteria::Test
{
namespace
{
namespace F = ImagingFixture;
namespace I = User::Imaging;
using TestSupport::TestContext;
static I::Workspace Work;
static I::CursorSet Set;
void Cases(TestContext& test)
{
    std::ifstream input{ASTERIA_CURSOR_DATA_DIR "/Default.acsr", std::ios::binary};
    F::V file{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    F::V arena(I::MaximumPixels * 4U);
    ASTERIA_EXPECT(test, "generated cursor set round trip", !file.empty(),
                   I::DecodeCursorSet(F::View(file), {arena.data(), arena.size()}, Work, Set) == I::Status::Ok,
                   Set.Count == 11U, Set.Length[2] == 2U);
    for (F::UInt32 i{0U}; i < Set.Count; ++i)
        ASTERIA_EXPECT(test, "validated frames and hotspots", Set.Frames[i].Pixels.Valid(),
                       Set.Frames[i].Pixels.Width == 32U, Set.Frames[i].HotspotX < 32U, Set.Frames[i].HotspotY < 32U);
    // Independent encoder: mix raw and PNG, two busy frames, exact decoded bytes.
    auto png = F::Png(6U, 16U, 4U, true, false, 2U, 2U);
    F::V mixed{'A', 'C', 'S', 'R'};
    F::Le(mixed, 1U, 2U);
    F::Le(mixed, 10U, 2U);
    F::Le(mixed, 0U);
    F::Le(mixed, 0U);
    for (F::UInt32 role{0U}; role < 10U; ++role)
    {
        F::Le(mixed, role, 2U);
        F::Le(mixed, role == 2U ? 2U : 1U, 2U);
        for (F::UInt32 frame{0U}; frame < (role == 2U ? 2U : 1U); ++frame)
        {
            F::Le(mixed, 2U, 2U);
            F::Le(mixed, 2U, 2U);
            F::Le(mixed, 1U, 2U);
            F::Le(mixed, 1U, 2U);
            F::Le(mixed, role == 2U ? 50U + frame : 0U);
            F::Le(mixed, role % 2U);
            const auto& payload = role % 2U == 0U ? png.Pixels : png.File;
            F::Le(mixed, static_cast<F::UInt32>(payload.size()));
            mixed.insert(mixed.end(), payload.begin(), payload.end());
        }
    }
    F::Set(mixed, 8U, static_cast<F::UInt32>(mixed.size()));
    ASTERIA_EXPECT(test, "raw PNG animation container roundtrip",
                   I::DecodeCursorSet(F::View(mixed), {arena.data(), arena.size()}, Work, Set) == I::Status::Ok,
                   Set.Count == 11U);
    for (F::UInt32 i{0U}; i < Set.Count; ++i)
        ASTERIA_EXPECT(test, "pixel exact container payloads",
                       std::memcmp(Set.Frames[i].Pixels.Pixels, png.Pixels.data(), png.Pixels.size()) == 0);
    auto refusal = [&](const F::V& v, I::Status s)
    {
        ASTERIA_EXPECT(test, "typed cursor refusal",
                       I::DecodeCursorSet(F::View(v), {arena.data(), arena.size()}, Work, Set) == s, Set.Count == 0U);
    };
    for (F::Size n{0U}; n < mixed.size(); ++n)
    {
        F::V truncated(mixed.begin(), mixed.begin() + static_cast<std::ptrdiff_t>(n));
        if (n >= 12U)
            F::Set(truncated, 8U, static_cast<F::UInt32>(n));
        ASTERIA_EXPECT(test, "all cursor truncations refused",
                       I::DecodeCursorSet(F::View(truncated), {arena.data(), arena.size()}, Work, Set) != I::Status::Ok,
                       Set.Count == 0U);
    }
    auto bad = mixed;
    bad[0] = 0U;
    refusal(bad, I::Status::Signature);
    bad = mixed;
    bad[4] = 2U;
    refusal(bad, I::Status::Unsupported);
    for (F::Size field : {6U, 12U})
    {
        bad = mixed;
        bad[field] = 1U;
        refusal(bad, I::Status::Header);
    }
    bad = mixed;
    bad[8] = 0U;
    refusal(bad, I::Status::SizeMismatch);
    bad = mixed;
    bad[16] = 1U;
    refusal(bad, I::Status::Order);
    bad = mixed;
    bad[18] = 0U;
    refusal(bad, I::Status::Limit);
    bad = mixed;
    bad[18] = 33U;
    refusal(bad, I::Status::Limit);
    bad = mixed;
    bad[20] = 0U;
    refusal(bad, I::Status::Header);
    bad = mixed;
    F::Set(bad, 20U, 129U, 2U);
    refusal(bad, I::Status::Limit);
    for (F::Size field : {24U, 26U})
    {
        bad = mixed;
        bad[field] = 2U;
        refusal(bad, I::Status::Hotspot);
    }
    bad = mixed;
    bad[28] = 1U;
    refusal(bad, I::Status::Header);
    bad = mixed;
    bad[32] = 2U;
    refusal(bad, I::Status::Unsupported);
    bad = mixed;
    bad[36] = 15U;
    refusal(bad, I::Status::SizeMismatch);
    bad = mixed;
    F::Set(bad, 36U, 100000U);
    refusal(bad, I::Status::Truncated);
    for (F::Size field : {22U})
    {
        bad = mixed;
        bad[field] = 0U;
        refusal(bad, I::Status::Header);
    }
    bad = mixed;
    F::Set(bad, 22U, 129U, 2U);
    refusal(bad, I::Status::Limit);
    bad = mixed;
    F::Set(bad, 60U, 3U, 2U);
    refusal(bad, I::Status::SizeMismatch); // PNG dimension mismatch
    // Locate busy role records using the independent encoding's lengths.
    F::Size busy{16U};
    for (F::UInt32 role{0U}; role < 2U; ++role)
    {
        busy += 4U;
        F::UInt32 bytes{0U};
        for (F::UInt32 i{0U}; i < 4U; ++i)
            bytes |= static_cast<F::UInt32>(mixed[busy + 16U + i]) << (i * 8U);
        busy += 20U + bytes;
    }
    bad = mixed;
    F::Set(bad, busy + 4U + 8U, 0U);
    refusal(bad, I::Status::Header);
    bad = mixed;
    F::Set(bad, busy + 4U + 8U, I::MaximumFrameDelay + 1U);
    refusal(bad, I::Status::Header);
    bad = mixed;
    bad.push_back(0U);
    F::Set(bad, 8U, static_cast<F::UInt32>(bad.size()));
    refusal(bad, I::Status::SizeMismatch);
    ASTERIA_EXPECT(test, "cursor arena and input bounds",
                   I::DecodeCursorSet(F::View(mixed), {arena.data(), 15U}, Work, Set) == I::Status::Limit,
                   I::DecodeCursorSet(F::View(mixed), {nullptr, 1U}, Work, Set) == I::Status::InvalidBuffer,
                   I::DecodeCursorSet({mixed.data(), I::MaximumInput + 1U}, {arena.data(), arena.size()}, Work, Set) ==
                       I::Status::Limit);
    std::printf("storage: InflateReader=%zu Workspace=%zu CursorSet=%zu bytes\n", sizeof(I::InflateReader),
                sizeof(I::Workspace), sizeof(I::CursorSet));
}
} // namespace
} // namespace Asteria::Test
int main()
{
    Asteria::TestSupport::TestContext test{"Imaging CursorSet"};
    Asteria::Test::Cases(test);
    return test.Finish();
}
