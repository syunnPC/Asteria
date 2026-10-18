<!-- SPDX-License-Identifier: Apache-2.0 -->

# USB host-controller libraries

These allocation-free libraries implement `Input::IUsbHostController` through
port views. Calls are serialized; controller, device-core and buffer storage
must remain outside target stacks. Components supply register access, DMA
ordering/cache maintenance, clocks, scheduling and input delivery.

| Controller | Profile | Supported topology and transfers |
| --- | --- | --- |
| `XhciController` / `XhciPort` | xHCI 0.96, 1.0–1.2 | Root devices; control, interrupt IN, bulk IN/OUT |
| `EhciController` / `EhciPort` | EHCI 1.0 | High-speed roots and one-level USB2 hubs with full/low-speed children; control and interrupt IN |
| `OhciController` / `OhciPort` | OpenHCI 1.0a | Full/low-speed roots; control and interrupt IN |
| `UhciController` / `UhciPort` | UHCI 1.1 | Full/low-speed roots; control and interrupt IN |

Component wiring is described in [XhciDriver.md](../../../Documentation/Architecture/XhciDriver.md)
and [UsbHostDriver.md](../../../Documentation/Architecture/UsbHostDriver.md).
Class enumeration and HID decoding belong to the [input library](../Input/Readme.md);
[BOT/SCSI mass storage](MassStorageReadme.md) uses a separate pipe interface.
Isochronous transfers, streams, active alternate-setting changes and suspend/resume
are unsupported.

## Memory and lifetime

`DmaMemory { Address, Cpu, Bytes }` describes exclusive, disjoint, contiguous,
pinned CPU/device mappings. Descriptors use little-endian byte codecs; CPU
pointers need no descriptor alignment. Device addresses, capacities, overlap,
overflow and controller address width are validated before reset or DMA writes.
Each mapping is at most 64 KiB. `PublishDma` and `ObserveDma` provide the
platform's MMIO/cache ordering.

Mappings remain valid until `Halt` succeeds. Core buffers remain borrowed until
their matching completion, including during cancellation and disconnect. A
failed halt leaves the controller Faulted with its borrows retained; the component
must revoke DMA or reset and successfully halt before releasing memory. After a
successful halt, drain completions and rebuild the owner/core state before reuse.

## Handoff and request progression

Call `Handoff(barBytes, pollLimit)` before enabling bus mastering or releasing
firmware DMA reservations. xHCI walks bounded extended capabilities and uses the
USBLEGSUP OS semaphore; EHCI uses PCI EECP legacy support; OHCI requests ownership
when InterruptRouting is set. UHCI has no BIOS semaphore: `IUhciConfig` writes
LEGSUP `0xAF00` and verifies `(LEGSUP & 0x20BF) == 0x2000`.
Handoff disables legacy interrupts/traps and acknowledges applicable RW1C status.
Failure does not authorize reset or firmware reservation release. Poll limits
are read counts, not elapsed milliseconds; the component supplies the cadence.
EHCI firmware reservation release also requires trusted controller/DMA quiescence.

Call `Poll(currentMilliseconds)` before submitting the first request. Deliver
`TakeCompletion` results to the device core before its next `Step`. Time is
monotonic and calls never block waiting for hardware. `TimeoutMilliseconds` is
relative to acceptance; zero permits indefinite NAKs. Enumeration uses 1000 ms,
while interrupt IN has no deadline. `Abort(pipe/port, token)` cancels a matching
transfer; stale tokens and non-cancellable controller commands are refused.
Completions are unique and release borrows only after DMA retirement. A HID
report that completes before cancellation takes effect is delivered as Success.
Tokens cannot alias another live request or retained completion.

Disconnect is exposed after retirement, with pending requests completed as
Disconnected. Controller errors require bounded halt recovery. Timer progress
is required for reset, address settling, deadlines and retirement.

## xHCI

The controller supports up to 255 hardware root ports, a caller-supplied pool of
at most 16 attached devices and eight non-control pipes per device. Device-pool
capacity cannot exceed MaxSlots. There is one active controller command; EP0
and separate endpoint pipes can progress independently, each with one request
and one retained completion. Configured endpoints are added individually;
binding selects alternate zero. Hubs and routed children are refused.

| Mapping | Required capacity | Device alignment |
| --- | --- | --- |
| DCBAA | `(device pool capacity + 1) * 8` bytes | 64 |
| Command ring | 16..4096 TRBs, including one Link | 64; one 64 KiB window |
| Event ring / ERST | 16..4096 TRBs without Link / one 16-byte entry | 64; ring in one 64 KiB window |
| Scratchpad array / each scratchpad | Count × 8 bytes / 4096 bytes | 64 / 4096 |
| Input / output context per pooled device | 33 / 32 × context size | 64 |
| EP0 and each endpoint ring | 16..4096 TRBs, including one Link | 64; one 64 KiB window |
| Separate bounce buffer per pipe | 1024..65536 bytes | 64; one 64 KiB window |

HCCPARAMS1 selects 32/64-byte contexts and DMA width. PAGESIZE must support
4096-byte pages; up to 1023 scratchpads are supported. Supported Protocol PSI
tables replace default speed IDs 1..4 within their port ranges; malformed,
ambiguous or unsupported mappings are refused.

Initialization programs DCBAA, command/event rings and interrupter zero.
Interrupt mode enables IMAN.IE/USBCMD.INTE with 1 ms moderation. `Poll` handles
at most `min(eventBudget, 64)` events and 255 ports per call. Ring Link TRBs
toggle cycle; DMA publication precedes ownership and doorbells. Completion
matching checks pointer, slot, endpoint and residual length. Short control IN
waits for Status completion. Invalid events increment `RejectedEvents`.

USB2 connection uses CCS even while speed is Undefined. Attach debounce is
100 ms; reset completion requires PR/PRC evidence rather than PED alone. Reset
with an undecodable speed fails. Address Device BSR=1 establishes EP0; BSR=0
assigns the controller address. Evaluate Context preserves EP0 fields except
maximum packet size. Version 0.96 leaves Setup Stage TRT zero.

EP0 stalls and transaction/babble errors use Reset Endpoint/Set Dequeue recovery.
Non-control stalls are returned for class policy. `ClearHalt` uses EP0 to clear
the device halt and reset host dequeue/toggle; the target pipe must be idle with
no retained completion. Cancellation uses Stop Endpoint, Set Dequeue and event
draining, without requiring optional forced stopped events. Transfer events
cannot restart a cancelled request. Reset and command watchdogs are 1000 ms.

## EHCI and USB2 hubs

One active request shares a reclamation QH, transfer QH and periodic schedule;
up to 16 logical pipes retain independent interrupt endpoints and DATA toggles.
`IsBusy()` lets components serialize devices without consuming submission retries.

| Mapping | Minimum bytes | Device alignment |
| --- | ---: | ---: |
| Periodic frame list | 4096 | 4096 |
| Async head / transfer QH | 96 each | 32 |
| Three qTDs at 64-byte strides | 192 | 32 |
| Setup/data bounce buffer | 1088 | 64 |

Schedule regions share CTRLDSSEGMENT and cannot cross 4 GiB boundaries. Without
64-bit addressing, complete mappings must be below 4 GiB. Buffer page pointers
carry their own high dwords. Short control IN follows the alternate link to
Status. Root reset clears endpoint configuration/toggles on all associated pipes.

FS/LS intervals round down to a power of two frames; HS intervals use
`2^(bInterval-1)` microframes. For bInterval 15/16, **Poll must run at least
every 1 ms** to gate service across the 1024-frame-list wrap. Async cancellation
waits for IAA; periodic cancellation waits two frame boundaries. Schedule-off
acknowledgment protects reuse. Split-control cancellation/errors also clear the
hub TT buffer. Missing retirement acknowledgment for 100 ms triggers bounded
halt recovery; failed halt retains borrows.

`UsbHub` supports class-9 USB2 hubs with at most 15 ports. It configures/powers
ports, waits bPwrOn2PwrGood plus 100 ms debounce, polls status/change and resets
children with a 1000 ms bound and 10 ms recovery. Attach each child through a
dedicated `EhciPort`/`UsbHubPort` route. Serialize address-zero enumeration until
SET_ADDRESS settles. Quiesce child borrows before disconnect and drain all
children before root-hub teardown. Single/multi-TT hubs share a conservative
one-transfer schedule; second-level hubs and high-bandwidth interrupts are refused.
Full/low-speed root devices are handed to companion controllers where present;
integrated rate-matching hubs use the child route instead.

## OHCI and UHCI

`Usb1Controller` supplies serialized request handling with at most 16 logical
pipes, one active controller request and one retained completion per pipe.
OHCI supports 1..15 root ports; UHCI supports two. Each pipe retains one
interrupt IN endpoint and its toggle. Root disconnect clears all associated
pipe configurations. Controller views do not support hub routes or bulk.

All mappings must lie wholly below 4 GiB:

| Controller | Mapping | Minimum bytes | Device alignment |
| --- | --- | ---: | ---: |
| OHCI | HCCA / endpoints / descriptors / buffer | 256 / 32 / 64 / 1088 | 256 / 16 / 16 / 64 |
| UHCI | Frame list / queues / descriptors / buffer | 4096 / 32 / 2080 / 1088 | 4096 / 16 / 16 / 64 |

OHCI's shared HCCA requires coherent/uncached DMA or cache maintenance that
preserves concurrent hardware updates. `IOhciMmio` provides aligned BAR-relative
dword access. `IUhciPortIo` provides byte/word/dword accesses at I/O BAR offsets.
Accessors must surface platform failures rather than silently discard writes.

OHCI retains firmware frame calibration, programs HCCA/control EDs and powers
the root hub. UHCI resets and programs QH/frame schedules; reset assertion is
50 ms, followed by enable/recovery waits. Address settling is 2 ms. Call `Poll`
at least every 1 ms while requests/DMA are active. Intervals 2..255 are gated
through one upcoming frame slot, including NAK retries and table wrap.

OHCI validates reversed done-list pointers and drains at most three TDs. UHCI
keeps active retries/NAKs pending; short IN advances to Status after a halt.
Both capture hardware toggles before reuse, and successful CLEAR_FEATURE resets
the matching interrupt endpoint to DATA0. OHCI cancellation leaves a skipped
ED reachable for two SOFs before unlinking. UHCI completed reports retire in
two frames; cancellation waits 33 frames to cover asynchronous queue retention.
Missing SOFs trigger bounded halt/reset fallback after 100 ms. Synchronous halt
requires HCR/HCRESET completion, not only software-visible halted status.

## Verification and references

[USB tests](../../../Tests/User/Usb/Readme.md) describe device-address schedule
models, seeded replay, deadline/abort cases, parser fuzzing and mutation commands.
Target builds compile at O0/O2 with 512-byte frame checks on AMD64 and IA-64.

Protocol sources: [xHCI 1.2 with appendix G's 0.96 exceptions](https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf),
[EHCI 1.0](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/ehci-specification-for-usb.pdf),
[UHCI 1.1](https://ftp.netbsd.org/pub/NetBSD/misc/blymn/uhci11d.pdf),
[USB 2.0](https://www.usb.org/document-library/usb-20-specification),
[HID 1.11](https://www.usb.org/document-library/device-class-definition-hid-111).
