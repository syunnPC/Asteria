// SPDX-License-Identifier: Apache-2.0
#include "User/Identity/Support.hpp"
#include <map>
#include <set>
#include <string>
using Asteria::User::Identity::AccountCapacity;
using Asteria::User::Identity::AccountInfo;
using Asteria::User::Identity::AccountStore;
using Asteria::User::Identity::Everyone;
using Asteria::User::Identity::FormatCodec;
using Asteria::User::Identity::GroupCapacity;
using Asteria::User::Identity::MaximumCounter;
using IdentityTest::Memory;
using Asteria::User::Identity::Name;
using Asteria::User::Identity::PrincipalContext;
using Asteria::User::Identity::PrincipalId;
using Asteria::User::Identity::Role;
using Asteria::User::Identity::Size;
using Asteria::User::Identity::SlotBytes;
using Asteria::User::Identity::Snapshot;
using Asteria::User::Identity::Status;
using Asteria::User::Identity::StorageBytes;
using Asteria::User::Identity::System;
using Asteria::User::Identity::UInt64;
using Asteria::User::Identity::UInt8;
using Asteria::User::Identity::Users;
using IdentityTest::View;
struct ModelAccount
{
    std::string Name;
    UInt64 Revision;
    std::set<UInt64> Groups;
};
static Memory Disk;
static AccountStore Store;
static Snapshot SnapshotState;
static PrincipalContext Context;
int main() noexcept
{
    Asteria::TestSupport::TestContext t{"IdentityAccountModel"};
    ASTERIA_EXPECT(t, "format", Store.Format(Disk.Interface()) == Status::Ok);
    std::map<UInt64, ModelAccount> accounts{{2U, {"administrator", 1U, {}}}};
    std::map<UInt64, std::string> groups{{3U, "Everyone"}, {4U, "Users"}, {5U, "Administrators"}};
    UInt64 next = 256U;
    UInt64 generation = 1U;
    UInt64 random = 321U;
    bool matches = true;
    for (Size step = 0U; step < 500U; ++step)
    {
        random = random * 6364136223846793005ULL + 1U;
        auto action = (random >> 32U) % 6U;
        const auto name = "n" + std::to_string(step);
        PrincipalId id = 999U;
        if (action == 0U && accounts.size() < 60U)
        {
            auto result = Store.CreateAccount(View(name.c_str()), Role::Standard, id);
            matches &= result == Status::Ok && id == next;
            accounts[next++] = {name, 1U, {}};
            ++generation;
        }
        else if (action == 1U && groups.size() < 60U)
        {
            matches &= Store.CreateGroup(View(name.c_str()), id) == Status::Ok && id == next;
            groups[next++] = name;
            ++generation;
        }
        else if (action == 2U && accounts.size() > 1U)
        {
            auto it = accounts.begin();
            std::advance(it, static_cast<long>(1U + random % (accounts.size() - 1U)));
            matches &= Store.UpdateAccount(it->first, View(name.c_str()), Role::Standard, true) == Status::Ok;
            it->second.Name = name;
            ++it->second.Revision;
            ++generation;
        }
        else if (action == 3U && accounts.size() > 1U)
        {
            auto it = accounts.begin();
            std::advance(it, static_cast<long>(1U + random % (accounts.size() - 1U)));
            matches &= Store.DeleteAccount(it->first) == Status::Ok;
            accounts.erase(it);
            ++generation;
        }
        else if (action == 4U && groups.size() > 3U)
        {
            auto it = groups.begin();
            std::advance(it, static_cast<long>(3U + random % (groups.size() - 3U)));
            matches &= Store.DeleteGroup(it->first) == Status::Ok;
            for (auto& pair : accounts)
            {
                if (pair.second.Groups.erase(it->first) != 0U)
                {
                    ++pair.second.Revision;
                }
            }
            groups.erase(it);
            ++generation;
        }
        else if (action == 5U)
        {
            auto it = accounts.begin();
            std::advance(it, static_cast<long>(random % accounts.size()));
            PrincipalId membership[12U]{};
            Size count = 0U;
            for (const auto& pair : groups)
            {
                if (pair.first >= 256U && count < 12U && ((pair.first + random) & 1U))
                {
                    membership[count++] = pair.first;
                }
            }
            matches &= Store.SetMemberships(it->first, membership, count) == Status::Ok;
            it->second.Groups.clear();
            for (Size i = 0; i < count; ++i)
            {
                it->second.Groups.insert(membership[i]);
            }
            ++it->second.Revision;
            ++generation;
        }
        if (step % 17U == 0U)
        {
            matches &= Store.Open(Disk.Interface()) == Status::Ok;
        }
        matches &= Store.Generation() == generation;
        for (const auto& pair : accounts)
        {
            AccountInfo info;
            matches &=
                Store.GetAccount(pair.first, info) == Status::Ok && info.Revision == pair.second.Revision &&
                std::string(reinterpret_cast<const char*>(info.Login.Bytes), info.Login.Length) == pair.second.Name;
            PrincipalId list[12U]{};
            Size count = 99U;
            matches &= Store.GetMemberships(pair.first, list, 12U, count) == Status::Ok;
            std::set<UInt64> actual(list, list + count);
            matches &= actual == pair.second.Groups;
        }
    }
    ASTERIA_EXPECT(t, "500 independent account/membership mutations with reopen", matches);
    ASTERIA_EXPECT(t, "builtins cannot repurpose", Store.DeleteAccount(2U) == Status::Invalid,
                   Store.DeleteGroup(3U) == Status::Invalid, Store.RenameGroup(5U, View("x")) == Status::Invalid);
    AccountInfo before;
    ASTERIA_EXPECT(t, "read admin", Store.GetAccount(2U, before) == Status::Ok);
    PrincipalId invalid[]{5U};
    ASTERIA_EXPECT(t, "cannot edit role group", Store.SetMemberships(2U, invalid, 1U) == Status::Invalid);
    ASTERIA_EXPECT(t, "system context", Store.BuildContext(System, 0U, 0U, Context) == Status::Ok,
                   Context.AllowSubjects.Count == 2U, Context.AllowSubjects.Ids[0U] == 1U,
                   Context.AllowSubjects.Ids[1U] == 3U);
    // Synthetic codec credential allows context tests without reducing product hashing.
    UInt64 gen = 0U;
    const auto slot = static_cast<UInt8>((generation - 1U) % 2U);
    ASTERIA_EXPECT(t, "synthetic context fixture",
                   FormatCodec::Decode(Disk.Bytes + slot * SlotBytes, SlotBytes, slot, SnapshotState, gen));
    PrincipalId standardId = 0U;
    for (auto& a : SnapshotState.Accounts)
    {
        if (a.Info.Id == 2U)
        {
            a.Info.Disabled = false;
            a.Password.Profile = 1U;
        }
        else if (a.Info.Id >= 256U && standardId == 0U)
        {
            standardId = a.Info.Id;
            a.Info.Disabled = false;
            a.Password.Profile = 1U;
        }
    }
    std::memset(Disk.Bytes, 0, StorageBytes);
    ASTERIA_EXPECT(t, "enabled fixture", FormatCodec::Encode(SnapshotState, gen, 0U, Disk.Bytes, SlotBytes),
                   Store.Open(Disk.Interface()) == Status::Ok, Store.Ready() == Status::Ok);
    if (standardId != 0U)
    {
        ASTERIA_EXPECT(t, "standard context from independent membership model",
                       Store.BuildContext(standardId, 4U, 0U, Context) == Status::Ok);
        auto expectedSubjects = accounts[standardId].Groups;
        expectedSubjects.insert(standardId);
        expectedSubjects.insert(Everyone);
        expectedSubjects.insert(Users);
        const std::set<UInt64> allow(Context.AllowSubjects.Ids,
                                     Context.AllowSubjects.Ids + Context.AllowSubjects.Count),
            deny(Context.DenySubjects.Ids, Context.DenySubjects.Ids + Context.DenySubjects.Count);
        ASTERIA_EXPECT(t, "exact standard subject sets", allow == expectedSubjects, deny == expectedSubjects,
                       Store.BuildContext(standardId, 4U, 8U, Context) == Status::Invalid);
    }
    ASTERIA_EXPECT(t, "ordinary admin keeps deny group", Store.BuildContext(2U, 4U, 0U, Context) == Status::Ok,
                   Context.AllowSubjects.Count + 1U == Context.DenySubjects.Count,
                   Context.DenySubjects.Ids[Context.DenySubjects.Count - 1U] == 5U);
    ASTERIA_EXPECT(t, "elevated enables allow group", Store.BuildContext(2U, 4U, 8U, Context) == Status::Ok,
                   Context.AllowSubjects.Count == Context.DenySubjects.Count,
                   Context.AllowSubjects.Ids[Context.AllowSubjects.Count - 1U] == 5U);
    PrincipalId list[13U]{};
    for (Size i = 0; i < 13U; ++i)
    {
        const auto n = "g" + std::to_string(i);
        ASTERIA_EXPECT(t, "create group", Store.CreateGroup(View(n.c_str()), list[i]) == Status::Ok);
    }
    ASTERIA_EXPECT(t, "membership cap", Store.SetMemberships(2U, list, 13U) == Status::Invalid,
                   Store.SetMemberships(2U, list, 12U) == Status::Ok,
                   Store.BuildContext(2U, 4U, 8U, Context) == Status::Ok, Context.AllowSubjects.Count == 16U,
                   Context.DenySubjects.Count == 16U);
    ASTERIA_EXPECT(t, "read member revision", Store.GetAccount(2U, before) == Status::Ok);
    const auto rev = before.Revision;
    ASTERIA_EXPECT(t, "group deletion revises member atomically", Store.DeleteGroup(list[0U]) == Status::Ok,
                   Store.GetAccount(2U, before) == Status::Ok, before.Revision == rev + 1U);
    // All nonwrapping persisted counters are tested at their representable end.
    ASTERIA_EXPECT(t, "decode latest",
                   FormatCodec::Decode(Disk.Bytes + ((Store.Generation() - gen) % 2U) * SlotBytes, SlotBytes,
                                       static_cast<UInt8>((Store.Generation() - gen) % 2U), SnapshotState, gen));
    SnapshotState.NextId = MaximumCounter;
    std::memset(Disk.Bytes, 0, StorageBytes);
    ASTERIA_EXPECT(t, "id exhaustion fixture", FormatCodec::Encode(SnapshotState, 1U, 0U, Disk.Bytes, SlotBytes),
                   Store.Open(Disk.Interface()) == Status::Ok);
    PrincipalId sentinel = 99U;
    ASTERIA_EXPECT(t, "id exhaustion preserves output",
                   Store.CreateAccount(View("end"), Role::Standard, sentinel) == Status::Exhausted, sentinel == 99U);
    for (auto& a : SnapshotState.Accounts)
    {
        if (a.Info.Id == 2U)
        {
            a.Info.Revision = MaximumCounter;
        }
    }
    ASTERIA_EXPECT(t, "revision fixture", FormatCodec::Encode(SnapshotState, 2U, 0U, Disk.Bytes, SlotBytes),
                   Store.Open(Disk.Interface()) == Status::Ok,
                   Store.UpdateAccount(2U, View("end"), Role::Admin, true) == Status::Exhausted,
                   Store.SetMemberships(2U, nullptr, 0U) == Status::Exhausted,
                   Store.DeleteGroup(list[1U]) == Status::Exhausted);
    SnapshotState.NextId = 10000U;
    ASTERIA_EXPECT(
        t, "generation fixture", FormatCodec::Encode(SnapshotState, MaximumCounter, 0U, Disk.Bytes, SlotBytes),
        Store.Open(Disk.Interface()) == Status::Ok, Store.CreateGroup(View("last"), sentinel) == Status::Exhausted);
    Disk.Reset();
    ASTERIA_EXPECT(t, "bounds format", Store.Format(Disk.Interface()) == Status::Ok);
    bool capacity = true;
    for (Size i = 1U; i < AccountCapacity; ++i)
    {
        const auto n = "a" + std::to_string(i);
        capacity &= Store.CreateAccount(View(n.c_str()), Role::Standard, sentinel) == Status::Ok;
    }
    sentinel = 99U;
    ASTERIA_EXPECT(t, "64 accounts no overflow", capacity,
                   Store.CreateAccount(View("overflow"), Role::Standard, sentinel) == Status::Full, sentinel == 99U);
    for (Size i = 3U; i < GroupCapacity; ++i)
    {
        const auto n = "q" + std::to_string(i);
        capacity &= Store.CreateGroup(View(n.c_str()), sentinel) == Status::Ok;
    }
    sentinel = 99U;
    ASTERIA_EXPECT(t, "64 groups no overflow", capacity, Store.CreateGroup(View("overflow"), sentinel) == Status::Full,
                   sentinel == 99U);
    return t.Finish();
}
