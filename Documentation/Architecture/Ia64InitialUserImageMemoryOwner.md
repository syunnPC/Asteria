<!-- SPDX-License-Identifier: Apache-2.0 -->

# IA-64 Initial User Image Memory Owner

## Scope

`Ia64InitialUserImageMemoryOwner` is the post-staging, pre-mapping owner for
the IA-64 initial user image staged from the boot-file region. It claims the pointer-free receipt's
materialization right exactly once, represents that arena with one aggregate
`Frame`, and creates one pinned `MemoryObject` for every nonempty logical
range. The staging owner remains the sole owner of the allocator receipt and
physical pages after the claim.

This boundary creates no `AddressSpace`, mapping, capability, protection
domain, capability space, thread, scheduling context, or launch record.
`Ia64InitialUserImageMappingOwner` consumes its public range views in the next
transaction and installs them in the already-owned private root. Installing
the P64 global pointer in `r1` and transferring execution remain the separate
`Ia64InitialUserImageLaunchOwner` and BSP-runtime transaction.

The owner is noncopyable, nonmovable, placement-constructed in aligned raw
storage, and retained for boot lifetime. The staging owner, its pages, the
registry, issuer, root `ResourceDomain`, and this owner must remain alive and
immovable. `Materialize` is externally serialized on the non-preemptible BSP;
unrelated registry or resource-domain mutation and all destruction,
quiescence, or collection work are excluded.

## Preflight and state

`Materialize` first performs a preflight free of retained or persistent
effects. It requires a staged source, revalidates the complete image and arena
summary, obtains each segment plan through the public staging-owner accessors,
and proves exact physical packing. It also verifies that the issuer belongs to
the supplied registry and that the registered root `ResourceDomain`
generation identifies the supplied ready domain. Exact identity verification
temporarily takes and resets an `ObjectPin`.

The root-domain snapshot must show enough committed pages, resident pages, and
MemoryObject metadata capacity for the complete transaction. The registry's
`GetActiveCount` check is only a necessary diagnostic: it excludes occupied
quiescing or retired slots and does not reserve the reported capacity. The boot
caller supplies the stronger invariant that no destruction, quiescence, or
collection has occurred or can race this serialized call.

A preflight rejection leaves the memory owner `Fresh` and the staging owner
`Staged`, records a diagnostic, and may be retried. After the final source
revalidation, the first persistent commit-cut effect changes the staging owner
from `Staged` to `Claimed` exactly once. The memory owner then changes to
`Failed` before the first object initialization. `Claimed` continues to own the
allocator receipt and expose its immutable summary and segment plans. Any
later rejection is terminal: the claim, initialized objects, registrations,
and ResourceDomain usage are conservatively retained, so a fresh memory owner
cannot replay the arena. Only a fully verified transaction publishes `Ready`.
Repeating `Materialize` after a mutating attempt or success is rejected.
Unexpected registration exhaustion after the preflight cut is therefore a
terminal failure and all committed state is retained.

## Object model and ranges

The aggregate `Frame` covers the staging owner's exact contiguous arena with
write-back RAM policy and the root domain's numeric charge owner. Nonempty
decoded load segments appear first in their sorted order. Each receives one
`MemoryObject` with its decoded maximum user permissions, no guard request,
and the exact offset into the aggregate Frame. Zero-page segments receive no
object. The four-page memory-stack and four-page RSE backing-store ranges
follow, each with read/write user permission and a one-page-before-and-after
guard policy. Guards describe a requirement for the later mapper and consume
neither Frame storage nor a MemoryObject.

Every range `MemoryObject` uses write-back cache policy and
`MemoryCommitment::Pinned`. Its initialization charges the root
`ResourceDomain` for that range's committed pages, resident pages, and
MemoryObject metadata. `Pinned` is the MemoryObject commitment policy; it is
not a permanently retained `ObjectPin` handle.

The root issuer registers the aggregate Frame and every range MemoryObject.
After each registration, the owner takes a temporary registry pin to prove
that the returned identity resolves to the exact embedded object and expected
type, then releases that verification pin. The registry registrations and
embedded objects remain live with the owner. No capability handle is minted
or charged by this transaction.

On success, the summary retains the decoded image information, aggregate
arena, registered aggregate-Frame identity, exact root ResourceDomain
generation, and range count. Each range plan retains its purpose, guard
policy, permissions, virtual start, page count, Frame offset, original segment
index where applicable, and registered MemoryObject identity. `BorrowRange`
exposes only non-owning pointers to the embedded aggregate Frame and selected
MemoryObject for `Ia64InitialUserImageMappingOwner` to consume during its
mapping transaction.

## Boot placement and diagnostics

The owned-context continuation calls `PrepareIa64InitialUserImageObjects`
only after `PrepareIa64BspRuntimeMemory` has succeeded and before
`InitialTranslationActivator::Activate`. The helper placement-constructs the
owner, calls `Materialize` with the staged owner and retained BSP
registry/issuer/root-domain objects, and publishes the boot-lifetime owner
pointer only on success.

Failure emits `ASTERIA:INITIAL-USER-IMAGE-OBJECTS:FAIL` and halts before the
mapping step or translation activation. Success reports exactly one
`ASTERIA:STAGE:INITIAL-USER-IMAGE-OBJECTS` event between
`ASTERIA:STAGE:BSP-RUNTIME-MEMORY` and
`ASTERIA:STAGE:INITIAL-USER-IMAGE-MAPPINGS`, which in turn precedes
`ASTERIA:STAGE:INITIAL-TRANSLATION-ACTIVATION`. The object stage proves
materialization and registration only; the later mapping stage still does not
prove launch. After translated runtime preparation, the default IA-64 path
reports `ASTERIA:STAGE:INITIAL-USER-IMAGE-LAUNCH-PREPARED` and
`ASTERIA:STAGE:INITIAL-USER-IMAGE-LAUNCH` around the separate launch boundary.
AMD64 reports none of these stages and does not compile this implementation.

## Verification boundary

Hosted Debug and Release tests cover current and maximum range shapes,
permissions, guard policy, Frame offsets, object identity binding, exact
ResourceDomain accounting, retryable preflight failures, retained terminal
failure, successful and failed claim replay rejection, preservation of the
first identities and retained accounting, and the absence of `AddressSpace`
mutation. The focused test also runs under the hosted stack probe.

Standalone headers compile for IA-64 and AMD64. Target checks compile the
implementation for IA-64 at O0 and O2 with stack and callgraph gates. A static
dependency oracle requires the staging accessors, Frame and MemoryObject
initialization, root registration, ResourceDomain inspection, and temporary
registry pinning; its exact allowlist excludes mapping, capability-minting,
scheduler, and launch dependencies. A separate boot oracle requires the
runtime-memory, object-materialization, and translation-activation calls in
that order. Firmware builds link the owner only into IA-64, and every IA-64
VM case requires the object stage exactly once and in order; AMD64 never
reports it.

