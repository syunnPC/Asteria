<!-- SPDX-License-Identifier: Apache-2.0 -->

# Pager Channel

`PagerChannel` joins one external pager to the user faults of the address
spaces it serves, one fault at a time from capture to the faulting thread's
wake. It composes `PageFaultIngress`, `FaultTokenFactory`,
`PagerFaultDispatcher` and `FaultResolutionDispatcher`, and is the capability
layer those components leave to their caller.

## Fault side

A trap path that decoded a user fault of the running thread calls
`PrepareFault` with the thread, its AddressSpace and SchedulingContext, and the
normalized `FaultRecord`. The channel correlates them through its ingress and
becomes `FaultPending`; a second fault while one is pending is `Busy` and
changes nothing. The trap path then completes the run as Blocked through
`ProcessorKernelRunLoop::FinishFaultAndDispatch` with the channel as its
ingress. `CommitBlocked` creates the FaultToken in the configured kernel token
space, delivers the [pager message](PagerProtocol.md) through the pager
Endpoint's reserved control class, which moves an Invoke-only token handle into
the pager's space, and signals the configured readiness Notification once. The
pager waits on that Notification in a WaitSet. Token-pool or Endpoint pressure
leaves the delivery retryable through `Retry`.

## Resolution side

`Resolve` serves the pager's [FaultResolve](SystemCallAbi.md#faultresolve-semantics)
call. It trusts nothing in the request: it looks up the token handle in the
caller's own capability space with `Invoke` and requires it to name the
delivered token, looks up the AddressSpace handle with `Map` and requires it to
name the space recorded in the token, and looks up the MemoryObject and Frame
handles with `Map`. It then claims the token and lets `FaultResolutionDispatcher`
validate the mapping, install it, invalidate translations and complete the
pager transaction, which wakes the thread. A rejected mapping cancels the claim
before any native change, so the fault stays pending for a corrected request.
After success the channel retires the token, resets its ingress and dispatcher,
and returns to `Idle`; the pager's handle to the token no longer resolves and
remains closeable. Retirement withdraws authority immediately. Physical
collection may wait for another processor's existing lease or pin:
`NoDestructionReady` is collection pressure, not a channel failure. The channel
retains at most `FaultTokenFactory::MaximumTokens` retired identities and
collects them on `Retry` (also while Idle), before preparing another fault, and
when its owner checks `IsReclaimable`. New faults may use remaining factory
slots while collection waits. A `FaultChannel` cannot destroy its embedded
storage until its channel is Idle and every retired identity has been reclaimed
([processor scheduling](ProcessorScheduler.md)).

A Terminate request ([supervised fault termination](ThreadLifecycle.md))
names only the token. The channel looks it up with `Invoke`, requires it to
name the delivered fault, claims it and completes the `PagerFaultDispatcher`
transaction with Terminate, which exits the Blocked thread with
`Abi::FaultTerminatedExitCode` instead of waking it. It then retires the token
and returns to `Idle` like a Map, counting the resolution in `Terminations`
too. A Terminate that carries a mapping is `InvalidRequest`.

When a resolution leaves remote translation invalidation pending, the channel
waits through the configured `IPagerShootdownProgress` for a bounded number of
rounds; without one, or after the bound, it faults. A failure after a possible
native change also faults the channel. The resolver's failure behind the last
rejected or failed resolution stays readable through `GetLastResolutionFailure`
for diagnostics.

## Serialization

The channel owns no lock. `FaultChannelTransition` provides acquire/release CAS
ownership. Prepare contenders return Busy; Resolve owns
Resolving through its shootdown without holding a channel or pool lock.
CommitBlocked, Retry, signal and snapshot operations also claim transitions.
The composed objects lock themselves in the [lock order](LockOrder.md):
`PageFaultIngress` is the outermost, and every lease taken by `Resolve` is
released before the token is retired; other processors' leases may delay
collection. The channel collects each exact token (`CollectDestroyedObject(token)`), so another quiescing
object in the registry never fails a retirement.

ObjectType 17, `FaultChannel`, embeds this implementation. The dispatcher obtains the issuer from the Invoke-authorized
token and pins the channel across Resolve. A ProtectionDomain pins its channel,
and the channel pins its Endpoint's receiver CapabilitySpace. These pins make
the collection order ProtectionDomains, FaultChannels, CapabilitySpaces.
