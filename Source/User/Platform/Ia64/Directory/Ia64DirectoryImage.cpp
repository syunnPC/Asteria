// SPDX-License-Identifier: Apache-2.0

#include "User/Directory/DirectoryProgram.hpp"
#include "User/Platform/Ia64/System/Ia64SystemCallTransport.hpp"

// The image has no allocator; the service core is constructed in its.bss.
#if defined(ASTERIA_TARGET_IA64)
void* operator new(Asteria::Foundation::Size, void* const place) noexcept
{
    return place;
}
#else
#include <new>
#endif

namespace
{
// The Directory's state lives in.bss (the image has no initialized data):
// the service core, built at entry, and its buffers.
alignas(Asteria::Executive::ObjectDirectory::ObjectDirectoryServiceCore) constinit Asteria::Foundation::UInt8
    g_serviceStorage[sizeof(Asteria::Executive::ObjectDirectory::ObjectDirectoryServiceCore)]{};
constinit Asteria::User::Directory::DirectoryBuffers g_buffers{};
constinit Asteria::User::Directory::DirectoryCounters g_counters{};
} // Namespace

// The root service passes the start block's address in r4
// (Ia64DirectoryImage.S); ThreadExit returns only if refused.
extern "C" void AsteriaIa64DirectoryImageMain(const Asteria::Foundation::UInt64 startBlockAddress) noexcept
{
    const auto transport = Asteria::User::Platform::Ia64::System::GetIa64SystemCallTransport();
    const auto& startBlock =
        *reinterpret_cast<const Asteria::Foundation::UInt64(*)[Asteria::Abi::ComponentStartBlockWords]>(
            static_cast<Asteria::Foundation::UIntPointer>(startBlockAddress));
    auto* const service =
        ::new (static_cast<void*>(g_serviceStorage)) Asteria::Executive::ObjectDirectory::ObjectDirectoryServiceCore{
            Asteria::User::Directory::MakeDirectoryConfiguration()};
    const Asteria::Foundation::UInt64 code =
        Asteria::User::Directory::RunDirectory(transport, startBlock, *service, g_buffers, g_counters);
    const Asteria::User::System::SystemCallClient client{transport};
    for (;;)
    {
        static_cast<void>(client.ExitThread(code));
    }
}
