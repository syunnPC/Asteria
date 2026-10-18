// SPDX-License-Identifier: Apache-2.0

#include "User/Identity/Internal.hpp"
#include "Foundation/Crc32c.hpp"
#include "User/Text/Utf8.hpp"
namespace Asteria::User::Identity
{
namespace
{
using Detail::BuiltinName;
using Detail::Clear;
using Detail::Copy;
using Detail::Little;
using Detail::Overlap;
using Detail::Put;
using Detail::Same;
using Detail::SetName;
using Detail::View;
using Detail::Zero;
constexpr UInt8 Magic[]{'A', 'S', 'T', 'I', 'D', '0', '0', '1'};
constexpr UInt8 SealMagic[]{'A', 'S', 'T', 'I', 'D', 'C', '0', '1'};
struct Record
{
    PrincipalId Id;
    UInt64 Revision;
    TextView Name;
    UInt8 RoleValue;
    UInt8 Disabled;
    UInt8 Profile;
    const UInt8* Salt;
    const UInt8* Tag;
};
struct StateView
{
    const Snapshot& S;
    [[nodiscard]] PrincipalId Next() const noexcept
    {
        return S.NextId;
    }
    [[nodiscard]] Record A(Size i) const noexcept
    {
        const auto& a = S.Accounts[i];
        return {a.Info.Id,
                a.Info.Revision,
                View(a.Info.Login),
                static_cast<UInt8>(a.Info.AccountRole),
                static_cast<UInt8>(a.Info.Disabled),
                a.Password.Profile,
                a.Password.Salt,
                a.Password.Tag};
    }
    [[nodiscard]] Record G(Size i) const noexcept
    {
        const auto& g = S.Groups[i];
        return {g.Id, g.Revision, View(g.Label), 0U, 0U, 0U, nullptr, nullptr};
    }
    [[nodiscard]] Membership M(Size i) const noexcept
    {
        return S.Memberships[i];
    }
};
struct WireView
{
    const UInt8* Bytes;
    [[nodiscard]] PrincipalId Next() const noexcept
    {
        return Little(Bytes + 24U, 8U);
    }
    [[nodiscard]] Record A(Size i) const noexcept
    {
        const auto* p = Bytes + HeaderBytes + i * AccountRecordBytes;
        return {Little(p, 8U), Little(p + 8U, 8U), {p + 72U, p[21U]}, p[20U], p[16U], p[22U], p + 24U, p + 40U};
    }
    [[nodiscard]] Record G(Size i) const noexcept
    {
        const auto* p = Bytes + GroupTableOffset + i * GroupRecordBytes;
        return {Little(p, 8U), Little(p + 8U, 8U), {p + 24U, p[16U]}, 0U, 0U, 0U, nullptr, nullptr};
    }
    [[nodiscard]] Membership M(Size i) const noexcept
    {
        const auto* p = Bytes + MembershipTableOffset + i * MembershipRecordBytes;
        return {Little(p, 8U), Little(p + 8U, 8U)};
    }
};
// Metadata is public; password tags are never compared here.
template <class V> [[nodiscard]] bool Semantic(const V& v) noexcept
{
    if (v.Next() < FirstCustomId)
    {
        return false;
    }
    Size admins{0U};
    Size builtins{0U};
    for (Size i{0U}; i < AccountCapacity; ++i)
    {
        const auto a = v.A(i);
        if (a.Id == 0U)
        {
            continue;
        }
        if ((a.Revision == 0U) || !FormatCodec::ValidName(a.Name) || (a.RoleValue > 1U) || (a.Disabled > 1U) ||
            ((a.Id != Administrator) && (a.Id < FirstCustomId)) || ((a.Id >= FirstCustomId) && (a.Id >= v.Next())) ||
            (a.Profile > 1U))
        {
            return false;
        }
        if (a.Profile == 0U && (a.Disabled != 1U || !Zero(a.Salt, Crypto::LoginArgon2SaltBytes) ||
                                !Zero(a.Tag, Crypto::LoginArgon2TagBytes)))
        {
            return false;
        }
        if (a.Id == Administrator)
        {
            if (a.RoleValue != 1U)
            {
                return false;
            }
            ++admins;
        }
        for (Size j{0U}; j < i; ++j)
        {
            const auto b = v.A(j);
            if (b.Id != 0U &&
                (b.Id == a.Id || (b.Name.Length == a.Name.Length && Same(b.Name.Bytes, a.Name.Bytes, a.Name.Length))))
            {
                return false;
            }
        }
        Size memberships{0U};
        for (Size m{0U}; m < MembershipCapacity; ++m)
        {
            if (v.M(m).AccountId == a.Id)
            {
                ++memberships;
            }
        }
        if (memberships > MembershipsPerAccount)
        {
            return false;
        }
    }
    for (Size i{0U}; i < GroupCapacity; ++i)
    {
        const auto g = v.G(i);
        if (g.Id == 0U)
        {
            continue;
        }
        if (g.Revision == 0U || !FormatCodec::ValidName(g.Name))
        {
            return false;
        }
        if (g.Id >= Everyone && g.Id <= Administrators)
        {
            const auto expected = BuiltinName(g.Id);
            if (g.Name.Length != expected.Length || !Same(g.Name.Bytes, expected.Bytes, expected.Length))
            {
                return false;
            }
            ++builtins;
        }
        else if (g.Id < FirstCustomId || g.Id >= v.Next())
        {
            return false;
        }
        for (Size j{0U}; j < i; ++j)
        {
            const auto b = v.G(j);
            if (b.Id != 0U &&
                (b.Id == g.Id || (b.Name.Length == g.Name.Length && Same(b.Name.Bytes, g.Name.Bytes, g.Name.Length))))
            {
                return false;
            }
        }
        for (Size j{0U}; j < AccountCapacity; ++j)
        {
            if (v.A(j).Id == g.Id)
            {
                return false;
            }
        }
    }
    for (Size i{0U}; i < MembershipCapacity; ++i)
    {
        const auto m = v.M(i);
        if (m.AccountId == 0U && m.GroupId == 0U)
        {
            continue;
        }
        if (m.AccountId == 0U || m.GroupId < FirstCustomId)
        {
            return false;
        }
        bool account{false};
        bool group{false};
        for (Size j{0U}; j < AccountCapacity; ++j)
        {
            account |= v.A(j).Id == m.AccountId;
        }
        for (Size j{0U}; j < GroupCapacity; ++j)
        {
            group |= v.G(j).Id == m.GroupId;
        }
        if (!account || !group)
        {
            return false;
        }
        for (Size j{0U}; j < i; ++j)
        {
            const auto b = v.M(j);
            if (b.AccountId == m.AccountId && b.GroupId == m.GroupId)
            {
                return false;
            }
        }
    }
    return admins == 1U && builtins == 3U;
}
[[nodiscard]] UInt32 BodyCrc(const UInt8* p) noexcept
{
    UInt32 crc = Foundation::Crc32c::Start();
    for (Size i{0U}; i < BodyBytes; ++i)
    {
        crc = Foundation::Crc32c::Update(crc, (i >= 44U && i < 48U) ? UInt8{0U} : p[i]);
    }
    return Foundation::Crc32c::Finish(crc);
}
[[nodiscard]] bool Canonical(const UInt8* bytes) noexcept
{
    Size accounts{0U};
    Size groups{0U};
    Size members{0U};
    for (Size i{0U}; i < AccountCapacity; ++i)
    {
        const auto* p = bytes + HeaderBytes + i * AccountRecordBytes;
        if (Little(p, 8U) == 0U)
        {
            if (!Zero(p, AccountRecordBytes))
            {
                return false;
            }
            continue;
        }
        ++accounts;
        if (Little(p + 16U, 4U) > 1U || p[23U] != 0U || p[21U] > MaximumNameBytes ||
            !Zero(p + 72U + p[21U], MaximumNameBytes - p[21U]) || !Zero(p + 135U, 25U))
        {
            return false;
        }
    }
    for (Size i{0U}; i < GroupCapacity; ++i)
    {
        const auto* p = bytes + GroupTableOffset + i * GroupRecordBytes;
        if (Little(p, 8U) == 0U)
        {
            if (!Zero(p, GroupRecordBytes))
            {
                return false;
            }
            continue;
        }
        ++groups;
        if (p[16U] > MaximumNameBytes || !Zero(p + 17U, 7U) || !Zero(p + 24U + p[16U], MaximumNameBytes - p[16U]) ||
            !Zero(p + 87U, 9U))
        {
            return false;
        }
    }
    for (Size i{0U}; i < MembershipCapacity; ++i)
    {
        if (!Zero(bytes + MembershipTableOffset + i * MembershipRecordBytes, MembershipRecordBytes))
        {
            ++members;
        }
    }
    return accounts == Little(bytes + 36U, 2U) && groups == Little(bytes + 38U, 2U) &&
           members == Little(bytes + 40U, 2U);
}
} // Namespace
bool FormatCodec::ValidName(TextView name) noexcept
{
    if (name.Length == 0U || name.Length > MaximumNameBytes || !Text::ValidUtf8(name.Bytes, name.Length))
    {
        return false;
    }
    for (Size i{0U}; i < name.Length; ++i)
    {
        if (name.Bytes[i] < 0x20U || name.Bytes[i] == 0x7FU || name.Bytes[i] == '/' || name.Bytes[i] == '\\')
        {
            return false;
        }
    }
    return true;
}
bool FormatCodec::ValidSnapshot(const Snapshot& state) noexcept
{
    return Semantic(StateView{state});
}
bool FormatCodec::Encode(const Snapshot& state, UInt64 generation, UInt8 slot, UInt8* bytes, Size length) noexcept
{
    if (bytes == nullptr || length != SlotBytes || slot > 1U || generation == 0U ||
        Detail::Overlap(bytes, length, &state, sizeof(state)) || !ValidSnapshot(state))
    {
        return false;
    }
    Detail::Clear(bytes, length);
    Detail::Copy(bytes, Magic, sizeof(Magic));
    Detail::Put(bytes + 8U, 4U, 1U);
    Detail::Put(bytes + 12U, 4U, BodyBytes);
    Detail::Put(bytes + 16U, 8U, generation);
    Detail::Put(bytes + 24U, 8U, state.NextId);
    Detail::Put(bytes + 32U, 4U, slot);
    UInt16 accounts{0U};
    UInt16 groups{0U};
    UInt16 members{0U};
    for (Size i{0U}; i < AccountCapacity; ++i)
    {
        const auto& a = state.Accounts[i];
        if (a.Info.Id == 0U)
        {
            continue;
        }
        ++accounts;
        auto* p = bytes + HeaderBytes + i * AccountRecordBytes;
        Detail::Put(p, 8U, a.Info.Id);
        Detail::Put(p + 8U, 8U, a.Info.Revision);
        p[16U] = static_cast<UInt8>(a.Info.Disabled);
        p[20U] = static_cast<UInt8>(a.Info.AccountRole);
        p[21U] = a.Info.Login.Length;
        p[22U] = a.Password.Profile;
        Detail::Copy(p + 24U, a.Password.Salt, Crypto::LoginArgon2SaltBytes);
        Detail::Copy(p + 40U, a.Password.Tag, Crypto::LoginArgon2TagBytes);
        Detail::Copy(p + 72U, a.Info.Login.Bytes, a.Info.Login.Length);
    }
    for (Size i{0U}; i < GroupCapacity; ++i)
    {
        const auto& g = state.Groups[i];
        if (g.Id == 0U)
        {
            continue;
        }
        ++groups;
        auto* p = bytes + GroupTableOffset + i * GroupRecordBytes;
        Detail::Put(p, 8U, g.Id);
        Detail::Put(p + 8U, 8U, g.Revision);
        p[16U] = g.Label.Length;
        Detail::Copy(p + 24U, g.Label.Bytes, g.Label.Length);
    }
    for (Size i{0U}; i < MembershipCapacity; ++i)
    {
        const auto& m = state.Memberships[i];
        if (m.AccountId == 0U)
        {
            continue;
        }
        ++members;
        auto* p = bytes + MembershipTableOffset + i * MembershipRecordBytes;
        Detail::Put(p, 8U, m.AccountId);
        Detail::Put(p + 8U, 8U, m.GroupId);
    }
    Detail::Put(bytes + 36U, 2U, accounts);
    Detail::Put(bytes + 38U, 2U, groups);
    Detail::Put(bytes + 40U, 2U, members);
    const auto crc = BodyCrc(bytes);
    Detail::Put(bytes + 44U, 4U, crc);
    auto* seal = bytes + SealOffset;
    Detail::Copy(seal, SealMagic, sizeof(SealMagic));
    Detail::Put(seal + 8U, 8U, generation);
    Detail::Put(seal + 16U, 4U, crc);
    Detail::Put(seal + 20U, 4U, slot);
    Detail::Put(seal + 24U, 4U, Foundation::Crc32c::Compute(seal, 24U));
    return true;
}
bool FormatCodec::Validate(const UInt8* bytes, Size length, UInt8 slot, UInt64& generation) noexcept
{
    using Detail::BuiltinName;
    using Detail::Clear;
    using Detail::Copy;
    using Detail::Little;
    using Detail::Overlap;
    using Detail::Put;
    using Detail::Same;
    using Detail::SetName;
    using Detail::View;
    using Detail::Zero;
    if (bytes == nullptr || length != SlotBytes || slot > 1U || Overlap(bytes, length, &generation, sizeof(generation)))
    {
        return false;
    }
    const auto* seal = bytes + SealOffset;
    const auto gen = Little(bytes + 16U, 8U);
    if (!Same(bytes, Magic, 8U) || Little(bytes + 8U, 4U) != 1U || Little(bytes + 12U, 4U) != BodyBytes || gen == 0U ||
        Little(bytes + 32U, 4U) != slot || !Zero(bytes + 42U, 2U) || !Zero(bytes + 48U, 16U) ||
        !Zero(bytes + BodyBytes, SealOffset - BodyBytes) || !Same(seal, SealMagic, 8U) ||
        Little(seal + 8U, 8U) != gen || Little(seal + 20U, 4U) != slot || !Zero(seal + 28U, 4U) ||
        Little(bytes + 44U, 4U) != BodyCrc(bytes) || Little(seal + 16U, 4U) != Little(bytes + 44U, 4U) ||
        Little(seal + 24U, 4U) != Foundation::Crc32c::Compute(seal, 24U) || !Canonical(bytes) ||
        !Semantic(WireView{bytes}))
    {
        return false;
    }
    generation = gen;
    return true;
}
bool FormatCodec::Decode(const UInt8* bytes, Size length, UInt8 slot, Snapshot& state, UInt64& generation) noexcept
{
    UInt64 gen{0U};
    if (Detail::Overlap(bytes, length, &state, sizeof(state)) ||
        Detail::Overlap(bytes, length, &generation, sizeof(generation)) ||
        Detail::Overlap(&state, sizeof(state), &generation, sizeof(generation)) || !Validate(bytes, length, slot, gen))
    {
        return false;
    }
    Detail::Clear(&state, sizeof(state));
    state.NextId = Detail::Little(bytes + 24U, 8U);
    for (Size i{0U}; i < AccountCapacity; ++i)
    {
        const auto r = WireView{bytes}.A(i);
        if (r.Id == 0U)
        {
            continue;
        }
        auto& a = state.Accounts[i];
        a.Info.Id = r.Id;
        a.Info.Revision = r.Revision;
        a.Info.Disabled = r.Disabled != 0U;
        a.Info.AccountRole = static_cast<Role>(r.RoleValue);
        Detail::SetName(a.Info.Login, r.Name);
        a.Password.Profile = r.Profile;
        Detail::Copy(a.Password.Salt, r.Salt, Crypto::LoginArgon2SaltBytes);
        Detail::Copy(a.Password.Tag, r.Tag, Crypto::LoginArgon2TagBytes);
    }
    for (Size i{0U}; i < GroupCapacity; ++i)
    {
        const auto r = WireView{bytes}.G(i);
        if (r.Id == 0U)
        {
            continue;
        }
        auto& g = state.Groups[i];
        g.Id = r.Id;
        g.Revision = r.Revision;
        Detail::SetName(g.Label, r.Name);
    }
    for (Size i{0U}; i < MembershipCapacity; ++i)
    {
        state.Memberships[i] = WireView{bytes}.M(i);
    }
    generation = gen;
    return true;
}
} // Namespace Asteria::User::Identity
