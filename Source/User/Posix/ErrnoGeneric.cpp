// SPDX-License-Identifier: Apache-2.0

#include "User/Posix/Errno.hpp"

#include "User/Libc/Include/errno.h"
#include "User/Runtime/GenericStatus.hpp"

namespace Asteria::User::Posix
{
int ErrnoGeneric(const Runtime::Status status) noexcept
{
    const auto decoded = status.TryAs<Runtime::GenericStatus>();
    if (!decoded.HasValue())
    {
        return EIO;
    }
    switch (decoded.GetValue())
    {
    case Runtime::GenericStatus::Restarted:
        return EINTR;
    case Runtime::GenericStatus::Pending:
        return EAGAIN;
    case Runtime::GenericStatus::RefusedForCredit:
        return EAGAIN;
    case Runtime::GenericStatus::StaleObject:
        return ESTALE;
    case Runtime::GenericStatus::StateLost:
        return EIO;
    case Runtime::GenericStatus::RebindRequired:
        return ENOTCONN;
    case Runtime::GenericStatus::CommitStatusUnknown:
        return EIO;
    case Runtime::GenericStatus::TooLate:
        return EALREADY;
    case Runtime::GenericStatus::NotSupported:
        return EOPNOTSUPP;
    case Runtime::GenericStatus::InvalidArgument:
        return EINVAL;
    case Runtime::GenericStatus::OutOfResources:
        return ENOMEM;
    case Runtime::GenericStatus::AccessDenied:
        return EACCES;
    }
    return EIO;
}
} // namespace Asteria::User::Posix
