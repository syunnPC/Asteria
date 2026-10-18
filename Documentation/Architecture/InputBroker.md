<!-- SPDX-License-Identifier: Apache-2.0 -->

# Input Broker v3

`User/Component/InputFrame.hpp` defines a canonical AIPC asynchronous frame:
interface `{0x415354494E505554, 1}`, four little-endian u64 words followed by
8 explicitly encoded 24-byte InputEvents. Payload is always 224 bytes; unused
slots are zero. Frames are 312 bytes without an attachment, 344 with one, 376 with two.
Requests carry one Invoke-copy reply Endpoint; the recipient closes it after
answering, or retains it as a single outstanding credit. Successful registration
replies carry an Invoke|Duplicate-copy broker-created Endpoint. No native
structure is copied onto the wire. Header flags, sizes, reserved bytes,
attachment type/rights/mode/deadline, method words and every event field are
validated; undecodable requests are dropped and all attachments closed.
Delivery metadata (rights, sender domain, sealed selector) comes from the kernel.

| Method | Number | Words | Attachment and authority |
| --- | --- | --- | --- |
| RegisterSource | 1 | epoch, 0 or source 1..6, 0, 0 | Reply; main Endpoint, Manage |
| RegisterClient | 2 | epoch, flags bit 0, 0, 0 | Reply; main Endpoint, Invoke |
| Publish | 3 | epoch, count 1..8, 0, 0 | Reply; minted source Endpoint |
| Credit | 4 | epoch, maximum 1..8, 0, 0 | Reply retained; minted client Endpoint |
| RequestFocus | 5 | epoch, 0, 0, 0 | Reply; minted client Endpoint |
| SetFocus | 6 | epoch, client 0..2, lock bit 0, 0 | Reply; main Endpoint, Manage |
| AttentionCredit | 7 | epoch, 0, 0, 0 | Reply retained; main Endpoint, Manage |
| Query | 8 | epoch, 0, 0, 0 | Reply; main/source/client Endpoint |
| Reply | 9 | current epoch, answer, id, value | Registration alone returns an Endpoint |
| Events | 10 | epoch, client, count 1..8, session pointer horizon (console 0) | None; answers a client credit |
| Attention | 11 | epoch, source, usage (0 or 66..68), 0 | None; answers a Manage-held credit |
| FocusRequest | 12 | epoch, client, 0, 0 | None; answers a Manage-held credit |
| SetPointerRoute | 13 | input epoch, display epoch, last display sequence, 0 | Reply then Invoke-only display sink; main Endpoint, Manage |
| RebindClient | 14 | epoch, session client 1..2, retire bit 0, 0 | Reply; main Endpoint, Manage, after sole consumer stop and client Query fence |
| Resume | 15 | epoch, 0, 0, 0 | Reply; session client Endpoint, once per handoff |
| Baseline | 16 | epoch, delivered keys, accepted pointer sequence, held buttons | None; answers Resume before either stream's credit |

Reply answers are Done=1, Refused=2, Exhausted=3, Busy=4, Restarted=5.
Registration returns broker id 1..6 (a source) or 1..2 (a client) and protocol
version 3. Query returns focus
id (0 means no focus) and version 3; other acknowledgments have id/value zero.
Driver Query uses the same Reply method with a different schema, selected by
the caller's outstanding request (`InputReplySchema::DriverQuery`). Done carries
an unsigned 32-bit polling-interface count in word 2's low half, the unsigned
32-bit root-port powered-off bitmap in its high half (xHCI; zero for USB host
and PS/2), and a full unsigned 64-bit decoded-event count in word 3. It carries
no attachment; failure answers have both fields zero. The default Broker schema
retains the registration id, version, attachment and acknowledgment checks.
PS/2 uses zero interfaces/power bits and the same full event counter. Producers
and consumers retain the existing four-word frame and method numbers.

`Input::KeyboardGeometry` describes PS/2 physical Ansi/Iso/Jis scan decoding;
`Input::KeyboardLayout` describes Us/Jis character legends. These are separate
types, with no implicit conversion or mutable global selection.

A request with an old epoch returns Restarted with the current epoch, without
changing state. Dead-instance Endpoints fail through the kernel's stale-handle
errors; callers register again with the new instance. There is no replay of
input across a restart. Each registration consumes a fresh slot. A minted
Endpoint's sealed selector identifies a source/client; sender-domain charge
metadata is not its identity. The root enrolls a driver source and hands that
driver its minted Invoke|Duplicate source Endpoint; an Invoke-only client cannot
enroll a source. Possession of a minted Endpoint is delegable authority; a
numeric id alone grants nothing. A lost registration answer requires broker restart to
recover the consumed slot; registration itself must not be blindly retried.

RegisterSource naming a registered source re-enrolls a restarted driver without
consuming a slot: the
keys the source still holds are released as its device would have released them (a client that saw the make gets
a zero-modifier break), its deferred modifier makes are dropped, and its replay fence is cleared, so the new
instance's transactions, which start again at 1, are not refused as replays. Source reset also releases
that source's pointer buttons, preserving other sources' buttons, after its key releases and with their
cumulative key stamp. Both key and route capacity (including sequence exhaustion) are reserved first;
Exhausted preserves physical state and replay fences for retry, even when the route is disabled. The answer carries the source's same
id and Endpoint; Exhausted (changing nothing) when a client's queue cannot take the releases, Refused for an
unregistered source. Whatever the dead instance left queued on the source Endpoint must be processed first: the
root fences the source with a Query on it, which the broker answers only after everything queued before it, and
only then registers it again.

The broker stamps every published Source with its minted source id. Source
fields supplied by drivers are ignored, including a forged nonzero field.
Keyboard usages are page 0x07, 4..0xE7; modifiers are an eight-bit HID snapshot.
All irrelevant event fields and three reserved bytes must be zero. Pointer
buttons are restricted to bits 0..2. Full signed 32-bit motion/wheel values are
preserved without arithmetic. Invalid batches fail atomically.

## Focus, attention and bounded storage

The pure `InputBrokerLogic` supports six sources (PS/2, four catalog xHCI functions and the USB host group) and two clients. Each client has
64 queued events and at most 32 held `(source, usage)` keys. Publish commits a
whole batch or returns Exhausted without changes. Only the focus holder queues
new events. Accepted queued key downs count as seen: their eventual delivery
precedes their releases. SetFocus appends a zero-modifier key-up for every key
seen by the old holder, retaining its original source; if its queue cannot hold
all releases, focus remains unchanged and the manager retries after credits
have drained it. Existing queued events remain with their original recipient.
Physical releases after focus loss do not reach a new holder that saw no make.
Repeated makes and unmatched breaks are ignored; clients synthesize repeat.
Product init and the test pager's real-device terminal use `User/Input/KeyRepeat`, defaulting to 500 ms and 30 repeats per second. The
client combines current per-source modifiers and excludes modifiers and all
lock keys. Focus changes already enqueue the KeyUp that cancels its repeat;
xHCI publishes decoder releases before disconnect, including missing physical
breaks. Driver exit and source re-registration also cancel terminal repeat.
The [input notes](../../Source/User/Input/Readme.md) describe elapsed clock
scaling, bounded catch-up and fault-serving timed waits.

LCtrl/RCtrl (E0/E4), LAlt/RAlt (E2/E6) and Delete (4C) form secure attention.
Ctrl/Alt makes are deferred until an ordinary key or modifier release makes them
part of an ordinary shortcut. Delete while either Ctrl and either Alt are held
consumes its make, repeats and break, discards deferred modifier makes across
sources, and suppresses their breaks and modifier snapshot bits. Modifier
state aggregates the sources and tracked physical modifier edges; a forged
zero modifier snapshot cannot bypass a chord whose makes were received. The
four modifier combinations and split batches are supported. A modifier already delivered as part of an earlier ordinary
shortcut cannot be withdrawn: its balancing release remains deliverable, but
the secure Delete never is. Pointer events do not flush deferred modifiers.
Changing focus discards deferred modifiers and suppresses their later breaks.
A lone Ctrl/Alt make may therefore remain deferred until release or another key;
this intentional latency prevents a contiguous attention chord leaking its
modifier edges.

Attention queues eight reports; overflow refuses the source batch atomically.
AttentionCredit is the root/session manager's Manage-only notification channel,
shared with focus requests. Attention takes priority. One pending focus request
is retained, another client's request gets Busy. A request never changes focus:
only the root's SetFocus does. Each client and the manager may hold one credit;
a second gets Busy. A credit answers once, with up to its requested count,
including queued focus-loss releases even after the recipient loses focus.
A full reply Endpoint retains the credit and queued events; a 10 ms backstop
retries only while a notification is waiting to be sent. Idle waits are
indefinite. A failed send otherwise closes the credit and retains the queue for
a fresh credit. Wait/Receive/non-stale Close failures end the broker;
a timed backstop expiry is an ordinary retry wakeup. No hardware is touched.

## Native component and scripted scenario

Behaviour 16 is the broker; 17 is its small event-consuming probe client. Broker
quota: nine Endpoints (main, six sources, two clients), one Thread, no pages
beyond image/stack. Its start block uses epoch/incarnation word 1, WaitSet words
6..7 (Wait|Manage), receive Endpoint 8..9, ResourceDomain 24..25 and AddressSpace
26..27 as the display model does. Only its domain is used; nothing is mapped.
The probe has one Endpoint and Thread; its broker Invoke capability is at words
34..35 and broker epoch at 36, with its own reply capability at 12..13. These
are role-specific aliases; the component start block is 57 words. The
separate pager bootstrap is 85 words, including i8042 authorities at 38..45;
those words do not overlap this component protocol. The supervisor
kills and drains both services, collecting minted Endpoints with the broker's
domain. No graceful deregistration exists; reclaiming a client/source
slot requires a broker restart. On IA-64, whose component text fills its 64-page segment limit,
`InputBroker.cpp` and `InputBrokerLogic.cpp` are compiled for size (`-Os`, the platform's
`COMPONENT_SIZE_SOURCES`) and linked into the same component image; it serves a few events per
keystroke and is on no hot path.

The scripted root-service scenario verifies source stamping, key and pointer
delivery, secure attention, focus transfer and balancing releases. It checks
that all component domains, Endpoints, mappings and staging allocations are
reclaimed; linked-image budget gates account for retained shared images.

The [PS/2 component](Ps2Driver.md) is behaviour 18 and runs after this scripted
scenario tears down. PS/2 and xHCI components need a registered source Endpoint and current epoch
from the root, and their own reply Endpoint with Invoke|Duplicate for Publish
acknowledgments. The root enrolls each source through RegisterSource and grants
its driver the returned Invoke|Duplicate capability; Manage stays with the
root/session manager. Client components receive Invoke on the main Endpoint
for RegisterClient.
Split library EventBatch (up to 20) into batches of at most eight; retain each
batch until acknowledged, retry Exhausted after a bounded pause, and re-register
on Restarted/stale handles. Drivers retain physical key state and suppress
hardware repeats; they never choose focus, reach client Endpoints or stamp
trusted source ids. A controller can receive either of the two bounded source
ids; separate per-device tags, hot-unplug release/reset and deregistration
require an extension before a multi-device session manager needs them. Publish
transactions must be nonzero and monotonically increasing per source.
The broker caches its last accepted transaction and exact batch bytes: retrying
that transaction/body after a lost acknowledgment returns Done without replay,
including pointer motion. Reusing it with different bytes or sending an older
transaction is Refused. Exhausted does not consume a transaction; retry the
same batch. Restart clears this cache with the physical-state model.

## Pointer route

The manager installs an Invoke-only display sink on the existing main Endpoint;
no additional Endpoint is allocated. A zero display epoch/sequence and only the
reply attachment clears it. Replacement closes the old sink, clears unsent
pointer events and starts the sequence from the manager's display fence. A stale
input epoch returns Restarted and closes the offered sink. Invoke cannot change
the route; refused and malformed requests release their attachments.

`PointerRoute` retains at most 64 snapshots and retries WouldBlock on a timed
wait. A publish batch reserves capacity before changing the focused-client
state, then queues each motion, button and wheel exactly once. Publish replay
acknowledges without forwarding twice. The source is the broker-assigned id;
button snapshots combine all three sources. Keys and secure attention remain in
their existing paths. Sequence exhaustion returns Exhausted rather than wrapping.
A fatal sink failure closes and clears the route for the manager to rebind.
For an ordinary client, pointer delivery follows acceptance by the sink, so a
root fence after an event credit cannot overtake its queued pointer updates.
A brokerPointer client queues keys only; its credit can answer while the route
is blocked, avoiding a cycle between that route and the compositor's key wait.
Session clients use the display route; InputTerminal receives ordinary delivery
when no session is open.

The product root service and test input scenario set this route whenever both
brokers run. The display owns position, clamping, visibility and composition;
the Input Broker holds no output geometry. See [display and composition](Display.md).

## Session order

RegisterClient word 1 bit 0 selects `brokerPointer`; other bits are refused.
Only one registration may select it (a second is Refused). Its key sequence
starts at zero and counts every queued key and synthesized release in that
registration and broker epoch. Each accepted pointer route entry captures that
count in word 7 at its position within Publish, including modifier makes
flushed earlier in the batch. Publish replay never increments it again.
The counter cannot wrap; refusal leaves the committed queues unchanged.

Each retained session key carries the preceding route sequence. Events word 3 is
that horizon; Peek ends its batch at a change of horizon, even within one Publish.
Ordinary console clients keep mixed key/pointer batches with word 3 zero.
A blocked credit freezes its event count and retains its exact prefix and
horizon while new input is queued. Compact retained entries preserve all wire
fields while adding the horizon; held keys retain only source and usage.

The display broker carries the stamp unchanged through FaceSnapshots (the
wire name for PointerSnapshots) answering FaceCredit (SessionCredit). Motion
coalesces only at equal stamps and button state, never across a button edge.
SessionCompositor uses InputOrder: keys wait for their horizon, pointers wait
for their stamp. Both streams have 64 entries. It applies ready actions in
broker order with the current WindowManager focus, retaining no old-focus
recipient while waiting. A stale epoch, refusal, queue exhaustion or five-second
wait drops and counts waiting input. The compositor keeps draining both live
channels after a merge failure, discarding further input until restart; it
never forwards waiting keys to the old focus. The display serves snapshots
inside its drain before Render. Client congestion never delays seat credits.

Manage SetFocus word 2 bit 0 sets the root focus lock, cleared by the next
successful Manage SetFocus with zero. The pure broker rejects non-Manage
focus changes while locked. A separate Invoke-only focus Endpoint and
Component Manager authority wiring are unimplemented.
The root-started scenario supplies the compositor's enrolled client Endpoint
and epoch at component bootstrap words 38..39 and 40 respectively; its display
face still uses words 34..35 and 36. No kernel or input-event ABI changes.

The product desktop root registers the console client first, a pointer session client second,
and explicitly focuses the session after its first presented frame. The
root holds the Manage Attention credit in desktop and fallback modes. A
Delete attention has usage 0; while a session pointer client exists,
Ctrl+Alt+F9/F10/F11 are consumed with the same modifier suppression and
carry HID usages 66/67/68 to the root, preserving storage-test staging and
shutdown/reboot without granting power authority to the compositor/client.
Console-only configurations retain the terminal's previous chord handling.
The wire protocol remains version 3; validators explicitly bound the new
Attention usage values. Each driver's source remains sealed and exclusive;
increasing the bound does not allow clients to inject input. Console admission uses at most three sources, while desktop admits
all four published xHCI functions plus the 8042 and the USB host group.

For receiver handoff, the owner stops the sole compositor
consumer, fences its minted client route with Query, then uses Manage RebindClient. The broker
closes its retained credit and immediate answer, clears frozen bytes/transaction/focus request and
returns the same client Endpoint without consuming a slot. Queued keys and their cumulative count
remain; held keys whose make was already delivered are suppressed until release. Resume returns
immutable delivered-key and accepted-pointer bases captured at registration/rebind, plus held
buttons. The compositor starts InputOrder at those bases, discards earlier face snapshots and masks
already-held buttons until physical release. It requests pointer credit only after Baseline, and
requests key credit after Resume. A resumed session's dead credit retains its queue/focus for the
manager's bounded recovery; ordinary clients still detach on stale replies. The display face's FIFO
FaceFence precedes Detach, which closes old pointer credit, drops/counts queued snapshots and clears
staleness. Only the stopped consumer could invoke the old route; no request can be submitted after
its owner's FIFO fence. No endpoint rotation or bootstrap/evidence layout extension is required.

RebindClient word 2 bit 0 retires the abandoned session after its final death: it closes the
credit, drops/counts pending keys, suppresses held state and releases focus. Its Done reply
carries no Endpoint. The root can then focus the console even when the dead session's queue
was full. Zero retains the ordinary handoff described above.
