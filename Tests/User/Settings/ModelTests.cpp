// SPDX-License-Identifier: Apache-2.0
#include "User/Settings/MemoryStorage.hpp"
#include "Support/Random.hpp"
#include "Support/TestContext.hpp"
#include <map>
#include <set>
#include <string>
#include <vector>
#include <cstdio>
namespace S = Asteria::User::Settings;
namespace H = Asteria::TestSupport::Settings;
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt8;
H::MemoryStorage medium;
S::Database db;
S::Value input;
S::Value output;
struct ModelValue
{
    S::ValueType Type{S::ValueType::None};
    std::vector<UInt8> Bytes;
};
struct ModelKey
{
    std::map<std::string, ModelValue> Values;
    std::vector<S::AclEntry> Acl;
};
using Model = std::map<std::string, ModelKey>;
std::string Parent(const std::string& path)
{
    const auto last = path.rfind('/');
    return last == 0U ? "/" : path.substr(0U, last);
}
UInt8 Rights(const Model& model, const std::string& path, const S::Access caller)
{
    UInt8 allow{0U};
    UInt8 deny{0U};
    std::string at = path;
    for (Size depth{0U}; depth <= S::MaximumDepth; ++depth)
    {
        const auto found = model.find(at);
        if (found == model.end())
        {
            return 0U;
        }
        for (const auto& ace : found->second.Acl)
        {
            if ((ace.Principal == caller.Principal) && ((at == path) || ace.Inherit))
            {
                if (ace.Deny)
                {
                    deny |= ace.Rights;
                }
                else
                {
                    allow |= ace.Rights;
                }
            }
        }
        if (at == "/")
        {
            return static_cast<UInt8>(allow & ~deny & caller.ViewRights);
        }
        at = Parent(at);
    }
    return 0U;
}
S::Status Lookup(const Model& model, const std::string& path, const S::Access caller, const UInt8 right)
{
    std::string at = "/";
    for (Size part{0U}; part <= S::MaximumDepth; ++part)
    {
        if (!model.contains(at))
        {
            return S::Status::NotFound;
        }
        if ((Rights(model, at, caller) & S::ReadRight) == 0U)
        {
            return S::Status::Denied;
        }
        if (at == path)
        {
            return ((Rights(model, at, caller) & right) == right) ? S::Status::Ok : S::Status::Denied;
        }
        const Size start = at == "/" ? 1U : at.size() + 1U;
        const auto end = path.find('/', start);
        at = path.substr(0U, end);
    }
    return S::Status::Invalid;
}
void MakeValue(Asteria::TestSupport::Random& random)
{
    input.Type = static_cast<S::ValueType>(random.Below(6U));
    std::memset(input.Bytes, 0, sizeof(input.Bytes));
    switch (input.Type)
    {
    case S::ValueType::None:
        input.Length = 0U;
        break;
    case S::ValueType::Integer:
        input.Length = 8U;
        for (Size i{0U}; i < 8U; ++i)
        {
            input.Bytes[i] = static_cast<UInt8>(random.Next());
        }
        break;
    case S::ValueType::Boolean:
        input.Length = 1U;
        input.Bytes[0U] = static_cast<UInt8>(random.Below(2U));
        break;
    case S::ValueType::String:
        input.Length = 6U;
        std::memcpy(input.Bytes, "a\0\xE6\x97\xA5z", 6U);
        break;
    case S::ValueType::Binary:
        input.Length = static_cast<Asteria::Foundation::UInt16>(random.Below(S::MaximumValueBytes + 1U));
        for (Size i{0U}; i < input.Length; ++i)
        {
            input.Bytes[i] = static_cast<UInt8>(random.Next());
        }
        break;
    case S::ValueType::StringList:
        input.Length = 8U;
        input.Bytes[0U] = 2U;
        input.Bytes[2U] = 2U;
        input.Bytes[4U] = 'x';
        input.Bytes[5U] = 0U;
        break;
    }
}
int main()
{
    Asteria::TestSupport::TestContext t{"SettingsModelTests"};
    const auto seeds = Asteria::TestSupport::GetSeedRange(0x534554U, 8U);
    const char* paths[]{"/",
                        "/a",
                        "/A",
                        "/a/A",
                        "/b",
                        "/c",
                        "/d",
                        "/a/a",
                        "/a/b",
                        "/a/a/a",
                        "/b/a",
                        "/b/b",
                        "/c/a",
                        "/d/a",
                        "/\xE6\x97\xA5",
                        "/\xE6\x97\xA5/a",
                        "/ab"};
    const char* names[]{"", "colour", "Colour", "a", "日", "/value", "e\xCC\x81", "\xC3\xA9"};
    for (auto seed{seeds.First}; seed < seeds.First + seeds.Count; ++seed)
    {
        Asteria::TestSupport::Random random{seed};
        Model model;
        model["/"].Acl.push_back(H::RootAcl);
        medium.Reset();
        bool good = db.Format(medium.Interface(), &H::RootAcl, 1U) == S::Status::Ok;
        Asteria::Foundation::UInt64 generation{1U};
        for (Size operation{0U}; (operation < 400U) && good; ++operation)
        {
            const std::string path = paths[random.Below(sizeof(paths) / sizeof(paths[0U]))];
            const std::string valueName = names[random.Below(sizeof(names) / sizeof(names[0U]))];
            S::Access caller{random.Below(3U) + 1U, static_cast<UInt8>(random.Below(32U))};
            if (random.Chance(1U, 2U))
            {
                caller = H::Owner;
            }
            const auto kind = random.Below(6U);
            S::Status expected;
            S::Status actual;
            if (kind == 0U)
            {
                expected = Lookup(model, path == "/" ? "/" : Parent(path), caller, S::CreateRight);
                if (expected == S::Status::Ok)
                {
                    if (model.contains(path))
                    {
                        expected = S::Status::Exists;
                    }
                    else
                    {
                        model[path] = {};
                    }
                }
                actual = db.Create(caller, H::View(path.c_str()));
            }
            else if (kind == 1U)
            {
                MakeValue(random);
                expected = Lookup(model, path, caller, S::WriteRight);
                if (expected == S::Status::Ok)
                {
                    auto& values = model[path].Values;
                    Size total{0U};
                    for (const auto& [key, node] : model)
                    {
                        static_cast<void>(key);
                        total += node.Values.size();
                    }
                    if (!values.contains(valueName) &&
                        ((values.size() == S::ValuesPerKey) || (total == S::ValueCapacity)))
                    {
                        expected = S::Status::Full;
                    }
                    else
                    {
                        values[valueName].Type = input.Type;
                        values[valueName].Bytes.assign(input.Bytes, input.Bytes + input.Length);
                    }
                }
                actual = db.Write(caller, H::View(path.c_str()), H::View(valueName.c_str()), input);
            }
            else if (kind == 2U)
            {
                expected = Lookup(model, path, caller, S::ReadRight);
                output.Length = 17U;
                if ((expected == S::Status::Ok) && !model[path].Values.contains(valueName))
                {
                    expected = S::Status::NotFound;
                }
                actual = db.Read(caller, H::View(path.c_str()), H::View(valueName.c_str()), output);
                if ((actual == S::Status::Ok) && (expected == actual))
                {
                    const auto& wanted = model.at(path).Values.at(valueName);
                    good &= (output.Type == wanted.Type) && (output.Length == wanted.Bytes.size()) &&
                            std::equal(wanted.Bytes.begin(), wanted.Bytes.end(), output.Bytes);
                }
                else
                {
                    good &= output.Length == 17U;
                }
            }
            else if (kind == 3U)
            {
                expected = Lookup(model, path, caller, S::DeleteRight);
                if (expected == S::Status::Ok)
                {
                    if (path == "/")
                    {
                        expected = S::Status::Invalid;
                    }
                    else
                    {
                        for (const auto& [name, node] : model)
                        {
                            static_cast<void>(node);
                            if ((name != "/") && (Parent(name) == path))
                            {
                                expected = S::Status::NotEmpty;
                            }
                        }
                        if (expected == S::Status::Ok)
                        {
                            model.erase(path);
                        }
                    }
                }
                actual = db.Delete(caller, H::View(path.c_str()));
            }
            else if (kind == 4U)
            {
                expected = Lookup(model, path, caller, S::DeleteRight);
                if (expected == S::Status::Ok)
                {
                    if (model[path].Values.erase(valueName) == 0U)
                    {
                        expected = S::Status::NotFound;
                    }
                }
                actual = db.DeleteValue(caller, H::View(path.c_str()), H::View(valueName.c_str()));
            }
            else
            {
                const S::AclEntry entries[3U]{
                    H::RootAcl,
                    {2U, static_cast<UInt8>(random.Below(31U) + 1U), random.Chance(1U, 2U), random.Chance(1U, 2U)},
                    {3U, static_cast<UInt8>(random.Below(31U) + 1U), random.Chance(1U, 2U), random.Chance(1U, 2U)}};
                expected = Lookup(model, path, caller, S::ChangeAclRight);
                if (expected == S::Status::Ok)
                {
                    model[path].Acl.assign(entries, entries + 3U);
                }
                actual = db.SetAcl(caller, H::View(path.c_str()), entries, 3U);
            }
            if ((expected == S::Status::Ok) && (kind != 2U))
            {
                ++generation;
            }
            good &= actual == expected;
            good &= db.Generation() == generation;
            if (!good)
            {
                std::fprintf(stderr, "seed %llu operation %zu kind %llu path %s expected %u actual %u\n",
                             static_cast<unsigned long long>(seed), operation, static_cast<unsigned long long>(kind),
                             path.c_str(), static_cast<unsigned>(expected), static_cast<unsigned>(actual));
                break;
            }
            if ((operation % 29U) == 0U)
            {
                good &= db.Open(medium.Interface()) == S::Status::Ok;
            }
            for (const auto& [name, node] : model)
            {
                for (const auto& [valueKey, data] : node.Values)
                {
                    good &=
                        db.Read(H::Owner, H::View(name.c_str()), H::View(valueKey.c_str()), output) == S::Status::Ok;
                    good &= (output.Type == data.Type) && (output.Length == data.Bytes.size()) &&
                            std::equal(data.Bytes.begin(), data.Bytes.end(), output.Bytes);
                }
                std::set<std::string> values, children;
                S::Name enumerated;
                S::ValueType type{S::ValueType::None};
                for (Size ordinal{0U}; ordinal <= S::ValuesPerKey; ++ordinal)
                {
                    const auto result = db.EnumerateValue(H::Owner, H::View(name.c_str()), ordinal, enumerated, type);
                    if (ordinal == node.Values.size())
                    {
                        good &= result == S::Status::NotFound;
                        break;
                    }
                    good &= result == S::Status::Ok;
                    const std::string key{reinterpret_cast<const char*>(enumerated.Bytes), enumerated.Length};
                    good &= node.Values.contains(key) && values.insert(key).second;
                    if (node.Values.contains(key))
                    {
                        good &= type == node.Values.at(key).Type;
                    }
                }
                for (Size ordinal{0U}; ordinal < S::KeyCapacity; ++ordinal)
                {
                    const auto result = db.EnumerateSubkey(H::Owner, H::View(name.c_str()), ordinal, enumerated);
                    if (result == S::Status::NotFound)
                    {
                        break;
                    }
                    good &= result == S::Status::Ok;
                    children.insert(std::string{reinterpret_cast<const char*>(enumerated.Bytes), enumerated.Length});
                }
                std::set<std::string> wantedChildren;
                for (const auto& [child, childNode] : model)
                {
                    static_cast<void>(childNode);
                    if ((child != "/") && (Parent(child) == name))
                    {
                        wantedChildren.insert(child.substr(child.rfind('/') + 1U));
                    }
                }
                good &= values.size() == node.Values.size();
                good &= children == wantedChildren;
                S::AclEntry entries[S::AclCapacity]{};
                Size count{0U};
                good &= db.GetAcl(H::Owner, H::View(name.c_str()), entries, S::AclCapacity, count) == S::Status::Ok;
                good &= count == node.Acl.size();
                for (Size i{0U}; i < count; ++i)
                {
                    good &= (entries[i].Principal == node.Acl[i].Principal) &&
                            (entries[i].Rights == node.Acl[i].Rights) && (entries[i].Deny == node.Acl[i].Deny) &&
                            (entries[i].Inherit == node.Acl[i].Inherit);
                }
            }
        }
        ASTERIA_EXPECT(t, "seeded reference hierarchy, typed values, principal ACLs and recovery", good);
    }
    return t.Finish();
}
