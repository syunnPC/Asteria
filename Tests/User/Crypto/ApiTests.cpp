// SPDX-License-Identifier: Apache-2.0
#include "User/Crypto/Vectors.hpp"
#include <initializer_list>

int main() noexcept
{
    using Asteria::User::Crypto::AeadAlgorithm;
    using CryptoTest::Cleared;
    using CryptoTest::Empty;
    using Asteria::User::Crypto::Equal;
    using Asteria::User::Crypto::Hash;
    using Asteria::User::Crypto::HashAlgorithm;
    using Asteria::User::Crypto::HmacDrbg;
    using Asteria::User::Crypto::MaximumAeadBytes;
    using Asteria::User::Crypto::MaximumDrbgRequest;
    using Asteria::User::Crypto::Open;
    using CryptoTest::Output;
    using Asteria::User::Crypto::Result;
    using Asteria::User::Crypto::Seal;
    using Asteria::User::Crypto::Size;
    using Asteria::User::Crypto::UInt8;
    using CryptoTest::Work;
    Asteria::TestSupport::TestContext context{"CryptoApi"};
    UInt8 key[32U]{};
    UInt8 nonce[12U]{};
    UInt8 aad[1U]{};
    UInt8 input[65U]{};
    UInt8 tag[16U]{};
    UInt8 saved[65U]{};
    for (Size i = 0U; i < sizeof(input); ++i)
    {
        input[i] = static_cast<UInt8>(i);
    }
    const AeadAlgorithm algorithms[]{AeadAlgorithm::AesGcm, AeadAlgorithm::ChaCha20Poly1305};
    for (const auto algorithm : algorithms)
    {
        for (const Size bytes : {0U, 1U, 15U, 16U, 17U, 63U, 64U, 65U})
        {
            std::memcpy(Output, input, bytes);
            ASTERIA_EXPECT(context, "in-place seal",
                           Seal(Work, algorithm, {key, 32U}, {nonce, 12U}, {aad, 1U}, {Output, bytes}, {Output, bytes},
                                {tag, 16U}) == Result::Success,
                           Cleared());
            std::memcpy(saved, Output, bytes);
            tag[15] ^= 1U;
            ASTERIA_EXPECT(context, "in-place failure preserves ciphertext",
                           Open(Work, algorithm, {key, 32U}, {nonce, 12U}, {aad, 1U}, {Output, bytes}, {tag, 16U},
                                {Output, bytes}) == Result::AuthenticationFailed,
                           Equal({Output, bytes}, {saved, bytes}));
            tag[15] ^= 1U;
            ASTERIA_EXPECT(context, "in-place open",
                           Open(Work, algorithm, {key, 32U}, {nonce, 12U}, {aad, 1U}, {Output, bytes}, {tag, 16U},
                                {Output, bytes}) == Result::Success,
                           Equal({Output, bytes}, {input, bytes}), Cleared());
        }
        ASTERIA_EXPECT(context, "partial overlap refused",
                       Seal(Work, algorithm, {key, 32U}, {nonce, 12U}, {aad, 1U}, {Output, 16U}, {Output + 1U, 16U},
                            {tag, 16U}) == Result::InvalidArgument);
        ASTERIA_EXPECT(context, "nonces and tags exact",
                       Open(Work, algorithm, {key, 32U}, {nonce, 11U}, {aad, 1U}, {input, 16U}, {tag, 16U},
                            {Output, 16U}) == Result::InvalidArgument,
                       Open(Work, algorithm, {key, 32U}, {nonce, 12U}, {aad, 1U}, {input, 16U}, {tag, 15U},
                            {Output, 16U}) == Result::InvalidArgument);
        ASTERIA_EXPECT(context, "payload bound before read",
                       Open(Work, algorithm, {key, 32U}, {nonce, 12U}, {aad, 1U}, {input, MaximumAeadBytes + 1U},
                            {tag, 16U}, {Output, MaximumAeadBytes + 1U}) == Result::InvalidArgument);
    }
    ASTERIA_EXPECT(context, "unknown algorithms",
                   Hash(Work, static_cast<HashAlgorithm>(255U), Empty, {Output, 32U}) == Result::InvalidArgument,
                   Seal(Work, static_cast<AeadAlgorithm>(255U), {key, 32U}, {nonce, 12U}, Empty, Empty, {Output, 0U},
                        {tag, 16U}) == Result::InvalidArgument,
                   Cleared());
    HmacDrbg rng;
    ASTERIA_EXPECT(context, "DRBG entropy bound",
                   rng.Instantiate(Work, {key, 31U}, {nonce, 12U}, Empty) == Result::InvalidArgument);
    UInt8 seedNonce[16U]{};
    ASTERIA_EXPECT(context, "DRBG request and cleanup",
                   rng.Instantiate(Work, {key, 32U}, {seedNonce, 16U}, Empty) == Result::Success,
                   rng.Generate(Work, Empty, {Output, MaximumDrbgRequest + 1U}) == Result::BufferSize, Cleared(),
                   rng.Reseed(Work, {key, 32U}, {aad, 1U}) == Result::Success,
                   rng.Generate(Work, Empty, {Output, 64U}) == Result::Success, Cleared());
    rng.Clear();
    ASTERIA_EXPECT(context, "DRBG cleared", rng.Generate(Work, Empty, {Output, 1U}) == Result::NotInitialized);
    return context.Finish();
}
