// SPDX-License-Identifier: Apache-2.0

#include "User/Posix/Errno.hpp"

#include "User/Libc/Include/errno.h"
#include "User/Runtime/ConsoleStatus.hpp"

namespace Asteria::User::Posix
{
int ErrnoConsole(const Runtime::Status status) noexcept
{
    const auto decoded = status.TryAs<Runtime::ConsoleStatus>();
    if (!decoded.HasValue())
    {
        return EIO;
    }
    switch (decoded.GetValue())
    {
    case Runtime::ConsoleStatus::Refused:
        return EINVAL;
    }
    return EIO;
}
} // namespace Asteria::User::Posix
