// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_IDENTITY_SUPPORT_HPP_INCLUDED
#define ASTERIA_TESTS_IDENTITY_SUPPORT_HPP_INCLUDED
#include "User/Identity/Identity.hpp"
#include "Foundation/Crc32c.hpp"
#include "Support/TestContext.hpp"
#include <cstring>
#include <algorithm>
namespace IdentityTest
{
using Asteria::User::Identity::Administrator;
using Asteria::User::Identity::BodyBytes;
using Asteria::User::Identity::Clock;
using Asteria::User::Identity::Everyone;
using Asteria::User::Identity::Name;
using Asteria::User::Identity::Role;
using Asteria::User::Identity::SealOffset;
using Asteria::User::Identity::Size;
using Asteria::User::Identity::Snapshot;
using Asteria::User::Identity::Storage;
using Asteria::User::Identity::StorageBytes;
using Asteria::User::Identity::TextView;
using Asteria::User::Identity::TimeTicksPerSecond;
using Asteria::User::Identity::UInt64;
using Asteria::User::Identity::UInt8;
namespace C = Asteria::User::Crypto;
inline TextView View(const char* s) noexcept
{
    return {reinterpret_cast<const UInt8*>(s), std::strlen(s)};
}
inline void Put(UInt8* p, Size n, UInt64 v) noexcept
{
    for (Size i = 0U; i < n; ++i)
    {
        p[i] = static_cast<UInt8>(v >> (8U * i));
    }
}
inline UInt64 Get(const UInt8* p, Size n) noexcept
{
    UInt64 v = 0U;
    for (Size i = 0U; i < n; ++i)
    {
        v |= static_cast<UInt64>(p[i]) << (8U * i);
    }
    return v;
}
inline bool Zero(const void* p, Size n) noexcept
{
    const auto* b = static_cast<const UInt8*>(p);
    for (Size i = 0U; i < n; ++i)
    {
        if (b[i] != 0U)
        {
            return false;
        }
    }
    return true;
}
inline void NameOf(Name& n, const char* s) noexcept
{
    n = {};
    n.Length = static_cast<UInt8>(std::strlen(s));
    std::memcpy(n.Bytes, s, n.Length);
}
inline void Fresh(Snapshot& s) noexcept
{
    s = {};
    s.Accounts[0].Info.Id = Administrator;
    s.Accounts[0].Info.Revision = 1U;
    s.Accounts[0].Info.AccountRole = Role::Admin;
    NameOf(s.Accounts[0].Info.Login, "administrator");
    for (Size i = 0; i < 3U; ++i)
    {
        s.Groups[i].Id = Everyone + i;
        s.Groups[i].Revision = 1U;
    }
    NameOf(s.Groups[0].Label, "Everyone");
    NameOf(s.Groups[1].Label, "Users");
    NameOf(s.Groups[2].Label, "Administrators");
}
inline void Repair(UInt8* bytes) noexcept
{
    Put(bytes + 44U, 4U, 0U);
    auto crc = Asteria::Foundation::Crc32c::Compute(bytes, BodyBytes);
    Put(bytes + 44U, 4U, crc);
    Put(bytes + SealOffset + 16U, 4U, crc);
    Put(bytes + SealOffset + 24U, 4U, Asteria::Foundation::Crc32c::Compute(bytes + SealOffset, 24U));
}
struct Memory
{
    UInt8 Bytes[StorageBytes]{};
    UInt8 Durable[StorageBytes]{};
    UInt8 Trace[6U][StorageBytes]{};
    Size Calls{0U};
    Size Fail{0U};
    Size Prefix{0U};
    bool Capture{false};
    bool FailRead{false};
    Storage Interface() noexcept
    {
        return {this, StorageBytes, Read, Write, Flush};
    }
    static bool Read(void* p, UInt64 off, UInt8* out, Size n) noexcept
    {
        auto& s = *static_cast<Memory*>(p);
        if (s.FailRead || off > StorageBytes || n > StorageBytes - off)
        {
            return false;
        }
        std::memcpy(out, s.Bytes + off, n);
        return true;
    }
    void Record() noexcept
    {
        if (Capture && Calls <= 6U)
        {
            std::memcpy(Trace[Calls - 1U], Bytes, StorageBytes);
        }
    }
    static bool Write(void* p, UInt64 off, const UInt8* in, Size n) noexcept
    {
        auto& s = *static_cast<Memory*>(p);
        ++s.Calls;
        if (off > StorageBytes || n > StorageBytes - off)
        {
            return false;
        }
        bool fail = s.Calls == s.Fail;
        std::memcpy(s.Bytes + off, in, fail ? std::min(n, s.Prefix) : n);
        s.Record();
        return !fail;
    }
    static bool Flush(void* p) noexcept
    {
        auto& s = *static_cast<Memory*>(p);
        ++s.Calls;
        bool fail = s.Calls == s.Fail;
        if (!fail)
        {
            std::memcpy(s.Durable, s.Bytes, StorageBytes);
        }
        s.Record();
        return !fail;
    }
    void Reset() noexcept
    {
        std::memset(static_cast<void*>(this), 0, sizeof(*this));
    }
};
struct Time
{
    UInt64 Seconds{0U};
    bool Available{true};
    bool RawTicks{false};
    Clock Interface() noexcept
    {
        return {this, Read};
    }
    static bool Read(void* p, UInt64& out) noexcept
    {
        auto& t = *static_cast<Time*>(p);
        if (!t.Available)
        {
            return false;
        }
        out = t.RawTicks ? t.Seconds : t.Seconds * TimeTicksPerSecond;
        return true;
    }
};
} // namespace IdentityTest
#endif
