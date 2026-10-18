// SPDX-License-Identifier: Apache-2.0

#include "User/Posix/Errno.hpp"

#include "User/Libc/Include/errno.h"
#include "User/Runtime/IpcStatus.hpp"

namespace Asteria::User::Posix
{
int ErrnoIpc(const Runtime::Status status) noexcept
{
    const auto decoded = status.TryAs<Runtime::IpcStatus>();
    if (!decoded.HasValue())
    {
        return EIO;
    }
    switch (decoded.GetValue()) {}
    return EIO;
}
} // namespace Asteria::User::Posix
