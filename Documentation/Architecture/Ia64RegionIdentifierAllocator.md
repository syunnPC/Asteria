<!-- SPDX-License-Identifier: Apache-2.0 -->

# IA-64 runtime region-identifier allocation

`Ia64RegionIdentifierAllocator` is the candidate-RID source owned by the live
BSP runtime-memory handoff. It issues one naturally aligned
block of exactly eight consecutive candidate region identifiers for each
runtime `AddressSpace`, with receipt index zero through seven corresponding to
IA-64 virtual-region numbers zero through seven. It allocates no storage,
performs no privileged operation, and is not a page-table or region-register
activation backend.

The bootstrap region-register set owns RIDs `0x2000` through `0x2007`. Runtime
allocation therefore begins at `0x2008`. SAL handoff RIDs `0x1000` through
`0x1003` are below that boundary. Every successful allocation advances by
eight, so runtime blocks are aligned, do not overlap either reserved range,
and do not overlap one another.

## Capability and arithmetic boundary

The primary initialization entry accepts the already-reduced common RID width
from `Ia64RuntimeProcessorCapabilities`. The decoder and system profile own the
unrelated physical-width, protection-key, translation-register, page-size,
cache-topology, memory-attribute, and reserved-field invariants. The allocator
checks that the implemented RID width is in the architected 18-through-24-bit
range and can represent at least one complete runtime block. A compatibility
entry accepts one already-reduced `PalVirtualMemoryCapabilities` record and
preserves its reserved-field validation for existing callers.

The supplied width must be the profile-wide minimum across every CPU on which
these address spaces may run, not an arbitrary CPU-local PAL sample. The
BSP-owned runtime-memory owner freezes every enabled processor
record and passes their common minimum width before it admits the BSP root.
Profile membership does not by itself make an AP resident or runnable.

After width validation, the exclusive representable limit is computed in a
64-bit value as `1 << ridBits` and then retained in 32 bits. Allocation checks
that a complete eight-RID block fits before advancing. At 18 bits this yields
31,743 runtime blocks; the final block is `0x3fff8` through `0x3ffff`. At 24
bits it yields 2,096,127 runtime blocks. A failed allocation at exhaustion does
not change the next RID or allocation count, and repeated failures remain
terminal for new allocation.

## Lifetime and synchronization

Exactly one authoritative allocator instance inside `Ia64BspRuntimeMemory`
owns the system-wide RID namespace for the complete boot lifetime.
Independently initialized instances are forbidden: each would start at
`0x2008` and issue duplicate blocks. The owner enforces boot-lifetime
retention; the allocator deliberately adds no singleton, global registry, or
lifecycle machinery.

The runtime profile never reuses a RID. There is no release, reset,
epoch, global translation-cache purge, hashing, or tamper-detection protocol.
The allocator's existing platform spin lock serializes initialization,
allocation, and snapshots. Callers must exclude same-CPU preemption and re-entry
through the same lock; the allocator is not ISR-safe. Work after lock
acquisition is constant and bounded.

An allocation receipt is an immutable candidate set, not a claim that all eight
RIDs must be loaded or are already semantically private. The runtime profile keeps RR0 on one fixed shared bootstrap RID and deliberately
ignores candidate index zero. RR1 through RR7 use candidates one through seven
as never-reused private identities. Spending the unused candidate zero
preserves the simple aligned eight-RID stride and monotonic accounting. A region-set policy may replace another selected entry with a
shared bootstrap RID only after validating that every mapping in the
corresponding complete virtual region is `Global`. The allocator itself neither
makes that mapping-policy decision nor implies that a shared/global identity
has already been selected.
