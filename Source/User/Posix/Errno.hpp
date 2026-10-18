// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_POSIX_ERRNO_HPP_INCLUDED
#define ASTERIA_USER_POSIX_ERRNO_HPP_INCLUDED

#include "User/Runtime/Status.hpp"

namespace Asteria::User::Posix
{
/// Pure translation using the stable Asteria libc ABI numbers; never changes errno storage.
[[nodiscard]] int ToErrno(Runtime::Status status) noexcept;
[[nodiscard]] int ErrnoGeneric(Runtime::Status status) noexcept;
[[nodiscard]] int ErrnoKernel(Runtime::Status status) noexcept;
[[nodiscard]] int ErrnoMemory(Runtime::Status status) noexcept;
[[nodiscard]] int ErrnoTime(Runtime::Status status) noexcept;
[[nodiscard]] int ErrnoConsole(Runtime::Status status) noexcept;
[[nodiscard]] int ErrnoFile(Runtime::Status status) noexcept;
[[nodiscard]] int ErrnoDatagram(Runtime::Status status) noexcept;
[[nodiscard]] int ErrnoIpc(Runtime::Status status) noexcept;
[[nodiscard]] int ErrnoProcess(Runtime::Status status) noexcept;
} // namespace Asteria::User::Posix

#endif
