<!-- SPDX-License-Identifier: Apache-2.0 -->

# USB 1.1/2.0 host group driver

The `usb` module (`\EFI\ASTERIA\MODULES\USB.ELF`, `module = usb USB.ELF crc32c=...`)
drives every EHCI, OHCI and UHCI controller the boot publishes, on both ISAs,
for HID boot keyboards/mice and bounded report-protocol relative mice on their root ports: behaviour 19
(`ComponentBehaviorXhci`) with `Abi::UsbHostGroupFlag` in start-block word 37.
The xHCI driver uses COMP.ELF; the USB host group uses a separate image.
Component segment bounds are 40 pages on AMD64 and 128 on IA-64
(`User/Pager/ComponentImage.hpp`). It links the pure
libraries of [Source/User/Usb](../../Source/User/Usb/Readme.md) (EHCI, the
shared USB1 policy, OHCI, UHCI), the USB core and HID decoders
(`User/Input/UsbDevice`, `HidBoot`, `HidRelativeMouse`) and the xHCI driver's bounded progress
queue (`XhciProgress`). Everything is written from EHCI 1.0, the UHCI design
guide 1.1, OpenHCI 1.0a, USB 2.0, HID 1.11 and the PIIX4/ICH datasheets; no
third-party driver code is incorporated.

## Publication (both ISAs)

`Platform/Device/UsbHostPublication.cpp` publishes PCI class 0C/03 functions
with programming interface 00 (UHCI), 10 (OHCI) or 20 (EHCI) into the pager's
USB host table: four function/region pairs at start-block words 69-84
(`Abi::PagerDeviceUsbHost`, `PagerBootstrapWords` 88). Functions go in PCI
catalog order and by whole PCI devices: an EHCI and the companion UHCI/OHCI
functions of its device are published together, and a device whose USB host
functions exceed the remaining capacity is skipped whole (step 3), so no EHCI
hands a full-speed device to a companion nobody drives. ICH9/ICH10 boxes have
an EHCI with three UHCI companions per device (four functions, the whole
table); the second device is then skipped.

| Step | Meaning |
| ---: | --- |
| 1 | the runtime refused the BAR or region (error: `PciEcamRuntimeError`) |
| 3 | the device's functions do not fit the table |
| 4 | an IOMMU handoff was refused and no BOOT.CFG trust entry names the function |
| 5 | the function is restricted: a firmware DMA reservation (a VT-d RMRR), a bridge or a display |

Each outcome is a diagnostic line (`ASTERIA:USB:PUBLISHED:<bbddf>` or
`ASTERIA:USB:SKIPPED:<bbddf>:<step>:<err>`) and an inventory record: the
`# devices:` line names the `usb host driver` with `publication=` and the step.

An EHCI's or OHCI's register BAR 0 is the extent the boot application's report
captured from firmware (`CaptureUsbHostBar`, through GetBarAttributes before
ExitBootServices, as for xHCI), so a controller firmware may still use is never
sized; without one it is sized as `CaptureAssignableResource` sizes a quiescent
function. The DeviceRegion covers the whole pages that hold the BAR: a driver
maps pages, and ICH EHCIs have 1 KiB BARs and QEMU's OHCI 256 bytes. A BAR
smaller than a page is published only when no other function's memory BAR
starts in that page (a BAR is aligned to its power-of-two size, so one that
starts elsewhere cannot reach into it); the driver reads BAR 0 to find its
registers within the page. A UHCI's I/O BAR 4 (USBBASE, UHCI 1.1 section 2.1.3)
becomes a 32-byte port I/O region once the function decodes I/O. INTx stays
disabled until the driver's Interrupt enables it. On IA-64 each published
function's INTx line is isolated as the assignable device's; a line that
cannot be isolated (`ASTERIA:USB:INTX-NOT-ISOLATED`) leaves the controller to
polling. USB never stops a boot.

## Configuration access

EHCI's USB Legacy Support capability lives in PCI configuration space at
HCCPARAMS.EECP (EHCI 1.0 section 2.1.7) and UHCI's LEGSUP at 0xC0 (UHCI 1.1
section 5.2.1), while drivers never write configuration space
([driver isolation](IommuDomain.md)). Two
DeviceControl operations give the assignee exactly what the handoff needs,
judged by the kernel (`PciFunctionControl::AccessLegacySupport`):

- `DeviceReadConfiguration` (6): the identity, class, BAR 0 and interrupt
  dwords, an EHCI's dwords from 0x40 on (its EECP list), a UHCI's 0xC0.
- `DeviceWriteLegacySupport` (7): at an EHCI USB Legacy Support capability
  (ID 1 and not in the function's standard capability list, where ID 1 is
  power management) it only sets OS Owned, re-reading the register so the BIOS
  semaphore is never written back stale; at the USBLEGCTLSTS after it only
  zero SMI enables and acknowledged SMI status; at a UHCI's LEGSUP only status
  acknowledgement, PIRQ enable and zero trap/SMI enables. Anything else is
  InvalidArguments.

A driver can thus only relinquish firmware's legacy support, never claim it
back or enable an SMI.

## Start

The root service grants the table's functions at words 30-33 and 45-56
(`ComponentUsbHostRegionWord`). For each function the driver reads its class
dword, maps its register page into window page k, and takes it from firmware:

1. EHCI and OHCI map BAR 0's page; UHCI uses the PortIo system call on its
   region.
2. The library handoff runs with the adapter pacing every read at one
   millisecond (`UsbHostAccess`), so a poll count of 1000 is one second:
   EHCI sets OS Owned and waits for BIOS Owned to clear, then disables every
   SMI; OHCI requests ownership change and waits for InterruptRouting to
   clear; UHCI disables its traps and SMIs and enables PIRQ. A timeout ends
   the instance (step 21, TimedOut) and authorizes nothing.
3. What firmware left running stops before any bus mastering: EHCI and UHCI
   clear Run/Stop and wait for HCHalted, OHCI enters UsbReset.
4. One IommuDomain of 32 address bits confines the group, every function
   attached to it (an IommuDomain takes sixteen requesters); on the trusted
   driver profile the kernel creates the identity backend instead (every
   function needs its own `driver=usb` trust entry, naming module `usb`).
   Without either the instance ends (step 9): a function never masters the
   bus unconfined.
5. The DMA pages are one Frame of `UsbHostDmaPages` (eight), two per
   controller, mapped at the address IommuMap answers (IOVA 0x100000 behind a
   translating IOMMU). Page 0 is the EHCI/UHCI frame list or the OHCI HCCA;
   page 1 holds the queue heads/EDs, descriptors and the bounce buffer at the
   alignments each library asks. Every structure lies below 4 GiB as UHCI and
   OHCI require; the identity backend answers physical addresses, which the
   libraries refuse above 4 GiB.
6. Each function's Interrupt where the kernel offers one (INTx on IA-64; EHCI,
   OHCI and UHCI have no MSI, so AMD64 offers none) is bound to the WaitSet;
   then bus mastering, then the controllers start, EHCI first: CONFIGFLAG
   routes every port to it, and it releases full- and low-speed devices to
   the companions, which start next.
7. Readiness (`XhciReadyMethod` on the report Endpoint, as an xHCI driver's)
   follows the first enumeration: at least 1.5 s, for companions to report a
   released device after their debounce, and at most five seconds.

State (controllers, adapters, six USB cores, four progress queues) lives in
the image's zero-filled `.bss`, constructed in place at entry; the root service
charges its pages with the image's writable segment.

## Devices and scheduling

A root port's device takes one of six slots, a control pipe and, as it
configures them, up to three interrupt IN endpoint pipes of its controller's
sixteen logical pipes, so a composite keyboard and mouse keeps one DATA
toggle per endpoint (USB 2.0 section 8.6). Its USB address is its root port
number: each controller is a bus of its own. Hubs are not driven (their
devices stay unconfigured, as on xHCI); the EHCI library has separate one-level hub support, not exposed by this driver.

Each library accepts one request at a time per controller. The driver
delivers every completion to its core, then looks at port changes, then gives
an idle controller to the next slot that has a request (round robin), except
that a device whose last poll returned a report keeps it for up to four polls,
so a queued burst drains. An idle interrupt poll that held a controller for
24 ms yields when another device, or another interface of the same device,
waits: the driver aborts it, and a report that reached the bounce buffer
before the unlink took effect completes as Success with its data
(`Usb1Controller::WonReport`, the EHCI periodic retirement), so time slicing
loses no report. A yield costs the library's retirement barrier: two frames on
EHCI and OHCI, 33 on UHCI, whose cancellation must outlast an emulator's
32-frame queue validation. Two devices on one UHCI therefore see a key edge
up to about a tenth of a second late; each still receives every report. A
completed UHCI report retires after two frames (nothing is in flight then).

The loop polls every millisecond while a request other than an idle poll is
in flight or a report arrived in the last 100 ms, and every ten otherwise; a
controller's interrupt wakes it sooner. A controller whose library leaves
Running reports a `controller error` record, releases its devices' held keys
and stops; the group's other controllers continue.

## Diagnostics

Product init sets `XhciReportProgressFlag`; each controller has a progress
queue whose records name it by kind and table index (`UsbControllerByte`):

Each controller announces its start with its root ports and whether its
interrupt is bound (`interrupt`, IA-64's INTx) or it is `polled` (AMD64, where
the kernel's Interrupts are MSI). The core's records (device, configuration,
interfaces, requests, a stalled SET_IDLE tolerated, polling, first report) are
the xHCI driver's. IA-64's
product and test pagers link the formatter (`User/Pager/UsbProgress.cpp`).

## Product init and failure

Product init starts the xHCI controllers first, then (controller 4 of the
loop) one group driver from the `usb` image when the table has a function.
Five component slots ([driver isolation](IommuDomain.md)): display and broker leave three,
so PS/2, an xHCI driver and the group run together; with two xHCI drivers
beside PS/2 the group is deferred (`# init: usb deferred, component
capacity`). The ready line ends in `, usb`. A start failure prints `# init:
usb failed, continuing step=... err=0x.. code=0x...` and retires only the
driver.

A group that fails while serving (a fault, a kill, an exit) is recovered by
the Device Manager (`RecoverInputDrivers`): the
failure line, then the drain, then for every function the group held the
proof that it is quiet (`User/System/UsbHostQuiesce.hpp`), one line each:

An EHCI is halted (interrupts masked, Run/Stop and both schedule enables
clear, HCHalted with both schedule statuses clear) and only then reset
(HCRESET, EHCI 1.0 section 2.3.1); an OHCI is put in UsbReset with its lists
off and reset (HostControllerReset, OpenHCI 1.0a section 7.1.3), after which
it must stay out of UsbOperational; a UHCI is halted and reset (HCRESET,
UHCI 1.1 section 2.1.1). A controller firmware still owns (an EHCI's BIOS
Owned semaphore, an OHCI's InterruptRouting, a UHCI's LEGSUP SMI or trap
enables, read before every step) is never written: `not quiet: firmware owns
it, quarantined`. The Device Manager reads a released function's class, BAR 0
and legacy-support dwords itself (DeviceReadConfiguration with Manage) and
reaches a UHCI through PortIo on its region. With every function released and
restarts left (three per boot), the root fences and registers the group's
broker source again, which releases keys the dead instance held, and starts
the group in a fresh domain on it; a failed restart counts as the next
failure. After the third restart the next failure gives the group up
(`# init: usb gave up after 3 restarts`), its functions still released; a
function that stays quarantined ends the restarts at once (`# init: usb not
restarted: its controller stays quarantined`).

Shutdown keeps USB input alive while storage and filesystem services flush.
`CheckPagerGrant.py` checks the overlap's memory and object requirements from
the linked images, including bounce/client-copy storage and test data Frames
([PowerOff](PowerOff.md)). The module table has a sixteen-entry maximum.

itanium2-vpc has an OHCI (00:02.0, with QEMU's keyboard and tablet when its
i8042 is off) and a UHCI (00:03.0) of its own, so every IA-64 product boot on
it runs the group driver beside PS/2 and its ready line ends in `, usb`;
the test firmware's pager starts no USB driver.

## Verification

`Amd64UsbHid` and `Ia64UsbHid` cover EHCI devices, companion routing, polling
and interrupts behind translating IOMMUs, and the trusted-driver profile. VM
cases check keyboard echo, pointer motion, screen pixels and coexistence with
PS/2 and xHCI. Recovery cases kill the serving driver with interrupt polls
queued, require a quiet proof for every controller, then verify renewed input
and the three-restart bound. Evidence checks assignment quarantine, DMA faults
and Interrupt collection.

## Reserved-EHCI diagnostics

An EHCI reserved by a VT-d RMRR is skipped at publication step 5 and stays
reserved until a validated release ([driver isolation](IommuDomain.md)).
AMD64's REPORT.TXT includes an `EHCI-RMRR` block per reserved EHCI with PCI
power, command, BAR and EHCI capability, legacy-support and operational
registers, plus the RMRR range and its UEFI memory types. Recognized Intel
6-series chipsets also report SPECIAL_SMI and PCH SMI_EN; other layouts
remain unknown. These accesses are read only.

## Limits

- Root ports only; hubs, including Intel Series 6 rate-matching hubs, are not
  driven.
- EHCI functions with an RMRR (and any restricted function) are not
  published until a validated release ([driver isolation](IommuDomain.md)).
- One request per controller: shared controllers time-slice, UHCI slowly.
- Three restarts per boot; the group's first start is not retried.
- Six devices across the group, three interrupt endpoints each.


## Report-protocol mice

The shared USB core also considers HID 03/00/00 interfaces with an interrupt IN endpoint and a
bounded report descriptor. It fetches that descriptor with the standard
interface GET_DESCRIPTOR (Report 0x22), skipping SET_PROTOCOL for a
non-boot interface. A boot mouse whose SET_PROTOCOL(boot) stalls can use
its report descriptor. HidRelativeMouse accepts four report IDs, eight
fields, signed relative X/Y/wheel up to 32 bits and the first three buttons;
absolutely positioned X/Y is refused. Descriptor length is at most 1024
bytes and interrupt storage retains its existing bound. Layout publication
is atomic; truncated/unknown-ID reports do not change held buttons. The
same decoder/enumeration states run on xHCI, EHCI, OHCI and UHCI. This is
not a general HID parser or hub implementation.

Specifications: *Device Class Definition for Human Interface Devices*,
revision 1.11 (2000-06-27), §§6.2.2.4–8, 7.2.6, 8.1; *HID Usage Tables*,
revision 1.5 (2024-01-26), §§4.1/4.2 and 12. Project-authored descriptor vectors test signed 16-bit relative axes/report IDs and generic-interface
enumeration. xHCI owner storage occupies 70 pages: the first 32 contain a
17-page controller prefix and the first four device slots; the other 38
hold twelve device slots, with sizeof assertions. AMD64's component segment bound is 40 pages.
