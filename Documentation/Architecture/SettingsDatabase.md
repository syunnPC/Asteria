<!-- SPDX-License-Identifier: Apache-2.0 -->

# Settings database storage library

A pure user-space library for typed hierarchical settings, per-key ACLs and
change notifications. [Identity](Identity.md) supplies principal
and view policy. Service integration and persistent product settings are not implemented.
UTF-8 validation uses the project's decoder (Unicode Standard 16.0, section
3.9, table 3-7). The disk encoding below is project-owned, not an external
registry or filesystem format.

## API and bounds

`User/Settings/Database.hpp` provides explicit `Format` and recovering `Open`,
`Create`, named `Read`/`Write`/`DeleteValue`, separate `EnumerateValue` and
`EnumerateSubkey`, leaf `Delete`, `GetAcl`, `SetAcl`, and bounded
`Watch`, `Poll`, `Unwatch`. Every mutation is a durable single-operation
transaction. `Access` carries a principal id and independently limited view
rights. The embedding service authenticates these arguments and serializes
calls, including storage callbacks (no reentrancy), and keeps watch handles
private to the corresponding authenticated caller. There is no administrator
bypass. Formatting is an explicit provisioning operation on a zero-filled
medium; opening never formats or repairs it.

The database contains at most 32 keys including root, depth at most eight
below root and key names at most 63 UTF-8 bytes. A separate pool holds 64
named values, at most eight per key, each at most 256 bytes with a value name
of at most 63 UTF-8 bytes. Empty value names are the default value; other
value names may include `/` or dots, but never NUL. Names and paths are
separate API arguments. A subkey and value may have the same name. Absolute
paths use `/`, root is `/`; no empty components,
trailing separators, NUL, `.` or `..`. Names compare exact UTF-8 bytes, without
case folding or normalization. Strings may contain NUL. Integer values are
signed 64-bit two's-complement little-endian bytes; booleans are one byte 0/1;
strings and binary are byte sequences; string lists are a little-endian u16
count followed by u16 byte lengths and UTF-8 bytes (empty items allowed).
`None` is an explicitly stored empty typed value; `DeleteValue` removes the
named entry. Read of an absent entry returns NotFound. Creating/replacing a
value requires Write, removal requires Delete, using the containing key ACL.
Deleting a leaf key atomically removes all its values. Enumeration uses
zero-based ordinals in slot order, skips unreadable subkeys and leaves outputs
unchanged at the end (NotFound); callers restart if Generation changes. Bytes
past the value length are ignored on input and
zero on successful reads. Up to eight explicit ACL entries per key contain u64
principal, allow/deny, a five-bit rights mask and a boolean inheritance flag.
Inheritable entries on every ancestor apply dynamically, plus all local entries.
Matching allows are unioned, matching denies are unioned and removed, then
intersected with view rights. Absent grants deny access. Read permission on the
key and
all ancestors is required for lookup; create checks parent Create, delete checks
key Delete, and ACL replacement checks ChangeAcl. Delete refuses keys with subkeys
and root. ACLs can lock out every caller; provisioning must retain a recovery
principal if that is needed.

Eight watches have eight change slots each, bound to their registering Access;
subtree matching is component-aware. Each change contains a key path, value
name (empty for the default value), and a kind distinguishing key/value/ACL
changes. Only committed mutations enqueue changes. Key deletion names the
deleted key; authorization is captured before deletion and its parent is
rechecked on delivery. Poll rechecks traversal/read permissions;
ACL changes discard all pending queues and report rescan-required only to
still-authorized watchers, followed by the ACL change if it matches. Overflow
discards the queue and reports
rescan-required; no event history is promised. Watches are ephemeral and handles
are monotonically issued, never recycled (exhaustion is reported).

The caller places the database, snapshots, values and output paths in static
storage or an injected arena. No heap, exceptions, RTTI, recursion, unbounded
loops or large automatic buffers are used. All operations have bounds derived
from the constants in the public header. The database retains two snapshots
and an encoded slot body as scratch; failed validation leaves caller outputs
unchanged. Instances are not internally synchronized.

## Persistent format version 2

All integers are little-endian; C++ object layout is never persisted. The
injected `Storage` supplies exact byte-range Read/Write, Flush and capacity.
Reads must report unavailable/truncated bytes as failure. Write may tear on
failure/crash; successful Flush guarantees durability of all prior writes,
with ordering, and writes cannot damage bytes outside their supplied range.
Two disjoint 32,768-byte slots require 65,536 bytes. The unused gap before
each seal is ignored. CRC32C detects accidental torn writes; it is not an
authentication mechanism and cannot promise detection of checksum collisions
or media corruption outside the newly written slot.

Each slot body is a 32-byte header, 32 fixed 167-byte key records and 64
fixed 327-byte named-value records (value table begins at offset 5,376):

| Offset | Header field |
| --- | --- |
| 0 | eight bytes `ASTSET02` |
| 8 | u32 version = 2 |
| 12 | u32 body bytes = 26,304 |
| 16 | u64 nonzero generation (no wrap) |
| 24 | u32 slot index (0 or 1) |
| 28 | u32 CRC32C of body, with these four bytes zero |

Each key record starts with used u8, parent index u8, name length u8, ACL
count u8 and four reserved zero bytes. Then follow 63 name bytes and eight
12-byte ACL records: principal u64, rights u8, deny u8, inherit u8, reserved
zero u8. Root is used record zero, parent zero, empty name. Other used records
must have a used parent, unique sibling name and an acyclic bounded chain to
root.

Each named-value record starts with used u8, containing key index u8, name
length u8, value type u8 (None=0, Integer=1, Boolean=2, String=3, Binary=4,
StringList=5), value length u16 and two reserved zero bytes. Then follow 63
name bytes and 256 data bytes. The containing key must exist; value names
must be unique within a key (including the empty default name), and no key
may exceed eight values. All unused records, entries and tails are zero.
Values, ACL bits, UTF-8 and canonical zeros are validated even with valid
checksums. Derived sizes and table offsets have one source in the public
header with static assertions.

The 32-byte seal at slot offset 32,736 contains eight bytes `ASTDONE2`,
u64 generation, u32 body CRC, u32 slot index, u32 CRC32C of its first 24
bytes and u32 reserved zero. A snapshot is committed only when both checksums,
slot identity, generation and semantic validation succeed.

Commit chooses the inactive slot, writes a zero seal and flushes, writes the
body and flushes, then writes the seal and flushes. The previous active slot
is never written. Open validates both slots and chooses the greatest valid
generation; duplicate generations are rejected. A read failure invalidates
that slot; both invalid returns Corrupt, without changing storage. A failed
write/flush returns IoError and closes the instance: the caller must reopen
because the last seal may already be durable. Success publishes memory and
notifications only after the final flush. Format requires the whole reserved
region to be readable and zero, then commits generation one with an explicit
root ACL. No multi-operation transactions, compaction, migration, repair,
cryptographic authenticity, group expansion, ACL inheritance blocking or
unbounded enumeration is included.

Version 1 media is rejected; there is no implicit migration. Service integration
and migration support remain outside this library.

## Verification

Host tests include seeded comparison with an independent hierarchy/value/ACL
model, ACL evaluation tables, bounds and queue/revocation cases, and recovery
from torn writes, truncation, corruption and failures at every write/flush
boundary. The parser fuzz harness supplies valid snapshots and repairs CRCs
after mutation to reach semantic validation. Target compile and stack gates
cover AMD64 and IA-64.
