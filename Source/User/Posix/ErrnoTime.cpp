// SPDX-License-Identifier: Apache-2.0

#include "User/Posix/Errno.hpp"

#include "User/Libc/Include/errno.h"
#include "User/Runtime/TimeStatus.hpp"

namespace Asteria::User::Posix
{
int ErrnoTime(const Runtime::Status status) noexcept
{
    const auto decoded = status.TryAs<Runtime::TimeStatus>();
    if (!decoded.HasValue())
    {
        return EIO;
    }
    switch (decoded.GetValue()) {}
    return EIO;
}
} // namespace Asteria::User::Posix
