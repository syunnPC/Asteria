<!-- SPDX-License-Identifier: Apache-2.0 -->

# IA-64 capture-only page-fault entry

`AsteriaIa64PageFaultNativeEntry` is a capture-only entry: it captures a
PL3 fault's interruption evidence once and then stops. No IVT slot branches
to it, and default firmware neither configures nor publishes its context.
Page-not-present, key-permission, instruction access-rights and data
access-rights vectors (`0x5000`–`0x5300`) use the complete
[user-fault entry](Ia64UserFaultEntry.md), as do other native PL3 faults.
That entry delivers IFA and ISR to the supervisor and restores the saved frame
after a mapping; PL0 faults stop. Hosted lifecycle and native capture checks
validate this separate capture-only component.

## Context ownership

Selectors 0–3 name page not present, key permission, instruction access
rights and data access rights. Direct entry validates the pointer
at offset 32 of `ar.k3`, processor identity, Region-0 geometry and pristine
headers before `Configured -> Claiming` compare-exchange.

`Ia64BspRuntimeMemory` owns the immutable entry context, one-shot control,
frame, 16 KiB coordinator memory stack, and 32 KiB coordinator RSE store in
the pinned Region-0 runtime corridor. Its publication API admits only those
exact objects, with a separately validated user RBS and initially pristine
frame. The frame can be bound to the exact `Ia64KernelContext` before that
context is prepared. No storage allocation or C++ call occurs in native entry.

## Captured state

Only native, translated, bank-one PL3 faults with interruption collection
enabled are admitted. Key-permission delivery also requires the interrupted
protection-key enable. Nested and incomplete-frame status is rejected before
ownership changes. Live translation/key bits must match the interrupted
state. Hardware supplies the bank-zero privileged entry; `mov from psr` is
only a partial PSR observation, not an independent readback of CPL or BN.
The architectural basis is Intel's
[Itanium Software Developer's Manual, Volume 2, sections 3.3.2 and 5.3](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/itanium-architecture-software-developer-rev-2-3-vol-2-manual.pdf).

The entry preserves the initial selector, predicates, and CCV in bank-zero
scratch registers. A successful claim initializes generation one, then saves
the vector identity, pre-cover ISR, live PSR observation, IIP, IPSR, hardware
IFS, ISR, IFA, ITIR, IIPA, and IIM. It leaves covered IFS and every later
capture field zero. CCV is restored immediately after compare-exchange.

The frame remains `Capturing` at stage `InterruptionState`, with
`PublishedToken == 0`. The control release-publishes `Capturing` only after
its entry token, next token, frame token, vector identity, and interruption
tuple are complete. It then enters a local pause loop. Rejected entry uses a
separate local loop. Neither path enables interruptions, calls C++, changes
stacks, executes `cover`, accesses the user RBS, or returns to PL3.

## Hosted continuation contract

Hosted tests model integer, application, floating-point, and RSE stages;
normalized pager records; exact blocked scheduler ownership; retained
publication; and generation-bound resume. The native capture entry stops at
`InterruptionState` without executing those continuation stages.

RSE validation counts every dirty logical register from the original
`BSPSTORE` through the **post-cover** BSP, including the current frame. The
encoded `loadrs` span is rebased to the page-aligned coordinator RBS and
accounts for its RNAT slots. Original and rebased byte spans can differ.
Both must remain below the 16 KiB transition limit. The covered BSP may
converge exactly at the prepared user RBS end, while the pre-cover BSP and
BSPSTORE must remain strictly inside it.

The complete [user-fault entry](Ia64UserFaultEntry.md) performs what this
model describes natively, on the thread's own storage and with the same frame
type, outside this lifecycle: it saves the complete register image, performs
the RSE transition, hands the fault to the supervisor and restores the exact
retry IIP and slot. This entry routes nothing to a pager.

## Verification boundaries

Hosted tests cover frame/control lifecycle, publication ownership, RNAT-phase
differences, post-cover overflow, zero dirty state, and the one-past RBS limit.
The target verification (`Tools/Verify`, a golden plus the `Ia64PageFaultEntry*`
properties) checks the native symbol, stackless instruction subset, capture
store sequence, atomic claim/publication, terminal loops, object relocations,
and final linked targets at O0, O2 and in the firmware. Every property rejects
its mutants on every run. No firmware boot executes this capture path; the
user-fault entry's guest probes cover page-fault recovery.
