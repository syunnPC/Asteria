<!-- SPDX-License-Identifier: Apache-2.0 -->

# USB mass storage: BOT and transparent SCSI

The `Bot`, `Scsi`, and `MassStorage` components implement the Bulk-Only
Transport (BOT) and transparent SCSI subsystem without
dynamic allocation, system calls, USB device addressing, or dependencies on
`Input::UsbDevice` or controller internals. Invocations must be serialized, and
instance objects must be kept in component or static storage.
The supported profile is LUN 0, BOT, SCSI direct-access disks and 512-byte
blocks. UAS, additional LUNs and READ/WRITE(16) are unsupported.

## Pipe seam and ownership

`IMassStoragePipe::Submit(PipeRequest)` copies metadata and up to 64 inline
bytes. A request names an opaque logical pipe and monotonic completion token.
Bulk IN/OUT uses either inline bytes or `FrameBuffer { FrameHandle { Selector,
Authenticator }, Offset, Length }`; each transfer is at most 65536 bytes.
`BotBinding` supplies distinct control, bulk IN and bulk OUT pipe IDs and an
interface number, selected by 08/06/50 interface binding.
The inline setup packet addresses the interface, never a USB device address.

The adapter validates handle rights and full Frame bounds, maps into its fixed
pipe window at issue, and unmaps/proves hardware quiet before its one completion.
Statuses are `Ok/Short/Stall/Error/Cancelled`, with actual length; inline IN
bytes are borrowed only during `Complete`. No callbacks or synchronous reentry.
False Submit retains nothing; three consecutive refusals exhaust that stage.
`ClearHalt` completes after both USB endpoint halt and host toggle/dequeue are
reset. `Abort(pipe, originalToken)` yields exactly the original completion,
including a racing successful transfer, only after quiet. A refused Abort
retains ownership. Tokens/tags never wrap; exhausted owners refuse new work.

`Step` submits at most one request; `Complete` advances one state and submits
nothing; `TakeResult` consumes exactly one retained result before a new command.
Stale/duplicate completions are rejected without changing ownership.
The facade's `Step(pipe, monotonicMilliseconds)` has a configurable 1..60000ms
operation deadline (5000ms by default), measured from its first Step. Deadline
Abort retries are capped at three refusals; exhaustion faults the owner with
its borrow retained. The component must then quiesce/revoke the adapter and
release its original completion. Accepted Abort, class-control and recovery
requests require finite adapter bounds. No deadline kills an entire controller
merely because a bulk endpoint NAKs. Without Step/clock progress or the adapter's
completion/quiescence contract, elapsed-time completion cannot be guaranteed.

## Transport and SCSI states

BOT: CBW OUT (31 bytes), optional data IN/OUT, CSW IN (13 bytes). The codecs
encode little-endian fields explicitly, pad the CDB, enforce LUN zero and
validate signature, tag, length, residue and status. Phase-error residue is
undefined and ignored. Relevant bytes are requested minus residue and may not
exceed bytes actually transferred; padding beyond relevant data is ignored.
A command-failed CSW is returned for REQUEST SENSE, without resetting BOT.

Data stall clears that data pipe and proceeds to CSW. A CSW stall clears IN
and retries CSW once. A second CSW stall, invalid/mismatched CSW, impossible
residue/actual combination, phase error, CBW failure or transfer error runs
Bulk-Only Reset, ClearHalt(IN), ClearHalt(OUT), in that order. Any failed
or cancelled recovery faults the owner, without restarting a cancelled reset. Recovery returns the original failed/cancelled
command, never replays a potentially applied write, and flags lost state.
GET_MAX_LUN is an interface control request; stall means maximum LUN zero;
values 0..15 are accepted but commands always address LUN zero.

Describe: GET_MAX_LUN → INQUIRY(36) → TEST UNIT READY → READ CAPACITY(10)
→ READ CAPACITY(16) only on 0xFFFFFFFF saturation → MODE SENSE(6), falling
back to (10) on ILLEGAL REQUEST. REQUEST SENSE follows command failure.
TUR has four total attempts shared across becoming-ready 02/04/01 and UNIT
ATTENTION 06/28 or 06/29; becoming-ready waits at least 100ms between attempts.
Fixed and descriptor sense formats are length checked; deferred sense fails
conservatively. No-medium TUR publishes a removable description with no medium,
512-byte blocks and count zero. Other block sizes are Unsupported.

Read/Write uses one READ/WRITE(10) command, bounded by the caller's allowed
bytes, the Frame slice, 128 blocks, capacity and the 32-bit CDB LBA range.
Larger ranges are refused before issue; the caller splits them explicitly.
Flush uses SYNCHRONIZE CACHE(10); only ILLEGAL REQUEST is accepted as success.
Write-protect is checked before issuing a write and decoded from MODE SENSE.
The result includes a small typed status, zero blocks on failure and the current
medium generation. UNIT ATTENTION medium-change/reset and BOT recovery bump
that generation and invalidate the cached description; a new Describe is
required before further I/O. Generation exhaustion faults rather than wraps.

## Atomicity and integration limits

Begin refusals and parser failures change no caller output. Describe assembles
a private candidate and publishes it only on success; a failed probe preserves
the prior snapshot unless sense/recovery invalidates the medium. Every accepted
operation has one final result and reports all requested blocks or zero.

BOT/SCSI offers no transactional rollback of media writes. A failed/cancelled
write may already have modified media, and DMA for a failed read may already
have modified the supplied Frame. **To provide unchanged-on-failure reads to a
block client, the component must supply a private staging Frame and copy into
the client Frame only after Ok.** This library has no authority to map/copy
Frames. Reporting zero blocks does not undo media writes.

Component integration must bind the mass-storage bulk pair, adapt this seam to per-pipe queues,
supply confined Frame windows/inline DMA scratch, complete ClearHalt/Abort with
quiet proofs, schedule clocks/deadlines and results fairly, handle detach and
retain pins through Device Manager quiescence. It must separate controller
instance generations from these medium generations and provide staging for
client reads. See [USB host group](../../../Documentation/Architecture/UsbHostDriver.md)
for component integration and its current limits.

## Tests and references

`FakeBot.hpp`, `BotTests`, `ScsiTests` and `MassStorageTests` model wire stages,
media and Frame storage. Seeded operations compare status, generation, cached
state, contents and ownership; `ASTERIA_TEST_SEED` replays failures. Cases cover
transfer bounds, readiness/protection, residue, recovery, cancellation and
failure atomicity. Fuzz harnesses cover CSW, sense and SCSI data parsers.
See [USB verification](../../../Tests/User/Usb/Readme.md) for controller coverage.

Protocol reference: [USB-IF BOT 1.0](https://www.usb.org/sites/default/files/usbmassbulk_10.pdf),
sections 3–6. Wire fixtures are project-authored. AMD64/IA-64 builds include
O0/O2, standalone-header and stack-budget checks.
