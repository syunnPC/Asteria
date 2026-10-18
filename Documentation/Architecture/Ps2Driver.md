<!-- SPDX-License-Identifier: Apache-2.0 -->

# PS/2 input component

Behaviour 18 runs the same freestanding driver on AMD64 and IA-64. Both publish
i8042 authorities when firmware evidence and the status-port gate hold. The component quota is one Endpoint, one Thread, image/stack pages and
no device window or DMA. The loader supplies its own Manage ResourceDomain
at 24/25, Manage|Wait WaitSet at 6/7, receive Endpoint at 8/9 and
Invoke|Duplicate reply at 12/13. Role-specific words 30/31 and 32/33 hold the
Read|Write data and command port regions; 38/39 and 40/41 hold Invoke on the
keyboard and mouse InterruptLines. Words 34/35 hold the broker's minted
Invoke|Duplicate source Endpoint, 36 the broker epoch, and 2/3 Invoke on the
root's report Endpoint. The common block has 44 words; xHCI uses the clock scale at word 43. The root
retains all hardware authorities and broker Manage; the driver receives none
of the controller's reset/A20 commands and cannot enroll a source itself.

`Ps2Controller` uses a serialized port adapter and at most three transmissions
of a device byte on RESEND. Every wait polls at least 65,536 times; with the
component clock (ISA counter, start-block word 43 ticks per microsecond) it
also lasts at least 500 ms, reads status once more after that bound, and ends
after 256 times the poll floor even if the clock stops. A slow embedded
controller is therefore not declared dead by a fast processor. Drains stay
poll-bounded (each poll that finds a byte consumes it). It disables the
ports (AD, A7), drains the shared output buffer and reads the configuration
(20). The **set-2 path** writes it (60 plus data) preserving unrelated bits and
port-disable flags, clearing translation and the auxiliary interrupt and
setting the keyboard interrupt, reads it back (20) to prove translation is off,
enables the keyboard port (AE), disables scanning (F5), queries the scan set
(F0, 00), selects set 2 (F0, 02) only when necessary, and enables scanning
(F4). A configuration answer with the reserved bit 7 set and a set answer of
FA/AA are unsolicited and skipped (at most four per answer).

When any step after AD/A7/flush fails or answers outside the protocol (no
configuration byte, translation still on after the write, no ACK, RESEND on
all three transmissions, a set number other than 1-3, or the translated set
numbers 43/41/3F), the **fallback** restores the firmware's configuration byte
with its translation bit (forced on when translation was observed), sends no
scan-set command, enables the keyboard port (AE) and scanning (F4, sent twice
at most and tolerated when unanswered) and drains. The keyboard is then decoded
as set 1 (translation on), or as set 2 when the firmware had translation off.
Without a configuration byte the controller is left as the firmware set it,
translation on is assumed and the auxiliary probe is skipped. Embedded
controllers that emulate an 8042 commonly implement only translated set 2. Only failures of AD, A7, the
flush, the fallback's 60/AE or its final drain fail startup.

The mouse (when declared, below) follows with the decided translation bit:
auxiliary commands each have a D4 prefix: defaults F6,
stream EA, sample rates F3/200, F3/100, F3/80, ID F2, reporting F4. Only ID 3
selects IntelliMouse four-byte packets; ID 0 retains standard three-byte
packets. No controller self-test, reset or device reset
is sent. Status bit 5 routes every byte; parity/timeout bytes are discarded.
Unsolicited bytes encountered while awaiting ACK are decoded and retained; in
a fallback mode the driver discards their events before it publishes.

**Diagnostics.** A failure records the step, the reason (timeout, unexpected
answer, RESEND exhausted, transport), the last byte written to either port,
the last status byte read and the last data byte read (or none). The driver
exits with `0xC0DE00F0` plus that record in bits 32-63 (step bits 0-4,
reason 5-6, data-valid 7, command 8-15, status 16-23, data 24-31;
`Input/Ps2Diagnostic.hpp`), and product init writes, on screen and serial,
`# init: ps2 failed, continuing: keyboard F0 00 (cmd 0x00, status 0x14, byte
0xfa, timeout) code=0x...`. The ready report's method keeps 0x8042/0x8043 in
bits 0-15 and carries the keyboard mode in bits 16-19; a fallback's set-2
failure travels as the report's transaction identifier. Product init writes
`# init: ps2 keyboard=set2`, or for example `# init: ps2 keyboard=set1-translated;
set 2 failed at keyboard F0 00 (cmd 0x00, status 0x14, byte 0xfe, resend
exhausted)`, before its ready line.

The driver creates two Interrupts from its line authorities and binds them to
its existing WaitSet. It re-arms a line and then drains the controller until
status reports the output buffer empty, including initial arming. That order
serves edge- and level-triggered lines alike: a byte that arrived while the pin
was masked holds the controller's request until this drain reads it, and a
byte arriving after the final empty read raises a new request on the unmasked
pin (draining before re-arming would lose that edge).
The keyboard decoder suppresses repeated makes and uses scan set 2, or set 1
in a translated fallback. Tables and Print Screen, Pause and shift-compensation
sequences follow Microsoft's [Keyboard Scan Code Specification, revision 1.3a](https://download.microsoft.com/download/1/6/1/161ba512-40e2-4cc9-843a-923143f3456c/scancode.doc),
Appendices A and C. The
specification assigns no HID usage to the Far East rows; International1/2/4/5
(0x87, 0x88, 0x8A, 0x8B) are taken from the HID Usage Tables. Mouse decoding
produces relative motion, button transitions and wheel events. A 64-event FIFO
freezes at most eight head events and a nonzero transaction for Publish.
Done removes only that head and increases the transaction; Busy/Exhausted
retain the exact head and wait before another attempt. A lost answer retries
identical bytes, which the broker's accepted-transaction cache deduplicates.
FIFO exhaustion fails the driver rather than silently losing a key release.

Restarted discards queued old input and resets the mouse packet decoder,
preserving the confirmed mouse mode and physical keyboard state. Held keys
still suppress repeated makes and carry their modifiers across renewal.
Revoked/stale source handles take the same path. A single RegisterSource request goes to the root's report Endpoint;
Manage stays with the root, which enrolls on the current broker and forwards
the new source/epoch. The root caches that renewal transaction and never
blindly repeats the broker registration. A lost registration answer cannot be
recovered by minting another slot: broker replacement and manager intervention
are required, as in the Input Broker. The driver resumes only after a successful
renewal reply. This scenario does not exercise a broker process replacement;
host transport tests exercise the epoch renewal path.

On termination or failed startup, domain drain masks and collects both line
Interrupts. The supervisor then calls the Device Manager's i8042 quiet rule:
disable both ports and drain the output buffer to a bounded empty proof. This
also runs on scenario abandonment. The next instance must program the controller
again; graceful device deregistration is unsupported by the broker.

## Verification

Host tests cover controller setup, fallback, decoder tables, event retry and
epoch renewal. Target staging and linked-image grant checks cover both ISAs;
IA-64 access uses no additional translation registers. VM tests wait for the
readiness report before injecting keyboard, pointer and secure-attention events.

The root validates sixteen ordered ordinary key edges, exact motion and button
states and source stamping, renders the received HID characters on Enter,
consumes the manager attention report and queries the driver until all 25
decoded events have acknowledged Publish. It then fences an empty focused
client credit and verifies that the second client received zero events. Query
is a manager fence on the driver's service Endpoint; its reply value is the
saturating 32-bit number of decoded events, and it answers only with no pending Publish.
The completion line is:

`# input: typed 'asteria', pointer moved (5,-3), left button, PS/2, secure attention reported, second client empty`

Without published ports the root prints
`# input: i8042 not published, PS/2 demo skipped`. The runner records the exact
QMP transcript, samples the interrupt evidence when the ready line appears,
and requires at least 13 further deliveries (most edges their own) and zero
further spurious messages.
All earlier drivers have torn down, so that delta belongs to the two i8042
lines. Its normal console model checks every pixel, including the received text.
The stop runner performs the same readiness-gated injection before its display
fence. IA-64 cases without an i8042 require the skip line.

The component implements the ordinary relative stream. Native touchpad packets,
pass-through ports and pointing-stick configuration are unsupported.
The trusted Device Manager must also finish USB legacy ownership/RMRR handoff
before relying on input independent of SMM emulation; the PS/2 component cannot
perform that handoff. Controller resets and self-tests remain prohibited.

[AML boot facts](AmlBootFacts.md) make the mouse optional. Pager word 68
and PS/2 component word 44 carry FadtFlag (1), KeyboardAml (2), MouseAml (4),
and StaUnknown (8). Only FadtFlag or MouseAml permits aux enabling and mouse
commands; a bounded aux failure is reported once and preserves the keyboard.
Both interrupt lines remain available for quiesce and stable handle accounting.

On IA-64, firmware port space is validated as a
unique complete 64 MiB aligned window. The physical UC leaf needs no TR and
orders each access with `mf`/load-acquire, `mf.a`, and a subsequent `mf` before
restoring translation and interrupt state (SDM rev. 2.3 Vol. 2 Part I §§4.4.2,
10.7.1–10.7.4; Part II §§11.1–11.2). The shared gate, ISA override resolver,
write allowlist and diagnostics are in `Kernel/Boot/I8042Publication.hpp`. IA-64 reserves
vectors 0x40–0x4f across PCI and ISA lines; IRQ1/12 default edge/high; a valid
MADT override, or for a namespace-only publication the `_CRS` descriptor, decides
GSI, polarity and trigger, and
dispatch masks and issues vector-matched EOI.
The four pager handles retain words 38–45 and declaration word 68. Root handles
have Inspect/Duplicate/Transfer plus the device rights; driver copies are
attenuated to Read/Write or Invoke. Bound namespace evidence in BootInfo v14 fields is also accepted by local SAPIC topology validation.

Product init uses the keyboard/mouse broker, US terminal, client-side repeat
and display pointer route on both ISAs. VM tests check key edges, repeat, cursor
movement and screen pixels. IA-64 SMP repeat remains an emulator limitation;
its SMP fixture skips the repeat probe.
Machine attachment and namespace presence
come from QEMU binary options and Asteria's case-local ACPI dumps; firmware
tables are not repository fixtures.
