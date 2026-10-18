<!-- SPDX-License-Identifier: Apache-2.0 -->

# Boot device inventory

The UEFI application produces a best-effort inventory before ExitBootServices on
AMD64 and IA-64. `Boot/Uefi/BootReport`, `TopologyReport` and `PeripheralReport`
use firmware protocols; `Inventory/ReportText` owns the shared bounded formatter.
PCI root-bridge configuration reads include functions without PCI I/O handles.
This is a snapshot, not device discovery by the kernel or a promise that every
firmware device can be driven by Asteria.

The firmware report is printed through ConOut and overwrites
`\EFI\ASTERIA\REPORT.TXT` on the loaded image's DeviceHandle filesystem. Missing
directories are created. A previous report is deleted before creation, so a
shorter report has no stale suffix. ASCII sanitization, CRLF and a 64 KiB limit
make the file usable without a serial port. Failure prints one status line and
continues; no firmware file handle or buffer survives ExitBootServices.

The report contains firmware identity, GOP mode and framebuffer, AMD64 CPUID
identity/features and firmware APIC mode, conventional-memory bytes, descriptor
count and highest address, ACPI root/table OEM identities, MADT processor modes,
DMAR/RMRR, IVRS/IVHD/IVMD and 8042 summaries, PCI functions, UEFI block media/model strings and
USB descriptors including HID boot keyboard/mouse interfaces. IA-64 reports its
ISA instead of AMD64 CPUID features. Enumeration is bounded at 4096 handles.
The shared ACPI parser and AMD64 VT-d/AMD-Vi topology resolvers supply the routing
summary, including display-only units left disabled, RMRR scopes, IVMD requester
ranges and the same AMD-Vi backend priority, alias and reservation refusals.
On AMD64 each EHCI an RMRR reserves gets an `EHCI-RMRR` block (power state,
command word, BAR 0, capability and operational registers, USBLEGSUP and
USBLEGCTLSTS through EECP, the 6-series SPECIAL_SMI register, the RMRR's UEFI
memory types) and the report one `PCH SMI_EN` line, all read only
([driver isolation](IommuDomain.md)).
The firmware-handoff APIC mode remains separate from the kernel x2APIC mode. Tables are
read only within mapped firmware memory ranges. A malformed/missing mandatory
ACPI table can leave the topology summary unavailable without making reporting
fatal.

| PCI support | Behavior |
| --- | --- |
| ahci driver | AMD64 AHCI capture succeeds for that function |
| e1000e NIC driver | Capture selects 8086:10d3; on IA-64 as edu's: ECAM capture pending before exit, then the kernel's capture result |
| edu demo driver | Capture selects 1234:11e8; IA-64 root-only functions report ECAM capture pending before exit, then record the actual capture result |
| display broker | Display-class BAR contains the chosen GOP framebuffer |
| xhci driver | The kernel published an xHCI into the pager's xHCI table |
| usb host driver | The kernel published an EHCI, OHCI or UHCI into the pager's USB host table ([driver](UsbHostDriver.md)) |
| USB host controller, no driver published | UHCI, OHCI, EHCI or xHCI class/interface the kernel did not publish |
| bridge | Uncaptured PCI bridge-class function |
| unsupported | Remaining functions |

Capture repeats the platform's existing firmware capture rules; in particular,
multiple matching controllers or unsupported BAR descriptions do not imply a
captured driver. IA-64 has an AHCI boot diagnostic but no user AHCI driver.
A displayed driver name means a component/capture path exists, not successful
initialization on arbitrary hardware. The display broker drives GOP memory,
not the GPU's acceleration interface.

The report also includes raw FADT revision/IAPC_BOOT_ARCH and each bounded DSDT/SSDT's address, length,
OEM table ID, revision and checksum verdict. [ACPI Bootstrap](AcpiBootstrap.md#definition-blocks-and-table-dump)
describes the 64-SSDT retention bound and independent dump to `\EFI\ASTERIA\ACPI\*.AML`, written after the first
REPORT.TXT and followed by a report append with non-fatal I/O outcomes. The boot application then
attempts [AML evaluation and ACPI.TXT publication](AmlBootFacts.md).

## Carriage and publication

[BootInfo](BootProtocol.md) carries the loader location and the
complete `Abi::DeviceInventory` payload (version 3) under the envelope's CRC-32C.
The kernel validates version/count bounds and imports it into its own
page-aligned four-page storage. Native AHCI diagnostics append disk records
and IA-64 resolves pending EDU captures there; the kernel retargets its
retained inventory location to that copy. All writers finish before the
pager is admitted. The loader never updates the kernel's inventory storage.
The record holds up to 256 PCI functions and eight disk model/capacity
records, the FADT 8042 bit, and explicit overflow. Version 3
includes the trusted-driver profile ([DMA isolation](IommuDomain.md)):
each PCI entry's DMA half-word holds its mode (`Trusted` for a
function driven without DMA isolation) and the one-based trust entry that
applies (the record keeps its size, so BootInfo carries it unchanged); the
record is followed in the inventory's pages by a trust block
(`Abi::DeviceInventoryFrame`) that only the kernel writes: its copy of up to
eight `BOOT.CFG` trust entries (selector, driver, module name, digest) and each
one's outcome (applied, unused behind a working IOMMU, no matching function,
digest or driver mismatch, ambiguous, not assignable). The root service prints
`(TRUSTED: no DMA isolation)` after a trusted function's driver; it renders
no trust-block line (its IA-64 test image has no room): the loader's
`ASTERIA:BOOT-TRUST` lines (console, debug channel, REPORT.TXT) and the
kernel's `ASTERIA:DMA-TRUST` lines (debug channel) report the entries and
their outcomes.

Both pager runtimes
mint a read-only RAM Frame with Map/Read authority. The pager start block carries the Frame capability pair at words 46/47 and
page count at word 48 on both ISAs. The IA-64 aggregate staging limit includes the
root inventory text buffer and composition components
([staging bounds](Ia64InitialUserImageOwner.md)). The root service maps it read-only, validates version/count bounds, formats and
prints `# devices:` lines after starting display and before product readiness,
then unmaps it. Both pager links use this product reader. Text remains in
root-service-owned memory for the test filesystem request.

The record resides in the boot image, already covered by a kernel seed mapping.
A trusted read-only bootstrap Frame may alias that seed RAM with matching cache
policy; it cannot alias a runtime mapping or request write/execute permissions.
Ordinary Frames retain the physical overlap checks. This read-only publication
contract also forbids later boot-image writers, although the kernel seed alias
itself remains writable.

## Saving through the filesystem service

Firmware reporting and root console output run independently of all scenarios.
The inventory save unit lives with the filesystem scenario under
`Tests/Guest/Pager/FsDemo.cpp` in the product and test boot profiles test pager. Product init prints the inventory independently of those tests.

On AMD64, after its filesystem scenario, the test root service saves the inventory
as root-directory `DEVICES.TXT`. The existing `PagerManagementDiskWritable`
rule permits emulator disks. For other disks it starts the filesystem service
without format permission, requires FAT/exFAT and opens an existing root file
`ASTERIA.OK` before any inventory create/write. A missing marker or unavailable
volume prints the reason and continues. Other destructive demonstrations remain
skipped on those disks. The same filesystem interface governs saves on either ISA.

The current filesystem protocol writes whole 4096-byte pages and has no
truncate operation. `DEVICES.TXT` therefore contains the console text followed
by ASCII whitespace, and overwriting a longer file clears every old page.
The report never inserts NUL padding. USB/NVMe media visible only to UEFI can
receive REPORT.TXT when they are the boot volume; saving DEVICES.TXT currently
requires the AMD64 AHCI component path. The marker is explicit consent to the
inventory write only, not to the demonstration or formatting a real disk.

## Verification

Hosted tests cover bounded formatting, support classes and read-only seed alias
permissions. The VM runner compares the report and console function set, IDs
and class/subclass against recursive QMP query-pci results, validates ASCII,
CRLF and the report bound, and reads DEVICES.TXT with mtools in FAT cases. The
FAT32 recipe includes ASTERIA.OK. It retains each case ESP and report. Current
IA-64 test media can be read-only; absent report files are recorded as skipped.

The firmware disk protocol prefix, GUID, and interface identifiers are defined by
[UEFI PI 1.7, volume 5, section 7.3.1, EFI_DISK_INFO_PROTOCOL](https://uefi.org/sites/default/files/resources/PI_Spec_1_7_final_Jan_2019.pdf).
UEFI 2.11 section 13 supplies the block and disk I/O protocols used to enumerate media.
The CPUID/MSR assembly adapters have normalized AMD64 goldens and mutation-tested
properties for RBX preservation, output stores and privileged-read guards.

## AML observations

After the report and raw table dump, the boot application attempts ACPI.TXT
using a temporary page-backed pure evaluator. NOAML bypasses the step; kernel
input and routing policies remain unchanged. See [AML boot facts](AmlBootFacts.md)
for ownership, capacity fallback, timing and file comparison.

A successfully published AMD64 xHCI
function carries the appended `Xhci` owner value and renders `xhci driver`.
Native publication marks the kernel-owned inventory after a successful BAR
capture and capability publication; a skipped controller retains its previous
owner label. The loader's inventory is copied through BootInfo before this.
The pager bootstrap has 68 words: four xHCI function/region entries at 51–66
append after the module table base and size at 49–50; clock scale is word 67.
Scanout, i8042 and inventory retain their original word positions. The inventory
Frame layout and version stay unchanged. Unpublished USB controllers retain
the library-only label. See [the component](XhciDriver.md).
