// SPDX-License-Identifier: Apache-2.0
#include "User/Settings/MemoryStorage.hpp"
#include "Support/TestContext.hpp"
#include <cstdio>
#include <string>
namespace S = Asteria::User::Settings;
namespace H = Asteria::TestSupport::Settings;
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt64;
using Asteria::Foundation::UInt8;
H::MemoryStorage medium;
S::Database db;
S::Value value;
S::Value output;
S::Change event;
S::Snapshot aclState;
int main()
{
    Asteria::TestSupport::TestContext t{"SettingsDatabaseTests"};
    ASTERIA_EXPECT(t, "open never provisions empty storage", db.Open(medium.Interface()) == S::Status::Corrupt,
                   db.Generation() == 0U);
    ASTERIA_EXPECT(t, "explicit provisioning", db.Format(medium.Interface(), &H::RootAcl, 1U) == S::Status::Ok,
                   db.Generation() == 1U);
    ASTERIA_EXPECT(t, "format refuses existing media",
                   db.Format(medium.Interface(), &H::RootAcl, 1U) == S::Status::Exists,
                   db.Open(medium.Interface()) == S::Status::Ok);
    UInt64 handle{0U};
    UInt64 exact{0U};
    ASTERIA_EXPECT(t, "watch root and create hierarchy",
                   db.Watch(H::Owner, H::View("/"), true, handle) == S::Status::Ok,
                   db.Create(H::Owner, H::View("/a")) == S::Status::Ok,
                   db.Create(H::Owner, H::View("/a/\xE6\x97\xA5")) == S::Status::Ok,
                   db.Watch(H::Owner, H::View("/a"), false, exact) == S::Status::Ok);
    ASTERIA_EXPECT(t, "creation paths in commit order", db.Poll(handle, event) == S::Status::Ok,
                   event.KeyPath.Length == 2U, db.Poll(handle, event) == S::Status::Ok, event.KeyPath.Length == 6U,
                   db.Poll(handle, event) == S::Status::Empty);
    value.Type = S::ValueType::Integer;
    value.Length = 8U;
    std::memset(value.Bytes, 0xFF, value.Length);
    ASTERIA_EXPECT(t, "negative integer", db.Write(H::Owner, H::View("/a"), H::View(""), value) == S::Status::Ok,
                   db.Read(H::Owner, H::View("/a"), H::View(""), output) == S::Status::Ok, H::Equal(value, output));
    value.Type = S::ValueType::StringList;
    value.Length = 11U;
    const UInt8 list[11U]{3U, 0U, 0U, 0U, 3U, 0U, 0xE6U, 0x97U, 0xA5U, 0U, 0U};
    std::memcpy(value.Bytes, list, sizeof(list));
    ASTERIA_EXPECT(t, "string list preserves empty items",
                   db.Write(H::Owner, H::View("/a/\xE6\x97\xA5"), H::View(""), value) == S::Status::Ok,
                   db.Read(H::Owner, H::View("/a/\xE6\x97\xA5"), H::View(""), output) == S::Status::Ok,
                   H::Equal(value, output));
    ASTERIA_EXPECT(t, "exact watch excludes descendants", db.Poll(exact, event) == S::Status::Ok,
                   event.KeyPath.Length == 2U, db.Poll(exact, event) == S::Status::Empty);
    value.Type = S::ValueType::Boolean;
    value.Length = 1U;
    value.Bytes[0U] = 2U;
    const auto before = db.Generation();
    ASTERIA_EXPECT(
        t, "invalid value is atomic", db.Write(H::Owner, H::View("/a"), H::View(""), value) == S::Status::Invalid,
        db.Generation() == before, db.Read(H::Owner, H::View("/a/\xE6\x97\xA5"), H::View(""), output) == S::Status::Ok,
        output.Type == S::ValueType::StringList);
    const char* invalid[]{
        "", "a", "//", "/a/", "/.", "/..", "/a//b", "/\xC0\xAF", "/\xED\xA0\x80", "/\xF4\x90\x80\x80", "/\xE2\x82"};
    bool rejected{true};
    for (const char* path : invalid)
    {
        rejected &= db.Create(H::Owner, H::View(path)) == S::Status::Invalid;
    }
    const UInt8 nul[4U]{'/', 'x', 0U, 'y'};
    rejected &= db.Create(H::Owner, {nul, sizeof(nul)}) == S::Status::Invalid;
    ASTERIA_EXPECT(t, "UTF-8 and path grammar", rejected, db.Generation() == before);
    while (db.Poll(handle, event) == S::Status::Ok)
    {
    }
    ASTERIA_EXPECT(t, "leaf delete, root and children protected",
                   db.Delete(H::Owner, H::View("/")) == S::Status::Invalid,
                   db.Delete(H::Owner, H::View("/a")) == S::Status::NotEmpty,
                   db.Delete(H::Owner, H::View("/a/\xE6\x97\xA5")) == S::Status::Ok);
    ASTERIA_EXPECT(t, "deletion names deleted key", db.Poll(handle, event) == S::Status::Ok, event.KeyPath.Length == 6U,
                   event.Kind == S::ChangeKind::KeyDeleted, db.Poll(handle, event) == S::Status::Empty);
    value.Type = S::ValueType::None;
    value.Length = 0U;
    std::memset(value.Bytes, 0xAA, sizeof(value.Bytes));
    ASTERIA_EXPECT(t, "unused value bytes are canonicalized",
                   db.Write(H::Owner, H::View("/a"), H::View(""), value) == S::Status::Ok,
                   db.Read(H::Owner, H::View("/a"), H::View(""), output) == S::Status::Ok, output.Bytes[0U] == 0U,
                   output.Bytes[S::MaximumValueBytes - 1U] == 0U);
    bool overflow{true};
    for (Size i{0U}; i <= S::QueueCapacity; ++i)
    {
        overflow &= db.Write(H::Owner, H::View("/a"), H::View(""), value) == S::Status::Ok;
    }
    ASTERIA_EXPECT(t, "bounded overflow explicitly requests rescan", overflow,
                   db.Poll(handle, event) == S::Status::RescanRequired, db.Poll(handle, event) == S::Status::Empty);
    S::AclEntry grants[2U]{H::RootAcl, {2U, S::ReadRight, false, true}};
    UInt64 reader{0U};
    ASTERIA_EXPECT(t, "principal and view ceilings", db.SetAcl(H::Owner, H::View("/"), grants, 2U) == S::Status::Ok,
                   db.Watch({2U, S::ReadRight}, H::View("/a"), true, reader) == S::Status::Ok,
                   db.Write({1U, S::ReadRight}, H::View("/a"), H::View(""), value) == S::Status::Denied,
                   db.Write({2U, S::AllRights}, H::View("/a"), H::View(""), value) == S::Status::Denied,
                   db.Read({3U, S::AllRights}, H::View("/a"), H::View(""), output) == S::Status::Denied,
                   db.Read({3U, S::AllRights}, H::View("/missing"), H::View(""), output) == S::Status::Denied);
    ASTERIA_EXPECT(t, "ACL revocation purges queued paths",
                   db.Write(H::Owner, H::View("/a"), H::View(""), value) == S::Status::Ok,
                   db.SetAcl(H::Owner, H::View("/"), &H::RootAcl, 1U) == S::Status::Ok,
                   db.Poll(reader, event) == S::Status::Denied, db.Poll(handle, event) == S::Status::RescanRequired);
    S::AclEntry acl[S::AclCapacity]{};
    Size count{99U};
    ASTERIA_EXPECT(t, "ACL read and failed output atomicity",
                   db.GetAcl(H::Owner, H::View("/"), acl, S::AclCapacity, count) == S::Status::Ok, count == 1U,
                   acl[0U].Principal == 1U, db.GetAcl(H::Owner, H::View("/"), nullptr, 0U, count) == S::Status::Invalid,
                   count == 1U);
    const auto oldHandle = handle;
    ASTERIA_EXPECT(t, "recovery preserves data, discards watches, avoids handle reuse",
                   db.Open(medium.Interface()) == S::Status::Ok, db.Poll(oldHandle, event) == S::Status::NotFound,
                   db.Read(H::Owner, H::View("/a"), H::View(""), output) == S::Status::Ok,
                   output.Type == S::ValueType::None, db.Watch(H::Owner, H::View("/"), true, handle) == S::Status::Ok,
                   handle != oldHandle);
    bool watches{true};
    for (Size i{1U}; i < S::WatchCapacity; ++i)
    {
        watches &= db.Watch(H::Owner, H::View("/"), true, exact) == S::Status::Ok;
    }
    ASTERIA_EXPECT(t, "watch capacity and unregister", watches,
                   db.Watch(H::Owner, H::View("/"), true, exact) == S::Status::Full,
                   db.Unwatch(handle) == S::Status::Ok, db.Unwatch(handle) == S::Status::NotFound);
    std::string deep;
    bool depth{true};
    for (Size i{0U}; i < S::MaximumDepth; ++i)
    {
        deep.push_back('/');
        deep.append(S::MaximumNameBytes, 'z');
        depth &= db.Create(H::Owner, H::View(deep.c_str())) == S::Status::Ok;
    }
    ASTERIA_EXPECT(t, "maximum UTF-8 byte length and depth accepted", depth, deep.size() == S::MaximumPathBytes);
    deep += "/z";
    ASTERIA_EXPECT(t, "depth beyond bound refused", db.Create(H::Owner, H::View(deep.c_str())) == S::Status::Invalid);
    std::string longName{"/"};
    longName.append(S::MaximumNameBytes + 1U, 'z');
    ASTERIA_EXPECT(t, "name beyond bound refused",
                   db.Create(H::Owner, H::View(longName.c_str())) == S::Status::Invalid);
    Size made{0U};
    for (Size i{0U}; i < S::KeyCapacity; ++i)
    {
        const std::string name = "/n" + std::to_string(i);
        if (db.Create(H::Owner, H::View(name.c_str())) == S::Status::Ok)
        {
            ++made;
        }
    }
    ASTERIA_EXPECT(t, "key bound, slot reuse", made == S::KeyCapacity - 10U,
                   db.Create(H::Owner, H::View("/full")) == S::Status::Full,
                   db.Delete(H::Owner, H::View("/n0")) == S::Status::Ok,
                   db.Create(H::Owner, H::View("/replacement")) == S::Status::Ok);

    // Named/default values share key ACLs, but not names with subkeys.
    ASTERIA_EXPECT(t, "named values independent from default and subkeys",
                   db.Write(H::Owner, H::View("/a"), H::View("colour"), value) == S::Status::Ok,
                   db.Write(H::Owner, H::View("/a"), H::View("Colour"), value) == S::Status::Ok,
                   db.Write(H::Owner, H::View("/a"), H::View("/../日"), value) == S::Status::Ok);
    S::Name name;
    S::ValueType type{S::ValueType::Boolean};
    ASTERIA_EXPECT(t, "separate value enumeration includes empty default",
                   db.EnumerateValue(H::Owner, H::View("/a"), 0U, name, type) == S::Status::Ok, name.Length == 0U,
                   db.EnumerateValue(H::Owner, H::View("/a"), 1U, name, type) == S::Status::Ok, name.Length == 6U,
                   db.EnumerateValue(H::Owner, H::View("/a"), 4U, name, type) == S::Status::NotFound, name.Length == 6U,
                   db.EnumerateSubkey(H::Owner, H::View("/a"), 0U, name) == S::Status::NotFound);
    ASTERIA_EXPECT(t, "ACL and view apply to every named value",
                   db.Read({2U, S::AllRights}, H::View("/a"), H::View("colour"), output) == S::Status::Denied,
                   db.DeleteValue({1U, S::ReadRight}, H::View("/a"), H::View("colour")) == S::Status::Denied,
                   db.DeleteValue(H::Owner, H::View("/a"), H::View("colour")) == S::Status::Ok,
                   db.Read(H::Owner, H::View("/a"), H::View("colour"), output) == S::Status::NotFound,
                   db.Read(H::Owner, H::View("/a"), H::View("Colour"), output) == S::Status::Ok);
    bool valueBound{true};
    for (Size i{3U}; i < S::ValuesPerKey; ++i)
    {
        const auto text = std::to_string(i);
        valueBound &= db.Write(H::Owner, H::View("/a"), H::View(text.c_str()), value) == S::Status::Ok;
    }
    ASTERIA_EXPECT(t, "per-key value bound and update at capacity", valueBound,
                   db.Write(H::Owner, H::View("/a"), H::View("full"), value) == S::Status::Full,
                   db.Write(H::Owner, H::View("/a"), H::View("Colour"), value) == S::Status::Ok);
    ASTERIA_EXPECT(t, "value-name UTF-8 and NUL bounds",
                   db.Write(H::Owner, H::View("/a"), {nul, 4U}, value) == S::Status::Invalid,
                   db.Write(H::Owner, H::View("/a"), H::View(longName.c_str()), value) == S::Status::Invalid,
                   db.Write(H::Owner, H::View("/a"), H::View("\xED\xA0\x80"), value) == S::Status::Invalid);

    UInt64 namedWatch{0U};
    ASTERIA_EXPECT(t, "notifications identify exact key and value",
                   db.Watch(H::Owner, H::View("/a"), false, namedWatch) == S::Status::Ok,
                   db.Write(H::Owner, H::View("/a"), H::View("Colour"), value) == S::Status::Ok,
                   db.Poll(namedWatch, event) == S::Status::Ok, event.Kind == S::ChangeKind::ValueWritten,
                   event.KeyPath.Length == 2U, event.ValueName.Length == 6U,
                   std::memcmp(event.ValueName.Bytes, "Colour", 6U) == 0,
                   db.DeleteValue(H::Owner, H::View("/a"), H::View("Colour")) == S::Status::Ok,
                   db.Poll(namedWatch, event) == S::Status::Ok, event.Kind == S::ChangeKind::ValueDeleted,
                   event.ValueName.Length == 6U);
    ASTERIA_EXPECT(t, "key and value namespaces independent",
                   db.Write(H::Owner, H::View("/"), H::View("replacement"), value) == S::Status::Ok,
                   db.Read(H::Owner, H::View("/"), H::View("replacement"), output) == S::Status::Ok,
                   db.Read(H::Owner, H::View("/replacement"), H::View(""), output) == S::Status::NotFound);
    ASTERIA_EXPECT(t, "no administrator or take-ownership bypass",
                   db.Read({0U, S::AllRights}, H::View("/a"), H::View(""), output) == S::Status::Denied,
                   db.SetAcl({0U, S::AllRights}, H::View("/a"), &H::RootAcl, 1U) == S::Status::Denied);

    // Independent evaluation table covers every requested mask and view mask.
    aclState.Keys[0U].Used = true;
    aclState.Keys[0U].AclCount = 2U;
    aclState.Keys[0U].Acl[0U] = {7U, S::AllRights, false, true};
    aclState.Keys[0U].Acl[1U] = {7U, S::WriteRight, true, true};
    aclState.Keys[1U].Used = true;
    aclState.Keys[1U].AclCount = 2U;
    aclState.Keys[1U].Acl[0U] = {7U, S::WriteRight, false, false}; // inherited deny wins
    aclState.Keys[1U].Acl[1U] = {7U, S::DeleteRight, true, false};
    aclState.Keys[2U].Used = true;
    aclState.Keys[2U].Parent = 1U;
    bool table{true};
    for (UInt8 view{0U}; view <= S::AllRights; ++view)
    {
        table &= S::Database::EffectiveRights(aclState, 0U, {7U, view}) == (view & 29U);
        table &= S::Database::EffectiveRights(aclState, 1U, {7U, view}) == (view & 21U);
        table &= S::Database::EffectiveRights(aclState, 2U, {7U, view}) == (view & 29U);
        table &= S::Database::EffectiveRights(aclState, 2U, {8U, view}) == 0U;
    }
    aclState.Keys[0U].Acl[0U].Inherit = false;
    table &= S::Database::EffectiveRights(aclState, 2U, {7U, S::AllRights}) == 0U;
    aclState.Keys[2U].Parent = 2U;
    table &= S::Database::EffectiveRights(aclState, 2U, {7U, S::AllRights}) == 0U;
    ASTERIA_EXPECT(t, "ACL table: deny-wins, local/inherited, view ceiling, default deny, cycles", table);
    medium.Reset();
    bool pool = db.Format(medium.Interface(), &H::RootAcl, 1U) == S::Status::Ok;
    for (Size k{0U}; k < S::ValueCapacity / S::ValuesPerKey; ++k)
    {
        const auto path = "/pool" + std::to_string(k);
        pool &= db.Create(H::Owner, H::View(path.c_str())) == S::Status::Ok;
        for (Size v{0U}; v < S::ValuesPerKey; ++v)
        {
            const auto text = "v" + std::to_string(v);
            pool &= db.Write(H::Owner, H::View(path.c_str()), H::View(text.c_str()), value) == S::Status::Ok;
        }
    }
    ASTERIA_EXPECT(t, "global named-value pool full without affecting updates", pool,
                   db.Create(H::Owner, H::View("/spare")) == S::Status::Ok,
                   db.Write(H::Owner, H::View("/spare"), H::View(""), value) == S::Status::Full,
                   db.Write(H::Owner, H::View("/pool0"), H::View("v0"), value) == S::Status::Ok);
    const std::string maximumName(S::MaximumNameBytes, 'z');
    ASTERIA_EXPECT(t, "leaf deletion frees all values, maximum value name persists",
                   db.Delete(H::Owner, H::View("/pool0")) == S::Status::Ok,
                   db.Create(H::Owner, H::View("/pool0")) == S::Status::Ok,
                   db.Read(H::Owner, H::View("/pool0"), H::View("v0"), output) == S::Status::NotFound,
                   db.Write(H::Owner, H::View("/"), H::View(maximumName.c_str()), value) == S::Status::Ok,
                   db.Open(medium.Interface()) == S::Status::Ok,
                   db.Read(H::Owner, H::View("/"), H::View(maximumName.c_str()), output) == S::Status::Ok);
    const S::AclEntry hidden{2U, S::ReadRight, true, true};
    ASTERIA_EXPECT(t, "enumeration hides denied subkeys and exposes no values",
                   db.SetAcl(H::Owner, H::View("/"), grants, 2U) == S::Status::Ok,
                   db.SetAcl(H::Owner, H::View("/pool1"), &hidden, 1U) == S::Status::Ok,
                   db.EnumerateValue({2U, S::AllRights}, H::View("/pool1"), 0U, name, type) == S::Status::Denied,
                   db.EnumerateSubkey({2U, S::ReadRight}, H::View("/"), 7U, name) == S::Status::Ok,
                   db.EnumerateSubkey({2U, S::ReadRight}, H::View("/"), 8U, name) == S::Status::NotFound);
    db.Close();
    ASTERIA_EXPECT(t, "closed instance refuses work",
                   db.Read(H::Owner, H::View("/"), H::View(""), output) == S::Status::Closed);
    return t.Finish();
}
