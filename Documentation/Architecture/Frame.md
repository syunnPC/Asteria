<!-- SPDX-License-Identifier: Apache-2.0 -->

# Physical Frame object

`Frame` is the first architecture-neutral physical-memory capability object.
It describes one immutable, physically contiguous range of 4 KiB pages, its
resource-domain charge owner, whether it is ordinary RAM or device memory, and
its publication cache policy. RAM mappings preserve it exactly; a WC device
Frame permits UC downgrade through its DeviceRegion.

Initialization validates a nonzero aligned physical start, a nonempty page
count, multiplication and exclusive-end overflow, the complete enum domain,
and zero reserved bytes. Device ranges accept only uncached or write-combining
policy; ordinary RAM can retain any known platform cache policy. Successful
initialization publishes the descriptor exactly once and permits registration
as `ObjectType::Frame`.

The object deliberately does not allocate pages, install mappings, or infer
physical ownership. A physical allocator or device enumerator proves ownership
before creating it, and a mapped Frame claims its pages against every other
Frame of its registry (see Physical claims). `FaultFrameMappingPreparation`
pins the exact `FrameId`, checks cache-policy compatibility, and enforces bounds, W^X,
requested access, and device execution policy after capability authorization.
Device Frames also require an exact `DeviceRegion` whose MMIO range, access
policy, and cache policy cover the requested slice. `AddressSpace` retains the
exact Frame, MemoryObject, and any required DeviceRegion while its target
backend owns canonical-address checks, page-table encoding, and translation
invalidation.

The originating registry outlives every `FrameId`. Equal raw object numbers
from different registries are distinct identities. Frame storage remains alive
and immovable until registry destruction and collection complete.

## Physical claims

Frames of one registry may name overlapping pages (a twin, or one nested in
another), but at most one of them can be mapped at a time. The first mapping
reservation of a Frame, of any access and by any root or IOMMU domain, claims
the Frame's whole physical byte range `[start, start + pages * 4 KiB)` in the
`ObjectRegistry` that holds it (`ClaimExclusiveRange`). A claim of another
Frame of that registry that overlaps the range refuses the reservation, so the
mapping fails with `PhysicalOwnershipConflict` in a root and with
`FramePolicyRejected`/`PhysicalOwnershipConflict` in an IOMMU domain, before
any native write. The last release of a reservation ends the claim; a claim
also ends when the collector returns its Frame. Bordering ranges do not
conflict, and the Frame lock is held while the registry lock is taken.

`AddressSpace` and `IommuDomain` map only Frames of their own registry, and a
kernel registers every Frame, root, and IOMMU domain in one registry (both
boot runtimes do). While a Frame is mapped, it is therefore the only source of
mappings of its pages, so:

- RAM mappings require the Frame's exact cache policy. A WC device Frame
  permits a UC MemoryObject downgrade; both preparations
  use the selected MemoryObject policy and the root checks compatibility.
  Its DeviceRegion excludes differing live policies, preventing mixed aliases;
- the Frame's write-versus-execute ledger holds every reservation of those
  pages.

Both boot runtimes issue disjoint Frames (IA-64 takes every Frame range from
its bootstrap page allocator, AMD64 uses disjoint runtime-pool pages for its
probe page and user-image ranges), so no boot mapping meets a claim.

## Write-versus-execute ledger

For RAM creation, `FrameObjectPool` pins and charges the ResourceDomain,
then obtains a run from `PhysicalPageAllocator`. A Zeroing
slot retains the pin, charge, run and remaining dirty mask without a Frame or
capability. With both pool and AddressSpace locks released, the scrubber clears
dirty pages in chunks of at most 16; only a successful chunk clears its mask.
Registration and capability minting follow successful zeroing. Deferred or
Faulted zeroing releases the original run dirty and tries one clean run with
the charge retained; no clean run returns Busy/WouldBlock. A later failure uses
ReturnVerified. Collect skips Zeroing slots and subtree destruction returns Busy.

The allocator has one rank-6 leaf lock, naturally aligned buddy orders 0–9,
metadata outside managed pages, and dirty/allocated/claimed/retired bits.
The AMD64 scrub leaf derives its own slot from GS:16, checks the seed ignoring
A/D and invalidates after repoint and restore in any known active root, with
interrupts off and no AddressSpace lock.

A `BootFrame` keeps at most 256 reservation records keyed by exact physical
start, page count, and access (writable or executable), each with a reference
count; a new key beyond that is refused (`MappingCapacityExceeded` for a root,
`FramePolicyRejected` for a domain). A writable reservation conflicts with any
overlapping executable one and the reverse. Read-only access creates no record
but, like every reservation, holds the Frame's claim. `AddressSpace` reserves
before its backend installs a CPU translation, and `IommuDomain` reserves
device-write access before its backend installs a DMA translation; the Frame
lock serializes both across every root and domain. Disjoint subranges of one
boot Frame remain independently usable.

`FrameObjectPool` uses a `CreatedFrame` instead:
two per-Frame W/X counters and no subrange records. Any writable reservation
refuses while an executable reservation exists anywhere in the Frame, and
the reverse, including disjoint slices. Read-only reservations take neither
counter. Both CPU and IOMMU commits reserve through `FrameMappingKey`,
constructible only by their owners, and retain the same whole-Frame claim
and rollback rules. Boot Frames keep their original subrange behavior.
The 4,096 created-Frame slots occupy a platform-attached slab; bitmaps find
free/active/quiescing slots and the object address identifies its slot.

A reservation is taken only after every other validation has
passed, and it is released only:

- when the backend refuses the operation before any native write: an ordinary
  `AddressSpace` backend rejection (including the terminal `CorruptState`,
  `InternalFailure`, and `NotReady` ones), or an IOMMU
  `UnsupportedConfiguration` or `CapacityExceeded` rejection;
- by CPU or IOMMU `Unmap` once the backend has proved the translation drained;
- when the owning `AddressSpace` or `IommuDomain` quiesces.

A published CPU mapping keeps its reservation until unmap or root quiescence, and a
published DMA mapping until `Unmap` or domain quiescence. Every outcome that
may have written native state also keeps its reservation until quiescence:
`CommittedStateUnknown`, an unknown backend error, a malformed backend success,
a rejected or failed shootdown, an ambiguous IOMMU `Map`, and a failed IOMMU
`Unmap`. The owner is then `Faulted`, and its retained reservation keeps
refusing the opposite access, and keeps the Frame's claim, through every other
root and IOMMU domain until it quiesces. A page that has been mapped writable
therefore becomes executable only after every root and domain holding that
writable reservation has quiesced (or unmapped it, for DMA), and the reverse;
an overlapping Frame can be mapped only after every reservation of the
claiming Frame has been released.
