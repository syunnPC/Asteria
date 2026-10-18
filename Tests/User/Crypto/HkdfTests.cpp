// SPDX-License-Identifier: Apache-2.0 AND BSD-3-Clause
// RFC 5869 test vectors only; project-authored test logic is Apache-2.0.
// Copyright (c) 2010 IETF Trust and the persons identified as authors of the code. All rights reserved.
// Redistribution and use in source and binary forms, with or without modification, is permitted pursuant to,
// and subject to the license terms contained in, the Revised BSD License set forth in Section 4.c of the
// IETF Trust's Legal Provisions Relating to IETF Documents (https://trustee.ietf.org/license-info).
// See LICENSES/BSD-3-Clause.txt and https://www.rfc-editor.org/rfc/rfc5869.txt.
#include "User/Crypto/Vectors.hpp"
int main() noexcept
{
    using CryptoTest::Cleared;
    using CryptoTest::Empty;
    using Asteria::User::Crypto::HashAlgorithm;
    using CryptoTest::Hex;
    using Asteria::User::Crypto::HkdfExpand;
    using Asteria::User::Crypto::HkdfExtract;
    using CryptoTest::IsHex;
    using CryptoTest::Output;
    using Asteria::User::Crypto::Result;
    using Asteria::User::Crypto::Size;
    using Asteria::User::Crypto::UInt8;
    using CryptoTest::Work;
    Asteria::TestSupport::TestContext context{"CryptoHkdf"};
    // RFC 5869 Appendix A.1, A.2 and A.3.
    const Hex ikm{"0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b"};
    const Hex salt{"000102030405060708090a0b0c"};
    const Hex info{"f0f1f2f3f4f5f6f7f8f9"};
    UInt8 key[32U]{};
    ASTERIA_EXPECT(
        context, "HKDF A.1",
        HkdfExtract(Work, HashAlgorithm::Sha256, salt.View(), ikm.View(), {key, 32U}) == Result::Success,
        IsHex(key, 32U, "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5"),
        HkdfExpand(Work, HashAlgorithm::Sha256, {key, 32U}, info.View(), {Output, 42U}) == Result::Success,
        IsHex(Output, 42U, "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"),
        Cleared());
    UInt8 input2[80U]{};
    UInt8 salt2[80U]{};
    UInt8 info2[80U]{};
    for (Size i = 0U; i < 80U; ++i)
    {
        input2[i] = static_cast<UInt8>(i);
        salt2[i] = static_cast<UInt8>(i + 0x60U);
        info2[i] = static_cast<UInt8>(i + 0xb0U);
    }
    ASTERIA_EXPECT(context, "HKDF A.2",
                   HkdfExtract(Work, HashAlgorithm::Sha256, {salt2, 80U}, {input2, 80U}, {key, 32U}) == Result::Success,
                   IsHex(key, 32U, "06a6b88c5853361a06104c9ceb35b45cef760014904671014a193f40c15fc244"),
                   HkdfExpand(Work, HashAlgorithm::Sha256, {key, 32U}, {info2, 80U}, {Output, 82U}) == Result::Success,
                   IsHex(Output, 82U,
                         "b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c59045a99cac7827271cb41c65e590"
                         "e09da3275600c2f09b8367793a9aca3db71cc30c58179ec3e87c14c01d5c1f3434f1d87"));
    ASTERIA_EXPECT(
        context, "HKDF A.3", HkdfExtract(Work, HashAlgorithm::Sha256, Empty, ikm.View(), {key, 32U}) == Result::Success,
        IsHex(key, 32U, "19ef24a32c717b167f33a91d6f648bdf96596776afdb6377ac434c1c293ccb04"),
        HkdfExpand(Work, HashAlgorithm::Sha256, {key, 32U}, Empty, {Output, 42U}) == Result::Success,
        IsHex(Output, 42U, "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8"));
    static UInt8 maximum[255U * 64U + 1U]{};
    ASTERIA_EXPECT(context, "HKDF SHA512 bounds",
                   HkdfExtract(Work, HashAlgorithm::Sha512, Empty, ikm.View(), {Output, 64U}) == Result::Success,
                   HkdfExpand(Work, HashAlgorithm::Sha512, {Output, 64U}, Empty, {maximum, 255U * 64U}) ==
                       Result::Success,
                   HkdfExpand(Work, HashAlgorithm::Sha512, {Output, 64U}, Empty, {maximum, sizeof(maximum)}) ==
                       Result::BufferSize);
    return context.Finish();
}
