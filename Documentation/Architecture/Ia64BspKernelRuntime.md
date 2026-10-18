<!-- SPDX-License-Identifier: Apache-2.0 -->

# IA-64 BSP Kernel Runtime

## Scope

`Ia64BspKernelRuntime` owns boot-lifetime BSP scheduling and the initial-image
worker through the common processor run loop. The default path launches
`MinimalUserImage` from the retained boot-file region, admits the attached
[boot pager](BootPager.md) first, and schedules boot and created threads through
native user time slices and the idle continuation. A mutually exclusive
diagnostic path uses the worker for the
resumable synthetic `Ia64UserExecutionProbe` and one exact WaitSet block/wake/
readmission cycle. Cooperative AP service and the optional native-preemption
probe have separate admission and execution paths.

The translated boot continuation also runs a separate AP-runtime sequence
before this owner's timer probe and user dispatch. That sequence shares the
same runtime root and IVT but is owned by the AP mapping, release, and context
objects described below; it does not turn `Ia64BspKernelRuntime` into a
multiprocessor scheduler.

The owner does not allocate, invent object identities, or construct another
BSP address-space controller. It borrows both exact roots exported by
`Ia64BspRuntimeMemory`: the common bootstrap/idle root and the distinct
private initial-component root. Both views retain the same BSP processor and
controller; only the common object is the controller's idle root.

## Lifetime and storage

The class is mapping-page aligned, has a mapping-page-multiple extent, is
immovable, and is intended for placement construction in mapped boot-lifetime
storage. Its complete numerical extent must be covered by supervisor-readable
and supervisor-writable Region 0 mappings in the borrowed root.

Two distinct embedded, page-aligned ranges are reserved for the worker:

- a 16-KiB downward-growing memory stack;
- a 16-KiB RSE backing store.

The owner also retains an initialized `Ia64LocalTimer` that starts masked, one
registered Timer and Notification shared by the two local-timer sequences, an
empty `TimerQueue`, a retained `TimerNotificationDelivery`, a fixed-capacity
`DeferredTimerWorker`, one WaitSet and its exact Thread dispatcher, a `RevocationRegistry`,
an initially empty `CapabilitySpace` keyed with the boot's `BootstrapSpace`
validation key (derived from `BootInfo::Entropy` by the boot application before
construction; the PCI/ECAM runtime's space gets the `DeviceSpace` key), one
user-capable `ProtectionDomain`
bound to that space, one `Thread`, one `SchedulingContext`, fresh idle and
prepared worker `Ia64KernelContext` objects, a hardware-deadline
`ProcessorRunController`, and an `Ia64ProcessorRunLoop`. The controller and run
loop are placement-constructed only after the exact BSP root is borrowed.

The synthetic user-probe path additionally retains a value copy of the BSP's decoded
48-byte `PalVirtualMemoryCapabilities`, an `Ia64LocalTlbInvalidator`, inline
storage for one placement-constructed `TlbShootdownDispatcher`, one
placement-constructed single-message `Endpoint`, a stack-only direct
user-memory backend, processor-local system-call scratch, the native bridge
context, WaitSet syscall ingress/dispatcher and IA-64 synchronous continuation,
the three minted capability handles, the 120-byte Endpoint/WaitSet/frame
bootstrap record, the prepared launch request, compact terminal diagnostics,
and non-owning boot-lifetime pointers to the exact runtime-memory owner,
hierarchy-root resource domain, issuer, and staged probe owner. The dedicated
system-call frame, 16-KiB memory stack, and 32-KiB RSE store are retained by
`Ia64BspRuntimeMemory`; this owner publishes their validated entry context
after it constructs the run loop and user-memory bridge.

The external probe owner is itself immovable boot-lifetime supervisor state.
Its complete object range, including the embedded supervisor continuation that
the native launch leaf writes after `cover`, must remain covered by continuous
supervisor-readable and supervisor-writable mappings in the admitted root.

The worker entry uses a two-word P64 descriptor embedded in this boot-lifetime
owner. During preparation, a target leaf writes the raw `WorkerEntry` bundle
address and the live image `gp` into those retained words. The raw entry uses
an `R_IA64_IMM64` relocation; it does not create an
`R_IA64_FPTR64LSB`, `.opd`, `.got`, or `R_IA64_LTOFF_FPTR22` dependency.
This is essential because the firmware entry contract reserves the sole
linked `.opd` descriptor for `EfiMain`.

Once `Prepare` reaches any side effect, the owner and every passed or borrowed
dependency must outlive all continuations. An accepted failure is terminal and
does not attempt to unregister, destroy, or reuse partially initialized kernel
objects. Destruction is safe only when preparation was rejected during its
read-only root preflight.

## Preparation contract

The public preparation entry is:

```cpp
Prepare(
    Ia64BspRuntimeMemory& runtimeMemory,
    ResourceDomainObjectId resourceDomainIdentity,
    ResourceDomain& resourceDomain,
    RootCapabilityIssuer& issuer,
    const PalVirtualMemoryCapabilities& palVirtualMemoryCapabilities,
    UInt8 timerVector);
```

The caller supplies a real ready hierarchy-root `ResourceDomain`, its exact
registered typed identity, and the issuer for the same `ObjectRegistry` passed
to the runtime constructor. Preparation validates the borrowed root and owner
mapping, initializes the timer while it remains masked and disarmed, creates a
user-capable protection domain for the borrowed address space and initially
empty capability space, registers the
protection domain, local-probe Timer, local-probe Notification, thread, and
scheduling context, binds the scheduling context to the thread, and prepares
the worker context with the implemented virtual address bit from the
value-copied PAL record. Both local-probe objects are charged to the supplied
root resource domain and retain exact generation-bearing identities. It then
publishes the exact processor, run-loop, frame, memory-stack, and RSE entry
context through the runtime IVT's release-published break-transport word. The
thread remains `Dormant`, the capability space remains empty, and no work is
queued, so every failed preparation is deterministically non-runnable.

The returned typed summary names the exact address space, protection domain,
thread, scheduling context, processor, and timer vector. Detailed compact
failure information remains in the owner to avoid copying large common-layer
failure records on IA-64's bounded memory stack.

## Synthetic-user-probe configuration

`PrepareUserExecutionProbe(Ia64UserExecutionProbe&)` is distinct from
`Prepare` and is accepted only while the kernel runtime is exactly `Prepared`.
The caller must already have staged the supplied owner before translation
takeover. The runtime then:

1. overflow-checks the complete external owner range and proves continuous
   supervisor-readable/writable coverage before any irreversible mapping;
2. revalidates that the admitted root is resident only on the exact BSP;
3. initializes its `Ia64LocalTlbInvalidator` from the complete retained PAL
   virtual-memory capabilities, including the real purge-page-size mask;
4. placement-constructs an allocation-free `TlbShootdownDispatcher` bound to
   the exact BSP `CpuId`;
5. passes the retained runtime memory, registry, issuer, registered root
   resource-domain identity and object, and the runtime's exact registered BSP
   `ThreadId` to the probe owner;
6. supplies the complete embedded 16-KiB worker memory-stack and RSE ranges as
   the supervisor continuation ranges;
7. retains the validated launch request only after all three private-root
   `FaultToken` / `AddressSpace` / shootdown mapping transactions commit; and
8. only after that mapping and launch-record readiness, creates one revocation
   root and first mints an infinite Inspect-only capability to the registered
   ProtectionDomain, placing its selector/authenticator in launch
   `Register4`/`Register5` (`Register6`/`Register7` stay zero: the probe
   receives no DiagnosticChannel);
9. constructs and registers one Endpoint with capacity for exactly one
   attachment-free 88-byte message, binds the probe capability space as its
   receiver and the root resource domain as its charge owner, then mints one
   infinite `Invoke|Read` capability sharing the same revocation root; and
10. initializes one processor-local IA-64 synchronous WaitSet continuation over
    the retained syscall frame/stack/RSE and the probe's supervisor continuation;
11. mints an infinite `Wait` capability for the runtime-owned WaitSet at
    selector `0x102`; and
12. encodes one canonical asynchronous `AIPC` frame, stages its Endpoint and
    WaitSet handles plus bytes in the mapped user stack, clears syscall scratch,
    and validates the exact pre-launch Endpoint queue state
    `{bytes=0, next=1, messages=0, attachments=0}`.

Before step 2, the runtime selects the private root through the common
`ProcessorAddressSpaceController`. Every success and every failure after that
selection passes through one bounded cleanup that restores the common idle
root. Bootstrap copies use the direct user virtual address only while the
private root is active; there is no assumed common-root alias. Timer evidence
goes to the kernel evidence record instead of user memory.
The ProtectionDomain and direct user-memory backend both name the private
AddressSpace identity.

At dispatch, `Ia64ProcessorRunLoop` derives the private root from the
ProtectionDomain binding and performs common-to-private selection before
`RunWorker`. Wait blocking uses the run loop's ordinary finish transaction to
return private-to-common. Readmission selects private again before the syscall
continuation is resumed. The one-shot `IA64-PRIVATE-USER-ROOT-WAIT-IDLE` and
`IA64-PRIVATE-USER-ROOT-RESUME` stage events are reported only after the checked
common-idle and private-resume states respectively.

Configuration never makes the worker ready or enqueues it. Its independent
state machine is exact: `Fresh`, `Prepared`, `Dispatching`,
`PreparationFailed`, or `DispatchFailed`. A call made before runtime
preparation returns `RuntimeNotPrepared` without changing `Fresh`. An accepted
configuration failure is terminal as `PreparationFailed`; it does not silently
fall back to, or alter the contract of, `DispatchBootstrapWorker`.
An unmapped or partially read-only owner is the side-effect-free
`InvalidProbePlacement` preflight rejection and leaves the state `Fresh`.

## Default initial-image preparation and dispatch

`PrepareInitialUserImage` is accepted only while the runtime is `Prepared` and
the synthetic probe remains `Fresh`. It passes the boot-lifetime memory and
mapping owners, the private-root identity, and the existing registry,
revocation registry, capability space, ProtectionDomain, and ResourceDomain to
the embedded `Ia64InitialUserImageLaunchOwner`. That owner revalidates the
complete image and mapping graph, selects the decoded entry, GP, user stack,
and user RSE ranges, and retains a fresh launch request over this runtime's
existing supervisor stack and RSE ranges.

Preparation first revalidates and captures the complete pinned image mapping
graph. It then reserves one boot-lifetime resource bundle for the Endpoint and
DiagnosticChannel metadata, two capabilities, and 88 queue bytes. After that
commit cut it constructs and registers one self-receiving, single-message boot
Endpoint and one DiagnosticChannel over the platform line sink (lines appear as
`USER:` lines on the debug channel), both charged to the root domain,
initializes the bounded `DirectUserMemoryAccess` backend from the captured
mappings, and asks the launch owner to mint exactly two infinite capabilities,
each in its own revocation root: `Inspect|Invoke|Read` to the Endpoint, whose
selector and authenticator travel in initial `r4` and `r5`, and
`Inspect|Write` to the DiagnosticChannel, in `r6` and `r7`. It does not create
another root, ProtectionDomain, Thread, or SchedulingContext, and it does not
initialize the synthetic Endpoint, WaitSet, or BSP local-timer proof. A
mapping or resource-reservation rejection before the commit cut is retryable;
a failure after an accepted reservation is terminal and retains that
boot-lifetime charge.

`DispatchInitialUserImage` makes the existing worker ready, binds and enqueues
it through the common run loop, and switches from the common idle root to the
private root. `RunInitialUserImageWorker` then calls the one-way
`Ia64UserModeLaunch::Launch` request. A successful native transition never
returns: the minimal image observes virtual BSS, queries the supplied Endpoint,
sends one canonical frame to it, receives and validates the exact loopback
delivery, and remains in its PL3 idle loop. A defined pre-commit launch
rejection is retained through the ordinary
run-loop failure path; a contradictory post-commit return fail-stops.

With the boot pager attached (`AttachPager`), the dispatch first admits the
pager's thread, so the pager enters PL3 and blocks in its WaitSet wait before
the initial image runs ([boot pager](BootPager.md)).
`RunInitialUserImageWorker` selects the initial image's entry contexts before
its launch. The idle continuation of `DispatchInitialUserImage` then becomes the
pager's wake loop: whenever both threads are blocked, it requires the expected
no-runnable-thread result, polls the pager's wait, and dispatches again. A
fault the initial image delivered completes that wait, and a timed wait
([wait timeout](SystemCallAbi.md#waitset-suspension-semantics)) expires: with
nothing runnable the loop polls the ITC up to the pager's deadline and polls
again, and the replenishment wait stops at that deadline if it comes first. A
poll that wakes nothing, with no deadline to wait for, fail-stops with
`ASTERIA:IA64-PAGER:IDLE-STALL`. Only an exited initial thread,
a rejected launch, leaves the loop for the original failure path.

Idle runs with `PSR.i` clear, so a device interrupt
([driver isolation](IommuDomain.md)) pends in the
local SAPIC meanwhile. With the INTx lines attached (`AttachDeviceInterrupts`),
both ITC waits poll `cr.irr1` for the device vectors `0x40`-`0x4F`: while one
is pending, `DeliverPendingDeviceInterrupts` reads `cr.ivr` in normal context,
writes EOI and dispatches the vector to its Interrupt, and the wait returns so
the loop dispatches whatever the Interrupt woke. A timer tick pending ahead of
a device vector is acknowledged as the PL3 claim acknowledges one that is not
due (idle polls the ITC for its own deadlines, and the next dispatch programs
the run's); without a device vector it stays pending for the next PL3 entry.
Every PL3 tick dispatches the device vectors pending at it too,
before it progresses the waits: a vector that became pending while `PSR.i`
was clear may still be pending when the next tick comes due, which outranks
it at `cr.ivr`, so without the tick's drain it would wait behind every tick.
With no
deadline but an Interrupt bound, idle waits for a device interrupt instead of
stalling, and a device interrupt that woke nothing dispatches again. A vector
that is neither a device's, the timer's nor spurious fail-stops with
`ASTERIA:IA64-INTERRUPT:FAIL:UNEXPECTED-VECTOR`.

The pager runtime also owns the pager's object management
([object management ABI](SystemCallAbi.md)): a management grant, a child of the boot ResourceDomain (charge
owner 2) whose limits fit two Endpoints of the largest queue, minted into start-block words 14-15 with Manage and
Inspect, and the `ObjectManager` the pager's bridge context names for ObjectCreate and ObjectDestroy
(`Ia64SystemCallBridgeContext::ObjectManagement`, 64 bytes; the initial image's bridge names none). Both
firmwares provide the management grant, and the pager runs its self-test before serving. The grant's
creation requires the object registry to still hold the pager channel's 16 FaultTokens and full Endpoint, Frame and
MemoryObject pools (the pager's preparation fails otherwise) and publishes `Ia64ObjectManagementEvidence`
with the charge of one Endpoint and the registered objects; every wait of the pager publishes it again with the pool's counts and the grant's charge, since the pager's
management calls all precede its waits.

## Created threads

With the pager attached, the boot constructs `Ia64UserThreadTable`, the IA-64 thread backend
([thread creation ABI](SystemCallAbi.md)), configures it with the pager's domain record (its
ProtectionDomain, image mappings, PagerChannel, object management and the pager as fault supervisor), which admits all
five created-thread entry slots' system-call and user-fault contexts, and gives it to the pager's object management and to
this runtime. A created thread's system calls reach its slot as the bridge's thread hook (ThreadExit ends its run
Exited, WaitSetWait is `InvalidConfiguration`, management calls reap first); its faults reach the supervisor with the
shared bounded deferral; its PL3 ticks go through `PreemptThread`, which ends its run Ready through the table and
counts the stop in `Ia64ThreadEvidence`. The initial-image idle loop reaps exited created threads on every pass; the
pager reaps before its own management calls, so the ObjectDestroy that reclaims a thread frees its slot.

The table also keeps the records of the domains the pager creates ([domain creation ABI](SystemCallAbi.md)): the ProtectionDomain pool records each with its supervision tag, a record-backed user-memory accessor
bound to the domain's AddressSpace, the pager's object management and the pager as supervisor, and retires it once no
slot holds a thread of the domain. `Start` reads the entry's function descriptor through the caller's record, whose
root is the active one, and runs the thread in the start's domain: the slot's bridge, admitted by address, takes that
domain's user memory, object management and pager resolution, its faults carry the tag, and its ThreadExit signals the
pager's readiness (`OnExit`). The run-time AddressSpaces those domains use are
[slots in the runtime memory](Ia64RuntimeAddressSpaceSlots.md).

## User preemption

Outside the synthetic-probe firmware, the initial thread and the boot pager
run in time slices (`MakeIa64UserSchedulingConfiguration`: a budget equal to a
20M-ITC-tick period, 50 ms at QEMU's 400-MHz ITC) and launch at PL3 with `PSR.i` set
([run-loop policy](ProcessorKernelRunLoop.md)). The deadline that ends a
slice arrives as a PL3 timer interrupt, which the
[PL3 interrupt path](Ia64UserFaultEntry.md#pl3-interrupts) claims (masking
the timer) and hands to the domain's handler; both handlers call
`PreemptUser`, on the interrupted thread's entry storage:

1. It acknowledges the masked deadline to `Ia64LocalTimer`
   (`AcknowledgeExternallyMaskedDeadline`), so its software state matches the
   hardware. The deadline controller keeps the deadline until a refresh or the
   next dispatch programs another one, which unmasks the timer.
2. It retries pager delivery and polls the pager's wait, as idle does.
3. It refreshes the active run's deadline. A deadline that did not fire leaves
   the thread running; one that fired ends the run Ready through
   `FinishAndDispatch`, on the initial thread's worker context or the pager's
   own (`Ia64PagerRuntime::Preempt`), returning once the thread is readmitted.
4. It selects the domain's entry contexts again.

A failure writes `ASTERIA:IA64-USER-TICK:FAIL:<stage>` before the entry
parks the processor. `Ia64UserPreemptionEvidence` records the ticks and the
runs they ended per boot domain; the dispatch publishes it once the boot
threads are admitted. The VM driver lets the initial image's CPU-bound idle
loop run out two slices before it stops the guest and requires the record to
show that two PL3 ticks ended its runs, which readmitted it each time. The kernel itself runs with `PSR.i` clear, so a deadline
that falls due in kernel context stays pending; the claim acknowledges it
without effect when it finally arrives at PL3 after a later deadline replaced
it.

The idle loop retries the two nonterminal dispatch results that time slices
add: an admission whose run stop passed before entry (bounded at 64 in a row
while no PL3 tick shows that a thread ran) and a dispatch that finds every
Ready thread out of budget. For the latter it first progresses the pager,
then polls the ITC up to the armed scheduler-retry deadline, since a PL0 tick
cannot reach idle. Any other result is written as
`ASTERIA:IA64-USER-DISPATCH:REJECTED:<detail>` before the dispatch fails.

## Shared-root AP runtime

On translated kernel entry, the boot continuation constructs
`Ia64ApplicationProcessorRuntimeMappingOwner`. It maps each AP's 32-KiB idle
memory stack, 32-KiB idle RSE store, 16-KiB worker
memory stack, and 16-KiB worker RSE store into a fixed Region-1 slot in the
already active root. The existing BSP root stays active while these append-only
BSP-local mapping transactions complete. The continuation then constructs this
BSP runtime and prepares either the default initial-image request or the
explicit synthetic-probe diagnostic.

The release coordinator is initialized from the terminal physical-takeover
snapshot. `Ia64ApplicationProcessorKernelRuntime` prepares one dedicated
idle/worker `Ia64KernelContext` pair for each AP, then the BSP releases CPUs 1,
2, and 3 sequentially. Each released AP:

1. claims its exact processor/generation release while still executing in
   physical Region 0;
2. installs its runtime-IVT context, admits the shared root, performs the
   physical-bootstrap RR activation, and initializes its per-CPU controller;
   its physical admission path then publishes the dedicated Region-0
   external-interrupt context and initially zero mailbox in
   `Ia64BspRuntimeMemory`, which the AP runtime retains as proof input;
3. pivots with local interrupts disabled from the physical takeover stack/RSE
   to its translated Region-1 idle stack/RSE aliases;
4. publishes runtime-entry evidence, initializes the masked `0xe8` local timer,
   installs and reads back
   `cr.tpr=0xd0`, arms one deadline, and native-polls exact mailbox sequence one
   with `PSR.ic=1` and controlled `PSR.i`; a deadline that passed before
   `cr.itm` was written arms nothing, so the AP samples again with twice the
   lead (see [Time bounds](#time-bounds-of-ap-start-up));
5. validates the acquired interrupt evidence, cancels and masks the timer, and
   publishes the passive timer proof;
6. with `PSR.i` clear, borrows the exact admitted shared root and AP-local
   address-space controller,
   constructs a fixed-storage kernel-only protection domain, thread, scheduling
   context, empty `TimerQueue`, `ProcessorRunController` in `HardwareDeadline`
   mode, and `Ia64ProcessorRunLoop`, then lets the portable run loop select the
   worker;
7. runs the retained worker at sequence one until it blocks through
   `FinishAndDispatch`, resumes idle with the canonical no-runnable result, and
   validates the empty queue, disarmed timer, and unchanged root;
8. enters the boot-lifetime `IdleServing` loop and polls a reusable
   acquire/release BSP-to-AP service mailbox with `PSR.ic=1` and `PSR.i=0`; one
   external BSP request is acquire-observed by the owning AP, which uses the
   retained worker's exact block token to wake and re-enqueue it, resumes the
   same native context at run sequence two, and lets the worker block again;
9. publishes immutable scheduler-service sequence-one completion and serving
   evidence and remains `IdleServing` for later requests.

The BSP acquire-observes the service proof, validates the register, passive
timer, portable scheduler, persistent-service, and processor-local transport
records against the kernel's own contracts, records what they observed in the
AP's slot of the evidence record, acknowledges the observed `0xe8` sequence,
and records the acknowledgement. It then publishes a separate one-shot
timer-delivery request. The AP handles that request in the `IdleServing`
context, where `PSR.i=0` except inside the bounded polling leaf; neither the
scheduler worker nor interrupt preemption performs the delivery. Boot-lifetime
per-AP `Timer`, `Notification`, `TimerQueue`,
`ProcessorDeadlineController`, and `DeferredTimerWorker` objects arm a finite
deadline, poll exact `0xe8` sequence two, and reject any record without
predecessor acknowledgement one. The idle context drains exactly one
Timer-to-Notification delivery, claims and commits the Notification, and
restores the Timer, queue, deadline controller, and local timer to empty or
disarmed state. It also proves the scheduler worker remains blocked with run
sequence two and the scheduler-service dispatch, wake, and service counters are
unchanged, then publishes immutable timer-delivery completion.

The BSP validates that completion against the kernel's own contracts
(receipts and snapshots that agree with each other; objects idle, empty,
disarmed, or blocked afterwards), records its sequences, counts, and interrupt
record, acknowledges the observed `0xe8` sequence, records the
acknowledgement, and publishes the AP's evidence slot before releasing the next
AP. Each earlier AP keeps
serving while later APs are admitted, and the common root remains resident on
CPUs 0 through 3. This is a persistent cooperative polling service; it does not
claim timer-driven AP scheduling, periodic policy, a general timer service,
general interrupt routing, general interrupt-driven preemption, IPI-driven
scheduler/service wake, migration, load balancing, general runtime cross-
processor shootdown, IPI-driven shootdown notification, hotplug, or teardown.
A separate fixed one-page shared-root mapping performed after AP admission
proves one polled resident-mask shootdown, cross-CPU in SMP2 through SMP4 and
BSP-local in SMP1; it is not part of this scheduler service. Each serving AP
polls the shootdown notifier at the start of every idle pass and, after a
Pending poll, runs one bounded mailbox pass. Notifications coalesce, and one
may stay latched after an earlier pass already drained its work, so a pass may
find no work, or the work of several back-to-back shootdowns, while the root
shows any shootdown sequence and acknowledgement state. The AP therefore
requires only that the pass complete every request it receives and that the
root be Ready, or ShootdownPending for exactly the enabled processors, before
and after it; the initiator, not the AP, completes each shootdown.

The kernel does not compare these sequences and counts with the values the
scenario expects. After every AP serves, it records the runtime processor count
and the release coordinator's serving mask, publishes the
`ApplicationProcessors` part of the evidence record, and reports
`ASTERIA:STAGE:AP-RUNTIME`. A violated contract, an AP failure record, or an AP
that never serves instead emits the unchanged `ASTERIA:AP-LOCAL-TIMER:FAIL`,
`ASTERIA:AP-SCHEDULER:FAIL`, `ASTERIA:AP-PERSISTENT-SCHEDULER:FAIL`,
`ASTERIA:AP-TIMER-DELIVERY:FAIL`, and `ASTERIA:AP-RUNTIME:FAIL` diagnostics
(SMP1: only the last) and halts.

The authoritative normal 512-MiB runs of the VM runner's `Ia64` suite cover every
processor count from one through four, and forced-relocation mode repeats
SMP4. The runner reads the evidence record through QMP and judges each AP's
slot against its own definition of the scenario (`ApplicationProcessorScenario`
in `Tools/Vm/AsteriaVm/Observers/Ia64Evidence.py`): the proof interrupt is
sequence one and acknowledged; the worker runs at sequences one and two and
stays bound with no queued timer; the service request and completion are
sequence one, echo the BSP's cookie, and complete on run two after two
dispatches, one wake, and one service, with the resumed worker's stack pointer
as the value; the delivery registers, arms, drains, and claims exactly once,
leaves the ready queue at position three, and arrives as interrupt sequence
two, which is acknowledged. The task priority, polling grace, vector,
interrupted-PSR contract, and polling leaf come from the build's headers and
ELF. The BSP's slot and the slots of absent processors stay empty, and the
serving mask names every AP; SMP1 performs an empty AP transaction. The default
path then checks the initial image's virtual BSS, successful Query/send/receive
loopback, and stable PL3 idle state. `Ia64SyntheticProbe` separately retains the
BSP local-timer, Query/send/receive/wait-resume, spill/refill, and richer
live-state oracles.

### Time bounds of AP start-up

The emulator the VM runner uses runs every CPU in turn on one host thread, for
100 ms each, and derives ITC (400 MHz on the Montecito model) from host time.
An AP can therefore lose hundreds of milliseconds, and more on a loaded host,
between any two instructions, while ITC keeps advancing; the BSP meanwhile
polls at whatever speed the host runs it. No verdict of AP start-up rests on
one such measure alone:

- The local-timer proof arms `sample + 10'000'000` ticks (25 ms). A deadline
  that has passed when `cr.itm` is written (a lead lost between the sample
  and the write) programs nothing that can fire: `ProgramDeadline` rejects it
  before the write, or the native adapter restores the masked, disarmed
  timer. The AP samples again with twice the lead, up to eight attempts
  (3.2 s); only a timer that rejects all of them fails the proof
  (`LocalTimerProgramRejected`). The adapter can leave an interrupt pending
  only if the AP stalls within its few instructions between unmasking
  `cr.itv` and rereading ITC; the retried proof then rejects that interrupt's
  evidence (`CounterBeforeDeadline`), a loud failure.
- The timer delivery arms its `Timer` exactly once, as its evidence records,
  so it cannot retry: its lead of 400'000'000 ticks (1 s) outlasts several
  round-robin rotations, and the delivery waits that long for its interrupt.
- After a deadline, the poll grace of 4'000'000'000 ticks (10 s) covers the
  emulator raising the interrupt and the AP's next turn, a hundred times what
  they take; a lost interrupt is still reported by the AP itself.
- Every BSP wait for AP work (rendezvous takeover, runtime start and serving,
  timer delivery, the shared-root shootdown acknowledgements) gives up only
  after both 10'000'000 polls, which the BSP can make only while it runs, and
  8'000'000'000 ticks (20 s), which a faster host cannot shorten
  (`Ia64CrossProcessorWaitBudget`). The interval is twice the AP's grace, so
  an AP's own failure record comes first, and short enough for the VM
  runner's boot budget.

The VM runner's IA-64 stage waits follow the same rule: a 60 s budget that
ends without the awaited stage but with new kernel lines is renewed, at most
four times (`GATE_RENEWALS` in `Tools/Vm/AsteriaVm/Drivers/Ia64Default.py`).

## Synthetic-probe BSP local-timer delivery proof

`RunLocalTimerInterruptProbe` is a separate one-shot diagnostic operation
between synthetic-probe configuration and dispatch. It requires the runtime and user-memory
backend to remain prepared while the worker is dormant and the caller keeps
`PSR.i` clear. It then:

1. publishes the runtime-memory owner's retained external-interrupt context
   and initially zero evidence record for the exact BSP;
2. samples ITC and overflow-checks a future deadline plus a bounded absolute
   timeout;
3. arms the retained registered Timer through the exact `TimerQueue` and asks
   the idle `ProcessorRunController` to install that queue deadline;
4. enters `AsteriaIa64PollForExternalInterrupt`, which enables delivery only
   while acquire-polling the record;
5. after the unchanged stackless handler masks ITV, acknowledges the interrupt,
   and returns through `rfi`, calls `TimerNotificationDelivery` in normal
   context to expire the exact Timer, signal the exact Notification, commit the
   queue claim, and acknowledge the Timer;
6. claims and commits that Notification, then refreshes the empty queue through
   the same controller to reconcile the handler's hardware mask with the
   retained software arm;
7. acquire-observes and validates the exact deadline, `ITC >= ITM`, polling-leaf
   IIP, the required interrupted-IPSR fields and legal RI while preserving the
   saved UP/AC/MFL/MFH fields, cleared `IFS.v`, processor, vector, and zero
   reserved bytes; and
8. acknowledges the interrupt and records the same 56-byte architecture
   record (with its acknowledgement) and an 80-byte common
   TimerQueue/Notification record in the `UserProbe` part of the kernel
   evidence record, whose publication word is stored last.

The second record names distinct nonzero Timer and Notification identities;
records arm, queue-claim, and Notification-claim sequence one; shares the exact
deadline with the architecture record; proves observation at or beyond that
deadline and no earlier than the handler's recorded ITC; and records the
complete armed-to-expired-to-disarmed,
signal-to-consumed, and armed-to-disarmed deadline transitions. Its `Sequence`
is assigned last only after all retained final-state checks succeed.

The fixed 80-byte layout is:

| Offset | Field |
| ---: | --- |
| 0 | `Sequence` |
| 8 | `TimerObjectIdentity` |
| 16 | `NotificationObjectIdentity` |
| 24 | `TimerArmSequence` |
| 32 | `DeadlineTicks` |
| 40 | `ObservedTicks` |
| 48 | `QueueClaimSequence` |
| 56 | `NotificationClaimSequence` |
| 64–77 | fourteen one-byte initial, claimed, signaled, and final-state fields |
| 78–79 | zero reserved bytes |

For this one-shot proof, bytes 64–79 are exactly
`01 02 01 03 01 01 01 00 00 01 00 00 00 00 00 00`.

The second timer mailbox (sequence, deadline, observed ITC, interrupted IIP,
interrupted IPSR, `IFS.v`, processor/vector plus six zero bytes, and the
acknowledged predecessor sequence) is validated for the next sequence and
recorded, before its acknowledgement, in the same evidence part. The VM runner
requires sequence two, predecessor one, a deadline after the first interrupt's
ITC, and an interrupted IPSR that the external entry's contract accepts. After exact
wake/readmission, PL3 records the 24-byte
syscall response `{error, tag, registration sequence}` at
`0x20000000020044D0` and accepts only
`{0, 0x5741495453455401, 1}`.

Completion of this pre-dispatch first-sequence operation leaves the local timer
Ready, masked, and disarmed, the retained Timer
and Notification Disarmed/Ready with no pending signal, the timer queue empty,
and the common controller deadline disarmed. A target timeout or any other
non-completion is terminal after a bounded queue/controller rollback attempt
because the path cannot prove that an interrupt is neither pending nor in
service. Hosted `NativeUnavailable` execution performs that rollback, retains
diagnostics, and leaves the independently prepared dispatch path usable; it
does not simulate privileged delivery.

This proof establishes one BSP external entry, acknowledgement, source
quiescence, `rfi` resume, controller-selected TimerQueue boundary, and
normal-context Notification delivery. Its successful commit also registers the
same Timer/Notification pair with the retained deferred worker and Notification
with the runtime WaitSet. It does not provide periodic ticks, scheduler
preemption, or general interrupt routing, and it is not evidence for the
separate AP idle-context delivery proof. The VM runner judges both interrupt
records against the external entry's interrupted-PSR contract (value under
mask, legal RI). It does not pin the saved UP/AC/MFL/MFH bits of this path
(under QEMU, first/direct non-RI IPSR `0x0000101008026018` with the seeded AC
and MFL state, second/wait `0x0000101008026010` with AC cleared and MFL
retained), which the entry accepts and preserves without interpreting.

## Native dispatch round trip

`DispatchBootstrapWorker` is a one-shot operation. It performs these common
run-loop transitions:

1. make the sole worker thread ready;
2. bind its native context and user-capable protection domain;
3. enqueue its scheduling context in the hardware-deadline controller;
4. call `DispatchFromIdle`, which switches from the fresh retained idle
   context to the prepared worker context;
5. capture the worker's native `r12`, `ar.bsp`, `ar.bspstore`, `ar.rnat`,
   `ar.rsc`, and PSR evidence;
6. finish the worker with disposition `Exited` and the fixed
   `Ia64BspKernelWorkerExitCode`, then switch back to the idle context.

The worker must never return from `FinishAndDispatch`; any such return enters a
private interrupt-disabled fail-stop loop. After idle resumes, success requires
the precise terminal observation: `NoRunnableThread`, a disarmed zero-deadline
receipt that records removal of the worker run-stop, no retained active run, no native or address-space
error, an exited worker with the fixed code, idle controller and run-loop
states, one retained binding, the exact borrowed root still active, and a
ready/masked/disarmed timer with an empty queue.

Captured `r12` must lie in the dedicated memory stack. Both `ar.bsp` and
`ar.bspstore` must lie in the dedicated RSE range, with `ar.bsp` not below
`ar.bspstore`. The worker must observe RSE lazy mode disabled, translation bits
enabled, and interruption delivery disabled. Dispatch reports success only
after all of these checks pass.

## Resumable synthetic-user-probe dispatch

`DispatchUserExecutionProbe` is a separate terminal diagnostic operation. It uses the
same common selection sequence as the bootstrap path: make the exact worker
ready, bind it to the prepared native context and user-capable protection
domain, enqueue its scheduling context, and call `DispatchFromIdle`. Only after
that common scheduler/run-loop selection reaches `RunWorker` does the runtime
call `Ia64UserModeLaunch::LaunchResumable` with the retained request.

The launch record has the final fixed contract:

- user IPSR `0x0000'1013'080E'2000`;
- observable current-PSR mask/value
  `0x0000'0010'080E'E002` / `0x0000'0010'0802'2000`;
- trusted current CPL 0, VM 0, instruction set 0, and register bank 1;
- initial user `r12` at the user memory-stack end minus 16 bytes;
- initial user `ar.rsc` value `0xC`;
- initial user `r1` equal to zero for this relocation-free probe; and
- the Inspect-only ProtectionDomain handle in initial `r4`/`r5`, and zero in
  `r6`/`r7`.

Native launch success enters PL3. Query, Endpoint send, and Endpoint receive
return directly through the synchronous break bridge. Syscall 8 instead commits
the exact WaitSet transaction and switches from the dedicated syscall stack/RSE
back to the retained worker supervisor continuation. The worker validates the
initial continuation epoch and active run correlation, arms the second Timer
sequence, commits the Thread as Blocked through `FinishWaitAndDispatch`, and
returns the processor to idle.

Idle processing requires the exact prepared dispatcher, blocked Thread and
SchedulingContext, waiting continuation, armed TimerQueue deadline, and
published external-interrupt context. It polls the second interrupt sequence,
drains the exact Timer through `DeferredTimerWorker`, refreshes the empty
deadline and thereby transiently masks/disarms the local timer, polls the
WaitSet dispatcher to wake the exact Thread, acknowledges the interrupt
evidence, and dispatches the readmitted worker. That admission programs the
scheduling-context run stop at `ITM=0xfffffffffffffffe` and unmasks
`ITV=0xe8`. The worker
consumes the exact WaitSet completion, prepares the tag/registration response,
and switches back to the retained syscall frame. Normal entry return then
restores PL3. The supervisor loop accepts subsequent exact wait epochs; the
current payload performs one and then enters its stopped terminal loop.

The defined pre-commit launch returns `UnsupportedProcessorState`,
`InvalidActiveSupervisorState`, and hosted-only
`NativeTransitionUnavailable` are retained as a terminal `LaunchRejected`
failure. For those returns the worker finishes through the common run loop with
the distinct `Ia64BspUserProbeLaunchFailureExitCode`, switches back to idle,
and lets the caller inspect the exact launch, scheduler, deadline, and
address-space diagnostics. An unknown adapter return, a continuation mismatch,
or any contradictory post-commit state fail-stops instead of fabricating a
resumable transaction.

## Boot integration

The BSP boot application must:

- reserve page-aligned boot-lifetime storage in the mapped writable nucleus;
- placement-construct the owner with the already initialized object registry;
- initialize and register the real resource hierarchy root first;
- before placement-constructing this owner, map the AP idle/worker storage into
  the shared root through the AP runtime mapping owner;
- pass the exact `Ia64BspRuntimeMemory`, root domain identity and storage,
  issuer, complete PAL virtual-memory capability record, and the dedicated
  external-entry timer vector `0xe8`;
- construct the boot-lifetime PCI ECAM owner after runtime preparation, adapt
  the copied optional MCFG topology, create its exact `DeviceRegion`
  authorities, and retain the first-present function-zero Read32 evidence;
- when selecting the synthetic diagnostic path, stage one boot-lifetime
  `Ia64UserExecutionProbe` and its allocator receipts before translation
  takeover, then pass that same owner by reference to
  `PrepareUserExecutionProbe` after `Prepare` succeeds;
- after the selected user-path preparation, initialize the AP release and context owners;
  for each released AP, publish and retain its dedicated Region-0 interrupt
  transport after physical admission and before pivot, publish one request on
  the reusable scheduler-service mailbox, then acquire-observe and validate the
  AP's `IdleServing` proof against the kernel's contracts, record it, and
  acknowledge the observed local-timer sequence; publish the separate
  timer-delivery request, validate its immutable completion, record it, and
  acknowledge the observed sequence; and only then publish the AP's evidence
  slot before releasing the next AP;
- only for the synthetic diagnostic, call `RunLocalTimerInterruptProbe` after
  every AP timer-delivery proof and before `DispatchUserExecutionProbe`;
- keep interruption delivery disabled at operation boundaries and exclude
  migration, re-entry, ISR access, and concurrent calls; only the bounded
  native local-timer polling leaf temporarily enables `PSR.i`, while each AP's
  scheduler-service polling loop keeps `PSR.ic=1` and `PSR.i=0`;
- keep the debug UART unmapped in the shared root. Stage events and bounded
  failure diagnostics after translation activation use physical UART access
  that temporarily disables interrupts, quiesces the RSE, clears `PSR.dt`, and
  restores the exact entry `PSR.dt`, `PSR.i`, and `ar.rsc` state;
- use `PrepareInitialUserImage` and `DispatchInitialUserImage` as the default
  terminal path. `ASTERIA_IA64_SYNTHETIC_USER_PROBE=1` instead selects
  `PrepareUserExecutionProbe`, the BSP local-timer proof, and
  `DispatchUserExecutionProbe`. `DispatchBootstrapWorker` and
  `[[noreturn]] AsteriaIa64KernelIdle()` remain retained diagnostic APIs.
- build with `ASTERIA_IA64_NATIVE_PREEMPTION_PROBE=1` (which requires the
  synthetic probe) to run `DispatchNativeExternalPreemptionProbe`. Its code,
  the sentinel and its landing, the coordinator, the fresh idle landing and
  the preparation and validation only they use, is compiled only in such a
  firmware (and in the hosted and compile-check builds, which leave the
  option unset): a production firmware, built with the option zero, does
  not contain it.

Default firmware reports the `INITIAL-USER-IMAGE-LAUNCH-PREPARED` and
`INITIAL-USER-IMAGE-LAUNCH` stages exactly once and in that order, and the VM
runner rejects the synthetic probe's private-root and preemption stages. In the
test firmware the guest program checks its BSS, Query/send/receive responses,
and the delivered frame through the system-call ABI and reports TAP; the runner
then requires three stable PL3 observations at
`AsteriaIa64MinimalUserImageIdle`. This checkpoint starts no general pager,
Component Manager, filesystem, or service layer. The [AMD64 runtime](Amd64BootRuntime.md) provides
the corresponding native user-launch and cooperative AP paths.

The opt-in native synthetic-probe integration attests completion from the stopped PL3 loop,
successful `CapabilityQuery`, Endpoint send, Endpoint receive, and resumed
WaitSet responses,
the exact canonical queued-and-delivered frame and trusted `AIPD` header,
mandatory parent RSE spill/refill words, result sentinels, translated processor
state, installed runtime IVA/RR/TR and user TC state, two correlated timer
evidence records, predecessor acknowledgement, and final active-run
`ITV=0xe8` plus run-stop `ITM=0xfffffffffffffffe`.
The BSP synthetic-probe proof does not depend on a post-translation stage event
or private object layout; the separately judged AP-runtime evidence attests the
earlier per-AP persistent-service proof.

The retained Timer is Disarmed and its queue is empty after the resumed wait,
but the local timer's transient masked/disarmed interval ends at readmission;
the stopped PL3 loop retains the active scheduling-context run-stop program.
The separately executed fixed shared-root mapping and optional native-
preemption scenario prove, respectively, one polled resident-mask shootdown
(cross-CPU in SMP2 through SMP4 and BSP-local in SMP1) and one BSP capture,
scheduler handoff, and resume transaction. Timer-driven AP scheduling,
periodic timer policy, general interrupt-driven preemption,
general multiprocessor scheduler policy, a general timer service, general
interrupt routing, IA-64 IPI-driven scheduler/service wake, migration, load
balancing, general runtime cross-processor shootdown, IPI-driven shootdown
notification, general runtime mapping, hotplug, teardown, and general thread
creation are unsupported by this boot integration. Current cross-ISA coverage is recorded
in [Implementation Status](../ImplementationStatus.md).
