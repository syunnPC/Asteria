<!-- SPDX-License-Identifier: Apache-2.0 -->

# Synchronous Reply Dispatcher

## Scope

`SynchronousReplyDispatcher` joins one registered `ReplyToken`, one reply `Endpoint`, and one per-CPU `ReadyQueue`
from the same object-registry domain. The accepted server capability space also shares the endpoint's registry,
revocation registry, clock, and synchronization domain.
It allocates no memory and owns no second message or scheduler queue. The syscall layer is responsible for
identifying the server thread, selecting its capability space and resource domain, validating user buffers, and
excluding independent caller/server lifecycle transitions for the transaction. The server's authority to answer is
the ReplyToken capability that the Call delivered, which the dispatcher itself checks and consumes.
`SynchronousCallDispatcher` owns caller-side preparation and post-block Call
publication, then transfers the armed token exclusively to this dispatcher.
The immutable wake-queue processor is exposed so that preparation can prove it
matches the caller's captured running CPU before any mutation occurs.
`PreflightInitialize` is the side-effect-free pre-block check: it verifies that
the dispatcher is still unclaimed and that its immutable reply Endpoint and
wake-queue processor configuration are ready. It does not pin or claim a token.

## Transaction

Initialization pins the token and the exact caller and scheduling-context generations named by it, then verifies the
same blocked generation and current donation endpoint. Acceptance requires the server's ReplyToken capability: it
must authorize `Invoke` on exactly this dispatcher's token in the server space (`ReplyCapabilityRejected` with the
lookup error, or `ReplyCapabilityMismatch`), so a server that did not receive the Call, or whose authority was revoked,
cannot accept it. Acceptance then pins one exact server, validates its resource owner, and delegates the donation edge
to `ReplyToken::Accept`. A rejected Accept changes no token, donation, pin, or capability state. A successful one
consumes the capability: the one-shot reply authority becomes the accepted binding of this exact server generation,
transaction, and server space, which alone authorizes `CompleteReply` and an accepted `Cancel`, and no ReplyToken
capability remains in the server space. If the server closes the capability concurrently, the acceptance stands; the
capability is gone either way. A call cancelled before it is accepted leaves any delivered capability to the server,
where it goes stale with its token and is closed like any other handle.

A reply completes in this order:

1. require the accepted server and donated context to be stopped;
2. reserve an invisible ReadyQueue entry for the exact blocked caller;
3. privately prepare the Endpoint reply, including capability transfer;
4. consume the ReplyToken and return the scheduling donation;
5. publish the prepared Endpoint entry;
6. wake the exact `ThreadBlockToken` generation;
7. publish the reserved ReadyQueue entry;
8. release the dispatcher's token, caller, context, and server pins.

Queue capacity and endpoint backpressure occur before token consumption and roll back their reservations, so the
server may retry. Endpoint preparation holds the sources that the reply moves and removes them only when step 5
publishes the reply; a rejected token consumption cancels the preparation, which restores them under their unchanged
handles, so a move removes its source only on a successful reply. Every step after successful token consumption is a
prevalidated invariant transition; an unexpected rejection, including one at token consumption, marks the dispatcher
`Faulted` and retains the affected ownership rather than pretending the call rolled back.

Pending cancellation skips reply preparation. Accepted cancellation first returns the donation. Both reserve the
blocked entry before consuming the token, then wake and publish it. No cancellation path fabricates a message.

## Lifetime and concurrency

The dispatcher is the sole mutable access path to its token from successful initialization through terminal commit.
The caller cannot be independently woken, reblocked, terminated, unbound, destroyed, or moved to another queue; the
accepted server and donated context cannot resume while completion runs. These exclusions are established by the
trusted syscall/scheduler dispatch path. The object registry, endpoint, ready queue, accepted server capability
space, registered objects, and their storage outlive the active dispatcher.

All dispatcher operations serialize on one spin lock and then enter the existing bounded token, queue, thread,
context, endpoint, capability, and registry operations. They may spin, are not ISR-safe, allocate no memory, and
require same-CPU reentry to be excluded. A `Faulted` dispatcher and its retained reservations are a fail-stop owner;
the containing subsystem preserves them for diagnosis instead of destructing live state.

## Verification boundary

Hosted tests cover reply publication, pending and accepted cancellation, donation return, endpoint-backpressure
rollback and retry, exact block-generation wakeup, invisible queue reservation, terminal replay rejection, retained
object cleanup, reply-authority checks and consumption at acceptance, a fault at token consumption that restores the
reply's moved capability, and concurrent reply-versus-cancel exact-one completion. Debug uses ASan/UBSan, Thread
uses TSan, and IA-64 and AMD64 builds compile and link the implementation at O0 and O2 under the shared ABI and
stack checks.
