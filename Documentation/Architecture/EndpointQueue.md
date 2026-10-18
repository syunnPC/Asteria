<!-- SPDX-License-Identifier: Apache-2.0 -->

# Bounded Endpoint Queue

## Scope

`Endpoint` is the first fixed-capacity typed-IPC delivery object. It owns one receiver capability space and one FIFO
with at most eight entries. Each entry retains a canonical `AIPC` frame, at most four receiver-authenticated handles,
an immutable sender resource-domain identifier, trusted invoking-Endpoint rights and sealed selector, a send class,
and a monotonically increasing endpoint-local sequence. The implementation allocates no memory and exposes no
backing-buffer address.

The Endpoint queue provides asynchronous enqueue, transactional dequeue, explicit unread-message discard, and one privately
staged send. `SynchronousReplyDispatcher` uses the send reservation to prepare a reply before consuming its one-shot
`ReplyToken`. The common syscall ingress supplies nonblocking user send/receive with bounded, fault-contained
copying. Wait-set integration, blocking IPC, cancellation deadlines, and native privilege entry remain later layers.

## Configuration and backpressure

An endpoint configuration fixes all of these dimensions before publication:

- total and emergency-reserved message counts;
- total and emergency-reserved encoded bytes;
- total and emergency-reserved capability attachments;
- normal-traffic message, byte, and attachment limits per sender.

The hard implementation ceilings are eight messages, 4,032 encoded bytes, and 32 attachments. Validation proves
that every advertised message slot can hold at least the 88-byte minimum frame, each reserved message has minimum
reserved byte capacity, and normal slots retain minimum normal byte capacity. Symmetrically, no budget exceeds what
the envelopes that could use it can carry: the total, reserved, and normal byte budgets are at most 504 bytes per
message of their class, the per-sender byte budget at most 504 bytes per per-sender message, and each attachment
budget at most four attachments per message of its class.

Normal sends are charged against total use, a total limit with the emergency reservation subtracted, and the
matching sender's current normal entries. `ReservedControl` may use the total limits and bypasses per-sender normal
quotas. It is not a user-selectable priority: the syscall or fault dispatcher must prove emergency-control authority
before using that class. A full dimension returns `WouldBlock`; the endpoint does not sleep or evict another message.

## Send transaction

One send acquires the receiver registry-domain lock and keeps it through publication:

1. for an invocation, authorize the invoking capability (see below);
2. encode and validate the caller's normalized envelope into private zeroed staging storage;
3. prove message, byte, attachment, and sender capacity without changing the FIFO;
4. translate each wire attachment into one capability-transfer request;
5. sample the shared monotonic clock once (an invocation reuses its sample), validate the message deadline and every
   source/lease/rights/type rule, and preflight receiver slots;
6. commit all capability moves/copies;
7. copy the staged frame, replace its source handle fields with destination-authenticated handles, install private
   pending-handle metadata, and advance FIFO counters and sequence.

After capability commit, publication contains no fallible operation. Message deadline and attachment deadlines use
the same clock sample. A message without attachments inspects no source space: an empty transfer is a no-op, so its
source space may lack validation keys or belong to another lock domain. Such a message samples the clock only for a
finite deadline, against the receiver space's watermark alone, and an infinite-deadline one is clock-free. A failed
send may advance the capability-space clock watermarks it sampled, but it changes no capability slot and publishes
no queue entry. Message input is never modified, and private staging storage is scrubbed after every path that
wrote it.

The message deadline is an admission deadline: a send whose deadline has passed at its linearization point fails
with `Expired`. A queued message is delivered unchanged even if its deadline passes while it waits. Receivers read
the absolute deadline from the delivered frame and decide what a late request means; the queue cannot drop it
silently, because its send may already have moved capabilities that dropping would destroy.

`PrepareSend` performs the same validation and capability transfer but leaves the completed entry at the FIFO tail
without advancing any public queue counter. At most one such reservation exists per endpoint, so ordinary sends are
rejected with `ReservationActive` until it is resolved; older queued entries remain receivable. Copies and private
destination handles are installed during preparation, but every moved source is held rather than removed: its slot,
generation, and descriptor stay in the source space, counted as active, while no handle resolves to it (every
operation, `Close` included, reports `InvalidHandle`). A matching `CommitPreparedSend` removes the held sources and
publishes the staged entry and sequence, without a fallible step. `CancelPreparedSend` closes the private destination
handles, restores every held source under its unchanged handle, and erases the entry. A move therefore removes its
source only when the message is published, and a cancelled preparation leaves both capability spaces as they were.
A held slot is never chosen as a destination, so a same-space prepared move needs a distinct free slot. The source
space must outlive the reservation.

Ordinary kernel sends record zero invoking rights and a zero sealed selector. `SendFromInvocation` instead sends for
the holder of an Endpoint capability. Inside the critical section that publishes the message, and with the one clock
sample the transaction reuses, the capability must still authorize `Invoke` on exactly this Endpoint: the caller
space must be ready and share the Endpoint's registry domain (`InvalidConfiguration` otherwise), and the handle must
be authentic, of a current revocation epoch, unexpired, of type Endpoint, and name this live Endpoint
(`InvalidArgument` for another object). Otherwise the send fails with `InvocationRejected`, whose capability field
gives the reason, before any queue or capability state is examined. A `Revoke`, `Close`, expiry, or object
destruction ordered before the publication is therefore always observed, however long ago the caller looked the
capability up. The queued message records that capability's rights and sealed selector as trusted metadata. The
user syscall adapter uses this form and always selects `Normal`; message bytes cannot forge either metadata field or
select `ReservedControl`.

Send failures follow this precedence; the first failing check is reported, and no capability slot or queue state
changes (only a sampled clock watermark may advance):

1. `InvalidConfiguration`: the Endpoint is not ready;
2. `InvalidArgument`: invalid sender or unknown send class;
3. `SequenceExhausted`: `PrepareSend` only, when reservation identifiers are exhausted;
4. `InvocationRejected`: `SendFromInvocation` only, in the order above;
5. `ReservationActive`: a prepared send is unresolved;
6. `SequenceExhausted`: the Endpoint sequence is exhausted;
7. `InvalidMessage`: the codec rejects the envelope, with its codec error;
8. `WouldBlock`: a message, byte, or attachment budget or a per-sender quota is full;
9. `CapabilityTransferFailed`, with the capability error: a source space without keys or outside the registry domain
   when attachments are present, a clock sample behind a watermark (`InvalidConfiguration`), an expired message
   deadline (`Expired`), then each attachment in order as `IpcCapabilityTransfer.md` specifies, then too few free
   receiver slots (`SpaceExhausted`).

## Receive and discard

Receive holds the same lock, validates private entry metadata, decodes into endpoint-owned staging storage, and
cross-checks every decoded receiver handle against the private pending-handle list. The staging storage is zeroed
before each decode and after every path that wrote it, and consuming or discarding an entry zeroes its frame, pending
handles, and metadata, so no endpoint storage keeps a delivered message. `PrepareReceive` copies the
normalized message and trusted metadata to caller output but retains the exact FIFO head under one generation-bound
reservation. Sends may enqueue behind that head. `CommitPreparedReceive` consumes only that reserved head;
`CancelPreparedReceive` releases it without changing the queue. The older immediate `Receive` operation is the same
prepare-and-commit transaction completed inside one call. Any empty, malformed, inconsistent, foreign, or stale
reservation leaves caller output and queue state unchanged. A successful commit transfers knowledge of the
already-installed receiver handles to the caller; it does not close them.

A prepared receive already reveals the queued receiver handles. A receiver that closes one of them and then cancels
has released that capability itself; this is not corruption. The message stays queued and is later delivered with
the closed handle, which then reports `InvalidHandle`.

`PrepareReceiveFromInvocation` is the form for the holder of an Endpoint capability. The caller space must be the
Endpoint's exact receiver space (`InvocationRejected` with `RightsDenied` otherwise, without examining that space),
and the handle must authorize `Read` on exactly this Endpoint, checked as for `SendFromInvocation` against one clock
sample of the receiver space, before the reservation. `CommitPreparedReceive` authorizes the same handle again in the
critical section that consumes the head; if the authority has ended it fails with `InvocationRejected` and leaves the
reservation and FIFO unchanged, and the caller cancels the reservation.

Receive failures follow this precedence: `InvalidConfiguration`; for the invocation form `InvocationRejected`;
`ReservationActive` while a receive is prepared; `SequenceExhausted` when `PrepareReceive` reservation identifiers are
exhausted; `WouldBlock` for an empty queue; `QueueCorrupt` for inconsistent head metadata or a frame that fails
decoding. Resolving a reservation reports `InvalidConfiguration`, then `InvalidReservation` for a default or foreign
token, `StaleReservation` for one that is not the active reservation, `QueueCorrupt` for inconsistent state, and, for
an invoked receive's commit, `InvocationRejected`.

The syscall receive path uses the invocation form. It prepares the queue head, prepares an exact fault-contained user
write, consumes the head, and performs the contractually infallible publication. Every retryable failure before
consumption, including authority that ended while the head was reserved, cancels the reservation, so neither user
memory nor FIFO state changes.

`DiscardUnread` rejects an unresolved prepared send or receive (`ReservationActive`). Otherwise it is the destruction
coordinator's explicit cleanup operation. It first resolves every unread pending handle: a handle that no longer
resolves was already closed by the receiver and is skipped, and two pending handles resolving to one slot are
`QueueCorrupt`, as is metadata whose attachment total disagrees with the queue counters. Only then does it close the
remaining receiver slots and empty the FIFO, preserving the sequence counter; every failure changes nothing. The
destructor itself never acquires a lock or waits: object destruction must quiesce endpoint users and either drain
receive or complete `DiscardUnread` before releasing storage.

## Concurrency and bounds

Send, reservation, receive, state inspection, and discard serialize on the capability registry's shared spin lock. The bootstrap
contract therefore requires callers to prevent same-CPU preemption and reentry and forbids ISR use. Normal sender
accounting scans at most eight entries. Capability placement makes one 256-slot pass and at most 1,024 same-space
source comparisons. Discard resolves at most 32 handles and makes at most 496 duplicate-index comparisons. Codec,
frame-copy, scrub, and patch loops are fixed by the 504-byte frame and four-attachment limits.

Per-function target frames remain below 512 bytes and dynamic, unbounded stack
use is rejected. Whole-path stack sizing belongs to the later scheduler and
architecture context-switch integration, where the actual kernel stack,
preemption boundary, concrete clock implementation, and IA-64 RSE requirements
are available.
