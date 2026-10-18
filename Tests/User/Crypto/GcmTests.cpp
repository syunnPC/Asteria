// SPDX-License-Identifier: Apache-2.0
#include "User/Crypto/Vectors.hpp"
#include "User/Crypto/GcmVectors.hpp"
int main() noexcept
{
    using Asteria::User::Crypto::AeadAlgorithm;
    using CryptoTest::Cleared;
    using Asteria::User::Crypto::Equal;
    using CryptoTest::Hex;
    using Asteria::User::Crypto::Open;
    using CryptoTest::Output;
    using Asteria::User::Crypto::Result;
    using Asteria::User::Crypto::Seal;
    using Asteria::User::Crypto::UInt8;
    using CryptoTest::Work;
    Asteria::TestSupport::TestContext context{"GcmCavp"};
    for (const auto& v : GcmVectors)
    {
        const Hex key{v.Key};
        const Hex nonce{v.IV};
        const Hex aad{v.AAD};
        const Hex cipher{v.CT};
        const Hex plain{v.PT};
        const Hex tag{v.Tag};
        std::memset(Output, 0xa5, sizeof(Output));
        const auto opened = Open(Work, AeadAlgorithm::AesGcm, key.View(), nonce.View(), aad.View(), cipher.View(),
                                 tag.View(), {Output, cipher.Length});
        if (v.Fail)
        {
            bool unchanged = true;
            for (auto b : Output)
            {
                unchanged = unchanged && (b == 0xa5U);
            }
            ASTERIA_EXPECT(context, "CAVP authentication failure", opened == Result::AuthenticationFailed, unchanged,
                           Cleared());
        }
        else
        {
            ASTERIA_EXPECT(context, "CAVP open", opened == Result::Success,
                           Equal({Output, cipher.Length}, plain.View()), Cleared());
            UInt8 computedTag[16U]{};
            ASTERIA_EXPECT(context, "CAVP seal",
                           Seal(Work, AeadAlgorithm::AesGcm, key.View(), nonce.View(), aad.View(), plain.View(),
                                {Output, plain.Length}, {computedTag, 16U}) == Result::Success,
                           Equal({Output, plain.Length}, cipher.View()), Equal({computedTag, 16U}, tag.View()));
            ASTERIA_EXPECT(context, "CAVP in-place open",
                           Open(Work, AeadAlgorithm::AesGcm, key.View(), nonce.View(), aad.View(),
                                {Output, plain.Length}, tag.View(), {Output, plain.Length}) == Result::Success,
                           Equal({Output, plain.Length}, plain.View()));
        }
    }
    return context.Finish();
}
