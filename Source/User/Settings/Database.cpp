// SPDX-License-Identifier: Apache-2.0

#include "User/Settings/Database.hpp"
#include "User/Text/Utf8.hpp"

namespace Asteria::User::Settings
{
namespace
{
[[nodiscard]] bool Same(const UInt8* a, const UInt8* b, const Size size) noexcept
{
    for (Size i{0U}; i < size; ++i)
    {
        if (a[i] != b[i])
        {
            return false;
        }
    }
    return true;
}
[[nodiscard]] bool ValidPath(const TextView path) noexcept
{
    if ((path.Bytes == nullptr) || (path.Length == 0U) || (path.Length > MaximumPathBytes) || (path.Bytes[0U] != '/') ||
        !Text::ValidUtf8(path.Bytes, path.Length))
    {
        return false;
    }
    if (path.Length == 1U)
    {
        return true;
    }
    Size start{1U};
    Size depth{0U};
    for (Size at{1U}; at <= path.Length; ++at)
    {
        if ((at < path.Length) && (path.Bytes[at] == 0U))
        {
            return false;
        }
        if ((at == path.Length) || (path.Bytes[at] == '/'))
        {
            const Size size{at - start};
            if ((size == 0U) || (size > MaximumNameBytes) || (++depth > MaximumDepth))
            {
                return false;
            }
            if ((path.Bytes[start] == '.') && ((size == 1U) || ((size == 2U) && (path.Bytes[start + 1U] == '.'))))
            {
                return false;
            }
            start = at + 1U;
        }
    }
    return true;
}
void ResetKey(Key& key) noexcept
{
    key.Used = false;
    key.Parent = 0U;
    key.NameLength = 0U;
    key.AclCount = 0U;
    for (auto& byte : key.Name)
    {
        byte = 0U;
    }
    for (auto& entry : key.Acl)
    {
        entry = {};
    }
}
void ResetValue(NamedValue& value) noexcept
{
    value.Used = false;
    value.KeyIndex = 0U;
    value.NameLength = 0U;
    value.Data.Type = ValueType::None;
    value.Data.Length = 0U;
    for (auto& byte : value.Name)
    {
        byte = 0U;
    }
    for (auto& byte : value.Data.Bytes)
    {
        byte = 0U;
    }
}
void CopyName(Name& out, const TextView name) noexcept
{
    out.Length = name.Length;
    for (Size i{0U}; i < MaximumNameBytes; ++i)
    {
        out.Bytes[i] = i < name.Length ? name.Bytes[i] : UInt8{0U};
    }
}
[[nodiscard]] TextView Parent(const TextView path) noexcept
{
    Size length{1U};
    for (Size i{1U}; i < path.Length; ++i)
    {
        if (path.Bytes[i] == '/')
        {
            length = i;
        }
    }
    return {path.Bytes, length};
}
void CopyPath(Path& out, const TextView path) noexcept
{
    out.Length = path.Length;
    for (Size i{0U}; i < MaximumPathBytes; ++i)
    {
        out.Bytes[i] = i < path.Length ? path.Bytes[i] : UInt8{0U};
    }
}
[[nodiscard]] TextView View(const Path& path) noexcept
{
    return {path.Bytes, path.Length};
}
} // namespace
bool Database::ValidStorage(const Storage storage) noexcept
{
    return (storage.Capacity >= StorageBytes) && (storage.Read != nullptr) && (storage.Write != nullptr) &&
           (storage.Flush != nullptr);
}
void Database::Close() noexcept
{
    m_open = false;
    m_generation = 0U;
    m_storage = {};
    for (auto& watcher : m_watchers)
    {
        watcher.Used = false;
        watcher.Count = 0U;
        watcher.Overflow = false;
    }
}
Status Database::Format(const Storage storage, const AclEntry* const rootAcl, const Size count) noexcept
{
    Close();
    if (!ValidStorage(storage) || !FormatCodec::ValidAcl(rootAcl, count))
    {
        return Status::Invalid;
    }
    for (Size slot{0U}; slot < 2U; ++slot)
    {
        if (!storage.Read(storage.Context, slot * SlotBytes, m_wire, SlotBytes))
        {
            return Status::IoError;
        }
        for (const UInt8 byte : m_wire)
        {
            if (byte != 0U)
            {
                return Status::Exists;
            }
        }
    }
    for (auto& key : m_candidate.Keys)
    {
        ResetKey(key);
    }
    for (auto& value : m_candidate.Values)
    {
        ResetValue(value);
    }
    Key& root = m_candidate.Keys[0U];
    root.Used = true;
    root.AclCount = static_cast<UInt8>(count);
    for (Size i{0U}; i < count; ++i)
    {
        root.Acl[i] = rootAcl[i];
    }
    m_storage = storage;
    m_slot = 1U;
    m_open = true;
    constexpr UInt8 slash{'/'};
    return Commit({&slash, 1U}, {}, ChangeKind::KeyCreated);
}
Status Database::Open(const Storage storage) noexcept
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
        if (found && (generation == m_generation))
        {
            m_generation = 0U;
            return Status::Corrupt;
        }
        if (!found || (generation > m_generation))
        {
            m_state = m_candidate;
            m_generation = generation;
            m_slot = slot;
            found = true;
        }
    }
    if (!found)
    {
        return Status::Corrupt;
    }
    m_storage = storage;
    m_open = true;
    return Status::Ok;
}
UInt8 Database::EffectiveRights(const Snapshot& state, const Size key, const Access access) noexcept
{
    if ((key >= KeyCapacity) || !state.Keys[key].Used || ((access.ViewRights & ~AllRights) != 0U))
    {
        return 0U;
    }
    UInt8 allow{0U};
    UInt8 deny{0U};
    Size current{key};
    for (Size depth{0U}; depth <= MaximumDepth; ++depth)
    {
        const Key& ancestor = state.Keys[current];
        if (!ancestor.Used || (ancestor.AclCount > AclCapacity))
        {
            return 0U;
        }
        for (Size i{0U}; i < ancestor.AclCount; ++i)
        {
            const AclEntry& entry = ancestor.Acl[i];
            if ((entry.Principal == access.Principal) && ((current == key) || entry.Inherit))
            {
                if (entry.Deny)
                {
                    deny |= entry.Rights;
                }
                else
                {
                    allow |= entry.Rights;
                }
            }
        }
        if (current == 0U)
        {
            return static_cast<UInt8>(allow & ~deny & access.ViewRights & AllRights);
        }
        if (ancestor.Parent >= KeyCapacity)
        {
            return 0U;
        }
        current = ancestor.Parent;
    }
    return 0U;
}
Status Database::Find(const Snapshot& state, const TextView path, Size& key) noexcept
{
    if (!ValidPath(path))
    {
        return Status::Invalid;
    }
    Size current{0U};
    Size start{1U};
    for (Size at{1U}; at <= path.Length; ++at)
    {
        if ((at == path.Length) || (path.Bytes[at] == '/'))
        {
            if (path.Length == 1U)
            {
                break;
            }
            Size child{KeyCapacity};
            for (Size i{1U}; i < KeyCapacity; ++i)
            {
                const Key& candidate = state.Keys[i];
                if (candidate.Used && (candidate.Parent == current) && (candidate.NameLength == at - start) &&
                    Same(candidate.Name, path.Bytes + start, at - start))
                {
                    child = i;
                    break;
                }
            }
            if (child == KeyCapacity)
            {
                return Status::NotFound;
            }
            current = child;
            start = at + 1U;
        }
    }
    key = current;
    return Status::Ok;
}
bool Database::Traversable(const Size key, const Access access) const noexcept
{
    Size current{key};
    for (Size depth{0U}; depth <= MaximumDepth; ++depth)
    {
        if ((EffectiveRights(m_state, current, access) & ReadRight) == 0U)
        {
            return false;
        }
        if (current == 0U)
        {
            return true;
        }
        current = m_state.Keys[current].Parent;
    }
    return false;
}
Status Database::Lookup(const Access access, const TextView path, const UInt8 right, Size& key) const noexcept
{
    if (!m_open)
    {
        return Status::Closed;
    }
    if ((access.ViewRights & ~AllRights) != 0U)
    {
        return Status::Invalid;
    }
    // Check each existing prefix before probing the next component, so a
    // denied ancestor cannot expose existence through NotFound versus Denied.
    if (!ValidPath(path))
    {
        return Status::Invalid;
    }
    Size prefix{1U};
    Size current{0U};
    for (Size step{0U}; step <= MaximumDepth; ++step)
    {
        const Status result = Find(m_state, {path.Bytes, prefix}, current);
        if (result != Status::Ok)
        {
            return result;
        }
        if ((EffectiveRights(m_state, current, access) & ReadRight) == 0U)
        {
            return Status::Denied;
        }
        if (prefix == path.Length)
        {
            if ((EffectiveRights(m_state, current, access) & right) != right)
            {
                return Status::Denied;
            }
            key = current;
            return Status::Ok;
        }
        Size next{prefix == 1U ? 1U : prefix + 1U};
        for (; next < path.Length; ++next)
        {
            if (path.Bytes[next] == '/')
            {
                break;
            }
        }
        prefix = next;
    }
    return Status::Invalid;
}
Status Database::Create(const Access access, const TextView path) noexcept
{
    if (!m_open)
    {
        return Status::Closed;
    }
    if (!ValidPath(path))
    {
        return Status::Invalid;
    }
    if (path.Length == 1U)
    {
        Size root{0U};
        const Status result = Lookup(access, path, CreateRight, root);
        return result == Status::Ok ? Status::Exists : result;
    }
    Size nameStart{1U};
    for (Size at{1U}; at < path.Length; ++at)
    {
        if (path.Bytes[at] == '/')
        {
            nameStart = at + 1U;
        }
    }
    Size parent{0U};
    const Status result = Lookup(access, {path.Bytes, nameStart == 1U ? 1U : nameStart - 1U}, CreateRight, parent);
    if (result != Status::Ok)
    {
        return result;
    }
    Size existing{0U};
    if (Find(m_state, path, existing) == Status::Ok)
    {
        return Status::Exists;
    }
    Size free{KeyCapacity};
    for (Size i{1U}; i < KeyCapacity; ++i)
    {
        if (!m_state.Keys[i].Used)
        {
            free = i;
            break;
        }
    }
    if (free == KeyCapacity)
    {
        return Status::Full;
    }
    m_candidate = m_state;
    Key& key = m_candidate.Keys[free];
    ResetKey(key);
    key.Used = true;
    key.Parent = static_cast<UInt8>(parent);
    key.NameLength = static_cast<UInt8>(path.Length - nameStart);
    for (Size i{0U}; i < key.NameLength; ++i)
    {
        key.Name[i] = path.Bytes[nameStart + i];
    }
    return Commit(path, {}, ChangeKind::KeyCreated);
}
Size Database::FindValue(const Snapshot& state, const Size key, const TextView name) noexcept
{
    for (Size i{0U}; i < ValueCapacity; ++i)
    {
        const NamedValue& value = state.Values[i];
        if (value.Used && (value.KeyIndex == key) && (value.NameLength == name.Length) &&
            Same(value.Name, name.Bytes, name.Length))
        {
            return i;
        }
    }
    return ValueCapacity;
}
Status Database::Read(const Access access, const TextView path, const TextView name, Value& value) const noexcept
{
    Size key{0U};
    const Status result = Lookup(access, path, ReadRight, key);
    if (result != Status::Ok)
    {
        return result;
    }
    if (!FormatCodec::ValidValueName(name))
    {
        return Status::Invalid;
    }
    const Size index = FindValue(m_state, key, name);
    if (index == ValueCapacity)
    {
        return Status::NotFound;
    }
    value = m_state.Values[index].Data;
    return Status::Ok;
}
Status Database::Write(const Access access, const TextView path, const TextView name, const Value& value) noexcept
{
    Size key{0U};
    const Status result = Lookup(access, path, WriteRight, key);
    if (result != Status::Ok)
    {
        return result;
    }
    if (!FormatCodec::ValidValueName(name) || !FormatCodec::ValidValue(value.Type, value.Bytes, value.Length))
    {
        return Status::Invalid;
    }
    Size index = FindValue(m_state, key, name);
    if (index == ValueCapacity)
    {
        Size count{0U};
        for (Size i{0U}; i < ValueCapacity; ++i)
        {
            if (m_state.Values[i].Used && (m_state.Values[i].KeyIndex == key))
            {
                ++count;
            }
            if (!m_state.Values[i].Used && (index == ValueCapacity))
            {
                index = i;
            }
        }
        if ((count == ValuesPerKey) || (index == ValueCapacity))
        {
            return Status::Full;
        }
    }
    m_candidate = m_state;
    NamedValue& entry = m_candidate.Values[index];
    ResetValue(entry);
    entry.Used = true;
    entry.KeyIndex = static_cast<UInt8>(key);
    entry.NameLength = static_cast<UInt8>(name.Length);
    for (Size i{0U}; i < name.Length; ++i)
    {
        entry.Name[i] = name.Bytes[i];
    }
    entry.Data = value;
    for (Size i{value.Length}; i < MaximumValueBytes; ++i)
    {
        entry.Data.Bytes[i] = 0U;
    }
    return Commit(path, name, ChangeKind::ValueWritten);
}
Status Database::DeleteValue(const Access access, const TextView path, const TextView name) noexcept
{
    Size key{0U};
    const Status result = Lookup(access, path, DeleteRight, key);
    if (result != Status::Ok)
    {
        return result;
    }
    if (!FormatCodec::ValidValueName(name))
    {
        return Status::Invalid;
    }
    const Size index = FindValue(m_state, key, name);
    if (index == ValueCapacity)
    {
        return Status::NotFound;
    }
    m_candidate = m_state;
    ResetValue(m_candidate.Values[index]);
    return Commit(path, name, ChangeKind::ValueDeleted);
}
Status Database::EnumerateValue(const Access access, const TextView path, const Size ordinal, Name& name,
                                ValueType& type) const noexcept
{
    Size key{0U};
    const Status result = Lookup(access, path, ReadRight, key);
    if (result != Status::Ok)
    {
        return result;
    }
    Size found{0U};
    for (const auto& value : m_state.Values)
    {
        if (!value.Used || (value.KeyIndex != key))
        {
            continue;
        }
        if (found++ == ordinal)
        {
            CopyName(name, {value.Name, value.NameLength});
            type = value.Data.Type;
            return Status::Ok;
        }
    }
    return Status::NotFound;
}
Status Database::EnumerateSubkey(const Access access, const TextView path, const Size ordinal,
                                 Name& name) const noexcept
{
    Size key{0U};
    const Status result = Lookup(access, path, ReadRight, key);
    if (result != Status::Ok)
    {
        return result;
    }
    Size found{0U};
    for (Size i{1U}; i < KeyCapacity; ++i)
    {
        const Key& child = m_state.Keys[i];
        if (!child.Used || (child.Parent != key) || !Traversable(i, access))
        {
            continue;
        }
        if (found++ == ordinal)
        {
            CopyName(name, {child.Name, child.NameLength});
            return Status::Ok;
        }
    }
    return Status::NotFound;
}
Status Database::Delete(const Access access, const TextView path) noexcept
{
    Size key{0U};
    const Status result = Lookup(access, path, DeleteRight, key);
    if (result != Status::Ok)
    {
        return result;
    }
    if (key == 0U)
    {
        return Status::Invalid;
    }
    for (const auto& child : m_state.Keys)
    {
        if (child.Used && (child.Parent == key))
        {
            return Status::NotEmpty;
        }
    }
    for (auto& watcher : m_watchers)
    {
        watcher.DeleteAllowed = watcher.Used && WatchReadable(watcher, path);
    }
    m_candidate = m_state;
    ResetKey(m_candidate.Keys[key]);
    for (auto& value : m_candidate.Values)
    {
        if (value.Used && (value.KeyIndex == key))
        {
            ResetValue(value);
        }
    }
    return Commit(path, {}, ChangeKind::KeyDeleted);
}
Status Database::GetAcl(const Access access, const TextView path, AclEntry* const entries, const Size capacity,
                        Size& count) const noexcept
{
    Size key{0U};
    const Status result = Lookup(access, path, ReadRight, key);
    if (result != Status::Ok)
    {
        return result;
    }
    const Key& found = m_state.Keys[key];
    if ((capacity < found.AclCount) || ((entries == nullptr) && (found.AclCount != 0U)))
    {
        return Status::Invalid;
    }
    for (Size i{0U}; i < found.AclCount; ++i)
    {
        entries[i] = found.Acl[i];
    }
    count = found.AclCount;
    return Status::Ok;
}
Status Database::SetAcl(const Access access, const TextView path, const AclEntry* const entries,
                        const Size count) noexcept
{
    Size key{0U};
    const Status result = Lookup(access, path, ChangeAclRight, key);
    if (result != Status::Ok)
    {
        return result;
    }
    if (!FormatCodec::ValidAcl(entries, count))
    {
        return Status::Invalid;
    }
    m_candidate = m_state;
    Key& found = m_candidate.Keys[key];
    found.AclCount = static_cast<UInt8>(count);
    for (Size i{0U}; i < AclCapacity; ++i)
    {
        if (i < count)
        {
            found.Acl[i] = entries[i];
        }
        else
        {
            found.Acl[i] = {};
        }
    }
    return Commit(path, {}, ChangeKind::AclChanged);
}
Status Database::Commit(const TextView path, const TextView name, const ChangeKind kind) noexcept
{
    if (m_generation == ~UInt64{0U})
    {
        return Status::Exhausted;
    }
    const UInt8 slot = static_cast<UInt8>(1U - m_slot);
    const UInt64 generation = m_generation + 1U;
    if (!FormatCodec::Encode(m_candidate, generation, slot, m_wire, SlotBytes))
    {
        return Status::Invalid;
    }
    const UInt64 base = static_cast<UInt64>(slot) * SlotBytes;
    const UInt8 invalidSeal[SealBytes]{};
    if (!m_storage.Write(m_storage.Context, base + SealOffset, invalidSeal, SealBytes) ||
        !m_storage.Flush(m_storage.Context) || !m_storage.Write(m_storage.Context, base, m_wire, BodyBytes) ||
        !m_storage.Flush(m_storage.Context) ||
        !m_storage.Write(m_storage.Context, base + SealOffset, m_wire + SealOffset, SealBytes) ||
        !m_storage.Flush(m_storage.Context))
    {
        Close();
        return Status::IoError;
    }
    m_state = m_candidate;
    m_slot = slot;
    m_generation = generation;
    Notify(path, name, kind);
    return Status::Ok;
}
bool Database::WatchReadable(const Watcher& watcher, const TextView path) const noexcept
{
    Size key{0U};
    return (Find(m_state, path, key) == Status::Ok) && Traversable(key, watcher.Caller);
}
void Database::Notify(const TextView path, const TextView name, const ChangeKind kind) noexcept
{
    for (auto& watcher : m_watchers)
    {
        if (!watcher.Used)
        {
            continue;
        }
        if (kind == ChangeKind::AclChanged)
        {
            watcher.First = 0U;
            watcher.Count = 0U;
            watcher.Overflow = WatchReadable(watcher, View(watcher.Root));
        }
        if (!WatchReadable(watcher, View(watcher.Root)) ||
            !((kind == ChangeKind::KeyDeleted) ? (watcher.DeleteAllowed && WatchReadable(watcher, Parent(path)))
                                               : WatchReadable(watcher, path)))
        {
            continue;
        }
        const Size length = watcher.Root.Length;
        const bool match =
            (path.Length >= length) && Same(path.Bytes, watcher.Root.Bytes, length) &&
            ((path.Length == length) || (watcher.Subtree && ((length == 1U) || (path.Bytes[length] == '/'))));
        if (!match || (watcher.Overflow && (kind != ChangeKind::AclChanged)))
        {
            continue;
        }
        if (watcher.Count == QueueCapacity)
        {
            watcher.First = 0U;
            watcher.Count = 0U;
            watcher.Overflow = true;
            continue;
        }
        Change& change = watcher.Queue[(watcher.First + watcher.Count) % QueueCapacity];
        CopyPath(change.KeyPath, path);
        CopyName(change.ValueName, name);
        change.Kind = kind;
        ++watcher.Count;
    }
}
Status Database::Watch(const Access access, const TextView path, const bool subtree, UInt64& handle) noexcept
{
    Size key{0U};
    const Status result = Lookup(access, path, ReadRight, key);
    if (result != Status::Ok)
    {
        return result;
    }
    if (m_nextHandle == ~UInt64{0U})
    {
        return Status::Exhausted;
    }
    for (auto& watcher : m_watchers)
    {
        if (watcher.Used)
        {
            continue;
        }
        watcher.Used = true;
        watcher.Subtree = subtree;
        watcher.Caller = access;
        watcher.Count = 0U;
        watcher.First = 0U;
        watcher.Overflow = false;
        watcher.Handle = m_nextHandle++;
        CopyPath(watcher.Root, path);
        handle = watcher.Handle;
        return Status::Ok;
    }
    return Status::Full;
}
Status Database::Poll(const UInt64 handle, Change& change) noexcept
{
    if (!m_open)
    {
        return Status::Closed;
    }
    for (auto& watcher : m_watchers)
    {
        if (!watcher.Used || (watcher.Handle != handle))
        {
            continue;
        }
        if (!WatchReadable(watcher, View(watcher.Root)))
        {
            watcher.Count = 0U;
            watcher.Overflow = false;
            return Status::Denied;
        }
        if (watcher.Overflow)
        {
            watcher.Overflow = false;
            return Status::RescanRequired;
        }
        for (Size step{0U}; (step < QueueCapacity) && (watcher.Count != 0U); ++step)
        {
            const Change& next = watcher.Queue[watcher.First];
            watcher.First = (watcher.First + 1U) % QueueCapacity;
            --watcher.Count;
            const TextView path = View(next.KeyPath);
            if (WatchReadable(watcher, (next.Kind == ChangeKind::KeyDeleted) ? Parent(path) : path))
            {
                change = next;
                return Status::Ok;
            }
        }
        return Status::Empty;
    }
    return Status::NotFound;
}
Status Database::Unwatch(const UInt64 handle) noexcept
{
    if (!m_open)
    {
        return Status::Closed;
    }
    for (auto& watcher : m_watchers)
    {
        if (watcher.Used && (watcher.Handle == handle))
        {
            watcher.Used = false;
            return Status::Ok;
        }
    }
    return Status::NotFound;
}
} // namespace Asteria::User::Settings
