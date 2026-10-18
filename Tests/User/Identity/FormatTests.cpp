// SPDX-License-Identifier: Apache-2.0
#include "User/Identity/Support.hpp"
using Asteria::User::Identity::Everyone;
using Asteria::User::Identity::FormatCodec;
using IdentityTest::Fresh;
using IdentityTest::NameOf;
using IdentityTest::Put;
using IdentityTest::Repair;
using Asteria::User::Identity::Role;
using Asteria::User::Identity::Size;
using Asteria::User::Identity::SlotBytes;
using Asteria::User::Identity::Snapshot;
using Asteria::User::Identity::UInt64;
using Asteria::User::Identity::UInt8;
static Snapshot State;
static Snapshot Decoded;
static Snapshot Before;
static UInt8 Wire[SlotBytes];
static UInt8 Fixture[SlotBytes];
int main() noexcept
{
    Asteria::TestSupport::TestContext t{"IdentityFormat"};
    Fresh(State);
    // Independently laid out golden slot, no codec-derived offsets/serializer.
    std::memcpy(Fixture, "ASTID001", 8U);
    Put(Fixture + 8U, 4U, 1U);
    Put(Fixture + 12U, 4U, 28736U);
    Put(Fixture + 16U, 8U, 1U);
    Put(Fixture + 24U, 8U, 256U);
    Put(Fixture + 36U, 2U, 1U);
    Put(Fixture + 38U, 2U, 3U);
    Put(Fixture + 64U, 8U, 2U);
    Put(Fixture + 72U, 8U, 1U);
    Fixture[80U] = 1U;
    Fixture[84U] = 1U;
    Fixture[85U] = 13U;
    std::memcpy(Fixture + 136U, "administrator", 13U);
    const char* names[]{"Everyone", "Users", "Administrators"};
    for (Size i = 0; i < 3U; ++i)
    {
        auto* p = Fixture + 10304U + 96U * i;
        Put(p, 8U, 3U + i);
        Put(p + 8U, 8U, 1U);
        p[16U] = static_cast<UInt8>(std::strlen(names[i]));
        std::memcpy(p + 24U, names[i], p[16U]);
    }
    std::memcpy(Fixture + 32736U, "ASTIDC01", 8U);
    Put(Fixture + 32744U, 8U, 1U);
    Repair(Fixture);
    ASTERIA_EXPECT(t, "exact entire slot", FormatCodec::Encode(State, 1U, 0U, Wire, SlotBytes),
                   std::memcmp(Wire, Fixture, SlotBytes) == 0);
    UInt64 generation = 88U;
    ASTERIA_EXPECT(t, "fixture decode", FormatCodec::Decode(Fixture, SlotBytes, 0U, Decoded, generation),
                   generation == 1U, Decoded.NextId == 256U);
    Before = Decoded;
    struct Mutation
    {
        Size At;
        UInt64 Value;
        Size Width;
    };
    const Mutation bad[]{{8U, 2U, 4U},      {12U, 28735U, 4U}, {16U, 0U, 8U},      {24U, 255U, 8U},   {32U, 1U, 4U},
                         {36U, 2U, 2U},     {38U, 2U, 2U},     {40U, 1U, 2U},      {42U, 1U, 2U},     {48U, 1U, 1U},
                         {64U, 1U, 8U},     {64U, 6U, 8U},     {72U, 0U, 8U},      {80U, 2U, 4U},     {84U, 0U, 1U},
                         {85U, 0U, 1U},     {85U, 64U, 1U},    {86U, 2U, 1U},      {87U, 1U, 1U},     {88U, 1U, 1U},
                         {104U, 1U, 1U},    {136U, 0U, 1U},    {136U, 0xC0U, 1U},  {136U, '/', 1U},   {136U, '\\', 1U},
                         {136U, 0x7FU, 1U}, {149U, 1U, 1U},    {199U, 1U, 1U},     {224U, 1U, 1U},    {10304U, 2U, 8U},
                         {10312U, 0U, 8U},  {10320U, 0U, 1U},  {10321U, 1U, 1U},   {10328U, 'e', 1U}, {10336U, 1U, 1U},
                         {10391U, 1U, 1U},  {16448U, 2U, 8U},  {16456U, 256U, 8U}, {28736U, 1U, 1U},  {32764U, 1U, 4U}};
    bool refused = true;
    for (const auto& m : bad)
    {
        std::memcpy(Wire, Fixture, SlotBytes);
        Put(Wire + m.At, m.Width, m.Value);
        Repair(Wire);
        generation = 88U;
        refused &= !FormatCodec::Decode(Wire, SlotBytes, 0U, Decoded, generation) && generation == 88U &&
                   std::memcmp(&Before, &Decoded, sizeof(Before)) == 0;
    }
    ASTERIA_EXPECT(t, "reserved/profile/name/id/count/gap refusals preserve outputs", refused);
    Fresh(State);
    State.NextId = 270U;
    State.Accounts[1U] = State.Accounts[0U];
    auto& a = State.Accounts[1U];
    a.Info.Id = 256U;
    a.Info.AccountRole = Role::Standard;
    NameOf(a.Info.Login, "用户");
    State.Groups[3U].Id = 257U;
    State.Groups[3U].Revision = 1U;
    NameOf(State.Groups[3U].Label, "team");
    State.Memberships[0U] = {256U, 257U};
    ASTERIA_EXPECT(t, "UTF8 custom account reference", FormatCodec::Encode(State, 8U, 1U, Wire, SlotBytes),
                   FormatCodec::Decode(Wire, SlotBytes, 1U, Decoded, generation));
    const auto Reject = [&]() noexcept
    {
        std::memset(Wire, 0xA5, sizeof(Wire));
        return !FormatCodec::Encode(State, 9U, 1U, Wire, SlotBytes) && Wire[0] == 0xA5U;
    };
    State.Memberships[1U] = State.Memberships[0U];
    ASTERIA_EXPECT(t, "duplicate pair", Reject());
    State.Memberships[1U] = {};
    State.Memberships[0U].GroupId = Everyone;
    ASTERIA_EXPECT(t, "implicit membership refused", Reject());
    State.Memberships[0U].GroupId = 268U;
    ASTERIA_EXPECT(t, "dangling group", Reject());
    State.Memberships[0U] = {999U, 257U};
    ASTERIA_EXPECT(t, "dangling account", Reject());
    State.Memberships[0U] = {256U, 257U};
    State.Groups[3U].Id = 256U;
    ASTERIA_EXPECT(t, "cross-kind collision", Reject());
    State.Groups[3U].Id = 257U;
    State.NextId = 257U;
    ASTERIA_EXPECT(t, "high watermark", Reject());
    State.NextId = 270U;
    NameOf(a.Info.Login, "administrator");
    ASTERIA_EXPECT(t, "duplicate account name", Reject());
    NameOf(a.Info.Login, "other");
    for (Size i = 0; i < 13U; ++i)
    {
        auto& g = State.Groups[i + 3U];
        g.Id = 257U + i;
        g.Revision = 1U;
        g.Label = {};
        g.Label.Length = 1U;
        g.Label.Bytes[0] = static_cast<UInt8>('A' + i);
        State.Memberships[i] = {256U, 257U + i};
    }
    ASTERIA_EXPECT(t, "thirteen memberships", Reject());
    const UInt8 badUtf8[][4U]{{0xC0U, 0x80U}, {0xEDU, 0xA0U, 0x80U}, {0xF4U, 0x90U, 0x80U, 0x80U}, {0xE2U, 0x82U, 0U}};
    for (const auto& text : badUtf8)
    {
        ASTERIA_EXPECT(t, "UTF8 refusal", !FormatCodec::ValidName({text, sizeof(text)}));
    }
    return t.Finish();
}
