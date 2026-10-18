# SPDX-License-Identifier: Apache-2.0
"""The trusted-driver profile.

On a machine without a usable IOMMU, a function BOOT.CFG trusts (AsteriaVm.Trust
writes the entries from the build's module digests) is driven without DMA
isolation; without an entry nothing changes; where an IOMMU works the entry is
unused and the function DMA-confined. Each case runs its machine's
usual flow, then judges what the profile reports:

- the loader's `ASTERIA:BOOT-TRUST:<n>... VERIFIED` line per entry, and the
  boot report's (REPORT.TXT) `DMA trust` section, which names the matching PCI
  function of each entry;
- the kernel's `ASTERIA:DMA-TRUST:IOMMU:<ABSENT|WORKING>` and, per function,
  `ASTERIA:DMA-TRUST:<bdf>:<driver>:ENTRY-<n>:<TRUSTED-NO-DMA-ISOLATION|
  UNUSED-IOMMU-CONFINES>` on the debug channel;
- the root service's `# devices:` lines (screen and channel): the function's
  `(TRUSTED: no DMA isolation)` suffix exactly where it applies;
- without entries, none of these.

The flows: the production firmware's resident console with its xHCI input
(Drivers.Production), the test firmware's xHCI HID kill and reassignment
(Drivers.Xhci), the AMD64 test firmware's storage and network scenarios
without edu (here: the AHCI driver's crash with a read in flight, whose late
write the parked, quarantined Frame takes, and the LAN script), and IA-64's
default flow with its disk and its LAN (Drivers.Ia64Default).
"""
from __future__ import annotations

import re

from AsteriaVm import Amd64Layout, DiskRecipes, Evidence, Guest, Inventory, Lan, Machine, Stages, Trust
from AsteriaVm.Drivers import Amd64Default, Ia64Default, Production, Xhci
from AsteriaVm.Observers import Amd64Cpu, Amd64Evidence, MemoryImport
from AsteriaVm.Qmp import wait_until
from AsteriaVm.Scenarios import PRODUCTION_FIRMWARE

TRUSTED_SUFFIX = '(TRUSTED: no DMA isolation)'


def run(context) -> None:
    case = context.case
    if case.isa == 'Ia64':
        Ia64Default.run(context)
    elif case.firmware == PRODUCTION_FIRMWARE:
        Production.run(context)
    elif case.xhci:
        Xhci.run(context)
    else:
        _storage(context)
    report = Inventory.report_path(context.run_directory / 'Esp')
    text = report.read_text(errors='replace') if report is not None else None
    context.check_problems('trust.reported', problems(case, context.console.lines, text))


def _bdf(address: tuple[int, int, int, int]) -> str:
    return '%04x:%02x:%02x.%x' % address


def problems(case, lines: list[str], report: str | None) -> list[str]:
    """Why the debug channel's lines, the root service's inventory and the
    boot report (None where the firmware's volume is read-only) do not show
    the case's trust entries as they must apply."""
    found = []
    channel = Guest.channel_lines(lines)
    devices = [line for line in channel if line.startswith('# devices:')]
    if not case.trust:
        if any('DMA-TRUST' in line or 'BOOT-TRUST' in line for line in lines):
            found.append('a boot without trust entries reported a trust decision')
        if any('TRUSTED' in line for line in devices):
            found.append('a boot without trust entries reported a trusted function')
        if report is not None and 'DMA trust' in report:
            found.append('REPORT.TXT has a DMA trust section without entries')
        return found
    working = case.iommu != 'None'
    wanted = 'ASTERIA:DMA-TRUST:IOMMU:%s' % ('WORKING' if working else 'ABSENT')
    if wanted not in lines:
        found.append('no %r line' % wanted)
    listed = Trust.entries(case.isa, case.trust)
    if report is not None and ('DMA trust: %d BOOT.CFG entries' % len(listed)) not in report:
        found.append('REPORT.TXT has no DMA trust section for %d entries' % len(listed))
    for number, (driver, (address, identifier)) in enumerate(listed, start=1):
        bdf = _bdf(address) if address is not None else None
        loader = [line for line in lines if line.startswith('ASTERIA:BOOT-TRUST:%d ' % number)]
        if len(loader) != 1 or not loader[0].endswith(' VERIFIED') or ' driver=%s ' % driver not in loader[0]:
            found.append('entry %d: the loader reported %r, expected one VERIFIED %s line' % (number, loader, driver))
        decision = 'UNUSED-IOMMU-CONFINES' if working else 'TRUSTED-NO-DMA-ISOLATION'
        kernel = [line for line in lines if re.fullmatch(
            r'ASTERIA:DMA-TRUST:[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]:%s:ENTRY-%d:%s' % (driver, number, decision),
            line)]
        if len(kernel) != 1 or (bdf is not None and bdf not in kernel[0]):
            found.append('entry %d: the kernel reported %r, expected one %s decision for %s' % (
                number, kernel, decision, bdf or driver))
            continue
        bdf = kernel[0].split(':', 2)[2][:12]
        function = [line for line in devices if line.startswith('# devices: %s ' % bdf)]
        if len(function) != 1 or ((TRUSTED_SUFFIX in function[0]) == working):
            found.append('entry %d: the inventory shows %r, expected %s' % (
                number, function, 'no trusted suffix' if working else TRUSTED_SUFFIX))
        if report is not None and not any(line.startswith('ASTERIA:BOOT-TRUST:%d ' % number)
                                          and line.endswith('VERIFIED matches %s' % bdf)
                                          for line in report.splitlines()):
            found.append('entry %d: REPORT.TXT does not name %s as its function' % (number, bdf))
    if any('TRUSTED' in line for line in devices) and working:
        found.append('a function is reported trusted although an IOMMU works')
    return found


# The AMD64 test firmware's scenarios, without edu ---------------------------------------------

def _storage(context) -> None:
    """The guest's TAP stream and the spinner as Amd64Default, then the root
    service's scenarios without edu: the filesystem, network and block
    demonstrations on the trusted AHCI driver and NIC; then the drained AHCI
    driver's late write in its parked Frame and the LAN's frames."""
    peer = (Lan.LanPeer(context.lan, context.artifact(Lan.PCAP_ARTIFACT).parent).start()
            if getattr(context, 'lan', None) is not None else None)
    try:
        launch, _ = context.wait_for_line(Stages.predicate(Amd64Default.LAUNCH_STAGE), Amd64Default.BOOT_SECONDS)
        Guest.wait(context, since=launch + 1)
        spinner = Guest.wait_for_spinner(context, since=launch + 1)
        context.wait_for_line(Amd64Default.TERMINATED_EVENT, Guest.SPINNER_SECONDS, since=spinner + 1)
        throttled = []
        case = context.case

        def component() -> list[str]:
            return Guest.component_problems(context.console.lines, echo=True, disk=case.disk, ports=case.i8042,
                                            edu='edu' in case.assignable, network='e1000e' in case.assignable)

        def throttle_once() -> None:
            channel = Guest.channel_lines(context.console.lines)
            if peer is not None and Lan.LISTENING_LINE in channel:
                peer.start_script()
            if not throttled and Guest.BLOCK_THROTTLE_LINE in channel:
                context.qmp.execute('block_set_io_throttle', Amd64Default.DISK_THROTTLE)
                throttled.append(True)

        wait_until(lambda: throttle_once() or context.exited() or bool(context.console.failures())
                   or not component(), context.seconds(Amd64Default.COMPONENT_SECONDS), 0.1)
        # The guest's idle loop follows its last line.
        loop = Amd64Cpu.idle_loop(context.target.tool('objdump'), context.image('user_elf'),
                                  Amd64Layout.USER_IDLE_SYMBOL)
        reached, detail = Amd64Cpu.wait_for_idle(
            context.qmp, context.artifact('CpuPoll.elf'), loop, context.seconds(Amd64Default.IDLE_SECONDS),
            abort=lambda: context.exited() or next(iter(context.console.failures()), None))
        context.check('lifecycle.guest-complete', reached, detail)
        context.stop()
        if peer is not None:
            peer.stop()
        Guest.record(context, context.console.lines)
        context.check_problems(Guest.SPINNER_CHECK, Guest.spinner_problems(context.console.lines))
        context.check_problems(Guest.COMPONENT_CHECK, component())
        if 'e1000e' in case.assignable:
            for name, found in Lan.judge_artifacts(context.artifact(Lan.PCAP_ARTIFACT).parent,
                                                    context.console.lines).items():
                context.check_problems(name, found)
        path = context.artifact(Evidence.ARTIFACT)
        problem = Evidence.save(context.qmp, context.console.lines, path)
        context.require('trust.evidence', problem is None, problem or '')
        record = Amd64Evidence.schema(context.target.root).decode(path.read_bytes())
        context.check_problems('trust.no-iommu', [] if (record.Iommu.Enabled, record.IommuBackend.Kind) == (0, 0)
                               else ['an IOMMU translated (Enabled %d, backend %d)' % (
                                   record.Iommu.Enabled, record.IommuBackend.Kind)])
        context.check_problems("trust.memory-import", MemoryImport.problems(record,
            context.qmp.pmemsave(record.MemoryImport.MapPhysical, record.MemoryImport.MapRegions * MemoryImport.MAP_RECORD.size)))
        dma = [run for run in record.Block.Released if run.Pages]
        context.check("trust.dma32", bool(dma) and all(0 < run.PhysicalStart
                      and run.PhysicalStart + run.Pages * MemoryImport.PAGE <= MemoryImport.LOW_END for run in dma),
                      "parked private DMA Frames must end below 4 GiB")
        control = record.DeviceControl
        quarantine = []
        if not (control.Quarantines == control.Confirmations >= 1 and control.PinsParked == control.PinsReleased >= 1
                and control.ParkedPins == 0):
            quarantine.append('quarantines %d, confirmations %d, pins parked %d, released %d, held %d' % (
                control.Quarantines, control.Confirmations, control.PinsParked, control.PinsReleased,
                control.ParkedPins))
        quarantine += Amd64Evidence.block_problems(record, Amd64Evidence.BlockScenario(
            token=Machine.DISK_TOKEN if case.disk else None))
        context.check_problems('trust.quarantine', quarantine)
        Amd64Default.check_disk_images(context, [DiskRecipes.hello_page(3), DiskRecipes.hello_page(1)])
    finally:
        if peer is not None:
            peer.stop()
