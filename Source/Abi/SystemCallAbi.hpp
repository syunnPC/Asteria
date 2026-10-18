// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_ABI_SYSTEM_CALL_ABI_HPP_INCLUDED
#define ASTERIA_ABI_SYSTEM_CALL_ABI_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Abi
{
inline constexpr Foundation::Size SystemCallArgumentCount{6U};
inline constexpr Foundation::UInt64 SystemCallQueryObjectTypeMask{0xFFU};
inline constexpr Foundation::Size SystemCallQueryRightsShift{16U};
/// Capability rights in Query, Derive, Duplicate, grants and IPC attachments.
inline constexpr Foundation::UInt16 CapabilityRightInspect{0x0001U};
inline constexpr Foundation::UInt16 CapabilityRightInvoke{0x0002U};
inline constexpr Foundation::UInt16 CapabilityRightRead{0x0004U};
inline constexpr Foundation::UInt16 CapabilityRightWrite{0x0008U};
inline constexpr Foundation::UInt16 CapabilityRightMap{0x0010U};
inline constexpr Foundation::UInt16 CapabilityRightWait{0x0020U};
inline constexpr Foundation::UInt16 CapabilityRightSignal{0x0040U};
inline constexpr Foundation::UInt16 CapabilityRightTransfer{0x0080U};
inline constexpr Foundation::UInt16 CapabilityRightDuplicate{0x0100U};
inline constexpr Foundation::UInt16 CapabilityRightDerive{0x0200U};
inline constexpr Foundation::UInt16 CapabilityRightRevoke{0x0400U};
inline constexpr Foundation::UInt16 CapabilityRightManage{0x0800U};
inline constexpr Foundation::UInt16 CapabilityRightDebug{0x1000U};
inline constexpr Foundation::UInt16 CapabilityRightCode{0x2000U};
/// Code on the ResourceDomain authorizing ObjectCreate admits Execute;
/// otherwise an executable MemoryObject request returns RightsDenied.
/// Derive argument 4. With this bit the derived handle receives a new child of
/// its source's revocation domain, the only way a derived handle may carry
/// Revoke. Every other bit is reserved and must be zero.
inline constexpr Foundation::UInt64 SystemCallDeriveChildRevocationDomain{1U};
/// Upper bound of one DebugWrite call. Both syscall paths run with interrupts
/// disabled, so the kernel copies and emits at most this much per call.
inline constexpr Foundation::Size DebugWriteMaximumBytes{256U};

enum class SystemCallNumber : Foundation::UInt64
{
    CapabilityClose = 1U,
    CapabilityQuery = 2U,
    CapabilityDerive = 3U,
    CapabilityDuplicate = 4U,
    CapabilityRevoke = 5U,
    IpcSend = 6U,
    IpcReceive = 7U,
    WaitSetWait = 8U,
    DebugWrite = 9U,
    FaultResolve = 10U,
    ObjectCreate = 11U,
    ObjectDestroy = 12U,
    MemoryMap = 13U,
    MemoryUnmap = 14U,
    ThreadExit = 15U,
    CapabilityGrant = 16U,
    ThreadTerminate = 17U,
    WaitSetBind = 18U,
    DeviceControl = 19U,
    IommuControl = 20U,
    InterruptControl = 21U,
    /// Handle (selector, authenticator), offset, width (1/2/4), operation
    /// (0 read, 1 write), value (zero on read). Read returns Value0.
    PortIo = 22U,
};

/// DeviceControl: arguments 0 and 1 name a PCI function, argument 2 the operation, arguments 3 and
/// 4 a ProtectionDomain handle for DeviceAssign and zero otherwise, argument 5 zero. DeviceAssign
/// (Manage on the function and on the domain) assigns the function to that domain with bus
/// mastering off; DeviceRelease (Manage) turns bus mastering off and releases the assignment;
/// DeviceEnableBusMaster (Invoke) lets the assignee make it a bus master once an IommuDomain is
/// attached; DeviceDisableBusMaster (Invoke) turns it off for the assignee, and with Manage revokes
/// it until the next assignment.
inline constexpr Foundation::UInt64 DeviceAssign{1U};
inline constexpr Foundation::UInt64 DeviceRelease{2U};
inline constexpr Foundation::UInt64 DeviceEnableBusMaster{3U};
inline constexpr Foundation::UInt64 DeviceDisableBusMaster{4U};
/// The Device Manager's confirmation (Manage) that a function a drain took from a dead driver is
/// quiet: argument 3 the drained assignment's sequence (DeviceAssign's value 0), argument 4 zero.
inline constexpr Foundation::UInt64 DeviceQuiesced{5U};
/// The assignee's (Invoke) narrow configuration access to a USB host controller : argument 3 a
/// naturally aligned dword offset, argument 4 zero for DeviceReadConfiguration or the value for
/// DeviceWriteLegacySupport. Reads cover the identity, class and BAR 0 dwords (0x00, 0x08, 0x10),
/// the interrupt dword (0x3C), an EHCI's dwords from 0x40 on (its EECP capability list) and a
/// UHCI's LEGSUP dword (0xC0); value 0 is the dword. Writes only relinquish firmware legacy
/// support: an EHCI's USBLEGSUP may only set its OS Owned semaphore and its USBLEGCTLSTS may only
/// clear SMI enables and acknowledge SMI status; a UHCI's LEGSUP may only acknowledge status,
/// enable PIRQ and clear its trap and SMI enables. Anything else is InvalidArguments. The Device
/// Manager (Manage) reads the same dwords of a function no driver holds (one a drain released into
/// quarantine), for its proof that the device is quiet; while a driver is assigned, its read is
/// refused.
inline constexpr Foundation::UInt64 DeviceReadConfiguration{6U};
inline constexpr Foundation::UInt64 DeviceWriteLegacySupport{7U};
/// PowerOff.md: Manage on the exact boot ResourceDomain; arguments 3–5 zero.
/// Query returns availability bits (S5=1, reset=2, mapped EFI runtime=4).
inline constexpr Foundation::UInt64 DevicePowerQuery{9U};
inline constexpr Foundation::UInt64 DevicePowerOff{10U};
inline constexpr Foundation::UInt64 DeviceReboot{11U};
inline constexpr Foundation::UInt64 DevicePowerOffAvailable{1U << 0U};
inline constexpr Foundation::UInt64 DeviceRebootAvailable{1U << 1U};
inline constexpr Foundation::UInt64 DevicePowerRuntimeMapped{1U << 2U};

/// IommuControl: arguments 0 and 1 name an IommuDomain, argument 2 the operation. - IommuAttach and
/// IommuDetach (Manage on the domain): arguments 3 and 4 a PciFunction (Invoke) whose assignee the
/// caller is; bus mastering is off across either; argument 5 zero. - IommuMap (Map on the domain):
/// argument 3 the address of an IommuMapRequestSizeBytes block, argument 4 its size, argument 5
/// zero. Value 0 is the device address the mapping starts at, which the driver programs and
/// IommuUnmap names: the requested one behind a translating IOMMU, the Frame's physical address on
/// the trusted-driver profile's identity backend. - IommuUnmap (Map on the domain): argument 3 the
/// device address a mapping starts at; arguments 4 and 5 zero. - IommuFaults (Inspect on the
/// domain): arguments 3 to 5 zero; value 1 is the last fault input address; value 0 holds the
/// domain's fault count in bits 0-31, the last fault's requester in bits 32-47 and its reason in
/// bits 48-55. IommuMap answers ReservedMemory for a device range that overlaps a firmware DMA
/// reservation (a VT-d RMRR, an AMD-Vi IVMD) of a requester the domain serves, and IommuAttach for
/// a requester such a present mapping overlaps, released from firmware or not.
inline constexpr Foundation::UInt64 IommuAttach{1U};
inline constexpr Foundation::UInt64 IommuDetach{2U};
inline constexpr Foundation::UInt64 IommuMap{3U};
inline constexpr Foundation::UInt64 IommuUnmap{4U};
inline constexpr Foundation::UInt64 IommuFaults{5U};
/// The IommuMap block: a Frame capability (Map), the Frame page the mapping
/// starts at, the page count, the page-aligned device address, and a
/// permission byte (IommuMapDeviceRead, IommuMapDeviceWrite, at least one);
/// the other bytes are zero.
inline constexpr Foundation::Size IommuMapRequestSizeBytes{48U};
inline constexpr Foundation::Size IommuMapFrameOffset{0U};
inline constexpr Foundation::Size IommuMapFramePageOffset{16U};
inline constexpr Foundation::Size IommuMapPagesOffset{24U};
inline constexpr Foundation::Size IommuMapDeviceAddressOffset{32U};
inline constexpr Foundation::Size IommuMapPermissionsOffset{40U};
inline constexpr Foundation::UInt8 IommuMapDeviceRead{1U};
inline constexpr Foundation::UInt8 IommuMapDeviceWrite{2U};

/// InterruptControl: arguments 0 and 1 name an Interrupt (Manage), argument 2 the operation,
/// arguments 3 to 5 zero. InterruptRearm acknowledges a pending event and arms the Interrupt (and
/// arms a masked one with none); InterruptKeepMasked acknowledges a pending event and masks. Value
/// 0 is the last event's sequence. A message that arrived while masked is delivered at the next
/// arm.
inline constexpr Foundation::UInt64 InterruptRearm{1U};
inline constexpr Foundation::UInt64 InterruptKeepMasked{2U};

/// CapabilityGrant: arguments 0 and 1 the source handle, arguments 2 and 3 a CapabilitySpace handle
/// with Manage, the destination; argument 4 the requested rights in bits 0-15 and the source's
/// ObjectType in bits 16-23, every other bit zero; argument 5 the disposition, CapabilityGrantMove
/// or CapabilityGrantCopy. Value0 and Value1 return the destination handle.
inline constexpr Foundation::UInt64 CapabilityGrantMove{1U};
inline constexpr Foundation::UInt64 CapabilityGrantCopy{2U};
inline constexpr Foundation::UInt64 CapabilityGrantTypeShift{16U};

/// FaultResolve argument 2: how the pager resolves the fault its FaultToken names. Map installs a
/// mapping and wakes the thread; Terminate exits the thread with FaultTerminatedExitCode. Other
/// values are reserved.
enum class FaultResolveKind : Foundation::UInt64
{
    Map = 1U,
    Terminate = 2U,
};

/// The exit code of a thread whose fault was resolved with Terminate, which
/// FaultResolve also returns in Value0. Bit 63 marks an exit code the kernel
/// chose rather than the thread.
inline constexpr Foundation::UInt64 FaultTerminatedExitCode{0x8000'0000'0000'0002ULL};

/// ThreadTerminate: arguments 0 and 1 name a Thread with Manage, arguments 2 to 5 are zero. A Ready
/// thread ends at once with KilledExitCode, which its ObjectDestroy returns; an exited one is left
/// as it is; one blocked on a page fault is WouldBlock (its supervisor terminates it through
/// FaultResolve); the caller itself is InvalidArguments. Bit 63: the kernel chose the code.
inline constexpr Foundation::UInt64 KilledExitCode{0x8000'0000'0000'0003ULL};

/// WaitSetBind: arguments 0 and 1 name a WaitSet with Manage, arguments 2 and 3 an Endpoint with
/// Read, argument 4 is the nonzero tag WaitSetWait returns when the Endpoint has messages, argument
/// 5 is zero. It registers the Endpoint's readiness in the WaitSet; there is no unbind, since
/// destroying the WaitSet drops its registrations. An Endpoint that is already bound, or one the
/// kernel built for itself, is InvalidArguments; a full WaitSet is ResourceExhausted.

/// FaultResolve argument 3 names, and argument 4 sizes, one request of exactly
/// FaultResolveRequestSizeBytes in the pager's memory; MemoryMap takes the same block in arguments
/// 3 and 4, its other arguments zero. Three handles of selector and authenticator (the faulting
/// AddressSpace, or the one MemoryMap maps into, then the MemoryObject and Frame to map, each with
/// Map), then the virtual start, page count, MemoryObject page offset and Frame page offset, then
/// one permission byte (Read 1, Write 2, Execute 4, User 8; MemoryMap requires User) and seven zero
/// bytes. Every word is little-endian.
inline constexpr Foundation::Size FaultResolveRequestSizeBytes{88U};
inline constexpr Foundation::Size FaultResolveAddressSpaceOffset{0U};
inline constexpr Foundation::Size FaultResolveMemoryObjectOffset{16U};
inline constexpr Foundation::Size FaultResolveFrameOffset{32U};
inline constexpr Foundation::Size FaultResolveVirtualStartOffset{48U};
inline constexpr Foundation::Size FaultResolvePageCountOffset{56U};
inline constexpr Foundation::Size FaultResolveMemoryObjectPageOffset{64U};
inline constexpr Foundation::Size FaultResolveFramePageOffset{72U};
inline constexpr Foundation::Size FaultResolvePermissionsOffset{80U};

/// MemoryUnmap: arguments 0 and 1 name the AddressSpace with Map, argument 2 is the virtual start
/// and argument 3 the page count of exactly one mapping MemoryMap made; arguments 4 and 5 are zero.

/// ObjectCreate and ObjectDestroy argument 2: the ObjectType value Query reports of the objects the
/// kernel creates and destroys.
inline constexpr Foundation::UInt64 ObjectTypeProtectionDomain{1U};
inline constexpr Foundation::UInt64 ObjectTypeAddressSpace{2U};
inline constexpr Foundation::UInt64 ObjectTypeCapabilitySpace{3U};
inline constexpr Foundation::UInt64 ObjectTypeThread{4U};
inline constexpr Foundation::UInt64 ObjectTypeResourceDomain{6U};
inline constexpr Foundation::UInt64 ObjectTypeMemoryObject{7U};
inline constexpr Foundation::UInt64 ObjectTypeFrame{8U};
inline constexpr Foundation::UInt64 ObjectTypeEndpoint{9U};
inline constexpr Foundation::UInt64 ObjectTypeWaitSet{12U};
inline constexpr Foundation::UInt64 ObjectTypeInterrupt{14U};
inline constexpr Foundation::UInt64 ObjectTypeFaultChannel{17U};
/// Interrupt request Source=1 selects an InterruptLine (type 23, Invoke)
/// instead of a PCI function. Source=0 retains the PCI ABI.
inline constexpr Foundation::UInt64 InterruptSourceLine{1U};
inline constexpr Foundation::UInt64 ObjectTypeInterruptLine{23U};
inline constexpr Foundation::UInt64 ObjectTypeIommuDomain{16U};

/// An Interrupt's ObjectCreate request: the PciFunction (Invoke) whose interrupt it serves, the
/// caller its assignee, at 0, and the source at 16: 0, the function's interrupt as the platform
/// routes it (its MSI on AMD64, its INTx line on IA-64).
inline constexpr Foundation::Size ObjectCreateInterruptRequestSizeBytes{24U};
inline constexpr Foundation::Size ObjectCreateInterruptFunctionOffset{0U};
inline constexpr Foundation::Size ObjectCreateInterruptSourceOffset{16U};

/// An IommuDomain's ObjectCreate request: the device address width in bits (32 to 39 on VT-d) in
/// byte 0, the other bytes zero.
inline constexpr Foundation::Size ObjectCreateIommuDomainRequestSizeBytes{8U};
inline constexpr Foundation::Size ObjectCreateIommuAddressBitsOffset{0U};

/// A Frame's ObjectCreate request: its page count (1 to 16), one byte of physical alignment in
/// pages (0 or 1 for a page; 2, 4, 8 or 16 for a Frame of RAM, whose physical address a device sees
/// on the trusted-driver profile's identity IommuDomains:
/// Documentation/Architecture/IommuDomain.md) and seven zero bytes. A MemoryObject's: its page
/// count (1 to 1024), one byte of maximum permissions (Read 1, Write 2, Execute 4, User 8, as
/// FaultResolve's), one byte of cache policy (0 write-back, 1 uncached, 2 write-combining;
/// Documentation/Architecture/IommuDomain.md ) and six zero bytes. Every word is little-endian.
/// Execute requires Code on the ResourceDomain authority; missing it returns RightsDenied.
inline constexpr Foundation::Size ObjectCreateFrameRequestSizeBytes{16U};
// Preserve the legacy bounds; the size selects the large RAM form (log2 alignment, address limit,
// fourteen zero bytes).
inline constexpr Foundation::Size ObjectCreateRamFrameRequestSizeBytes{24U};
inline constexpr Foundation::UInt64 ObjectCreateFrameLegacyMaximumPages{128U};
inline constexpr Foundation::Size ObjectCreateRamFrameLimitOffset{9U};
inline constexpr Foundation::UInt8 ObjectCreateRamFrameMaximumAlignmentLog2{9U};
inline constexpr Foundation::Size ObjectCreateMemoryObjectRequestSizeBytes{16U};
inline constexpr Foundation::Size ObjectCreatePagesOffset{0U};
inline constexpr Foundation::Size ObjectCreatePermissionsOffset{8U};
inline constexpr Foundation::Size ObjectCreateFrameAlignmentOffset{8U};
inline constexpr Foundation::UInt8 ObjectCreateFrameMaximumAlignmentPages{16U};
inline constexpr Foundation::Size ObjectCreateCachePolicyOffset{9U};
inline constexpr Foundation::UInt8 ObjectCreateCacheWriteBack{0U};
inline constexpr Foundation::UInt8 ObjectCreateCacheUncached{1U};
inline constexpr Foundation::UInt8 ObjectCreateCacheWriteCombining{2U};
/// A device Frame's ObjectCreate request: the Frame request's page count and eight zero bytes (no
/// alignment), then a DeviceRegion handle (selector, authenticator; Map) and the page offset of the
/// Frame's first page in the region. The Frame names the region's pages with its cache policy and
/// charges kernel metadata only.
inline constexpr Foundation::Size ObjectCreateDeviceFrameRequestSizeBytes{40U};
inline constexpr Foundation::Size ObjectCreateDeviceFrameRegionOffset{16U};
inline constexpr Foundation::Size ObjectCreateDeviceFrameRegionPageOffset{32U};

/// An AddressSpace's and a CapabilitySpace's ObjectCreate request: 32 bytes each, with the
/// AddressSpace flag below; all other bytes are zero. ObjectDestroy of either answers WouldBlock
/// until teardown can reclaim one.
inline constexpr Foundation::Size ObjectCreateAddressSpaceRequestSizeBytes{32U};
// Bit 0 of byte 0; every other bit/byte is reserved.
inline constexpr Foundation::UInt8 ObjectCreateAddressSpaceFlagLargeArena{1U};
inline constexpr Foundation::Size ObjectCreateCapabilitySpaceRequestSizeBytes{32U};

/// A ResourceDomain's ObjectCreate request: the child's limits as seven little-endian words,
/// committed and resident memory pages, kernel-metadata bytes and capability handles (both zero:
/// the child takes its parent's metadata limit, and nothing charges handles), IPC queue bytes,
/// threads and child domains. Each limit is a cap within the parent's, not a guarantee: every
/// ancestor's remaining quota bounds each charge. Its first capability carries Manage, Inspect,
/// Duplicate, Transfer and Derive; the parent is charged the child's slot.
inline constexpr Foundation::Size ObjectCreateResourceDomainRequestSizeBytes{56U};
// Extended forms carry flags and per-type limits, then an optional large-arena limit.
inline constexpr Foundation::Size ObjectCreateResourceDomainExtendedRequestSizeBytes{80U};
inline constexpr Foundation::Size ObjectCreateResourceDomainArenaRequestSizeBytes{88U};
inline constexpr Foundation::Size ObjectCreateDomainFlagsOffset{56U};
inline constexpr Foundation::Size ObjectCreateDomainObjectLimitsOffset{64U};
inline constexpr Foundation::Size ObjectCreateDomainLargeArenaLimitOffset{80U};
inline constexpr Foundation::UInt64 ObjectCreateDomainFlagCode{1U};
inline constexpr Foundation::Size ObjectCreateDomainCommittedPagesOffset{0U};
inline constexpr Foundation::Size ObjectCreateDomainResidentPagesOffset{8U};
inline constexpr Foundation::Size ObjectCreateDomainMetadataOffset{16U};
inline constexpr Foundation::Size ObjectCreateDomainHandlesOffset{24U};
inline constexpr Foundation::Size ObjectCreateDomainQueueBytesOffset{32U};
inline constexpr Foundation::Size ObjectCreateDomainThreadsOffset{40U};
inline constexpr Foundation::Size ObjectCreateDomainChildrenOffset{48U};

/// A WaitSet's ObjectCreate request: eight zero bytes. Its first capability carries Inspect, Wait,
/// Transfer, Duplicate, Derive and Manage; ObjectDestroy needs Manage and refuses a WaitSet the
/// kernel did not create through this call.
inline constexpr Foundation::Size ObjectCreateWaitSetRequestSizeBytes{8U};

/// A ProtectionDomain's ObjectCreate request: an AddressSpace handle and a CapabilitySpace handle
/// of selector and authenticator, each with Manage, then the domain's nonzero supervision tag, then
/// a billing ResourceDomain handle that must be zero (the authorizing domain) and a diagnostic
/// component-kind word (zero or a ComponentBootstrap behavior). This Watchdog.md conveys no
/// authority. Every word is little-endian. ObjectDestroy of a ProtectionDomain answers WouldBlock
/// while a thread is bound in it.
inline constexpr Foundation::Size ObjectCreateProtectionDomainRequestSizeBytes{64U};
inline constexpr Foundation::Size ObjectCreateProtectionDomainChannelRequestSizeBytes{80U};
inline constexpr Foundation::Size ObjectCreateDomainFaultChannelOffset{56U};
inline constexpr Foundation::Size ObjectCreateFaultChannelRequestSizeBytes{8U};
inline constexpr Foundation::Size ObjectCreateDomainAddressSpaceOffset{0U};
inline constexpr Foundation::Size ObjectCreateDomainCapabilitySpaceOffset{16U};
inline constexpr Foundation::Size ObjectCreateDomainSupervisionTagOffset{32U};
inline constexpr Foundation::Size ObjectCreateDomainBillingOffset{40U};
/// Watchdog diagnostic component kind; zero retains the generic name. The 80-byte ProtectionDomain
/// form retains its FaultChannel at 56 and places the diagnostic word at 72 (Watchdog.md ).
inline constexpr Foundation::Size ObjectCreateDomainComponentKindOffset{56U};
inline constexpr Foundation::Size ObjectCreateDomainChannelComponentKindOffset{72U};
static_assert(ObjectCreateDomainComponentKindOffset + sizeof(Foundation::UInt64) ==
              ObjectCreateProtectionDomainRequestSizeBytes);
static_assert(ObjectCreateDomainFaultChannelOffset + 16U == ObjectCreateDomainChannelComponentKindOffset);
static_assert(ObjectCreateDomainChannelComponentKindOffset + sizeof(Foundation::UInt64) ==
              ObjectCreateProtectionDomainChannelRequestSizeBytes);

/// A Thread's ObjectCreate request: a ProtectionDomain handle of selector and authenticator, zero
/// for the caller's own domain or one the caller manages, then the entry, the user stack top and
/// the argument the thread starts with, then a budget and a period, which must be zero (the
/// platform's user scheduling), and a thread-pointer word reserved for static TLS, required to be zero.
/// Every word is little-endian. ObjectDestroy of a Thread accepts only an exited one and returns
/// its exit code in Value0; ThreadExit (argument 0 the exit code, bit 63 clear; the rest zero) ends
/// the calling thread and does not return.
inline constexpr Foundation::Size ObjectCreateThreadRequestSizeBytes{64U};
inline constexpr Foundation::Size ObjectCreateThreadDomainOffset{0U};
inline constexpr Foundation::Size ObjectCreateThreadEntryOffset{16U};
inline constexpr Foundation::Size ObjectCreateThreadStackOffset{24U};
inline constexpr Foundation::Size ObjectCreateThreadArgumentOffset{32U};
inline constexpr Foundation::Size ObjectCreateThreadBudgetOffset{40U};
inline constexpr Foundation::Size ObjectCreateThreadPeriodOffset{48U};
/// Reserved for a static TLS thread pointer. Must be zero until TLS is implemented; nonzero is
/// InvalidArguments.
inline constexpr Foundation::Size ObjectCreateThreadPointerOffset{56U};

/// ObjectCreate argument 3 names, and argument 4 sizes, an Endpoint's request of exactly
/// ObjectCreateEndpointRequestSizeBytes in the caller's memory: a receiver capability-space handle
/// of selector and authenticator, zero for the caller's own space or a CapabilitySpace with Manage,
/// then the Endpoint configuration: queued bytes, reserved control bytes and bytes per sender (16
/// bits each), then queued messages, reserved control messages, messages per sender, queued
/// attachments, reserved control attachments and attachments per sender (one byte each), four zero
/// bytes, and the sealed selector the first capability carries, which every copy inherits and every
/// delivery reports, zero for none. Every field is little-endian.
inline constexpr Foundation::Size ObjectCreateEndpointRequestSizeBytes{40U};
inline constexpr Foundation::Size ObjectCreateEndpointReceiverSpaceOffset{0U};
inline constexpr Foundation::Size ObjectCreateEndpointQueuedBytesOffset{16U};
inline constexpr Foundation::Size ObjectCreateEndpointReservedControlBytesOffset{18U};
inline constexpr Foundation::Size ObjectCreateEndpointBytesPerSenderOffset{20U};
inline constexpr Foundation::Size ObjectCreateEndpointQueuedMessagesOffset{22U};
inline constexpr Foundation::Size ObjectCreateEndpointReservedControlMessagesOffset{23U};
inline constexpr Foundation::Size ObjectCreateEndpointMessagesPerSenderOffset{24U};
inline constexpr Foundation::Size ObjectCreateEndpointQueuedAttachmentsOffset{25U};
inline constexpr Foundation::Size ObjectCreateEndpointReservedControlAttachmentsOffset{26U};
inline constexpr Foundation::Size ObjectCreateEndpointAttachmentsPerSenderOffset{27U};
inline constexpr Foundation::Size ObjectCreateEndpointReservedOffset{28U};
inline constexpr Foundation::Size ObjectCreateEndpointSealedSelectorOffset{32U};

enum class SystemCallError : Foundation::UInt64
{
    None = 0U,
    UnknownSystemCall = 1U,
    InvalidArguments = 2U,
    InvalidConfiguration = 3U,
    InvalidHandle = 4U,
    ObjectNotFound = 5U,
    ResourceExhausted = 6U,
    SpaceExhausted = 7U,
    RightsDenied = 8U,
    RightsAmplification = 9U,
    NoAttenuation = 10U,
    TypeMismatch = 11U,
    Revoked = 12U,
    Expired = 13U,
    LeaseExtension = 14U,
    RevocationDomainExhausted = 15U,
    WouldBlock = 16U,
    UserMemoryFault = 17U,
    /// A WaitSetWait whose timeout passed before a member was ready.
    TimedOut = 18U,
    WriteValueDenied = 19U,
    /// Firmware-owned DMA has not completed its BIOS-to-OS handoff.
    ReservedMemory = 20U,
    /// The firmware display remains owned by the kernel.
    DisplayReserved = 21U,
    /// PCI bridge forwarding is managed only by the kernel.
    BridgeReserved = 22U,
    /// Append new errors before Last and update its pinned ABI assertion.
    Last = BridgeReserved,
};
static_assert(static_cast<Foundation::UInt64>(SystemCallError::None) == 0U);
static_assert(static_cast<Foundation::UInt64>(SystemCallError::Last) == 22U);
static_assert(SystemCallError::Last == SystemCallError::BridgeReserved);
static_assert(static_cast<Foundation::UInt64>(SystemCallError::Last) <= 0x00FF'FFFFU);

/// Architecture-neutral logical register image for one system call.
/// Target transports copy the number and six unsigned arguments to or from
/// their native registers. This is not an in-memory kernel/user exchange.
struct SystemCallRequest
{
    Foundation::UInt64 Number;
    Foundation::UInt64 Arguments[SystemCallArgumentCount];
};

/// Architecture-neutral logical register image returned by one system call.
struct SystemCallResponse
{
    SystemCallError Error;
    Foundation::UInt64 Value0;
    Foundation::UInt64 Value1;
};

static_assert(sizeof(SystemCallNumber) == 8U);
static_assert(sizeof(SystemCallError) == 8U);
static_assert(sizeof(SystemCallRequest) == 56U);
static_assert(alignof(SystemCallRequest) == 8U);
static_assert(__builtin_offsetof(SystemCallRequest, Number) == 0U);
static_assert(__builtin_offsetof(SystemCallRequest, Arguments) == 8U);
static_assert(sizeof(SystemCallResponse) == 24U);
static_assert(alignof(SystemCallResponse) == 8U);
static_assert(__builtin_offsetof(SystemCallResponse, Error) == 0U);
static_assert(__builtin_offsetof(SystemCallResponse, Value0) == 8U);
static_assert(__builtin_offsetof(SystemCallResponse, Value1) == 16U);
} // Namespace Asteria::Abi

#endif // ASTERIA_ABI_SYSTEM_CALL_ABI_HPP_INCLUDED
