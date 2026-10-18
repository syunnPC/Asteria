// SPDX-License-Identifier: Apache-2.0
#include "User/Imaging/Fixture.hpp"
namespace Asteria::Test
{
namespace
{
namespace F = ImagingFixture;
namespace I = User::Imaging;
using TestSupport::TestContext;
static I::InflateReader Reader;
void Cases(TestContext& test)
{
    const F::V raw{0U, 1U, 255U, 128U, 65U, 65U, 65U};
    for (F::UInt32 mode{0U}; mode < 3U; ++mode)
    {
        const F::V z{F::Zlib(raw, mode)};
        std::vector<I::Bytes> segments;
        for (const auto& b : z)
            segments.push_back({&b, 1U});
        F::V out(raw.size());
        F::Size written{};
        ASTERIA_EXPECT(test, "stored/fixed/dynamic across byte segments",
                       I::Inflate(segments.data(), static_cast<F::UInt32>(segments.size()), {out.data(), out.size()},
                                  Reader, written) == I::Status::Ok,
                       out == raw, written == raw.size());
        for (F::Size n{0U}; n < z.size(); ++n)
        {
            const I::Bytes in{z.data(), n};
            ASTERIA_EXPECT(test, "all zlib truncations refused",
                           I::Inflate(&in, 1U, {out.data(), out.size()}, Reader, written) != I::Status::Ok);
        }
        auto bad = z;
        if (!bad.empty())
            bad.back() ^= 1U;
        const I::Bytes in{F::View(bad)};
        ASTERIA_EXPECT(test, "bad Adler refused",
                       I::Inflate(&in, 1U, {out.data(), out.size()}, Reader, written) == I::Status::Adler);
    }
    // A stored block seeds a full window; next compressed block matches its
    // first bytes at maximum distance, then overlapping copies at distance 1.
    for (bool dynamic : {false, true})
    {
        F::V seed(I::WindowBytes);
        for (F::Size i{0U}; i < seed.size(); ++i)
            seed[i] = static_cast<F::UInt8>(i * 7U);
        F::V stream{F::Deflate(seed, 0U)};
        stream[0] = 0U;
        F::Bits b;
        b.Put(dynamic ? 5U : 3U, 3U);
        const F::V lit{F::LiteralLengths(dynamic)}, dist(32U, 5U);
        if (dynamic)
            F::DynamicHeader(b, lit, dist);
        b.Code(257U, lit);
        b.Code(29U, dist);
        b.Put(8191U, 13U); // length 3, distance 32768
        b.Code(285U, lit);
        b.Code(0U, dist); // length 258, overlapping distance 1
        b.Code(256U, lit);
        stream.insert(stream.end(), b.Data.begin(), b.Data.end());
        F::V expected{seed};
        expected.insert(expected.end(), seed.begin(), seed.begin() + 3);
        expected.resize(expected.size() + 258U, seed[2]);
        F::V out(expected.size());
        const I::Bytes input{F::View(stream)};
        F::Size written{};
        ASTERIA_EXPECT(test, "length 3/258 distance 32768 across blocks",
                       I::Inflate(&input, 1U, {out.data(), out.size()}, Reader, written, false) == I::Status::Ok,
                       out == expected);
    }
    F::Bits joined;
    const auto fixed = F::LiteralLengths(false);
    const auto dyn = F::LiteralLengths(true);
    joined.Put(2U, 3U);
    joined.Code(65U, fixed);
    joined.Code(256U, fixed);
    joined.Put(5U, 3U);
    F::DynamicHeader(joined, dyn, F::V(32U, 5U), true, true);
    joined.Code(66U, dyn);
    joined.Code(256U, dyn);
    F::UInt8 pair[2]{};
    const auto joinedView = F::View(joined.Data);
    F::Size joinedWritten{};
    ASTERIA_EXPECT(test, "non-byte block boundary and repeat 17",
                   I::Inflate(&joinedView, 1U, {pair, sizeof(pair)}, Reader, joinedWritten, false) == I::Status::Ok,
                   pair[0] == 65U, pair[1] == 66U);
    const F::V large(65536U, 37U);
    const F::V compressed{F::Zlib(large)};
    F::V out(large.size());
    F::Size written{};
    const I::Bytes input{F::View(compressed)};
    ASTERIA_EXPECT(test, "stored length 65535 and next block",
                   I::Inflate(&input, 1U, {out.data(), out.size()}, Reader, written) == I::Status::Ok, out == large);
    auto refusal = [&](const F::V& bytes, I::Status expected, bool zlib = false)
    {
        const I::Bytes v{F::View(bytes)};
        ASTERIA_EXPECT(test, "typed DEFLATE refusal",
                       I::Inflate(&v, 1U, {out.data(), out.size()}, Reader, written, zlib) == expected);
    };
    refusal({7U}, I::Status::Header);
    refusal({1U, 1U, 0U, 0U, 0U, 9U}, I::Status::Header);
    refusal({0x78U, 0U}, I::Status::Header, true);
    refusal({0x79U, 0x18U}, I::Status::Header, true);
    refusal({0x88U, 0U}, I::Status::Header, true);
    refusal({0x78U, 0x20U}, I::Status::Unsupported, true);
    F::Bits distance;
    distance.Put(3U, 3U);
    const F::V lit{F::LiteralLengths(false)};
    distance.Code(257U, lit);
    distance.Code(0U, F::V(32U, 5U));
    refusal(distance.Data, I::Status::Distance);
    F::Bits reserved;
    reserved.Put(3U, 3U);
    reserved.Code(286U, lit);
    refusal(reserved.Data, I::Status::Huffman);
    F::Bits rd;
    rd.Put(3U, 3U);
    rd.Code(65U, lit);
    rd.Code(257U, lit);
    rd.Code(30U, F::V(32U, 5U));
    refusal(rd.Data, I::Status::Distance);
    for (F::UInt32 n : {0U, 1U, 3U})
    {
        F::Bits invalid;
        invalid.Put(5U, 3U);
        invalid.Put(0U, 5U);
        invalid.Put(0U, 5U);
        invalid.Put(0U, 4U);
        for (F::UInt32 i{0U}; i < 4U; ++i)
            invalid.Put(i < n ? 1U : 0U, 3U);
        if (n != 1U)
            refusal(invalid.Data, I::Status::Huffman);
        else
            refusal(invalid.Data, I::Status::Huffman); // incomplete code-length alphabet
    }
    // Valid complete code-length alphabet; malformed literal trees.
    for (bool over : {false, true})
    {
        F::Bits invalid;
        invalid.Put(5U, 3U);
        F::V lengths(257U, 0U);
        lengths[0] = over ? 1U : 5U;
        lengths[1] = over ? 1U : 0U;
        lengths[256] = over ? 1U : 5U;
        F::DynamicHeader(invalid, lengths, F::V{0U});
        refusal(invalid.Data, I::Status::Huffman);
    }
    F::Bits noEnd;
    noEnd.Put(5U, 3U);
    F::V noEndLengths(257U, 0U);
    noEndLengths[0] = 1U;
    noEndLengths[1] = 1U;
    F::DynamicHeader(noEnd, noEndLengths, F::V{0U});
    refusal(noEnd.Data, I::Status::Huffman);
    F::Bits single;
    single.Put(5U, 3U);
    F::V one(257U, 0U);
    one[256] = 1U;
    F::DynamicHeader(single, one, F::V{0U});
    single.Code(256U, one);
    refusal(single.Data, I::Status::Ok); // specified singleton and absent distances
    F::V trailing{F::Zlib({})};
    trailing.push_back(0U);
    refusal(trailing, I::Status::SizeMismatch, true);
    const I::Bytes empty{nullptr, 0U}; // only its zero length is used below
    ASTERIA_EXPECT(test, "invalid buffers typed", Reader.Begin(nullptr, 1U) == I::Status::InvalidBuffer,
                   Reader.Begin(&empty, I::MaximumChunks + 1U) == I::Status::Limit);
    auto z = F::Zlib(raw);
    const I::Bytes zview{F::View(z)};
    ASTERIA_EXPECT(test, "short output bounded",
                   I::Inflate(&zview, 1U, {out.data(), 2U}, Reader, written) == I::Status::Limit, written == 2U);
    const F::V known{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    ASTERIA_EXPECT(test, "CRC polynomial check vector", I::Crc32(F::View(known)) == 0xCBF43926U,
                   F::Crc(known.data(), known.size()) == 0xCBF43926U);
}
} // namespace
} // namespace Asteria::Test
int main()
{
    Asteria::TestSupport::TestContext test{"Imaging Inflate"};
    Asteria::Test::Cases(test);
    return test.Finish();
}
