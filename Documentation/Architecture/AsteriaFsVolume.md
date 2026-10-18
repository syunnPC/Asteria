<!-- SPDX-License-Identifier: Apache-2.0 -->

# AsteriaFS v0 Volume Library

The pure library (`Source/User/Filesystem`) of [filesystem protocol](FilesystemProtocol.md): the native
copy-on-write format over a synchronous sector device, without service, IPC, kernel or pager code. The
library has no heap, runtime library containers, data cache, or data checksums. `FsVolume` must have static
storage.
All API functions are `noexcept`; value-returning functions are `[[nodiscard]]`.

## Device and volume API

```cpp
struct FsBlockDevice
{
    void* Context;
    bool (*ReadSectors)(void*, UInt64 sector, UInt32 count, UInt8* buffer) noexcept;
    bool (*WriteSectors)(void*, UInt64 sector, UInt32 count, const UInt8* buffer) noexcept;
};

FsStatus Mount(FsBlockDevice device) noexcept;
FsStatus Format(FsBlockDevice device) noexcept;
FsHandleResult Create(const UInt8* name, Size size) noexcept;
FsHandleResult Lookup(const UInt8* name, Size size) const noexcept;
FsStatResult Stat(FsHandle handle) const noexcept;
FsAllocationResult AllocateData(FsHandle handle, Size index) noexcept;
FsCommitResult Commit(UInt64 sender, UInt64 transaction) noexcept;
FsCommitResult QueryCommit(UInt64 sender, UInt64 transaction) const noexcept;
UInt64 GetGeneration() const noexcept;
const FsSuperblock& GetCommittedSuperblock() const noexcept;
```

`FsHandle` is `{UInt64 ObjectId, UInt64 BirthGeneration}`. Results contain
`Status` and respectively `Handle`, `Object`, `Block`, or `Generation`.
`Stat` returns birth generation, size, kind, and the six direct block numbers.

Mount returns `Blank` on two all-zero superblock sectors, without writing.
Explicit Format rereads both sectors and refuses any nonzero sector, including
an existing valid filesystem. Initial format publishes generation 1 in A;
root object 0 has birth generation 1. Create assigns dense IDs and birth
`VolumeGeneration + 1`. There is no deletion or committed ID reuse in v0.
Names are sorted lexicographically by ASCII bytes. Names obey the full
DirectoryName rules: 1–64 lowercase ASCII letters/digits, with `.`, `-`, `_`
allowed only inside a name.

The caller serializes one staging transaction and controls sender admission
and authorization. The service must await successful writes of **whole** fresh
4096-byte data blocks before calling Commit. AllocateData changes a direct
pointer and extends size to that block's end; absent pointers are sparse holes.
Sub-block sizes and truncation have no mutator in this API. Stat sees
staging, while QueryCommit always reads the committed superblock.

Commit writes a fresh table, a fresh directory, then the other one-sector
superblock, awaiting each callback. The allocator protects both the committed
reachable set and every staging allocation (even superseded staging blocks).
It reserves two fresh blocks for commit metadata. Freed blocks become available
only after successful superblock publication. Any failed commit write fences
all operations until remount: failure can mean a write actually landed. The
caller must quiesce the device before remounting; late writes are outside this
synchronous interface's contract. Ordering covers service crashes; power-loss
durability is unsupported without a flush callback.

Mount checks both candidate superblocks, highest generation first (A wins a
tie), the seeded table CRC against the superblock's TableCrc, the seeded root
directory CRC, object/name ownership, and bitmap equality to the reachable set.
Invalid metadata can fall back to the other valid generation. An I/O read error
returns IoError rather than treating unread data as corrupt or blank. After
subsequent commits reuse freed blocks, the older superblock is not guaranteed
to remain mountable; the currently committed tree remains protected throughout
its successor commit.

## Byte layout

All integers are unsigned little endian. Logical C++ structures are never
cast onto disk buffers; native padding is irrelevant. Every offset and encoded
size has a static assertion in FsFormat.hpp. The
64-bit generation fields at offsets 20/12 are intentionally unaligned and are
accessed only through individual bytes. Reserved bytes are zero and v0
refuses unknown versions, geometry, features, noncanonical padding and names.

The volume is exactly 4096 sectors of 512 bytes, or 512 blocks of 4096 bytes.
Sectors 0–127 (blocks 0–15) are permanently reserved. Superblock A is sector 0,
B is sector 16. Sector 8 (boot token) and sector 64 (block-demo scratch) are
never written. Only blocks 16–511 may be allocated. Reserved bitmap bits are
set: the committed bitmap equals the reserved set **plus** reachable CoW blocks.

Superblock, exactly 512 bytes:

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 8 | Magic `ASTRFS00` |
| 8 | 4 | Version = 0 |
| 12 | 4 | BlockBytes = 4096 |
| 16 | 4 | TotalBlocks = 512 |
| 20 | 8 | VolumeGeneration |
| 28 | 4 | TableBlock |
| 32 | 4 | TableCrc (the table header's CRC) |
| 36 | 4 | Reserved map-block pointer, zero |
| 40 | 8 | NextObjectId |
| 48 | 64 | Committed[4], each `{u64 Sender, u64 Transaction}` |
| 112 | 64 | Committed block bitmap |
| 176 | 32 | CommitGenerations[4], each u64 |
| 208 | 8 | Reserved snapshot root, zero |
| 216 | 8 | Reserved quota, zero |
| 224 | 8 | Reserved features, zero |
| 232 | 16 | Reserved UUID, zero |
| 248 | 260 | Reserved words, zero |
| 508 | 4 | CRC-32C |

Both metadata kinds have this header:

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 8 | Magic `ASTRMET0` |
| 8 | 4 | Kind: ObjectTable = 1, Directory = 2 |
| 12 | 8 | VolumeGeneration |
| 20 | 4 | CRC-32C |
| 24 | 8 | Reserved zero padding |
| 32 | remaining | Entries |

The table has 127 entries of 32 bytes, occupying the rest of the 4096-byte
block. Relative entry offsets:

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 8 | BirthGeneration |
| 8 | 8 | SizeBytes |
| 16 | 4 | Kind: Empty = 0, File = 1, Directory = 2 |
| 20 | 12 | Direct[6], each u16; 0 means absent |

Object 0 is the flat root directory: size 4096, Direct[0] points to its directory
block, remaining pointers zero. Files contain at most six whole blocks; sparse
holes are permitted. All unallocated table entries are zero.

The directory has 50 entries of 80 bytes, followed by 64 zero bytes. Relative
entry offsets:

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 4 | NameLength; 0 means unused |
| 4 | 64 | Name, unused tail zero |
| 68 | 4 | Reserved zero padding |
| 72 | 8 | ObjectId |

Unused entries are completely zero and follow all named entries. Each allocated
file has exactly one directory entry. Reachable blocks have unique owners.

CRC-32C uses reflected polynomial 0x82F63B78, initial/final complement, and a
seedable finalized CRC for incremental hashing. Format checksums hash
`LE_u64(location) || LE_u64(VolumeGeneration) || encoded_bytes`, with the four
CRC bytes logically zero. Location is a **sector** for superblocks and a
**block** for metadata. Metadata decoding requires the expected generation as
an argument. A superblock's generation is authenticated by its own CRC; its
sector is supplied by the caller. TableCrc duplicates the seeded table CRC.

The format codecs require exactly 512/4096 input/output bytes. Object and
directory output arrays must have 127/50 entries respectively. Failed decoding
or encoding leaves its output unchanged. The primitive byte helpers borrow
valid caller storage (ReadLittle/WriteLittle widths 1–8; Checksum's CRC field
must fit the buffer). They never allocate temporary sector/block buffers.

## Commit records and caller requirements

The format holds 127 objects and 50 directory entries per block, including
the header and each entry's 64 DirectoryName bytes.

Generation slots at offset 176 preserve the exact generation for each retained commit, including an older
sender's commit after other commits. The last four pairs are retained globally,
newest first. Replaying a retained pair performs no writes, returns the original
generation, and leaves staging intact. Sender and transaction values may be zero;
generation zero distinguishes an unused record from a committed zero pair.

Callers must satisfy the following contracts:

- Bound transaction query/retry lifetime to the four-record window, or extend the
  format/API: an evicted pair returns NotCommitted even if it once committed.
  Unbounded definite history is impossible with only four records.
- Serialize staging across senders, or add explicit per-sender transaction
  storage/admission before allowing simultaneous transactions. Sender binding
  is a service responsibility, outside this pure API.
- Fence handles from a previous service instance with InstanceTag. A handle for
  a rolled-back uncommitted Create can have its ID and birth tuple reused after
  restart; committed objects are never reused in this API. BirthGeneration
  alone cannot distinguish those uncommitted incarnations across a crash.
- Add a size/truncation mutator if the service needs byte-sized files, and a
  flush callback if it promises durability across power loss.

## Hosted tests

The RAM test device has 4096 sectors and implements fault
injection through normal callbacks, with no production test branches.

- Crc32cTests: standard `123456789`, empty, SCTP zero/one/ascending vectors,
  and every split point of incremental CRC computation.
- FsFormatTests: canonical field round trips, sizes/nulls, invalid magic, CRC,
  location/generation, version/geometry/reserved fields, object kinds and names;
  invalid outputs remain unchanged. Name validation is compared against
  DirectoryName for every byte at every position for lengths 1–65.
- FsVolumeTests: blank-only format, format write crashes, I/O errors, newer
  superblock selection and fallback for damaged superblock/table/directory,
  reachability/bitmap mismatch, create/lookup/stat/stale handles, sparse sixth
  block, CoW allocation and reclamation, query/replay/record eviction, directory
  and allocation exhaustion. An independent image walker checks metadata CRCs
  and exact reachable bitmap. Token and scratch sectors retain sentinels.
- Crash property: three scripted creates and overwrites, five writes per
  transaction (two client data writes, table, directory, superblock). All 16
  boundaries are checked with writes failing before landing, landing but
  reporting failure, or tearing halfway: 48 images. Remount must recover
  exactly a complete committed prefix, including all bytes and QueryCommit.
- Reference workload: 800 deterministic LCG operations compare staged and
  committed handles, pointers, sizes, contents and generation after each step;
  ASTERIA_TEST_SEED replays other seeds with failure seed/step/op diagnostics.
- FsFormatFuzz: every decoder, failure atomicity, canonical reencoding, wrong
  location/generation; raw and CRC-repaired mutated inputs. The common driver
  replays 16 fixtures and 20000 mutations; the same harness supports libFuzzer.
