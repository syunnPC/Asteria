// SPDX-License-Identifier: Apache-2.0
// Known-answer tests of SHA-256 (FIPS 180-4 examples), HMAC-SHA256 (RFC 4231)
// and the HKDF-SHA256 steps built from it (RFC 5869 test case 1), plus the
// padding boundaries around one block and the independence of the digest from
// how the input is split across Update calls.

#include <cstddef>
#include <cstdio>

#include "Kernel/Security/Sha256.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt8;
using Kernel::Security::HmacSha256;
using Kernel::Security::Sha256;
using Kernel::Security::Sha256DigestBytes;

using Digest = UInt8[Sha256DigestBytes];

[[nodiscard]] UInt8 HexValue(const char digit) noexcept
{
    if ((digit >= '0') && (digit <= '9'))
    {
        return static_cast<UInt8>(digit - '0');
    }
    return static_cast<UInt8>(digit - 'a' + 10);
}

// Compares a digest with 64 lower-case hexadecimal digits.
[[nodiscard]] bool DigestIs(const Digest& digest, const char* const hex) noexcept
{
    for (Size index{0U}; index < Sha256DigestBytes; ++index)
    {
        const UInt8 expected = static_cast<UInt8>((HexValue(hex[index * 2U]) << 4U) | HexValue(hex[(index * 2U) + 1U]));
        if (digest[index] != expected)
        {
            return false;
        }
    }
    return hex[Sha256DigestBytes * 2U] == '\0';
}

[[nodiscard]] const UInt8* Bytes(const char* const text) noexcept
{
    return reinterpret_cast<const UInt8*>(text);
}

[[nodiscard]] Size Length(const char* const text) noexcept
{
    Size length{0U};
    while (text[length] != '\0')
    {
        ++length;
    }
    return length;
}

[[nodiscard]] bool HashIs(const UInt8* const bytes, const Size sizeBytes, const char* const hex) noexcept
{
    Sha256 hash{};
    hash.Update(bytes, sizeBytes);
    Digest digest{};
    hash.Finish(digest);
    return DigestIs(digest, hex);
}

[[nodiscard]] bool MacIs(const UInt8* const key, const Size keyBytes, const UInt8* const message,
                         const Size messageBytes, const char* const hex) noexcept
{
    HmacSha256 mac{key, keyBytes};
    mac.Update(message, messageBytes);
    Digest tag{};
    mac.Finish(tag);
    return DigestIs(tag, hex);
}

void TestFipsExamples(TestContext& runner) noexcept
{
    ASTERIA_EXPECT(runner, "the empty message",
                   HashIs(nullptr, 0U, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    ASTERIA_EXPECT(runner, "one-block message abc",
                   HashIs(Bytes("abc"), 3U, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    const char* const twoBlocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    ASTERIA_EXPECT(runner, "448-bit message whose padding needs a second block",
                   HashIs(Bytes(twoBlocks), Length(twoBlocks),
                          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    const char* const longer = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                               "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    ASTERIA_EXPECT(
        runner, "896-bit message",
        HashIs(Bytes(longer), Length(longer), "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"));

    // One million 'a' bytes, absorbed in uneven pieces.
    static constexpr Size Million{1'000'000U};
    UInt8 piece[997U]{};
    for (UInt8& value : piece)
    {
        value = static_cast<UInt8>('a');
    }
    Sha256 hash{};
    Size remaining{Million};
    while (remaining != 0U)
    {
        const Size count = remaining < sizeof(piece) ? remaining : sizeof(piece);
        hash.Update(piece, count);
        remaining -= count;
    }
    Digest digest{};
    hash.Finish(digest);
    ASTERIA_EXPECT(runner, "one million repetitions of a",
                   DigestIs(digest, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));

    // Finish restarts the object: the next message is hashed from scratch.
    hash.Update(Bytes("abc"), 3U);
    hash.Finish(digest);
    ASTERIA_EXPECT(runner, "Finish restarts the hash for the next message",
                   DigestIs(digest, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

void TestPaddingBoundaries(TestContext& runner) noexcept
{
    struct Boundary
    {
        Size Length;
        const char* Digest;
    };
    // Message byte i is (7i + 3) mod 256; the digests come from an
    // independent SHA-256 implementation.
    static constexpr Boundary Boundaries[]{
        {55U, "e7313d333c272e639f790978283f9eb392e843d0f29b7016828bb1daa4aac70b"},
        {56U, "4324d65f3c103567f5589c710bc08f8523f929a9272e3af36fc968e52abc6c27"},
        {57U, "35df609437dcfea3279283ab79fd554e2bf78f8f7ae2de532d8ee300b09e8f73"},
        {63U, "81c80242132f230c3bd41b3e63bbcff16107339549214a99614ff26664625055"},
        {64U, "39e3d7b6b5d075d37d053ad89b24b41bef4f3c29760c84447cab3f3be1882241"},
        {65U, "aacca6ff74fdbb296d165a45cecfa04e5127bc008770fbbdd48006f2d2fae95e"},
        {119U, "9ce7368e4daf32341631b492e80359dc9f594b48453cd0dd5bf0b19279cc177e"},
        {120U, "7836b787757e95e58b3ca5aec90b1b004e8deba1e50e9675af9cabf1a13a04b5"},
    };
    UInt8 message[120U]{};
    for (Size index{0U}; index < sizeof(message); ++index)
    {
        message[index] = static_cast<UInt8>((index * 7U) + 3U);
    }
    for (const Boundary& boundary : Boundaries)
    {
        if (!ASTERIA_EXPECT(runner, "a message at a padding boundary",
                            HashIs(message, boundary.Length, boundary.Digest)))
        {
            static_cast<void>(std::fprintf(stderr, "  length %zu\n", boundary.Length));
        }

        // Every split into two Update calls gives the same digest.
        bool splitsAgree{true};
        for (Size split{0U}; split <= boundary.Length; ++split)
        {
            Sha256 hash{};
            hash.Update(message, split);
            hash.Update(message + split, boundary.Length - split);
            Digest digest{};
            hash.Finish(digest);
            splitsAgree = splitsAgree && DigestIs(digest, boundary.Digest);
        }
        ASTERIA_EXPECT(runner, "the digest does not depend on how the input is split", splitsAgree);
    }
}

void TestRfc4231(TestContext& runner) noexcept
{
    UInt8 key20[20U]{};
    UInt8 message50[50U]{};
    UInt8 key131[131U]{};
    for (UInt8& value : key131)
    {
        value = 0xAAU;
    }

    for (UInt8& value : key20)
    {
        value = 0x0BU;
    }
    ASTERIA_EXPECT(runner, "RFC 4231 test case 1",
                   MacIs(key20, sizeof(key20), Bytes("Hi There"), 8U,
                         "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));

    ASTERIA_EXPECT(runner, "RFC 4231 test case 2: a key shorter than the output",
                   MacIs(Bytes("Jefe"), 4U, Bytes("what do ya want for nothing?"), 28U,
                         "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));

    for (UInt8& value : key20)
    {
        value = 0xAAU;
    }
    for (UInt8& value : message50)
    {
        value = 0xDDU;
    }
    ASTERIA_EXPECT(runner, "RFC 4231 test case 3",
                   MacIs(key20, sizeof(key20), message50, sizeof(message50),
                         "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe"));

    UInt8 key25[25U]{};
    for (Size index{0U}; index < sizeof(key25); ++index)
    {
        key25[index] = static_cast<UInt8>(index + 1U);
    }
    for (UInt8& value : message50)
    {
        value = 0xCDU;
    }
    ASTERIA_EXPECT(runner, "RFC 4231 test case 4",
                   MacIs(key25, sizeof(key25), message50, sizeof(message50),
                         "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b"));

    // A key of exactly one block is used as it is, one byte more is hashed
    // first (independent implementation).
    UInt8 key65[65U]{};
    for (Size index{0U}; index < sizeof(key65); ++index)
    {
        key65[index] = static_cast<UInt8>(index);
    }
    const char* const oneBlock = "Asteria HMAC with a key of exactly one block";
    ASTERIA_EXPECT(runner, "a key of exactly one block is not hashed",
                   MacIs(key65, 64U, Bytes(oneBlock), Length(oneBlock),
                         "4b8bf8bcb3831b26fc78a0de63634085c82865045e2c4154eceb18a4a8182424"),
                   MacIs(key65, 65U, Bytes(oneBlock), Length(oneBlock),
                         "aef785c8047092015a22dfd02c61e81c06aaa431cd6e2b3288d24bcefde8c299"));

    const char* const hashKeyFirst = "Test Using Larger Than Block-Size Key - Hash Key First";
    ASTERIA_EXPECT(runner, "RFC 4231 test case 6: a key longer than one block is hashed first",
                   MacIs(key131, sizeof(key131), Bytes(hashKeyFirst), Length(hashKeyFirst),
                         "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));

    const char* const longData = "This is a test using a larger than block-size key and a larger than "
                                 "block-size data. The key needs to be hashed before being used by the HMAC "
                                 "algorithm.";
    ASTERIA_EXPECT(runner, "RFC 4231 test case 7: a key and a message longer than one block",
                   MacIs(key131, sizeof(key131), Bytes(longData), Length(longData),
                         "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2"));
}

void TestRfc5869(TestContext& runner) noexcept
{
    UInt8 inputKeyingMaterial[22U]{};
    for (UInt8& value : inputKeyingMaterial)
    {
        value = 0x0BU;
    }
    UInt8 salt[13U]{};
    for (Size index{0U}; index < sizeof(salt); ++index)
    {
        salt[index] = static_cast<UInt8>(index);
    }
    UInt8 info[11U]{};
    for (Size index{0U}; index < 10U; ++index)
    {
        info[index] = static_cast<UInt8>(0xF0U + index);
    }
    info[10U] = 0x01U;

    HmacSha256 extract{salt, sizeof(salt)};
    extract.Update(inputKeyingMaterial, sizeof(inputKeyingMaterial));
    Digest pseudorandomKey{};
    extract.Finish(pseudorandomKey);
    ASTERIA_EXPECT(runner, "RFC 5869 test case 1: HKDF-Extract",
                   DigestIs(pseudorandomKey, "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5"));

    HmacSha256 expand{pseudorandomKey, sizeof(pseudorandomKey)};
    expand.Update(info, sizeof(info));
    Digest firstBlock{};
    expand.Finish(firstBlock);
    ASTERIA_EXPECT(runner, "RFC 5869 test case 1: the first HKDF-Expand block",
                   DigestIs(firstBlock, "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"));
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"Sha256Tests"};
    Asteria::Test::TestFipsExamples(runner);
    Asteria::Test::TestPaddingBoundaries(runner);
    Asteria::Test::TestRfc4231(runner);
    Asteria::Test::TestRfc5869(runner);
    return runner.Finish();
}
