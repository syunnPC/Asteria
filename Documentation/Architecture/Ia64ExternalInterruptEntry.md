<!-- SPDX-License-Identifier: Apache-2.0 -->

# IA-64 external-interrupt entry

`AsteriaIa64ExternalInterruptEntry` is the per-processor stackless external-
interrupt front end for the BSP and each admitted AP. The runtime IVT's
architected `+0x3000` slot reaches it with one direct branch after `ar.k3`
selects that processor's context. This integration handles only local interval-
timer vector `0xe8` and architected spurious vector 15; it is not a general
interrupt dispatcher or a preemptive scheduler entry.

The retained `ar.k3` context contains a release-published pointer to one
align-16, 32-byte `Ia64ExternalInterruptEntryContext`. That context names one
aligned 64-byte reusable `Ia64LocalTimerInterruptEvidence` mailbox, the owning
processor index, and the fixed timer vector. Both objects live inside
`Ia64BspRuntimeMemory`'s locked, identity-Global Region-0 corridor for the boot
lifetime. The first publication uses a zero mailbox. Later reuse requires
`Sequence == AcknowledgedSequence`; entry overwrites the first 56-byte payload
and publishes only the next nonwrapping sequence. The publication path keeps
external interruption delivery disabled while publishing the complete context.
Entry consumes the pointer with an acquire load before dereferencing it.

Hardware selects register bank zero and clears `IFS.v` on collected delivery.
The entry therefore uses only bank-zero `r16-r31` and predicates, saves and
restores `pr`, and performs no `alloc`, `cover`, call, stack access, RSE
transition, branch-register write, floating-point access, or bank switch. It
validates the current and interrupted processor states, `ISR.ni`, aligned
`IIP`, the immutable context, and an available mailbox generation before the
destructive `cr.ivr` read.

The interrupted-IPSR check is strict for endian state, privilege, ISA,
translation, register bank, interruption state, and reserved bits. It admits
only the three legal RI values and the interrupted UP/AC/MFL/MFH user-mask
fields. Those four fields are saved interruption state rather than entry
safety preconditions: the handler records the original IPSR byte-for-byte and
lets `rfi` restore it. Under QEMU the current first/direct translated boot
path enters the poll with seeded AC and MFL (non-RI IPSR `0x0000101008026018`),
and after the system-call interruption the second/wait path enters supervisor
idle polling with AC cleared and MFL retained (`0x0000101008026010`). The VM
runner judges every recorded interrupt against this entry contract
(`Ia64ExternalInterruptInterruptedProcessorStatusValue` under
`Ia64ExternalInterruptInterruptedProcessorStatusMask`, and a legal RI), so
these path-specific observations are neither required nor part of the public
entry contract.

The IVR result has three bounded outcomes:

- Vector 15 is architecturally spurious. The entry restores predicates and
  executes `rfi` without EOI or evidence publication.
- Vector `0xe8` requires no outstanding mailbox generation, masks and reads
  back ITV as `0x100e8`, captures ITM, ITC, IIP, IPSR, the cleared `IFS.v`,
  processor index, and vector, writes EOI exactly once, serializes, and
  release-publishes the next nonwrapping `Sequence` last. It then restores
  predicates and returns with `rfi`.
- Any other claimed vector first masks the local timer, satisfies the single
  EOI and serialization obligation, and fail-stops. A contradiction found
  after the IVR read follows the same cleanup rule. A contradiction found
  before IVR can fail-stop immediately.

`AsteriaIa64PollForExternalInterrupt` is the first-generation wrapper around the
target-only PL0 leaf `AsteriaIa64PollForExternalInterruptSequence`. The reusable
leaf accepts the evidence address, one exact expected generation, and an
absolute ITC timeout. It requires delivery disabled and the preceding generation
acknowledged on entry, then enables `PSR.i` only while polling an acquire load of
`Sequence`. On completion, timeout, or an unexpected sequence, it disables
delivery, serializes, and acquire-rechecks the sequence before returning. This
closes the boundary race where delivery completes immediately before the timeout
path clears `PSR.i`. Hosted execution reports `NativeUnavailable` without
making a native-delivery claim.

Each admitted AP uses its dedicated 32-byte context, complete 64-byte mailbox,
and the same sequence-aware leaf for one bounded exact-sequence-one proof. It
validates the first 56-byte payload, cancels and masks its local timer, publishes
the timer proof, and returns with delivery disabled. That first proof remains
outside the portable run loop. Normal context subsequently enters a boot-
lifetime cooperative run-loop service with its own AP `TimerQueue`. A retained
worker blocks at run sequence one, resumes in
the same native context at sequence two for one request acquired from a reusable
polled BSP-to-AP mailbox, and blocks again while the AP remains `IdleServing`
with `PSR.ic=1` and `PSR.i=0`. After the BSP records and acknowledges the
first interrupt, a separate one-shot timer-delivery request is handled in
that idle context. The AP arms its boot-lifetime TimerQueue graph, polls exact
`0xe8` sequence two with predecessor acknowledgement one, drains exactly one
Timer-to-Notification delivery through its deferred worker, consumes the
Notification, and restores the timer, queue, controller, and local timer to
empty/disarmed. Its scheduler worker remains blocked with run sequence two and
the scheduler-service counters remain unchanged. The BSP validates the
immutable completion against the kernel's contracts, records it in the evidence
record, and acknowledges the observed sequence; the VM runner requires the
recorded pairs `1/1` and `2/2`. The service does not add timer-driven AP scheduling, a
general timer service, IPI wake, periodic policy, interrupt preemption,
migration, load balancing, shootdown, hotplug, or teardown.

The raw polling code interval is returned by a dedicated target helper rather
than by interpreting an IA-64 P64 function descriptor as a bundle address.
Successful evidence must place the interrupted bundle-aligned IIP inside that
exact half-open interval. The handler still performs no common-object work.
After the first `rfi` returns to the polling leaf with interruption delivery
disabled, normal context claims and delivers the due Timer, consumes the
Notification, refreshes the now-empty queue through the common deadline
controller, acknowledges mailbox generation one, and records the first 56-byte
payload (with its acknowledgement) and an 80-byte common-delivery record in the
kernel evidence record. During the later PL3 WaitSet suspension, idle code uses
the sequence-aware leaf for generation two. After that `rfi`, idle code validates
and records the complete 64-byte second-generation record; the deferred timer
worker then drains the Timer, WaitSet processing wakes and readmits the Thread,
and generation two is acknowledged. The initial direct
probe attempts bounded queue/controller rollback before treating a timeout as
terminal. A contradiction in the second wait path is terminal in place because
the worker is already blocked and its exact Timer/WaitSet transaction remains
owned; it does not claim to roll that transaction back. In either sequence,
masking alone cannot retract an interrupt that may already be pending or in
service.

Hosted model tests cover context, processor-state, vector, timer, evidence,
publication, and validation boundaries. IA-64 target checks inspect the real
instruction groups and reject loss of the IVR/EOI, mask/readback,
release/acquire, state-restoration, bounded-poll, and stackless-entry
properties. The VM runner reads the recorded interrupts from the evidence
record through QMP and requires both BSP architecture records of the synthetic
probe, the first record's direct Timer/Notification transitions, the second
record's exact sequence/predecessor and the resumed WaitSet response, polling
IIPs inside the relocated sequence-leaf interval, each admitted AP's
sequence-one proof and sequence-two delivery records, and final
queue/controller/ITM state. The four-vCPU default and forced-relocation cases
pass with 512 MiB; the forced-relocation case boots its own preset tree with a
different image base.
