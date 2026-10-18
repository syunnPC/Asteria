<!-- SPDX-License-Identifier: Apache-2.0 -->

# Processor address-space controller

## Scope

`ProcessorAddressSpaceController` is the architecture-neutral root-selection
boundary for one CPU. It composes an exact idle `AddressSpace`, the currently
active `AddressSpace`, and each root's native page-table backend. It allocates
no memory, performs a constant number of bounded calls, and is suitable for
the processor run loop's 512-byte per-function stack discipline.

The controller does not choose a runnable Thread or derive an AddressSpace
from untrusted input. Its caller supplies the destination associated
with the already-admitted `ProtectionDomain`. `Initialize(CpuId, idle)` first
activates the dedicated idle root. `SwitchTo(AddressSpace&)` and
`SwitchToIdle()` then serialize later selections on that owning CPU.
`GetSnapshot()` exposes the exact idle, active, and conservatively retained
previous storage pointers together with their registry generations. A terminal
snapshot retains each unforgeable source or destination transition receipt
still known to authorize the current reservation for fail-stop teardown.
Before initialization the idle fields
are null and invalid, allowing a run loop constructed earlier to validate
first use without relying on global construction order. The boot-integrated IA-64 BSP
runtime-memory owner uses this same `Initialize` boundary to activate its
one registered common bootstrap/idle `AddressSpace`. It does not call
`SwitchTo`, enter the processor run loop, or establish a runnable root.

## Native activation contract

`IAddressSpacePageTable::Activate(owner, CpuId)` operates on the exact claimed
owner. `None` means the root switch and all target-required local translation
invalidation completed. `NotReady`, `InvalidOwner`,
`UnsupportedConfiguration`, and `RejectedBeforeCommit` all guarantee that the
previous root remains current. `CommittedStateUnknown` means the destination
may already be current and requires conservative fail-stop handling. An
unknown enum value is treated as the same terminal protocol violation.

The owning `AddressSpace` holds its lock while calling the backend, publishing
the CPU in its resident mask only after success. Its transition reservation
remains held independently of that lock and state. On an unknown commit it
enters `Faulted`, publishes the CPU conservatively, and retains the
reservation. The lock order is therefore:

1. destination `AddressSpace` lock;
2. that destination's private page-table backend synchronization.

The backend is callback-free and does not acquire another AddressSpace lock.
The controller itself has no lock and requires local interrupts disabled plus
same-CPU re-entry exclusion.

## Switch transaction

For distinct source and destination objects, `SwitchTo` performs these bounded
steps:

1. reserve the source for this CPU, then validate its cached identity and
   residency while mapping is excluded;
2. reserve the destination through a separate AddressSpace lock acquisition;
3. complete `destination.ActivateReservedProcessor(destinationReceipt)`;
4. publish and validate the destination's exact identity, residency, and
   reservation sequence;
5. call `source.DetachReservedProcessor(sourceReceipt)`;
6. release the source reservation and then the destination reservation.

No step holds two AddressSpace locks simultaneously. Reservation acquisition
never waits for another reservation. If destination acquisition loses to a
mapping, activation, or opposing CPU transition, `SwitchTo` releases the
source and returns `TransitionRejected` with
`AdmissionRollbackSafe == true`. A mapping checks the reservation under the
same root lock before retaining resources or calling its backend and returns
the retryable `ProcessorTransitionPending` error. Therefore the mapping and
the transition have one atomic winner.

After both reservations are acquired, the source pointer and identity remain
in the controller's `Previous` fields while the saved source receipt remains
authoritative. On the committed path this includes source detach and lasts
until the following release attempt; a pre-commit rejection needs no detach.
Either `None` or `InvalidProcessorTransitionReceipt` from that attempt means
the saved receipt is no longer current, so the controller invalidates it and
clears `Previous` together. A commit-unknown, destination-invariant failure,
or detach rejection occurs before release and therefore retains `Previous`
and the authoritative source receipt for self-contained fail-stop teardown.

Between steps 3 and 5 both resident masks contain the CPU. Mappings remain
excluded from both roots throughout that interval, so no shootdown can wait on
the locally IRQ-disabled switching CPU. Selecting the exact active object
acquires and validates one source reservation, releases it, and returns without
calling the backend or changing either mask.

## Failure and scheduler admission

`ProcessorAddressSpaceControllerFailure::AdmissionRollbackSafe` is the only
permission for the run loop to undo a provisional scheduler admission as if
no architecture state changed.

| Outcome | Active binding | Residency | Rollback safe |
| --- | --- | --- | --- |
| Controller not initialized (`NotReady`) | None | Unchanged | Yes |
| Source mapping/transition already pending | Source | Unchanged | Yes |
| Destination reservation rejects | Source | Source only; source reservation released | Yes |
| Destination rejects before commit | Source | Source only | Yes |
| Destination commit is unknown | Destination conservatively; exact source retained as Previous | Source and destination retained | No |
| Destination success violates its published invariant | Destination; exact source retained as Previous | Conservative | No |
| Source detach rejects after destination success | Destination; exact source retained as Previous | Conservative | No |
| Cached source invariant is already broken | Untrusted | Conservative | No |

`SwitchTo` and `SwitchToIdle()` report these outcomes identically. Every
non-rollback-safe outcome moves the controller to `Faulted`, whose later
refusals are not rollback-safe either. Every saved receipt still known to
authorize a current reservation remains in the controller snapshot. A
commit-unknown, post-commit destination invariant failure, or defensive detach
failure therefore retains both receipts. Cleanup attempts every remaining root
even if an earlier release reports a lifecycle error, invalidates each
attempted receipt immediately, and reports the first error. `Previous` remains
through a committed source detach only while the source receipt is
authoritative and is cleared together with that receipt. Later switches return
`Faulted` without touching native state, and each root with a retained current
receipt continues to exclude mapping. After a commit-unknown or post-commit
bookkeeping failure, returning the admitted Thread to Ready could duplicate
execution or run it under the wrong root.

## Lifetime and teardown

The idle root and every root stored as active or previous outlive the controller. Their
registry-backed `AddressSpaceId` generations must remain unchanged. A
successful switch detaches the prior root before returning, so only the active
root retains this CPU. A faulted controller may leave both roots resident;
platform fail-stop teardown must stop execution, establish the real native
root state, use the retained receipts to detach every conservative residency
bit and release each reservation, and only then quiesce and release the
AddressSpaces. Bare detach cannot bypass a reservation, even with the matching
CPU identifier. The IA-64 BSP integration retains its owner, registry,
preparation, registered `AddressSpace`, backend, and controller for the boot
lifetime and provides no teardown path.

## Verification

Hosted tests cover exact idle/same-root/moving-root residency, mapping exclusion,
transition contention, receipt validation, rollback before commitment, and
conservative retention after unknown commitment. IA-64 BSP owner tests check
first-root publication and both precommit rejection and fail-stop retention.
