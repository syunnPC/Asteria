<!-- SPDX-License-Identifier: Apache-2.0 -->

# IA-64 runtime AddressSpace page-table backend

`Ia64RuntimeAddressSpacePageTable` is a fixed-storage adapter for the common
`IAddressSpacePageTable` contract. Its lifecycle is
`Uninitialized -> Ready -> Claimed`; a committed uncertainty moves it to
`Faulted`, and release permanently moves `Claimed` or `Faulted` to `Retired`.
Retired private RIDs are never reused. The public surface is only `Initialize`
plus `IsReady`, the bounded `IsActiveLedgerBound` postcondition observer,
`Claim`, `Release`, `Activate`, and `Map`.

Initialization takes the common intersection of every admitted processor's
runtime translation capabilities. It normalizes that intersection into the
ledger encoder profile, rather than copying a stronger per-CPU record.
`Ia64BspRuntimeMemory` freezes the enabled-processor mask and the common
capability intersection for that multi-CPU profile and owns the one
authoritative RID allocator. It allocates one complete receipt for the common
root and the next complete receipt for the fixed private initial-component
root. Each receipt initializes exactly one embedded backend; receipt-copy
uniqueness cannot be detected locally.

The exact sealed canonical preparation plan is retained only as a borrowed
pending seed identity. `Claim` accepts that same plan object and its exact
Region-0 owner, then drops the seed pointer. Seed mappings are not appended to
the runtime ledger because they are shared-Global Region-0 mappings and the
runtime encoder reserves Region 0. The backend, preparation, plan, bindings,
ledger, and later owner must have complete numerical ranges in Region 0 and
must obey their documented lifetimes and target-stack prohibition. Numerical
placement is necessary; the page-aligned BSP owner derives its exact object
corridor and retains the preparation as the pinned shared-Global coverage
authority.

`Map` accepts nonoverlapping private-region mappings. It publishes
the invalidation range only after an exact ledger append and otherwise leaves
the output unchanged. Capacity, overlap, invalid-plan, and unsupported-
configuration failures are translated into the common interface; inconsistent
or impossible ledger results fault the backend. The backend accepts a valid
single top page in RR7 and rejects a wrapping two-page range. The common
`AddressSpace` exclusive-end limitation still prevents claiming general
top-page reachability through that higher layer.

`Unmap` tombstones the ledger record `Map` appended for the mapping and returns
the same invalidation range; the owning `AddressSpace` purges it on every
resident processor before releasing the mapping's objects. After shootdown completes, later maps may reuse the tombstoned slot. The
256-record limit bounds simultaneous mappings ([ledger](Ia64RuntimeTranslationLedger.md)). A mapping the ledger does not hold is `CorruptState` and faults the
backend.

`Activate` validates the exact owner, CPU, and retained common-profile mask,
then calls `AcquireActiveLedger` as a nonmutating admission query before
`ActivateAndBind`. This backend never calls `AdmitProcessor`. A production-zero
binding table returns `UnsupportedConfiguration` and makes no native call.
Precommit rejection preserves the claimed backend and prior binding. A
committed uncertainty faults the backend and leaves the slot at admitted with
no published ledger; conservative residency and both possible root lifetimes
remain retained until fail-stop resolution and a proven native switch-away.
Private RIDs are monotonic and never reused, so no other space's translation
can match this one's. A stale translation of this space
itself can: writing the region registers keeps every translation-cache entry
of other RIDs, so a translation `Unmap` removed while the space was not
resident on a processor can still be cached there and matches again once the
space's RIDs are back in the region registers. The owning `AddressSpace`
purges only resident processors. So every successful `Unmap` advances the
backend's unmap epoch, and `Activate` on a processor where the space ran
before, and whose recorded epoch is older, purges that processor's whole local
translation cache once the region registers and ledger are this space's (a
refill during the purge inserts only what the ledger still holds), then
records the current epoch. The purge is an `Ia64LocalTranslationCachePurge`
(`ConfigureTranslationCachePurge`, once, while `Ready` or `Claimed`), which the
BSP runtime memory names for every root it owns: a `ptc.e` loop over the BSP's
PAL_PTCE_INFO operands followed by `srlz.d` and `srlz.i`, called directly, since
the native-preemption corridor admits no indirect call. An activation that needs a purge on a processor with none is
`UnsupportedConfiguration` before any native call. An unmap the resident
shootdown already covered costs one redundant purge. A retired space needs
none, since its RIDs never return. `GetState` reports the backend's state.

`IsActiveLedgerBound` holds the backend lock, requires the backend to remain
`Claimed`, acquire-loads the selected processor's binding, and returns true
only when it is the exact address of this backend's private ledger. It is a
postcondition check for the BSP owner; it proves neither refill-reader
quiescence nor reclamation.

All methods serialize on the backend lock, ordered after the owning
`AddressSpace` lock. Same-CPU activation and binding clear remain externally
serialized with interrupts, preemption, migration, and re-entry excluded.
Release requires an exact owner plus proven native switch-away, zero
`AddressSpace` residency, resolved uncertainty, and refill-reader quiescence.
`IsBound` is only an additional post-quiescence guard and never lifetime or
reclamation proof; release never clears a binding.

The [BSP runtime-memory owner](Ia64BspRuntimeMemory.md) retains separate
claimed backends for the common bootstrap/idle root and private initial-component
root. Both use the same shared-Global Region-0 seed, with private Regions 1–7
and ledgers. The common root is activated first; the private root starts
unresident and is selected through `ProcessorAddressSpaceController`.
The owner retains both backends, roots, registry, controller and sealed
preparation for boot lifetime and provides no teardown path.

The [translated runtime](Ia64BspKernelRuntime.md) consumes these roots for
BSP user launch and AP cooperative service. Its scheduling, timer delivery,
IVT admission and polled shootdown operations are separate from this backend.
Hosted and target checks validate its region selection and exact ledger binding.
