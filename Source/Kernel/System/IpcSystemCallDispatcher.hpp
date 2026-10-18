// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_SYSTEM_IPC_SYSTEM_CALL_DISPATCHER_HPP_INCLUDED
#define ASTERIA_KERNEL_SYSTEM_IPC_SYSTEM_CALL_DISPATCHER_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/System/SystemCallContext.hpp"
#include "Kernel/System/SystemCallDispatcher.hpp"

namespace Asteria::Kernel::Ipc
{
enum class EndpointErrorCode : Foundation::UInt8;
}

namespace Asteria::Kernel::Memory
{
enum class UserMemoryAccessError : Foundation::UInt8;
}

namespace Asteria::Kernel::System
{
enum class IpcSystemCallDispatchError : Foundation::UInt8
{
    None,
    InvalidContext,
    EndpointProtocolViolation,
    UserMemoryProtocolViolation,
    /// The object management could neither complete nor undo an
    /// ObjectCreate or ObjectDestroy.
    ObjectManagementViolation,
};

struct IpcSystemCallDispatchFailure
{
    IpcSystemCallDispatchError Error;
    Ipc::EndpointErrorCode Endpoint;
    Memory::UserMemoryAccessError UserMemory;
    Foundation::UInt8 Reserved;
};

/// Dispatches the system calls that need the caller's user memory or the
/// context's collaborators: the bounded Endpoint send/receive subset,
/// DebugWrite, FaultResolve, ObjectCreate, ObjectDestroy, MemoryMap,
/// MemoryUnmap, ThreadTerminate, WaitSetBind, DeviceControl, IommuControl,
/// InterruptControl and PortIo.
class IpcSystemCallDispatcher final
{
public:
    IpcSystemCallDispatcher() = delete;

    [[nodiscard]] static constexpr bool IsIpcSystemCall(const Foundation::UInt64 number) noexcept
    {
        return (number == static_cast<Foundation::UInt64>(SystemCallNumber::IpcSend)) ||
               (number == static_cast<Foundation::UInt64>(SystemCallNumber::IpcReceive));
    }

    /// True for every system call that Dispatch handles with the caller context
    /// and its user-memory adapter and other collaborators.
    [[nodiscard]] static constexpr bool UsesUserMemory(const Foundation::UInt64 number) noexcept
    {
        return IsIpcSystemCall(number) || (number == static_cast<Foundation::UInt64>(SystemCallNumber::DebugWrite)) ||
               (number == static_cast<Foundation::UInt64>(SystemCallNumber::FaultResolve)) ||
               (number == static_cast<Foundation::UInt64>(SystemCallNumber::ObjectCreate)) ||
               (number == static_cast<Foundation::UInt64>(SystemCallNumber::ObjectDestroy)) ||
               (number == static_cast<Foundation::UInt64>(SystemCallNumber::MemoryMap)) ||
               (number == static_cast<Foundation::UInt64>(SystemCallNumber::MemoryUnmap)) ||
               (number == static_cast<Foundation::UInt64>(SystemCallNumber::ThreadTerminate)) ||
               (number == static_cast<Foundation::UInt64>(SystemCallNumber::WaitSetBind)) ||
               (number == static_cast<Foundation::UInt64>(SystemCallNumber::DeviceControl)) ||
               (number == static_cast<Foundation::UInt64>(SystemCallNumber::IommuControl)) ||
               (number == static_cast<Foundation::UInt64>(SystemCallNumber::InterruptControl)) ||
               (number == static_cast<Foundation::UInt64>(SystemCallNumber::PortIo));
    }

    [[nodiscard]] static Foundation::Result<SystemCallResponse, IpcSystemCallDispatchFailure>
    Dispatch(const SystemCallContext& context, const SystemCallRequest& request) noexcept;

private:
    [[nodiscard]] static Foundation::Result<SystemCallResponse, IpcSystemCallDispatchFailure>
    DispatchSend(const SystemCallContext& context, const SystemCallRequest& request) noexcept;
    [[nodiscard]] static Foundation::Result<SystemCallResponse, IpcSystemCallDispatchFailure>
    DispatchReceive(const SystemCallContext& context, const SystemCallRequest& request) noexcept;
    [[nodiscard]] static Foundation::Result<SystemCallResponse, IpcSystemCallDispatchFailure>
    DispatchFaultResolve(const SystemCallContext& context, const SystemCallRequest& request) noexcept;
    [[nodiscard]] static Foundation::Result<SystemCallResponse, IpcSystemCallDispatchFailure>
    DispatchFaultTerminate(const SystemCallContext& context, const SystemCallRequest& request) noexcept;
    [[nodiscard]] static Foundation::Result<SystemCallResponse, IpcSystemCallDispatchFailure>
    DispatchDebugWrite(const SystemCallContext& context, const SystemCallRequest& request) noexcept;
    [[nodiscard]] static Foundation::Result<SystemCallResponse, IpcSystemCallDispatchFailure>
    DispatchObjectCreate(const SystemCallContext& context, const SystemCallRequest& request) noexcept;
    [[nodiscard]] static Foundation::Result<SystemCallResponse, IpcSystemCallDispatchFailure>
    DispatchObjectDestroy(const SystemCallContext& context, const SystemCallRequest& request) noexcept;
    [[nodiscard]] static Foundation::Result<SystemCallResponse, IpcSystemCallDispatchFailure>
    DispatchThreadTerminate(const SystemCallContext& context, const SystemCallRequest& request) noexcept;
    [[nodiscard]] static Foundation::Result<SystemCallResponse, IpcSystemCallDispatchFailure>
    DispatchWaitSetBind(const SystemCallContext& context, const SystemCallRequest& request) noexcept;
    [[nodiscard]] static Foundation::Result<SystemCallResponse, IpcSystemCallDispatchFailure>
    DispatchDeviceControl(const SystemCallContext& context, const SystemCallRequest& request) noexcept;
    [[nodiscard]] static Foundation::Result<SystemCallResponse, IpcSystemCallDispatchFailure>
    DispatchIommuControl(const SystemCallContext& context, const SystemCallRequest& request) noexcept;
    [[nodiscard]] static Foundation::Result<SystemCallResponse, IpcSystemCallDispatchFailure>
    DispatchInterruptControl(const SystemCallContext& context, const SystemCallRequest& request) noexcept;
    [[nodiscard]] static Foundation::Result<SystemCallResponse, IpcSystemCallDispatchFailure>
    DispatchMemoryMap(const SystemCallContext& context, const SystemCallRequest& request) noexcept;
    [[nodiscard]] static Foundation::Result<SystemCallResponse, IpcSystemCallDispatchFailure>
    DispatchMemoryUnmap(const SystemCallContext& context, const SystemCallRequest& request) noexcept;
};

static_assert(sizeof(IpcSystemCallDispatchError) == 1U);
static_assert(sizeof(IpcSystemCallDispatchFailure) == 4U);
static_assert(alignof(IpcSystemCallDispatchFailure) == 1U);
} // namespace Asteria::Kernel::System

#endif // ASTERIA_KERNEL_SYSTEM_IPC_SYSTEM_CALL_DISPATCHER_HPP_INCLUDED
