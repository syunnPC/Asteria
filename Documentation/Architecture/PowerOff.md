<!-- SPDX-License-Identifier: Apache-2.0 -->

# Shutdown and reboot

Shutdown and reboot use [AML boot facts](AmlBootFacts.md) and
[processor parking](ProcessorStops.md). The kernel does not interpret AML.

The root service holds the authority. DeviceControl power operations require
Manage on the exact boot ResourceDomain, whose object identity the kernel
retains; Manage on a child domain is insufficient.
A query reports available transitions before root starts teardown. Components
receive no power authority. The root-owned terminal offers Ctrl+Alt+F10 to shut
down and Ctrl+Alt+F11 to reboot.

Root stops accepting new work, stops application/session components, and asks
each filesystem to flush and close admission with a bounded response wait.
Filesystem v2 reserves method 17 for FsQuiesceMethod on its sealed manager
face, requiring Manage as well as Invoke; the application face cannot
authorize it. The eight payload words contain the incarnation and seven
zeros. Root commits the stopped writer's staging, preserving v2 ownership.
Requests carry the view and slot generations.
The filesystem commits staged metadata/data, flushes the block device, marks
FAT/exFAT clean, flushes again, and answers only after completion. Storage
drivers remain alive until this finishes. Root then tears down services and
drivers through the supervisor's bounded drain and device/DMA quiesce paths.
A timeout or failed flush prevents a transition and reports the failed phase;
there is no automatic forced shutdown. The display is fenced with the final
message before being drained.
The linked grant gate includes input, storage and filesystem overlap. IA-64's
USB profile includes the AHCI driver's 16-page bounce run, client-copy MemoryObject
and the probe's data Frame; the target gate derives the minimum from every
linked image. The allocator grants all allocatable RAM, with a checked minimum
and at least the existing four-page/one-pair growth allowance. The combined
IA-64 USB flush peak is 265 pages/28 pairs; its minimum is 269 pages/29 pairs.
AMD64's grant suffices. Target checks enforce stack, staging, relocation and
segment limits for the linked components.

The domain drain returns component pages to `PhysicalPageAllocator`
and completes its bounded scrubbing before the power operation. No allocation,
page collection or scrubbing occurs after that operation parks CPUs: the
transition uses retained static storage and existing stop-aperture tables.
The 64-byte BootInfo Power record follows the watchdog configuration word.

Only then does the kernel disable interrupts, park the other online CPUs and
wait for every acknowledgement before one CPU writes the transition register.
AMD64 uses the existing physical NMI delivery; IA-64 uses cooperative polling.
Power parking has its own state and never publishes a fatal STOP record. An
unconfirmed CPU prevents the write even when the fatal-stop path would proceed.

Before requesting the freeze,
the runtime ProcessorSet closes placement, wake and thread-request admission.
Each parking CPU clears its deferred kicks and publishes a lock-free parked
bit before acknowledging; OnlineMask excludes that bit. Wait progress goes
directly to the stop poll, and watchdog comparisons cease for this terminal
phase, so frozen CPUs cannot trigger a false lockup. Pending request words,
thread bindings, root receipts and residency pins stay in retained storage;
parking never drains, reclaims or waits on them. This is terminal power
parking, separate from processor stop policy's contained offlining and its forced detach.
A failed acknowledgement leaves the fence closed and prevents the register
write; the final failure path parks the requesting CPU too.

On ACPI platforms, the boot processor first checks SCI_EN. If clear, it writes
the FADT ACPI_ENABLE byte to its validated SMI_CMD I/O port and waits boundedly
for SCI_EN; an absent command or timeout prevents every sleep-control write
(ACPI 6.5 §§4.8.2.5, 5.2.9). Shutdown then prefers PM1a/PM1b control: preserve unrelated bits,
set SLP_TYPa/b from known root `_S5` package integers, then SLP_EN. A missing,
Unknown, incomplete or exhausted AML namespace supplies no power facts. No
sleep type or register address is guessed. Hardware-dependent `_PTS`/`_TTS`
is unsupported; this implementation refuses transitions that require such
preparation.
Reboot first uses a supported FADT reset register and RESET_VALUE, independently
of AML success. Memory and I/O GAS spaces/widths are validated. A PCI-configuration reset GAS
is refused until there is an exact-byte configuration write interface. 8042 and CF9 fallback commands
require a documented platform contract; none is inferred from an ISA or an
8042-present bit. A FADT that itself names CF9 is authoritative.

UEFI ResetSystem is eligible only with all runtime regions and its table mapped
under a valid firmware calling convention. Neither ISA retains that complete
mapping or the runtime table: BootInfo retains memory
descriptors, AMD64 replaces firmware roots, and IA-64 installs its bounded TR
mapping set. ResetSystem is therefore unavailable, reported explicitly.
IA-64 uses the firmware-declared ACPI interface where present. SAL_RESET is a firmware entry, not an invented SAL procedure:
SAL documents implementation-dependent warm boot and supplies no general
shutdown call. SAL §5.1 does require a warm reboot on INIT when OS_INIT is
unregistered. This kernel registers only OS_BOOT_RENDEZ, so a validated SAL
2.9-or-later platform may use a self-directed architectural INIT after parking,
following the FADT reset preference. Registration of OS_INIT must disable that
fallback. INIT is never used to park processors.

The console shows “Shutting down” or “Restarting”, flush completion, and the
final transition message. Unsupported firmware reports that before teardown.
Failure after teardown leaves the last fenced message. Unconfirmed CPUs
produce a diagnostic and prevent register writes. After a reset write the
requesting CPU spins with no more I/O (ACPI §4.8.3.6). An incomplete flush
never authorizes the transition.

Specifications:

- [Advanced Configuration and Power Interface Specification, release 6.5](https://uefi.org/specs/ACPI/6.5/):
  §§4.8.1.2, 4.8.2.5 (ACPI ownership), 4.8.3.2.1 (PM1 control), 4.8.3.6 (reset), 5.2.3.2 (GAS),
  5.2.9 (FADT), 7.4.1–7.4.2.6, 7.4.4 and 16.1.7 (S5 preparation/entry).
- Unified Extensible Firmware Interface Specification, release 2.11,
  §§2.3.3/2.3.4 (Itanium/x64 conventions), 4.5 (runtime table),
  8.4 (runtime mappings), 8.5.1.1 (ResetSystem).
- Itanium System Abstraction Layer Specification, revision 3.0 (July 2001, 245359-005),
  §§3.2 (SAL_RESET), 5.1 (SAL_INIT and implementation-dependent warm boot), 9 (SAL procedures).

- Intel IA-64 Architecture Software Developer’s Manual, volume 2, revision 1.1
  (July 2000, 245318-002), §§5.8.1–5.8.2 (INIT delivery), 5.8.4.1,
  figures 5-16/5-17 and table 5-16 (self-directed INIT encoding). SAL revision 2.9
  (July 2000), §5.1 has the same unregistered-OS_INIT warm-boot contract.
- AT Attachment 8 — ATA/ATAPI Command Set (ATA8-ACS), T13/1699-D revision 3f
  (11 December 2006), §7.15 (FLUSH CACHE EXT, EAh, non-data, completion only
  after durable data or an error); [T13 draft](https://t13.org/system/files/Project%20Drafts/2006/D1699r3f-ATA8-ACS_2.pdf).
  AHCI specification revision 1.3.1, §§4.2.2–4.2.3 governs its command header/FIS.
