// SPDX-License-Identifier: Apache-2.0 AND LicenseRef-IETF-Code-2005
// Copyright (C) The Internet Society 2005. This version of this test vector set is part of RFC 4231;
// see the RFC itself for full legal notices: https://www.rfc-editor.org/rfc/rfc4231.txt.
// Only the test vectors carry these terms; project-authored test logic is Apache-2.0.
// See LICENSES/LicenseRef-IETF-Code-2005.txt.
#include "User/Crypto/Vectors.hpp"
int main() noexcept
{
    using CryptoTest::Cleared;
    using Asteria::User::Crypto::HashAlgorithm;
    using CryptoTest::Hex;
    using Asteria::User::Crypto::Hmac;
    using CryptoTest::IsHex;
    using CryptoTest::Output;
    using Asteria::User::Crypto::Result;
    using CryptoTest::Work;
    Asteria::TestSupport::TestContext context{"CryptoHmac"};
    {
        const Hex key{"0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b"};
        const Hex message{"4869205468657265"};
        ASTERIA_EXPECT(context, "RFC 4231 case 1 SHA-256",
                       Hmac(Work, HashAlgorithm::Sha256, key.View(), message.View(), {Output, 32U}) == Result::Success,
                       IsHex(Output, 32U, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"),
                       Cleared());
        ASTERIA_EXPECT(context, "RFC 4231 case 1 SHA-512",
                       Hmac(Work, HashAlgorithm::Sha512, key.View(), message.View(), {Output, 64U}) == Result::Success,
                       IsHex(Output, 64U,
                             "87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cdedaa833b7d6b8a702038b274ea"
                             "ea3f4e4be9d914eeb61f1702e696c203a126854"),
                       Cleared());
    }
    {
        const Hex key{"4a656665"};
        const Hex message{"7768617420646f2079612077616e7420666f72206e6f7468696e673f"};
        ASTERIA_EXPECT(context, "RFC 4231 case 2 SHA-256",
                       Hmac(Work, HashAlgorithm::Sha256, key.View(), message.View(), {Output, 32U}) == Result::Success,
                       IsHex(Output, 32U, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"),
                       Cleared());
        ASTERIA_EXPECT(context, "RFC 4231 case 2 SHA-512",
                       Hmac(Work, HashAlgorithm::Sha512, key.View(), message.View(), {Output, 64U}) == Result::Success,
                       IsHex(Output, 64U,
                             "164b7a7bfcf819e2e395fbe73b56e0a387bd64222e831fd610270cd7ea2505549758bf75c05a994a6d034f65f"
                             "8f0e6fdcaeab1a34d4a6b4b636e070a38bce737"),
                       Cleared());
    }
    {
        const Hex key{"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
        const Hex message{
            "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"};
        ASTERIA_EXPECT(context, "RFC 4231 case 3 SHA-256",
                       Hmac(Work, HashAlgorithm::Sha256, key.View(), message.View(), {Output, 32U}) == Result::Success,
                       IsHex(Output, 32U, "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe"),
                       Cleared());
        ASTERIA_EXPECT(context, "RFC 4231 case 3 SHA-512",
                       Hmac(Work, HashAlgorithm::Sha512, key.View(), message.View(), {Output, 64U}) == Result::Success,
                       IsHex(Output, 64U,
                             "fa73b0089d56a284efb0f0756c890be9b1b5dbdd8ee81a3655f83e33b2279d39bf3e848279a722c806b485a47"
                             "e67c807b946a337bee8942674278859e13292fb"),
                       Cleared());
    }
    {
        const Hex key{"0102030405060708090a0b0c0d0e0f10111213141516171819"};
        const Hex message{
            "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd"};
        ASTERIA_EXPECT(context, "RFC 4231 case 4 SHA-256",
                       Hmac(Work, HashAlgorithm::Sha256, key.View(), message.View(), {Output, 32U}) == Result::Success,
                       IsHex(Output, 32U, "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b"),
                       Cleared());
        ASTERIA_EXPECT(context, "RFC 4231 case 4 SHA-512",
                       Hmac(Work, HashAlgorithm::Sha512, key.View(), message.View(), {Output, 64U}) == Result::Success,
                       IsHex(Output, 64U,
                             "b0ba465637458c6990e5a8c5f61d4af7e576d97ff94b872de76f8050361ee3dba91ca5c11aa25eb4d679275cc"
                             "5788063a5f19741120c4f2de2adebeb10a298dd"),
                       Cleared());
    }
    {
        const Hex key{"0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c"};
        const Hex message{"546573742057697468205472756e636174696f6e"};
        ASTERIA_EXPECT(context, "RFC 4231 case 5 SHA-256",
                       Hmac(Work, HashAlgorithm::Sha256, key.View(), message.View(), {Output, 32U}) == Result::Success,
                       IsHex(Output, 16U, "a3b6167473100ee06e0c796c2955552b"), Cleared());
        ASTERIA_EXPECT(context, "RFC 4231 case 5 SHA-512",
                       Hmac(Work, HashAlgorithm::Sha512, key.View(), message.View(), {Output, 64U}) == Result::Success,
                       IsHex(Output, 16U, "415fad6271580a531d4179bc891d87a6"), Cleared());
    }
    {
        const Hex key{"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
        const Hex message{
            "54657374205573696e67204c6172676572205468616e20426c6f636b2d53697a65204b6579202d2048617368204b657920"
            "4669727374"};
        ASTERIA_EXPECT(context, "RFC 4231 case 6 SHA-256",
                       Hmac(Work, HashAlgorithm::Sha256, key.View(), message.View(), {Output, 32U}) == Result::Success,
                       IsHex(Output, 32U, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"),
                       Cleared());
        ASTERIA_EXPECT(context, "RFC 4231 case 6 SHA-512",
                       Hmac(Work, HashAlgorithm::Sha512, key.View(), message.View(), {Output, 64U}) == Result::Success,
                       IsHex(Output, 64U,
                             "80b24263c7c1a3ebb71493c1dd7be8b49b46d1f41b4aeec1121b013783f8f3526b56d037e05f2598bd0fd2215"
                             "d6a1e5295e64f73f63f0aec8b915a985d786598"),
                       Cleared());
    }
    {
        const Hex key{"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
        const Hex message{
            "5468697320697320612074657374207573696e672061206c6172676572207468616e20626c6f636b2d73697a65206b65792061"
            "6e642061206c6172676572207468616e20626c6f636b2d73697a6520646174612e20546865206b6579206e6565647320746f20"
            "626520686173686564206265666f7265206265696e6720757365642062792074686520484d414320616c676f726974686d2e"};
        ASTERIA_EXPECT(context, "RFC 4231 case 7 SHA-256",
                       Hmac(Work, HashAlgorithm::Sha256, key.View(), message.View(), {Output, 32U}) == Result::Success,
                       IsHex(Output, 32U, "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2"),
                       Cleared());
        ASTERIA_EXPECT(context, "RFC 4231 case 7 SHA-512",
                       Hmac(Work, HashAlgorithm::Sha512, key.View(), message.View(), {Output, 64U}) == Result::Success,
                       IsHex(Output, 64U,
                             "e37b6a775dc87dbaa4dfa9f96e5e3ffddebd71f8867289865df5a32d20cdc944b6022cac3c4982b10d5eeb55c"
                             "3e4de15134676fb6de0446065c97440fa8c6a58"),
                       Cleared());
    }
    return context.Finish();
}
