// SPDX-License-Identifier: Apache-2.0 AND BSD-3-Clause
// RFC 9106 test vectors only; project-authored test logic is Apache-2.0.
// Copyright (c) 2021 IETF Trust and the persons identified as authors of the code. All rights reserved.
// Redistribution and use in source and binary forms, with or without modification, is permitted pursuant to,
// and subject to the license terms contained in, the Revised BSD License set forth in Section 4.c of the
// IETF Trust's Legal Provisions Relating to IETF Documents (https://trustee.ietf.org/license-info).
// See LICENSES/BSD-3-Clause.txt and https://www.rfc-editor.org/rfc/rfc9106.txt.
#include "User/Crypto/Vectors.hpp"
int main() noexcept
{
    using Asteria::User::Crypto::Argon2Block;
    using Asteria::User::Crypto::Argon2Parameters;
    using Asteria::User::Crypto::Argon2id;
    using CryptoTest::Cleared;
    using CryptoTest::Empty;
    using CryptoTest::IsHex;
    using Asteria::User::Crypto::LoginArgon2Defaults;
    using Asteria::User::Crypto::LoginArgon2SaltBytes;
    using Asteria::User::Crypto::LoginArgon2TagBytes;
    using CryptoTest::Output;
    using Asteria::User::Crypto::Result;
    using Asteria::User::Crypto::Size;
    using Asteria::User::Crypto::UInt8;
    using Asteria::User::Crypto::Wipe;
    using CryptoTest::Work;
    Asteria::TestSupport::TestContext context{"Argon2id"};
    static Argon2Block memory[36U]{};
    UInt8 password[32U]{};
    UInt8 salt[16U]{};
    UInt8 secret[8U]{};
    UInt8 aad[12U]{};
    for (auto& b : password)
    {
        b = 1U;
    }
    for (auto& b : salt)
    {
        b = 2U;
    }
    for (auto& b : secret)
    {
        b = 3U;
    }
    for (auto& b : aad)
    {
        b = 4U;
    }
    // RFC 9106 §5.3, includes secret and associated data, 4 lanes / 3 passes.
    ASTERIA_EXPECT(context, "RFC 9106 Argon2id",
                   Argon2id(Work, {32U, 3U, 4U}, {password, 32U}, {salt, 16U}, {secret, 8U}, {aad, 12U}, memory, 36U,
                            {Output, 32U}) == Result::Success,
                   IsHex(Output, 32U, "0d640df58d78766c08c037a34a8b53c9d01ef0452d75b65eb52520e96b01e659"), Cleared());
    bool wiped = true;
    for (Size i = 0U; i < 32U; ++i)
    {
        for (auto x : memory[i].Word)
        {
            wiped = wiped && (x == 0U);
        }
    }
    ASTERIA_EXPECT(context, "arena wiped", wiped);
    Size polls{0U};
    const auto poll = [](void* p) noexcept
    {
        ++*static_cast<Size*>(p);
    };
    ASTERIA_EXPECT(context, "public segment callback preserves RFC result and wiping",
                   Argon2id(Work, {32U, 3U, 4U}, {password, 32U}, {salt, 16U}, {secret, 8U}, {aad, 12U}, memory, 36U,
                            {Output, 32U}, {&polls, poll}) == Result::Success,
                   polls == 3U * 4U * 4U,
                   IsHex(Output, 32U, "0d640df58d78766c08c037a34a8b53c9d01ef0452d75b65eb52520e96b01e659"), Cleared());
    ASTERIA_EXPECT(context, "RFC 9106 second recommended option", LoginArgon2Defaults.MemoryKiB == 64U * 1024U,
                   LoginArgon2Defaults.Passes == 3U, LoginArgon2Defaults.Lanes == 4U, LoginArgon2SaltBytes == 16U,
                   LoginArgon2TagBytes == 32U);
    // Only memory is scaled down to 32 KiB; all other login default dimensions
    // are unchanged. Crypto.md records the pre-change project regression value.
    Argon2Parameters login = LoginArgon2Defaults;
    login.MemoryKiB = 32U;
    ASTERIA_EXPECT(
        context, "login default shape with scaled memory",
        Argon2id(Work, login, {password, 32U}, {salt, LoginArgon2SaltBytes}, Empty, Empty, memory, 36U,
                 {Output, LoginArgon2TagBytes}) == Result::Success,
        IsHex(Output, LoginArgon2TagBytes, "03aab965c12001c9d7d0d2de33192c0494b684bb148196d73c1df1acaf6d0c2e"),
        Cleared());
    wiped = true;
    for (Size i = 0U; i < login.MemoryKiB; ++i)
    {
        for (auto x : memory[i].Word)
        {
            wiped = wiped && x == 0U;
        }
    }
    ASTERIA_EXPECT(context, "login arena wiped", wiped);
    std::memset(Output, 0xa5, 32U);
    memory[0].Word[0] = 123U;
    ASTERIA_EXPECT(context, "insufficient memory is transactional",
                   Argon2id(Work, {32U, 3U, 4U}, {password, 32U}, {salt, 16U}, Empty, Empty, memory, 31U,
                            {Output, 32U}) == Result::BufferSize,
                   Output[0] == 0xa5U, memory[0].Word[0] == 123U, Cleared());
    ASTERIA_EXPECT(context, "bounds",
                   Argon2id(Work, {31U, 3U, 4U}, {password, 32U}, {salt, 16U}, Empty, Empty, memory, 36U,
                            {Output, 32U}) == Result::InvalidArgument,
                   Argon2id(Work, {32U, 0U, 4U}, {password, 32U}, {salt, 16U}, Empty, Empty, memory, 36U,
                            {Output, 32U}) == Result::InvalidArgument);
    std::memset(reinterpret_cast<UInt8*>(&Work), 0xa5, sizeof(Work));
    const Result overlap = Argon2id(Work, {8U, 1U, 1U}, {password, 32U}, {salt, 16U}, Empty, Empty,
                                    reinterpret_cast<Argon2Block*>(&Work), 8U, {Output, 32U});
    const Result oversizedOverlap =
        Argon2id(Work, {8U, 1U, 1U}, {password, 32U}, {salt, 16U}, Empty, Empty, reinterpret_cast<Argon2Block*>(&Work),
                 static_cast<Size>(-1), {Output, 32U});
    bool preserved = true;
    const auto* bytes = reinterpret_cast<const UInt8*>(&Work);
    for (Size i = 0U; i < sizeof(Work); ++i)
    {
        preserved = preserved && bytes[i] == 0xa5U;
    }
    ASTERIA_EXPECT(context, "overlapping arena refusal preserves caller memory", overlap == Result::InvalidArgument,
                   oversizedOverlap == Result::InvalidArgument, preserved, Output[0] == 0xa5U);
    Wipe({reinterpret_cast<UInt8*>(&Work), sizeof(Work)});
    return context.Finish();
}
