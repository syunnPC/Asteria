<!-- SPDX-License-Identifier: Apache-2.0 -->

# Kernel ELF and Entry ABI

The kernel image contract separates the UEFI application
from `Kernel.elf`. The loader composes file/protocol adapters, reports, ACPI
parsing, entropy capture and the ExitBootServices transaction. The kernel
composes native execution, memory, interrupt and capability owners. Pure
runtime, diagnostic and hardware-fact adapters may be linked into both;
their storage is private to each link. Both links are closed. The loader
reads `\EFI\ASTERIA\KERNEL.ELF`, selected by the required `kernel` key in
`BOOT.CFG`, and checks CRC-32C before ELF validation. The installed file is
stripped with `--strip-unneeded`, retaining note, load/section geometry, symbols
needed by RELA, and every relocation record; a build-time invocation of the
production parser validates the actual installed bytes. The full linked ELF
remains an observer artifact. No kernel payload is embedded in the loader.

The loader validates its own firmware image. `image-base=` and
`image-size=` describe the placed kernel, and `kernel-base=` repeats that
base explicitly. `ASTERIA:EVIDENCE:<16 hexadecimal digits>` names kernel-owned
storage obtained through the entry query. Both ISAs report `firmware-image-base=` for the original PE, so relocated
loader presets prove firmware relocation separately from kernel placement. Runner symbol addresses use the kernel link's preferred
base plus the placement delta, including the relocated-loader presets.

## ELF profile and ownership

The profile is ELF64, little-endian, closed ET_EXEC, with explicit section and
program tables and retained RELA records. Preferred addresses are identity
addresses. The loader decodes every field from bytes; it rejects unsupported
ISA/flags, dynamic linking, TLS, REL/RELR, unknown relocation kinds, unresolved
symbols, out-of-file tables, ambiguous or overlapping allocated sections,
non-load program headers and writable executable segments. Limits are 16 MiB
of source, 64 sections, eight program headers and 65,536 relocations. It uses
no heap or hidden state. The source must remain immutable through placement.

`KernelElf::Parse` assigns its 400-byte plan only on complete success. `Place`
reparses the borrowed source and checks destination alignment, size and alias
bounds before any write. It zeroes the declared extent, copies PT_LOAD file
bytes, leaves BSS/padding zero, and applies the validated relocations. Source,
plan and destination must be distinct. Failure leaves every destination byte
unchanged. Validation boundaries prohibit inlining to keep O0/O2 frames within
the 512-byte limit; this is a bounded-storage contract, not a warning exception.

The loader allocates LoaderCode below 4 GiB, retaining exactly the kernel
extent after releasing alignment slack: 2 MiB alignment on AMD64, 16 MiB on
IA-64. Firmware permissions govern physical preparation. Kernel takeover
publishes only note-derived RX, RO/NX and RW/NX mappings; no writable alias
of executable bytes enters an owned address space. IA-64 publishes instruction
coherence after all copies and fixups and before calling any kernel descriptor.

## Reservation note

`.note.asteria` is one read-only 4 KiB SHT_NOTE. Its header has namesz 8,
descsz 360, type `0x41535401`, and the name `ASTERIA\0`. The remaining page
padding is zero. The descriptor is little-endian, field-encoded:

| Descriptor offset | Fields |
| --- | --- |
| 0 | Four u32: entry ABI version (1), BootInfo version (20), ISA (1 AMD64 / 2 IA-64), ownership count |
| 16 | Three u64: preferred base, extent bytes, placement alignment bytes |
| 40 | Three u64: preparation, enter and query entry RVAs |
| 64 | Three u64: GP RVA, descriptor-array RVA and descriptor-array bytes; AMD64 uses zero |
| 88 | Reserved u64 zero |
| 96 | Seven 24-byte ownership records: u64 RVA, u64 4 KiB page count, u32 purpose (0 code / 1 read-only / 2 writable), u32 zero |
| 264 | Three 32-byte reservations: u64 pages, u64 alignment pages, u64 maximum address, u32 kind, u32 flags |

Unused records are all zero. Ownership ranges cover the PT_LOAD extents in
address order with exactly matching permissions. AMD64 retains seven logical
ranges for its initial planner. IA-64 has six ranges: text, the note, read-only
body, read-only tail, data and BSS. They cover three 4 MiB PT_LOAD groups at
base +4/+8/+12 MiB, with RX/RO/RW permissions and a 16 MiB total extent.
The first 4 MiB is an unmapped gap. Descriptor bytes and the GOT belong to
the read-only body.
The production and test residue proofs consume these note ranges and verify
that they agree with the actual PT_LOAD geometry.

Reservation kinds are 0 bootstrap, 1 AMD64 pool, 2 AMD64 startup and 3 IA-64
runtime. AMD64 declares separate bootstrap and pool extents, plus two startup
pages below 1 MiB. The pool stays below 4 GiB. IA-64 declares its preferred
16 MiB runtime body, equally aligned, with flag 2: the allocation also retains
one body-sized lower slack extent and one 4 KiB upper guard. Flag 1 declares
the bootstrap prefix as a subview of that body, with no extra allocation.
The note is generated from kernel constants; the loader copies none of those
constants. Allocations finish before the final-map transaction. Receipts in
BootInfo bind body/prefix to write-back, non-runtime LoaderData. The kernel
checks the declared preferred size and its entire guard ownership against the
final map. PAL verifies insertability after exit. The legacy physical/host
interface retains its Conventional allocator and fallback-size characterizations.

The note header assembly contains only link metadata and section-flag markers.
Compile-check closures omit it because they are generic relocatable closures,
without the final note linker contract. `KernelElfImageContract` checks its
exact final bytes, zero padding, ownership, reservations, entries and relocations,
and rejects mutations on both linked images.

## Relocations and IA-64 descriptors

AMD64 supports R_X86_64_64 (`S+A`) and RELATIVE (`B+A`, with zero symbol),
rebased to the allocated identity address. Resolved PC32/PLT32 displacement
fields are validated against `S+A-P` and preserved under uniform placement.
Only values inside the closed image are admitted.

IA-64 supports DIR64LSB, IMM64, FPTR64LSB and IPLTLSB. IMM64 requires the
MLX template, slot 1 and the movl opcode; the loader writes only the immediate
bit fields across slots 1/2 and preserves unrelated instruction bits. Resolved
PCREL21B, GPREL22 and GPREL64I fields are checked against their linked values
and preserved. Unsupported forms are rejected rather than approximated.

An IA-64 function pointer names a 16-byte pair containing code address and GP.
FPTR64 cells retain descriptor identity, not `S+A` as a substitute pointer.
The loader validates every owned descriptor against kernel text and the note's
GP, then rebases both words, including linker-generated pairs without RELA
records. FPTR64 sites must point to a pair for their symbol/addend; IPLTLSB
sites contain and relocate a code/GP pair. Effective fixups must be ordered,
non-overlapping and outside the owned descriptor array. SAL rendezvous getters
are kernel code, so registration names placed text and the placed GP.

References: [ELF gABI](https://gabi.xinuos.com/elf/06-reloc.html),
[AMD64 psABI](https://refspecs.linuxbase.org/elf/x86_64-abi-0.99.pdf),
[Intel Itanium psABI, May 2001](https://refspecs.linuxfoundation.org/elf/IA64-SysV-psABI.pdf),
and project code. Synthetic hosted fixtures encode the published wire and
instruction fields independently. Parser fuzzing checks immutability,
determinism and rejection atomicity under sanitizers.

## Entries and preparation diagnostics

`KernelEntryAbi.hpp` declares three extern-C entries with entry ABI version 1:
`AsteriaKernelPrepare(BootInfo*) -> u32`, `AsteriaKernelEnter(const BootInfo*)`
and `AsteriaKernelQuery(u32 selector, u32 index) -> u64`. AMD64 uses System V;
IA-64 uses P64 descriptor calls. Enter never returns. Prepare returns 1 on
success and 0 on rejection, validates/copies the sealed BootInfo, and calls
no firmware. The loader calls it at the existing transaction preparation
point; console failures remain available before exit. The caller must provide
one aligned, live record for the duration of either entry.

Query is a closed scalar interface for evidence, firmware APIC identity,
preparation/capture/table/mapping failures, timer-source masks and bounded
MTRR facts, and `MetadataArenaPages`. That selector accepts a managed span
in GiB as its index and returns a bounded 2 MiB-rounded page count on AMD64
(zero on IA-64), using pure layout arithmetic. Other unknown selectors/indices
return zero. BootInfo v20 carries 576 bytes of architecture inputs, including
the arena receipt at input offset 544. The arena is a separate queried
allocation; the ELF note still declares only its three reservations. Diagnostic string results
are identity addresses; the loader accepts only a terminating string of at
most 511 bytes inside a placed read-only ownership range. Queries allocate
nothing and mutate no kernel state. They are single-BSP boot operations.
No kernel storage is written by the loader; source-copy placement and
relocations create the image before any kernel entry runs.

## Kernel file and expired firmware services

The whole-file reader owns a separate LoaderData allocation of at most 16 MiB.
Manifest validation and all file CRC checks run quietly before GOP mode selection,
desktop drawing and the banner. The optional display policy remains loader state;
only the resulting framebuffer mode crosses BootInfo. File rejection is retained
as pointer-free loader text and reported on the selected boot console; failure
return restores the original GOP mode. The watchdog is disabled at entry.
The kernel-file receipt is loader-only storage, copied with the IA-64 loader continuation.
User files and module pages retain their existing BootInfo region. The
kernel file is never mapped by the kernel: placement copies its PT_LOAD bytes,
zeroes BSS and applies validated relocations. `LoadKernelImage` clears its source
borrow on every return, and the application frees the file pages before reports,
reservation allocation and the final-map transaction. Release failure stops
the handoff. A missing, truncated, growing or CRC-mismatched file stops before
placement; CRC rejection names `KERNEL-CRC32C-MISMATCH`. Kernel file size/CRC
facts appear as `ASTERIA:STAGE:BOOT-FILE:KERNEL.ELF...`; retained user/module
receipts alone appear in kernel evidence and REPORT.TXT.

After successful exit, the loader clears the system table's expired boot-services
and console pointers and updates its IEEE CRC32 using project code. It calls no
firmware at this point. This follows the field lifetime in [UEFI 2.10 section
4.3](https://uefi.org/specs/UEFI/2.10/04_EFI_System_Table.html); runtime services
and configuration-table fields are retained. The fact line
`ASTERIA:BOOT-SERVICES:ABSENT system-table=0x<16 hex digits>` names the table
for physical-memory observation, and neither entry receives it. The
`Amd64.BootServicesAbsent` VM case reaches the full guest/runtime completion
and checks every cleared field, table extent and IEEE CRC32 through QMP.
