<!-- SPDX-License-Identifier: Apache-2.0 -->

# Interrupt Object

## Scope

`Interrupt` is the architecture-independent object behind
`ObjectType::Interrupt`. It joins one platform-supplied hardware source to one
exact, registry-pinned `Notification` generation. The platform source remains
responsible for vector routing, trigger mode, controller register access, and
processor affinity. The kernel object owns source lifetime, event generations,
failure containment, and Notification publication.

Target interrupt routing and source implementations are separate from the
common state machine.

## Source ownership and lifetime

Initialization validates Notification registry provenance, pins its exact
generation, checks the supplied object address, and then calls `Claim` on the
dedicated source. The source itself rejects a second owner and begins masked.
No state is published if pinning, instance validation, or source claiming
fails. The registry, source, Notification storage, and Interrupt storage remain
alive and immovable through `Quiesce` and registry collection.

`Quiesce` accepts only a masked source with no pending event or a faulted source
that the platform has recovered sufficiently for `Release` to succeed. It
releases the source before dropping the Notification pin. An armed source must
first be masked; a pending event must be acknowledged with `KeepMasked`.

## Event transaction

```text
Masked --Arm--> Armed
Armed --HandleInterrupt--> Pending(sequence + 1) + Notification::Signal
Pending --Acknowledge(KeepMasked)--> Masked
Pending --Acknowledge(Rearm)--> Armed
Armed --Mask--> Masked
Masked --HandleInterrupt--> Masked, the edge remembered
Masked --Arm--> Armed --remembered edge--> Pending(sequence + 1) + Notification::Signal
Pending --Acknowledge(Rearm)--> Armed --remembered edge--> Pending(sequence + 1) + Notification::Signal
```

`HandleInterrupt` is the sole ISR entry. It never takes the kernel object lock.
It first masks and acknowledges the platform source, then publishes the new
nonwrapping generation, and only then signals the Notification. Duplicate
delivery while the same event is Pending is acknowledged and coalesced without
advancing the generation. A delivery while Masked is acknowledged as spurious
and signals nothing, but it is remembered, as a message source's pending bit:
the next `Arm` or `Acknowledge(Rearm)` delivers it at once as a new event
([driver isolation](IommuDomain.md)). A message-signaled
device raises one message per event and never resends one, so nothing is
dropped. A driver still reads its device's status again after it rearms,
because an edge that arrives while its event is Pending coalesces into that
event.

Normal operations run on the bound processor with local interrupts disabled.
This prevents a source from entering between a successful hardware operation
and publication of the matching common state. On AMD64, device messages
target CPU0 and every user thread runs there, so a system call's operations
(which run with IF clear) never interleave with the handler. An operation on
another processor would need compare-and-swap transitions first. `GetSnapshot` may run
concurrently; the packed state/generation word is always-lock-free and uses
release/acquire publication. Exact pointer-and-sequence tokens prevent replay,
foreign-object collision, and acknowledgement of a later event.

Nonterminal HAL `NotReady` results change no common state. Ownership protocol
failures and unknown hardware state become `Faulted`. A failed rearm retains the
pending event and its token unless the HAL reports terminal hardware failure.
Once faulted, interrupt re-entry returns without touching the unavailable
source; the platform must recover it before attempting the terminal release.

## Interrupt pool

`InterruptPool` holds the Interrupts drivers create for their functions' MSIs ([driver isolation](IommuDomain.md)). Each slot has the Interrupt, its software-masked `MessageSignaledSource` and its own Notification, both
registered. Creation binds a processor vector through the platform's `IMessageVectors` (AMD64's device vectors
0x40-0x4F, allocated in rotation, messages to the BSP's xAPIC) and has `PciFunctionControl` program the function's MSI
for its assignee. Destruction and the drain turn the messages off, then acknowledge, mask and quiesce. The vector
stays bound until the pool collects the slot, so a late message lands on a quiesced Interrupt instead of a new owner.

A platform whose drivers have no MSI (IA-64) configures the pool with `IInterruptLines`
instead: the Interrupt takes its function's INTx line (configuration byte 0x3C) in two phases. `Reserve` gives the
line's source and vector before the Interrupt is built on that source (which claims it), `Publish` binds the vector to
the Interrupt once it is, and `Release` frees both when the slot is collected; one Interrupt per line.
`PciFunctionControl` clears the function's INTx Disable for its assignee (`EnableLine`), and destruction and the drain
set it again (`DisableLine`); assignment always leaves it set. IA-64's source is an IOSAPIC redirection entry
(`Ia64IoSapicLine`: level, active low; acknowledging masks the entry and writes the vector to the IOSAPIC's EOI
register, since a level line's Remote IRR is cleared only there).

## Verification

Hosted tests cover initialization rollback, registry and storage identity,
single source ownership, pin lifetime, arm/mask transitions, duplicate IRQ
coalescing, Notification-to-WaitSet delivery, default/foreign/stale tokens,
rearm rollback, fault recovery, quiescence, sequence bounds, and concurrent
snapshot observation. Debug uses ASan/UBSan and Thread uses TSan. IA-64 and
AMD64 O0/O2 builds check the public ABI, bounded frames, and helper-free target
linkage.
