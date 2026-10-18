<!-- SPDX-License-Identifier: Apache-2.0 -->

# IA-64 Initial User Image Mapping Owner

## Scope

`Ia64InitialUserImageMappingOwner` installs the ranges materialized by
`Ia64InitialUserImageMemoryOwner` into the existing private initial-component
`AddressSpace`. It creates no address space, Frame, MemoryObject, capability,
protection domain, thread, scheduling context, or launch record. In particular,
it does not make the private root resident and does not transfer execution to
the staged P64 image.

The owner is a trusted boot coordinator, not a general mapping authority. It
derives the destination from `Ia64BspRuntimeMemory` and derives every object,
address, extent, offset, and permission from the ready memory owner. It never
accepts an independent user-supplied mapping description and never fabricates
a `FaultToken`.

## Batch preflight and commit

`Install` is allocation-free and externally serialized before native
translation activation. Before changing the private root it validates the
complete batch:

- the retained BSP runtime and initial-user memory owners are ready and belong
  to the supplied registry;
- the borrowed root is the exact registered initial-component root, is ready,
  and has no resident processor, transition, or shootdown;
- the aggregate arena is addressable by the common capability profile of all
  admitted processors and the root has capacity for every range;
- every range names the exact registered pinned MemoryObject and the one
  aggregate write-back RAM Frame, with readable user permissions and no
  writable/executable combination;
- logical MemoryObject offsets are zero, while physical Frame offsets preserve
  the materializer's exact contiguous packing;
- planned virtual and physical ranges are mutually disjoint and do not overlap
  an existing root mapping; and
- the one-page stack and RSE guards are representable and vacant.

Preflight failures leave the mapping owner `Fresh`, expose a diagnostic, and
permit a corrected retry. Immediately before the first mapping preparation it
changes to terminal `Failed`. The IA-64 ledger is append-only, so any later
failure retains already completed or possibly committed mappings and rejects
replay. Only a completely verified batch publishes `Ready`.

## Mapping transaction

Each materialized range is translated to one
`PinnedFrameMappingRequest`: the plan supplies its virtual start, page count,
permissions, and aggregate-Frame offset, while the logical MemoryObject offset
is zero. `PinnedFrameMappingPreparation` revalidates exact object generations,
ranges, maximum permissions, pinned commitment, RAM kind, cache policy, and
W^X. `AddressSpace::BeginPinnedFrameMapping` then applies the common per-root
and Frame-wide alias policies and calls the IA-64 backend.

The private root is deliberately nonresident throughout this operation.
Consequently each successful begin publishes a zero target mask, and
`CompleteMapping` can commit it directly because the acknowledgement mask is
also zero. No root switch, TLB invalidation transport, synthetic
acknowledgement, or fault-resolution path participates.

After every completion the owner checks the exact appended mapping and
receipt. Final validation checks the complete ordered batch, active mapping
references, unchanged nonresident root state, and continued absence of both
guard pages. `GetSummary` and `GetMapping` become available only in `Ready`.

## Boot integration and verification

`BootApplication` placement-constructs this boot-lifetime owner after the
initial-user objects and private root exist, and before
`InitialTranslationActivator::Activate`. Failure emits
`ASTERIA:INITIAL-USER-IMAGE-MAPPINGS:FAIL` and halts before activation. Success
reports exactly one `ASTERIA:STAGE:INITIAL-USER-IMAGE-MAPPINGS` event after the
object stage and before translation activation.

After translation activation, the default IA-64 path passes this ready owner
to `Ia64InitialUserImageLaunchOwner`. That later boundary revalidates every
retained range and mapping, constructs the request with the decoded P64 entry
bundle and GP, and launches through the BSP runtime's already constructed
private-root scheduler graph. None of those responsibilities or authorities is
owned by this mapping class.

Hosted tests compose a decoded P64 image, real materialized objects, and the
real IA-64 private-root backend. They verify exact permissions and physical
offsets, zero-target completion, guard vacancy, unchanged resource accounting,
retryable preflight, and replay rejection. Target checks compile the owner at
O0 and O2, enforce the existing stack budget, and prohibit dependencies on
fault tokens, fault mapping, capabilities, and scheduler launch machinery.
