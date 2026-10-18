<!-- SPDX-License-Identifier: Apache-2.0 -->

# Pure input libraries

`Asteria::User::Input` parses input without system calls, allocation, exceptions,
hardware I/O, blocking waits or callbacks. Drivers serialize calls and keep
`UsbDevice` and `EventBatch` outside target stacks. A batch holds up to 20 events;
drivers split it into `InputFrame.hpp` wire batches of at most eight. The Input
Broker replaces driver-supplied source tags with registered identifiers.

## Events and repeat

`InputEvent` is a 24-byte, four-byte-aligned little-endian ABI:

| Field | Offset | Type |
| --- | ---: | --- |
| Kind / Modifiers / Usage | 0 / 1 / 2 | u8 / u8 / u16 |
| Source / Dx / Dy | 4 / 8 / 12 | u32 / i32 / i32 |
| Buttons / Reserved / Wheel | 16 / 17 / 20 | u8 / three zero bytes / i32 |

Kinds are KeyDown=1, KeyUp=2, Motion=3, Buttons=4, Wheel=5. Keys use HID page
0x07; modifier bits follow LCtrl, LShift, LAlt, LGui, RCtrl, RShift, RAlt, RGui.
Motion is positive right/down, wheel positive up, and button bits 0/1/2 mean
left/right/middle. Irrelevant fields are zero. Frame codecs validate kinds and
encode fields explicitly.

`KeyRepeat` accepts key edges and unsigned microsecond timestamps, retaining
one repeating usage/source and modifier snapshots for up to four sources.
Defaults are 500000 microseconds delay and 30 repeats/second (33334 microseconds
per period). A new repeatable key replaces the old one; releasing it stops
repeat. Duplicate makes preserve delay. Modifiers and lock keys do not repeat.
`SourceGone` cancels a removed source; `Reset` handles focus/session loss.

Calls use modular unsigned time with observations less than 2^63 microseconds
apart. Late wakes produce at most eight repeats and skip missed slots.
`Settle(now, settle)` moves an overdue repeat to now + settle so queued releases
can be processed first. Delay/rate clamp to 1..2^63-1 microseconds and 1..1000
per second. The terminal repeats printable characters, Backspace, Tab and Enter;
missing clock/scale disables repeat. Releases are processed before due repeats.
Drivers emit `UsbDevice::ReleaseKeys`/`HidBoot::Release` before resetting decoder
state on disconnect, and broker focus changes release held keys.

## PS/2 and HID codecs

`Ps2Keyboard` supports scan set 2 and translated scan set 1. Both tables follow
[Microsoft Keyboard Scan Code Specification 1.3a](https://download.microsoft.com/download/1/6/1/161ba512-40e2-4cc9-843a-923143f3456c/scancode.doc),
Appendices A/C; Far East usages come from HID Usage Tables page 0x07.
Layout policy resolves the ANSI/ISO 0x5D position; JIS includes its additional
codes. Unsupported and consumer-page codes yield Unknown. Print Screen's fake
shifts are ignored; Pause is a pulse. Repeated makes and releases of unpressed
keys emit no event. Controller responses abandon incomplete prefixes; malformed
Pause sequences consume their mismatching byte. Reset clears held keys.

`Ps2Mouse` defaults to three-byte packets. Bit 3 validates packet starts only;
lost payload bytes cannot always be detected. Overflow discards motion while
preserving button/wheel changes. Select IntelliMouse only after confirming ID 3;
its fourth byte is fully signed. ID-4 Explorer mode is unsupported. Y/wheel signs
are converted to the common event convention.

`HidBoot` accepts reports without report IDs: keyboard requires eight bytes,
mouse three, with an optional fourth wheel byte. Rollover reports are ignored;
other errors, nonzero reserved bytes and modifiers in ordinary slots reject
without changing state. Duplicate usages form a set, and emitted keys carry
the final modifier snapshot. Short/rejected reports emit nothing.

## USB descriptors and enumeration

`UsbConfiguration` borrows one immutable complete configuration of at most
1024 bytes. It retains interfaces/alternates, endpoints, SuperSpeed companions,
IADs and HID entries. Duplicate or colliding entries, invalid counts/lengths,
orphans and malformed companions/IADs reject atomically with `DescriptorError`.
Unknown class descriptors are skipped using validated lengths.

`Bind` selects alternate-zero boot keyboard/mouse interfaces (03/01/01 and
03/01/02) with interrupt IN, or mass storage (08/06/50) with bulk IN/OUT.
It reports reasons for unsupported or ambiguous bindings. MSC binding issues
no class I/O. `UsbDescriptors::FindBoot` selects the first boot binding.

`UsbDevice` enumerates a root or one-level hub-child device and up to eight
HID boot interfaces: reset, initial descriptor/EP0 size, address, full device
and configuration descriptors, SET_CONFIGURATION, then boot protocol, idle
and endpoint configuration for each HID interface. Unsupported configurations
are tried within the advertised count; malformed ones fail without partial binding.
Each interface owns its buffer, token, retry budget and decoder. `Step` submits
at most one operation, rotating idle interfaces; `Complete` routes by device token.
`InterfaceCount`/`GetInterface(index)` expose all bindings.

Operations have three attempts. Retryable, TimedOut and submission refusal
consume them; success replenishes them. Polling Stall is exposed for caller
recovery without changing HID state; Cancelled resets that pipe's budget.
Enumeration failure still requires other pipe completions to be drained.

Adapters own DMA, scheduling, toggles and reset/address delays.
`TimeoutMilliseconds` is relative to acceptance: enumeration uses 1000 ms,
interrupt IN zero. `Abort` completes once after DMA retirement. Borrowed buffers
remain live until completion; disconnect/reattach requires all borrows quiescent
and completions delivered. Tokens never wrap; reattachment uses explicit `Attach`.
See the [controller library](../Usb/Readme.md) for controller-specific contracts
and [XhciDriver.md](../../../Documentation/Architecture/XhciDriver.md) for system wiring.

## Verification

`Tests/User/Input` covers events, repeat, PS/2, HID, descriptors and enumeration.
Seeded models check pressed sets, modifiers, rejection atomicity, wrap, source
loss and USB retry/disconnect transitions. `Tests/Fuzz` contains codec harnesses.
[USB verification](../../../Tests/User/Usb/Readme.md) covers composite reports,
bulk traffic and cancellation. Native builds enforce O0/O2 stack budgets.
