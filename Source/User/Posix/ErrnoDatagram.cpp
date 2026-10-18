// SPDX-License-Identifier: Apache-2.0

#include "User/Posix/Errno.hpp"

#include "User/Libc/Include/errno.h"
#include "User/Runtime/DatagramStatus.hpp"

namespace Asteria::User::Posix
{
int ErrnoDatagram(const Runtime::Status status) noexcept
{
    const auto decoded = status.TryAs<Runtime::DatagramStatus>();
    if (!decoded.HasValue())
    {
        return EIO;
    }
    switch (decoded.GetValue())
    {
    case Runtime::DatagramStatus::NoRule:
        return EACCES;
    case Runtime::DatagramStatus::NotBound:
        return ENOTCONN;
    case Runtime::DatagramStatus::IdentityRefused:
        return EACCES;
    case Runtime::DatagramStatus::Malformed:
        return EINVAL;
    case Runtime::DatagramStatus::IoFailed:
        return EIO;
    }
    return EIO;
}
} // namespace Asteria::User::Posix
