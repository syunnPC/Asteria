<!-- SPDX-License-Identifier: Apache-2.0 -->

# System Call ABI

## Scope

The system-call surface combines capability, nonblocking Endpoint, and resumable
WaitSet dispatch with trusted ingress and ISA register codecs. It fixes operation
numbers, result values, validation order, and register assignments. Native
[IA-64 `break` entry](Ia64UserSystemCallRuntime.md) and
[AMD64 SYSCALL entry](Amd64BootRuntime.md#native-entry-contract) capture owned
supervisor frames and return to privilege level 3 through this ABI.

`Source/Abi/SystemCallAbi.hpp` is the kernel-independent authority for the
stable numbers, errors, query-result packing constants, and logical request and
response images. The kernel dispatcher re-exports those definitions as aliases
to preserve existing kernel source compatibility; it does not own a second
copy. `User::System::SystemCallClient` consumes the same definitions and only
constructs canonical register images for a borrowed transport.

No C++ object representation crosses the boundary. Target code normalizes its
registers into this unsigned logical image:

```text
request = number, argument[0],..., argument[5]
response = error, value[0], value[1]
```

Every field is one 64-bit word. The internal checked layouts are therefore 56
bytes for `SystemCallRequest` and 24 bytes for `SystemCallResponse`, both with
eight-byte alignment. These structures are an adapter contract, not memory a
user process may directly share with the kernel.

The user client provides typed operation wrappers. Its
transport is one function pointer plus an opaque non-owning context; hosted
tests can provide a fake without importing kernel implementation types. A null
transport returns `{InvalidConfiguration, 0, 0}` without executing a call. The
client validates no kernel policy and performs no allocation, I/O, locking, or
hidden initialization.

IA-64 system calls use `break` with immediate `0x100000` and a frame with no
locals or rotating registers, at least six outputs and `ar.rsc == 0xC`.
A different PL3 break immediate or rejected PL3-writable frame is delivered
as an Exception to the thread's supervisor; an unsupervised thread stops
([IA-64 entry](Ia64UserSystemCallRuntime.md#kernel-entry-and-return)).
The AMD64 codec maps RAX, RDI, RSI, RDX, R10, R8, and R9 to the request number
and arguments zero through five. A response uses RAX for the error and RDX/R8
for values zero and one. The IA-64 codec maps GR15 and GR32 through GR37 to the
request, and GR8 through GR10 to the response. Its trusted input image also
records the NaT state of every input register: a known NaT becomes a canonical
`InvalidArguments` response in the native bridge, while malformed NaT metadata
is a kernel capture failure. The detailed, checked layouts are recorded in
[AMD64 codec](Amd64SystemCallRegisterCodec.md) and
[IA-64 codec](Ia64SystemCallRegisterCodec.md).

## Authority ingress

`SystemCallIngress` and the resumable WaitSet ingress accept the actual owning processor's
`ProcessorKernelRunLoop`; it never accepts a caller-selected
`ProtectionDomain`, `CapabilitySpace`, or copied authority snapshot. The run
loop returns its exact active domain pointer, AddressSpace pointer and identities
only while its state is Active. Ingress reacquires the live domain binding,
matches the ProtectionDomain and AddressSpace storage and identities, requires a
non-null CapabilitySpace, requires the recorded and live boot-unique
capability-space identifiers to agree, and only then calls a dispatcher. An
explicitly kernel-only ProtectionDomain carries the canonical null capability
pointer and invalid identifier pair, so it is rejected as
`CapabilitySpaceMissing` even when it is otherwise Ready and actively scheduled.

Endpoint calls, DebugWrite, FaultResolve, ObjectCreate, ObjectDestroy, MemoryMap and MemoryUnmap additionally require processor-owned
`IUserMemoryAccess` and
`SystemCallScratch` collaborators. The user-memory backend must be Ready and
name the exact active AddressSpace generation. Scratch is exclusive to the
non-reentrant dispatch and is cleared on entry and exit. Calling the ingress
overload without those collaborators remains valid for capability operations,
but completes an Endpoint operation with the canonical user-visible
`{InvalidConfiguration, 0, 0}` response and leaves the kernel-failure record
unchanged.

A completed user call returns through the normal response channel even when
the response is a syscall error. A run-loop, domain, or capability-space
invariant failure uses the separate kernel-ingress failure channel and leaves
the response unchanged. Success leaves that failure channel unchanged. The
caller enters without either ingress lock held, keeps local interrupts
disabled, and prevents same-CPU reentry into code that may acquire those locks
for the whole operation.

## Operations

All capability handles occupy argument 0 (selector) and argument 1
(authenticator). An unused argument must be zero.

| Number | Operation | Argument 2 | Argument 3 | Success value 0 | Success value 1 |
|---:|---|---|---|---|---|
| 1 | Close | zero | zero | zero | zero |
| 2 | Query | zero | zero | packed metadata | lease deadline |
| 3 | Derive | rights | lease deadline | new selector | new authenticator |
| 4 | Duplicate | rights | lease deadline | new selector | new authenticator |
| 5 | Revoke | zero | zero | zero | zero |
| 6 | IPC send | user frame address | exact `AIPC` frame size | endpoint sequence | zero |
| 7 | IPC receive | user delivery address | delivery capacity | endpoint sequence | exact delivery size |
| 8 | WaitSet wait | timeout in microseconds (0 for none) | zero | selected tag | registration sequence |
| 9 | DebugWrite | user text address | text size (1 to 256) | text size | zero |
| 10 | FaultResolve | resolution kind (1 Map, 2 Terminate) | user request address (Map) | exit code (Terminate) | zero |
| 11 | ObjectCreate | object type (1 ProtectionDomain, 2 AddressSpace, 3 CapabilitySpace, 4 Thread, 6 ResourceDomain, 7 MemoryObject, 8 Frame, 9 Endpoint, 12 WaitSet, 14 Interrupt, 16 IommuDomain, 17 FaultChannel) | user request address | new selector | new authenticator |
| 12 | ObjectDestroy | object type (1 to 4, 6 to 9, 12, 14 or 16) | zero | exit code (4), zero otherwise | zero |
| 13 | MemoryMap (arguments 0 and 1 zero) | zero | user request address | zero | zero |
| 14 | MemoryUnmap (an AddressSpace handle) | virtual start | page count | zero | zero |
| 15 | ThreadExit (argument 0 the exit code, argument 1 zero) | zero | zero | does not return | does not return |
| 16 | CapabilityGrant (the source handle) | destination CapabilitySpace selector | its authenticator | destination selector | destination authenticator |
| 17 | ThreadTerminate (a Thread handle) | zero | zero | zero | zero |
| 18 | WaitSetBind (a WaitSet handle) | Endpoint or Interrupt selector | its authenticator | zero | zero |
| 19 | DeviceControl (a PciFunction handle) | operation (1 Assign, 2 Release, 3 EnableBusMaster, 4 DisableBusMaster, 5 Quiesced) | ProtectionDomain selector (Assign), assignment sequence (Quiesced) | the assignment's sequence (Assign) | zero |
| 21 | InterruptControl (an Interrupt handle) | operation (1 Rearm, 2 KeepMasked) | zero | the last event's sequence | zero |
| 20 | IommuControl (an IommuDomain handle) | operation (1 Attach, 2 Detach, 3 Map, 4 Unmap, 5 Faults) | PciFunction selector (Attach, Detach), block address (Map), device address (Unmap) | the fault word (Faults), zero otherwise | zero |

CapabilityGrant ([domain creation ABI](SystemCallAbi.md)) puts one capability of the caller's
space into a CapabilitySpace the caller holds with `Manage`, such as one it created for a component. Argument 4 holds
the requested rights in bits 0-15 and the source's object type in bits 16-23, every other bit zero; argument 5 is the
disposition, 1 move (the source needs `Transfer` and leaves the caller's space) or 2 copy (the source needs
`Duplicate`, and the copy may not carry `Revoke`, which over a shared revocation domain could revoke the source). The
granted capability keeps the source's revocation domain and lease deadline and carries the requested rights, which
must be a subset of the source's. The kernel checks everything before changing either space, as an IPC attachment's
transfer does; success returns the destination handle, which the caller writes where the receiving program finds it.
Another disposition, a zero type or a reserved bit is `InvalidArguments`; a destination handle that is not a
CapabilitySpace with `Manage`, or a source without the disposition's right, reports its capability error.

Derive's argument 4 carries flags: bit 0
(`SystemCallDeriveChildRevocationDomain`) gives the result a child revocation
domain, and every other bit is zero. FaultResolve's, ObjectCreate's and MemoryMap's argument 4
is the request size. Argument 5, and argument 4 of every other operation, are zero. Query packs
the stable
object-type value in bits 0 through 7 and capability rights in bits 16 through
31 of value 0. Bits 8 through 15 and 32 through 63 are zero. Value 1 is the
absolute monotonic lease deadline; all-one bits mean an infinite lease.

The default IA-64 `MinimalUserImage` receives one infinite
`Inspect|Invoke|Read` self-receiving Endpoint capability in `r4`/`r5` and the
DiagnosticChannel capability described under DebugWrite in `r6`/`r7`. Its
canonical Query
response is `r8 = 0`, `r9 = 0x0000000000070009`, and
`r10 = UINT64_MAX` (`0xffffffffffffffff`). It then uses that same handle to send
one canonical 88-byte attachment-free frame and receive its exact 136-byte
delivery. The successful IPC responses are `{0, 1, 0}` and `{0, 1, 136}`; the
shared sequence value demonstrates that receive consumed the message published
by send. All three responses, the outbound frame, the delivery, and a completion
sentinel remain in image memory before the stable PL3 idle loop. The explicit
synthetic-user-probe diagnostic additionally waits through a separately staged
infinite `Wait`-only WaitSet handle. The resumed response is
`{0, 0x5741495453455401, 1}`, proving selection of the registered Notification
tag and exact registration generation.

For IPC send, argument 3 is between 88 and 504 bytes. For IPC receive, it is a
capacity between 136 and 552 bytes. Both user intervals must be non-null,
non-wrapping, and exact. The rights input to Derive and Duplicate is exactly 16 bits. Zero, bits above bit 15, and any undefined
bit are rejected before capability-space mutation. Object-type values and
rights decoding are shared with typed IPC through
`CapabilityWireTypes.hpp`, so the two external boundaries cannot acquire
independent registries.

## Capability semantics

Capability rights occupy bits 0-13 (bits 14-15 remain reserved):

| Right | Bit | Encoding |
| --- | ---: | ---: |
| Inspect | 0 | `0x0001` |
| Invoke | 1 | `0x0002` |
| Read | 2 | `0x0004` |
| Write | 3 | `0x0008` |
| Map | 4 | `0x0010` |
| Wait | 5 | `0x0020` |
| Signal | 6 | `0x0040` |
| Transfer | 7 | `0x0080` |
| Duplicate | 8 | `0x0100` |
| Derive | 9 | `0x0200` |
| Revoke | 10 | `0x0400` |
| Manage | 11 | `0x0800` |
| Debug | 12 | `0x1000` |
| Code | 13 | `0x2000` |

`Code` on a ResourceDomain authorizing ObjectCreate is required for Execute in
MemoryObject maximum permissions; missing it returns `RightsDenied`. Derive,
Duplicate, grants and IPC copies can preserve or drop Code and cannot add it.
The pager grant has Manage, Inspect and Code; component account copies remain
Manage and Inspect (`0x0801`), with no Code ([shared executable segments](Elf64Image.md)).


Close authenticates one exact handle generation and frees that slot. It remains
available for explicit cleanup after expiry or revocation. It grants no object
operation and returns no metadata.

Query requires `Inspect`. It checks the handle, current revocation epoch,
deadline, object generation, type, and immutable owner metadata while holding
the common capability lock. The syscall exports only type, rights, and deadline;
the internal object identifier, revocation domain, sealed selector, and charge
owner do not cross the boundary. The returned values are an observation, not a
retained object lease.

Derive requires `Derive`, nonempty rights contained by the source, and a
deadline no later than the source. Its result must have strictly fewer rights
or, with flag bit 0, a new child of the source's revocation domain; a shorter
lease alone is no attenuation, since any far-future deadline is as good as an
infinite one and Duplicate already grants equal leases. Duplicate requires
`Duplicate` and permits either equality or attenuation. Both return a newly
authenticated handle only after all checks and slot reservation succeed.

A result in its source's revocation domain may not carry `Revoke`: revoking
that domain would revoke the source itself, and the delegator with it. Derive
without flag bit 0 and Duplicate therefore reject requested rights containing
`Revoke` with `RightsAmplification`, and so does an IPC copy attachment. A
child-domain result may keep every right of its source, `Revoke` included.
Derive creates the child only after every other check and a free slot are
confirmed, so a failure creates no domain; a source domain already eight levels
deep, or a registry holding 64 domains, fails it with
`RevocationDomainExhausted`. A child domain remains occupied until an ancestor is destroyed; see the
[capability model](CapabilityModel.md#handle-encoding).

Revoke requires `Revoke` and advances the referenced revocation domain while
the same shared lock still protects authorization. The operation therefore
linearizes as one authorization-and-epoch transition. It revokes the invoking
handle and every other handle in that domain or in a descendant of it; those
handles remain closeable. Revoking a child-domain branch therefore leaves its
source live, and revoking the source ends every branch derived from it.

## Endpoint IPC semantics

IPC send first copies and decodes one canonical `AIPC` frame into
processor-local scratch. Copy or decode failure occurs before Endpoint lookup
or queue mutation. The Endpoint handle must authorize `Invoke`. The lookup
finds and pins the Endpoint; the send then authorizes the same handle again in
the critical section that publishes the message, so a `Revoke`, `Close`, or
expiry ordered before the publication fails the send with that capability
error and publishes nothing. The queued message records the invoking
capability's effective rights and sealed object selector as trusted metadata;
neither value is accepted from message bytes. User calls always use the normal
send class, so emergency `ReservedControl` capacity remains reachable only
through a trusted kernel path. A successful send returns the Endpoint-local
sequence.

IPC receive requires `Read` and additionally requires the invoking
CapabilitySpace to be the Endpoint's exact receiver space. It reserves and
decodes the FIFO head without consuming it, encodes a canonical delivery into
scratch, and prepares the exact user write interval. Insufficient capacity or a
fallible user-write preparation cancels the receive reservation and leaves the
FIFO and user bytes unchanged. Only after user publication has become
contractually infallible does the dispatcher consume the queue head and commit
the copy. The handle is authorized again when the head is reserved and in the
critical section that consumes it; authority that ended in between cancels the
reservation and returns its capability error with nothing consumed or written.
A successful receive returns the sequence and exact delivery extent.

The delivery is a 48-byte little-endian `AIPD` header followed immediately by
the complete canonical `AIPC` frame. The header carries total and frame sizes,
effective sender Endpoint rights, sequence, sender resource-domain identifier,
sealed object selector, attachment count, and send class. Its six reserved
bytes are zero. The total delivery is bounded to 552 bytes.

## DebugWrite semantics

DebugWrite requires `Write` on an exact DiagnosticChannel capability (object
type 22). The text is 1 to 256 bytes of printable ASCII (0x20 through 0x7E)
and line feeds, and its last byte is a line feed, so every call writes whole
lines. The kernel copies and validates the whole text before any output; a
rejected call (`InvalidArguments`) writes nothing. Each line reaches the
kernel diagnostic device as one `USER:` line that starts on a fresh line, so
it cannot imitate a kernel `ASTERIA:` line. Both syscall paths run with local
interrupts disabled, so nothing on the same processor interleaves with a
line; diagnostics of another processor could, and the host treats a malformed
line as a failure. The bound keeps the copy and the device write short.

Both kernels hand the initial user image one DiagnosticChannel capability:
`Inspect|Write` (so the image may Query it; Write is its only operation), an
infinite lease, and a revocation root of its own, separate from the bootstrap
Endpoint's. Its selector and authenticator arrive in R8/R9 on AMD64, the
fifth and sixth integer arguments of `AsteriaAmd64MinimalUserImageMain`, and in
`r6`/`r7` on IA-64, whose entry stub forwards `r4` through `r7` as the four
arguments of `AsteriaIa64MinimalUserImageMain`. The initial user image of a
test firmware uses the channel for its TAP report; the production images
ignore it.

## FaultResolve semantics

FaultResolve is the pager's half of the [fault-to-pager round
trip](PagerChannel.md). Arguments 0 and 1 are the
FaultToken handle the pager received with the fault message, argument 2 is the
resolution kind (Map 1 or Terminate 2), and for Map argument 3 is the address of one
request in the pager's memory and argument 4 is its size, exactly
`FaultResolveRequestSizeBytes` (88); argument 5 is zero. The request holds three
handles of selector and authenticator at offsets 0, 16 and 32 (the AddressSpace
that faulted, then the MemoryObject and Frame to map), then the virtual start,
page count, MemoryObject page offset and Frame page offset at offsets 48, 56, 64
and 72, then one permission byte at offset 80 (Read 1, Write 2, Execute 4,
User 8) and seven zero bytes; every word is little-endian. The kernel copies
and validates the whole request before any lookup: another kind or size, a
nonzero reserved argument or byte, or an undefined permission bit is
`InvalidArguments`.

Any domain may call FaultResolve. The dispatcher leases the Invoke-authorized
token in the caller's space, takes a pin on its issuing channel and releases the
lease before Resolve, so token retirement can collect it. The caller's
`PagerResolution` field does not select a channel.
The [pager channel](PagerChannel.md) then proves the caller's authority from
its own capability space. The token needs `Invoke` and must name the fault the
channel delivered; a resolved or retired token, or one for no pending fault, is
`ObjectNotFound`. The AddressSpace handle needs `Map` and must name the space
that faulted, or the call is `RightsDenied`; this is the check that a pager maps
only where it was asked to. The MemoryObject and Frame handles each need `Map`.
A handle that fails its lookup reports that capability error. The mapping must
cover the faulting address and stay within both objects, their cache policy and
W^X, or it is `InvalidArguments` and the fault stays pending for a corrected
request. A fault whose cause is an exception
([domain creation ABI](SystemCallAbi.md)) is `InvalidArguments`
for every Map: only Terminate ends it. Success maps the pages, wakes the faulting thread at its faulting
instruction, retires the token and returns `{None, 0, 0}`. A domain without a
pager channel gets `InvalidConfiguration`, as does a resolution the kernel
could not finish.

Terminate ([supervised fault termination](ThreadLifecycle.md)) carries
no request: arguments 3, 4 and 5 are zero, or the call is `InvalidArguments`.
The token needs `Invoke` and must name the delivered fault, as for Map. The
kernel exits the faulting thread with `FaultTerminatedExitCode`
(`0x8000000000000002`; bit 63 marks an exit code the kernel chose), never wakes
it, retires the token and returns `{None, FaultTerminatedExitCode, 0}`; the
channel then delivers the next fault. The kind states the authority, not a
policy: whoever holds `Invoke` on the token may end the faulting thread instead
of mapping. The exited thread's kernel context, run-loop binding and
SchedulingContext stay as they are until its domain is torn down.

## Object management semantics

ObjectCreate and ObjectDestroy create and destroy ProtectionDomains, AddressSpaces, CapabilitySpaces, Threads, MemoryObjects, Frames and Endpoints (object
types 1, 2, 3, 4, 7, 8 and 9, the values Query reports). The ingress supplies the kernel's object management (`IObjectManagement`) per domain; a domain
without it gets `InvalidConfiguration`. The pointer selects the mechanism, not the authority, which comes from the
caller's capabilities.

ObjectCreate: arguments 0 and 1 name a ResourceDomain capability with `Manage`, the domain charged; argument 2 is the
object type; argument 3 is the address of one request in the caller's memory and argument 4 its size, exactly
`ObjectCreateEndpointRequestSizeBytes` (40); argument 5 is zero. Another type, size or a nonzero argument 5 is
`InvalidArguments` before user memory is read. An Endpoint's request (40 bytes) holds a receiver capability-space
handle of selector and
authenticator at offset 0, zero for the caller's own space or a CapabilitySpace handle with `Manage` (a created space, which the Endpoint pins while it lives), then the Endpoint
configuration: queued bytes, reserved control bytes and bytes per sender (16 bits each) at
16, 18 and 20, then queued messages, reserved control messages, messages per sender, queued attachments, reserved
control attachments and attachments per sender (one byte each) at 22 through 27, four zero bytes, and at 32 the sealed
selector the creator's capability carries ([service IPC](IpcWireFormat.md)), zero for none,
which every copy inherits (Duplicate, Derive, CapabilityGrant and IPC transfer copy the descriptor) and every delivery
reports as the invoking capability's; every field is little-endian. A nonzero reserved byte, or a configuration the
Endpoint rejects, is `InvalidArguments`; a 32-byte request is refused by its size.

The kernel looks up the ResourceDomain capability for `Manage` (its capability error otherwise), charges that domain
the Endpoint's size in kernel-metadata bytes and the configured queued bytes in IPC-queue bytes, constructs the
Endpoint with that receiver space (the caller's by default), registers it, and mints its first capability into
the caller's space with Inspect, Invoke, Read, Transfer, Duplicate, Derive and Manage, never Revoke. The mint
re-authorizes the ResourceDomain capability in the same critical section and puts the new capability in that
capability's revocation domain with its lease deadline and a zero sealed selector, so revoking or expiring the grant
ends everything created under it. Success returns `{None, selector, authenticator}`. A full Endpoint pool, a domain
limit or a full object registry is `ResourceExhausted`; a full capability space is `SpaceExhausted`. Every failure
leaves no object, charge or capability behind.

A Frame's legacy request (16 bytes) holds its page count (1 to 128). Byte 8 is the Frame's physical alignment in pages: 0 or 1
for a page, or 2, 4, 8 or 16 for a Frame of RAM, whose physical address a device sees on the trusted-driver profile
([DMA isolation](IommuDomain.md));
another value, alignment on a device Frame, or a nonzero byte after it is `InvalidArguments`. The kernel charges the
ResourceDomain those pages, committed and resident, and `sizeof(Frame)`, takes that many zeroed pages from the frame
pool, and mints Inspect, Map, Transfer, Duplicate, Derive and Manage; a charge beyond the domain's limits, a full pool
and a kernel without a frame pool are `ResourceExhausted`.
The 24-byte RAM form supports desktop-session allocations: pages 1–512 at 0, log2 alignment 0–9 at 8, address limit 0 or 32
at 9, and fourteen zero bytes. The legacy form keeps limit 32 and its bounds.
Memory-pressure notifications are unsupported; allocation refusals return
`ResourceExhausted`.
A device Frame's request (40 bytes,
[driver isolation](IommuDomain.md)) holds the same page count and eight zero bytes, then a
memory-mapped DeviceRegion handle with `Map` (a zero handle is `InvalidArguments`) and the page of the region its first
page is: the Frame names those pages of the region with the region's cache policy, takes none from the frame pool,
pins the region and charges `sizeof(Frame)` only; a range beyond the region, a port-I/O or unaligned region is
`InvalidArguments`. A MemoryObject's request (16 bytes) holds its page count (1 to 8,192), one byte of maximum
permissions (Read 1, Write 2, Execute 4, User 8; Read required), one byte of cache policy (0 write-back, 1 uncached,
2 write-combining; another value is `InvalidArguments`) and six zero bytes. It is Pinned, charges its size only, and
receives the same rights. Execute in its maximum permissions additionally requires `Code` on the authorizing
ResourceDomain capability, else `RightsDenied` ([shared executable segments](Elf64Image.md)). The two are independent: a mapping (MemoryMap) names one of each, and the Frame, which owns
the physical pages, carries the page charge ([resource accounting](ResourceDomain.md); boot
memory's MemoryObjects still charge theirs). MemoryMap requires the two cache policies equal; a device Frame maps
never executable and retains its region for the mapping, and the kernel never reads or writes user memory through
it.

A ResourceDomain's legacy request (56 bytes, [resource accounting](ResourceDomain.md)) holds the child's
limits as seven words in `ResourceDomainLimits` order: committed and resident pages, kernel-metadata bytes and
capability handles (both zero, else `InvalidArguments`: the child takes its parent's metadata limit), IPC queue
bytes, threads and child domains. The child is created under the authorizing domain within its limits (a limit beyond
the parent's, a depth beyond eight or an ancestor without a free `ChildDomains` is `InvalidArguments`), the parent is
charged the child's slot, the child takes the next identity of a never-reused counter, and its first capability
carries Manage, Inspect, Duplicate, Transfer and Derive. Its eight object limits
inherit the parent's. The 80-byte form adds flags at 56 (bit 0 Code, other bits
InvalidArguments) and eight little-endian UInt16 limits at 64 in order:
ProtectionDomains, AddressSpaces, CapabilitySpaces, Endpoints, WaitSets,
FaultChannels, Frames, MemoryObjects. Code requires Code on the authorizing
capability or RightsDenied before allocation. Each limited creation reserves
one unit in every ancestor, refuses exhaustion with ResourceExhausted and
releases the unit at collection. The 88-byte form adds
a UInt16 large-page-table-arena limit at 80 and six zero bytes; the shorter
forms select zero. The arena limit is bounded by the parent's and retained.
A flagged AddressSpace reserves one unit through every ancestor and releases
it at collection. The boot root's limit is two.
A platform that has not configured the pool (both firmwares,
until the root service uses it) answers `InvalidConfiguration`. ObjectDestroy of a ResourceDomain drains it: while a thread charged to it or to a descendant has not exited, the call is `WouldBlock` and changes nothing that a
repeated call would not redo. Otherwise the kernel destroys every object charged to the domain or a descendant, pool by
pool in collection order, whoever holds its capability, then the domain and its descendants, and returns
`{None, 0, 0}`; every capability to any of them now fails with `ObjectNotFound`. A ProtectionDomain the platform cannot
retire yet makes the call `WouldBlock` after the other objects were destroyed, and the call is repeated. A drained
object that a surviving AddressSpace maps stays quiescing until that space unmaps it, and keeps its domain, slot and
charge until then; each domain returns its slot and its parent's charge once nothing charged to it is left.

ObjectDestroy: arguments 0 and 1 name the object with `Manage`, argument 2 its type, arguments 3 to 5 are zero
(`InvalidArguments` otherwise). An object the kernel did not create through ObjectCreate is `RightsDenied`. A
destroyed Frame's pages are not handed out again until the kernel zeroed them. Success
returns `{None, 0, 0}`: the object is quiescing, every capability to it in any space now fails with `ObjectNotFound`
and stays closeable. `Close` never destroys. The object's storage and charge return once nothing references it:
ObjectDestroy reclaims its object before it returns unless another holder still references it, and the kernel
collects every such object at the start of each ObjectCreate and ObjectDestroy. An operation the kernel can neither complete nor undo is a kernel-ingress failure,
not a response.

A Thread's request (64 bytes, thread creation ABI) holds a ProtectionDomain handle at offset 0, zero for the caller's own
domain or a domain the caller holds with `Manage` (domain creation ABI; a domain the platform has no record of is
`InvalidArguments`), the entry, user stack top (16-byte aligned, else `InvalidArguments`) and argument at 16, 24 and
32, a budget and a period at 40 and 48 that must be zero (the platform's user scheduling configuration on CPU 0), and a thread-pointer word at 56 reserved for static TLS. It must be zero; nonzero is `InvalidArguments`
([shared executable segments](Elf64Image.md)). The kernel charges the
ResourceDomain one `Threads` and the slot's kernel metadata (Thread, SchedulingContext and the platform's native slot),
starts the thread in the caller's domain with the argument in its first argument register, and mints Inspect, Manage,
Transfer, Duplicate and Derive; the thread is Ready when the call returns. A full thread pool or native slot table is
`ResourceExhausted`. On IA-64 the entry word is the address of a function descriptor {ip, gp} in the
caller's memory, which the kernel reads (ip 16-byte aligned in regions 1-7; its page is the code range), the stack top
names a 32 KiB region whose lower 16 KiB is the RSE backing store (growing up) and upper 16 KiB the memory stack
(growing down, SP = top - 16), and the argument arrives in r4 with r5-r7 zero; an unreadable descriptor or a start
outside those shapes is `InvalidArguments`. ObjectDestroy of a Thread accepts only
an exited one: it returns `{None, exit code, 0}` and makes the thread quiescing; a thread that has not exited is
`WouldBlock` and keeps running, so polling ObjectDestroy waits for an exit. The slot, its `Threads` charge and its
metadata return once the thread is collected and the platform's idle loop released its native slot, so a creator may
see `ResourceExhausted` for a moment after reaping.

A CapabilitySpace's request (32 bytes, domain creation ABI) is all zero. The kernel takes a free slot of its
sixteen-slot pool, charges the ResourceDomain the space's size in kernel-metadata bytes, constructs an empty space that
shares the boot spaces' registries, clock and lock (with the slot's own key and a never-reused space identifier), and
mints Inspect, Manage, Transfer, Duplicate and Derive. A kernel that has not configured the pool, or a full one, is
`ResourceExhausted`. ObjectDestroy of a CapabilitySpace makes it quiescing and returns `{None, 0, 0}`; the kernel
reclaims it, dropping every capability it holds, once no ProtectionDomain binds it and no Endpoint receives in it, and
its slot then builds a space with a new identifier.

An AddressSpace's request (32 bytes, domain creation ABI and desktop session)
accepts byte 0 bit 0 for a large page-table arena. Any other bit or nonzero byte
is InvalidArguments. The flag selects one of two reserved 256-page AMD64 arenas
instead of the slot's 64-page arena and charges one ResourceDomain arena unit;
on IA-64 it charges the unit and selects no additional storage. No free arena
or no domain unit is ResourceExhausted. Collection releases both the unit and
the selected storage's metadata charge. The platform has a fixed number of
page-table roots prepared from the boot seed (the kernel's mappings and an empty user half); the kernel charges the
ResourceDomain one root's bytes (the AddressSpace and its page-table arena) in kernel metadata, initializes a free
root's AddressSpace charged to that domain, registers and binds it, and mints Inspect, Map, Manage, Transfer,
Duplicate and Derive; MemoryMap and MemoryUnmap then work on it through that capability. No free root, or a kernel
that has none, is `ResourceExhausted`, and so is a root that cannot be prepared, which then stays unusable for the
boot. ObjectDestroy of an AddressSpace makes it quiescing and returns `{None, 0, 0}`; once it is resident nowhere and
no ProtectionDomain, fault or lease pins it, the kernel quiesces it, which releases what mappings left in it pin, and
the next creation prepares its root again.

A ProtectionDomain's legacy request (64 bytes, domain creation ABI) holds an AddressSpace handle at 0 and a
CapabilitySpace handle at 16, each with `Manage`, the domain's supervision tag at 32 (nonzero: its creator's name for
it in every fault delivery of its threads), a billing ResourceDomain handle at 40 that must be zero
(the authorizing domain also charges the new domain's activity) and eight zero bytes. The kernel binds the domain to
both spaces (pinning the AddressSpace, the ResourceDomain and the CapabilitySpace), charges its size, has the platform
record it for the threads that will run in it, and mints Inspect, Manage, Transfer, Duplicate and Derive. A zero tag
or a nonzero reserved byte is `InvalidArguments`; a handle of the wrong type or without `Manage` reports its capability
error; a full pool, or a platform without a free record, is `ResourceExhausted`. ObjectDestroy of a ProtectionDomain is
`WouldBlock` while a thread is bound in it; otherwise the platform forgets the domain, every capability to it fails
with `ObjectNotFound`, and the kernel releases its pins and charge once nothing references it.

The 64-byte form is restricted to boot callers; other callers
receive InvalidArguments. The 80-byte form names a Manage-authorized FaultChannel
at 56 and requires bytes 72–79 zero. Its charge domain must be that channel's
domain or a descendant. A tag already held by an uncollected ProtectionDomain
on the selected channel (including the boot channel) is InvalidArguments.
Protocol 1.2 carries a 48-byte payload: FaultRecord, tag and the kernel-assigned
ProtectionDomain identity, in that order. Supervisors validate identity first.

FaultChannel creation takes eight zero bytes and Manage on the charged
ResourceDomain. Four pooled channels per ISA each own
a private receiver Endpoint, readiness Notification, token space, revocation
root and sixteen-token factory. The initial capability has Inspect, Read,
Manage, Duplicate, Transfer and Derive. IpcReceive and WaitSetBind accept Read
on the channel; ObjectDestroy requires Manage and returns WouldBlock while
a ProtectionDomain references it. Collection and drain visit ProtectionDomains,
then FaultChannels, then CapabilitySpaces; retained leases and WaitSet pins
delay final collection.

ThreadExit (system call 15): argument 0 is the exit code with bit 63 clear (codes with bit 63 set are the kernel's),
arguments 1 to 5 are zero; otherwise `InvalidArguments`. It does not return: the calling thread finishes its run
Exited with that code. Only created threads end this way: a boot thread's ThreadExit answers `UnknownSystemCall`. On
AMD64 a created thread's WaitSetWait waits in a dispatcher of its own thread slot, which the idle loop and every user
tick progress like the pager's, with a timeout as the pager's ([service IPC](IpcWireFormat.md)); on IA-64 it still answers `InvalidConfiguration`. ThreadTerminate of an AMD64 created thread blocked in
WaitSetWait aborts the wait and answers `None`: the thread is woken,
and its system call ends it with `KilledExitCode` instead of returning; its ObjectDestroy answers `WouldBlock` until
then.

MemoryMap : arguments 0 to 2 and 5 are zero, argument 3 is the address of FaultResolve's
88-byte request block and argument 4 its size; another size, a nonzero reserved argument or byte, or an undefined
permission bit is `InvalidArguments`. The block names an AddressSpace, a MemoryObject and a Frame, each a capability
with `Map` in the caller's space (its capability error otherwise), then the virtual start, page count, MemoryObject
and Frame page offsets and permissions. The permissions must include User and Read and must not combine Write with
Execute; the range must lie within both objects and the MemoryObject's maximum permissions, and the MemoryObject must
be Pinned with the Frame's cache policy (`InvalidArguments` otherwise, and for an overlapping range or a writable and
executable alias of the Frame's pages in any space). The kernel installs the mapping, invalidates every processor
the AddressSpace is resident on and returns `{None, 0, 0}`; the AddressSpace keeps both objects pinned, so a
destroyed Frame's pages stay allocated while the mapping exists. A full mapping table or page-table arena is
`ResourceExhausted`, another transaction of the AddressSpace in flight is `WouldBlock`, and a kernel without a mapper
is `InvalidConfiguration`. Runtime mappings are not visible to the kernel's user-memory access, so a request
block or message buffer must stay in the image's own memory. The kernel does not make memory it maps executable
coherent with instruction fetch: whoever wrote code into the frame flushes it first (on IA-64 `fc.i` over the bytes
through a mapping it holds, then `sync.i` and `srlz.i`; AMD64 needs nothing).

MemoryUnmap: arguments 0 and 1 name the AddressSpace with `Map`, argument 2 is the virtual start and argument 3 the
page count of exactly one mapping MemoryMap made; arguments 4 and 5 are zero (`InvalidArguments` otherwise, and for a
range that is not exactly one such mapping, including a seed mapping). The kernel clears the translations, invalidates
every processor the AddressSpace is resident on, and only then releases the mapping's pins, the Frame's W^X
reservation and the MemoryObject's mapping and frees the record; a Frame or MemoryObject destroyed while mapped is
reclaimed before the call returns. Success returns `{None, 0, 0}`; another transaction of the space in flight is
`WouldBlock`, and a kernel without a configured mapper `InvalidConfiguration`. A destroyed Frame's pages are
handed out again only after a management call zeroed them, which AMD64 does in a root resident on the calling
processor alone; until then they stay out of the pool.

ThreadTerminate: arguments 0 and 1 name a Thread with
`Manage`, arguments 2 to 5 are zero (`InvalidArguments` otherwise). Like ObjectCreate and ObjectDestroy it is a
management call. A Ready thread is taken out of its processor's ready queue and ends with `Abi::KilledExitCode`
(`0x8000'0000'0000'0003`, bit 63 marking a code the kernel chose), which its ObjectDestroy then returns; its context
is never resumed. A thread that already exited is left as it is (`None`). A thread blocked on a page fault is
`WouldBlock`: the fault reaches its supervisor, which ends it through FaultResolve, so the pager channel is never
bypassed. The caller itself is `InvalidArguments` (ThreadExit ends the caller), and a kernel whose platform cannot
remove a thread answers `InvalidConfiguration`. A kill wakes no supervisor.

## WaitSet suspension semantics

WaitSet wait requires `Wait` on an exact WaitSet capability. Argument 2 is a
relative timeout in microseconds, zero for none and at most `0xFFFF'FFFF`
(`InvalidArguments` beyond); arguments 3 through 5 are zero. A kernel whose
platform does not expire waits answers a nonzero timeout `InvalidConfiguration`.
When the timeout passes before a member is ready, the wait returns
`{TimedOut, 0, 0}`; readiness found at the same check wins. The deadline is set
in the processor's local ticks when the wait is prepared and is checked where
the kernel already regains control while the waiter is blocked (every user
tick and idle pass on both ISAs; an idle IA-64 BSP polls the ITC up to the
deadline), so a wait may expire up to one tick late. AMD64 converts with the
TSC rate its APIC timer calibration measured, IA-64 with the ITC rate the boot
measured across its firmware stall. Thread and
SchedulingContext identity are not
ABI inputs: the dispatcher obtains them from the owning processor's exact
active run-loop binding and pins those generations independently of the
dispatch-scoped capability lease.

The IA-64 bridge treats its processor-local WaitSet continuation and dispatcher
as one optional pair. With both absent, every other syscall retains its normal
behavior and WaitSet wait completes with the ordinary
`{InvalidConfiguration, 0, 0}` ABI response. An exactly-one-null pair is a
malformed trusted bridge configuration: it uses the kernel diagnostic channel
and leaves the response registers and resume coordinates unchanged.

If readiness is already available, the syscall consumes the exact completion
and returns normally. Otherwise the initial ingress returns an internal
`SuspendPrepared` disposition. IA-64 commits that transaction to its retained
synchronous continuation, restores the selected worker supervisor context, and
finishes the exact Thread run as Blocked with the scheduler-created block token.
After WaitSet polling wakes and readmits that Thread, the resumed ingress
consumes the retained completion once, without accepting a user handle or
repeating capability lookup, and restores the original syscall frame with the
tag and registration sequence.

AMD64 retains its exclusive syscall stack across the same block/readmission
transaction and returns through IRETQ; its exact ownership contract is in the
[native runtime](Amd64BootRuntime.md#native-entry-contract).

Prepared and completed transactions are bound to a stable dispatcher instance
and nonwrapping transaction generation. Stale, cross-dispatcher, foreign-thread,
or repeated completion receipts cannot consume the active transaction. A
non-completed observation is non-destructive, so a concurrent completion
remains retained for the next exact consume attempt.

## WaitSet binding

WaitSetBind ([service IPC](IpcWireFormat.md)) needs `Manage` on the WaitSet in arguments 0
and 1, created or not, and `Read` on the Endpoint in arguments 2 and 3. Argument 4 is the tag WaitSetWait returns
for the Endpoint, nonzero; argument 5 is zero. It registers the Endpoint's readiness Notification in the WaitSet: the
Endpoint signals it whenever a message is published and whenever a receive leaves messages queued, so the wait is
level-triggered, and a receiver that wakes receives until `WouldBlock` before it waits again. An Endpoint that is
already bound, or one the kernel built for itself (it has no readiness), is `InvalidArguments`, as are a zero tag and
a nonzero argument 5; a WaitSet with all 32 entries taken is `ResourceExhausted`. There is no unbind: destroying the
WaitSet drops its registrations, and an Endpoint destroyed while bound stays reserved until its WaitSet is collected.

## Device control

DeviceControl operation 8 is unsupported and returns `InvalidArguments`.
[PowerOff](PowerOff.md) defines operations 9 (query), 10 (shutdown),
and 11 (reboot) for the exact root ResourceDomain with `Manage`. Arguments 3–5 are
zero. Manage on a child domain or an inspect-only root handle is insufficient.
Query value 0 has bits 0 (S5) and 1 (reboot: FADT reset or IA-64 SAL INIT); bit 2 is reserved for mapped
EFI runtime services and is currently clear on both ISAs. Unsupported
transitions return `InvalidConfiguration` before disabling interrupts; an
admitted transition parks every other online CPU and does not return. Root
must complete the filesystem/device shutdown protocol before invoking it.

DeviceControl ([driver isolation](IommuDomain.md)) operates on the PciFunction in arguments 0
and 1. Assign and Release are the Device Manager's: they need `Manage` on the function, and Assign also `Manage` on
the ProtectionDomain in arguments 3 and 4, which becomes the function's one assignee. Assignment first turns the
function's bus mastering off and releases any earlier assignee; it clears the revoked latch. EnableBusMaster and
DisableBusMaster need `Invoke` and that the caller's ProtectionDomain is the assignee. Enabling also needs the latch
clear and the function attached to a live IommuDomain; until a platform has one, it is `InvalidConfiguration`.
DisableBusMaster with `Manage` instead is the revocation: it sets the latch, whoever calls it, and only a later
assignment clears it. Assign returns the assignment's sequence in value 0. Quiesced (`Manage`) is the Device Manager's confirmation that a function a drain took from a dead driver is quiet: argument
3 names the drained assignment's sequence, argument 4 is zero; it lifts that quarantine only (`InvalidArguments`
otherwise). Until then the function refuses every assignment (`WouldBlock`). Arguments 3 and 4 are zero but for
Assign and Quiesced, and argument 5 is always zero; another operation is `InvalidArguments`. A function that is not assigned, or one the platform cannot assign, is `InvalidArguments`; a
caller that is not the assignee, or a revoked function, is `RightsDenied`; more than four assigned functions is
`ResourceExhausted`. A function whose MSI serves an Interrupt cannot be released or reassigned until that Interrupt is
destroyed (`WouldBlock`); an assignment turns a former driver's MSI off first. A kernel without run-time configuration access answers `InvalidConfiguration`.

Destroying a ResourceDomain stops a dead driver's devices before the subtree's objects drain, whatever
order its manager destroys things in:

0. It ends every Interrupt charged to the subtree: messages off, any pending event acknowledged, masked, quiesced.
1. It turns off the bus mastering of every function whose assignee is charged to the drained subtree, revoked until a
   reassignment.
2. It quiesces and destroys every IommuDomain charged to the subtree.
3. It detaches those functions from any surviving IommuDomain and releases them into quarantine.

A dead driver's device therefore stops writing memory before that memory is reused, and no driver gets it again before
its Device Manager confirmed it quiet: a transfer the dead driver started may still be running in the device. A
configuration write one of these steps does not get makes the drain `WouldBlock`, before anything of the subtree's
memory goes.

## Interrupts

ObjectCreate type 14 ([driver isolation](IommuDomain.md)) creates an Interrupt for a PCI
function's interrupt as the platform routes it, charged to the authorizing ResourceDomain. Its 24-byte request holds a
PciFunction capability with `Invoke` (whose assignee the caller's ProtectionDomain must be) and the source index, 0.
Where the kernel has message vectors (AMD64) it binds a processor vector and programs the function's MSI to raise it,
with INTx disabled. Where it has interrupt lines instead (IA-64, where an untrusted driver has no MSI) the
Interrupt takes the function's INTx line (configuration byte 0x3C; a function without an interrupt pin has none) with
a vector of its own, and the kernel clears the function's INTx Disable; one Interrupt per line. Either way one
Interrupt per function, more is `WouldBlock`, and assignment leaves INTx disabled. The first capability carries
Inspect, Wait, Manage, Transfer, Duplicate and Derive. The Interrupt starts masked. A kernel with neither answers
`InvalidConfiguration`.

WaitSetBind takes an Interrupt with `Wait` in place of an Endpoint: its event Notification is registered under the
tag. InterruptControl (system call 21, `Manage`) with Rearm acknowledges the pending event, if any, and arms the
Interrupt; with KeepMasked it acknowledges and masks. Value 0 is the last event's sequence. A message that arrives
while the Interrupt is masked is delivered at the next arm; one that arrives while an event is pending coalesces into
it, so a driver reads its device's status again after it rearms. A waiter wakes within a timer tick of the message.
ObjectDestroy turns the function's MSI (or INTx) off, acknowledges and masks, and quiesces; the vector returns once nothing
references the Interrupt, and its Notification stays while a WaitSet registration holds it.

## IOMMU domains

ObjectCreate type 16 ([driver isolation](IommuDomain.md)) creates an IommuDomain charged to the
authorizing ResourceDomain. Its 8-byte request holds the device address width in byte 0 (32 to 39 bits on VT-d) and
zeros. The first capability carries Inspect, Map, Transfer, Duplicate, Derive and Manage. The domain uses one of the
platform's backends, four on AMD64 behind VT-d. A kernel without a usable IOMMU  answers
`InvalidConfiguration`. ObjectDestroy first turns off the bus mastering of every function attached to the domain, then
quiesces it.

IommuControl names the IommuDomain in arguments 0 and 1 and the operation in argument 2:
- **Attach and Detach** need `Manage` on the domain and a PciFunction in arguments 3 and 4 with `Invoke`, whose assignee
  is the caller's ProtectionDomain; argument 5 is zero. Bus mastering is turned off across either. A function already
  attached elsewhere, or not to this domain, is `InvalidArguments`. A function the unit cannot confine (one the DMAR does
  not name) is `InvalidConfiguration`. Only an attached function, whose domain is Ready, may become a bus master.
- **Map** needs `Map` on the domain. Argument 3 addresses a 48-byte block and argument 4 is 48:
  - a Frame capability with `Map` (16 bytes);
  - the Frame page the mapping starts at;
  - the page count;
  - the page-aligned device address;
  - a permission byte, 1 device-read and 2 device-write, at least one;
  - zeros.

  The device then reaches those pages at that address and nothing else. A device-writable mapping takes part in the
  Frame's write-versus-execute policy. A range outside the domain's width, an overlapping one or a non-RAM Frame is
  `InvalidArguments`; running out of mappings or table pages is `ResourceExhausted`.
- **Unmap** needs `Map`; argument 3 is the device address a mapping starts at. The unmap returns once no DMA can use the
  removed translation.
- **Faults** needs `Inspect`. Value 0 holds the faults the unit recorded against the domain's functions in bits 0-31,
  the last one's requester in bits 32-47, and its reason in bits 48-55.

## Errors and canonical results

| Value | Error | Value | Error |
|---:|---|---:|---|
| 0 | None | 9 | RightsAmplification |
| 1 | UnknownSystemCall | 10 | NoAttenuation |
| 2 | InvalidArguments | 11 | TypeMismatch |
| 3 | InvalidConfiguration | 12 | Revoked |
| 4 | InvalidHandle | 13 | Expired |
| 5 | ObjectNotFound | 14 | LeaseExtension |
| 6 | ResourceExhausted | 15 | RevocationDomainExhausted |
| 7 | SpaceExhausted | 16 | WouldBlock |
| 8 | RightsDenied | 17 | UserMemoryFault |
| | | 18 | TimedOut |

Every user-visible error response has both value words zero. Unknown numbers
return `UnknownSystemCall` without interpreting arguments. Each known operation
validates all reserved and operation-specific fields before its first mutation.
`WouldBlock` reports retryable Endpoint pressure or an empty receive queue;
`UserMemoryFault` reports a contained access or copy fault.

Malformed internal Endpoint reservation state, a user-memory backend protocol
violation, or an ingress identity mismatch is not converted into an ordinary
user error. It uses the separate kernel failure channel and leaves the syscall
response unchanged. Dispatch allocates no memory and retains no object lease
after return.

## Synchronization

Ingress and dispatch may spin on the ProtectionDomain and shared
object/capability/revocation locks. Endpoint IPC also relies on exclusive
processor-local user-copy and scratch state. WaitSet suspension additionally
retains one processor-local continuation and excludes a second resumable syscall
until exact completion. These paths are not valid in an ISR or a same-CPU
reentrant path. Concurrent Query and Revoke calls
have one lock-defined order: Query either returns the complete pre-revocation
snapshot or returns `Revoked`, and every operation ordered after Revoke observes
the new epoch. An operation is ordered where it takes effect. IPC send and
receive take effect under the shared lock and authorize their handle there
again, however long ago they looked it up. DebugWrite output and a WaitSet
wait take effect outside that lock and are ordered by their lookup: a Revoke
after the lookup does not stop an operation it already authorized.
