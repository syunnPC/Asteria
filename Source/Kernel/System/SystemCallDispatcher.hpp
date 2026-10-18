// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_SYSTEM_SYSTEM_CALL_DISPATCHER_HPP_INCLUDED
#define ASTERIA_KERNEL_SYSTEM_SYSTEM_CALL_DISPATCHER_HPP_INCLUDED

#include "Abi/SystemCallAbi.hpp"
#include "Kernel/Object/CapabilitySpace.hpp"

namespace Asteria::Kernel::System
{
using Abi::SystemCallArgumentCount;
using Abi::SystemCallDeriveChildRevocationDomain;
using Abi::SystemCallError;
using Abi::SystemCallNumber;
using Abi::SystemCallQueryObjectTypeMask;
using Abi::SystemCallQueryRightsShift;
using Abi::SystemCallRequest;
using Abi::SystemCallResponse;

/// Dispatches the initial capability-only syscall surface. This is the post-ingress capability
/// primitive. Production native entry must call SystemCallIngress with the owning processor run
/// loop; it must not call this function with caller-selected authority. Direct callers are limited
/// to lower-layer tests and trusted kernel composition that has already resolved the exact
/// CapabilitySpace. Calls may spin on the shared capability registry lock and are forbidden from
/// ISR or same-CPU reentry. The dispatcher performs no allocation and retains no lease across
/// return.
class SystemCallDispatcher final
{
public:
    SystemCallDispatcher() = delete;

    [[nodiscard]] static SystemCallResponse Dispatch(Object::CapabilitySpace& caller,
                                                     const SystemCallRequest& request) noexcept;

    /// ThreadExit never reaches Dispatch, which answers it UnknownSystemCall: the platform ends the
    /// calling thread's run Exited. On true, exitCode is argument 0; a code with bit 63 set (the
    /// kernel's own) or a nonzero argument 1 to 5 is false, which the platform answers
    /// InvalidArguments.
    [[nodiscard]] static bool DecodeThreadExit(const SystemCallRequest& request, Foundation::UInt64& exitCode) noexcept;

private:
    [[nodiscard]] static SystemCallResponse DispatchClose(Object::CapabilitySpace& caller,
                                                          const SystemCallRequest& request) noexcept;
    [[nodiscard]] static SystemCallResponse DispatchQuery(Object::CapabilitySpace& caller,
                                                          const SystemCallRequest& request) noexcept;
    [[nodiscard]] static SystemCallResponse DispatchDerive(Object::CapabilitySpace& caller,
                                                           const SystemCallRequest& request) noexcept;
    [[nodiscard]] static SystemCallResponse DispatchDuplicate(Object::CapabilitySpace& caller,
                                                              const SystemCallRequest& request) noexcept;
    [[nodiscard]] static SystemCallResponse DispatchRevoke(Object::CapabilitySpace& caller,
                                                           const SystemCallRequest& request) noexcept;
    [[nodiscard]] static SystemCallResponse DispatchGrant(Object::CapabilitySpace& caller,
                                                          const SystemCallRequest& request) noexcept;
};

} // Namespace Asteria::Kernel::System

#endif // ASTERIA_KERNEL_SYSTEM_SYSTEM_CALL_DISPATCHER_HPP_INCLUDED
