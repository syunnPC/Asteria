// SPDX-License-Identifier: Apache-2.0

#include "User/Posix/Errno.hpp"

#include "User/Libc/Include/errno.h"
#include "User/Runtime/MemoryStatus.hpp"

namespace Asteria::User::Posix
{
int ErrnoMemory(const Runtime::Status status) noexcept
{
    const auto decoded = status.TryAs<Runtime::MemoryStatus>();
    if (!decoded.HasValue())
    {
        return EIO;
    }
    switch (decoded.GetValue()) {}
    return EIO;
}
} // namespace Asteria::User::Posix
