<!-- SPDX-License-Identifier: Apache-2.0 -->

# ACPI Bootstrap

## Scope

The firmware bootstrap consumes only the ACPI data needed to establish the first processor and interrupt-controller
topology and to retain optional PCI Express MCFG allocations and the optional DMAR's DMA-remapping units. It records definition-block metadata and dumps firmware SDTs before exit. It does not
execute AML, retain arbitrary firmware pointers, access PCI configuration MMIO, discover devices through AML, or implement power policy. Those jobs belong
to later isolated services and platform adapters.

UEFI configuration tables are scanned once before the final memory-map transaction. The ACPI 2.0 GUID takes
precedence over the ACPI 1.0 GUID. Duplicate entries for the same GUID must identify the same non-null RSDP. The
selected pointer is otherwise inert until `BootTransition` has normalized a candidate final map.

## Validation and normalization

For every `ExitBootServices` attempt, the parser requires no byte of each complete RSDP, selected RSDT/XSDT, and MADT
extent to lie in memory that the candidate map marks reusable: conventional, loader, boot-services, persistent or
unaccepted memory, which the kernel or the firmware may still hand out or rewrite (`TableInReusableMemory`). Real
firmware places the RSDP and tables not only in `AcpiReclaimable` or `AcpiNonVolatile` memory but in reserved or
runtime-services memory or in ranges the map omits, such as the legacy BIOS area at 0xE0000-0xFFFFF; all of these are
accepted, as are MMIO, unusable and PAL ranges, since the kernel never reuses them and the firmware's identity map
makes every address readable before exit. An MCFG or DMAR extent must meet the same table-memory rule before it can be
published. The parser validates RSDP and SDT signatures, declared
extents, checksums, bounded root-entry counts, exactly one MADT, and bounded table entries before its first output
write. It publishes MCFG data only when exactly one structurally valid table is present; absent, duplicate, or
malformed MCFG input produces an empty MCFG subrecord without rejecting the mandatory processor/interrupt topology,
and the DMAR follows the same rule.
XSDT is preferred when an ACPI 2.0-or-later RSDP supplies one; RSDT is the fallback. All table access occurs while
UEFI's pre-virtual-map identity addressing is still in force.

The normalized record contains physical addresses only as inert numeric values and contains no table pointers. It
retains at most four usable (enabled or online-capable) processors in firmware order, eight I/O interrupt controllers,
and sixteen ISA source overrides. Four processors is the supported maximum, not a firmware error: enabled processors
take precedence, so the record holds the first four enabled processors and, in the slots none of them needs, the first
online-capable ones. Every further usable processor is still validated and then counted in `IgnoredProcessorCount`
instead of rejecting the topology; the boot application reports a nonzero count after exit as
`ASTERIA:ACPI-PROCESSORS:IGNORED:0x<count>`. ACPI asks firmware to list the boot processor first; a firmware that
lists it after four other enabled processors leaves it out of the record, and the kernel's boot-processor checks then
stop the boot. At least one processor must be enabled and at least one target-appropriate I/O controller must exist.
Processor flags, reserved encodings, duplicate identities among the retained processors and between an ignored and a
retained one, duplicate controller IDs/GSI bases, interrupt polarity/trigger encodings, and controller and override
capacity are checked before publication.

AMD64 admits local APIC and x2APIC processor records with I/O APIC controllers. IA-64 admits both extended Local
SAPIC records and the legacy 12-byte ACPI 2.0 form, deriving the legacy record's ACPI UID from `ProcessorId`, plus
I/O SAPIC controllers. Lengths 13 through 15 and malformed reserved fields are rejected. When IA-64 firmware supplies
both an I/O APIC and I/O SAPIC with the same
controller ID, the I/O SAPIC record replaces the legacy record. A local-controller-address override replaces the
MADT header address for either target.

An MCFG, when present, must contain one through eight 16-byte allocations. Each allocation has zero reserved fields,
a 1 MiB-aligned bus-zero base, an ordered inclusive bus range, and a non-wrapping physical aperture. Segment/bus
overlap and physical aperture aliasing are rejected. Accepted allocations are sorted by segment and bus before they
are copied into the pointer-free topology. Bus numbers are scoped to a segment group, so distinct segments may each
advertise all 256 bus numbers. MCFG absence remains valid for firmware compatibility.

A DMAR, when present, must have its remapping structures tile the table. Every DMA-remapping hardware unit (DRHD)
needs a nonzero, page-aligned register base, and its device scopes must tile the rest of the unit, each with at least
one whole (device, function) hop. The record keeps the host address width in bits and the DMAR flags. Every DRHD is
counted, and the first four are kept in firmware order with their register base, segment, flags and size field. Every
device scope of a kept unit is counted, and the first 32 are kept with their unit, type, start bus, hop count and
up to eight complete path entries. Every RMRR is counted; the first eight retain their inclusive whole-page ranges
and segments, and the first 32 RMRR scopes retain their paths separately. Malformed RMRRs with a trustworthy enclosing length are skipped and counted.
Malformed DRHDs or enclosing lengths discard the optional DMAR. Lengths exceeding retained capacity remain visible so the kernel refuses truncation.
ATSR, RHSA, ANDD and unknown structures are skipped by length. The kernel resolves PCI paths through ECAM before its
bootstrap aperture closes; explicit DRHD scopes take precedence over INCLUDE_PCI_ALL. Assignment, display and
firmware-domain policy is [driver isolation](IommuDomain.md).

MCFG normalization does not require the ECAM aperture to appear in the UEFI memory map and does not interpret cache
capability bits as an installed mapping policy. The later ECAM topology adapter translates the ACPI record into its
generic region contract by checking `BaseAddress + (FirstBus << 20)`. IA-64 then creates boot-lifetime uncached
`DeviceRegion` authorities before its architecture-specific backend accesses configuration space.

## Handoff and lifetime

The topology retains FADT 8042 fields, calibration counters, IVRS, IOMMU handoff
records and definition-block metadata. Its current wire layout is defined by
the [boot protocol](BootProtocol.md).

Normalization clears the complete topology field on every retry, the parser republishes it from the matching candidate
map, and a failed mandatory ACPI parse prevents `ExitBootServices`. Invalid optional MCFG or DMAR data cannot block that
transition. After a successful exit, later kernel code consumes the copied
topology rather than the reclaimable firmware tables. Reclaiming ACPI
memory and creating CPU/IRQ or PCI authorities remain separate ownership transitions.

The DMAR topology carries a count of
malformed RMRRs skipped by validated length. Absent final-hop scopes are counted
when ECAM resolves the boot plan; broken intermediate bridges remain refusals.
RMRR reservations survive unit-selection failure. Graphics stolen memory may
span unreported memory-map gaps, but cannot overlap allocatable RAM. See
[driver isolation](IommuDomain.md) for bridge/SMM preservation, firmware-state takeover and per-unit evidence.

The optional calibration fields retain FADT X_PM_TMR_BLK when its GAS has a
usable 32-bit memory or I/O register, otherwise PM_TMR_BLK with length four.
TMR_VAL_EXT selects 24 versus 32 counter bits. Hardware-reduced FADTs omit
legacy timers. A unique checksummed HPET table retains its page-aligned memory
GAS. Duplicate, short, malformed or absent optional timer tables leave zero
addresses, without rejecting mandatory topology. Parser tests cover these
boundaries and legacy ACPI 1.0 PM timer fallback. The topology also retains
IVRS and bounded IOMMU handoff records.

On AMD64 the capture request supplies the executing BSP's full APIC ID.
The bounded processor list always retains its enabled entry, replacing the
last non-BSP retained entry when necessary and preserving firmware order.
A type-9 entry below ID 255 is ignored if a type-0 entry describes the same
ID, regardless of which entry appears first. Both entries still have to pass
framing and flag validation. IA-64 retains its existing selection policy.

## Definition blocks and table dump

[AML boot facts](AmlBootFacts.md) records one DSDT and the first 64 SSDT root entries in firmware
order (`MaximumAcpiSsdts`); `SsdtOverflowCount` counts further bounded SSDT entries. Records retain physical
address, length, four-byte signature, eight-byte OEM table ID, revision and a byte-valued checksum verdict.
A unique checksummed FADT selects X_DSDT at offset 140 when its length is at least 148 and the pointer is nonzero;
otherwise it selects the 32-bit DSDT at offset 40. An invalid extended pointer does not fall back to the legacy
pointer. DSDT/SSDT lengths must be 36 bytes through 1 MiB, and their full extents must pass the same non-reusable
memory check as other SDTs. Bad checksums are retained as diagnostic metadata; short, oversized or inaccessible
extents are skipped. Optional definition-block failures never reject the mandatory topology. Raw FADT revision
and IAPC_BOOT_ARCH are separate from the existing revision-3-or-later input-policy fields.

The report snapshot and the authoritative parse in each ExitBootServices attempt call the same parser. Only the
report path performs file I/O. After first saving REPORT.TXT it creates `\EFI\ASTERIA\ACPI` on the loaded volume,
dumps every checksummed SDT in the root walk, the root itself and FADT-selected DSDTs, then appends to REPORT.TXT with
the outcome. The dump does not inherit the 64-SSDT retention limit. RSDP and FACS are excluded: neither uses the
SDT header/checksum format. Files are byte-for-byte firmware data, with no padding or AML evaluation.

Unique signatures use `<signature>.AML`; repeated signatures use `<signature>1.AML`, `<signature>2.AML`, and so
on in visit order, including the first occurrence. Names remain FAT 8.3 through the root-walk bound; legal punctuation
such as `ASF!` is preserved. Existing same-name files are deleted before creation, preventing stale suffixes.
Files for signatures or ordinals absent on a later boot are not pruned. Writes are at most 64 KiB, retry nonzero
short writes, reject zero/oversized writes and errors, then flush and close. Only 8,208 bytes of static signature
counts are added, with no table-sized arena. Outcomes include failure counts and the first sixteen failed signatures
in REPORT.TXT and its ConOut copy. Missing volumes and other I/O failures remain non-fatal; an unwritable report
volume can only receive the console diagnostic. The IA-64 VM's read-only volume exercises this path.

Hosted tests cover pointer selection and optional failures on both ISAs, RSDT fallback, SSDT overflow, bad checksums,
truncated extents, file naming, chunk boundaries, short writes, overwrite and protocol failures. The parser fuzz
harness checks metadata against source bytes. The AMD64 VM runner requires DSDT/FACP/APIC and compares all dumps
with header metadata, checksums and independent QMP physical reads; QMP has no portable ACPI-table query.
