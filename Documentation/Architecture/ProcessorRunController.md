<!-- SPDX-License-Identifier: Apache-2.0 -->

# Processor Run Controller

## Scope

`ProcessorRunController` is the final architecture-neutral policy boundary for one CPU. It owns that CPU's
`ProcessorScheduler` and `ProcessorDeadlineController`, and refers to the exact `TimerQueue` that shares the local
timer's clock domain. It allocates no memory and performs no register or address-space switch.
`ProcessorKernelRunLoop` consumes this admission and performs the statically selected AMD64 or IA-64 cooperative
kernel-context switch.

The owning CPU serializes all operations with local interrupts disabled. The controller has no additional lock;
registry, queue, and scheduler operations beneath it may spin and are not callable from interrupt context or from a
same-CPU reentrant path.

## Dispatch transaction

In `HardwareDeadline` mode, `DispatchNext` first refuses entry while timer
delivery is Pending or Claimed. It then asks `ProcessorScheduler` for an
eligible run and programs the earliest nonzero absolute boundary from:

- the admitted run's budget/deadline stop;
- the earliest `TimerQueue` deadline; and
- the earliest replenishment of a higher-ranked deferred candidate.

The hardware-deadline admission is published only after the local one-shot
source accepts that boundary. An empty or wholly deferred
queue is a nonterminal result: the controller arms the next timer or replenishment wakeup, or disarms the source when
none exists, and returns the receipt with `NoRunnableThread`.

If the selected boundary is reached between scheduler admission and timer programming, the controller completes the
provisional run as Ready and returns without exposing it to architecture code. A hard timer-programming rejection
is terminal because the active scheduler transaction cannot safely execute without its authoritative preemption
boundary; the diagnostic records whether that transaction remains retained.
That retained flag follows the scheduler's exact active claim rather than its
public state, so a terminally Faulted scheduler cannot hide an owned thread or
context lifetime.

The scheduler dispatch start is passed to the deadline controller as the last
trusted clock observation. A timer sample below that start, or a final native-entry
sample below the installed receipt, is a hard clock reversal. The controller
faults and retains the exact scheduler claim; it does not call `Finish`, because
elapsed-time accounting cannot be trusted in the reversed clock domain.

## TimerQueue and interrupt reconciliation

`RefreshTimerQueueDeadline` re-installs the boundary after `TimerQueue` work
changed or a local-timer interrupt was acknowledged. While the controller is
Idle it supplies only the exact `TimerQueue` deadline to the embedded
`ProcessorDeadlineController`; active-run and scheduler-retry sources are zero.
This operation is available in both dispatch modes, including the cooperative
BSP bootstrap mode, without changing that mode's thread-admission contract.
Before cooperative scheduler mutation, dispatch also requires the embedded
deadline controller to have the exact zero-source Disarmed state. A consistent
Armed state returns the nonterminal `DeadlineRefreshRequired` without a timer
read or write; the caller first reconciles the empty queue and then retries. A
faulted or internally inconsistent deadline is terminal.

In the Idle state the operation refuses Pending or Claimed Timer delivery work
before touching the local timer, because such work must be resolved in normal
context rather than hidden behind a new hardware deadline. A Scheduled queue
entry programs its finite deadline and an empty queue cancels the prior
boundary. `DeadlineReached` is nonterminal so the caller can synchronously drain
the due queue; all other deadline failures fault the controller.

While a `HardwareDeadline` run is Active, a `TimerQueue` deadline or scheduler
retry earlier than the run stop consumes the one-shot when it fires, and the
target's entry leaves the local timer masked. The normal-context continuation
that acknowledged the interrupt, typically after draining the queue, calls the
same operation, which installs the minimum of the sources that remain: the
active run stop, the current `TimerQueue` deadline, and the admission's
scheduler retry. A retry that has already fired is omitted, because this run
cannot be preempted there and its replenished candidate is considered at the
next dispatch. A reached run stop or `TimerQueue` deadline is the nonterminal
`DeadlineReached`, reported without a hardware write so the caller completes
the run or drains the queue and refreshes again. Pending or Claimed delivery
work does not prevent the refresh, since the run's boundaries must stay armed
while that work is retried. The installed receipt replaces the active
admission's deadline, so a later external stop validates the boundary actually
armed; the refresh is therefore only for an admission the architecture has
already consumed by entering its context or committing its external-frame
resume. Other deadline failures fault the controller and retain the active run.
A `CooperativeBootstrap` run owns no hardware boundary, so its refresh stays
`AlreadyActive`, and a Faulted controller refuses the operation.

## Completion and lifetime

`Finish` commits the selected run's Ready, Blocked, or Exited accounting through `ProcessorScheduler` and returns the
controller to Idle. Interrupts remain disabled until the caller immediately dispatches again or otherwise programs
an authoritative local deadline. Destruction requires Idle with a disarmed deadline and no retained transaction;
the initial invalid-CPU configuration state is the only faulted exception.

The registry, timer queue, local timer, contexts, and threads obey their existing lifetime contracts. The supplied
CPU identifies the owner of both the scheduler and local timer. Architecture code must consume a successful
admission immediately. Its final successful validation sample is the
architecture-neutral admission commit point: the stop must still be in the
future at that sample. Exact instruction entry after the sample is governed by
the already-installed target timer and interrupt handoff, not by a stronger
common-code timing promise. Cooperative-bootstrap validation applies the same
sampled `StopAtTicks` rule to its exact disarmed receipt and dispatch-start
observation, but performs no timer program or cancellation. Reaching the stop
is rollback-safe; exhaustion or reversal faults the controller and retains the
active claim.
The run loop enforces that saved-context handoff and composes the admitted binding with the processor-local
address-space controller before native entry. User-return composition remains outside this controller.

## Verification

Hosted tests cover run admission and completion, idle disarm, deferred replenishment, a higher-ranked retry that
preempts a lower-ranked run, TimerQueue precedence, pending timer delivery, idle queue refresh in both dispatch
modes, hardware-inert rejection of faulted, Pending, and Claimed idle states and of an active cooperative run,
active-run refresh after a fired TimerQueue deadline (reported until drained, then ending as an external stop at the
re-armed run stop) and after a fired scheduler retry, the admission-to-program race, and
clock reversal both before programming and at the sampled entry commit point, plus invalid CPU configuration. Debug uses
ASan/UBSan, Thread uses TSan, and IA-64 and AMD64 compile and link the
controller at O0 and O2 with the kernel stack budget enforced.
The AMD64 native run-loop test additionally drives one selected Thread through Block, Wake, resume, Exit, and both
idle returns on real independent stacks.
