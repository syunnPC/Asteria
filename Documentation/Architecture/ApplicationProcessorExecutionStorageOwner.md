<!-- SPDX-License-Identifier: Apache-2.0 -->

# IA-64 application-processor execution-storage owner

`ApplicationProcessorExecutionStorageOwner` is the boot-lifetime, fixed-capacity
coordinator for every enabled non-BSP processor's initial IA-64 memory stack and
RSE backing store. It retains at most three inert descriptors because the
platform's immutable logical processor map admits at most four CPUs and always
assigns the BSP `CpuId` 0.

## Admission and allocation

In production, one BSP-serialized `Prepare` call follows the Local SAPIC
snapshot and retained wake-vector validation and precedes rendezvous handler
initialization, SAL registration, `Arm`, and every wake IPI. It consumes an
existing `BootstrapPageAllocator` and the actual
`Ia64LocalSapicIpiControllerSnapshot`. Before the allocator is touched, the
owner requires a `Ready` snapshot with an exact one-through-four processor
count, the corresponding contiguous mask, valid PIB bases and base kind, zero
top-level reserved bytes, and an interrupt address derived exactly from each
used record's hardware ID/EID. Used records require zero reserved bytes and
pairwise-unique ACPI UIDs and hardware ID/EID pairs. Every unused record must
be completely zero. Logical `CpuId` values are formed and checked directly from
their snapshot indices, preserving the controller's BSP-first ordering.

For each logical `CpuId` from 1 through `ProcessorCount - 1`, in ascending
order, the owner requests exactly 16 contiguous pages aligned to eight pages.
Each allocation must be Conventional memory with write-back capability. Its
physical identity address is split into an exact 32 KiB downward memory stack
in pages `[0, 8)` and a distinct exact 32 KiB RSE backing store in pages
`[8, 16)`. The per-CPU preparer receives only the descriptors accumulated by
earlier iterations. This is sufficient because the retained allocator admits
only Conventional memory; firmware, loaded-image, BSP, and prior bootstrap
allocations are not members of its free set. Production accepts no arbitrary
exclusion or provenance list.

## Commit and failure

The state machine is `Fresh`, terminal `Failed`, or terminal `Ready`. Processor
count, enabled mask, descriptor count, and descriptor access are published only
after all AP allocations have been prepared successfully. A one-CPU snapshot
therefore validly commits `Ready` with processor count one, mask one, and a
canonical null, zero-length descriptor view.

Invalid snapshots consume no pages. Allocation or preparer failure transitions
the owner to `Failed`, returns the owner error together with the underlying
allocator error, preparer error, and exact failing logical `CpuId`, and exposes
no descriptor or descriptor view. Pages already removed from the allocator are
retained for the remainder of boot. There is no retry, free, rollback, mapping,
guard-page construction, SAL-handler state, AP wake, runnable-state transition,
or BSP `InitialAddressSpacePreparation` mutation.

The production boot path reports allocation or preparation failure once as
`ASTERIA:AP-RENDEZVOUS-ERROR:EXECUTION-STORAGE` and returns without
initializing the rendezvous handler, registering it with SAL, arming it, or
waking any AP. A successful `Ready` commit still supplies only BSP-retained
metadata: the owner itself creates no mapping or guard and grants no AP
ownership or runnable state. Production next initializes the single-use
takeover coordinator from that complete descriptor set. The coordinator copies
one immutable 24-byte physical record per enabled AP, and the registered bridge
lets that AP claim only its own record before leaving SAL on the descriptor's
dedicated physical 32 KiB memory stack and 32 KiB RSE backing store.

The [rendezvous handler](Ia64SalBootRendezvousHandler.md) owns physical takeover,
PAL capture and initial translation installation. Later root/IVT admission,
cooperative AP execution and timer delivery belong to the
[translated runtime](Ia64BspKernelRuntime.md#shared-root-ap-runtime).
These transitions grant no additional authority through this storage owner.

## Concurrency and publication

Preparation and ordinary owner access are BSP-serialized and use no internal
atomic or lock. The owner accessors remain unavailable until the complete set
is `Ready`. The production takeover coordinator supplies the required external
publication: BSP initialization validates the complete owner, copies the
records, and release-publishes each AP slot as `Prepared`; an AP acquire-claims
its own stable record exactly once. The owner's ordinary `Ready` field alone is
not the cross-processor synchronization protocol.
