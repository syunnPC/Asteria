<!-- SPDX-License-Identifier: Apache-2.0 -->

# IA-64 Initial User Image Staging Owner

## Scope

`Ia64InitialUserImageOwner` is the bounded pre-takeover owner for sourcing,
decoding, allocating, and staging the initial user ELF from the boot-file region. It does not
create a `Frame` or `MemoryObject`, register an object, charge a
`ResourceDomain`, create or modify an address space, install a mapping, mint a
capability, admit scheduler state, or launch user execution. The separate
`Ia64InitialUserImageMemoryOwner` later claims its pointer-free receipt exactly
once to perform the Frame, MemoryObject, registration, and ResourceDomain
transaction. `Ia64InitialUserImageMappingOwner` performs the following
private-root mapping transaction; capability, scheduler, and launch work
remains outside this staging owner.

The owner is noncopyable, nonmovable, and single-shot. The first `Stage` call
changes `Fresh` pessimistically to `Failed` before observing the source. Only a
fully initialized and verified arena changes it to `Staged`. There is no retry,
free, rollback, or teardown path after allocator mutation; a failed aggregate
receipt remains reserved for boot lifetime. The materializer's commit cut
changes `Staged` to `Claimed` exactly once; a retryable materializer preflight
leaves it `Staged`, while no second memory owner can reuse a `Claimed` arena.
The staging owner remains the sole owner of the allocator receipt in both
states, and its immutable summary and segment-plan accessors remain available
in both states for boot-lifetime verification. The first staging failure
remains available separately.

## Decode and virtual layout policy

`Stage` accepts an immutable ELF view and the BootInfo boot-file receipt.
`Ia64InitialUserImageSource::CreateView` checks it against the nonwrapping,
page-aligned retained LoaderData region; `Get` builds views from recorded file
slices. Before decoding, its complete pointer range must be nonempty,
nonwrapping, and disjoint from the owner, the retained decoder output, and the
supplied `BootstrapPageAllocator`. One call to `Ia64P64Image::DecodeLoadPlan`
then selects Region 1 with the PAL-derived implemented virtual-address MSB and
these fixed limits:

- 4 KiB pages and segment alignment no greater than 64 KiB;
- at most eight `PT_LOAD` records;
- at most 160 normalized pages in one segment; and
- at most 352 normalized load pages in aggregate.

The three bounds are the header literals `Ia64InitialUserImageMaximumSegmentCount`,
`Ia64InitialUserImageMaximumSegmentPages` and `Ia64InitialUserImageMaximumLoadPages`,
which `Tools/Build/CheckImageStaging.py` also reads to check the linked pager
images at build time. They bound only the staging arena and the boot's copy and
verification work: the staged image is mapped by one ledger record per range and
refilled as translation-cache entries, so no translation register depends on
them.

The retained walk requires sorted normalized ranges and checked aggregate BSS
arithmetic. At least one byte must come from a real `p_memsz - p_filesz` BSS
interval; normalized page tail padding is not BSS. Empty load records remain
valid and receive no physical page.

The shared `Ia64UserVirtualLayout.hpp` contains layout constants used across
the Executable and Scheduler layers without importing the probe's mapping and
registry dependencies. It preserves all three fixed probe corridors, including
their one-page guards. The initial-user memory-stack mapping corridor
has mapped interval `[0x2000000005001000, 0x2000000005005000)`, and
the RSE backing-store corridor has mapped interval
`[0x2000000006001000, 0x2000000006005000)`. At this staging boundary the owner
retains only the corresponding four-page physical subranges; it installs
neither mapping. `Ia64InitialUserImageMappingOwner` leaves one unmapped page
on both sides when it installs the ranges. Every decoded normalized segment
range must avoid all five guarded envelopes. Guard pages consume no physical
storage.

## Aggregate physical arena

After complete decode-policy validation, the owner requests exactly one
physically contiguous `aggregate load pages + 8` arena with one-page allocator
alignment. Thus the arena is never larger than 360 pages. The original
allocator receipt is the only owning receipt. Segment, stack, and RSE records
are disjoint physical subrange descriptions packed in sorted-plan order:

```text
segment 0... segment N-1 | four stack pages | four RSE pages
```

The receipt must be nonzero, 4-KiB aligned, exact-sized Conventional memory
with the UEFI write-back attribute, without the runtime attribute, and with a
zero reserved field. Its exclusive end must not exceed
`1 << min(PAL PhysicalAddressBits, 50)`. PAL physical widths outside 32 through
63 are rejected. These checks happen before converting the physical start into
the pre-takeover identity-writable pointer.

The local identity alias must round-trip exactly through `UIntPointer`, retain
full alignment and extent, and avoid the source, owner/control storage, and
allocator. No pointer survives `Stage`. This is intentionally stricter than
the fixed probe's caller-supplied alias API because the aggregate arena is
allocated and consumed in one boundary before translation takeover.

## Byte initialization and publication

After every receipt, subrange, source offset, destination offset, and alias has
passed preflight, one `memset` zeroes the complete arena. Each file extent is
copied exactly to its packed segment base plus
`VirtualStart - PageStart`; overlapping source file extents are permitted.
The owner then compares every leading page prefix, payload, BSS interval, page
suffix, stack byte, and RSE byte against the required result. These are direct
correctness comparisons, not a hash, signature, provenance check, or tamper
gate.

On native IA-64, every aligned 16-byte bundle in the complete normalized
physical slice of every executable segment is published with `fc.i`, including
zero page padding. One ordered `sync.i`, `srlz.i`, `mf` sequence follows all
slices. Hosted builds deliberately make only this cache-publication boundary a
no-op.

## Boot integration and dependency

`BootApplication` placement-constructs the retained owner and calls `Stage`
before the shared-root shootdown page allocation. Synthetic-probe storage is
staged first only when `ASTERIA_IA64_SYNTHETIC_USER_PROBE=1`; the default image
path does not allocate or stage that diagnostic payload. The image owner
receives both PAL physical-address width and implemented virtual-address MSB.
Failure emits
`ASTERIA:INITIAL-USER-IMAGE-STAGE:FAIL` and halts. No pointer or owner reference
from this staging object is passed into `Ia64BspKernelRuntime`. After BSP runtime-memory
preparation, the owned-context continuation claims this retained owner's
materialization right and registers the initial-user objects before
translation activation. See `Ia64InitialUserImageMemoryOwner.md`; that boundary
still maps and launches nothing. The following retained mapping owner installs
the ranges in the private root. After translation activation, the BSP runtime's
separate `Ia64InitialUserImageLaunchOwner` consumes the ready memory and mapping
owners to prepare the default native launch; it does not transfer ownership of
the staging receipt.

The integrated boot-file ELF contains a nonempty raw
`PT_LOAD` BSS interval. The owner nevertheless retains `MissingRealBss` as a
hard semantic gate; normalized page padding never satisfies it. Hosted tests
use a mutable, page-aligned three-segment ELF fixture with real BSS and
production linker aliases, so they exercise the actual `Get` boundary without
adding an injection API. Native VM tests validate the staged image and its downstream owners together.
