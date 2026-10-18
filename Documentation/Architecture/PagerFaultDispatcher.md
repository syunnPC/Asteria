<!-- SPDX-License-Identifier: Apache-2.0 -->

# Pager Fault Dispatcher

`PagerFaultDispatcher` is the architecture-neutral transaction owner between a
blocked faulting Thread and its external pager. It composes existing objects; it
does not allocate token storage, decode an ISA exception, edit page tables, or
switch processor contexts.

## Delivery boundary

The exception path first finishes the exact Thread as `Blocked`. A kernel token
factory separately initializes and registers one `FaultToken`, then mints a
transferable `Invoke` capability into the trusted source capability space.
`Deliver` pins that token, its retained Thread, and the supplied
`SchedulingContext`. It requires the exact current block generation and a
non-running context whose current Thread matches the token.

Before publication, the dispatcher proves that the source handle names the same
registered token. It builds the canonical `PagerProtocol` envelope and sends it
through the Endpoint's `ReservedControl` class. Endpoint transfer attenuates the
receiver handle to `Invoke`, moves it, and publishes the message atomically. An
ordinary rejection leaves the dispatcher idle, the token pending, the Thread
blocked, the source handle present, and the Endpoint unchanged, so the same
attempt can retry.

## Resolution boundary

The pager claims the token and performs the authorized mapping or redirect
transaction outside this class. `Complete` accepts only that exact active claim.
For `Map` and `Redirect`, it reserves a ReadyQueue slot before consuming the
token, wakes the exact block generation, and then exposes the ready entry. For
`Terminate`, it consumes the token and exits the blocked Thread without a queue
entry.

Failures before token commitment remain retryable. After token commitment,
Wake or queue-publication failure is terminal and the dispatcher retains its
pins in `Faulted` for explicit recovery. Direct token cancellation/commit,
Thread lifecycle changes, context changes, and queue placement are excluded for
the active dispatcher lifetime.

`Reset` returns a Completed dispatcher to Idle for the next Deliver; completion
already released every pin.

`PageFaultIngress` supplies the fixed-capacity token factory and calls
`Deliver` between scheduler completion and the native switch away from the
faulting context. The higher pager owner owns eventual token destruction after
the pager closes its terminal capability.
