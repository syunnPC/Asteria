<!-- SPDX-License-Identifier: Apache-2.0 -->

# Capability Model

## Handle Encoding

A user-visible capability handle is 128 bits.
The first 64-bit word is a selector containing a 56-bit nonzero slot generation followed by an 8-bit slot index. The
second word is a 64-bit SipHash-2-4 authenticator. Rights never reside in the handle and never depend on the handle
being secret.

The authenticated message consists of these three independent little-endian 64-bit fields, in order:

1. selector;
2. boot-unique, nonreused capability-space identifier;
3. the numeric domain word `0x3150414352545341`, whose little-endian bytes are `ASTRCAP1`.

The 24-byte message is followed by SipHash's eight-byte final block, whose most significant byte encodes the message
length 24 and whose other bytes are zero. A capability space receives a 128-bit validation key derived from boot
entropy (see [Validation Keys](#validation-keys)). The raw 64-bit authenticator is preserved, including zero; selector
zero alone denotes an invalid handle.

The boot-global allocator serializes construction and issues each nonzero capability-space identifier at most once.
It fails closed after exhausting all 64-bit identifiers. The explicit slot generation makes same-space stale-handle
rejection deterministic: closing a slot advances its generation, and the slot is permanently retired instead of
wrapping. Authenticating the separate selector and space-identity fields prevents the algebraic cross-space collision
that a combined XOR encoding would permit.

Each revocation registry likewise receives a nonzero, boot-unique 32-bit identifier. A 64-bit
`RevocationDomainId` combines that registry provenance with the existing 16-bit generation and 16-bit slot. Every
lookup rejects an identifier whose provenance does not match the receiving registry, so identically positioned
domains in independent registries cannot be confused. Registry construction fails closed after exhausting the
nonreused identifier space.

A registry holds at most 64 live domains. Its trusted owner ends a domain with `Destroy`, which also ends every
descendant, since descendants inherit revocation. A destroyed identifier is unknown from then on: creating a child
under it or minting into it fails, and every capability minted in the subtree reports `Revoked` at use and stays
closeable. The freed slot is reused under the next 16-bit generation, so a stale identifier never names its
successor; a slot whose generation is exhausted retires instead of wrapping, and no identifier is issued twice.
Capabilities do not keep a domain alive: its owner holds the identifier, and only the owner knows when it may mint
again, so there is no reclamation when a domain's last capability closes.

Derive with the child-domain flag is the user-visible way to create a domain: the derived handle becomes the first
capability of a new child of its source's domain, so it may carry `Revoke` without being able to revoke its source
([System Call ABI](SystemCallAbi.md#capability-semantics)). Such a child has no owner that could `Destroy` it:
revoking it, or its last capability closing, leaves its registry slot occupied until an ancestor is destroyed. Each
child-domain Derive therefore consumes one of the registry's 64 domains until an ancestor is destroyed
([threat model](#threat-model)).

A newly created object's first capability is minted under an authority instead
([object management ABI](SystemCallAbi.md)). `RootCapabilityIssuer::MintUnderAuthority` authorizes a
capability in the destination space as `Lookup` would (right, type, and exact object) and inserts the new capability in
the same critical section, in that capability's revocation domain and epoch, with its lease deadline and a zero sealed
selector. A Revoke, Close, or expiry of the authority ordered before the mint fails it; one ordered after ends both.
Creation therefore consumes no revocation domain.

Handle validation computes the authenticator before consulting occupied slot metadata and returns a uniform
`InvalidHandle` for malformed, forged, closed, and stale handles. A valid authenticator grants no authority by itself:
lookup additionally verifies slot generation, object identity and type, rights, revocation lineage, and deadline.

## Code-producing authority

`Code` is bit 13 (`0x2000`) of the sixteen-bit rights field; bits 14-15 remain reserved.
A ResourceDomain capability needs both Manage and Code to create a MemoryObject whose
maximum permissions include Execute; without Code, ObjectCreate reports `RightsDenied`.
Derive, Duplicate, grants and IPC attenuation follow the same subset rule for Code as
for every other right. The boot pager grant carries Code on both ISAs; a created child
domain's first capability and the component's Manage|Inspect account copy do not.
Executable MemoryObjects require the loader's Code-authorized grant. Shared sealed
segments are charged once and retained across component incarnations; writable
Frames and other component objects remain charged to their child domain.
See [shared executable segments](Elf64Image.md).

## Validation Keys

Every key comes from this boot's entropy; no key is a source constant.

1. Before `ExitBootServices`, the UEFI application (`BootEntropyGathering`) mixes, in order: two 32-byte
   `EFI_RNG_PROTOCOL` blocks (the firmware's default algorithm) when the firmware offers the protocol; eight 64-bit
   `RDSEED` values, else `RDRAND` values, where CPUID reports the instruction (AMD64 only); the firmware time of day
   (`GetTime`) and `GetNextMonotonicCount`; and 2,049 readings of the processor's cycle counter (TSC or ITC), each taken
   after the pool update of the previous one.
2. `BootEntropyCollector` frames each sample (source, length, bytes) into HKDF-SHA256 Extract (RFC 5869) with the salt
   `Asteria boot entropy v1`; the 32-byte pseudorandom key is the boot seed. Health tests decide which sources are
   credited: a generator fails on a block shorter than eight bytes, a block of one repeated byte, or a block that starts
   with the previous block's first eight bytes; the jitter source fails on a zero delta, 32 equal consecutive deltas
   (the repetition count test of NIST SP 800-90B 4.4.1), or 384 deltas equal to the first of a 512-delta window (its
   adaptive proportion test), and is credited only after 1,024 deltas. Firmware state is mixed but never credited. A
   generator is credited after 32 healthy bytes.
3. Without a credited source the application reports `ASTERIA:BOOT-ENTROPY:FAIL` and never exits boot services; the
   boot stops rather than run with a predictable key. Otherwise it reports the credited sources as
   `ASTERIA:BOOT-ENTROPY:SOURCES:0x<mask>` and `BootTransition` copies the record into `BootInfo::Entropy` of every
   final-map candidate before the AMD64 runtime preparation runs.
4. The kernel derives the key of each capability space with `DeriveCapabilityValidationKey`: the first 16 bytes of
   HKDF-SHA256 Expand with the seed as pseudorandom key and the info `Asteria capability validation key v1` followed by
   the space's purpose as a little-endian 32-bit value. Each space of a boot has its own purpose, so no two spaces share
   a key: `BootstrapSpace` for the boot runtime's space (AMD64 `BootRuntime`, IA-64 `Ia64BspKernelRuntime`),
   `DeviceSpace` for the PCI/ECAM runtime's space, `PagerSpace` for the AMD64 boot pager's space and
   `FaultTokenSpace` for the kernel-only space in which its pager channel creates FaultTokens, and `SpinnerSpace` for
   the space of the AMD64 test firmware's spinner. A record without a credited source, with an unknown source bit or a
   nonzero reserved field, or with a seed of one repeated byte (for example the all-zero record of a reset `BootInfo`)
   yields no key, and the boot stops with a `CAPABILITY-KEY` failure diagnostic instead of constructing the space.

Object and revocation registries carry no key: their provenance identifiers are boot-unique counters that need no
secrecy. The seed stays in kernel memory (`BootInfo`), which user mappings never include, and is never written to a
diagnostic or persisted. Stack copies left by the extraction and derivation are not erased; the boot-services memory
that holds the firmware stack is never reclaimed as allocatable RAM.

## Boundary Representation

The syscall and IPC representation is exactly 16 serialized bytes:

| Offset | Size | Encoding | Field |
|---:|---:|---|---|
| 0 | 8 | unsigned little-endian | selector |
| 8 | 8 | unsigned little-endian | authenticator |

Serialization reads and writes each field explicitly. It never overlays external bytes with the C++ class, relies on
padding, or exposes the C++ ABI. Architecture syscall stubs pass or reconstruct two `UInt64` values rather than passing
`CapabilityHandle` by C++ object ABI. The internal type is 16 bytes with eight-byte alignment on IA-64 and AMD64, as
enforced by `Tests/Target/AbiContractTests.cpp`.

## Threat Model

The 128-bit representation provides only a 64-bit authentication tag, so it does not claim 128-bit forgery strength.
For a secret, independently generated key, one online guess succeeds with probability approximately 2^-64. Selector
values and failure timing are not secrets. The nucleus implements no per-principal attempt accounting, rate limits,
security telemetry, or response to sustained invalid-handle trials.

A Derive into a child revocation domain consumes one of the registry's 64 domains. Its slot remains occupied until
an ancestor is destroyed, and it is not charged to the caller's ResourceDomain. A holder of Derive can therefore
exhaust the registry ([Handle Encoding](#handle-encoding)).

Validation keys are generated once per boot, never exposed to userspace, never persisted, and never reused across
boots. SipHash protects handle integrity against guessing; it is not used as an encryption or general-purpose hashing
primitive.

The keys are only as unpredictable as the boot seed, and the sources differ in what they can promise:

- `EFI_RNG_PROTOCOL` and `RDSEED`/`RDRAND` are trusted as the platform's random-number generators. The health tests
  catch a stuck or constant generator, not a biased or backdoored one. On AMD64 the VM profile provides a virtio-rng
  device, which OVMF exposes as `EFI_RNG_PROTOCOL`.
- Cycle-counter jitter is the only credited source where neither exists, which includes the IA-64 firmware and its QEMU
  model. Its entropy is not measured: the health tests reject a counter that stops or repeats itself, but a regular
  deterministic machine (an in-order processor, or an emulator replaying recorded input or counting instructions) can
  pass them while producing the same seed on every boot. The fallback therefore only raises the cost of predicting
  the seed; it guarantees nothing against an attacker who can reproduce the machine's timing. A boot whose jitter also
  fails the tests stops.
- The time of day and the monotonic count are mixed but never credited: an attacker who knows the boot time predicts
  them.

A deployment that admits untrusted userspace therefore requires a credited hardware or firmware generator, which the
source mask of each boot (`ASTERIA:BOOT-ENTROPY:SOURCES`) makes checkable. The VM runner checks that every boot
credits a source, that AMD64 boots credit the firmware generator, and that boots of the same image mint different
authenticators for their bootstrap handles; it never judges a particular value.

## Object Lifetime

`ObjectRegistry` stores an externally owned `IKernelObject` instance. The owner must keep that instance alive and
immovable from successful registration until `CollectDestroyedObject()` returns it. Both dispatch-scoped capability
leases and persistent kernel object pins use the same atomic in-flight count. Successful capability lookup returns a
move-only lease containing the actual object pointer and the authorized descriptor snapshot. `PinObject()` instead
returns a move-only generation-safe reference for an already-authorized kernel relationship; a pin grants no rights.

An occupied entry, alive or quiescing, may also hold one exclusive claim on a half-open interval of a type-defined
resource. `ClaimExclusiveRange()` refuses, with `ExclusiveRangeConflict` and without any change, an interval that
overlaps a claim held by another entry of the same type; `ReleaseExclusiveRange()` or collection ends the claim. The
check uses an iterative AVL tree keyed by object type and range start under the registry lock.
Its links and height fit in the 64-byte entry; a 24-link path bounds updates at the 16,384-entry capacity.
Disjoint claims of one type let the overlap check inspect only the predecessor of the requested end.
The entries live in a platform-attached slab; two-level bitmaps index free and quiescing entries,
and identities directly index slots. Frames claim their physical byte ranges this way while
they hold mapping reservations (see `Frame.md`); claims grant no authority.

The implemented lifetime states are:

```text
Alive -> Quiescing -> Collectable -> ReturnedToOwner
```

`DestroyObject()` performs only the transition to `Quiescing`: it rejects new lookups and removes the object from the
active count but does not free storage or reuse the slot. Existing leases and pins remain valid. Their destructors and
explicit pin resets execute one wait-free atomic release and never acquire the registry lock.
`CollectDestroyedObject()` returns a quiescing object only after an acquire load observes zero in-flight references;
it then advances or retires the generation and makes the slot reusable. The owner may destroy or reuse backing
storage only after receiving that record. `CollectDestroyedObject(object)` collects only that exact generation:
`InvalidObject` if it is not quiescing, `NoDestructionReady` while it is still referenced. Every owner of destroyable
storage (`PagerChannel` for its FaultTokens, the object pools of
[object management ABI](SystemCallAbi.md)) collects its own objects this way, so no owner can receive another
owner's record; the untyped form remains for tests. A valid destruction record can be constructed only by `ObjectRegistry`
and retains that registry's exact provenance; other code can copy an authentic receipt or create an invalid empty
value, but cannot forge an early or cross-registry reclamation.
The originating registry must outlive every copy and use of its receipt, so
registry-address reuse cannot impersonate that provenance.

Interrupt or lock-free readers outside this lease discipline require an
additional epoch/RCU reclamation mechanism before they can access these objects.

Persistent pins are kernel-only lifetime references, not capability tokens. Creating one requires a valid nonzero
`ObjectId`, an exact expected type, an empty caller-owned output, and separate authorization by the subsystem that is
installing the relationship. Pin acquisition and destruction linearize under the registry lock: once destruction
enters `Quiescing`, every new pin attempt fails, while an earlier pin keeps the exact object generation alive. Move
construction transfers one reference; resetting or destroying a pin releases it. Move assignment is intentionally
disabled: a pin may be embedded in an object whose own lifetime another pin is holding, so replacement must be an
explicit reset followed by acquisition into empty storage. `SchedulingContext` uses this mechanism to retain every
bound or donated production `Thread` generation. `FaultToken` likewise retains the exact faulting `Thread` and
`AddressSpace` generations until the external-pager transaction has reached a terminal resolution.
Physical memory is represented by immutable, registry-generation-bound `Frame` objects; holding a `FrameId` alone
does not grant mapping authority. `FaultFrameMappingPreparation` is invoked only after capability authorization and
retains separate typed `MemoryObject` and `Frame` pins while the target AddressSpace mapper consumes its validated
mapping plan. A device Frame additionally requires an exact typed `DeviceRegion` pin. The MemoryObject supplies
logical range, maximum-protection, cache, commitment, and ResourceDomain policy; the Frame supplies physical
ownership, exclusive among the mapped Frames of its registry; the DeviceRegion supplies discovered MMIO range and
register-access authority.
The fixed-capacity `DeviceManager` is the current platform-discovery issuance
boundary for MMIO, port-I/O, and PCI-function authorities. It prevents
same-kind range overlap and duplicate live/quiescing BDFs, mints only the
descriptor-derived initial rights, and withholds range and slot reuse until the
registry collector returns the exact retired generation. PCI creation returns
only a registry-qualified `PciFunctionId` and exact `Inspect|Manage|Transfer`
capability; the embedded storage address is never exposed.
`IommuDomain` adds a separately capability-authorized DMA address space: it
pins exact Frame generations, participates in the Frame-wide
write-versus-execute ledger, and claims unique platform requester endpoints
before attachment. Native IOMMU backends remain behind the HAL boundary, so no
architecture encoding enters capability logic.
`PciFunction` is the generation-safe requester endpoint authority. It pins one
already-authorized, Ready `ProtectionDomain` generation and retains an internal
device-use reference that excludes domain quiescence. It permits an IOMMU claim
only while that assignment exists and requires exact IOMMU release before
dropping the driver use and pin. The object itself writes no PCI register and
grants no authority merely by holding its internal assignment token.

The originating `ObjectRegistry` and its synchronization object must outlive every pin and move-constructed
successor. Pin destruction is deliberately a direct wait-free release into that registry; it does not add another
indirection or dynamically managed registry lifetime.

Registry construction may set a real per-object reference capacity shared by
leases and pins. Acquisition returns `ReferenceExhausted` when the current
count reaches that ceiling, including when the configured ceiling is zero.
The production default is the full `UInt32` saturation limit. A smaller deployment policy can
bound references more tightly; hosted tests also use that same supported
policy to drive deterministic exhaustion and recovery paths without a
test-only failure hook.

The registry cannot infer which object owns the storage containing a pin, so it cannot detect strong-reference
cycles. Kernel relationship installation must preserve the ownership DAG and reset outbound pins during
explicit teardown before those pins would prevent every member of a cycle from becoming collectable. Reverse and
observational links use generation-bearing IDs rather than persistent pins.

## Authorization Linearization

A lease is valid for one kernel syscall or IPC dispatch only. Lookup linearizes under the shared registry lock and
captures the type, rights, revocation epoch, deadline, sealed selector, and charge owner that authorized that dispatch.
Closing the capability, revoking its domain, reaching its deadline, or beginning object destruction prevents every
later lookup. It does not asynchronously cancel an operation that already acquired a lease. Kernel callers must not
cache a lease or its descriptor across dispatches; long-running operations require a separate cancellation protocol.

An operation whose effect is published later under the shared lock does not rely on its lease for authority: it
authorizes the handle again in that critical section, so it is ordered by its effect and observes every `Close`,
`Revoke`, expiry, or destruction that precedes the effect. Endpoint send and receive through a capability do this
(`EndpointQueue.md`), including a synchronous Call published after its caller blocked. Operations whose effect lies
outside the lock (DebugWrite output, a WaitSet wait, a server's acceptance of a Call) are ordered by their lookup.

A capability moved by a prepared Endpoint send is held until the send is resolved: it keeps its slot and handle but
no operation accepts the handle (`InvalidHandle`), and it is either removed when the message is published or
restored unchanged when the preparation is cancelled (`IpcCapabilityTransfer.md`).

The initial architecture-neutral syscall dispatcher exposes only capability Close, Query, Derive, Duplicate, and
Revoke. Query requires `Inspect` and returns a checked immutable descriptor snapshot internally; its external result
is narrowed to stable type, rights, and deadline values. Revoke requires `Revoke` and performs authorization plus the
domain epoch transition under the same shared lock. Consequently a racing Query either returns its complete snapshot
before the transition or observes `Revoked`, and every later domain use is rejected. Revoked and expired handles stay
closeable so explicit cleanup does not depend on authority that has already ended. The logical register contract and
fixed error registry are specified in `Documentation/Architecture/SystemCallAbi.md`.

## Synchronization Contract

The object registry, revocation registry, and capability spaces bound to them share one spin lock. Their public
operations are allocation-free and bounded by fixed capacities, but can spin and therefore are forbidden from ISR or
same-CPU reentrant use. The boot-global capability-space and revocation-registry identifier locks are used only during
construction and are never nested with the registry lock or each other. Lease destruction and pin reset take no lock.
Callers may take the registry lock inside their own locks (an `AddressSpace` or `IommuDomain` pins objects, and a
`Frame` claims or releases its pages); the registry never calls back into objects while holding it.

The injected monotonic clock is called while the registry lock is held and must therefore be bounded, nonblocking,
allocation-free, callback-free, lock-free, safe with interrupts disabled, and monotonically nondecreasing. The object
metadata accessors are called before publication and have the same bounded, nonblocking, allocation-free contract.
