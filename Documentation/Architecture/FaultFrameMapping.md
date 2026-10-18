<!-- SPDX-License-Identifier: Apache-2.0 -->

# Fault-to-Frame mapping preparation

`FaultFrameMappingPreparation` is the architecture-neutral safety boundary
between an exclusive `FaultToken` claim and a target `AddressSpace`
mapper. It does not edit page tables and it does not authorize a pager. The
capability layer first proves that the resolver may map the exact logical
`MemoryObject` slice from the exact `Frame` into the faulting `AddressSpace`.
For device memory it also supplies the exact `DeviceRegion` authority;
preparation then validates the operation and pins every required generation
until the mapper has finished.

Preparation asks the `FaultToken` for the authoritative active claim rather
than trusting caller-copied fault fields. Cancelled, stale, foreign, and
resolved claims cannot prepare a plan. The `MemoryObjectId`, `FrameId`, optional
`DeviceRegionId`, claimed `Thread`, and claimed `AddressSpace` must all
originate in the supplied registry, and every typed pin must resolve to the
exact concrete instance supplied by the mapper.

The request describes a page-aligned virtual start, a nonempty page count, a
page offset into the MemoryObject, a page offset into the Frame, and runtime
page permissions. Runtime
`PagePermissions` are deliberately separate from bootstrap
`MappingPermissions`: runtime pages may be user-accessible, while the
architecture-neutral pager API never requests a global translation.
Preparation enforces the following invariants before publishing a plan:

- virtual size and exclusive end are representable;
- the requested logical slice is wholly contained in the MemoryObject;
- permissions and cache policy remain within the MemoryObject descriptor;
- the requested physical slice is wholly contained in the immutable Frame;
- a device Frame has a matching DeviceRegion covering the complete physical
  slice, requested access, and cache policy;
- ordinary RAM does not carry a DeviceRegion;
- the faulting virtual address lies inside the requested virtual range;
- every mapping is readable and no mapping is both writable and executable;
- the requested access is actually permitted;
- a user-origin fault produces a user-accessible mapping;
- a device Frame is never executable;
- all reserved bytes are zero.

On success the plan retains the exact claim, AddressSpace, MemoryObject,
DeviceRegion when required, and Frame identities, both offsets, the physical
and virtual extents, requested protection, commitment, cache policy, and Frame
kind. The preparation owner
remains alive while the target
mapper validates architecture-specific canonicality and alias policy, changes
the page tables, performs the required translation invalidation, and finally
commits or cancels the claim. Every failure before the first success leaves the
owner fresh and releases any provisional pin. After success the owner is
immutable and later `Prepare` calls return `AlreadyPrepared`.

`AddressSpace` revalidates and retains the
logical, physical, and device owners before its native mapper installs any
leaf. The IA-64 runtime encoder and append-only software-refill backend mapping
path are implemented and focused-tested: a successful backend `Map` validates
and appends the plan to its ledger before returning the exact invalidation
range. Fault delivery and shootdown composition belong to the integrating runtime.
