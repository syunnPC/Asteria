<!-- SPDX-License-Identifier: Apache-2.0 -->

# Address-space ownership and shootdown

`AddressSpace` is the architecture-neutral owner of one native page-table
backend. Initialization imports a sealed bootstrap plan and claims the backend
for exactly one `AddressSpace`; a native root cannot be shared by two owners.
The platform memory-manager factory supplies one unique preparation and arena
per root. On AMD64, runtime roots use the canonical verified bootstrap profile.
The current IA-64 canonical seed is narrower: it covers only the BSP image,
memory stack, RSE, IVT, and translation storage. Its builder admits only
identity mappings wholly in region 0 and marks every mapping `Global`; RR0
alone is shared at RID `0x2000`, and
preparation proves locked DTR coverage for every seed mapping plus locked ITR
coverage for executable chunks. The BSP runtime-memory owner imports that
exact seed into one common bootstrap/idle `AddressSpace`, claims its embedded
IA-64 runtime backend, registers and binds the common object, and activates it
for the BSP through `ProcessorAddressSpaceController`. This is still a single
boot-integrated bootstrap/idle root, not the general runnable-root contract.
Seed-owned physical pages remain reserved and are never issued as `Frame`
objects.

Runtime mappings have two preparation paths. Real fault resolution uses an
exclusive `FaultToken` claim through `FaultFrameMappingPreparation`;
`FaultToken` is reserved exclusively for that purpose. Proactive pinned-RAM
work uses `PinnedFrameMappingPreparation` and never creates or claims a
`FaultToken`. This second seam supports supervisor and user mappings, grants no
authority, and therefore requires trusted kernel integration or a caller
already capability-authorized for the exact target root, owners, ranges, and
permissions. See `PinnedFrameMapping.md` for its portable contract.

Both paths retain the exact logical `MemoryObject` and physical `Frame`
generations. Device Frames additionally require and retain the exact
`DeviceRegion` generation; ordinary RAM rejects one. The MemoryObject keeps
its resource commitment live and the DeviceRegion keeps its MMIO authority
live until the AddressSpace releases the mapping at quiescence. Each `Frame`
carries a bounded physical-interval policy ledger shared by every
`AddressSpace` and `IommuDomain`. A Frame that holds any reservation claims
its whole range in the `ObjectRegistry`, and a mapping through another Frame
of that registry over claimed pages fails with `PhysicalOwnershipConflict`, so
the claiming Frame's ledger is the ledger of those pages: overlapping writable
and executable aliases cannot be created through different roots, different
Frames, or DMA. Disjoint subranges of one Frame remain independently usable.
The per-root ledger also rejects virtual overlap and physical ownership
conflicts (a seed page, another Frame, or another DeviceRegion). Overlapping
records of one root therefore name one Frame, and both preparations take the
cache policy from that Frame, so a physical page has one cache policy in every
root and there is no separate cache-alias error. See `Frame.md` for claims and
for the reservation lifecycle.

Native installation precedes publication. A successful backend call creates a
provisional record and returns the exact invalidation range. The record becomes
visible only after every processor in the captured resident mask acknowledges
its local invalidation. `TlbShootdownDispatcher` performs the current CPU's
invalidation directly and publishes the remaining opaque work as one
all-or-none batch. A remote handler performs only the bounded local operation
and a lock-free acknowledgement or rejection. The initiating thread waits for
all target outcomes and is the only context that completes the mapping or
makes the owner terminal. Each CPU's first acknowledgement or rejection is its
final outcome, so a late duplicate cannot race completion and rewrite success
as failure (or failure as success). A local or transport failure publishes no incomplete
batch and makes the owner terminal while retaining the native mapping, all
object pins, and policy reservations until all CPUs leave the root and `Quiesce`
retires it. A successfully published transport batch keeps the AddressSpace
and transaction alive until every remote handler has returned. Dequeueing only
transfers a non-owning work copy to the target CPU;
the AddressSpace remains alive until that handler has returned after its
acknowledgement or rejection. Shutdown therefore stops new publication and
asynchronous delivery, synchronously handles every still-published target,
joins all CPU-local drain handlers, and verifies every slot empty before
quiescence. Empty slots without the handler join are not a reclamation
condition.

A backend normally leaves native state and output unchanged on failure. The
explicit `CommittedStateUnknown` result is different: a native write may have
occurred but its complete invalidation result is unavailable. `AddressSpace`
therefore retains the hidden provisional record and all resources, enters
`Faulted`, and requires root quiescence. An unknown error code, and a success
whose invalidation range differs from the request or whose reserved field is
nonzero, are treated the same way and reported as `PageTableProtocolViolation`.
Corrupt backend state is also terminal, but its no-write preflight failure
releases only the provisional resources. Every ordinary rejection releases the
provisional W^X reservation with the pins; every retained one stays until
`Quiesce` and meanwhile refuses the opposite access to those pages, and any
overlapping Frame, through every other root and IOMMU domain. Without unmap or
protect, published mappings accumulate their reservations in the same way.

Processor entry is one integrated `ActivateProcessor` transaction. Under the
AddressSpace lock it validates the CPU and state, excludes mapping and
quiescence with `ActivationPending`, and invokes the claimed backend's
`Activate(owner, CpuId)`. The backend operation is bounded, allocation-free,
and callback-free. It completes the native root switch and required local
translation invalidation before returning success. Only then does the
AddressSpace add the CPU to its resident mask and restore `Ready`; callers
cannot publish residency separately from the native commit.

An SMP root change first acquires an `AddressSpaceTransitionReceipt`. The
receipt binds the exact `AddressSpace` storage, a monotonically consumed
55-bit sequence, and one of the four valid CPU identifiers in 16 bytes. Only
the current receipt can perform reserved activation or detach and release the
reservation. A bare activation or detach returns
`ProcessorTransitionPending` while any reservation exists. This prevents
unrelated same-CPU code from bypassing the receipt.

The reservation is independent of `AddressSpaceState`, so a committed-unknown
activation can retain it while the root becomes `Faulted`. `BeginFaultMapping`
checks the reservation under the AddressSpace lock before validating or
retaining any mapping resource and returns the explicitly retryable
`ProcessorTransitionPending` result without calling the backend. Conversely,
reservation acquisition rejects `ActivationPending` or `ShootdownPending` and
never waits. Thus mapping and root transition have one atomic winner, and a
mapping cannot start a shootdown that waits on a CPU fail-stopped inside its
root switch.

One root holds at most one transition reservation. Controllers acquire source
and destination through separate bounded lock acquisitions and release the
source if the destination is already reserved. Opposing CPU switches may both
reject their second acquisition, but neither waits while retaining two locks.
After fail-stop has stopped the CPU, the retained receipt authorizes exact
conservative detach. Quiescence with no resident CPU retires any remaining
reservation together with the backend; detach and release with the stale
receipt then return `InvalidProcessorTransitionReceipt`. A root that is both
resident and reserved instead rejects quiescence with
`ProcessorTransitionPending`, preserving the exact teardown authority.

Backend activation failures have an explicit commit boundary. Every error
except `CommittedStateUnknown` proves that the previous root is still active,
so `ActivateProcessor` restores `Ready` and reports
`ProcessorMayBeResident == false`. The activation sequence is still consumed,
preventing a rejected attempt from being confused with a later operation.
`CommittedStateUnknown` and malformed backend results instead make the owner
`Faulted`, add the CPU to the conservative resident mask, and report
`ProcessorMayBeResident == true`. Detach remains permitted from `Faulted`, but
the root cannot be reused. A duplicate activation of an already-resident CPU
is rejected before calling the backend.

The caller identifies the executing CPU, disables local interrupts, and
excludes same-CPU re-entry for activation. The externally owned backend and
its claimed native-root storage outlive the AddressSpace through `Quiesce`.

`ProcessorAddressSpaceController` composes these transactions across roots. It
reserves both roots before activating the destination and detaches the source
afterward, so shootdown accounting temporarily over-approximates residency and
never omits a CPU that may use the root. It skips only the exact already-active
`AddressSpace` object after one bounded source reservation and validation. See
`ProcessorAddressSpaceController.md` for scheduler rollback and fail-stop
semantics. Its snapshot retains every saved receipt still known to authorize a
current reservation, while `Previous` remains through source detach only while
the source receipt is authoritative. After any release attempt, `None` means
completion and `InvalidProcessorTransitionReceipt` proves that saved receipt
is stale; either outcome removes it from the snapshot even if a later cleanup
step fails. The boot-integrated IA-64 BSP owner initializes its one
bootstrap/idle root, and the later translated path admits and activates that
same root on each enabled AP. This remains a single shared-root transaction;
each AP later borrows its exact admitted controller for its boot-lifetime
cooperative run-loop service without introducing another root.

The AMD64 backend extends an exclusively owned, verifier-accepted four-level
tree and the local invalidator issues one `invlpg` for each page in a bounded
range. The IA-64 local invalidator retains the owning processor's PAL-decoded
implemented virtual-address width, issues one 4 KiB `ptc.l` per page, and ends
the bounded sequence with instruction serialization, which also serializes
data. It never purges locked translation-register entries. The
architecture-neutral mailbox transport retains eight complete batches. Each
batch has one atomic publication word, so concurrent producers either publish
every target or publish none; CPU-local consumers acquire-copy their work and
clear only their own completion bit. AMD64 provides a no-fail x2APIC
notifier that maps the four kernel CPU identifiers to distinct physical
x2APIC destinations and emits one fixed edge-triggered IPI per selected CPU.
The AMD64 interrupt binding reads the accepting CPU's x2APIC identifier,
selects its immutable processor-local drain, processes one bounded mailbox
pass, and writes EOI exactly once. Its entry preserves every enabled general
register and returns with `IRETQ`. Installing the reserved vector into each
processor's IDT remains separate integration work. IA-64 runtime translation
encoding and the append-only software-refill backend mapping path are
implemented and focused-tested; successful `Map` operations append an encoded
ledger record and return the exact invalidation range. The IA-64 BSP owns
one worker memory-stack/RSE context and composes it with the admitted root and
processor run loop. The translated AP path installs and admits one per-AP IVT
context, activates the shared root on that processor, proves one exact-sequence-
one local-timer interrupt, then borrows that exact admitted root/controller for
one fixed-storage kernel-only `Ia64ProcessorRunLoop` service. The retained worker
blocks after run sequence one, and the AP polls a reusable acquire/release BSP-
to-AP mailbox with `PSR.ic=1` and `PSR.i=0`. The owning AP acquire-observes one
external request and uses the retained worker's exact block token to wake and
readmit the same native context at sequence two. The worker blocks again and
the AP remains `IdleServing` with the same root. After stable BSP
acknowledgement of timer sequence one, a separate one-shot request has that idle
context drain one AP TimerQueue-to-Notification delivery at exact `0xe8`
sequence two and restore the timer graph to empty/disarmed without running the
blocked worker.
The post-exit BSP owner supplies one registered common bootstrap/idle root
and one distinct registered private initial-component root. It activates the
common root, verifies both IA-64 ledger bindings, and leaves the private root
unresident. The separate `InitialTranslationActivator` then enables
translation; the translated kernel continuation borrows both roots, prepares
the BSP runtime, installs the CPL3 mappings and direct-user-memory authority in
the private root, and restores the common root before proving the first direct
local-timer delivery. Direct user-evidence writes use bounded private selection
and common restoration. The common processor run loop derives the private root
from the worker ProtectionDomain for launch, returns to the common idle root on
Wait, and selects private again before readmission and retained syscall resume.
A second timer sequence drives deferred delivery and WaitSet wake. Persistent AP
cooperative serving and its focused timer delivery are connected through
separate polled requests. IPI notification, interrupt preemption, timer-driven
AP scheduling, periodic policy, a general timer service, migration, load
balancing, remote shootdown delivery, hotplug, teardown, and general runtime
mapping are unsupported by this boot integration.

desktop session: ObjectCreate type 2 accepts byte 0 bit 0 in its 32-byte
request; all other bits and bytes are reserved. The flag requires one live
large-page-table-arena unit in the charging ResourceDomain and every ancestor.
AMD64 selects a free 256-page arena instead of the slot's 64-page storage;
IA-64 accepts the flag and uses its ordinary backend. Destruction retains the
unit and metadata charge until residency and pins are gone and collection
quiesces the space. Exhaustion creates nothing and leaves the slot reusable.
