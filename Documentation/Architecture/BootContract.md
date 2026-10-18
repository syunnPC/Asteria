<!-- SPDX-License-Identifier: Apache-2.0 -->

# UEFI Boot Contract

The loader/kernel boundary is [boot protocol](BootProtocol.md), with independent
loader and kernel links described in [Kernel ELF](KernelElf.md). The loader
CRC-checks and places the boot-volume `KERNEL.ELF` and allocates its note-declared reservations.
Kernel preparation remains inside the ExitBootServices transaction; all retained
inputs and state belong to the kernel. Initial mappings consume placed ELF
ownership ranges rather than the loader PE's layout.

Both targets cross `ExitBootServices`, activate owned execution state, and enter
the common capability kernel. Default boot starts one through four CPUs and a
boot-file user ELF. Architecture details belong to the
[IA-64 firmware handoff](Ia64FirmwareHandoff.md),
[IA-64 BSP runtime](Ia64BspKernelRuntime.md), and
[AMD64 runtime](Amd64BootRuntime.md).

## Firmware boundary

`EfiMain(ImageHandle, SystemTable)` is the firmware entry. IA-64 uses P64 function
descriptors; AMD64 uses Microsoft x64 at firmware boundaries and System V inside
the kernel. Imported structures have explicit widths, padding, and layout checks.
No UDK object code or standard library is linked into the target.

Before normal protocol use, validate:

- UEFI table signatures, revisions, reserved fields, extents, and CRCs;
- zeroed BSS, console geometry, loaded-image ownership, image layout and relocations;
- memory-map descriptor version, stride, complete ranges, attributes, and bounds;
- ACPI checksums and final-map ownership for the root, MADT, and optional MCFG and DMAR;
- GOP mode and buffer bounds before drawing the bootstrap diagnostic screen; optional EDID-based mode choice
  before display capture.

IA-64 captures an owned SAL table before exit and later validates its PAL/SAL
entrypoints against the normalized memory map. Both targets capture allocated
AHCI BAR5 provenance through UEFI PCI I/O before final-map preparation. Capture
retains only scalar BDF/base/length data, frees firmware buffers, and reports
absence or invalid resources without retrying the query inside the exit loop.

Both targets then gather boot entropy, still before the final-map transaction
because the firmware's RNG driver may allocate: `EFI_RNG_PROTOCOL` when the
firmware offers it, `RDSEED`/`RDRAND` where CPUID reports them (AMD64), the time
of day and monotonic count, and cycle-counter jitter, extracted into a 32-byte
seed as the [capability model](CapabilityModel.md#validation-keys) specifies.
The application reports the credited sources as the fact
`ASTERIA:BOOT-ENTROPY:SOURCES:0x<mask>`; without a credited source it reports
`ASTERIA:BOOT-ENTROPY:FAIL` and returns to the firmware instead of exiting.

## Boot files

Install `KERNEL.ELF` with retained relocations and stripped user images in
`EFI/ASTERIA` ([kernel image contract](KernelElf.md)): `INIT.ELF` is the product or test pager, `USER.ELF` is MinimalUserImage
or GuestTests, and AMD64 test ESPs also carry `SPINNER.ELF`. The linked ELFs remain build artifacts for observers.
`BOOT.CFG` is at most 4 KiB of ASCII `key = value` lines with `#` comments. The first key is `version = 1`;
`kernel`, `init` and `user` are required, `spinner` optional, with values such as `INIT.ELF crc32c=0x12345678`. CRCs use
exactly eight hex digits and the project's reflected Castagnoli CRC-32C, never firmware IEEE CalculateCrc32.
Names are uppercase 8.3 basenames (letters, digits or underscore), confined to the manifest directory.

Typed UEFI Read/GetInfo adapters open the loaded image's volume, inspect sizes, then read the manifest and
whole user files into one retained LoaderData AllocatePages receipt. The kernel
file has a separate temporary receipt, released after validated placement before
reports and the exit transaction. A file must have 1..16 MiB bytes. Short reads are
completed; premature EOF, growth past the recorded extent, malformed information and CRC mismatches stop
before ExitBootServices. Unknown/duplicate keys, missing entries, version, name and CRC syntax failures also
stop with a console/debug reason and the existing 30-second return-to-firmware message. No fallback exists.
Boot scratch lives in explicit image storage, never a large stack object or hidden allocation.

Reads precede REPORT.TXT and all reservations: AMD64 reads before its 23-page reservation; IA-64 reads in the
original entry before its LoaderCode alignment allocation, using only boot-file/console scratch and immutable
constants. Relocation copies the receipt to the aligned image. Runtime state is initialized in that copy.
AMD64's final-map preparation builds image views from BootInfo::Files; IA-64's staging validates supplied views
against the same recorded region and retains its extent/alias checks. IA-64 consumes the bytes while still
physical. The region is never freed and adds no translation register. Evidence and REPORT.TXT list every retained user/module
basename, size and CRC; debug stage lines have the form `ASTERIA:STAGE:BOOT-FILE:INIT.ELF size=... crc32c=0x...`.

The report writer preserves the order REPORT.TXT, raw ACPI table dump, then AML evaluation and ACPI.TXT
before ExitBootServices. AMD64 excludes the Block I/O write fallback; failed firmware file writes are reported.
The NOAML marker controls only AML evaluation, so boot-file reads and validation still run first.

Media tools copy the complete ESP; ISO media contain both the directory tree with ISO 9660 level-1 names and
an El Torito FAT boot image. The runner's IA-64 AHCI path uses that same level-1 directory naming contract.
[kernel image contract](KernelElf.md) adds up to eight lines of
`module = <logical-name> <8.3-basename> crc32c=0x........`. Files live under `MODULES/`; native ELFs are
stripped, and foreign samples keep their original bytes. Product media list component and directory; test media
add client, the native foreign interpreter stub and the three foreign samples. The loader isolates the module
table and bytes on pages of their own inside the same receipt, with a 2 MiB aggregate limit. Its table is a count,
a zero word, then 16-byte zero-padded names and 64-bit offsets and sizes. The kernel validates the extent,
alignment, LoaderData/write-back provenance and overlap before mapping it read-only into only the pager.
Start-block words 49 and 50 carry the table base and byte size; it leaves INIT.ELF and survives for the system
lifetime. Evidence and REPORT.TXT include logical names, basenames, sizes and CRCs, with stage lines
`ASTERIA:STAGE:BOOT-MODULE:<name> <file> size=... crc32c=0x...`.
The required kernel line is `kernel = KERNEL.ELF crc32c=0x........`; its CRC is
computed from the exact installed bytes after stripping unneeded symbols.
A mismatch reports `KERNEL-CRC32C-MISMATCH` on console/debug and stops before
ELF placement. No embedding fallback exists.

## Final-map transaction

1. Reserve all firmware-owned bootstrap/runtime storage before the first exit
   attempt. The final-map callback must not allocate or call firmware.
2. Obtain `GetMemoryMap` into the fixed 64 KiB buffer. Accept descriptor version 1,
   a stride of at least 40 bytes, integral descriptors, and at most 2,048 regions.
3. Normalize and sort ranges. Reject overlap, zero pages, unaligned or wrapping
   extents, invalid types, and inconsistent attributes. Preserve provenance;
   firmware virtual addresses do not authorize mappings.
4. Parse ACPI against that exact snapshot, publish the gathered entropy record,
   and prepare the target's owned state.
   A retry validates the same allocation, input identity, and retained bytes;
   it must not silently regenerate partially committed state.
5. Call `ExitBootServices` with that map key. Retry only `InvalidParameter`, at
   most four attempts. After the first attempt, only the next `GetMemoryMap` /
   `ExitBootServices` pair may call boot services.
6. Publish `BootInfo` only after successful exit. Terminal transition failure
   halts; execution never returns to firmware after exit.

[`BootInfo`](../../Source/Boot/BootInfo.hpp) uses the current [boot protocol](BootProtocol.md), with size/CRC and
version policy in [BootProtocol](BootProtocol.md), fixed normalized memory
storage, pointer-free ACPI topology, and the boot entropy record whose
seed keys every capability space. The topology retains up to four CPUs, eight
I/O controllers, sixteen interrupt overrides, eight MCFG regions, and four DMAR
remapping units with 32 device scopes. A machine
with more usable CPUs boots with the first four enabled ones; the rest are
validated, counted in `IgnoredProcessorCount`, and reported after exit as the
fact `ASTERIA:ACPI-PROCESSORS:IGNORED:0x<count>`. Optional MCFG or DMAR failure
leaves its fields empty; mandatory CPU/controller topology still has to validate. Page
zero is excluded from bootstrap allocation, and reserved or runtime memory is not
reclaimed as conventional RAM.

## Owned execution

| Target | Preparation and activation |
| --- | --- |
| IA-64 | Reserve stack, IVT, RSE, and translation storage, and the run-time memory page ([TR budget](Ia64TranslationRegisterBudget.md)); bind SAL/PAL; abandon firmware stack/RSE before IVA/RR/TR takeover; prepare per-AP ownership and admit the shared runtime root |
| AMD64 | Prepare the activation extent, runtime pool and low startup pages ([layout](Amd64BootRuntime.md#ownership-and-publication)); verify W^X tables and descriptors before exit; install the owned CR3/stack/descriptors and verify processor state |

AMD64's initial image has seven sections and its bootstrap plan has ten mappings.
Activation continues into per-CPU GDT/IDT/TSS/IST/GS setup, INIT/SIPI, private-root
CPL3 execution, and the common runtime. IA-64 uses native region/translation
resources and per-CPU stack/RSE ownership. Both paths keep guards unmapped and
perform no UEFI calls after exit. IA-64's SAL and PAL processor-firmware calls
remain part of the physical takeover.

Default user images prove Query/Send/Receive loopback. AMD64 also completes two
WaitSet waits; the IA-64 timer/WaitSet/RSE diagnostic is a separate synthetic
scenario. APs prove cooperative Block/Wake and deferred timer notification.
Shared-root publication waits for every resident CPU's TLB acknowledgement.
Optional native-preemption scenarios cover one fixed BSP loop, not general
preemptive scheduling. General pager return, device services, migration, hotplug,
and teardown remain outside this boot contract.

## Images and failure evidence

Both PE32+ images use page-aligned W^X sections and complete base relocations.
IA-64 links a P64 ELF and converts it to PE; its small normalizer sets required
EFI flags. AMD64 resolves a closed ELF aggregate before PE emission and verifies
all absolute/relative references and DIR64 fixups. Compiler/linker closure, stack
limits, and image structure are checked before publication to the ESP.

The boot profile has no Secure Boot, image-signature, or image-hash
attestation requirement. Failures produce bounded target diagnostics and stop.
Stage events (`ASTERIA:STAGE:<NAME>`) report completed transitions; a failure
stops with its FAIL/ERROR diagnostic before any dependent stage. The kernel does
not judge its own diagnostic scenarios: it stores what they observed in a
versioned evidence record (common header in
`Source/Kernel/Diagnostics/EvidenceRecord.hpp`: magic, schema, size, then
fixed sub-records each published by a release store) and reports the record's
address once as `ASTERIA:EVIDENCE:0x<address>`. Both ISAs report stage events
throughout and keep their own record (`Ia64EvidenceRecord`,
`Amd64EvidenceRecord`); FAIL/ERROR/FAULT diagnostics are the only verdicts the
kernel prints. The boot entropy sources and ignored processors above are the
other fact lines. Hosted integration tests reject firmware callbacks after exit and
check the AMD64 stage events, evidence fact line, and published entropy record of
the UEFI path.

## Real hardware (AMD64)

The boot assumes no emulator. Its diagnostic sinks are decided once, before
the first diagnostic (`Platform/Amd64/Firmware/DiagnosticSinkSelection.hpp`),
and every writer (the boot application, the kernel and the activation's
fail-stop handler) uses that decision before and after `ExitBootServices`.
The debug port `0x402` is QEMU's isa-debugcon; on physical hardware it may
overlap chipset registers. It is written only when CPUID leaf 1 reports a
hypervisor and leaf 0x40000000 names QEMU
(`TCGTCGTCGTCG` or `KVMKVMKVM`). Every diagnostic also goes to a 16550 UART,
the one the ACPI SPCR table names when it is 16550-compatible in I/O space
(memory-mapped UARTs are unsupported) or else COM1 at `0x3F8`, when the
port passes a scratch-register and loopback probe; it is programmed for
115200 8N1, every wait on it is bounded, and a UART that stops draining is
dropped. Under QEMU the runner's `Serial.log` therefore holds the log as well
(after OVMF's own console output). The boot
sets CR0.WP and EFER.NXE and clears CR4.PGE and CR4.PCIDE itself after
`ExitBootServices` ([InitialAddressSpace](InitialAddressSpace.md)); ACPI
tables may lie anywhere the kernel does not reuse
([AcpiBootstrap](AcpiBootstrap.md)). A failure before `ExitBootServices` is
reported on the firmware console as well, with the rejecting step and its
error codes, and the application waits 30 seconds before it returns to the
firmware. The root service's filesystem and block demonstrations, which write
to and may format the AHCI disk, run only when the boot found no disk or
identified the emulator's (`PagerManagementDiskWritable`); otherwise they
write a line saying they were skipped. Post-exit failures use the retained diagnostic sinks
([processor stops](ProcessorStops.md)).

## VM validation

The [VM test runner](../../Tools/Vm/RunVmTests.py) owns the stage-event,
evidence-record, and memory oracles, and records the TAP report of the
guest test program (`Tests/Guest`), the initial user image of each tree's test
firmware, which checks what the system-call ABI shows. Its `All` suite runs
fourteen cases: each ISA's test firmware at 1-4 CPUs, one SATA case and one
four-CPU forced-relocation case, and one smoke case per ISA that boots the
production firmware to its user idle loop, all with 512 MiB per VM.

- IA-64 uses the project QEMU/firmware profile and a physical debug UART; AMD64
  uses Q35/OVMF, TCG, debug port `0x402`, and a virtio-rng device that OVMF
  offers as `EFI_RNG_PROTOCOL`.
- Normal images prefer 64 MiB. Relocation tests request a base outside guest RAM
  and require a different loaded base; scripts rebuild normal artifacts afterward.
- Tests reject missing, duplicate, out-of-order, and foreign stage events, a
  missing or malformed evidence fact line, and failure diagnostics. The evidence
  record is read through QMP and judged against the case (CPU count, disk
  fixture), QEMU's own view, and the build's constants. Stopped-state checks validate native
  privilege, execution location, roots/translations, IPC and BSS data, per-CPU
  state, and disk data rather than accepting log text alone.
- Every boot must report one credited entropy source line, AMD64 boots with the
  firmware RNG among the sources. Boots of the same image must report different
  authenticators for the bootstrap handles the guest test program lists; the
  runner compares them across the cases of a run and never judges a value.
- Optional IA-64 synthetic/preemption and AMD64 preemption scenarios are separate
  from `All`; their owners document the extra frame/timer evidence.
- Disposable disk fixtures contain the known LBA-8 sector. VNC uses Unix sockets;
  screenshots are checked for PPM dimensions and payload length, not a pixel golden.

Artifacts are retained under `Build/{Ia64,Amd64}/Firmware/`. The available commands
and environment overrides are listed in the [README](../../README.md#firmware-and-vm-tests).

## Boot display policy

`BOOT.CFG` accepts one optional `display` key after `version = 1`: `native` (the default), `keep`, or positive
32-bit decimal dimensions separated by a lowercase `x`, for example `display = 1366x768`. Unknown values,
zero dimensions, overflow and duplicate keys are refused with `MANIFEST-INVALID-DISPLAY` or
`MANIFEST-DUPLICATE`, with the same failure-atomic parser contract as the file keys. The policy stays in
boot-local storage and does not extend BootInfo.

Native uses the first detailed timing from a valid 128-byte EDID base block on the GOP handle (Active,
else Discovered). It prefers an equal usable mode, then the largest RGBX/BGRX mode within both EDID
dimensions whose stride times height times four is at most 32 MiB. Missing, invalid or unsupported EDID
keeps the current mode. An explicit size requires a listed usable mode; an absent match keeps the current
mode. Keep never switches. Enumeration and REPORT.TXT are bounded to 256 modes. The report gives the
EDID checksum verdict, manufacturer, product and preferred timing when decoded, policy and final mode.

Only a changed choice calls GOP SetMode. A firmware error or unusable result causes one original-mode
restoration attempt; even a restoration failure is diagnostic rather than a boot failure. The actual
usable resulting mode is recorded; otherwise the display record remains empty. Quiet manifest/file
validation precedes selection; the switch and desktop precede all console text.
Later text, report-save status and the 30-second failure screen use GOP Blt from a static RAM line
after any switch attempt. The console wraps to the top without scrolling or scanout reads, with at
most 20,480 device pixels per completed line, independent of screen height. The original mode is
restored after the failure screen before return. The entry disables the firmware watchdog before
file I/O/placement. EDID 1.3 without its preferred flag is accepted and reported. REPORT.TXT saving
continues through the normal file path. Runtime display
SetMode remains reserved.

The AMD64 runner advertises 1280x800 for existing cases. Separate cases request 1440x900 natively,
keep 1280x800, and switch explicitly to 1024x768. The production USB case advertises 1366x768 and uses
QEMU's existing `bochs-display`: std VGA rounds scanout widths to multiples of eight. At the QMP boundary, the runner removes RGB24 row padding from QEMU's PPM screendumps
at widths such as 1366; its general PPM parser stays strict and its screen oracle compares every pixel.
