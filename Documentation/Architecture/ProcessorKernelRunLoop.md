<!-- SPDX-License-Identifier: Apache-2.0 -->

# Processor kernel run loop

## Scope

`ProcessorKernelRunLoop<TNativeContext>` connects one processor's completed
`ProcessorRunController` admission to a statically selected cooperative native
context. The target aliases instantiate the same common logic with
`Amd64KernelContext` or `Ia64KernelContext`; there is no virtual switch method,
function-pointer table, allocation, or firmware dependency.

This layer composes architecture-neutral processor-root selection with the
cooperative kernel call-frame switch. It still does not construct interrupt or
user-return frames, save extended CPU state, or claim untrusted user execution.

## Binding and ownership

The run loop holds at most the ready queue's 32 stable bindings from an
initialized `Thread` to one prepared native context and one Ready
`ProtectionDomain`. The idle context cannot be bound. Multiple Threads may
share a domain. A binding is added while the CPU is idle or while one of its
bound threads runs on it (a thread creating another,
[thread creation ABI](SystemCallAbi.md)), since adding only appends;
removal compacts the table and is accepted only while the CPU is idle. A
suspended dispatch finds its binding again when it resumes. The Thread, native
context, ProtectionDomain, its pinned AddressSpace, stacks, and IA-64 RSE storage
remain alive until the binding is removed.

Binding uniqueness is system-wide ownership supplied by the higher scheduler,
not a second global table in this low-level object. It serializes changes across
CPUs and binds each Thread and native context to at most one run loop. A context
may be rebound only before it has ever been selected. Once selected, its saved
return point is inside that exact run-loop transaction, so Ready and Blocked
contexts remain CPU-affine. Unbinding is then only for an Exited Thread whose
continuation and storage will be permanently retired. Live migration of an
entered kernel context is unsupported.

The owning CPU serializes every operation with local interrupts disabled.
`DispatchFromIdle` is called only by the exact idle context, while
`FinishAndDispatch` is called by the exact active Thread and native context.
The explicit identity arguments prevent one suspended stack from completing a
different Thread's retained scheduler transaction.

`GetActiveProtectionDomainBinding` exposes the exact domain pointer and
generation only while that bound context is Active. It verifies the live
Thread/context/domain tuple against the immutable binding table before
returning. `SystemCallIngress` consumes this result and then revalidates the
domain's live capability-space binding; callers cannot supply an alternate
authority object. The same result includes the exact AddressSpace storage and
generation selected by the active binding.

## Cooperative transition

`DispatchFromIdle` obtains an admission only after the common controller has
completed the configured dispatch policy. In the normal `HardwareDeadline`
mode this means that the absolute processor deadline is installed before the
run loop resolves the admitted Thread to its native context,
ProtectionDomain, and exact AddressSpace. It then asks the processor-local
address-space controller to activate that root before the native switch. An
activation rejected before its commit point rolls the admission back to
Ready. A missing or unprepared binding means that this processor can never
enter the candidate: the admission is rolled back and the candidate is also
removed from this processor's ready queue, so policy cannot select it again.
The Thread stays Ready without a placement, and the failure record names it
for its owner. A native switch rejection first restores the exact idle root
and only then rolls the admission back.

After root preparation, the run loop performs one final deadline validation.
Its successful clock sample is the architecture-neutral admission commit point;
the code does not claim that the later native-switch instruction occurs at the
same tick. Once committed, a deadline that latches during the switch is owned by
the target timer and interrupt-return path.

The controller also has a configured-once `CooperativeBootstrap` mode for the
single BSP bootstrap worker that brings up the remaining runtime services. It
uses the same run-loop binding, root activation, native switch, exact active
claim, and voluntary `FinishAndDispatch(Exited)` path, but it does not program
or cancel the processor-local timer. Its admission is tagged with the
configured mode and carries a zero-source, zero-deadline, non-reprogrammed
`Disarmed` receipt. Final entry validation checks that exact mode-tagged
scheduler claim and receipt without calling the deadline controller, then
reads the monotonic timer once after root activation. Entry is rejected and
the admission is restored to Ready when that sample is at or after the finite
`StopAtTicks`; clock exhaustion or reversal is terminal. This validation does
not program or cancel the timer. Consequently this mode makes no asynchronous
preemption guarantee and provides no evidence that timer interrupts or
interrupt-return handling are ready; the bootstrap worker must return control
voluntarily.

Cooperative bootstrap admission is deliberately narrow. It rejects scheduled,
pending, or claimed `TimerQueue` entries, an advertised next timer deadline,
and any scheduler replenishment retry. If a retry is discovered while the
scheduler searches ahead to an otherwise runnable worker, the selected claim
is returned to Ready before `DeferredWorkPending` is reported. An empty queue
reports `NoRunnableThread` with the same explicit `Disarmed` receipt and does
not cancel timer hardware. Normal `HardwareDeadline` selection, final deadline
validation, reprogramming, and idle cancellation retain their existing
behavior.

`FinishAndDispatch` first commits Ready, Blocked, or Exited accounting, then
selects the next admission without returning to ordinary work. It activates
the next binding's root before publishing that binding or switching native
contexts. Exact same-root selection is a validated no-op; different roots use
destination-activate then source-detach ordering. Selecting the same Ready
Thread requires no physical context switch. If no Thread is runnable, the
controller restores the dedicated idle root before the current context
switches to idle. The suspended call returns only after that Thread's exact
root has again been activated. A Blocked completion returns only after its
exact Wake token succeeds and the Thread is re-admitted. An Exited completion
never returns under the valid lifecycle.

Every selection of the idle root (going idle, restoring it after a rejected
entry or native switch, completing an external stop or page fault, and
routing to a retained frame) retries a rejection that reports
`AdmissionRollbackSafe`, up to `MaximumIdleRootTransitionAttempts` (65,536)
attempts in all. Such a rejection proves that the current root and every
reservation are unchanged. Its usual cause is ordinary contention for the
shared idle root's single transition reservation: another CPU entering or
leaving idle, another CPU's same-root no-op, or a mapping on that root. Every
such holder completes without waiting for this CPU, and no lock is held
between attempts. A rejection that is not rollback-safe is not retried, and
one that persists through the bound, such as a shootdown on the shared root
that needs this CPU's acknowledgement, remains terminal.

Every root used by this corridor maps the nucleus text and writable data, the
root-switch implementation, the outgoing C++ stack until activation returns,
and both native-context storage ranges at invariant supervisor addresses. For
IA-64 this includes each kernel RSE backing store. The current canonical IA-64
seed does not supply this general runtime-root contract. It covers only the BSP
image, memory stack, RSE, IVT, and translation storage, all wholly in region 0
and `Global`; RR0 alone is shared at RID `0x2000`. Preparation proves locked DTR
coverage for each of those seed mappings and locked ITR coverage for executable
chunks. The separate BSP runtime-memory owner uses that exact seed to
register and bind a common bootstrap/idle `AddressSpace` and a distinct private
initial-component `AddressSpace`. The processor address-space controller
initially admits only the common root; the owner verifies both exact private
IA-64 ledger bindings and zero private-root residency. The post-
`ExitBootServices` continuation executes that transaction and enables
translation. The translated BSP kernel continuation borrows both roots,
prepares the default initial-image request over the existing private-root
ProtectionDomain, then lets this run loop derive and select that root for the
one-way `MinimalUserImage` launch. The explicit synthetic diagnostic instead
prepares its three mappings, restores the common root for the direct local-
timer proof, uses the run loop for resumable launch, restores the common idle
root when Wait blocks, and selects the private root again before readmitting and
resuming the exact worker after the second timer-driven WaitSet wake.
The cooperative worker round trip and `AsteriaIa64KernelIdle` remain retained
legacy diagnostics rather than the production boot flow. The
implemented SAL/Local-SAPIC path completes AP physical stack/RSE,
fail-stop IVA, PAL-capability takeover, and sealed initial RR/TR installation
before parking. The translated boot path admits the shared runtime
IVT/root on CPUs 1 through 3, pivots each AP to mapped Region-1 idle storage,
and performs one bounded exact-sequence-one local-timer proof. With `PSR.i`
clear, the AP then borrows the exact admitted shared root/controller and enters
this portable run loop with a fixed-storage kernel-only `ProtectionDomain`,
`Thread`, `SchedulingContext`, empty `TimerQueue`, and
`ProcessorRunController` in `HardwareDeadline` mode. One retained worker blocks
after admission at run sequence one. The AP remains in a boot-lifetime
`IdleServing` loop, polling a reusable acquire/release BSP-to-AP mailbox with
`PSR.ic=1` and `PSR.i=0`. The owning AP acquire-observes one external request
and uses the retained worker's exact block token to wake and re-enqueue it,
resumes the same native context at sequence two, and lets it block again. AP
timer delivery follows as a separate one-shot BSP request handled directly by
the cooperatively polling `IdleServing` context. The per-AP common timer graph
drains one Timer-to-Notification delivery and returns to empty/disarmed while
the run-loop worker stays blocked with run sequence two and the scheduler-
service counters remain unchanged. Timer-driven AP scheduling, periodic policy,
preemptive dispatch, a general timer service, general interrupt routing, IPI
wake, migration, load balancing, cross-processor shootdown, general mapping,
hotplug, and teardown remain unimplemented or gated.

## Retained external-frame transition

The common run loop has a separate, normal-context completion path for a PL0
worker interrupted at its exact one-shot run-stop deadline. The architecture
continuation supplies the captured counter value, exact active Thread and
SchedulingContext generations, CPU, run sequence, programmed deadline, a
nonzero target frame token, and evidence that the fixed timer vector was
masked, read back, and acknowledged. The controller accepts only
`HardwareDeadline`, exactly `RunStop`, an empty and settled `TimerQueue`, and no
scheduler-retry source. Accounting uses the captured time with no second clock
read. Every mismatch is terminal and retains the active claim and diagnostics;
none is converted into ordinary deadline rollback or retry.

Successful Ready completion gives the worker a fresh FIFO position, switches
to the idle root, and stores its retained-frame observation in the exact native
binding. Each binding can own one frame, bounded by the 32-binding capacity, so
A may remain retained while B runs and is separately stopped. Only one resume
admission can be selected or pending on a processor at a time. Normal scheduler
priority and FIFO policy select retained owners; they are neither skipped nor
forced ahead of peers.

Admission classifies the selected binding before `IsPrepared` and before any
cooperative switch. From idle, a retained A has its root activated, admission
finally validated, and controller state made Active before an `ExternalFrame`
receipt is returned with zero `SwitchTo(A)` calls. When active B selects
retained A, the controller keeps A's exact admission while B switches only to
the idle/coordinator context. The run loop records the distinct
`ExternalFramePending` state. Either the suspended idle dispatch or a fresh
`DispatchFromIdle` coordinator entry then activates A's root and returns the
same kind of action without perturbing A's scheduler claim.

`DispatchRetainedExternalFrameFromCoordinator` is the narrow entry for a live
target coordinator whose only placement is one retained owner; any other scope
is terminal before dispatch. Its admission is programmed, activated, and
finally validated like one from idle, and it is rolled back like one: when the
processor stalls past the admitted stop before the timer is programmed or
before the final entry check, the admission returns to Ready (with the idle
root restored), the frame stays retained, the run loop stays Idle, and the call
returns false. The caller retries once the owner's budget is replenished. Only
a failure that retains the run or faults the controller is terminal.

The receipt binds the target token, a monotonically advancing nonzero
common-owned resume generation, native binding generation, stopped run
sequence, and new admission run sequence and deadline. Generation selection is
seeded by the exact idle context's lifetime generation, so a legally reused
context cannot alias a restarted run-loop counter. The lifetime counter never
wraps: generation `UINT64_MAX` is the last consumable receipt. Exhaustion is
fail-stop and is detected before scheduler, timer, root, or native mutation.
If that last receipt is consumed while another binding still owns a frame, the
remaining marker stays exact and queryable but cannot be admitted; removal,
unbind, and retry remain forbidden. Likewise, if the idle context already
publishes `UINT64_MAX` when a new hardware frame arrives, the run loop first
records that exact frame ownership and then faults before controller settlement
or root/timer/native work. This bounded 2^64-lifetime terminal case avoids ABA
or silent ownership loss without a larger generation-reservation subsystem.

Before consuming the action, the run loop asks its exact idle-context object to
save and publish a fresh dedicated coordinator continuation. Restoring that
image must enter normal idle dispatch; it must never return into the old
refresh/commit/`rfi` path. This handshake is required even for direct
idle-to-A selection: there may be no suspended common dispatch frame, and a
later A-to-idle or A-to-retained-B route must not switch to a stale idle image.
The architecture then calls `CommitExternalFrameResume` with interrupts
disabled. Commit queries the same context object and requires its exact action
generation. A stale, replayed, missing, or rejected refresh is terminal and
retains the exact frame, admission, root, and diagnostics. A successful commit
clears only that selected binding's ownership; its next operation must restore
the exact architecture frame and perform the native interrupt return (`rfi` or
`IRETQ`), or fail-stop.

While a selected action awaits commit and return, all ordinary completion,
Wait, fault, cooperative dispatch, placement, and second-stop mutations are
rejected before accounting, timer, queue, root, or native-switch work. A
composed run-loop owner uses `EnqueueBoundThread` to record the exact accepted
Thread and SchedulingContext identities. It later uses `RemoveBoundThread`,
which rejects retained, selected, or active ownership before internally
calling the controller with the stored Thread identity. Direct controller
enqueue/removal is valid only for a standalone controller and is outside the
composed contract. Removal also rejects an identity that was never accepted by
the composed enqueue operation and rejects every binding while the processor
is in `ExternalFramePending`; neither case changes placement or FIFO state.
`Unbind` and run-loop destruction likewise require retained and pending
ownership to be empty.
Root-activation and final-deadline rollback retain the marker while taking the
normal fresh FIFO position.

This common state machine enforces policy, ownership, failure atomicity, and the
absence of a cooperative switch to retained destinations. ISA owners supply
frame transport, coordinator storage, idle-continuation replacement, and native
return. Both [IA-64](Ia64BspKernelRuntime.md) and [AMD64](Amd64BootRuntime.md)
have one-shot native proofs restricted to fixed lock-free kernel loops. General
kernel preemption still requires a preempt-disable and lock discipline.

`FinishFaultAndDispatch` is the page-fault specialization. It completes the
exact Thread as Blocked, passes the resulting generation to a prepared
`PageFaultIngress`, and only then selects the next admission. This closes the
gap between the irreversible scheduler transition and pager publication without
resuming a faulting Thread on token-pool or Endpoint pressure.

Failures use compact caller-owned output records so IA-64 does not copy the
controller's complete nested diagnostic state on its bounded memory stack.
The record retains the common error, deadline receipt, active-run flag, native
error, and the processor-address-space failure classification. Detailed live
state remains available from the owning controllers. For both transition
calls, only the receipt is meaningful on a true return and only the failure
record is meaningful on a false return. This matters because a suspended call
can observe several intervening transitions before it eventually resumes. A
pre-commit activation rejection is rollback-safe. An unknown native commit,
post-commit detach failure, idle-root restoration failure that survives the
bounded retry, or retained scheduler transition makes the run loop terminal.

## Verification

Hosted tests cover binding and root identity, rollback-safe activation and
native-switch rejection, bounded idle-root retry, deadline expiry during entry,
commit-unknown retention, cooperative bootstrap, and Call publication under
reference pressure. External-frame tests check independent retained owners,
policy selection, admission rollback, exact commit generations, stale/replayed
receipts and mutation-free removal rejection.

AMD64 O0/O2 tests perform real cooperative switches between distinct registered
AddressSpaces, including Blocked wake, syscall authority, binding compaction and
Exited cleanup. Both target instantiations enforce the 512-byte stack limit and
link closure. IA-64 native coverage includes the BSP run loop and persistent AP
service described above; compile/closure checks alone do not prove AP user-root
admission.
