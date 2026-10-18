// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_DIRECTORY_DIRECTORY_ABI_HPP_INCLUDED
#define ASTERIA_USER_DIRECTORY_DIRECTORY_ABI_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::User::Directory
{
/// The sealed selector of the Directory's Endpoint, which the root service creates and the
/// Directory requires of every request, the Directory's generation, the tag of its Endpoint's
/// readiness in its WaitSet, and the code it exits with when a wait or a receive fails.
inline constexpr Foundation::UInt64 DirectorySealedSelector{0x4449'5245'4354'4F52ULL};
inline constexpr Foundation::UInt64 DirectoryGeneration{1U};
inline constexpr Foundation::UInt64 DirectoryWaitTag{0x4449'5230U};
inline constexpr Foundation::UInt64 DirectoryFailedExitCode{0x0000'0000'D1EC'00E1ULL};
} // Namespace Asteria::User::Directory

#endif // ASTERIA_USER_DIRECTORY_DIRECTORY_ABI_HPP_INCLUDED
