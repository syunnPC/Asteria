<!-- SPDX-License-Identifier: Apache-2.0 -->

# Page-fault ingress

`PageFaultIngress` is the common boundary between a synchronous target fault
and the external pager. It is prepared while the exact `Thread` and
`SchedulingContext` are still Running. Preparation pins the Thread,
`AddressSpace`, and context, validates that the normalized record is a user
fault, and does not change any lifecycle state.

`ProcessorKernelRunLoop::FinishFaultAndDispatch` then completes the Thread as
Blocked and passes the scheduler-produced block generation to the ingress
before selecting or switching to another context. The ingress revalidates that
generation, creates a `FaultToken`, and publishes the request through
`PagerFaultDispatcher`:

    decode user fault
        -> prepare and pin
        -> finish Thread as Blocked
        -> bind exact block token
        -> create FaultToken
        -> publish pager request
        -> dispatch another context

Fixed token-pool pressure leaves the transaction in `BlockedPendingToken`.
Retryable Endpoint pressure leaves it in `TokenReady`. `Retry` resumes either
phase without waking the faulting Thread. A changed Thread/context correlation
is terminal and supervisor faults never enter this path.

A permanent failure before Endpoint publication automatically closes any
source capability created by the transaction and retires its token. The higher
owner collects that quiescing token and chooses whether to terminate or
otherwise recover the still-blocked Thread; no unpublished capability handle is
lost inside the ingress.

If the source-space close or factory retirement itself violates its ownership
contract, the snapshot reports `RollbackRejected` and the ingress remains a
fail-stop retained owner; it must not be destroyed as though rollback had
completed.

`Reset` ends a Delivered transaction so the next fault can `Prepare`; delivery
already released every ingress pin, and the token belongs to the pager
transaction whose owner retires it ([pager channel](PagerChannel.md)). Any
other state is left unchanged.

This object runs in the serialized, synchronous post-capture kernel-fault path.
It may take kernel locks and must not run directly in a hard interrupt handler.
Target integration supplies the native exception frame, invokes the decoder
and ingress, and performs the architecture return.
