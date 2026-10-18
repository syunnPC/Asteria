// SPDX-License-Identifier: Apache-2.0

#include "User/Posix/Errno.hpp"

#include "User/Libc/Include/errno.h"
#include "User/Runtime/FileStatus.hpp"

namespace Asteria::User::Posix
{
int ErrnoFile(const Runtime::Status status) noexcept
{
    const auto decoded = status.TryAs<Runtime::FileStatus>();
    if (!decoded.HasValue())
    {
        return EIO;
    }
    switch (decoded.GetValue())
    {
    case Runtime::FileStatus::NotFound:
        return ENOENT;
    case Runtime::FileStatus::Corrupt:
        return EUCLEAN;
    case Runtime::FileStatus::IoFailed:
        return EIO;
    case Runtime::FileStatus::Cancelled:
        return ECANCELED;
    }
    return EIO;
}
} // namespace Asteria::User::Posix
