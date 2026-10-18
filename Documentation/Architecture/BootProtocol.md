<!-- SPDX-License-Identifier: Apache-2.0 -->

# Loader-to-Kernel Boot Protocol

The [kernel image contract](KernelElf.md) defines the boundary. The loader and kernel are independently linked. The loader CRC-checks the boot-volume
`KERNEL.ELF`, validates and places it, then calls the [kernel entry ABI](KernelElf.md). Neither kernel entry receives a UEFI system table, boot-services
table, image handle, console protocol or firmware callback.
`Kernel/Boot/KernelEntry.cpp` is the target entry/ABI adapter; its architecture
selection and native interfaces stay outside the generic kernel owners.

## Envelope and version policy

`BootInfo` version **20** is a little-endian, eight-byte-aligned record with
signature `0x4F464E4952545341`. Both ISA profiles use the same representation.
All addresses are 64-bit identity physical addresses, all page counts use
4 KiB pages, and externally visible flags have fixed-width integer encodings.
The source contracts are `Boot/BootInfo.hpp`, `Boot/BootKernelInput.hpp` and
`Boot/BootFiles.hpp`; their layout assertions are part of the ABI.

The first sixteen bytes carry the signature, 32-bit version and 32-bit total
size. Version 20 requires exactly **106,552 bytes**. A consumer rejects an
unknown version, a different size, nonzero envelope reserved fields or a bad
CRC before importing the record or following any carried address. There is
no implicit compatibility with an older prefix. Any layout, interpretation,
required-field or ownership change increments the version; a future consumer
may support multiple versions only through separately validated decoders.
Loader and kernel updates therefore ship together. The record carries validated
module receipts, trust policy, DMA exclusions, runtime metadata, watchdog
configuration and ACPI power facts. Runtime firmware pointers are not carried.

The reflected Castagnoli CRC-32C uses polynomial `0x82F63B78`, initial state
`0xFFFFFFFF` and final complement. It covers every byte of the size-prefixed
record, including unused array entries and padding, with the four checksum
bytes treated as zero. The loader zeroes the entire candidate before filling
it, then seals it. This is an integrity check; input parsers and architecture
planners still validate extents, values and memory provenance.
Before forming any file view, the kernel validates required init/user receipts,
the optional spinner, ordered non-overlapping whole-file ranges, module bounds,
and containment of the entire allocation in write-back, non-runtime LoaderData.

| Byte offset | Field | Contract |
| --- | --- | --- |
| 0 | Signature | Fixed 64-bit signature |
| 8 | Version | 20 |
| 12 | SizeBytes | 106,552 |
| 16–39 | Memory-map metadata | Count, descriptor version/stride, reserved word, firmware map byte size |
| 40 | MemoryRegions | At most 2,048 sorted, non-overlapping, 40-byte region records |
| 81,960 | Acpi | Validated topology, ACPI definition-block receipts and controller/resource facts |
| 87,976 | Entropy | Published boot seed and credited sources |
| 88,016 | Display | Framebuffer extent, dimensions, stride and pixel format |
| 88,048 | Inventory | Loader inventory location; the kernel retargets its own retained copy |
| 88,064 | Files | Retained LoaderData region, manifest, file and module receipts |
| 89,128 | Crc32c | Four checksum bytes |
| 89,132 | ReservedAbi | Zero |
| 89,136 | Kernel | 576-byte architecture input record |
| 89,712 | InventoryRecord | Complete DeviceInventory v3 payload, included in the CRC |
| 104,520 | DebugPort | 16-byte diagnostic UART record ([Debug port](#debug-port)) |
| 104,536 | Trust | `BootTrustPolicy`: count and eight 48-byte `BOOT.CFG` trust entries, each with its module index and the loader's digest verdict |
| 104,928 | DmaExclusions | Count, declared count, and up to 64 24-byte RMRR/IVMD ranges |
| 106,472 | LowMemoryBlocks | `lowmemory` in 2 MiB blocks below 4 GiB; zero selects the allocator default |
| 106,480 | WatchdogSeconds | Watchdog interval from `BOOT.CFG`; zero disables monitoring |
| 106,488 | Power | 64-byte PM1a/PM1b/reset GAS facts, ACPI ownership command and known `_S5` types; no runtime firmware pointers |

## Architecture inputs

Offsets below are relative to `BootInfo::Kernel`. A zero optional PCI BAR
means absent. Section purposes are 0 code, 1 read-only data and 2 writable
data; no section combines write and execute. The section array carries seven
AMD64 ranges or six IA-64 ranges. Placement value 2 selects the note-derived ELF geometry; values 0 and 1
retain legacy firmware/host and aligned-copy characterizations. The kernel
never parses firmware protocols.

| Offset | Input | Producer and consumer |
| --- | --- | --- |
| 0 | Image | Base, byte size, alignment, placement, section count and bounded permission ranges; kernel builds its initial mapping plan |
| 208 | Bootstrap | AMD64 reserved stack/table pages; IA-64 bootstrap prefix of its runtime reservation |
| 240 | RuntimePool | AMD64 preallocated pool below 4 GiB |
| 272 | Startup | AMD64 two startup pages below 1 MiB |
| 304 | Ia64Runtime | IA-64 aligned runtime page selected during preparation, with adjacent guard ownership |
| 336, 360, 384 | Ahci, Device, Nic | Captured PCI location, BAR index, physical base and extent |
| 408 | Sal | Validated SAL system-table facts, including PAL/SAL entries, GP, rendezvous and firmware ITR facts |
| 488 | Paging | AMD64 physical width and required paging features; flags are 0/1, with the capture status |
| 504 | RsdpPhysicalAddress | Retained RSDP location; ACPI topology was parsed against the final map |
| 512 | TimerTicksPerMicrosecond | IA-64 ITC rate from firmware stall; AMD64 calibrated TSC rate returned by preparation |
| 520 | EvidencePhysicalAddress | Kernel-owned evidence record; loader publishes its address, kernel initializes it |
| 528 | Diagnostics | AMD64 debug-port and serial-port selection, copied into kernel diagnostic state |
| 536 | Architecture | 1 AMD64, 2 IA-64; following reserved word is zero |
| 544 | MetadataArena | 32-byte AMD64 LoaderData receipt; all zero on IA-64 |

The memory map is the authority for reservation provenance. AMD64 allocation
receipts carry base/count before the kernel binds their final write-back,
non-runtime LoaderData attributes. The IA-64 ELF note declares the preferred runtime reservation; the loader
allocates it and its guards before the transaction. Legacy geometry selects
a reservation using the architected minimum insertable page sizes. It owns only
allocator metadata before ExitBootServices; the reserved pages are untouched.
The returned receipt also describes the bootstrap prefix. Its lower alignment
slack (one runtime-page size) and upper 4 KiB guard remain allocated and
unmapped. Every retry must retain that entire write-back extent: LoaderData for ELF
placement, Conventional for the legacy allocator.
After exit, PAL verifies that the selected page size is insertable before the
kernel builds or installs translations. SAL and PAL remain processor-firmware
interfaces, never UEFI boot services.

## Metadata arena and firmware DMA exclusions

Before final-map preparation the loader queries `MetadataArenaPages` with the
last map's managed span rounded up to GiB. AMD64 answers whole 2 MiB blocks
covering the roots, stacks, slabs and bitmap budget, capped at 4,096 pages;
IA-64 answers zero. The loader reserves 2 MiB-aligned LoaderData below
`Abi::Amd64FixedUserRangesStart` (1 GiB). The kernel checks whole-block size,
minimum layout size, the cap, a single covering write-back non-runtime
LoaderData descriptor, and disjointness from the kernel image, other
reservations, files, inventory and fixed user ranges. Firmware may merge
adjacent allocations into that one descriptor. Every root's seed maps the
arena supervisor RW/NX, leaving thread-stack guards unmapped.

The exclusion list is independent of the eight routing reservations retained
by the IOMMU topology. It counts every framed RMRR and IVMD, including malformed
and inert declarations. It retains each nonempty nonwrapping range in table
order up to 64, marking declarations the routing parser drops as malformed.
RMRR bounds are inclusive firmware bytes; IVMD start/length is rounded outward
to pages, as in the IVRS parser. The physical-page importer excludes every page that overlaps either kind.
The envelope checks kinds, bounds, reserved fields and unused entries; kernel
preparation separately refuses `DeclaredCount > 64` with
`ASTERIA:BOOT-INFO:FAIL:DMA-EXCLUSIONS`. Malformed framing retains only preceding
framed declarations. `lowmemory` accepts decimal 1–2,048, refuses duplicates,
and is zero when absent. The physical allocator enforces allocation bounds.

## Debug port

`BootInfo::DebugPort` (`Boot/BootDebugPort.hpp`) names the 16550-compatible
UART the IA-64 diagnostic writer uses; AMD64 leaves it zero and keeps
`Kernel.Diagnostics`. Its fields are the 64-bit `Address` (a physical address
or an I/O port), the 32-bit `BaudRate` the firmware configured (zero when the
table does not say; the kernel never reprograms the UART), the ACPI generic
address space ID (0 system memory, 1 system I/O), `AccessBytes` (1),
`RegisterStrideBytes` (1, 2, 4 or 8) and `Source`, the table that named it:

| Source | Table | Meaning |
| --- | --- | --- |
| 0 | — | No usable UART: diagnostics are dropped; the record is all zero |
| 1 | DBG2 | Microsoft Debug Port Table 2: the first serial device (port type 0x8000) of subtype 0x0000, 0x0001 or 0x0012 |
| 2 | DBGP | Microsoft Debug Port Table: interface type 0 or 1 |
| 3 | HCDP | DIG64 HCDP 1.0a / PCDP 2.0: the first UART descriptor of type 1 (debug port) |
| 4 | SPCR | Microsoft Serial Port Console Redirection table: interface type 0 or 1 (revision 1), or a DBG2 16550 subtype; the firmware console |
| 5 | HCDP | The first UART descriptor of type 0 (console): the firmware console |
| 6 | — | No table named a usable UART: the platform's debug UART (itanium2-vpc's at physical 0x47F0001000) |

Before its first diagnostic, the IA-64 loader reads these tables through the
RSDP's XSDT (or RSDT) and, for HCDP, also through the EFI configuration table
the DIG64 specification defines, validating each signature, bounded length and
checksum, and takes the first source in the order above that names a UART in
system memory or I/O space with byte access, a register stride of 1, 2, 4 or 8
bytes (DBG2 subtype 0x0012 and SPCR revision 2+ type 0x12 take it from the
generic address's bit width; the others are byte registers one byte apart) and
its eight registers below 2^50 or within the 16-bit I/O space. Dedicated debug
ports come before the firmware console. An I/O port is reached through the
firmware's single `EfiMemoryMappedIOPortSpace` window in the sparse encoding of
SDM rev. 2.3 Vol. 2 Part I 10.7; a port without such a window is recorded as
source 0. The loader writes its own diagnostics there from then on, except the
lines it also writes to the firmware console while boot services run, where the
UART is that console (sources 4 and 5): the firmware transmits those once
already. The kernel validates the record (zero for source 0, a known source,
the documented address, access and stride bounds) in both entries, resolves it
against the final map's port space window and directs every diagnostic of every
processor to it; until then it writes the platform UART. A UART that never
accepted a byte within a bounded wait (one million status polls) is treated as
absent and dropped for the boot; once a byte got through, a timeout drops only
the rest of that message. The pre-exit kernel preparation writes only failure
diagnostics, which may then share the console UART with the firmware.

## Transaction and ownership

For each final-map candidate the loader normalizes the map, parses ACPI, copies
entropy, display, inventory and file receipts, fills the architecture inputs
and seals the record. It calls `PrepareKernel(BootInfo&)` at the existing
transaction preparation point, before ExitBootServices. The kernel validates
the envelope and copies all retained values into its own storage. Preparation
uses native processor/device adapters and bounded preallocated memory; it
calls no UEFI service and changes no firmware allocation. It returns the
IA-64 reservation or AMD64 TSC rate through the caller-owned receipt and
reseals it. A rejected first candidate still reaches the firmware console.
After an ExitBootServices attempt only map retrieval and exit retry remain
firmware operations.

`EnterKernel(const BootInfo&)` revalidates and reimports the final receipt
after successful exit. It keeps no pointer to the loader's BootInfo or
inventory. The kernel owns a page-aligned inventory copy, adds native disk
and device facts there, and publishes that copy read-only to the pager.
The file/module region, ACPI tables and processor-firmware extents remain
referenced by integer receipts and retain their documented lifetimes; module
pages are never reclaimed. Plans, page tables, execution-storage views and
runtime owners are derived kernel state, not loader-written kernel globals.

AMD64 xHCI publication uses the retained ACPI MCFG windows, final memory map
and paging capabilities to enumerate PCI functions and probe assignable BAR
extents natively. These inputs, the inventory PCI facts and the timer scale
all cross this ABI. The kernel owns the four-controller publication array,
marks only successfully published functions in its inventory, and skips
controllers whose BAR or publication fails. Function/region handles in pager
words 51–66 are derived kernel capabilities; word 67 receives the kernel's
calibrated timer scale. IA-64 supplies its carried ITC scale there and has no
xHCI publication. Neither path needs a loader-owned controller or handle array.

The loader obtains the evidence storage address through a
read-only kernel query. It uses independent loader console/diagnostic state;
kernel evidence and diagnostics are initialized only by kernel entries.
Read-only preparation failure queries let the loader retain its detailed
console diagnostics through the placed ELF entry descriptions. The temporary
ELF file allocation ends before final-map preparation. After successful exit,
the loader clears the system table's expired services; the unavailable-services
VM case checks its physical bytes before accepting kernel completion. No
firmware-table pointer is present in BootInfo.

## Verification

`KernelLoaderSymbolBoundary` checks the undefined symbols of every kernel
link unit in firmware, O0 and O2 builds against loader definitions and UEFI
application symbols. It requires both kernel entries and rejects injected
loader/private-storage references. Existing stack, closure, image relocation
and IA-64 residue checks remain enabled. Hosted envelope tests cover unknown
versions, sizes, counts, reserved bits and corruption across every handoff
group; a deterministic fuzz harness mutates the envelope and body while
checking that validation never changes the input. It also reseals malformed
file receipts, exercising the separate extent/provenance checks without
dereferencing their addresses.
