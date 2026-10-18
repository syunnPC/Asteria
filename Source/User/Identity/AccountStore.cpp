// SPDX-License-Identifier: Apache-2.0

#include "User/Identity/Internal.hpp"
namespace Asteria::User::Identity
{
namespace
{
using Detail::BuiltinName;
using Detail::Clear;
using Detail::SameName;
using Detail::SetName;
using Detail::Zero;
[[nodiscard]] bool ValidStorage(Storage s) noexcept
{
    return s.Capacity >= StorageBytes && s.Read != nullptr && s.Write != nullptr && s.Flush != nullptr;
}
[[nodiscard]] bool ValidRole(Role role) noexcept
{
    return role == Role::Standard || role == Role::Admin;
}
} // Namespace
void AccountStore::Close() noexcept
{
    m_open = false;
    m_generation = 0U;
    m_slot = 0U;
    m_storage = {};
    Detail::Clear(&m_state, sizeof(m_state));
    Detail::Clear(&m_candidate, sizeof(m_candidate));
    Detail::Clear(m_wire, sizeof(m_wire));
}
Status AccountStore::Format(Storage storage) noexcept
{
    Close();
    if (!ValidStorage(storage))
    {
        return Status::Invalid;
    }
    for (Size slot{0U}; slot < 2U; ++slot)
    {
        if (!storage.Read(storage.Context, slot * SlotBytes, m_wire, SlotBytes))
        {
            Close();
            return Status::IoError;
        }
        if (!Detail::Zero(m_wire, SlotBytes))
        {
            Close();
            return Status::Exists;
        }
    }
    m_candidate.NextId = FirstCustomId;
    auto& a = m_candidate.Accounts[0U];
    a.Info.Id = Administrator;
    a.Info.Revision = 1U;
    a.Info.AccountRole = Role::Admin;
    a.Info.Disabled = true;
    constexpr UInt8 login[]{'a', 'd', 'm', 'i', 'n', 'i', 's', 't', 'r', 'a', 't', 'o', 'r'};
    Detail::SetName(a.Info.Login, {login, sizeof(login)});
    for (Size i{0U}; i < 3U; ++i)
    {
        auto& g = m_candidate.Groups[i];
        g.Id = Everyone + i;
        g.Revision = 1U;
        Detail::SetName(g.Label, Detail::BuiltinName(g.Id));
    }
    m_storage = storage;
    m_slot = 1U;
    m_open = true;
    return Commit();
}
Status AccountStore::Open(Storage storage) noexcept
{
    Close();
    if (!ValidStorage(storage))
    {
        return Status::Invalid;
    }
    bool found{false};
    for (UInt8 slot{0U}; slot < 2U; ++slot)
    {
        UInt64 generation{0U};
        if (!storage.Read(storage.Context, static_cast<UInt64>(slot) * SlotBytes, m_wire, SlotBytes) ||
            !FormatCodec::Decode(m_wire, SlotBytes, slot, m_candidate, generation))
        {
            continue;
        }
        if (found && generation == m_generation)
        {
            Close();
            return Status::Corrupt;
        }
        if (!found || generation > m_generation)
        {
            m_state = m_candidate;
            m_generation = generation;
            m_slot = slot;
            found = true;
        }
    }
    Detail::Clear(&m_candidate, sizeof(m_candidate));
    Detail::Clear(m_wire, sizeof(m_wire));
    if (!found)
    {
        Close();
        return Status::Corrupt;
    }
    m_storage = storage;
    m_open = true;
    return Status::Ok;
}
// Complete inactive body+zero gap, never active bytes.
Status AccountStore::Commit() noexcept
{
    if (m_generation == MaximumCounter)
    {
        Detail::Clear(&m_candidate, sizeof(m_candidate));
        return Status::Exhausted;
    }
    const UInt8 slot = static_cast<UInt8>(m_slot ^ 1U);
    const UInt64 generation = m_generation + 1U;
    if (!FormatCodec::Encode(m_candidate, generation, slot, m_wire, SlotBytes))
    {
        Detail::Clear(&m_candidate, sizeof(m_candidate));
        return Status::Invalid;
    }
    constexpr UInt8 empty[SealBytes]{};
    const UInt64 offset = static_cast<UInt64>(slot) * SlotBytes;
    if (!m_storage.Write(m_storage.Context, offset + SealOffset, empty, SealBytes) ||
        !m_storage.Flush(m_storage.Context) || !m_storage.Write(m_storage.Context, offset, m_wire, SealOffset) ||
        !m_storage.Flush(m_storage.Context) ||
        !m_storage.Write(m_storage.Context, offset + SealOffset, m_wire + SealOffset, SealBytes) ||
        !m_storage.Flush(m_storage.Context))
    {
        Close();
        return Status::IoError;
    }
    m_state = m_candidate;
    m_generation = generation;
    m_slot = slot;
    Detail::Clear(&m_candidate, sizeof(m_candidate));
    Detail::Clear(m_wire, sizeof(m_wire));
    return Status::Ok;
}
Size AccountStore::AccountIndex(PrincipalId id) const noexcept
{
    if (id != 0U)
    {
        for (Size i{0U}; i < AccountCapacity; ++i)
        {
            if (m_state.Accounts[i].Info.Id == id)
            {
                return i;
            }
        }
    }
    return AccountCapacity;
}
Size AccountStore::GroupIndex(PrincipalId id) const noexcept
{
    if (id != 0U)
    {
        for (Size i{0U}; i < GroupCapacity; ++i)
        {
            if (m_state.Groups[i].Id == id)
            {
                return i;
            }
        }
    }
    return GroupCapacity;
}
Status AccountStore::Ready() const noexcept
{
    if (!m_open)
    {
        return Status::Closed;
    }
    for (const auto& a : m_state.Accounts)
    {
        if (a.Info.Id != 0U && a.Info.AccountRole == Role::Admin && !a.Info.Disabled && a.Password.Profile == 1U)
        {
            return Status::Ok;
        }
    }
    return Status::NotProvisioned;
}
Status AccountStore::GetAccount(PrincipalId id, AccountInfo& out) const noexcept
{
    if (!m_open)
    {
        return Status::Closed;
    }
    const auto i = AccountIndex(id);
    if (i == AccountCapacity)
    {
        return Status::NotFound;
    }
    out = m_state.Accounts[i].Info;
    return Status::Ok;
}
Status AccountStore::FindAccount(TextView name, AccountInfo& out) const noexcept
{
    if (!m_open)
    {
        return Status::Closed;
    }
    if (!FormatCodec::ValidName(name))
    {
        return Status::Invalid;
    }
    for (const auto& a : m_state.Accounts)
    {
        if (a.Info.Id != 0U && Detail::SameName(a.Info.Login, name))
        {
            out = a.Info;
            return Status::Ok;
        }
    }
    return Status::NotFound;
}
Status AccountStore::GetGroup(PrincipalId id, Group& out) const noexcept
{
    if (!m_open)
    {
        return Status::Closed;
    }
    const auto i = GroupIndex(id);
    if (i == GroupCapacity)
    {
        return Status::NotFound;
    }
    out = m_state.Groups[i];
    return Status::Ok;
}
Status AccountStore::CreateAccount(TextView name, Role role, PrincipalId& id) noexcept
{
    if (!m_open)
    {
        return Status::Closed;
    }
    if (!FormatCodec::ValidName(name) || !ValidRole(role))
    {
        return Status::Invalid;
    }
    Size free{AccountCapacity};
    for (Size i{0U}; i < AccountCapacity; ++i)
    {
        const auto& a = m_state.Accounts[i];
        if (a.Info.Id == 0U)
        {
            free = i;
        }
        else if (Detail::SameName(a.Info.Login, name))
        {
            return Status::Exists;
        }
    }
    if (free == AccountCapacity)
    {
        return Status::Full;
    }
    if (m_state.NextId == MaximumCounter)
    {
        return Status::Exhausted;
    }
    m_candidate = m_state;
    auto& a = m_candidate.Accounts[free];
    a.Info.Id = m_candidate.NextId++;
    a.Info.Revision = 1U;
    a.Info.AccountRole = role;
    a.Info.Disabled = true;
    Detail::SetName(a.Info.Login, name);
    const auto allocated = a.Info.Id;
    const auto result = Commit();
    if (result == Status::Ok)
    {
        id = allocated;
    }
    return result;
}
Status AccountStore::UpdateAccount(PrincipalId id, TextView name, Role role, bool disabled) noexcept
{
    if (!m_open)
    {
        return Status::Closed;
    }
    const auto i = AccountIndex(id);
    if (i == AccountCapacity)
    {
        return Status::NotFound;
    }
    if (!FormatCodec::ValidName(name) || !ValidRole(role) || (id == Administrator && role != Role::Admin) ||
        (!disabled && m_state.Accounts[i].Password.Profile != 1U))
    {
        return Status::Invalid;
    }
    for (Size j{0U}; j < AccountCapacity; ++j)
    {
        if (j != i && m_state.Accounts[j].Info.Id != 0U && Detail::SameName(m_state.Accounts[j].Info.Login, name))
        {
            return Status::Exists;
        }
    }
    if (m_state.Accounts[i].Info.Revision == MaximumCounter)
    {
        return Status::Exhausted;
    }
    m_candidate = m_state;
    auto& a = m_candidate.Accounts[i].Info;
    ++a.Revision;
    a.AccountRole = role;
    a.Disabled = disabled;
    Detail::SetName(a.Login, name);
    return Commit();
}
Status AccountStore::DeleteAccount(PrincipalId id) noexcept
{
    if (!m_open)
    {
        return Status::Closed;
    }
    if (id < FirstCustomId)
    {
        return Status::Invalid;
    }
    const auto i = AccountIndex(id);
    if (i == AccountCapacity)
    {
        return Status::NotFound;
    }
    if (m_state.Accounts[i].Info.Revision == MaximumCounter)
    {
        return Status::Exhausted;
    }
    m_candidate = m_state;
    Detail::Clear(&m_candidate.Accounts[i], sizeof(Account));
    for (auto& m : m_candidate.Memberships)
    {
        if (m.AccountId == id)
        {
            m = {};
        }
    }
    return Commit();
}
Status AccountStore::CreateGroup(TextView name, PrincipalId& id) noexcept
{
    if (!m_open)
    {
        return Status::Closed;
    }
    if (!FormatCodec::ValidName(name))
    {
        return Status::Invalid;
    }
    Size free{GroupCapacity};
    for (Size i{0U}; i < GroupCapacity; ++i)
    {
        const auto& g = m_state.Groups[i];
        if (g.Id == 0U)
        {
            free = i;
        }
        else if (Detail::SameName(g.Label, name))
        {
            return Status::Exists;
        }
    }
    if (free == GroupCapacity)
    {
        return Status::Full;
    }
    if (m_state.NextId == MaximumCounter)
    {
        return Status::Exhausted;
    }
    m_candidate = m_state;
    auto& g = m_candidate.Groups[free];
    g.Id = m_candidate.NextId++;
    g.Revision = 1U;
    Detail::SetName(g.Label, name);
    const auto allocated = g.Id;
    const auto result = Commit();
    if (result == Status::Ok)
    {
        id = allocated;
    }
    return result;
}
Status AccountStore::RenameGroup(PrincipalId id, TextView name) noexcept
{
    if (!m_open)
    {
        return Status::Closed;
    }
    if (id < FirstCustomId || !FormatCodec::ValidName(name))
    {
        return Status::Invalid;
    }
    const auto i = GroupIndex(id);
    if (i == GroupCapacity)
    {
        return Status::NotFound;
    }
    for (Size j{0U}; j < GroupCapacity; ++j)
    {
        if (j != i && m_state.Groups[j].Id != 0U && Detail::SameName(m_state.Groups[j].Label, name))
        {
            return Status::Exists;
        }
    }
    if (m_state.Groups[i].Revision == MaximumCounter)
    {
        return Status::Exhausted;
    }
    m_candidate = m_state;
    ++m_candidate.Groups[i].Revision;
    Detail::SetName(m_candidate.Groups[i].Label, name);
    return Commit();
}
Status AccountStore::DeleteGroup(PrincipalId id) noexcept
{
    if (!m_open)
    {
        return Status::Closed;
    }
    if (id < FirstCustomId)
    {
        return Status::Invalid;
    }
    const auto i = GroupIndex(id);
    if (i == GroupCapacity)
    {
        return Status::NotFound;
    }
    for (const auto& m : m_state.Memberships)
    {
        if (m.GroupId == id && m_state.Accounts[AccountIndex(m.AccountId)].Info.Revision == MaximumCounter)
        {
            return Status::Exhausted;
        }
    }
    m_candidate = m_state;
    Detail::Clear(&m_candidate.Groups[i], sizeof(Group));
    for (auto& m : m_candidate.Memberships)
    {
        if (m.GroupId == id)
        {
            ++m_candidate.Accounts[AccountIndex(m.AccountId)].Info.Revision;
            m = {};
        }
    }
    return Commit();
}
Status AccountStore::SetMemberships(PrincipalId id, const PrincipalId* groups, Size count) noexcept
{
    if (!m_open)
    {
        return Status::Closed;
    }
    const auto i = AccountIndex(id);
    if (i == AccountCapacity)
    {
        return Status::NotFound;
    }
    if (count > MembershipsPerAccount || (count != 0U && groups == nullptr))
    {
        return Status::Invalid;
    }
    for (Size g{0U}; g < count; ++g)
    {
        if (groups[g] < FirstCustomId || GroupIndex(groups[g]) == GroupCapacity)
        {
            return Status::Invalid;
        }
        for (Size j{0U}; j < g; ++j)
        {
            if (groups[g] == groups[j])
            {
                return Status::Invalid;
            }
        }
    }
    if (m_state.Accounts[i].Info.Revision == MaximumCounter)
    {
        return Status::Exhausted;
    }
    m_candidate = m_state;
    ++m_candidate.Accounts[i].Info.Revision;
    for (auto& m : m_candidate.Memberships)
    {
        if (m.AccountId == id)
        {
            m = {};
        }
    }
    Size inserted{0U};
    for (auto& m : m_candidate.Memberships)
    {
        if (m.AccountId == 0U && inserted < count)
        {
            m = {id, groups[inserted++]};
        }
    }
    return Commit();
}
Status AccountStore::GetMemberships(PrincipalId id, PrincipalId* groups, Size capacity, Size& count) const noexcept
{
    if (!m_open)
    {
        return Status::Closed;
    }
    if (AccountIndex(id) == AccountCapacity)
    {
        return Status::NotFound;
    }
    Size needed{0U};
    for (const auto& m : m_state.Memberships)
    {
        if (m.AccountId == id)
        {
            ++needed;
        }
    }
    if (capacity < needed || (needed != 0U && groups == nullptr))
    {
        return Status::Full;
    }
    Size at{0U};
    for (const auto& m : m_state.Memberships)
    {
        if (m.AccountId == id)
        {
            groups[at++] = m.GroupId;
        }
    }
    count = needed;
    return Status::Ok;
}
Status AccountStore::BuildContext(PrincipalId id, UInt64 session, UInt64 operation,
                                  PrincipalContext& out) const noexcept
{
    if (!m_open)
    {
        return Status::Closed;
    }
    if (id == System)
    {
        if (operation != 0U)
        {
            return Status::Invalid;
        }
        Detail::Clear(&out, sizeof(out));
        out.AccountId = System;
        out.Revision = 1U;
        out.SessionId = session;
        out.AllowSubjects.Ids[0U] = System;
        out.AllowSubjects.Ids[1U] = Everyone;
        out.AllowSubjects.Count = 2U;
        out.DenySubjects = out.AllowSubjects;
        return Status::Ok;
    }
    const auto i = AccountIndex(id);
    if (i == AccountCapacity)
    {
        return Status::NotFound;
    }
    const auto& a = m_state.Accounts[i].Info;
    if (session == 0U || a.Disabled || (operation != 0U && a.AccountRole != Role::Admin))
    {
        return Status::Invalid;
    }
    Detail::Clear(&out, sizeof(out));
    out.AccountId = id;
    out.Revision = a.Revision;
    out.SessionId = session;
    out.OperationId = operation;
    auto& allow = out.AllowSubjects;
    allow.Ids[allow.Count++] = id;
    allow.Ids[allow.Count++] = Everyone;
    allow.Ids[allow.Count++] = Users;
    for (const auto& m : m_state.Memberships)
    {
        if (m.AccountId == id)
        {
            allow.Ids[allow.Count++] = m.GroupId;
        }
    }
    out.DenySubjects = allow;
    if (a.AccountRole == Role::Admin)
    {
        out.DenySubjects.Ids[out.DenySubjects.Count++] = Administrators;
        if (operation != 0U)
        {
            allow.Ids[allow.Count++] = Administrators;
        }
    }
    return Status::Ok;
}
Status AccountStore::SetCredential(PrincipalId id, const Credential& credential) noexcept
{
    if (!m_open)
    {
        return Status::Closed;
    }
    const auto i = AccountIndex(id);
    if (i == AccountCapacity)
    {
        return Status::NotFound;
    }
    if (credential.Profile != 1U)
    {
        return Status::Invalid;
    }
    if (m_state.Accounts[i].Info.Revision == MaximumCounter)
    {
        return Status::Exhausted;
    }
    m_candidate = m_state;
    ++m_candidate.Accounts[i].Info.Revision;
    m_candidate.Accounts[i].Password = credential;
    return Commit();
}
} // Namespace Asteria::User::Identity
