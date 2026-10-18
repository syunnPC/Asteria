<!-- SPDX-License-Identifier: Apache-2.0 -->

# AMD-Vi

AMD64 selects VT-d for a retained DMAR, AMD-Vi for IVRS otherwise, and no
DMA backend when neither is present. `AmdViDomainBackend` implements the
same `IIommuDomainBackend` contract as VT-d. `AmdViUnit` uses the existing
64-bit MMIO, cache-maintenance and physical-page adapters; their VT-d names
are shared adapter vocabulary, not AMD hardware encodings.

The implementation follows [AMD IOMMU specification 3.09, publication 48882](https://www.amd.com/content/dam/amd/en/documents/processor-tech-docs/specifications/48882_IOMMU.pdf),
sections 2.4 (commands), 2.5 (event log), and 5.2 (IVRS). It uses v1 I/O translation only; ATS,
PASID, guest translation and interrupt remapping are disabled.

## Firmware topology

`Boot/Acpi/Ivrs` decodes a checksummed SDT of 48..65536 bytes field by field.
All block lengths must tile the SDT. IVHD 10h has a 24-byte header; 11h and
40h have 40-byte headers, including the EFR images. Repeated versions with
identical MMIO base, segment and IOMMU BDF describe one unit; the highest
supported type wins. A distinct second unit, nonzero segment, invalid
register range, capacity overflow or malformed record refuses translation
with an explicit selection reason. Storage holds 64 device records and
eight IVMD records; counters retain overflow without indexing past storage.

Select, all-devices, start/end ranges, alias select/range, extended
select/range, IOAPIC and HPET special entries are retained. HID records use
the variable UID length and are skipped with a separate count. Truncated
entries stop the current IVHD; unknown independently framed entries, orphan
or reversed ranges and invalid IVMD ranges are skipped and counted. Such
incomplete topology is diagnosed and never enables DMA. IVMD 20h applies
to all devices, 21h to one, 22h to an inclusive RID range; unity
ranges retain read/write permissions and exclusion flags. Publication 48882 revision 3.09,
section 5.2.2.2, tables 108–110, defines a physical start address and a byte
length, and permits rounding the length up to a 4 KiB boundary. Asteria
conservatively encloses firmware intervals in its 4 KiB mappings by rounding the start down
and the inclusive end up to page boundaries: every requested byte is covered.
Zero lengths and arithmetic overflow are refused. This can map extra bytes within
the boundary pages; the existing memory-reservation checks still apply to those pages.

Shared aliases and their targets cannot receive independent assignment
under the current single-function authority model. They stay blocked.
An IVMD covering an unserved/aliased requester refuses the topology, except a host bridge (class 06h, subclass 00h) outside every IVHD device entry: that is the root complex's own function, not a requester the unit translates, so it takes no IVMD regions.
Alias-group assignment is unsupported by the single-function authority model.

## Storage and commands

The boot coherent reservation contains 517 pages: up to 512 device-table pages,
two command pages, two event pages,
and a completion page. `DevTblSize` exposes two pages per captured bus,
through the highest captured or IVRS-described bus; unused reserved device pages are not
published. A DTE is four 64-bit words. All published RIDs start V=TV=1,
mode zero, root zero and IR=IW=0. The unit's own function is never attached.
Attach publishes domain ID, root, actual domain walk depth and IR/IW,
then invalidates the device entry and domain before returning.

Each user backend has eight reserved pages, a domain ID 1..4, and 4 KiB
leaves. Domain address widths choose one to four levels; intermediate PTEs
encode the next level, present, IR and IW; leaves encode level zero, force-coherent FC and
requested IR/IW. MSI 0xFEE00000..0xFEEFFFFF and HyperTransport
0xFD00000000..0xFFFFFFFFFF IOVA windows are reserved and cannot be mapped. There is no execute permission. Firmware domains use IDs
5..12 and up to 64 pages each, shared by requester IDs with the same IVMD
region mask. Absent PCI requesters are seeded directly by ID. Map preflights table capacity before mutation.
Pages remain reserved until release, so repeated fresh windows can exhaust
capacity without allocation or unsafe reuse.

The command and event rings hold 512 sixteen-byte records each. Boot uses INVALIDATE_IOMMU_ALL when EFR.IASup is set. Otherwise it
invalidates every exposed device entry and interrupt table, then domains
0..12. Changes use INVALIDATE_DEVTAB_ENTRY and INVALIDATE_IOMMU_PAGES
(S=1/PDE=1 for the full domain), followed by COMPLETION_WAIT with a store
to reserved RAM. Map also invalidates negative translations. Unmap and
release wait for architectural completion before RAM authority can go.
One outstanding completion sequence prevents stale stores from satisfying
a later command. All waits are bounded at one million polls; a timeout
makes the unit failed and retains domain resources. Locks follow the
existing backend rank 5 -> unit rank 6 ordering.

Control updates preserve firmware SMI filter fields, apply the IVHD
PassPW/ResPassPW/Isoc flags, and enable translation, coherent walks, commands
and event logging.
Cache lines are conservatively flushed and fenced even on coherent units;
completion/event lines are invalidated before CPU reads. DTE interrupt
valid is clear and IntCtl is pass-through. MSI remains confined by the
existing PCI interrupt-authority programming; this backend provides DMA
isolation and does not add interrupt remapping. Faults are polled, counted
by served requester/domain and retain raw source, event type, address and
read/write direction. Device-caused types 2, 7 and 8 count per requester/domain; only hardware
and command errors 3..6 fail the unit. Section 2.5 requires hardware to finish an
entry before advancing the tail. Software consumes every published slot once,
then advances the head; it neither clears records nor treats a reserved type-zero
record as an unpublished entry. No revision-guide erratum is assumed.
Detach writes a mode-zero blocker even after unit failure and retains resources. Event overflow is
counted. Section 2.5.1 recovery first waits for EventLogRun=0, consumes the
final published tail, disables EventLogEn, clears only EventOverflow (RW1C),
and reenables logging. A stopped-log timeout fails the unit without changing
log ownership registers.

## Emulator evidence and limits

`Amd64.AmdVi` in suite `Amd64AmdVi` uses the standard test firmware, raw
AHCI disk, edu and e1000e. The runner pins `AMDVI-PCI` to 06.0 and connects
`amd-iommu,pci-id=amdvi,dma-remap=on,intremap=off`.

QEMU 10.2 emits 64 faults for edu's one rejected 256-byte write (four-byte
fallback transactions), leaves the event Address field zero, omits DomainID,
and puts write direction in reserved flag bit 49 instead of architectural
RW bit 53. The kernel retains raw counts and addresses and accounts by RID;
only architectural RW bit 53 controls direction diagnostics. The driver demonstration
requires a new fault and unchanged target contents, then proves remapping
works; the host AMD observer requires a fault from the expected requester; the
ordinary guest TAP proves the target was not written. Event counts, addresses
and direction are not fixed to one emulator version.
Nonzero-bus QEMU faults may additionally truncate DeviceID to devfn.
These emulator observations do not establish real-hardware or alias-group behavior.

The [boot protocol](BootProtocol.md) defines the retained IVRS representation;
`Amd64EvidenceRecord` defines backend and topology diagnostics.

## Early handoff and desktop capacity

ACPI retains eight deduplicated, safely framed MMIO bases separately from
translation policy, including rejected checksums/scopes, segments and extra
units. The bootstrap sweeps the complete ECAM aperture and attempts every
retained handoff before IDENTIFY/READ DMA and before reporting a failed
catalog preparation. Unsafe/truncated handoff descriptions stop boot before
kernel DMA. Bridges, display and known firmware-reserved functions stay
protected during the sweep. The original control/status observation survives
subsequent configuration. This early handoff applies to VT-d too.

The PCI catalog, DeviceManager and requester captures hold 256 functions;
capture overflow explicitly refuses either backend. Ryzen ranges reaching
FFFF publish all 512 reserved device-table pages, with every RID blocked.
PCI functions consume registry objects and capability slots, so the object registry
holds 512 entries; the extra 256 cover boot/runtime pools. PCI publications
and temporary device-region capabilities use separate 256-slot spaces. Publication is tested with all 256
functions and the existing boot registry-budget check remains active.
Endpoints behind a conventional/PCIe-to-PCI bridge are never independently
assignable, even without firmware alias entries, for either backend.
