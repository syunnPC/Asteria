// SPDX-License-Identifier: Apache-2.0
#include "User/Crypto/Vectors.hpp"

int main() noexcept
{
    using CryptoTest::Cleared;
    using CryptoTest::Empty;
    using Asteria::User::Crypto::Hash;
    using Asteria::User::Crypto::HashAlgorithm;
    using CryptoTest::IsHex;
    using CryptoTest::Output;
    using Asteria::User::Crypto::Result;
    using CryptoTest::Text;
    using Asteria::User::Crypto::UInt8;
    using CryptoTest::Work;
    Asteria::TestSupport::TestContext context{"CryptoHash"};
    // FIPS 180-4 §6; NIST SHS example vectors, empty, abc, multiblock.
    ASTERIA_EXPECT(context, "SHA-256 abc",
                   Hash(Work, HashAlgorithm::Sha256, Text("abc"), {Output, 32U}) == Result::Success,
                   IsHex(Output, 32U, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), Cleared());
    ASTERIA_EXPECT(context, "SHA-256 empty", Hash(Work, HashAlgorithm::Sha256, Empty, {Output, 32U}) == Result::Success,
                   IsHex(Output, 32U, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    ASTERIA_EXPECT(context, "SHA-256 two-block padding",
                   Hash(Work, HashAlgorithm::Sha256, Text("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
                        {Output, 32U}) == Result::Success,
                   IsHex(Output, 32U, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    ASTERIA_EXPECT(context, "SHA-512 abc",
                   Hash(Work, HashAlgorithm::Sha512, Text("abc"), {Output, 64U}) == Result::Success,
                   IsHex(Output, 64U,
                         "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3fee"
                         "bbd454d4423643ce80e2a9ac94fa54ca49f"));
    ASTERIA_EXPECT(context, "SHA-512 empty", Hash(Work, HashAlgorithm::Sha512, Empty, {Output, 64U}) == Result::Success,
                   IsHex(Output, 64U,
                         "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877ee"
                         "c2f63b931bd47417a81a538327af927da3e"));
    ASTERIA_EXPECT(context, "SHA-512 two-block padding",
                   Hash(Work, HashAlgorithm::Sha512,
                        Text("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrl"
                             "mnopqrsmnopqrstnopqrstu"),
                        {Output, 64U}) == Result::Success,
                   IsHex(Output, 64U,
                         "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018501d289e4900f7e4331b99dec4b54"
                         "33ac7d329eeb6dd26545e96e55b874be909"));
    static UInt8 million[1000000U];
    for (auto& b : million)
    {
        b = 'a';
    }
    ASTERIA_EXPECT(context, "SHA million-a",
                   Hash(Work, HashAlgorithm::Sha256, {million, sizeof(million)}, {Output, 32U}) == Result::Success,
                   IsHex(Output, 32U, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"),
                   Hash(Work, HashAlgorithm::Sha512, {million, sizeof(million)}, {Output, 64U}) == Result::Success,
                   IsHex(Output, 64U,
                         "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973ebde0ff244877ea60a4cb0432ce577c"
                         "31beb009c5c2c49aa2e4eadb217ad8cc09b"));
    ASTERIA_EXPECT(context, "invalid buffers and digest size",
                   Hash(Work, HashAlgorithm::Sha256, {nullptr, 1U}, {Output, 32U}) == Result::InvalidArgument,
                   Hash(Work, HashAlgorithm::Sha256, Empty, {Output, 31U}) == Result::BufferSize, Cleared());
    return context.Finish();
}
