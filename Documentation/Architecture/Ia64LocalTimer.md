<!-- SPDX-License-Identifier: Apache-2.0 -->

# IA-64 Local Timer

`Ia64LocalTimer` is the processor-local implementation of `Hal::Time::ILocalTimer` for IA-64. It uses the raw
`ar.itc` counter as the monotonic tick domain, writes absolute one-shot deadlines to `cr.itm`, and selects the external
interrupt vector through `cr.itv`.

The backend does not add a software epoch above the 64-bit architectural counter. The runtime must establish
an uptime limit and stop or reboot the time domain before ITC wraps. This keeps the current processor-local primitive
stateless; it is not a claim of indefinite monotonic operation across a hardware-counter rollover.

Initialization accepts vectors 16 through 255, writes ITV in the masked state, serializes that mask, and only then
clears ITM. Programming writes ITM first,
serializes the data-register update, and only then unmasks ITV. It then samples ITC again; if the deadline crossed
during programming, it masks ITV, restores the previous ITM/ITV arm, and returns `DeadlineReached` for synchronous
service. Cancellation masks ITV and is idempotent. A deadline already at or before ITC is rejected without changing
the existing arm. Zero and the all-ones sentinel are not finite deadlines.

The object is owned by one processor. Its caller serializes initialization, programming, cancellation, and snapshot
reads with external interruption delivery disabled in `PSR.i` and no local interruption entry able to run. `PSR.ic`
may remain set during translated normal-context operations and must be set before the current external-entry delivery
proof enables `PSR.i`. The backend does not own processor-wide `cr.tpr` policy; the composing runtime must install and
read back a priority profile that admits the selected vector before enabling delivery. Register readback failure makes
the object terminal and preserves the last diagnostic arm metadata. Successful cancellation prevents a new assertion
but cannot retract an interrupt that was already latched. The BSP's one-shot proof therefore lets the external entry
mask ITV and acknowledge the claimed vector before `Ia64LocalTimer::CancelDeadline` reconciles the retained software
arm. A poll timeout is terminal after masking because it cannot prove whether delivery is still pending or in service.

`AcknowledgeExternallyMaskedDeadline` is the bridge for an
interrupt-driven continuation. After target entry has masked ITV, read that
mask back, and acknowledged the interrupt controller exactly once, the method
validates the exact software-owned deadline and clears `m_armed` and its
deadline without another register access. A non-exact, disarmed, uninitialized,
or faulted call preserves the retained software metadata. The common
`ProcessorDeadlineController` consumes the same exact receipt and captured ITC
sample; its first run-stop corridor accepts only a due, untied RunStop source.
The existing evidence/poll paths continue to use `CancelDeadline` and are not
rerouted through this acknowledgement.

The translated BSP kernel path initializes this backend masked and disarmed.
After the user mapping and syscall transport are prepared, it publishes the
retained external-entry context, arms one retained Timer in `TimerQueue`, and
asks `ProcessorRunController::RefreshTimerQueueDeadline` to program the future
ITM boundary. Delivery is enabled only inside the bounded stackless polling
leaf. The first sequence returns to normal context for direct
Timer-to-Notification delivery and is acknowledged after the queue is emptied.
The user Wait syscall later restores the supervisor, which arms the same Timer
for sequence two and blocks the worker. Idle code observes the second interrupt,
drains the Timer through `DeferredTimerWorker`, refreshes the empty queue, and
wakes/readmits the worker through WaitSet. Both records prove the fixed vector
was claimed, ITV returned to `0x100e8` in the interrupt handler, EOI was
serialized, and execution resumed inside the polling sequence. That masked
state is transient: after the second drain, readmission programs the active
scheduling-context run stop at `ITM=0xfffffffffffffffe` and unmasks
`ITV=0xe8` for the final stopped PL3 loop.

Each admitted AP independently owns one `Ia64LocalTimer` for a narrower proof.
Before pivot, the BSP runtime-memory owner publishes that AP's dedicated
Region-0 external-interrupt context and zero mailbox. After the translated
entry, the AP initializes vector `0xe8` masked,
installs and reads back `cr.tpr=0xd0`, arms one deadline, and native-polls exact
mailbox sequence one with `PSR.ic=1` and `PSR.i` enabled only inside the polling
leaf. It validates the acquired evidence, cancels and masks the timer,
then clears `PSR.i` before borrowing the admitted root/controller for its
boot-lifetime cooperative scheduler service. A retained worker blocks after run
sequence one, and the AP enters `IdleServing`, polling a distinct reusable
acquire/release BSP-to-AP service mailbox with `PSR.ic=1` and `PSR.i=0`. One
external request is acquire-observed by the owning AP, which uses the retained
worker's exact block token to wake and re-enqueue it, resumes the same native
context at sequence two, and lets it block again before the AP returns to
`IdleServing`. The BSP validates the persistent scheduler-service proof and
the processor-local timer transport against the kernel's contracts, records
them in the evidence record, and acknowledges the observed timer mailbox
sequence; the VM runner requires the recorded pair `1/1`.

The BSP then publishes a separate one-shot AP timer-delivery request. The AP
handles it directly in `IdleServing`, not through its scheduler worker or
interrupt preemption. Its boot-lifetime `Timer`, `Notification`, `TimerQueue`,
`ProcessorDeadlineController`, and `DeferredTimerWorker` arm a finite deadline
and poll exact `0xe8` sequence two with `PSR.i=0` outside the polling leaf. The
AP validates predecessor acknowledgement one, drains exactly one Timer-to-
Notification delivery, claims and commits the Notification, and restores the
Timer, queue, controller, and `Ia64LocalTimer` to empty/disarmed. The scheduler
worker remains blocked with run sequence two and the scheduler-service counters
remain unchanged. The AP publishes immutable completion; the BSP validates it
against the kernel's contracts, records it, and acknowledges the observed
sequence; the VM runner requires the recorded pair `2/2`.

The richer two-sequence BSP TimerQueue-to-Notification/WaitSet proof remains an
independent slot-zero path and is not evidence for the focused AP delivery.
The AP service is cooperatively polled rather than timer- or IPI-driven; it
provides no timer-driven scheduling, periodic policy, general timer service,
general interrupt routing, interrupt preemption, migration, load balancing,
hotplug, or teardown.

The common external-stop transaction is therefore an independently tested
prerequisite, not evidence that this AP runtime preempts a worker. Native
frame capture, RSE/NaT preservation, scheduler-stack transfer, and `rfi`
restore are still required before the production vector may select it.
