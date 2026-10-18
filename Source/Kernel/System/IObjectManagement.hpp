// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_SYSTEM_I_OBJECT_MANAGEMENT_HPP_INCLUDED
#define ASTERIA_KERNEL_SYSTEM_I_OBJECT_MANAGEMENT_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Abi/SystemCallAbi.hpp"
#include "Kernel/Ipc/Endpoint.hpp"
#include "Kernel/Memory/MemoryMapper.hpp"
#include "Kernel/Memory/MemoryTypes.hpp"
#include "Kernel/Object/CapabilityTypes.hpp"
#include "Kernel/Protection/ProtectionDomain.hpp"
#include "Kernel/Scheduler/IUserThreadBackend.hpp"

namespace Asteria::Kernel::Object
{
class CapabilitySpace;
}

namespace Asteria::Kernel::System
{
enum class ObjectManagementError : Foundation::UInt8
{
    None,
    /// The object type is not one the kernel creates or destroys.
    UnsupportedType,
    /// The type-specific request was rejected, such as an Endpoint
    /// configuration the Endpoint refuses.
    InvalidRequest,
    /// A capability failed its authorization or the new capability could not
    /// be minted; Capability says why.
    CapabilityRejected,
    /// The capability names an object no pool created.
    NotOwned,
    /// A pool, the charged ResourceDomain or the object registry is full.
    ResourceExhausted,
    /// The kernel could neither complete nor undo the operation.
    Faulted,
    /// Another transaction of the object is in flight; the call may be
    /// repeated (MemoryMap).
    Busy,
    /// This kernel offers no such operation (MemoryMap without a mapper).
    NotConfigured,
    ReservedMemory,
    DisplayReserved,
    BridgeReserved,
};

struct ObjectManagementResult
{
    ObjectManagementError Error;
    Object::CapabilityError Capability;
    Foundation::UInt8 Reserved[6];
    /// The created object's first capability (ObjectCreate only).
    Object::CapabilityHandle Created;
    /// A destroyed Thread's exit code (ObjectDestroy of a Thread only).
    Foundation::UInt64 Value;
    /// Last fault input address for IommuFaults (zero for other operations).
    Foundation::UInt64 Value1{0U};
};

/// One ObjectCreate, decoded from its registers and request block. Only the
/// fields of Type are meaningful: an Endpoint's configuration, a Frame's
/// page count, a MemoryObject's page count and maximum permissions, a
/// Thread's start, a ProtectionDomain's spaces and supervision tag.
struct ObjectCreateRequest
{
    /// A ResourceDomain capability with Manage in the caller's space: the domain charged. Execute
    /// in a MemoryObject additionally requires Code on this same capability.
    Object::CapabilityHandle Authority;
    Object::ObjectType Type;
    // Watchdog.md: validated component kinds fit in the existing padding;
    // keep the decoded request within the 512-byte target stack budget.
    Foundation::UInt8 DiagnosticComponentKind{0U};
    Foundation::UInt8 Reserved[6];
    Ipc::EndpointConfiguration Endpoint;
    Memory::PageCount Pages;
    Memory::PagePermissions MaximumPermissions;
    Foundation::UInt8 ReservedMemory[7];
    Scheduler::UserThreadStart Thread;
    Object::CapabilityHandle DomainAddressSpace;
    Object::CapabilityHandle DomainCapabilities;
    Foundation::UInt64 SupervisionTag;
    /// An Endpoint's receiver space: zero for the caller's, or a CapabilitySpace with Manage.
    Object::CapabilityHandle EndpointReceiver;
    /// A Thread's domain: zero for the caller's, or a ProtectionDomain with Manage.
    Object::CapabilityHandle ThreadDomain;
    /// An Endpoint's sealed selector, zero for none.
    Foundation::UInt64 EndpointSealedSelector;
    /// A ResourceDomain's limits.
    Resource::ResourceDomainLimits DomainLimits;
    /// A device Frame's DeviceRegion (Map) and the page offset of its first page there, zero for a
    /// RAM Frame; a MemoryObject's cache policy.
    Object::CapabilityHandle FrameRegion;
    Foundation::UInt64 FrameRegionOffsetPages;
    Memory::MemoryCachePolicy MemoryCachePolicy;
    /// An IommuDomain's device address width.
    Foundation::UInt8 IommuInputAddressBits;
    /// A Frame of RAM's physical alignment in pages, a power of two up to 512; the legacy form
    /// still caps alignment at 16.
    Foundation::UInt16 FrameAlignmentPages{0U};
    Foundation::UInt8 FrameAddressLimitBits{32U};
    Foundation::UInt8 ReservedCache[3];
    /// An Interrupt's PciFunction (Invoke) and message source.
    Object::CapabilityHandle InterruptFunction;
    Foundation::UInt64 InterruptSource;
    /// The caller's ProtectionDomain, which the dispatcher fills in.
    Protection::ProtectionDomainId Caller;
    Foundation::UInt64 DomainFlags{0U};
    bool ExplicitObjectLimits{false};
    bool LargePageTableArena{false};
    Object::CapabilityHandle DomainChannel{};
};

/// One IommuControl operation (Abi::Iommu*), decoded from its registers and, for IommuMap, its
/// block.
struct IommuControlRequest
{
    Object::CapabilityHandle Domain;
    Foundation::UInt64 Operation;
    /// IommuAttach and IommuDetach: the PciFunction.
    Object::CapabilityHandle Function;
    /// IommuMap: the Frame, its first page and the page count.
    Object::CapabilityHandle Frame;
    Foundation::UInt64 FramePageOffset;
    Foundation::UInt64 Pages;
    /// IommuMap and IommuUnmap: the device address.
    Foundation::UInt64 DeviceAddress;
    /// IommuMap: Abi::IommuMapDeviceRead and IommuMapDeviceWrite bits.
    Foundation::UInt8 Permissions;
    Foundation::UInt8 Reserved[7];
};

/// The kernel side of the management system calls. The implementation proves the caller's authority
/// from its own capability space; nothing in a request is trusted.
class IObjectManagement
{
public:
    /// Creates one object charged to the domain request.Authority names and
    /// mints its first capability into caller.
    [[nodiscard]] virtual ObjectManagementResult Create(Object::CapabilitySpace& caller,
                                                        const ObjectCreateRequest& request) noexcept = 0;

    /// Makes the object that object, a capability with Manage in caller,
    /// names quiescing; its storage and charge return once unreferenced.
    [[nodiscard]] virtual ObjectManagementResult
    Destroy(Object::CapabilitySpace& caller, Object::CapabilityHandle object, Object::ObjectType type) noexcept = 0;

    /// Maps the MemoryObject and Frame request names into the AddressSpace it names, each a
    /// capability with Map in caller.
    [[nodiscard]] virtual ObjectManagementResult Map(Object::CapabilitySpace& caller,
                                                     const Memory::MemoryMapRequest& request) noexcept = 0;

    /// Removes the mapping of exactly [virtualStart, virtualStart + pages) from the AddressSpace
    /// space (Map in caller) names.
    [[nodiscard]] virtual ObjectManagementResult Unmap(Object::CapabilitySpace& caller, Object::CapabilityHandle space,
                                                       Memory::VirtualAddress virtualStart,
                                                       Memory::PageCount pages) noexcept = 0;

    /// Ends a live Thread that thread, a capability with Manage in caller, names, with Abi:
    /// KilledExitCode. Busy while it is blocked on a page fault; InvalidRequest for the caller
    /// itself; NotConfigured where the platform cannot remove a thread.
    [[nodiscard]] virtual ObjectManagementResult Terminate(Object::CapabilitySpace& caller,
                                                           Object::CapabilityHandle thread) noexcept = 0;

    /// Registers the readiness of the Endpoint endpoint (Read in caller) names in the WaitSet
    /// waitSet (Manage in caller) names, under tag, nonzero. InvalidRequest for an Endpoint without
    /// readiness or already bound; ResourceExhausted for a full WaitSet; Busy while the WaitSet's
    /// claim is in flight.
    [[nodiscard]] virtual ObjectManagementResult BindWaitSet(Object::CapabilitySpace& caller,
                                                             Object::CapabilityHandle waitSet,
                                                             Object::CapabilityHandle endpoint,
                                                             Foundation::UInt64 tag) noexcept = 0;

    /// One DeviceControl operation (Abi::Device*) on the PCI function function names in caller,
    /// with domain for DeviceAssign, by the caller whose ProtectionDomain is callerDomain. NotOwned
    /// for a caller that is not the assignee or a revoked bus master; NotConfigured for bus
    /// mastering without an IommuDomain or a kernel without configuration access.
    [[nodiscard]] virtual ObjectManagementResult ControlDevice(Object::CapabilitySpace& caller,
                                                               Protection::ProtectionDomainId callerDomain,
                                                               Object::CapabilityHandle function,
                                                               Foundation::UInt64 operation,
                                                               Object::CapabilityHandle domain) noexcept = 0;

    /// One IommuControl operation; callerDomain is the caller's ProtectionDomain, which must be an
    /// attached function's assignee.
    [[nodiscard]] virtual ObjectManagementResult ControlIommu(Object::CapabilitySpace& caller,
                                                              Protection::ProtectionDomainId callerDomain,
                                                              const IommuControlRequest& request) noexcept = 0;

    /// InterruptControl (Abi::InterruptRearm or InterruptKeepMasked) on the Interrupt interrupt
    /// (Manage) names; Value is the last event's sequence.
    [[nodiscard]] virtual ObjectManagementResult ControlInterrupt(Object::CapabilitySpace& caller,
                                                                  Object::CapabilityHandle interrupt,
                                                                  Foundation::UInt64 operation) noexcept = 0;

    [[nodiscard]] virtual Abi::SystemCallResponse AccessPorts(Object::CapabilitySpace&,
                                                              const Abi::SystemCallRequest&) noexcept = 0;

protected:
    constexpr IObjectManagement() noexcept = default;
    ~IObjectManagement() noexcept = default;
};

static_assert(sizeof(ObjectManagementError) == 1U);
static_assert(sizeof(ObjectManagementResult) == 40U);
static_assert(__builtin_offsetof(ObjectManagementResult, Value1) == 32U);
static_assert(sizeof(ObjectCreateRequest) == 352U);
} // Namespace Asteria::Kernel::System

#endif // ASTERIA_KERNEL_SYSTEM_I_OBJECT_MANAGEMENT_HPP_INCLUDED
