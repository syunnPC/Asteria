<!-- SPDX-License-Identifier: Apache-2.0 -->

# IA-64 runtime translation ledger

`Ia64RuntimeTranslationLedger` is a fixed-capacity, allocation-free software-refill
lookup table for one runtime address-space translation identity. It retains at
most 256 non-overlapping mapping records and uses
`Ia64RuntimeTranslationEncoder` as the authoritative validator.

The ledger supports one externally serialized, non-interrupt writer and bounded
concurrent readers. With `constinit` storage, its publication word has these
exact states:

- `0`: uninitialized;
- `1`: initialized with no records; and
- `n + 1`: a prefix of `n` records is published.

`Initialize` validates and freezes one common capability set. `Append` validates
the complete plan, rejects overlap or capacity exhaustion, reuses the first free
slot within the publication prefix before extending it, and stores VirtualStart
and BaseTranslationWord before publishing PageCount with a release store.
Failures publish no state and do not consume capacity. `Lookup` acquires the
prefix and each record's PageCount before its payload, aligns the raw miss address to 4 KiB, and reconstructs
the exact page translation word, insertion word, and faulting address. Readers
never allocate, lock, call firmware, or execute privileged instructions.

`Remove` finds the live record with the
exact virtual start, page count and base translation word Append stored and
tombstones it with a single release store of a zero page count
([physical memory management](PhysicalMemoryBootstrap.md)). Lookup, Append's
overlap check and the refill scan read the page count once and match no page of a
tombstone. The AddressSpace lock serializes writers and its pending-shootdown
state rejects Map until every resident processor acknowledges the unmap.
Only then may Append reuse the tombstone; no per-processor quiescence gate is
needed. The 256-record bound limits simultaneous mappings, rather than lifetime
appends. A reader that loaded the page
count before the tombstone may still insert one of the record's translations,
so the writer's caller invalidates the range on every processor that may run
the ledger before the record's frames are released; the refill handler runs
with interrupts disabled, so an invalidation request it delays lands after its
insertion.

The native refill loads PageCount first with `ld8.acq`, then VirtualStart and
BaseTranslationWord. Tools/Verify checks this ordering. The host model stages
both payload stores while PageCount is zero and publishes it last; the IA-64
pager self-test completes 300 map/unmap cycles in one space.

The object and its storage must outlive the writer and every reader. The caller
must exclude competing writers, writer preemption by another writer for the
same ledger, and destruction while access remains possible.

## Component boundary

The boot-lifetime BSP runtime-memory owner embeds this ledger and initializes it
empty; the shared-Global Region-0 seed mappings are not appended. It publishes
the empty ledger's exact identity through the active runtime binding. Each
later successful `Ia64RuntimeAddressSpacePageTable::Map` appends one mapping
record before returning its invalidation range. The ledger itself does not
provide an IVT refill entry, CPU-local binding, region-register activation, a
runtime `IAddressSpacePageTable` backend, or a runnable root.
Physical takeover and translated AP execution belong to the
[rendezvous handler](Ia64SalBootRendezvousHandler.md) and
[BSP runtime](Ia64BspKernelRuntime.md#shared-root-ap-runtime).
Their root, IVT, binding, scheduling and timer transactions are outside the ledger.
