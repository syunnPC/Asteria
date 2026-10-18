<!-- SPDX-License-Identifier: Apache-2.0 -->

# xHCI HID component

Behaviour 19 serves AMD64 PCI class 0C/03/30 controllers. IA-64 has a link-time
unsupported behaviour entry; platform publication is not implemented. The common controller,
register adapter, memory owner, input transport and recovery state machine are
ISA-neutral, like the AHCI and NIC components. Implementation follows the USB,
xHCI and HID specifications; no third-party driver code is incorporated.

The behaviour is linked into the component image, installed as
`\EFI\ASTERIA\MODULES\COMP.ELF` for both product and test boots. `BOOT.CFG`
lists it as `module = component COMP.ELF crc32c=...`; the boot application
checks its CRC-32C and the kernel maps the retained module table read-only into
the pager. The pager resolves `component` in that table; its
image carries neither embedded behaviour objects nor weak component symbols.

Boot walks the PCI catalog and publishes BAR0 and function/MSI authority for
all xHCI functions within the four-controller bootstrap capacity. A capacity
failure reports each omitted BDF explicitly. Before ExitBootServices, the report
captures authoritative firmware BAR0 extents without changing PCI or controller
registers. Publication prefers these extents, validates both halves of 64-bit
BARs against PCI configuration, and preserves firmware command state. A running
firmware controller therefore needs no destructive BAR probe. Without a decoded
firmware extent the runtime's bounded probe remains available for a quiescent
function. All physical addresses, DeviceRegions, page mappings and IOMMU addresses
retain 64 bits, including BARs above 4 GiB.
Publication changes the inventory owner to `xhci driver`. Bootstrap words 51–66
hold four function/region pairs after the first 51 words, including the
module table base and size; word 67 carries calibrated clock ticks per microsecond. Device Manager retains
assignment and recovery authority; the child receives only its own function,
region, domain, WaitSet and a root-enrolled Input Broker source.

The 44-word component block carries the clock scale at word 43. Startup maps
up to 16 BAR pages, performs the bounded USB Legacy Support BIOS handoff with
an independent OS semaphore byte write, and uses at most 1000 one-millisecond
polls for each ownership/reset wait. Failure ends the component and invokes
normal quarantine recovery. DMA requires an attached IommuDomain behind VT-d
or AMD-Vi. No IOMMU means startup refuses DMA; bus mastering remains disabled.
No RMRR reservation is released by this component.

The hardware root-port limit is the specification's 255. Each root keeps only
protocol/speed mapping, power and device-pool bookkeeping. A bounded pool of 16
attached root devices owns the contexts, EP0 ring/buffer, and eight independent
endpoint ring/buffer pairs. CONFIG.MaxSlotsEn is the pool capacity (at most the
hardware MaxSlots), independently of MaxPorts. The first connected roots acquire
pool entries; roots without an available entry remain unbound and are reconsidered
every poll. An entry is reused only after Disable Slot completion, all retained
completions, and the USB core's decoder state are drained. Hubs and routed devices
remain outside the existing root-device profile; hub routing is unsupported.

The DMA window is reserved for 324 pages at IOVA 0x100000: 68 base pages plus a
**256-page scratchpad budget** (1 MiB). Only the requested scratchpad pages are
allocated/mapped, so a controller with no scratchpads maps 68 DMA pages. The
first four pages hold DCBAA (256 bytes), command ring (512 bytes), event ring
(8192 bytes, 512 events), ERST (64 bytes at byte 8960), and the scratchpad address
array (2048 bytes, starting at byte 9024). The event capacity covers one change
per root plus the bounded outstanding device operations; a 255-root change burst
is exercised by the register model. Sixteen four-page device areas follow at byte 16384.
Each supports 32- or 64-byte contexts, a 256-byte EP0 ring, a 1024-byte bounce
buffer, and eight 256-byte endpoint rings with independent 1024-byte buffers.
Scratchpad page zero starts at DMA page 68. DMA Frames contain at most 32 pages;
all subobjects fit a single owning Frame and all rings/buffers stay in one
64 KiB window. Each 32-page Frame is programmed at the device address IommuMap
answers: IOVA 0x100000 onward behind a translating IOMMU, its physical address
on the trusted-driver profile's identity backend ([DMA isolation](IommuDomain.md)).
The 64 KiB property holds only from a 16 KiB-aligned device address, so each
Frame asks for 4-page physical alignment and the driver refuses an answer that
is not so aligned. The CPU-only 57-page state arena follows the reserved DMA window,
in two Frames of 32 and 25 pages. The controller occupies the first 17 pages;
six USB cores use the remaining 15 pages of the first Frame, and ten cores
use the second Frame. It holds the placement-owned controller,
register adapter and sixteen USB cores, and is never mapped into the IOMMU.

The core validates the full specification range of 0–1023 scratchpads, alignment,
range, disjoint ownership and protocol geometry before reset. The component
budget limits scratchpads to 256 pages independently of the hardware maximum. A count above 256
fails with `ComponentDriverScratchpadStep` (24), `ResourceExhausted`; a controller
whose PAGESIZE does not advertise exactly 4 KiB fails with
`ComponentDriverPageSizeStep` (25), `InvalidConfiguration`. There is no silently
truncated scratchpad array. Resource exhaustion within the admitted budget also
reports the explicit page allocation or device-map step. No implicit allocation
or runtime static constructor is used.

Supported Protocol capabilities map every USB2/USB3 range without a 32-bit port
bitmap. Standard IDs or explicit PSI bit rates decode the negotiated speed; the
actual Port Speed ID enters Slot Context even when it differs from the implied
IDs. Symmetric USB2 rates and SuperSpeed/Plus rates, including paired asymmetric
USB3 PSI entries, are recognized. USB2 reset requests PR; USB3 waits for automatic
training/PED without warm-resetting a functioning firmware-used port. Firmware
running and halted states both pass CNR-clear, halt, HCRST-clear and CNR-clear
waits before fresh contexts/rings are installed. After ownership handoff, the
component stops the firmware schedule before attaching its replacement IOMMU
domain; old firmware rings cannot run against the new DMA mappings. Legacy ownership uses independent
BIOS/OS semaphores and disables all five SMI enable bits. PORTSC operations keep
PP/PIC/wake fields, never replay PED/PR/LWS, and acknowledge only sampled W1C
change bits. 255-port loops use wider counters to avoid wrapping at port 255.

Root ports with advertised power control are powered on without changing
PED or acknowledging unrelated changes, followed by a 20 ms stabilization wait.
Initial enumeration settles before readiness (bounded to five seconds).
Context operations queue in their root port's owned input context while
another port uses the shared command ring. That serialization does not
consume a USB device retry; stepping several interfaces in one tick cannot
make a neighbouring device fail enumeration through controller contention.

When MSI is available, one Interrupt is bound to the driver's WaitSet. A
function without MSI uses the same ten-millisecond timed polling loop with
USBCMD.INTE and IMAN.IE clear. MSI-X and INTx are not enabled by this driver.
Other interrupt-creation failures remain startup failures. This permits the
default qemu-xhci configuration, which exposes MSI-X without MSI, to boot the
ESP from USB and continue serving HID after firmware controller use. The event ring is drained
before rearming and again on each wake; a ten-millisecond timed wait also
services enumeration and root-port changes. Enumeration uses the library's
USB descriptor/configuration model and binds every supported boot keyboard or
mouse interface, including composite devices. Every bound interface keeps its
own interrupt IN transfer queued, without a transfer deadline. Idle NAKs do
not retire or reset a device. Enumeration controls retain their finite deadlines.
Hubs and general HID report descriptors are outside this root-device profile.
A STALLed SET_IDLE is tolerated (it is optional for boot mice, HID 1.11
Appendix G); a STALLed SET_PROTOCOL drops only that interface, whose reports
would not be in boot format. Either STALL first resets the controller's halted
Default Control Endpoint. A device attached while the controller runs is reset
after the USB 2.0 100 ms attach debounce.
Enumeration reads unsupported devices' descriptors but sends no
SET_CONFIGURATION, HID class request or endpoint poll to them. Mass storage,
root hubs and other non-boot interfaces stay idle until disconnect; their
local unsupported state cannot fail the controller or other root ports.
Disconnect retires the slot and its endpoint transfers before reusing it.
Over-current removes port power, latches the port disabled for that controller
incarnation and reports its bit in the manager Query response.

Decoded key edges, relative motion and button changes enter the bounded
64-event FIFO. Publish freezes at most eight events until acknowledged, and
uses the Input Broker source/epoch protocol. Broker source stamping and secure
attention handling apply equally to USB and PS/2. display composition's trusted pointer
route carries broker motion/button events to the display broker; the focused
terminal receives events for echo. Query is a publication fence: word 2 has
the number of polling interfaces in its low half and the powered-off-port
bitmap for the first 32 roots in its high half; word 3 counts decoded events.
Higher roots retain independent power state without an out-of-range shift.

A killed component is drained first: MSI and bus mastering are disabled,
translations revoked and DMA pins parked against its quarantined assignment.
Device Manager maps the bounded BAR and verifies that BIOS ownership is clear
across its extended-capability chain. A handoff timeout retains quarantine
without resetting firmware-owned hardware. It then clears Run/Stop and controller interrupt enable, waits for
HCHalted with Controller Not Ready clear, issues HCRST, then requires reset
clear, Controller Not Ready clear and HCHalted still set. Only that final quiet
proof permits DeviceQuiesced, pin release and reassignment. A bounded timeout
retains quarantine. The new incarnation constructs fresh CPU state and rings.
The proof is reported as `# usb: xhci0 quiet (was running, halted and reset, 2
polls, status 0x00000001), released` (or `not quiet:..., quarantined`).

Product init restarts an xHCI driver that fails while serving after Device
Manager quiescence ([driver isolation](IommuDomain.md)), as it restarts the USB host group (`RecoverInputDrivers`,
[UsbHostDriver.md](UsbHostDriver.md)): after the quiet proof the root fences
and registers the driver's broker source again, which releases the keys the
dead instance held and clears its replay fence, and starts it in a fresh
domain (`# init: xhci 0000:00:08.0 restarting (1 of 3)`, `... restarted (1 of
3)`), at most three times per boot (`... gave up after 3 restarts`). A
driver whose first start failed is retired, not restarted.

Product init starts the broker after display, starts PS/2 first when published,
then xHCI in PCI catalog order and then the USB host group. The five component
slots permit display, broker, PS/2, one xHCI and the group; without PS/2, two
xHCI instances and the group. Additional controllers remain published and explicitly deferred.
The linked-image grant checks account for broker, driver, Interrupt and memory
object usage against the current kernel pool capacities.
Child xHCI page quotas use the captured
scratchpad count instead of reserving the whole component scratchpad budget.
The peak gate covers one controller with 256 scratchpads, two with 128
scratchpads shared (for example 64+64 or 0+128), or one with 128 beside the USB
host group (`five components`). Larger simultaneous counts may
fail ResourceDomain admission cleanly within the global grant; a
controller's individual supported budget does not promise simultaneous capacity
for another controller.
Both product and test boot profiles make input drivers optional: a
startup failure or later exit reports a continuing line with segment/BDF, driver
step, syscall error and full DriverFailure exit code and retires only
that driver. Display, Input Broker and terminal remain resident. Readiness
waits cover handoff/reset and the bounded enumeration settle interval.

The standalone test-pager path uses display, broker, one driver and an
unfocused probe, without the other driver demonstrations. `Amd64XhciHid` covers
VT-d, AMD-Vi and product boot with qemu-xhci, USB keyboard and USB mouse. QEMU
is configured with MSI enabled and MSI-X disabled. QMP input-send-event names
`usbconsole`, the VGA device ID, and usb-kbd routes to that console; QMP's
device argument is a display ID, not a USB device ID. The scenario verifies
ordered events, secure attention, a second empty client, MSI delivery, screen
pixels, kill/reassignment while interrupt polls are queued, and USB mouse
unplug/reconnect with the other interface still bound. The kill goes through
the product's recovery (`RecoverInputDrivers`): after the reconnect the runner
types `asteria`, Enter, a move and a click through the restarted driver, and
three more kills restart it twice and give it up, each after a quiet proof.
The suite also boots the product ESP as an MBR USB mass-storage device on
the same default qemu-xhci as its keyboard and mouse, with VT-d and two CPUs,
and checks serial echo, screen pixels and clamped cursor positions. A product
case with DMA isolation intentionally unavailable forces startup to fail and
checks the retained PS/2 terminal and display. `Amd64.XhciUsbLargeProduct` boots
the product from a USB stick on `qemu-xhci,p2=15,p3=15`, with keyboard/mouse on
logical ports 14/15 (high USB2 controller port numbers). The default MSI-X-only
configuration exercises polling; `Amd64.XhciX2Apic` exercises MSI with x2APIC
and destination IDs below 255. MSI-X and INTx remain disabled. The test pager reports absent or foreign scripted
xHCI input and falls back to real-device echo, as its PS/2 path does; the
runner continues to require the scripted success line.
`Amd64.XhciUsbHardware` boots the test ESP from USB without scripted input,
waits for that fallback, and then checks keyboard echo and cursor rendering.

The NEC/Renesas uPD720200 uses xHCI 0.96 and
MSI. The library selects its 0.96 context/register exceptions (including a
zero Setup Stage TRT) and supports direct low/full/high-speed devices on its
USB2 protocol ports as well as SuperSpeed ports. USB2-only ports on that
controller are enumerated by xHCI; no EHCI companion handoff is attempted.
Intel 6-series EHCI functions, their firmware ownership and their VT-d RMRRs
are untouched.

The enumeration path uses CCS alone to establish connection: a USB2 port's
Port Speed may read Undefined until reset and while PR is asserted (xHCI 1.2
sections 4.19.1.1 and 5.4.8). A STALLed SET_IDLE is recovered without failing
the device or leaving EP0 halted. `XhciHardwareTests` models these paths,
which need hardware-oriented coverage beyond emulator enumeration defaults.

## Enumeration diagnostics

For machines without a serial port, product init asks each xHCI driver for
enumeration progress: start-block word 37 carries the catalog index in its
low byte and `Abi::XhciReportProgressFlag`. The controller and USB core report
structured records (`Input::UsbDiagnostic`) through an optional synchronous
sink; the driver's `XhciProgress` queue keeps at most 64, forwards at most 48
per root port between connects and 400 per incarnation, announcing each limit
and any queue loss once, and sends at most four one-record frames
(`XhciProgressMethod`, `UsbProgressFrame.hpp`) per loop on its report
Endpoint. A full queue keeps the record for the next loop; another send error
only disables the queue. Readiness is retried while the report Endpoint's
per-sender share is full. The root service prints each record as one line on
the console and the debug channel, wherever it waits on that Endpoint: the
driver's readiness, broker answers and the terminal. A partial edited line is
erased first and redrawn after the diagnostic, so screen and serial keep the
same line order; the display metrics refresh uses the same path. Test-pager
starts send no flag and receive no records. IA-64's pagers link the decoder
and formatter too since the [USB host group driver](UsbHostDriver.md) reports
there. Records name their controller by kind and index
(`UsbControllerByte`): an xHCI driver's byte is its catalog index, so its
lines keep their `xhciN` form.

Lines also report command and transfer completion
codes other than success with their command or request, reset timeouts,
controller errors, report errors, the failing enumeration state, and
disconnects. A Stop Endpoint that loses its race with a STALL prints
`command stop endpoint failed code=19 (context state error)` before the
ordinary Reset Endpoint recovery; that line alone is not a failure. The report
Endpoint queues six normal messages and four per sender, so two controllers
whose diagnostics the root has not yet drained can briefly fill it: the Input
Broker then retries its event delivery on its timed wait, and nothing is lost. `Amd64XhciHid`'s product cases require the keyboard's and the
mouse's polling lines and compare the screen including every `# usb:` line.

The firmware report appends raw capability and operational values to each xHCI
PCI line, followed by USBLEGSUP/USBLEGCTLSTS, every bounded Supported Protocol
capability, and at most 32 connected PORTSC entries after scanning at most 255
roots. It performs no register writes, requires PCI MSE and a decoded firmware
BAR descriptor, bounds all reads by that extent, and tolerates all-ones reads.
The immutable inventory carries the captured BAR extent and publication index,
step and error. Product `# devices:` lines show publication success or the
capture/publication/capacity error; step 4
means a refused VT-d firmware handoff withheld every DMA device.
Host register models cover 255 roots with two device resources, high root reuse,
32/64-byte contexts, non-default PSI IDs, running/halted firmware state, page-size
refusal, scratchpad counts 17/256/1023, and DMA above 4 GiB. A kernel publication model covers BARs above 4 GiB with bus mastering enabled.
Physical firmware handoff, port routing, power, interrupt and DMA delivery
require hardware validation.

References: [Intel xHCI specification, revision 1.2, May 2019](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/extensible-host-controler-interface-usb-xhci.pdf),
[Renesas USB product guide](https://www.renesas.com/en/document/fly/usb-solutions-product-guide),
[QEMU input-send-event interface](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html#command-input-send-event).
