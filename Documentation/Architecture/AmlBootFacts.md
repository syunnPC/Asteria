<!-- SPDX-License-Identifier: Apache-2.0 -->

# AML facts on the boot volume

AML boot facts provide observations for
hardware bring-up. They do not change the FADT-based i8042 policy or interrupt
routing. [PowerOff](PowerOff.md) also consumes bound `_S5` types when the full
namespace loaded successfully and requires no hardware preparation methods.
Unknown or partial loads never authorize S5. The evaluator has no region handlers.

## Execution and ownership

`WriteBootReport` runs before the ExitBootServices transaction and its final
memory map. It publishes REPORT.TXT, flushes the validated raw SDTs under
`EFI/ASTERIA/ACPI`, appends the dump outcome, then calls `WriteAmlBootReport`.
The DSDT loads first, followed by SSDTs in the parser's firmware table order.
The firmware retains ownership of their immutable bytes until firmware exit.
The final authoritative BootInfo parse does not evaluate AML or allocate.

The boot memory owner allocates LoaderData pages for the typed AML arena,
Library, Facts, ReportStorage and a 16 KiB streaming buffer. The arena itself
is 34,493,824 bytes (32,768 nodes). On allocation refusal it retries with an
8,192-node arena of 10,114,432 bytes; syntax, objects, elements and byte storage
shrink proportionally while parser/evaluator stacks and diagnostics keep their
fixed bounds. ReportStorage is 158,624 bytes. No arena is placed in the image's
static storage; all temporary pages are freed before the writer returns.
Allocation refusal, partial loading, capacity exhaustion and file/flush/free
failures are reported and do not stop boot. Unknown facts remain explicit.

An existing `EFI/ASTERIA/NOAML` skips allocation, evaluation and ACPI.TXT writes.
An error checking the marker also skips with a diagnostic. REPORT.TXT and the
raw dump are attempted first. An ACPI.TXT from an earlier boot is retained when
the step is skipped; the current REPORT.TXT outcome distinguishes it.

## Output and time

ACPI.TXT uses the pure library's shared `Report::Print` formatter:
**Asteria AML facts v1**, ASCII with LF endings and escaped byte strings.
It lists device IDs, presence including ancestors, resources, PS/2 candidates,
PCI roots and routes after `_PIC(1)`, `_S5`, `_OSI` queries, suppressed effects,
diagnostics and used/available budgets. Reduced storage is explicitly marked
`arena reduced node-budget=8192`, and denominators describe the actual limits.
The formatter accumulates device, PS/2 candidate and route counts without a
second evaluation pass.

A single console/REPORT.TXT outcome includes those counts, diagnostic count,
node budget, arena bytes, counter type, elapsed ticks and elapsed microseconds.
TSC/ITC differences cover allocation, initialization, loading, fact evaluation,
file writing/flush/close and page release. A successful firmware Stall of 1 ms
calibrates the counter; microseconds are approximate and become `unavailable`
when calibration cannot supply a nonzero rate. Counter differences use modulo
64-bit subtraction. Timing is outside ACPI.TXT to preserve deterministic bytes.

On the host, use `Build/CMake/host-debug/Tools/AmlFacts DSDT.AML SSDT1.AML...`,
with SSDTs in numeric dump order; `--reduced-arena` reproduces reduced storage.
The tool prints partial facts even when it exits 1 for a load diagnostic.
The normal writer uses the loaded volume's EFI file protocol. AMD64 reports
write failure without Block I/O fallback, avoiding conflicts with firmware FAT
caches. On IA-64, a FAT32 Block I/O adapter uses only the loaded device's
logical partition when the writer cannot open the report directory. It requires pre-existing
EFI/ASTERIA/ACPI directories, writable 512-byte media, two matching FATs and
a clean volume of at most 1,048,576 clusters. It does not format or grow
directories. New data/FAT chains are flushed before replacing a directory
entry, old chains are freed afterward, and FSInfo counts become unknown.
I/O failure fences further writes; a failure before publication retains the
old entry, while a later failure may expose the new file or leak clusters.
Directory insertion refuses terminal end-marker slots and stale following
entries. REPORT.TXT names the fallback. The adapter follows the
[Microsoft FAT specification](https://www.pcjs.org/documents/papers/microsoft/MS_FAT_OVERVIEW_103-2000-12-06.pdf).

## Bring-up evidence

AMD64 VM cases check the PCI root, PNP0303 keyboard with ports 0x60/0x64 and
IRQ 1, and PNP0F13 mouse with IRQ 12. IA-64 checks its PNP0A03 PCI root,
128 routes and the same declared PS/2 resources with absent status. Both
compare ACPI.TXT with AmlFacts over
case-local dumps byte for byte. NOAML cases stage the marker before boot.
Recovery tests exercise refused allocations, reduced storage and short/failed
file writes. All third-party firmware tables are read only at test time and
remain under Build; no AML tables enter fixtures.

For hardware validation, collect REPORT.TXT, ACPI.TXT and the whole ACPI directory.
Check the AML outcome's time, arena and diagnostics; inspect keyboard/mouse
IDs, `_STA`/ancestor verdicts, IO ports and IRQs, then PCI root `_PRT` and
Unknown reasons. Keep the raw tables even if the facts are incomplete. Create
NOAML on the boot volume to bypass the evaluator on a later boot.

The same facts walk derives
DSDT-bound keyboard/mouse evidence even when report writes fail. NOAML
prevents this evidence, so a flag-clear machine will not publish its keyboard
with that marker. `\EFI\ASTERIA\NOFADT8042` is an all-build diagnostic marker
that masks FADT bit 1 in the retained topology and exercises namespace publication.
The kernel independently rejects a floating port; no controller command runs
there. ACPI.TXT includes qualification failures and IRQ mode/polarity.
