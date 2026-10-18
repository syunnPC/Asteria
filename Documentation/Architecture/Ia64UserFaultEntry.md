<!-- SPDX-License-Identifier: Apache-2.0 -->

# IA-64 user-fault entry

## Interruption inventory

Every interruption vector of the native IVT (Intel *Itanium Architecture
Software Developer's Manual*, revision 2.3, Volume 2, §§3.3.5, 5.3, 6.6–6.11
and chapter 8, tables 8-1–8-4), what a PL3 thread must do to raise it, which interruption
registers define its address and detail, and how the native entry handles it.
Offsets are relative to `cr.iva`; the vectors below `0x5000` have 1 KiB slots,
the others 256-byte slots. "Supervisor" is the thread's fault endpoint and the
FaultToken/FaultResolve contract: the boot pager for the initial image and
for the threads it creates. Cause and access name the FaultRecord fields;
"address" is its VirtualAddress, IFA where the vector defines one and the
faulting bundle otherwise. The same interruption at PL0 stops the system on
every vector. The evidence column names the guest probe of the test pager
([Verification](#verification)) or, where native PL3 code cannot raise the
vector in the target profile, the host classification test.

| Offset | Interruption(s); how PL3 raises it | Defined address and detail | Handling | Evidence |
| --- | --- | --- | --- | --- |
| `0x0000` | VHPT instruction/data translation; IR VHPT data; only with the walker enabled | IFA the reference; IHA the VHPT entry; ITIR for IHA | Stop: the walker stays disabled, so an occurrence is a kernel error | IVT check (stop slot) |
| `0x0400` | Instruction TLB; only with the walker enabled | IFA the fetched bundle; ISR.x | Supervisor: MissingMapping, execute, IFA; a mapping retries the slot | Host |
| `0x0800` | Data TLB; IR data TLB; only with the walker enabled | IFA the operand or RSE address; ISR.r/w/rs/ir | Supervisor: MissingMapping, read/write, IFA, ISR.rs/ir kept | Host |
| `0x0c00` | Alternate instruction TLB: a fetch from a page with no ITC entry | IFA the fetched bundle; ISR.x | Ledger refill; a miss goes to the supervisor: MissingMapping, execute, IFA; a mapping retries the slot | Guest: lazy instruction resolved and retried |
| `0x1000` | Alternate data TLB; IR alternate data TLB: a load, store or mandatory RSE spill/fill with no DTC entry | IFA the operand or RSE address; ISR.r/w/rs/ir | Ledger refill; every PL3 miss, mandatory RSE traffic and incomplete frames included, goes to the supervisor: MissingMapping, read/write, IFA | Guest: lazy data pages; lazy RSE spills and incomplete-frame fills resolved and resumed |
| `0x1400` | Data nested TLB; requires collection off | The earlier tuple is kept; ISR not written | Stop: no PL3 instruction to attribute it to | IVT check (stop slot) |
| `0x1800` | Instruction key miss; only with PSR.pk set | IFA; ISR.x | Supervisor: ProtectionViolation, execute, IFA. The launch clears PSR.pk | Host |
| `0x1c00` | Data key miss; IR data key miss; only with PSR.pk set | IFA; ISR.r/w/rs/ir | Supervisor: ProtectionViolation, read/write, IFA | Host |
| `0x2000` | Data dirty bit, mandatory RSE store included | IFA; ISR.w/rs | Supervisor: ProtectionViolation, write, IFA. The ledger sets the dirty bit of every writable page, so a write to a read-only page is an access-rights fault instead | Host |
| `0x2400` | Instruction access bit | IFA; ISR.x | Supervisor: ProtectionViolation, execute, IFA. The ledger sets every page's accessed bit | Host |
| `0x2800` | Data access bit; IR data access bit | IFA; ISR.r/w/rs/ir | Supervisor: ProtectionViolation, read/write, IFA | Host |
| `0x2c00` | Break instruction | IIM the zero-extended 21-bit immediate; ISR.ei the slot; IFA undefined | The system-call immediate enters the system call; any other goes to the supervisor: Exception, the IIM in syndrome bits 0–20 | Guest: break immediate and slot; busy pager; spinner's closing break |
| `0x3000` | External interrupt taken at PL3 | IVR names the source; IIP/IPSR the interrupted slot | Timers preempt, devices dispatch and unexpected sources stop; a CPU-bound PL3 thread is preempted at its run stop with its complete state | Guest and runner: spinner (`host.ia64.user-spinner-preempted`) |
| `0x3400` | Virtual external interrupt; virtualized execution only | — | Stop: PSR.vm is outside the native profile | IVT check (stop slot) |
| `0x5000` | Instruction/data page not present; IR data page not present | IFA; ISR.x/r/w/rs/ir | Supervisor: MissingMapping, IFA. The ledger inserts present pages only | Host |
| `0x5100` | Instruction/data key permission; IR data; only with PSR.pk set | IFA; ISR.x/r/w/rs/ir | Supervisor: ProtectionViolation, IFA | Host |
| `0x5200` | Instruction access rights: a fetch from a page without execute right | IFA the fetched bundle; ISR.x | Supervisor: ProtectionViolation, execute, IFA | Guest: instruction access rights IFA |
| `0x5300` | Data access rights; IR data access rights: a write to a read-only page, for one | IFA; ISR.r/w/rs/ir | Supervisor: ProtectionViolation, read/write, IFA | Guest: data access rights IFA |
| `0x5400` | General Exception: illegal operation (ISR.code{7:4} 0), privileged operation (1), privileged register (2), reserved register or field (3), disabled ISA transition (4), illegal dependency (8) | IFA undefined; ISR.code, ei | Supervisor: Exception, ISR.code and slot in the syndrome | Guest: privileged operation; host for the other codes |
| `0x5500` | Disabled floating-point register; only with PSR.dfl/dfh set | ISR.code the disabled set; IFA undefined | Supervisor: Exception. The launch enables f2–f127 | Host |
| `0x5600` | Register NaT consumption (ISR.code{7:4} 1); instruction/data NaT page consumption and IR data NaT page (2) | IFA defined for a NaT page only | Supervisor: Exception; IFA for a NaT page, the bundle for a NaT register | Guest: NaT register consumption; host for NaT pages, which the ledger never maps |
| `0x5700` | Speculative operation: chk.a, chk.s or fchkf whose recovery branch the processor leaves to software | IIM the check's immediate; ISR.code{3:0} the check; IFA undefined | Supervisor: Exception, which the pager terminates. The kernel does not emulate the branch (see [PL3 exceptions](#pl3-exceptions)) | Guest: QEMU branches to recovery itself, no fault; host |
| `0x5900` | Instruction/data debug; IR data debug; only with debug registers armed | IFA; ISR.x/r/w/rs/ir | Supervisor: Exception, IFA. No debug state is configured for PL3 | Host |
| `0x5a00` | Unaligned data reference, with PSR.ac (user mask) set or an access the processor does not support unaligned | IFA the operand; ISR.r/w | Supervisor: Exception, IFA | Guest: unaligned IFA |
| `0x5b00` | Unsupported data reference: an access the page's memory attribute does not support, such as a semaphore on a driver's uncached or write-combining device page | IFA; ISR.r/w | Supervisor: Exception, IFA | Host |
| `0x5c00` | Floating-point exception fault | ISR.code the FP cause; ISR.ei; IFA undefined | Supervisor: Exception. No FPSWA software assist | Guest: FP exception of a created thread |
| `0x5d00` | Floating-point exception trap | IIPA and ISR.ei the trapping instruction; IIP/IPSR the next | Supervisor: Exception at IIPA; the resume target keeps IIP/IPSR | Host |
| `0x5e00` | Unimplemented instruction address fault (ISR.x) or trap; lower-privilege transfer trap (PSR.lp) | IFA for the fault form only; a trap's IIPA is the branch, IIP its target | Supervisor: Exception; IFA for the fault, IIPA for a trap | Host |
| `0x5f00` | Taken branch trap; only with PSR.tb set | IIPA and ISR.ei the branch; IIP the target | Supervisor: Exception at IIPA. PSR.tb is a system-mask bit PL3 cannot set | Host |
| `0x6000` | Single step trap; only with PSR.ss set | IIPA and ISR.ei the completed instruction | Supervisor: Exception at IIPA. PSR.ss as PSR.tb | Host |
| `0x6100` | Virtualization fault; virtualized execution only | — | Stop: outside the native profile | IVT check (stop slot) |
| `0x6900`–`0x6b00` | IA-32 exception, intercept and software interrupt; IA-32 execution only | ISR.vector/code; IIPA | Stop: IPSR.is must be clear | IVT check (stop slot) |
| Other | Reserved | — | Stop | IVT check |

Reset, INIT and machine checks enter PAL, not an IVT slot; they are processor
and platform events, not supervisor faults. The `Ia64InterruptVectorTable` checks in `Tools/Verify` validate all 34 named
offsets of table 8-4 against the assembly IVT and pin each slot's route.

### Evidence and restart rules

With interrupted `PSR.ic=1`, hardware collects IIP, IPSR and IIPA and clears
`IFS.v`; `IFS.ifm` is then undefined. IFA, ITIR and IIM are vector-specific:
an undefined register must never become an alleged fault address or immediate.
`ISR.code` occupies bits 0–15. Access bits x/w/r are 32/33/34; na/sp/rs/ir/ni
are 35–39; `ISR.ei` is bits 41–42 and ed is bit 43. Faults and external
interrupts have `ISR.ei == IPSR.ri`; traps report the completed instruction's
slot in `ISR.ei`, while IPSR.ri names the next slot. IIPA supplies the trap's
source bundle. Simultaneous trap causes are retained in the code bitset.

Mandatory RSE faults can arise on `alloc`, `flushrs` and `loadrs`; a `br.ret`
or `rfi` fill fault is delivered at its target instruction. `ISR.rs=1` marks
mandatory RSE traffic and `ISR.ir=1` an incomplete frame. Incomplete means
BSPSTORE is above BSP. Before `cover` completes that frame, `alloc`, calls,
returns, `flushrs`, `loadrs`, and writes to BSPSTORE have undefined behaviour
(§6.8). The capture must therefore save the interruption tuple and RSE
pointers, execute `cover` before switching backing stores, retain the covered
IFS, and restore the dirty partition and original backing-store state before
`rfi`, which reloads the frame and retries the mandatory access. An external
interrupt taken while those fills reload the current frame also reports
`ISR.ir` (table 8-2, note i); the interrupt entry admits it the same way. A
nested interruption (`ISR.ni`) has no tuple of its own and cannot use this
contract.

The complete-state entries `AsteriaIa64UserFaultEntry`,
`AsteriaIa64UserExceptionEntry` and `AsteriaIa64UserInterruptEntry` share one
capture/restore body. Native PL3 faults and traps reach the domain supervisor
through FaultToken, for the initial image and created threads alike; timer/device interruptions
use the established claim path. The handler may block on the thread's own
kernel stack, then restore the complete saved image and use `rfi`.

Until the runtime admits and selects a user-fault context (offset 40 of the
processor's `ar.k3` root is zero), the entry rejects every escape before it
touches any storage, so a PL3 ledger miss stops.

## Route

Both alternate refill handlers (`0x0c00`, `0x1000`) refill ledger hits with
`itc.i`/`itc.d`. A PL3 ledger miss restores entry PR and tail branches to the
complete-state entry: instruction misses carry vector `0x0c00` in `ar.k4`
and selector 6; data misses selector 4. Mandatory RSE accesses take the same
escape. Kernel admission invariant failures and PL0 misses still stop;
rejected PL3-writable entry state is supervised as Exception.

The other native fault/trap slots in the inventory branch with selector 6
and their vector in `ar.k4`. `AsteriaIa64UserBreakEntry` preserves PR and
selects the system-call entry only for IIM `0x100000`; other immediates go to
the supervisor. Reserved, VHPT/virtualization and IA-32 vectors stay outside
the native profile. External interruptions keep the timer/device claim path.

## Admission

The entry first retains RSC in privileged scratch `ar.k6` and selects safe
`0xc`. Before writing any other interrupted state, it requires:

- selector 4, 5 (interrupt), or 6 (native fault/trap);
- the current PSR of the interruption (dt, rt, it set; ic, i, dfl, dfh
  clear) and an interrupted PSR of native PL3 on bank one with ic, dt, rt and
  it set and is and vm clear, and `psr.i` either (the boot domains run
  with it set, the synthetic user probe with it clear; the user interrupt
  entry requires it set);
- `IFS.v` clear, IIP bundle-aligned and `IPSR.ri` one of the three slots;
- `ISR.ni` clear (the non-access, speculative-load, mandatory-RSE and
  incomplete-frame bits are admitted and kept for the decoder);
- the original `ar.rsc` is retained in privileged scratch `ar.k6`, and
  `0xc` is installed before RSE work. A PL3-writable mode/byte-order
  violation becomes Exception;
  interruption `loadrs` remains free; syscall RSC is exactly `0xc`;
- a nonzero, 16-byte-aligned Region-0 `ar.k3` root and, acquired from its
  offset 40, a nonzero, 16-byte-aligned Region-0
  [`Ia64UserFaultEntryContext`](../../Source/Platform/Ia64/Interrupt/Ia64UserFaultEntry.hpp)
  whose `ProcessorIndex` equals the root's and whose `Reserved` word is zero;
- a 64-byte-aligned Region-0 frame, a 16 KiB memory stack and a 32 KiB RSE
  backing store that each end on a page in Region 0 above a nonzero start,
  and an 8-byte-aligned Region-0 handler.

Admission of the context (`Ia64RuntimeInterruptVectorTable::AdmitUserFaultEntryContext`)
already proved that the frame and both storage ranges lie in the DTR-covered
runtime-object corridor and are disjoint; the entry relies on that to access
them with collection off. Kernel invariant mismatches take the stop path;
rejected PL3-writable RSC or CFM reaches the thread's supervisor. PFS.pfm
can restore an invalid size/rotation/rename combination through `br.ret`
(SDM revision 1.1, Vol. 1 §3.1.8.10; Vol. 3, "mov ar" and "br", Operation);
the covered IFS header remains a kernel invariant. An
interruption leaves `PSR.pk` as it was (SDM Vol. 2, §5.5), so the entry
clears it and serializes before it touches kernel storage; the restored IPSR
gives the thread its own setting back.

The frame is the thread's reusable
[`Ia64PageFaultFrame`](../../Source/Platform/Ia64/Interrupt/Ia64PageFaultEntryContract.hpp),
normally the one embedded in its `Ia64KernelContext`. The entry requires
`PublishedToken` zero and `State` Inactive, then release-stores `Capturing`.

## Captured image

| Frame field | Content |
| --- | --- |
| `VectorOffset`, `PreCoverInterruptionStatus` | `0x1000` for selector 4, the offset the trampoline left in `ar.k4` for selector 6, `0x3000` (a device vector in its low byte) from `ar.k5` for selector 5; `cr.isr` |
| `CurrentProcessorStatus`, `InterruptedInstructionPointer`, `InterruptedProcessorStatus` | `psr`, `cr.iip`, `cr.ipsr` |
| `HardwareInterruptionFunctionState`, `CoveredInterruptionFunctionState` | `cr.ifs` on entry, `cr.ifs` after `cover` |
| `InterruptionStatus`, `InterruptionFaultingAddress`, `InterruptionTranslationInformation`, `InterruptionPreviousInstructionPointer`, `InterruptionImmediate` | `cr.isr`, `cr.ifa`, `cr.itir`, `cr.iipa`, `cr.iim` |
| `PreviousFunctionState`, `RseConfiguration`, `RseBackingStorePointer` | `ar.pfs`, `ar.rsc`, `ar.bsp` before `cover` |
| `RseBackingStoreStorePointer`, `RseNatCollection`, `CoveredRseBackingStorePointer` | `ar.bspstore` and `ar.rnat` of the user stack, `ar.bsp` after `cover` |
| `RseDirtyPartitionLoadConfiguration` | the `ar.rsc` value that reloads the dirty partition (`loadrs` distance << 16) |
| `OriginalNatCollection` | the interrupted `ar.unat` |
| `LoopCounter`, `EpilogCounter`, `CompareAndStoreData`, `CompareExchangeValue`, `FloatingPointStatus` | `ar.lc`, `ar.ec`, `ar.csd`, `ar.ccv`, `ar.fpsr` |
| `Predicates` | `pr` (see below) |
| `BranchRegisters[0..7]` | `b0`-`b7` |
| `GeneralRegisters[0..14]`, `StaticGeneralRegisterSpillNatCollection` | r1-r15 by `st8.spill`, their spill UNAT |
| `GeneralRegisters[15..30]`, `BankedGeneralRegisterSpillNatCollection` | bank-one r16-r31 by `st8.spill`, their spill UNAT |
| `FloatingRegisters[0..125]` | f2-f127 by `stf.spill` |

The static and bank-one registers use separate spill collections, as the
system-call and preemption entries do. `cover` runs after the bank-one capture;
it moves the interrupted frame into the dirty partition and resets the rename
bases (Intel SDM Vol. 2 section 6.11.1). The floating spills follow it, so
f32-f127 are spilled and later filled with `CFM.rrb.fr` zero, by their physical
names, whatever rotation the user frame had. `Predicates` is p1-p15 as sampled
on entry (the admission uses p6 and p7) deposited over p16-p63 read again
after `cover`; the restore writes PR with the rename base zero as well, so the
image is exact whether or not moves to and from PR rename the rotating
predicates. Any CFM is admitted: unlike a system call, a fault may interrupt a
frame with locals, outputs or rotating registers.

IA-32 application state (`ar.ssd`, `eflag`, `cflg`, `fsr`, `fir`, `fdr`) is
outside the native subset (`IPSR.is` must be clear) and is not captured; nor
are the kernel registers `ar.k0`-`ar.k7`, which PL3 cannot write.

## Register stack and call

The switch follows the system-call entry's PL3 sequence (section 6.11.1):
`invala`, `ar.rsc=0`, save `ar.bspstore` and `ar.rnat`, `ar.bspstore` = the
context's `RseBackingStoreStart`, `ar.rnat=0`. The new BSP distance must be
8-byte aligned and below the 16 KiB transition reserve; it becomes the
`loadrs` configuration. With the kernel RSC (eager, PL0) and r12 at
`MemoryStackEnd - 16`, a two-output frame passes the frame (out0) and the
context's `HandlerAddress` (out1). r4 keeps the frame across the call. The
call runs with `PSR.ic` set, bank one and the kernel GP:

```cpp
extern "C" Foundation::UInt64 AsteriaIa64UserFaultBridge(
    Ia64PageFaultFrame* frame, void* handler) noexcept;
```

`handler` is the `IIa64UserFaultHandler` subobject the runtime admitted as
`HandlerAddress`, passed opaquely so the C boundary carries no virtual type;
the bridge converts it back.

The bridge (`RunIa64UserFaultBridge`) checks the frame and handler pointers and
a `Capturing` frame with `PublishedToken` zero, moves the frame to `Retained`
and decodes it (`DecodeIa64UserFault`). Validation of native ISR, slots and
the covered IFS header is failure-atomic; rejected user CFM becomes Exception.
TLB misses and page not present classify as
`MissingMapping`; key, rights, access-bit and dirty-bit faults as
`ProtectionViolation`; other faults/traps as `Exception`. The vector specifies
the architectural reason. IFA is used only where defined in the inventory;
otherwise the faulting bundle address is reported. Traps report IIPA but
retain hardware IIP/IPSR as restart target. Syndrome is `(vector << 48) | ISR`,
preserving code, access, RSE and slot bits; for break, bits 0–20 carry IIM
(break ISR.code is zero). It stores the result in `CapturedEntry`
and calls `IIa64UserFaultHandler::HandleUserFault(frame, fault)`. The handler
may block the thread: `Ia64KernelContext::SwitchTo` refuses only the frame
states the entry owns (`Capturing`, `ResumeCommitted`), not `Retained`. On
`Resume`, with the frame still `Retained` and its IIP, IPSR and covered IFS
unchanged, the bridge release-stores `ResumeCommitted` and returns 0. `FailStop`,
an invalid disposition and every other failure return a nonzero
`Ia64UserFaultBridgeStatus`, and the entry stops.

The bridge runs on the thread's own kernel stack; selecting the thread's
contexts again after it is readmitted is the runtime's duty, as for system
calls.

## Restore

After a zero result the entry requires `ResumeCommitted`, then, still with
collection on, restores the register stack exactly as the system-call tail
does: `ar.rsc` = the stored load configuration, the stacked-register scrub
(grow to all 96 physical registers, clear each, drop the frame, so no kernel
value reaches a register PL3 may allocate), `loadrs`, `invala`, the user
`ar.bspstore` and `ar.rnat`, and a check that BSP converged on the post-cover
value. It fills f2-f127 and restores `ar.lc`, `ar.ec`, `ar.csd`, `ar.ccv`,
`ar.fpsr` and `b0`-`b7`.

A fault may not clobber floating registers, so the final controls do not travel
in floating carriers as in the system-call tail. Instead the entry clears
`PSR.ic` first: every remaining access is to the DTR-covered frame, so none can
miss, and with collection off no interruption can disturb bank zero. Bank-zero
r16-r23 receive the State address, IIP, IPSR, covered IFS, PR, PFS, RSC and the
interrupted UNAT; bank one is filled with its collection, then r1-r15 with
theirs (the pointer register last). Back on bank zero, the release store of
`Inactive` is the last memory access. `ar.pfs`, `cr.ifs`, `ar.rsc`, `cr.iip`,
`cr.ipsr` (both unchanged: no slot advance), `ar.unat` and PR follow, then `rfi`
re-executes the faulting instruction.

## PL3 interrupts

[user preemption](ProcessorKernelRunLoop.md) lets a timer
interrupt taken at PL3 enter the kernel on the interrupted thread's own entry
storage, like a fault:

1. The external-interrupt dispatcher `AsteriaIa64ExternalInterruptDispatchEntry`
   checks the current PSR, then branches on `IPSR.cpl` 3 before any of its PL0
   checks; it restores PR from its entry copy and branches to
   `AsteriaIa64UserInterruptClaim`. Every other interrupt keeps the native and
   legacy routes.
2. The claim runs on bank zero with `PSR.ic` and `PSR.i` clear, writes only
   bank-zero r16-r31, p6, p7 and `ar.k5` and accesses no memory. It saves PR,
   reads `cr.ivr` once and serializes the read, then:
   - spurious vector 15: restores PR and returns with `rfi`, without EOI;
   - timer vector `0xe8` whose deadline is not due (`ar.itc` below `cr.itm`):
     the tick pended while delivery was masked, and the kernel has since
     replaced its deadline by a later one. The claim writes EOI, leaves
     `cr.itv` unmasked so the new deadline stays armed, restores PR and returns
     with `rfi`;
   - a due timer interrupt: masks the timer (`cr.itv` = `0x100e8`) and reads it
     back, writes EOI, leaves the vector offset `0x3000` in `ar.k5`, restores
     PR and branches to `AsteriaIa64UserInterruptEntry` with selector 5 in r16;
   - a device vector, `0x40`-`0x4F`
     ([driver isolation](IommuDomain.md)): writes
     EOI at once, since the I/O SAPIC's Remote IRR holds the line until the
     kernel acknowledges it there, leaves `0x3000` with the vector in its low
     byte in `ar.k5`, and enters the same way, the timer left as it is;
   - any other vector, or `cr.itm` 0 or -1: masks and reads back the timer,
     writes EOI and stops. A readback mismatch also writes EOI before it stops.

   Every path that read `cr.ivr`, except the spurious one, writes EOI exactly
   once, so the bridge runs with no interrupt in service.
3. `AsteriaIa64UserInterruptEntry` is the second expansion of the entry body
   (the `AsteriaIa64UserCaptureEntry` macro of `Ia64UserFaultEntry.S`). Its
   admission differs only in selector 5 and the interrupted `psr.i`, which
   must be set; it records the `VectorOffset` the claim left in `ar.k5`, which
   PL3 can read but not write, and captures, switches and calls
   `AsteriaIa64UserFaultBridge` exactly as the user-fault entry does.
4. The bridge moves the frame to `Retained` and, for the vector offset
   `0x3000`, hands it to `IIa64UserFaultHandler::HandleUserInterrupt` without
   decoding a fault. For a device vector it calls the dispatch the boot
   configured (`ConfigureIa64UserDeviceInterrupts`, the IA-64 INTx lines),
   which enters the Interrupt bound to the vector and leaves its line masked
   and acknowledged, and resumes the thread without preempting it; with no
   dispatch configured it stops. On `Resume`, with the same unchanged-target
   check, it commits the frame, and the entry restores the complete image and
   returns with `rfi` to the interrupted slot with the captured IPSR, `psr.i`
   set. If the interrupted state holds a rejected PL3-writable RSC or CFM,
   the bridge first decodes it
   (failure-atomic), then services the interrupt exactly as above, so the
   tick's deadline is acknowledged and a device Interrupt is never lost; on
   `Resume`, instead of committing, it delivers the thread's Exception and
   retries a busy supervisor in the kernel continuation.

The boot domains (the initial image and the boot pager) launch with
`Ia64UserModePreemptibleProcessorStatus`, `PSR.i` set, so their timer ticks
take this path; the synthetic user probe keeps `PSR.i` clear and never does.
Both domains' handlers call `IIa64UserPreemption::PreemptUser`
([BSP kernel runtime](Ia64BspKernelRuntime.md#user-preemption)).

## PL3 exceptions

Every native fault and trap of the inventory takes one supervisor channel and
FaultToken lifecycle. The
initial image's supervisor is the boot pager, which maps a missing data page
of its lazy region and terminates every other fault; terminating the initial
image does not end BSP dispatch, which goes on serving the pager and the
threads it created, as on AMD64. A created thread's supervisor is its
domain's. The pager's own faults stop the system: it has no supervisor.
PL0 interruptions, nested interruptions, a captured state the decoder
rejects and an unavailable handler stop as well.

Three architectural services are not provided. The kernel does not emulate
the recovery branch the Speculation vector (`0x5700`) asks software for when
a processor leaves the branch of `chk.a`, `chk.s` or `fchkf` unimplemented
(SDM Vol. 2, Speculation vector): such a check is delivered as an Exception
and its supervisor terminates the thread. QEMU's model takes the branch
itself (the guest's `chk.s` probe). Floating-point faults that need the
FPSWA software assist, and unaligned references other software might
emulate, are delivered rather than completed.

### Mandatory RSE faults

A mandatory RSE spill or fill that faults keeps `ISR.rs`/`ISR.ir`, IFA, the
pre-cover IFS and BSP, BSPSTORE and RNAT in the frame. `cover` (SDM Vol. 2
§§6.8, 6.11) completes an incomplete frame before the entry allocates, calls
or switches backing stores, and moves the dirty partition onto the thread's
pinned kernel backing store, so no lazy user page is touched while collection
is off. The supervisor maps the user backing-store page; the restore reloads
the dirty partition, rebinds BSPSTORE and RNAT, restores the covered IFS, and
`rfi` retries the mandatory access. The PL3 RSC must be enforced-lazy, as the
launch sets it; its `loadrs` field may hold any value. A rejected user mode
or byte order is instead captured under safe RSC and supervised as Exception.

### A busy supervisor

A fault that finds its supervisor busy (its pager channel holding another
fault) yields through the ready queue and retries unchanged when the thread
runs again, however often; a delivery resets the count. The initial image's
and the created threads' handlers share this policy with AMD64
(`Kernel/Memory/FaultDeferral.hpp`). `Ia64ThreadEvidence` reports the
initial thread's deferrals beside the created threads' and the longest run of
consecutive deferrals one fault had.

#### Reading a kernel trap at vector `0x1000`

A `KernelTrap` stop at vector `0x1000` identifies one of three exits in its vector word (`Ia64StopExit`,
`Source/Platform/Ia64/Diagnostics/Ia64Stop.hpp`). The word is the snapshot's
`VectorOffset`, printed as `VECTOR=` and reported as the stop's P1: the
vector offset in bits 0–15, the exit in bits 16–19 and, after the bridge,
its `Ia64UserFaultBridgeStatus` in bits 20–27.

| Vector word | Exit |
| --- | --- |
| `0x11000` (`0x10c00` for an instruction miss) | a validity check of the alternate TLB refill handler (an unexpected PSR or ISR, an unusable `ar.k3` context, binding word or ledger publication), or a PL0 ledger miss |
| `0x21000` | a kernel invariant check of the user-fault entry before the bridge call (selector, PSRs, IFS, slot, ISR, context, frame state, storage, covered IFS, RSE distance) |
| `0x31000 + (status << 20)` | the bridge returned `status`, such as `0x5` (`HandlerFailStop`, `0x531000`); with `status` zero a restore check after a successful bridge failed |
| `0x1000` | a CPU that found another's stop pending at the entry's poll |

The user-exception entry's stops carry their own vector (`0x5400`, or
`0x0c00` behind an instruction-miss escape) with the same exits.
`AsteriaIa64StopUserExit` tells the entry exits apart by `PSR.ic`, which the
entries clear before the bridge call and set from it until their restore,
whose checks all precede clearing it again; r8 then holds the bridge status.

All record the live `cr.iip`, `cr.ipsr`, `cr.isr` and `cr.ifa`. For the
refill and entry exits, and for a bridge exit whose handler returned without
switching threads, those are the faulting access itself: with collection
off, and in the bridge, which runs with `PSR.i` clear and touches only
kernel memory the translation registers map, nothing interrupts in between.
After a switch they are the last interruption of whatever ran.

## Verification

Hosted tests cover all 26 vectors and slots, defined/undefined IFA, trap restart,
break immediates, RSE frame states, bridge transitions/rejections and interrupt
handoff. Goldens under `Tests/Golden/Ia64/{Interrupt,Diagnostics,Guest}` pin the
entry, claim, stop and guest-generator leaves. `Ia64UserFaultEntry*`,
`Ia64UserInterruptEntry*`, `Ia64UserExceptionEntry*`, `Ia64UserInterruptClaim*`,
`Ia64ExternalPreemptionDispatchContinuations`, `Ia64UserBreakDispatch`,
`Ia64StopUserExit`, `Ia64GuestFaultGenerators` and `Ia64UserSpinnerState`
properties check symbol/relocation, admission, complete image, RSE ordering,
restore, exit and GP/vector placement contracts and reject their mutants.
Refill tests check these escape paths.

Decoding follows Itanium SDM Vol. 2 part I tables 8-1/8-2 and §8.3:
ISR.code is zero for ordinary memory-management accesses, ISR.ei identifies
the faulting slot, and IFA/IIPA/IIM are valid only where the inventory specifies.
Capture relies on `cover` resetting rename bases (Vol. 2 §6.11.1; Vol. 3).

Native pager probes validate vector, cause, access, origin, address, instruction,
ISR.code and slot. They cover terminating faults (break 0/slot 1, privileged
operation, NaT, unaligned and access-right faults), successful `chk.s` recovery,
lazy RX instruction mapping and concurrent-fault deferral. Twelve 82-register
frames cross four lazy backing-store pages; mandatory spills and incomplete-
frame fills must resolve and restore every value. Deferral tests cover counts
above 512 yields, saturation, reset and failed delivery.

The created-thread spinner canaries integer, branch, predicate, floating-point
and RSE state, counts for 50 ITC time slices without memory accesses/syscalls,
and breaks with 0x1234 for preservation or 0x4321 for loss. The runner observes
its timer run stops before the initial image's idle wait, on one through four
CPUs; user threads remain on the BSP. This proves actual timer preemption
without consuming the spinner's phase in an earlier observer wait.
