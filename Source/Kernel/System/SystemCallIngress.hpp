// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_SYSTEM_SYSTEM_CALL_INGRESS_HPP_INCLUDED
#define ASTERIA_KERNEL_SYSTEM_SYSTEM_CALL_INGRESS_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Scheduler/ProcessorKernelRunLoop.hpp"
#include "Kernel/System/IpcSystemCallDispatcher.hpp"
#include "Kernel/System/SystemCallDispatcher.hpp"

namespace Asteria::Kernel::System
{
class WaitSetSystemCallIngress;

enum class SystemCallIngressError : Foundation::UInt8
{
    None,
    ProcessorRunLoopRejected,
    ProtectionDomainMissing,
    ProtectionDomainRejected,
    ProtectionDomainIdentityMismatch,
    CapabilitySpaceMissing,
    CapabilitySpaceNotReady,
    RecordedCapabilitySpaceIdentifierInvalid,
    LiveCapabilitySpaceIdentifierInvalid,
    CapabilitySpaceIdentifierMismatch,
    ActiveAddressSpaceMissing,
    ProtectionAddressSpaceMissing,
    ActiveAddressSpaceIdentityInvalid,
    ProtectionAddressSpaceIdentityInvalid,
    AddressSpaceStorageMismatch,
    AddressSpaceIdentityMismatch,
    ResourceDomainInvalid,
    UserMemoryNotReady,
    UserMemoryAddressSpaceInvalid,
    UserMemoryAddressSpaceMismatch,
    IpcDispatchRejected,
};

struct SystemCallIngressFailure
{
    SystemCallIngressError Error;
    Scheduler::ProcessorKernelRunLoopError ProcessorRunLoop;
    Protection::ProtectionDomainError ProtectionDomain;
    IpcSystemCallDispatchFailure Ipc;
    Foundation::UInt8 Reserved;
};

/// Resolves syscall authority exclusively from the owning CPU's active native
/// run-loop state. The caller disables local interrupts and prevents same-CPU
/// reentry for the whole operation. Dispatch may spin on the ProtectionDomain
/// and shared object-registry locks, does not allocate, and must not be called
/// from an ISR.
class SystemCallIngress final
{
public:
    SystemCallIngress() = delete;

    /// On true, response contains a completed user syscall, including a
    /// user-visible syscall error, and failure is unchanged. On false, response
    /// is unchanged and failure contains one canonical kernel ingress failure.
    /// After authority resolution, IPC calls complete with the canonical
    /// user-visible InvalidConfiguration response because this overload has no
    /// user-memory collaborators.
    template <typename TNativeContext>
    [[nodiscard]] static bool Dispatch(Scheduler::ProcessorKernelRunLoop<TNativeContext>& runLoop,
                                       const SystemCallRequest& request, SystemCallResponse& response,
                                       SystemCallIngressFailure& failure) noexcept
    {
        const auto active = runLoop.GetActiveProtectionDomainBinding();
        if (!active.HasValue())
        {
            WriteProcessorFailure(failure, active.GetError());
            return false;
        }
        return DispatchWithoutUserMemory(active.GetValue(), request, response, failure);
    }

    /// Dispatches with processor-owned user-copy and scratch collaborators.
    /// The collaborators must name the active AddressSpace and remain exclusive
    /// for the complete non-reentrant operation. pagerResolution is the pager
    /// resolution the active domain may use through FaultResolve, or null;
    /// objectManagement is what ObjectCreate and ObjectDestroy reach, or null.
    template <typename TNativeContext>
    [[nodiscard]] static bool
    Dispatch(Scheduler::ProcessorKernelRunLoop<TNativeContext>& runLoop, Memory::IUserMemoryAccess& userMemory,
             SystemCallScratch& scratch, const SystemCallRequest& request, SystemCallResponse& response,
             SystemCallIngressFailure& failure, Memory::IPagerResolution* const pagerResolution = nullptr,
             IObjectManagement* const objectManagement = nullptr) noexcept
    {
        const auto active = runLoop.GetActiveProtectionDomainBinding();
        if (!active.HasValue())
        {
            WriteProcessorFailure(failure, active.GetError());
            return false;
        }
        return DispatchWithUserMemory(active.GetValue(), userMemory, scratch, request, response, failure,
                                      pagerResolution, objectManagement);
    }

private:
    friend class WaitSetSystemCallIngress;

    static void WriteProcessorFailure(SystemCallIngressFailure& failure,
                                      Scheduler::ProcessorKernelRunLoopError error) noexcept;

    [[nodiscard]] static bool ResolveProtectionDomain(const Scheduler::ProcessorActiveProtectionDomainBinding& active,
                                                      Protection::ProtectionDomainBinding& protection,
                                                      Object::CapabilitySpace*& capabilities,
                                                      SystemCallIngressFailure& failure) noexcept;

    [[nodiscard]] static bool DispatchWithoutUserMemory(const Scheduler::ProcessorActiveProtectionDomainBinding& active,
                                                        const SystemCallRequest& request, SystemCallResponse& response,
                                                        SystemCallIngressFailure& failure) noexcept;

    [[nodiscard]] static bool DispatchWithUserMemory(const Scheduler::ProcessorActiveProtectionDomainBinding& active,
                                                     Memory::IUserMemoryAccess& userMemory, SystemCallScratch& scratch,
                                                     const SystemCallRequest& request, SystemCallResponse& response,
                                                     SystemCallIngressFailure& failure,
                                                     Memory::IPagerResolution* pagerResolution,
                                                     IObjectManagement* objectManagement) noexcept;
};

static_assert(sizeof(SystemCallIngressError) == 1U);
static_assert(sizeof(SystemCallIngressFailure) == 8U);
static_assert(alignof(SystemCallIngressFailure) == 1U);
} // namespace Asteria::Kernel::System

#endif // ASTERIA_KERNEL_SYSTEM_SYSTEM_CALL_INGRESS_HPP_INCLUDED
