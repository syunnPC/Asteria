<!-- SPDX-License-Identifier: Apache-2.0 -->

# Filesystem protocol v2

This protocol serves [launched components](BootPager.md).
`Source/User/Component/FsFrame.hpp` owns the wire constants and codec; `FsViews.hpp`
owns the bounded tables. The interface identifier is
`{0x4153545246535602, 0x46494C4553595303}`.

All requests use the canonical IPC frame and eight payload words. Word 0 is the
service incarnation and word 4 the view id. The kernel delivery supplies the
ResourceDomain identity; a payload cannot substitute another identity. A view
holds a root directory, read/write/exec rights and 64 slots, with root at slot 0.
Two views can be live simultaneously. View ids and slot generations never wrap.
A closed ancestor stays pinned by its descendants; reopening it gets a new slot
generation. Lookup of `..` follows these lexical parents and clamps at the root.

| Method | Number | Payload besides incarnation/view |
| --- | ---: | --- |
| Lookup (Open alias) | 1 | name length 1, parent slot 5, parent generation 6 |
| Create | 2 | as Lookup |
| ReadPages (Read alias) | 3 | slot 1, generation 2, first page 3, count 7 |
| Write | 4 | slot 1, generation 2, page 3 |
| Commit | 5 | transaction 1, crash-test flags 2, root slot/generation 5/6 |
| QueryCommit | 6 | transaction 1, root slot/generation 5/6 |
| Reply | 7 | transaction, status, detail, extra, kind, size, object, generation |
| Stat | 8 | slot 1, generation 2 |
| ReadDirectory | 9 | directory slot 1, generation 2, cursor 3 |
| MakeDirectory | 10 | as Lookup |
| Close | 11 | slot 1, generation 2 |
| InstallView (manager) | 12 | owner 1 (0 = sender), source slot/generation 2/3, source view 4 (0 = volume root), rights 5 |
| RemoveView (manager) | 13 | view 4 |
| PassFile (reserved, refused) | 14 | no supported encoding |
| ResetViews (manager) | 15 | incarnation only |
| ObserveWrites (reserved, refused) | 16 | no supported encoding |
| Quiesce (root manager, Manage) | 17 | incarnation and seven zero words; close admission, commit and flush |

The reserved file attachment kind is `0xFE`. Shutdown integrates method 17
through the existing manager face, requiring Manage for this terminal action.
[PowerOff](PowerOff.md) defines BlockFlush and durable clean-volume marking.
Unsupported methods/attachments are
refused by the codec. All unused request words must be zero as the codec requires.

Requests attach an Invoke reply Endpoint. ReadPages, Write and ReadDirectory also
attach a client Frame with `FsDataRights`; the server closes every received copy.
Named methods append a fixed 64-byte area with a strict single UTF-8 name and zero
padding. No path crosses the boundary. ReadDirectory writes one UTF-8 name into
the data Frame and replies with next cursor/byte count in detail/extra. Successful
Lookup/Create reply with slot/generation in detail/extra. Stat's final four words
are kind, size, backend object and backend generation; the backend generation is
distinct from the slot generation. ReadPages handles at most 32 pages, reusing a
single-page client mapping with checked offsets into the attached Frame.

A separate sealed manager Endpoint requires Invoke for InstallView/RemoveView
and additionally Manage for ResetViews. The boot test supervisor owns this face
until Component Manager integration. Rights cannot increase when deriving a view from an existing
view. Write views require the root ASTERIA.RW file and writable, clean media;
ASTERIA.OK is inventory consent only. Blank formatting is explicitly test-only.
A staged writer excludes other ResourceDomain identities. Removing its view or
resetting views aborts staging. Backend remounts invalidate non-root slots; clients
repeat lookup. AsteriaFS trees, principal/ACL policy and cross-process coherence
are not implemented by this service.

## Component quotas

Filesystem v2 charges three Endpoints (service, block replies and manager),
one runtime Frame and two runtime MemoryObjects, plus its writable image and
stack/start-block allocations. ReadPages' 32-page MemoryObject counts as one
object; borrowed Frame capabilities add no object charge. Shared immutable
images are charged once to root.

Object limits apply in addition to page, thread and queue-byte budgets.
The loader inherits ordinary component limits; only the account probe uses
explicit small limits to check refusal before a charge. Services use at most
one WaitSet and create no FaultChannel. AHCI's bounce-page budget remains
required for 32-bit DMA.

Root owns separate watchdog reply Endpoints outside monitored child accounts.
Storage drains before input starts, so the grant check takes their maximum
rather than their sum. TargetPagerGrant and VM object audits derive aggregate
bounds from the linked images and runtime configuration.

AMD64 uses size-oriented control code, per-function/data sections and linker
collection to fit its component segment; IA-64 retains its explicit-branch
size profile. Text-relocation, segment, stack and kernel-boundary gates apply.
