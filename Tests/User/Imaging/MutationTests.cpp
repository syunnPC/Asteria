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
F::UInt32 Random(F::UInt32& seed)
{
    seed ^= seed << 13U;
    seed ^= seed >> 17U;
    seed ^= seed << 5U;
    return seed;
}
void RepairPng(F::V& v)
{
    for (F::Size pos{8U}; pos + 12U <= v.size();)
    {
        F::UInt32 n{0U};
        for (F::UInt32 i{0U}; i < 4U; ++i)
            n = n * 256U + v[pos + i];
        if (n > v.size() - pos - 12U)
            break;
        F::RepairCrc(v, pos);
        pos += static_cast<F::Size>(n) + 12U;
    }
}
void Cases(TestContext& test)
{
    F::V cursor{'A', 'C', 'S', 'R'};
    F::Le(cursor, 1U, 2U);
    F::Le(cursor, 10U, 2U);
    F::Le(cursor, 0U);
    F::Le(cursor, 0U);
    for (F::UInt32 role{0U}; role < 10U; ++role)
    {
        F::Le(cursor, role, 2U);
        F::Le(cursor, 1U, 2U);
        F::Le(cursor, 1U, 2U);
        F::Le(cursor, 1U, 2U);
        F::Le(cursor, 0U, 2U);
        F::Le(cursor, 0U, 2U);
        F::Le(cursor, 0U);
        F::Le(cursor, 0U);
        F::Le(cursor, 4U);
        cursor.insert(cursor.end(), {1U, 2U, 3U, 255U});
    }
    F::Set(cursor, 8U, static_cast<F::UInt32>(cursor.size()));
    const auto png = F::Png(6U, 16U, 4U, true, false, 9U, 9U, 2U, 7U);
    std::vector<F::V> seeds{F::Zlib(png.Raw, 0U),
                            F::Zlib(png.Raw, 1U),
                            F::Zlib(png.Raw, 2U),
                            png.File,
                            F::Bmp(F::Dib(12U, 4U)),
                            F::Bmp(F::Dib(108U, 32U, false, false, 3U)),
                            F::Dib(40U, 8U),
                            F::Ico({F::Dib(40U, 24U, false, true), png.File}, {2U, 9U}),
                            cursor};
    for (F::UInt32 mode : {1U, 2U})
    {
        auto b = F::Dib(40U, mode == 1U ? 8U : 4U);
        b.resize(40U + (mode == 1U ? 256U : 16U) * 4U);
        F::Set(b, 16U, mode);
        F::Set(b, 20U, 6U);
        b.insert(b.end(), {2U, 0U, 0U, 0U, 0U, 1U});
        seeds.push_back(b);
    }
    F::V output(4096U + 32U);
    F::UInt32 seed{0x1951AC35U};
    for (F::Size kind{0U}; kind < seeds.size(); ++kind)
        for (F::UInt32 iteration{0U}; iteration < 4000U; ++iteration)
        {
            F::V v{seeds[kind]};
            const F::UInt32 edits{1U + Random(seed) % 4U};
            for (F::UInt32 edit{0U}; edit < edits; ++edit)
            {
                if (v.empty())
                    break;
                const F::Size pos{Random(seed) % v.size()};
                switch (Random(seed) % 4U)
                {
                case 0U:
                    v[pos] ^= static_cast<F::UInt8>(1U << (Random(seed) % 8U));
                    break;
                case 1U:
                    v[pos] = static_cast<F::UInt8>(Random(seed));
                    break;
                case 2U:
                    v.resize(pos);
                    break;
                default:
                    v.insert(v.begin() + static_cast<std::ptrdiff_t>(pos), static_cast<F::UInt8>(Random(seed)));
                    break;
                }
            }
            if (v.empty())
                v.push_back(0U);
            if (kind == 3U && iteration % 2U == 0U)
                RepairPng(v);
            std::fill(output.begin(), output.end(), 0xA5U);
            I::Image image{};
            F::Size written{};
            I::Status status{};
            const I::Buffer buffer{output.data() + 16U, 4096U};
            if (kind < 3U)
            {
                const auto in = F::View(v);
                status = I::Inflate(&in, 1U, buffer, Work.Inflate, written);
            }
            else if (kind == 3U)
                status = I::DecodePng(F::View(v), buffer, Work, image);
            else if (kind < 6U)
                status = I::DecodeBmp(F::View(v), buffer, image);
            else if (kind == 6U)
                status = I::DecodeDib(F::View(v), buffer, image);
            else if (kind == 7U)
                status = I::DecodeIco(F::View(v), 9U, 9U, buffer, Work, image);
            else if (kind > 8U)
                status = I::DecodeDib(F::View(v), buffer, image);
            else
                status = I::DecodeCursorSet(F::View(v), buffer, Work, Set);
            bool guards{true};
            for (F::Size i{0U}; i < 16U; ++i)
                if (output[i] != 0xA5U || output[4112U + i] != 0xA5U)
                    guards = false;
            ASTERIA_EXPECT(
                test, "seeded mutation bounded output and publication", guards, written <= buffer.Count,
                kind < 3U || kind == 8U ||
                    (status == I::Status::Ok ? image.Valid() && image.Bytes <= buffer.Count : image.Pixels == nullptr),
                kind != 8U || (status == I::Status::Ok ? Set.Count <= I::MaximumCursorFrames : Set.Count == 0U));
        }
}
} // namespace
} // namespace Asteria::Test
int main()
{
    Asteria::TestSupport::TestContext test{"Imaging mutations seed 1951AC35"};
    Asteria::Test::Cases(test);
    return test.Finish();
}
