// SPDX-License-Identifier: Apache-2.0

#include "User/Posix/Errno.hpp"

#include "User/Libc/Include/errno.h"
#include "User/Runtime/GenericStatus.hpp"
#include "User/Runtime/KernelStatus.hpp"
#include "User/Runtime/MemoryStatus.hpp"
#include "User/Runtime/TimeStatus.hpp"
#include "User/Runtime/ConsoleStatus.hpp"
#include "User/Runtime/FileStatus.hpp"
#include "User/Runtime/DatagramStatus.hpp"
#include "User/Runtime/IpcStatus.hpp"
#include "User/Runtime/ProcessStatus.hpp"

namespace Asteria::User::Posix
{
int ToErrno(const Runtime::Status status) noexcept
{
    if (status.IsSuccess())
    {
        return 0;
    }
    if (status.GetFacility() == Runtime::GenericFacility)
    {
        return ErrnoGeneric(status);
    }
    if (status.GetFacility() == Runtime::KernelFacility)
    {
        return ErrnoKernel(status);
    }
    if (status.GetFacility() == Runtime::MemoryFacility)
    {
        return ErrnoMemory(status);
    }
    if (status.GetFacility() == Runtime::TimeFacility)
    {
        return ErrnoTime(status);
    }
    if (status.GetFacility() == Runtime::ConsoleFacility)
    {
        return ErrnoConsole(status);
    }
    if (status.GetFacility() == Runtime::FileFacility)
    {
        return ErrnoFile(status);
    }
    if (status.GetFacility() == Runtime::DatagramFacility)
    {
        return ErrnoDatagram(status);
    }
    if (status.GetFacility() == Runtime::IpcFacility)
    {
        return ErrnoIpc(status);
    }
    if (status.GetFacility() == Runtime::ProcessFacility)
    {
        return ErrnoProcess(status);
    }
    return EIO;
}
} // namespace Asteria::User::Posix
