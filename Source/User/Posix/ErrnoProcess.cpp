// SPDX-License-Identifier: Apache-2.0

#include "User/Posix/Errno.hpp"

#include "User/Libc/Include/errno.h"
#include "User/Runtime/ProcessStatus.hpp"

namespace Asteria::User::Posix
{
int ErrnoProcess(const Runtime::Status status) noexcept
{
    const auto decoded = status.TryAs<Runtime::ProcessStatus>();
    if (!decoded.HasValue())
    {
        return EIO;
    }
    switch (decoded.GetValue()) {}
    return EIO;
}
} // namespace Asteria::User::Posix
