<!-- SPDX-License-Identifier: Apache-2.0 -->

# AMD64 runtime page-table mapper

`RuntimePageTableMapper` is the AMD64 consumer of a validated
fault-to-Frame plan. It adds absent 4 KiB leaves to an existing four-level AMD64
root backed by a fixed, physically contiguous page-table arena. One call maps at
most 512 pages (2 MiB). Desktop sessions use a
256-page arena beside ordinary storage bounded by 64 pages; capacities
65 through 255 are invalid. Each storage's PageCapacity bounds allocation.
Seed verification keeps its 64-page reachable-tree bound while checking all
unused pages of the selected arena. Runtime validation encodes level,
reachability and user-path state in one byte per page, within the 512-byte
stack budget. Neither unmap nor client slot reuse frees intermediate tables.

The mapper validates the complete request before writing anything. It requires
the captured four-level, write-protect, PAT and NX capabilities;
checks canonical virtual and physical-width bounds; rejects a mapping of the
page-table arena itself; walks every existing ancestor; rejects large, malformed,
or occupied paths; proves that every used table page is reachable
from exactly one parent; rejects a new writable/executable alias of an existing
physical page; and counts every missing table page before checking capacity. A
failure leaves both the arena and result unchanged.

User mappings occupy the lower four-level canonical half above page zero;
upper-half canonical mappings require supervisor permissions. The complete
last lower-half page remains mappable. `UserAddressPolicy.hpp` shares this
range with thread start and user returns. Fault normalization retains the
actual failing address, even outside this range, so a supervisor can terminate
the thread; describing a fault grants no mapping authority.

The existing root is owned state: it begins as the bootstrap tree accepted by
the initial verifier and is subsequently extended only through this mapper.
Checking every new physical range against all existing leaves therefore
preserves the root-local W^X alias invariant inductively. The `AddressSpace`
owner retains each mapping and its exact Frame generation, while Frame's shared
interval ledger rejects overlapping writable/executable aliases across roots.
The memory-manager factory separately owns the canonical seed and ensures seed
pages are never issued as Frames.

After successful preflight, each new table page is cleared before a release
store links it into the active tree. Reused supervisor ancestors are promoted
with an atomic release OR when a user leaf is added; existing nucleus leaves
remain supervisor-only at their leaf or lower ancestor. Leaves preserve the
Frame cache policy and encode user, write and execute protection without
permitting W+X. Existing translations are never replaced. The returned result
contains the new used-page count and exact virtual invalidation range; the caller
adopts the count and must invalidate that range before committing the
`FaultToken` claim.

`RuntimeAddressSpacePageTable` binds one unique preparation/arena to one
AddressSpace and serializes competing claims and map operations. It consumes
the preparation before verifying table bytes, preventing another wrapper from
reading while the winner writes. Claim still requires the exact seed object
published by that preparation, then clears its borrowed seed pointer. The
preparation and caller seed storage may then be destroyed; later activation
never reads either object. Seed structure and arena bounds were already
verified while initialization owned both objects, so activation does not
repeat those immutable checks. A malformed result after native success is reported as
`CommittedStateUnknown`, which keeps the root and all mapping resources
terminally owned.

Activation is an operation on that same claimed backend, not a detached root
token or codec. With local interrupts disabled and the owning AddressSpace lock
held, the architecture-neutral owner supplies its identity and the calling
`CpuId`. The backend serializes against mapping and release, then rejects an
unclaimed or faulted backend, a different owner, or an invalid processor before
invoking native code.

Immediately before that commit, a platform-local adapter reads live CR4. If
PGE, LA57, or PCIDE is set, activation returns `UnsupportedConfiguration`
without calling the CR3 helper. Otherwise the commit adapter writes the
verified, page-aligned PML4 physical address to CR3, reads CR3 back, and returns
the observed value. An exact readback means the switch and CR3's required local
invalidation completed. Any mismatch is after the write and is therefore
`CommittedStateUnknown`; the backend becomes faulted because the requested root
may already be current. All other activation failures are pre-commit and leave
the previous processor root unchanged. Host tests replace both platform
adapters with strict recording functions and never execute privileged register
instructions.

This raw-root contract enforces the current AMD64 target profile at every
commit: CR4.PCIDE is clear, so no PCID or no-flush bits are accepted in the
requested CR3 value; CR4.PGE is clear, so PTE global bits have no translation-
retention effect and a CR3 reload invalidates every translation relevant to
this root; and CR4.LA57 is clear, so the root is interpreted as the verified
four-level PML4. Enabling any
of these modes requires a separately specified activation and invalidation
policy; it must not silently reuse this path.

The architecture-neutral owner coordinates the resident-CPU acknowledgement
transaction, and `Amd64LocalTlbInvalidator` performs each bounded local
`invlpg` after mapping changes. The [native runtime](Amd64BootRuntime.md) connects
cross-CPU acknowledgement and private-root Thread resumption. Its polled
mailbox drain and shootdown IPI handling belong to that runtime. Scheduling and CPL3 frames belong to the
run loop and native entry, while this primitive owns root activation. IA-64 uses
a separate target mapper.
