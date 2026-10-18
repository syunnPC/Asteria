// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <cstring>
#include "User/Identity/Identity.hpp"

// Deterministic, explicitly synthetic test data. This tool is never a provisioning API and its
// entropy is never deployment entropy.
namespace
{
using Asteria::User::Identity::AccountStore;
using Asteria::User::Identity::Administrator;
using Asteria::User::Identity::ArenaBlocks;
using Asteria::User::Identity::Authentication;
using Asteria::User::Identity::AuthenticationWorkspace;
using Asteria::User::Identity::PrincipalId;
using Asteria::User::Identity::Role;
using Asteria::User::Identity::Size;
using Asteria::User::Identity::Status;
using Asteria::User::Identity::StorageBytes;
using Asteria::User::Identity::TextView;
using Asteria::User::Identity::UInt64;
using Asteria::User::Identity::UInt8;
UInt8 g_disk[StorageBytes]{};
AccountStore g_store{};
AuthenticationWorkspace g_work{};
Asteria::User::Crypto::Argon2Block g_arena[ArenaBlocks]{};
UInt8 g_saltSequence{1U};
bool Read(void*, UInt64 offset, UInt8* out, Size length) noexcept
{
    if (offset > StorageBytes || length > StorageBytes - offset)
    {
        return false;
    }
    std::memcpy(out, g_disk + offset, length);
    return true;
}
bool Write(void*, UInt64 offset, const UInt8* bytes, Size length) noexcept
{
    if (offset > StorageBytes || length > StorageBytes - offset)
    {
        return false;
    }
    std::memcpy(g_disk + offset, bytes, length);
    return true;
}
bool Flush(void*) noexcept
{
    return true;
}
bool Salt(void*, UInt8* bytes, Size length) noexcept
{
    for (Size i{0U}; i < length; ++i)
    {
        bytes[i] = static_cast<UInt8>(g_saltSequence + i);
    }
    g_saltSequence = static_cast<UInt8>(g_saltSequence + 32U);
    return true;
}
TextView View(const char* text) noexcept
{
    return {reinterpret_cast<const UInt8*>(text), std::strlen(text)};
}
bool Enable(Authentication& auth, PrincipalId id, const char* name, Role role, const char* password) noexcept
{
    return auth.Enroll(g_store, id, View(password), {nullptr, Salt}) == Status::Ok &&
           g_store.UpdateAccount(id, View(name), role, false) == Status::Ok;
}
} // Namespace
int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fputs("usage: WriteIdentityFixture OUTPUT (synthetic test store)\n", stderr);
        return 2;
    }
    Authentication auth{g_work, g_arena, ArenaBlocks};
    PrincipalId user{0U};
    const bool ready = g_store.Format({nullptr, StorageBytes, Read, Write, Flush}) == Status::Ok &&
                       Enable(auth, Administrator, "administrator", Role::Admin, "Asteria-test-admin-036") &&
                       g_store.CreateAccount(View("test-user"), Role::Standard, user) == Status::Ok &&
                       Enable(auth, user, "test-user", Role::Standard, "Asteria-test-user-036") &&
                       g_store.Ready() == Status::Ok;
    if (!ready)
    {
        std::fputs("synthetic account fixture failed\n", stderr);
        return 1;
    }
    auto* const file = std::fopen(argv[1], "wb");
    if (file == nullptr)
    {
        return 1;
    }
    const bool written = std::fwrite(g_disk, 1U, sizeof(g_disk), file) == sizeof(g_disk);
    const bool closed = std::fclose(file) == 0;
    Asteria::User::Crypto::Wipe({g_disk, sizeof(g_disk)});
    g_store.Close();
    return written && closed ? 0 : 1;
}
