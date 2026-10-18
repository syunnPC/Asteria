<!-- SPDX-License-Identifier: Apache-2.0 -->

# USB library verification

Tests run production controller/core APIs against device-address schedule models.
The models observe registers, DMA descriptors and USB wire traffic without
inspecting controller objects. They are bounded schedule models, not electrical
or firmware timing simulators. Fixtures are project-authored protocol examples.

## Controllers and class traffic

| Suites | Main coverage |
| --- | --- |
| `OhciTests`, `UhciTests`, `Usb1ScheduleSuite` | FS/LS enumeration, HID, control stages/short IN, toggles, intervals, disconnect, deadlines and retirement |
| `Usb1StatusTests` | OHCI condition codes and UHCI active/completed error combinations |
| `XhciTests`, `XhciCompatibilityTests` | Four speeds, 0.96/1.0–1.2, context sizes, DMA widths, scratchpads, ring wrap, handoff and capability validation |
| `XhciPipeTests` | Composite keyboard/mouse enumeration, independent reports, concurrent 64 KiB bulk, per-pipe completion and cancellation |
| `XhciHardwareTests` | xHCI 0.96 with 32-byte contexts and no scratchpads, firmware-enabled ports, undefined speed before reset, EP0 stalls and retryable babble |
| `BotTests`, `ScsiTests`, `MassStorageTests` | BOT/SCSI transport, medium state, capacity/range, recovery, cancellation and buffer ownership |

OHCI/UHCI models cover HCCA/frame traversal, descriptor links, NAKs, short
control transfers, done queues and frame wrap. Seeded runs compare completion
tokens, lengths, status, resources and wire toggles. Explicit intervals include
1, 2, 10, 32, 33, 128 and 255. Controller-specific checks cover firmware release,
configuration denial, DMA overlap/range, power/reset timing and root W1C status.

Deadline/abort cases retain idle interrupt IN through long simulated NAKs,
then deliver reports. Finite control timeout retires only that request and
permits subsequent enumeration. Races cover xHCI Stop/transfer event ordering,
late Success and stalls; EHCI IAA/periodic retirement and TT cleanup; OHCI late
done queues; UHCI asynchronous packets and toggle preservation. Faulted
controllers retain borrows until successful quiescence.

`XhciHardwareTests` uses 32-byte contexts, no scratchpads and a synthetic
full-speed composite receiver with boot mouse, boot keyboard and vendor HID.
It checks CCS-only connection, attach debounce, PR/PRC reset evidence, reserved
TRT on 0.96, preserved EP0 context and interface-specific stall handling.

`XhciProgressTests` compares diagnostic queue budgets/loss with a seeded model.
`UsbProgressTests` checks diagnostic frames and printable line formats;
`InputTerminalTests` checks partial-line redraw around diagnostics. Root-service
readiness filtering requires the `Amd64XhciHid` VM cases.

Use `ASTERIA_TEST_SEED` to replay model runs; failures print seed, step and
operation. Fuzz harnesses under `Tests/Fuzz` cover descriptor/capability/event
and status decoders, diagnostic frames, CSW, sense and SCSI data parsers.
They check bounds, rejection atomicity and canonical round trips.
See the [controller](../../../Source/User/Usb/Readme.md) and
[mass-storage](../../../Source/User/Usb/MassStorageReadme.md) READMEs for contracts.

## Mutation replay

After a host-debug build, run the required mutation suite:

```sh
export TMPDIR="$PWD/Build/Tmp"
python3 Tests/User/Usb/HandMutations.py
PYTHONDONTWRITEBYTECODE=1 python3 Tests/User/Usb/AbortHandMutations.py
PYTHONDONTWRITEBYTECODE=1 python3 Tests/User/Usb/XhciHandMutations.py
PYTHONDONTWRITEBYTECODE=1 python3 Tests/User/Usb/XhciHardwareMutations.py [name...]
```

Runners modify production sources, build each mutant and require selected
CTest failures. They restore originals and rebuild/retest the baseline.
Do not run mutation suites concurrently with other source edits or builds.

| Runner | Mutated contracts | Output directory under `Build/Tmp` |
| --- | --- | --- |
| `HandMutations.py` | Ownership, control/status, intervals, quiescence and root acknowledgment | `Usb1Mutations` |
| `AbortHandMutations.py` | Idle deadlines, Stop/dequeue, event/IAA retirement, TT cleanup and USB1 cancellation | `UsbAbortMutations` |
| `XhciHandMutations.py` | Endpoint ownership/binding, legacy handoff, context/capability interpretation and pipe routing | `XhciPipeMutations` |
| `XhciHardwareMutations.py` | Connection/reset evidence, 0.96 control rules, stall recovery and diagnostics | `XhciHardwareMutations` |
