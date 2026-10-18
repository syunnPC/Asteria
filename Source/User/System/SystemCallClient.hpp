// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_SYSTEM_SYSTEM_CALL_CLIENT_HPP_INCLUDED
#define ASTERIA_USER_SYSTEM_SYSTEM_CALL_CLIENT_HPP_INCLUDED

#include "Abi/SystemCallAbi.hpp"

namespace Asteria::User::System
{
struct CapabilityHandle
{
    Foundation::UInt64 Selector;
    Foundation::UInt64 Authenticator;
};

using SystemCallTransportFunction = void (*)(void* context, const Abi::SystemCallRequest* request,
                                             Abi::SystemCallResponse* response) noexcept;

struct SystemCallTransport
{
    SystemCallTransportFunction Function;
    void* Context;
};

/// Transport-neutral user-side construction of the stable system-call ABI.
/// The borrowed transport and context must remain valid for every call. Calls
/// allocate no memory and retain no request or response storage.
class SystemCallClient final
{
public:
    explicit constexpr SystemCallClient(const SystemCallTransport transport) noexcept : m_transport{transport}
    {
    }

    [[nodiscard]] constexpr bool IsReady() const noexcept
    {
        return m_transport.Function != nullptr;
    }

    [[nodiscard]] Abi::SystemCallResponse CloseCapability(CapabilityHandle handle) const noexcept;

    [[nodiscard]] Abi::SystemCallResponse QueryCapability(CapabilityHandle handle) const noexcept;

    /// Flags is Derive argument 4, such as Abi::SystemCallDeriveChildRevocationDomain.
    [[nodiscard]] Abi::SystemCallResponse DeriveCapability(CapabilityHandle handle, Foundation::UInt64 rights,
                                                           Foundation::UInt64 leaseDeadlineTicks,
                                                           Foundation::UInt64 flags = 0U) const noexcept;

    [[nodiscard]] Abi::SystemCallResponse DuplicateCapability(CapabilityHandle handle, Foundation::UInt64 rights,
                                                              Foundation::UInt64 leaseDeadlineTicks) const noexcept;

    [[nodiscard]] Abi::SystemCallResponse RevokeCapability(CapabilityHandle handle) const noexcept;

    [[nodiscard]] Abi::SystemCallResponse Send(CapabilityHandle endpoint, Foundation::UInt64 frameAddress,
                                               Foundation::UInt64 frameSizeBytes) const noexcept;

    [[nodiscard]] Abi::SystemCallResponse Receive(CapabilityHandle endpoint, Foundation::UInt64 deliveryAddress,
                                                  Foundation::UInt64 deliveryCapacityBytes) const noexcept;

    /// Waits on waitSet; a nonzero timeoutMicroseconds ends the wait with TimedOut once that long
    /// passed with no member ready.
    [[nodiscard]] Abi::SystemCallResponse Wait(CapabilityHandle waitSet,
                                               Foundation::UInt64 timeoutMicroseconds = 0U) const noexcept;

    /// Writes complete lines (printable ASCII and LF, ending with LF, at most Abi:
    /// DebugWriteMaximumBytes) through a DiagnosticChannel capability.
    [[nodiscard]] Abi::SystemCallResponse DebugWrite(CapabilityHandle channel, Foundation::UInt64 textAddress,
                                                     Foundation::UInt64 textSizeBytes) const noexcept;

    /// Resolves the fault a FaultToken names with the request of Abi::FaultResolveRequestSizeBytes
    /// at requestAddress.
    [[nodiscard]] Abi::SystemCallResponse FaultResolve(CapabilityHandle faultToken, Abi::FaultResolveKind kind,
                                                       Foundation::UInt64 requestAddress) const noexcept;

    /// Resolves the fault a FaultToken names by exiting the faulting thread
    /// (FaultResolve kind Terminate, which carries no request).
    [[nodiscard]] Abi::SystemCallResponse FaultTerminate(CapabilityHandle faultToken) const noexcept;

    /// Creates an Endpoint charged to the ResourceDomain that domain (Manage) names, from the
    /// request of Abi::ObjectCreateEndpointRequestSizeBytes at requestAddress; Value0 and Value1
    /// return its capability.
    [[nodiscard]] Abi::SystemCallResponse CreateEndpoint(CapabilityHandle domain,
                                                         Foundation::UInt64 requestAddress) const noexcept;

    /// Creates an object of type (Abi::ObjectTypeFrame or Abi::ObjectTypeMemoryObject, or an
    /// Endpoint) charged to the ResourceDomain that domain (Manage) names, from the request of
    /// requestSizeBytes at requestAddress; Value0 and Value1 return its capability.
    [[nodiscard]] Abi::SystemCallResponse CreateObject(CapabilityHandle domain, Foundation::UInt64 type,
                                                       Foundation::UInt64 requestAddress,
                                                       Foundation::UInt64 requestSizeBytes) const noexcept;

    /// Destroys the object that object (Manage) names; type is the ObjectType value Query reports,
    /// such as Abi::ObjectTypeEndpoint.
    [[nodiscard]] Abi::SystemCallResponse DestroyObject(CapabilityHandle object,
                                                        Foundation::UInt64 type) const noexcept;

    /// Ends the live thread that thread (Manage) names with Abi::KilledExitCode (ThreadTerminate,
    /// domain teardown); WouldBlock while it is blocked on a page fault.
    [[nodiscard]] Abi::SystemCallResponse TerminateThread(CapabilityHandle thread) const noexcept;

    /// Binds the readiness of endpoint (Read) to waitSet (Manage) under tag, nonzero, which
    /// WaitSetWait then returns (WaitSetBind).
    [[nodiscard]] Abi::SystemCallResponse BindWaitSet(CapabilityHandle waitSet, CapabilityHandle endpoint,
                                                      Foundation::UInt64 tag) const noexcept;

    /// One DeviceControl operation (Abi::Device*) on the PCI function function names; domain is
    /// DeviceAssign's ProtectionDomain, for DeviceQuiesced the drained assignment's sequence and
    /// zero, and zero otherwise.
    [[nodiscard]] Abi::SystemCallResponse ControlDevice(CapabilityHandle function, Foundation::UInt64 operation,
                                                        CapabilityHandle domain = {}) const noexcept;

    /// One IommuControl operation (Abi::Iommu*) on the IommuDomain domain names, with its arguments
    /// 3 and 4: a PciFunction for IommuAttach and IommuDetach, the block's address and size for
    /// IommuMap, the device address for IommuUnmap, nothing for IommuFaults.
    [[nodiscard]] Abi::SystemCallResponse ControlIommu(CapabilityHandle domain, Foundation::UInt64 operation,
                                                       Foundation::UInt64 argument3,
                                                       Foundation::UInt64 argument4) const noexcept;

    [[nodiscard]] Abi::SystemCallResponse PortIo(CapabilityHandle region, Foundation::UInt64 offset,
                                                 Foundation::UInt64 width, Foundation::UInt64 operation,
                                                 Foundation::UInt64 value = 0U) const noexcept;

    /// One InterruptControl operation (Abi::InterruptRearm or Abi::InterruptKeepMasked) on the
    /// Interrupt interrupt (Manage) names; Value0 returns the last event's sequence.
    [[nodiscard]] Abi::SystemCallResponse ControlInterrupt(CapabilityHandle interrupt,
                                                           Foundation::UInt64 operation) const noexcept;

    /// Creates a thread in the caller's domain charged to the ResourceDomain that domain (Manage)
    /// names, from the request of Abi::ObjectCreateThreadRequestSizeBytes at requestAddress; Value0
    /// and Value1 return its capability.
    [[nodiscard]] Abi::SystemCallResponse CreateThread(CapabilityHandle domain,
                                                       Foundation::UInt64 requestAddress) const noexcept;

    /// Ends the calling thread with exitCode (bit 63 clear). It returns only
    /// with the kernel's refusal.
    [[nodiscard]] Abi::SystemCallResponse ExitThread(Foundation::UInt64 exitCode) const noexcept;

    /// Maps memory as the request of Abi::FaultResolveRequestSizeBytes at requestAddress describes
    /// (MemoryMap).
    [[nodiscard]] Abi::SystemCallResponse MapMemory(Foundation::UInt64 requestAddress) const noexcept;

    /// Removes the mapping of exactly pages pages at virtualStart from the
    /// AddressSpace space (Map) names (MemoryUnmap).
    [[nodiscard]] Abi::SystemCallResponse UnmapMemory(CapabilityHandle space, Foundation::UInt64 virtualStart,
                                                      Foundation::UInt64 pages) const noexcept;

    /// Moves or copies source, of objectType, into the CapabilitySpace destination (Manage) names
    /// with rights (CapabilityGrant); disposition is Abi::CapabilityGrantMove
    /// or Abi::CapabilityGrantCopy. Value0 and Value1 return the handle there.
    [[nodiscard]] Abi::SystemCallResponse GrantCapability(CapabilityHandle source, CapabilityHandle destination,
                                                          Foundation::UInt16 rights, Foundation::UInt8 objectType,
                                                          Foundation::UInt64 disposition) const noexcept;

private:
    [[nodiscard]] Abi::SystemCallResponse Invoke(Abi::SystemCallNumber number, CapabilityHandle handle,
                                                 Foundation::UInt64 argument2, Foundation::UInt64 argument3,
                                                 Foundation::UInt64 argument4 = 0U,
                                                 Foundation::UInt64 argument5 = 0U) const noexcept;

    SystemCallTransport m_transport;
};

static_assert(sizeof(CapabilityHandle) == 16U);
static_assert(alignof(CapabilityHandle) == 8U);
static_assert(sizeof(SystemCallTransport) == 16U);
static_assert(alignof(SystemCallTransport) == 8U);
} // Namespace Asteria::User::System

#endif // ASTERIA_USER_SYSTEM_SYSTEM_CALL_CLIENT_HPP_INCLUDED
