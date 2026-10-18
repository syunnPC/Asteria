<!-- SPDX-License-Identifier: Apache-2.0 -->

# Terminal processor stops

AMD64 and IA-64 share terminal processor-stop handling, including the vocabulary,
record state machine, text formatting and screen layout. Lock ownership follows
the [spin-lock contract](LockOrder.md).

`StopCode.hpp` defines codes 1–9: BootStage, KernelTrap,
ApplicationProcessorStage, InvariantViolated, UnexpectedNmi, TestInjected,
ShootdownTimeout, LockOwnerLost and LastProcessorLost. ProcessorLost is not a
stop code. The table is the source for debug diagnostics, screen text and the
runner's expectations. BootStage carries a stable AMD64 stage ID in P1 and
detail in P2; AP stages carry their existing IDs in P1. KernelTrap carries
vector, CR2 and error code in P1–P3. DisplayBrokerDrained injection uses P1=1.

The stable BSP stage IDs in `Amd64StopStage.hpp` are:

| ID | Stage | ID | Stage |
| --- | --- | --- | --- |
| 1 | ApOwner | 18 | Reap |
| 2 | ApContext | 19 | DispatchLate |
| 3 | UserStack | 20 | WaitIdle |
| 4 | Root | 21 | WaitRearm |
| 5 | Apic | 22 | DispatchReturn |
| 6 | Endpoint | 23 | PagerIdle |
| 7 | WaitObjects | 24 | ThreadWait |
| 8 | Devices | 25 | IdleStall |
| 9 | Iommu | 26 | TimerCpu |
| 10 | Processors | 27 | UserTick |
| 11 | Shootdown | 28 | UserFault |
| 12 | Management | 29 | UserException |
| 13 | Pager | 30 | Syscall |
| 14 | Spinner | 31 | ResumeFrame |
| 15 | Thread | 32 | ResumeStack |
| 16 | Admission | 33 | NotPrepared |
| 17 | RegistryBudget | 34 | NoTrapOwner |

BSP IDs 35–38 name PagerStack, SpinnerStack, CreatedThreadStack and
MmioWindowInvariant. IDs 100–112 name NativePreemption Input,
PrepareAdmission, Dispatch, Arm, ResumeState, WorkerExit, CaptureState,
CaptureDeadline, StopRun, Readmission, IdleLanding, CompletionReturn and
CoordinatorOwner, respectively. IDs 200–210 name DeviceScanout, DeviceRead,
DeviceReadReceipt, DeviceIdentify, DeviceIdentifyReceipt, DeviceCapabilityKey,
DeviceEcam, DeviceAssignable, DeviceNicAssignable, DeviceAhciAssignable and
DeviceAhciBinding. Their P2 preserves the setup/command error detail. The MMIO
invariant uses InvariantViolated.

AP IDs 1–8 respectively name EntryAndTimer, ObjectsAndAdmission,
InitialDispatch, TimerDelivery, RunCounter, WorkerYield, ShootdownDrain and
ServiceDispatch.

## Publication and freeze

The AMD64 Stops sub-record occupies 592 bytes; its location follows the
versioned evidence layout. Nine UInt64 words hold publication, winner index,
online/confirmed/unconfirmed masks, uptime in TSC ticks, aperture root, aperture
base and aperture bytes. Four 112-byte processor slots follow. A slot contains
48 bytes of common record (UInt32 code, reserved UInt32, four UInt64 parameters,
UInt64 state), followed by eight UInt64 snapshot words: vector, error, RIP, CS,
RFLAGS, RSP, CR2 and CR3. State values are Running=0, Stopping=1, Frozen=2 and
Unconfirmed=3. DrawPublished, four retained DrawStates, and four per-CPU
StrayInterrupts counters follow the slots.

An owner writes its snapshot and payload, then release-publishes Stopping before
CAS of the global latch (zero or CPU index plus one). CPU identification uses
CPUID's xAPIC ID and the boot selection, avoiding GS even in a SWAPGS window.
The latch winner sends one physical-destination NMI to each online CPU except
itself: ICR high is the APIC ID shifted by 24, low is 0x400, without shorthand.
It uses LocalControllerPhysicalAddress, the uncached identity LAPIC mapping
present in every root. The 0x70000000 device window is repointed by device
access and is not a permanent LAPIC window. Never-started or mid-trampoline CPUs
are excluded. An AP reaching the online path after the latch parks; a second
check closes the race after its online publication. ICR availability is polled
at most 100000 times per destination, and peer records at most 2000000 times. No stop-path allocator or spin lock is entered.

Vector 2 is routed immediately to its terminal handler, before ordinary trap
ownership or syscall processing. Each CPU has its own two-page
IST2 NMI stack in pool pages 2296–2307, with unmapped guards. #MC uses IST3 on
the existing machine-check stack inside the CPU's 24-page extent; DF uses IST1.
The winner returns from an outside NMI, preserving its wait/draw. Other CPUs
with a latch set capture their own snapshot and release-publish
Frozen, and park with CLI/HLT. NMI blocking stays active: subsequent NMIs remain
pending, without repeated stack entry or IRET. With no latch, NMI initiates
UnexpectedNmi, with the raw port 0x61 byte in P1 (bit 7 SERR#, bit 6 IOCHK#).
An unknown APIC ID parks without touching any record. A losing stop parks too. Racing external NMIs that lose the latch join the
freeze without emitting another initiating STOP line. The winner marks its own record Frozen
before waiting. Only after the bound may it CAS an unanswered peer's state to
Unconfirmed; a late NMI can still publish Frozen. Confirmation masks preserve
the outcome at the bound. LINT1 is enabled for NMI delivery after descriptor/timer installation, so
external firmware and monitor NMIs enter the same vector-2 path. The online mask starts with BSP and gains each AP only
after that AP installs its descriptors and IST2.

Only the winner attempts the debug writer once, with CAS and no waiting;
losers publish records without taking the writer. The STOP and STOP-SNAPSHOT
lines and STOP-COMPLETE go through the boot-selected diagnostic sinks, including
the 16550 UART when present. Stop entry executes CLI before taking a snapshot;
a Stopping record with no latch proceeds to latch acquisition. The winner
release-publishes stop evidence and emits STOP-COMPLETE before drawing, so a
scanout fault preserves terminal evidence. Draw reads live CPU states once into
DrawStates and uses those retained values for rendering; DrawPublished is set
after rendering. The runner waits separately for that publication. Ordinary pre-exit
preparation rejections remain recoverable firmware diagnostics, and
HaltAfterFirmwareExit remains the raw parking primitive for firmware paths.

## Stop aperture and screen

Runtime pool pages 1780–2293 reserve a PDPT, a PD and 512 PTs (514 pages).
They are identity-mapped writable/NX in root 0, like the other reserved device
and table storage. Preparation initializes empty PTs without connecting the
aperture to the live PML4: the normal runtime mapper owns that root's tree and
rejects foreign table branches. Only the winner, after freezing or the bound,
switches CR3 to root 0, attaches PML4 slot 256, and fills consecutive 4 KiB PTEs
at 0xFFFF800000000000. Thus a page-aligned framebuffer need not be 2 MiB aligned;
its mode's full pitch times height times four bytes can exceed 4 MiB. The current
reservation supports up to 1 GiB, beyond supported GOP modes. Missing/invalid
records or larger modes retain serial/evidence diagnostics without a screen.

Leaves are supervisor writable NX. For write-combining scanout, PAT=1,
PCD=PWT=0 selects entry 4 (WC); a broker that selected UC fallback instead uses
PCD|PWT, PAT=0, entry 3 (UC). The boot-lifetime region's atomic last mapping
policy is latched with one lock-free CAS; a racing type selection skips drawing. The winner verifies `ExpectedInitialPatValue`
and installs it with the cache-disable/WBINVD/WRMSR sequence if needed, including
an early stop. A CR3 reload locally flushes the completed mapping. Each CPU
fences WC stores before publishing Frozen; drawing is fenced before DrawPublished.
There is no framebuffer mapping through the aperture before STOP.

The writer uses only 32-bit stores, packs RGBX or BGRX, clears the visible mode
and clips text on its full 8x16 character grid. It links only the public-domain
NetBSD bold8x16 printable ASCII bitmap (95 glyphs), sliced from the existing
user font by Tools/Build/WriteStopFont.py. White text on #008CA6 (RGB 0,140,166) shows the banner, 32-bit hex code and name,
parameters in two pairs (the PascalCase stage name next to P1 for BootStage
and ApplicationProcessorStage), CPU, vector/error, RIP/CS, RFLAGS, RSP, CR2/CR3, one
state/confirmation line per CPU, online mask, uptime, DMA not quiesced, and build
identity. Small modes clip trailing lines; no scrolling or allocation occurs.
The generated build identity is git short hash plus +dirty when appropriate;
it lives under the build tree and is not a golden or verification expectation.

## Runner and evidence

Case.expected_stop names the common table entry. Expected cases replace
lifecycle.no-kernel-failure with exactly one correctly named/coded STOP, payload
agreement with evidence, and each CPU frozen or explicitly unconfirmed. Other
cases reject every STOP. The two four-CPU Amd64Stops cases use the test firmware:
amd64-stop-probe stops at DisplayBrokerDrained after the broker's query fence and
teardown; the normal preset receives HMP nmi at that same display fence. Both
require all online CPUs confirmed frozen, inspect private IST2 stack tops and parked
CPU state, verify every aperture leaf is consecutive supervisor RW/NX with
the WC selector from the kernel PAT layout and no mapping beyond the mode extent, and compare all screen pixels against the
model derived from evidence, vocabulary and font, masking the build row.
The amd64-stop-early-probe preset (option 2) injects TestInjected with P1=2
at BeforeStartProcessors: four configured CPUs but only BSP online, proving
that no NMI reaches a firmware/trampoline IDT and the screen is drawn.

The timer masks unused thermal, performance-counter, CMCI (when implemented),
LINT0 and error LVT entries on every CPU; error has vector 0xFE. LINT1 becomes
NMI only after private descriptors are installed. APIC spurious vector 0xFF is
ignored without EOI; unclaimed external vectors 32–255 are counted and acknowledged
(the allocated device-vector range retains its own spurious counter). CPU
exceptions, unsupervised user faults, unexpected NMIs with no stop latch, fatal
boot/AP stages and invariants still stop the system.

The reader rejects evidence layouts it does not support.


## IA-64 records, cooperative freeze and physical screen

`Ia64StopStorage` is the first member of `Ia64BspRuntimeMemory`, inside its
existing locked runtime-memory page. `ar.k3` points to the CPU's 64-byte IVT
context; offset 48 holds the storage address and offset 8 the CPU index.
No new translation is installed. The residue proof remains **4 DTRs / 2 ITRs**,
including the planner's reserved firmware ITR; after native takeover the guest
has four active DTRs and one active ITR, the firmware ITR having been purged.

The storage starts with latch (0), online mask (8), reserved writer word (16),
start ITC (24), processor count (32), BootDisplay (40), and a 624-byte Stops
record (72). Each of four 128-byte CPU slots starts at `144 + cpu * 128`:
48 bytes common payload/state, then ten UInt64 snapshot words: vector word,
IIP, IPSR, ISR, IFA, IIM, IIPA, region register, RID and b0. The vector word
is the vector offset in bits 0–15; for the alternate TLB refill handlers and
the user-fault and user-exception entries, bits 16–19 name the exit
(`Ia64StopExit`) and bits 20–27 the user-fault bridge's status
([Ia64UserFaultEntry](Ia64UserFaultEntry.md#reading-a-kernel-trap-at-vector-0x1000)).
A KernelTrap stop reports it as P1. State lies at slot
+40 and the snapshot at +48. Four 24-byte terminal stack/RBS/GP descriptors
start at 696, followed by separately aligned 8 KiB memory stacks and 8 KiB
backing stores per CPU. The versioned IA-64 evidence layout determines the
Stops offset. The evidence copies only acquire-confirmed snapshots; an
unconfirmed CPU's payload may still be changing. DrawStates retain the bound's
state for the shared formatter and model.

Each CPU writes its own snapshot and payload before release-publishing Stopping
and CAS of the latch. The winner marks itself Frozen, samples the admitted
online mask and waits at most **400,000,000 ITC ticks** (one second on the
400 MHz itanium2-vpc). Confirmed and unconfirmed masks partition that sampled
mask. Marking Unconfirmed uses CAS and cannot erase a late confirmation. There
is no INIT, PMI, allocation or spin lock on this path. OS_INIT registration and
an interrupt-driven freeze remain unsupported.

Polls run in the AP external-interrupt poll leaf, BSP/AP idle loops, PL3 tick
entry, every syscall and fault entry, and the two alternate TLB refill handlers.
The raw polls acquire the latch through the locked ar.k3 context, preserving PR
when it is clear. A peer captures its own snapshot, release-publishes Frozen
and parks with PSR.i clear in a pause loop. Online admission checks the latch
both before and after release-publishing its bit. A raw terminal entry captures
on bank zero before touching a stack or RSE, then installs that CPU's fresh
terminal stack, backing store and GP before entering C++ with collection
restored. Normal snapshots clear PSR.ic while reading interruption registers,
keep the RSE lazy during that interval, and restore collection before eager RSE.
Normal stop snapshots use the return IIP; their interruption-register words
retain the last hardware interruption. Unknown CPU contexts park without
writing another CPU's record; physical bootstrap failures without a safe
translated context retain the raw parking primitive.

The winner attempts the diagnostic exclusion CAS exactly once, sharing it with
ordinary writers. Its dedicated identity-address word is materialized with
`movl`, never through GP, so the existing firmware/physical diagnostic callers
remain valid. STOP, STOP-SNAPSHOT and STOP-COMPLETE use the physical UART leaf;
on itanium2-vpc the debug-port channel captures that UART, distinct from the
firmware console's Serial.log. Published evidence survives a drawing failure.

The shared heading/footer and NetBSD bold8x16 printable ASCII subset draw white
on #008CA6. IA-64 adds five snapshot rows, with ITC uptime. Each glyph's sixteen
virtual font bytes are packed into two registers before calling the scanout
leaf. The leaves copy all stacked inputs first, mask interrupts, save ar.rsc
and force its mode lazy, set address bit 63, clear **only PSR.dt**, and serialize.
They issue **32-bit stores**, using registers for row/pixel traversal and
foreground/background values. `mf` precedes the stores and `mf.a` follows them;
PSR.dt, ar.rsc and PSR.i are restored in that order. PSR.it stays set; there is
no stack use, virtual data read or TR insertion during physical access.

`Ia64Stop.Injected` (`Ia64Stops`, preset `ia64-stop-probe`) injects TestInjected
with P1=1 at DisplayBrokerDrained, after the broker's final diagnostic borrow
has returned, on the next syscall. `Ia64Stop.Early` (`Ia64StopEarly`, preset
`ia64-stop-early-probe`) injects P1=2 before releasing APs into their runtime;
only CPU 0 is online. The driver checks the confirmation partition, frozen
records, parked PL0/PSR.i, unchanged active TR set, UART diagnostics and the
640x400 BGRX framebuffer against the ISA-aware model, and saves StopScreen.png.
Unit tests cover an unconfirmed peer; ordinary cases reject any STOP line.
Raw polling, terminal ownership/capture and physical scanout properties reject
mutants independently of the assembly goldens.

The stable IA-64 P1 stage ids (BootStage / ApplicationProcessorStage) are:

| ID | BSP stage |
| --- | --- |
| 1 | WaitCompletion |
| 2 | WaitResume |
| 3 | TimerPreparationRollback |
| 4 | TimerPollRollback |
| 5 | TimerPoll |
| 6 | TimerDelivery |
| 7 | TimerRefresh |
| 8 | TimerNotification |
| 9 | NativeWorkerReturn |
| 10 | NativeIdleState |
| 11 | NativeIdleDispatch |
| 12 | NativeIdleControl |
| 13 | DeviceInterrupt |
| 14 | Reap |
| 15 | DispatchLate |
| 16 | Replenishment |
| 17 | IdleStall |
| 18 | WorkerArgument |
| 19 | InitialRoot |
| 20 | InitialLaunch |
| 21 | InitialWorkerReturn |
| 22 | UserProbeRoot |
| 23 | UserProbeLaunch |
| 24 | UserProbeWait |
| 25 | UserProbeState |
| 26 | UserProbeContinuation |
| 27 | UserProbeBinding |
| 28 | UserProbeTimer |
| 29 | UserProbeReadmission |
| 30 | UserProbeDispatch |
| 31 | UserProbeEpoch |
| 32 | WorkerPurpose |
| 33 | WorkerReturn |
| 100 | KernelPreparation |
| 101 | DevicePublication |
| 102 | DeviceRead |
| 103 | DeviceBinding |
| 104 | Endpoint |
| 105 | Management |
| 106 | ThreadTable |
| 107 | Pager |
| 108 | ApplicationProcessors |
| 109 | InitialObjects |
| 110 | InitialMappings |
| 111 | UserPreparation |
| 112 | UserDispatch |
| 113 | SyntheticProbe |
| 114 | NativePreemption |
| 115 | ProcessorAdmission |

| ID | AP stage |
| --- | --- |
| 1 | IdleProcessor |
| 2 | ShootdownDrain |
| 3 | TimerServiceConflict |
| 4 | TimerServiceBegin |
| 5 | TimerServiceComplete |
| 6 | SchedulerService |
| 7 | LandingProcessor |
| 8 | LandingEntry |
| 9 | TimerProofBegin |
| 10 | TimerProofComplete |
| 11 | SchedulerPrepare |
| 12 | SchedulerDispatch |
| 13 | WorkerArgument |
| 14 | WorkerOwner |
| 15 | WorkerReserved |
| 16 | WorkerGeneration |
| 17 | WorkerPublication |
| 18 | WorkerFinish |
| 19 | ServiceCompletion |
