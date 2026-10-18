<!-- SPDX-License-Identifier: Apache-2.0 -->

# Proactive pinned-Frame mapping

`PinnedFrameMappingPreparation` is the portable preparation seam for mapping
an exact slice of a pinned `MemoryObject` to an exact slice of a RAM `Frame`
without first taking a page fault. It supports both supervisor and user
mappings. The seam grants no authority: only trusted kernel integration, or a
caller already capability-authorized for the exact target root, owners, ranges,
and permissions, may invoke it. The `AddressSpaceId` passed to
`BeginPinnedFrameMapping` identifies the exact destination root but is not an
authority token.

`FaultToken` is exclusively for resolution of a real fault. Proactive mapping
must not create or claim one, and this preparation cannot resolve a fault,
resume a thread, or establish that a caller may map memory.

Preparation pins the named `MemoryObject` and `Frame` generations through the
same `ObjectRegistry` and verifies that each identity resolves to the exact
concrete object supplied by the caller. It then enforces these portable
invariants:

- the virtual start is page-aligned, the page count is nonzero, and the virtual
  range is representable;
- the `MemoryObject` is ready, has pinned commitment, contains the complete
  logical range, and permits every requested permission;
- the `Frame` is ready RAM, contains the complete physical range, and has the
  same immutable cache policy as the `MemoryObject`;
- permissions contain only known bits, include read access, and never combine
  write and execute access; and
- every reserved byte is zero.

User access is accepted only when `PagePermission::User` is present in the
`MemoryObject` maximum permissions. Supervisor requests remain valid when
their exact permission set is allowed. On success, the prepared record retains
the exact logical and physical owners, offsets, ranges, cache policy, pinned
commitment, and requested permissions; the user bit is neither inferred nor
removed. Every failure before success releases provisional pins and leaves the
preparation reusable. A successful preparation is immutable and keeps its
pins until its owner is destroyed.

`AddressSpace::BeginPinnedFrameMapping` accepts only an exact matching target
identity and a prepared pinned-RAM record. The common mapping transaction then
revalidates the record, retains its own `MemoryObject` and `Frame` generations,
reserves the Frame-wide alias policy, and passes the exact permissions to the
native page-table backend. The backend installation remains provisional until
`CompleteMapping` observes acknowledgement from every processor resident when
the transaction began. If no processor is resident, both target and
acknowledgement masks are zero and the caller may complete the transaction
directly. Published mappings retain their owners and policy reservations until
root quiescence.

Callers serialize each preparation object, keep all supplied storage alive,
and do not invoke preparation from interrupt context. Shootdown acknowledgement
and completion follow the common `AddressSpace` contract described in
`AddressSpace.md`.
