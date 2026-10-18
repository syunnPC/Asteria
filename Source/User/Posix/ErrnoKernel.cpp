// SPDX-License-Identifier: Apache-2.0

#include "User/Posix/Errno.hpp"

#include "User/Libc/Include/errno.h"
#include "User/Runtime/KernelStatus.hpp"

namespace Asteria::User::Posix
{
int ErrnoKernel(const Runtime::Status status) noexcept
{
    const auto decoded = status.TryAs<Runtime::KernelStatus>();
    if (!decoded.HasValue())
    {
        return EIO;
    }
    switch (decoded.GetValue())
    {
    case Runtime::KernelStatus::UnknownSystemCall:
        return ENOSYS;
    case Runtime::KernelStatus::InvalidArguments:
        return EINVAL;
    case Runtime::KernelStatus::InvalidConfiguration:
        return EINVAL;
    case Runtime::KernelStatus::InvalidHandle:
        return EBADF;
    case Runtime::KernelStatus::ObjectNotFound:
        return ENOENT;
    case Runtime::KernelStatus::ResourceExhausted:
        return ENOMEM;
    case Runtime::KernelStatus::SpaceExhausted:
        return ENOSPC;
    case Runtime::KernelStatus::RightsDenied:
        return EACCES;
    case Runtime::KernelStatus::RightsAmplification:
        return EACCES;
    case Runtime::KernelStatus::NoAttenuation:
        return EINVAL;
    case Runtime::KernelStatus::TypeMismatch:
        return EINVAL;
    case Runtime::KernelStatus::Revoked:
        return ESTALE;
    case Runtime::KernelStatus::Expired:
        return ESTALE;
    case Runtime::KernelStatus::LeaseExtension:
        return EINVAL;
    case Runtime::KernelStatus::RevocationDomainExhausted:
        return ENOSPC;
    case Runtime::KernelStatus::WouldBlock:
        return EAGAIN;
    case Runtime::KernelStatus::UserMemoryFault:
        return EFAULT;
    case Runtime::KernelStatus::TimedOut:
        return ETIMEDOUT;
    case Runtime::KernelStatus::ReservedMemory:
    case Runtime::KernelStatus::DisplayReserved:
    case Runtime::KernelStatus::BridgeReserved:
    case Runtime::KernelStatus::WriteValueDenied:
        return EACCES;
    }
    return EIO;
}
} // namespace Asteria::User::Posix
