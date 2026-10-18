// SPDX-License-Identifier: Apache-2.0 AND BSD-3-Clause
// RFC 7748 test vectors only; project-authored test logic is Apache-2.0.
// Copyright (c) 2016 IETF Trust and the persons identified as authors of the code. All rights reserved.
// Redistribution and use in source and binary forms, with or without modification, is permitted pursuant to,
// and subject to the license terms contained in, the Revised BSD License set forth in Section 4.c of the
// IETF Trust's Legal Provisions Relating to IETF Documents (https://trustee.ietf.org/license-info).
// See LICENSES/BSD-3-Clause.txt and https://www.rfc-editor.org/rfc/rfc7748.txt.
#include "User/Crypto/Vectors.hpp"
int main() noexcept
{
    using CryptoTest::Cleared;
    using Asteria::User::Crypto::Equal;
    using CryptoTest::Hex;
    using CryptoTest::IsHex;
    using CryptoTest::Output;
    using Asteria::User::Crypto::Result;
    using Asteria::User::Crypto::Size;
    using Asteria::User::Crypto::UInt8;
    using CryptoTest::Work;
    using Asteria::User::Crypto::X25519;
    Asteria::TestSupport::TestContext context{"X25519"};
    // RFC 7748 §5.2 both X25519 vectors, §6.1 Alice/Bob exchange.
    const Hex scalar1{"a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4"};
    const Hex u1{"e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c"};
    const Hex scalar2{"4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d"};
    const Hex u2{"e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493"};
    ASTERIA_EXPECT(context, "X25519 vector1", X25519(Work, scalar1.View(), u1.View(), {Output, 32U}) == Result::Success,
                   IsHex(Output, 32U, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"), Cleared());
    ASTERIA_EXPECT(context, "X25519 vector2", X25519(Work, scalar2.View(), u2.View(), {Output, 32U}) == Result::Success,
                   IsHex(Output, 32U, "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957"));
    const Hex alice{"77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a"};
    const Hex bob{"5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb"};
    UInt8 base[32U]{9U};
    UInt8 alicePk[32U]{};
    UInt8 bobPk[32U]{};
    ASTERIA_EXPECT(context, "Alice and Bob public keys",
                   X25519(Work, alice.View(), {base, 32U}, {alicePk, 32U}) == Result::Success,
                   IsHex(alicePk, 32U, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"),
                   X25519(Work, bob.View(), {base, 32U}, {bobPk, 32U}) == Result::Success,
                   IsHex(bobPk, 32U, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"));
    ASTERIA_EXPECT(context, "shared secret both directions",
                   X25519(Work, alice.View(), {bobPk, 32U}, {Output, 32U}) == Result::Success,
                   IsHex(Output, 32U, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"),
                   X25519(Work, bob.View(), {alicePk, 32U}, {Output, 32U}) == Result::Success,
                   IsHex(Output, 32U, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"));
    UInt8 zero[32U]{};
    std::memset(Output, 0xa5, 32U);
    ASTERIA_EXPECT(context, "all-zero rejection preserves output",
                   X25519(Work, alice.View(), {zero, 32U}, {Output, 32U}) == Result::AuthenticationFailed,
                   Output[0] == 0xa5U, Cleared());
    // RFC 7748 noncanonical coordinate p+9 maps to coordinate 9.
    Hex noncanonical{"f6ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f"};
    ASTERIA_EXPECT(context, "noncanonical and high bit masked",
                   X25519(Work, alice.View(), noncanonical.View(), {Output, 32U}) == Result::Success,
                   Equal({Output, 32U}, {alicePk, 32U}));
    noncanonical.Data[31] |= 128U;
    ASTERIA_EXPECT(context, "ignored high bit",
                   X25519(Work, alice.View(), noncanonical.View(), {Output, 32U}) == Result::Success,
                   Equal({Output, 32U}, {alicePk, 32U}));
    // RFC 7748 §5.2 iterated X25519, k <- X25519(k,u), u <- old k.
    UInt8 iterationKey[32U]{9U};
    UInt8 iterationU[32U]{9U};
    UInt8 previous[32U]{};
    for (Size iteration = 0U; iteration < 1000U; ++iteration)
    {
        std::memcpy(previous, iterationKey, 32U);
        ASTERIA_EXPECT(context, "iterated ladder",
                       X25519(Work, {iterationKey, 32U}, {iterationU, 32U}, {Output, 32U}) == Result::Success);
        std::memcpy(iterationKey, Output, 32U);
        std::memcpy(iterationU, previous, 32U);
        if (iteration == 0U)
        {
            ASTERIA_EXPECT(
                context, "RFC one iteration",
                IsHex(iterationKey, 32U, "422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079"));
        }
    }
    ASTERIA_EXPECT(context, "RFC 1000 iterations",
                   IsHex(iterationKey, 32U, "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51"));
    return context.Finish();
}
