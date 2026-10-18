// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_IDENTITY_INTERNAL_HPP_INCLUDED
#define ASTERIA_USER_IDENTITY_INTERNAL_HPP_INCLUDED
#include "User/Identity/Identity.hpp"
namespace Asteria::User::Identity::Detail
{
[[nodiscard]] inline UInt64 Little(const UInt8* p, Size n) noexcept
{
    UInt64 v{0U};
    for (Size i{0U}; i < n; ++i)
    {
        v |= static_cast<UInt64>(p[i]) << (8U * i);
    }
    return v;
}
inline void Put(UInt8* p, Size n, UInt64 v) noexcept
{
    for (Size i{0U}; i < n; ++i)
    {
        p[i] = static_cast<UInt8>(v >> (8U * i));
    }
}
[[nodiscard]] inline bool Same(const UInt8* a, const UInt8* b, Size n) noexcept
{
    for (Size i{0U}; i < n; ++i)
    {
        if (a[i] != b[i])
        {
            return false;
        }
    }
    return true;
}
[[nodiscard]] inline bool Zero(const UInt8* p, Size n) noexcept
{
    for (Size i{0U}; i < n; ++i)
    {
        if (p[i] != 0U)
        {
            return false;
        }
    }
    return true;
}
inline void Copy(UInt8* a, const UInt8* b, Size n) noexcept
{
    for (Size i{0U}; i < n; ++i)
    {
        a[i] = b[i];
    }
}
inline void Clear(void* p, Size n) noexcept
{
    Crypto::Wipe({static_cast<UInt8*>(p), n});
}
inline void SetName(Name& name, TextView text) noexcept
{
    Clear(&name, sizeof(name));
    name.Length = static_cast<UInt8>(text.Length);
    Copy(name.Bytes, text.Bytes, text.Length);
}
[[nodiscard]] inline bool SameName(const Name& a, TextView b) noexcept
{
    return a.Length == b.Length && Same(a.Bytes, b.Bytes, b.Length);
}
[[nodiscard]] inline TextView View(const Name& n) noexcept
{
    return {n.Bytes, n.Length};
}
inline constexpr UInt8 EveryoneName[]{'E', 'v', 'e', 'r', 'y', 'o', 'n', 'e'};
inline constexpr UInt8 UsersName[]{'U', 's', 'e', 'r', 's'};
inline constexpr UInt8 AdministratorsName[]{'A', 'd', 'm', 'i', 'n', 'i', 's', 't', 'r', 'a', 't', 'o', 'r', 's'};
[[nodiscard]] inline TextView BuiltinName(PrincipalId id) noexcept
{
    if (id == Everyone)
    {
        return {EveryoneName, sizeof(EveryoneName)};
    }
    if (id == Users)
    {
        return {UsersName, sizeof(UsersName)};
    }
    return {AdministratorsName, sizeof(AdministratorsName)};
}
[[nodiscard]] inline bool Overlap(const void* a, Size an, const void* b, Size bn) noexcept
{
    const auto av = reinterpret_cast<UInt64>(a);
    const auto bv = reinterpret_cast<UInt64>(b);
    return an != 0U && bn != 0U && ((av <= bv) ? bv - av < an : av - bv < bn);
}
} // namespace Asteria::User::Identity::Detail
#endif
