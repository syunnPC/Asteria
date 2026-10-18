<!-- SPDX-License-Identifier: Apache-2.0 -->

# Frame Pool

`FramePool` is a compatibility facade over
`PhysicalPageAllocator`. Hosted initialization supplies one zeroed 1,024-page
range; native boot imports the usable ranges into that same allocator, with
metadata in the runtime reservation. No separate frame-pool receipt remains.
The allocator owns the leaf lock; the facade touches no page contents.

The allocator bounds a run and its power-of-two alignment at 512
pages. `Allocate` requires clean pages. `Release` makes a run dirty;
`BeginScrub`, `EndScrub` and `AbortScrub` manage a claimed dirty prefix.
Claimed or retired pages cannot be allocated. The snapshot reports the
allocator's free, allocated, dirty and claimed counts. Native RAM creation
allocates directly, then zeros its dirty mask outside both locks in chunks
of at most 16 pages.

`Amd64FrameScrubber` uses the current processor's scrub window in a known
active root; the per-processor root table includes created roots.
`Ia64FrameScrubber` performs the established physical-mode store sequence.
Both keep the run unavailable during zeroing and report Deferred or Faulted
without publishing an object. No allocation or pool lock crosses zeroing.

## Created Frames and MemoryObjects

`FrameObjectPool` and `MemoryObjectPool` each attach a 4,096-slot metadata
slab, and follow the [Endpoint pool](EndpointPool.md)
lifecycle. Manage on a ResourceDomain authorizes the operation and names the
charge; every failed creation rolls back its charge, run and capability.
The created Frame owns the committed and resident page charge and
`sizeof(CreatedFrame)`; the created MemoryObject is Pinned and charges only
its metadata. An AddressSpace pins both while mapped, so collection waits
for unmapping and all other pins. Collected pages return dirty.

Desktop sessions need the maximum 512-page runs for its fixed pool
layout and native output. The 24-byte RAM request supplies page count, log2 alignment, address limit
0 or 32, and fourteen zero bytes. The 16-byte request retains its 128-page
and 16-page-alignment bounds and limit 32. Session buffers use limit 32.
Allocation failures return ResourceExhausted; memory-pressure notifications
and POSIX integration remain unimplemented.
