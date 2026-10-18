<!-- SPDX-License-Identifier: Apache-2.0 -->

# Processor Deadline Controller

## Scope

`ProcessorDeadlineController` is the constant-time boundary between one CPU's
architecture-neutral scheduling policy and its local one-shot timer. It chooses
the earliest absolute tick from an active run stop, a `TimerQueue` deadline,
and a deferred scheduler retry. Zero means that a source is inactive; the
all-ones timestamp remains reserved and is never programmed.

The controller does not select a thread, expire a `Timer`, acknowledge an
interrupt, or contain an ISA register format. Those responsibilities remain in
`ProcessorScheduler`, `TimerQueue`, the deferred interrupt worker, and the
target HAL respectively.

## Programming transaction

Every source uses the exact clock domain exposed by the injected
`ILocalTimer`. The controller samples that clock, returns an already-due
earliest source for synchronous handling, and otherwise programs the selected
finite boundary. Equal values merge their source bits. An unchanged selected
value updates those bits without another hardware write.

`Program` may also receive the caller's last trusted observation. A newly
sampled tick below that baseline is `ClockReversed` and faults before any
hardware write. `ValidateCurrent` compares the installed receipt with the same
baseline and compares its final sample with the receipt observation. A reversal
is terminal because unsigned run-accounting differences would otherwise become
unbounded; it is distinct from the recoverable `DeadlineReached` result. The
controller also retains its greatest accepted sample, so later idle,
cancellation, and replacement transactions cannot silently move backward when
no scheduler-start baseline is present.

A successful `ValidateCurrent` sample is the architecture-neutral admission
commit point. It proves that the boundary was still in the future at that
sample; it cannot promise the timestamp of a later native-switch instruction.
After acceptance, the already-installed one-shot timer and the target interrupt
handoff own the boundary, including an event that latches while the native
switch is in progress.

Removing the last source cancels the local timer exactly once. The target HAL
must leave the previous deadline unchanged when it returns `NotReady` or
`InvalidDeadline`. On `DeadlineReached`, the previous deadline remains authoritative either as a restored hardware
arm or as an already-latched event after automatic hardware disarm. `DeadlineReached` from either
programming or cancellation is recoverable by synchronously servicing the
reported source; cancellation retains the old armed metadata until a later
retry succeeds. `NotReady` and `InvalidDeadline` still make the controller
terminal: although the old hardware deadline remains known, it does not
implement the newly selected authoritative boundary and can therefore miss a
required preemption. A register failure additionally has unknown hardware
state. All three non-recoverable cases become `Faulted` and publish no later
admission deadline.

Cancellation and later replacement prevent new assertion of the removed
deadline but cannot retract an interrupt already latched by the target
controller. Interrupt entry therefore acknowledges the event and the deferred
worker re-evaluates all three current sources; it never treats a hardware
interrupt as proof that the controller's last diagnostic source bits are still
due. For an active run this re-evaluation is
`ProcessorRunController::RefreshTimerQueueDeadline`: after a `TimerQueue`
deadline or scheduler retry fires before the run stop, it programs the minimum
of the sources that remain, so the run stop is never left unarmed.

## Concurrency and target boundary

`Program` is called only by the owning processor's serialized policy path with
local interrupts disabled. It has no lock, allocation, callback, or scan. The
HAL methods have the same bounded execution contract. Once a target connects
timer-interrupt delivery, entry merely captures and acknowledges the
architecture event; a deferred worker resolves the due source, accounts
preemption, drains `TimerQueue` claims, and refreshes the deadline from the
remaining sources.

The IA-64 BSP synthetic probe connects its ITC/ITM/ITV backend for masked/disarmed
initialization, ITC sampling, and hardware-deadline worker admission. The first
BSP timer sequence reconciles an idle controller from the exact `TimerQueue`
deadline, so the controller owns both the ITM program and final cancellation.
After the stackless vector/EOI entry returns with interruption
delivery disabled, normal context claims the due Timer and publishes its
Notification. The second sequence is armed while the user Wait syscall restores
the supervisor. Idle code then observes the interrupt, drains the due Timer
through the retained deferred worker, refreshes the empty queue, and lets
WaitSet processing wake and readmit the exact blocked Thread. Dispatch refuses
admission until the deadline state is coherent. This is a bounded focused
composition, not scheduler preemption or a general timer service.

Each admitted IA-64 AP independently retains a boot-lifetime deadline
controller for the focused timer-delivery request. After the persistent
scheduler proof and stable BSP acknowledgement of `0xe8` sequence one, the
`IdleServing` context uses this controller to install the AP TimerQueue's finite
deadline. Exact sequence-two processing drains one Timer-to-Notification
delivery through the deferred worker and refreshes the now-empty queue so the
controller and local timer are disarmed. The scheduler worker stays blocked
with run sequence two; the controller is not a preemption source for this
proof.

The [AMD64 runtime](Amd64BootRuntime.md) connects a separate xAPIC countdown
backend for user WaitSet resume, AP deferred delivery, and optional BSP
preemption. Its invariant-TSC/x2APIC backend remains unselected by boot.

## Verification

Hosted tests cover empty/idempotent programming, all three source orderings,
equal-source merging, earlier and later replacement, cancellation, infinite
and reached deadlines, a deadline crossed inside the HAL, baseline and
post-program clock reversal, clock exhaustion,
soft-error and hardware-failure terminal behavior for both program and cancel,
and faulted retries. Both target profiles compile and link the controller at O0
and O2 with the normal ABI, helper, and stack checks.
