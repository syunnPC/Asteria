// SPDX-License-Identifier: Apache-2.0 AND BSD-3-Clause
// RFC 8439 test vectors only; project-authored test logic is Apache-2.0.
// Copyright (c) 2018 IETF Trust and the persons identified as authors of the code. All rights reserved.
// Redistribution and use in source and binary forms, with or without modification, is permitted pursuant to,
// and subject to the license terms contained in, the Revised BSD License set forth in Section 4.c of the
// IETF Trust's Legal Provisions Relating to IETF Documents (https://trustee.ietf.org/license-info).
// See LICENSES/BSD-3-Clause.txt and https://www.rfc-editor.org/rfc/rfc8439.txt.
#include "User/Crypto/Vectors.hpp"
#include "User/Crypto/PolyVectors.hpp"
int main() noexcept
{
    using Asteria::User::Crypto::AeadAlgorithm;
    using Asteria::User::Crypto::AesEncrypt;
    using Asteria::User::Crypto::Bytes;
    using Asteria::User::Crypto::ChaCha20;
    using CryptoTest::Cleared;
    using CryptoTest::Empty;
    using Asteria::User::Crypto::Equal;
    using CryptoTest::Hex;
    using CryptoTest::IsHex;
    using Asteria::User::Crypto::Open;
    using CryptoTest::Output;
    using Asteria::User::Crypto::Poly1305;
    using Asteria::User::Crypto::Result;
    using Asteria::User::Crypto::Seal;
    using Asteria::User::Crypto::Size;
    using CryptoTest::Text;
    using Asteria::User::Crypto::UInt8;
    using CryptoTest::Work;
    Asteria::TestSupport::TestContext context{"CryptoSymmetric"};
    // FIPS 197 (2001) Appendix C.1/C.3 (upd1 points to maintained examples).
    const Hex block{"00112233445566778899aabbccddeeff"};
    const Hex key128{"000102030405060708090a0b0c0d0e0f"};
    const Hex key256{"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"};
    ASTERIA_EXPECT(context, "AES128 FIPS example",
                   AesEncrypt(Work, key128.View(), block.View(), {Output, 16U}) == Result::Success,
                   IsHex(Output, 16U, "69c4e0d86a7b0430d8cdb78070b4c55a"), Cleared());
    ASTERIA_EXPECT(context, "AES256 FIPS example",
                   AesEncrypt(Work, key256.View(), block.View(), {Output, 16U}) == Result::Success,
                   IsHex(Output, 16U, "8ea2b7ca516745bfeafc49904b496089"));
    // NIST GCM specification (McGrew/Viega, May 2005), Appendix B cases 1–3, 14–16.
    UInt8 zeros[32U]{};
    UInt8 nonce[12U]{};
    UInt8 tag[16U]{};
    UInt8 plain[256U]{};
    ASTERIA_EXPECT(context, "GCM128 empty",
                   Seal(Work, AeadAlgorithm::AesGcm, {zeros, 16U}, {nonce, 12U}, Empty, Empty, {Output, 0U},
                        {tag, 16U}) == Result::Success,
                   IsHex(tag, 16U, "58e2fccefa7e3061367f1d57a4e7455a"));
    ASTERIA_EXPECT(context, "GCM128 block",
                   Seal(Work, AeadAlgorithm::AesGcm, {zeros, 16U}, {nonce, 12U}, Empty, {zeros, 16U}, {Output, 16U},
                        {tag, 16U}) == Result::Success,
                   IsHex(Output, 16U, "0388dace60b6a392f328c2b971b2fe78"),
                   IsHex(tag, 16U, "ab6e47d42cec13bdf53a67b21257bddf"));
    ASTERIA_EXPECT(context, "GCM256 empty",
                   Seal(Work, AeadAlgorithm::AesGcm, {zeros, 32U}, {nonce, 12U}, Empty, Empty, {Output, 0U},
                        {tag, 16U}) == Result::Success,
                   IsHex(tag, 16U, "530f8afbc74536b9a963b4f1c4cb738b"));
    ASTERIA_EXPECT(context, "GCM256 block",
                   Seal(Work, AeadAlgorithm::AesGcm, {zeros, 32U}, {nonce, 12U}, Empty, {zeros, 16U}, {Output, 16U},
                        {tag, 16U}) == Result::Success,
                   IsHex(Output, 16U, "cea7403d4d606b6e074ec5d3baf39d18"),
                   IsHex(tag, 16U, "d0d1c8a799996bf0265b98b5d48ab919"));
    const Hex gkey{"feffe9928665731c6d6a8f9467308308"};
    const Hex gnonce{"cafebabefacedbaddecaf888"};
    const Hex gaad{"feedfacedeadbeeffeedfacedeadbeefabaddad2"};
    const Hex gplain{"d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b"
                     "16aedf5aa0de657ba637b39"};
    ASTERIA_EXPECT(context, "GCM partial AAD and payload",
                   Seal(Work, AeadAlgorithm::AesGcm, gkey.View(), gnonce.View(), gaad.View(), gplain.View(),
                        {Output, gplain.Length}, {tag, 16U}) == Result::Success,
                   IsHex(Output, gplain.Length,
                         "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84a"
                         "a051ba30b396a0aac973d58e091"),
                   IsHex(tag, 16U, "5bc94fbc3221a5db94fae95ae7121a47"),
                   Open(Work, AeadAlgorithm::AesGcm, gkey.View(), gnonce.View(), gaad.View(), {Output, gplain.Length},
                        {tag, 16U}, {plain, gplain.Length}) == Result::Success,
                   Equal({plain, gplain.Length}, gplain.View()));
    tag[0] ^= 1U;
    std::memset(plain, 0xa5, sizeof(plain));
    ASTERIA_EXPECT(context, "GCM forged tag preserves output",
                   Open(Work, AeadAlgorithm::AesGcm, gkey.View(), gnonce.View(), gaad.View(), {Output, gplain.Length},
                        {tag, 16U}, {plain, gplain.Length}) == Result::AuthenticationFailed,
                   plain[0] == 0xa5U, Cleared());
    // RFC 8439 §2.3.2 ChaCha block, §2.5.2 Poly1305, §2.8.2 AEAD.
    const Hex cn{"000000090000004a00000000"};
    UInt8 emptyBlock[64U]{};
    ASTERIA_EXPECT(context, "ChaCha20 block",
                   ChaCha20(Work, key256.View(), cn.View(), 1U, {emptyBlock, 64U}, {Output, 64U}) == Result::Success,
                   IsHex(Output, 64U,
                         "10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4ed2826446079faa0914c2d705d98b0"
                         "2a2b5129cd1de164eb9cbd083e8a2503c4e"));
    const Hex pk{"85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b"};
    ASTERIA_EXPECT(context, "Poly1305 RFC example",
                   Poly1305(Work, pk.View(), Text("Cryptographic Forum Research Group"), {tag, 16U}) == Result::Success,
                   IsHex(tag, 16U, "a8061dc1305136c6c22b8baf0c0127a9"));
    const Hex ak{"808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f"};
    const Hex an{"070000004041424344454647"};
    const Hex aa{"50515253c0c1c2c3c4c5c6c7"};
    const Bytes ap = Text("Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, "
                          "sunscreen would be it.");
    ASTERIA_EXPECT(context, "ChaChaPoly AEAD",
                   Seal(Work, AeadAlgorithm::ChaCha20Poly1305, ak.View(), an.View(), aa.View(), ap, {Output, ap.Length},
                        {tag, 16U}) == Result::Success,
                   IsHex(Output, ap.Length,
                         "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da927"
                         "28b1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b48"
                         "31d7bc3ff4def08e4b7a9de576d26586cec64b6116"),
                   IsHex(tag, 16U, "1ae10b594f09e26a7e902ecbd0600691"),
                   Open(Work, AeadAlgorithm::ChaCha20Poly1305, ak.View(), an.View(), aa.View(), {Output, ap.Length},
                        {tag, 16U}, {plain, ap.Length}) == Result::Success,
                   Equal({plain, ap.Length}, ap));
    for (Size bit = 0U; bit < 128U; ++bit)
    {
        tag[bit / 8U] ^= static_cast<UInt8>(1U << (bit % 8U));
        std::memset(plain, 0xa5, sizeof(plain));
        ASTERIA_EXPECT(context, "every tag-bit forgery rejected",
                       Open(Work, AeadAlgorithm::ChaCha20Poly1305, ak.View(), an.View(), aa.View(), {Output, ap.Length},
                            {tag, 16U}, {plain, ap.Length}) == Result::AuthenticationFailed,
                       plain[0] == 0xa5U, Cleared());
        tag[bit / 8U] ^= static_cast<UInt8>(1U << (bit % 8U));
    }
    ASTERIA_EXPECT(context, "counter exhaustion",
                   ChaCha20(Work, key256.View(), cn.View(), 0xffffffffU, {plain, 65U}, {Output, 65U}) ==
                       Result::BufferSize);
    for (const auto& vector : PolyVectors)
    {
        const Hex vectorKey{vector.Key};
        const Hex vectorData{vector.Data};
        const Hex vectorTag{vector.Tag};
        ASTERIA_EXPECT(context, "RFC Poly1305 appendix",
                       Poly1305(Work, vectorKey.View(), vectorData.View(), {tag, 16U}) == Result::Success,
                       Equal({tag, 16U}, vectorTag.View()), Cleared());
    }
    return context.Finish();
}
