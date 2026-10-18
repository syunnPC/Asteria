# SPDX-License-Identifier: Apache-2.0
"""Runs one VM case: start, drive, observe, tear down.

The lifecycle is fixed here; what a case waits for and checks is up to its
driver (`AsteriaVm.Drivers.<name>.run(context)`), after which the boot's
entropy facts are checked (`AsteriaVm.BootEntropy.record`). The case boots
the ESP of its firmware (`Case.firmware`: the test firmware with the guest
test program, or the production firmware). A driver records checks through
`CaseContext.check`; any exception ends the case as failed, with the
artifacts collected so far.
"""
from __future__ import annotations

import dataclasses
import importlib
import pathlib
import subprocess
import time
import traceback

from AsteriaVm import Evidence, Machine
from AsteriaVm.Console import Console, ConsoleTimeout
from AsteriaVm.Qmp import Qmp, QmpError
from AsteriaVm.Scenarios import Case


class CaseFailure(Exception):
    """Ends a case: a precondition failed and later checks cannot run."""


@dataclasses.dataclass
class CheckResult:
    name: str
    passed: bool
    detail: str = ''
    skipped: str = ''    # the reason, for a check that did not run (a TAP `# SKIP` point); it counts as passed


@dataclasses.dataclass
class CaseResult:
    case: Case
    run_directory: pathlib.Path
    checks: list[CheckResult] = dataclasses.field(default_factory=list)
    error: str | None = None
    seconds: float = 0.0
    # The bootstrap handles the guest reported (AsteriaVm.BootEntropy), which
    # the runner compares across boots of the same image.
    handles: dict[str, tuple[int, int]] = dataclasses.field(default_factory=dict)

    @property
    def passed(self) -> bool:
        return self.error is None and bool(self.checks) and all(check.passed for check in self.checks)


class CaseContext:
    """What a driver works with."""

    def __init__(self, case: Case, target, run_directory: pathlib.Path, qmp: Qmp, console: Console,
                 process: subprocess.Popen, timeout_scale: float, result: CaseResult):
        self.case = case
        self.target = target            # AsteriaVerify.Context.TargetContext of the preset tree
        self.run_directory = run_directory
        self.qmp = qmp
        self.console = console
        self.process = process
        self.timeout_scale = timeout_scale
        self.result = result
        self.lan = None                 # the runner's end of the NIC's datagram link, if the case has one

    # Checks -------------------------------------------------------------------

    def check(self, name: str, passed: bool, detail: str = '', *, skipped: str = '') -> bool:
        self.result.checks.append(CheckResult(name, bool(passed), detail, skipped))
        return bool(passed)

    def check_problems(self, name: str, problems: list[str]) -> bool:
        """Records one check that passes when `problems` is empty."""
        return self.check(name, not problems, '\n'.join(problems))

    def require(self, name: str, passed: bool, detail: str = '') -> None:
        """A check whose failure ends the case."""
        if not self.check(name, passed, detail):
            raise CaseFailure('%s: %s' % (name, detail))

    # Waiting ------------------------------------------------------------------

    def seconds(self, value: float) -> float:
        return value * self.timeout_scale

    def exited(self) -> str | None:
        code = self.process.poll()
        return None if code is None else 'QEMU exited with status %d' % code

    def wait_for_line(self, predicate, timeout: float, *, since: int = 0) -> tuple[int, str]:
        try:
            return self.console.wait_for(predicate, self.seconds(timeout), since=since, abort=self.exited)
        except ConsoleTimeout as error:
            raise CaseFailure('waiting for %s: %s' % (getattr(predicate, '__name__', predicate), error)) from error

    def stop(self) -> None:
        """Pauses the guest for observation."""
        self.qmp.stop(timeout=self.seconds(10))

    def artifact(self, name: str) -> pathlib.Path:
        return self.run_directory / name

    def image(self, name: str) -> pathlib.Path:
        """An artifact of the firmware the case boots, e.g. `user_elf` or `kernel_elf`."""
        return self.target.artifact('%s:%s' % (self.case.firmware, name))


def _driver(name: str):
    return importlib.import_module('AsteriaVm.Drivers.' + name)


def unexpected_watchdog_reports(case: Case, lines: list[str]) -> list[str]:
    """Explicit fault probes assert their own misses; healthy boots allow none."""
    allowed = case.driver == 'ComponentWatchdog' or (
        case.driver == 'WholeMachineStall' and bool(case.whole_machine_pause))
    return [line for line in lines
            if line.startswith('USER:# startup:')
            or (line.startswith('USER:# watchdog:') and not allowed
                and not (case.session_compositor_fault
                         and line.startswith('USER:# watchdog: session-compositor ')))]


def run_case(case: Case, target, run_directory: pathlib.Path, environment: Machine.Environment,
             timeout_scale: float = 1.0) -> CaseResult:
    result = CaseResult(case, run_directory)
    started = time.monotonic()
    esp = target.artifact('%s:esp' % case.firmware)
    launch = Machine.prepare(case.isa, esp, run_directory, case.cpus, case.disk, environment,
                             device=case.guest or bool(case.power), i8042=case.i8042, x2apic=case.x2apic, pit=case.pit,
                             iommu=case.iommu, aml=case.aml, xhci=case.xhci, usb_boot=case.usb_boot,
                             xhci_failure=case.xhci_failure, edid=case.edid, display_policy=case.display_policy,
                             graphics_device=case.graphics_device, fadt8042=case.fadt8042,
                             floating_keyboard=case.floating_keyboard, product_media=case.product_media,
                             xhci_large=case.xhci_large, machine=case.machine, machine_options=case.machine_options,
                             reboot=case.power == 'reboot',
                             trust=case.trust, assignable=case.assignable, usb_host=case.usb_host,
                             usb_layout=case.usb_layout, synthetic_rmrr=case.synthetic_rmrr, session_probe=case.session_probe,
                             session_compositor=case.session_compositor, session_input=case.session_input,
                             session_large=case.session_large, session_stress=case.session_stress,
                             memory=case.memory, memory_rmrr=case.memory_rmrr, fault_channel=case.fault_channel,
                             ui_font_absent=case.ui_font_absent, product_desktop=case.product_desktop, tcg_thread=case.tcg_thread, toolkit_session=case.toolkit_session)
    if case.session_face_withheld or case.session_compositor_fault:
        from AsteriaVerify import BootFiles
        import re
        broker = target.artifact('firmware-test:compositor_fault_elf' if case.session_compositor_fault else 'firmware-test:display_console_elf')
        name = 'COMPOSIT.ELF' if case.session_compositor_fault else 'DISPLAY.ELF'
        module = 'compositor' if case.session_compositor_fault else 'display'
        installed = run_directory / 'Esp/EFI/ASTERIA/MODULES' / name
        installed.write_bytes(broker.read_bytes())
        manifest = run_directory / 'Esp/EFI/ASTERIA/BOOT.CFG'
        manifest.write_text(re.sub(r'(module = '+module+' '+re.escape(name)+r' crc32c=)0x[0-9a-fA-F]{8}',
            lambda match: match[1] + '0x%08x' % BootFiles.crc32c(installed.read_bytes()), manifest.read_text()))
    process = Machine.spawn(launch, run_directory)
    qmp = Qmp(launch.qmp, scratch=run_directory)
    console = Console(launch.debug, run_directory / 'Debug.log')
    context = CaseContext(case, target, run_directory, qmp, console, process, timeout_scale, result)
    context.lan = launch.lan
    try:
        qmp.handshake(timeout=context.seconds(10))
        qmp.cont()
        _driver(case.driver).run(context)
        if not case.power:
            importlib.import_module('AsteriaVm.BootFiles').record(context)
            importlib.import_module('AsteriaVm.BootEntropy').record(context)
        if case.usb_boot or case.product_media:
            # Firmware writes belong to the raw ESP, rather than vvfat's
            # exported directory. Stop all guest writes before reading them.
            context.stop()
            extracted = subprocess.run(['mcopy', '-s', '-o', '-i',
                str(run_directory / ('ProductMedia/Asteria-product-mbr.img' if case.product_media
                                      else 'BootMedia/Asteria-mbr.img')) + '@@1048576',
                '::/EFI/ASTERIA', str(run_directory / 'Esp/EFI')],
                capture_output=True, timeout=30)
            context.check('usb.esp-extract', extracted.returncode == 0,
                          extracted.stderr.decode(errors='replace'))
        if not case.power:
            importlib.import_module('AsteriaVm.I8042Evidence').record(context)
            importlib.import_module('AsteriaVm.Inventory').record(context)
            importlib.import_module('AsteriaVm.AmlFacts').record(context)
        if console.corrupt:
            context.check('lifecycle.debug-channel', False,
                          'corrupted debug lines: %s' % ', '.join(repr(line) for line in console.corrupt[:3]))
        failures = console.failures()
        if case.expected_stop:
            importlib.import_module('AsteriaVm.Stops').lifecycle(context)
        else:
            failures += [line for line in console.lines if line.startswith('ASTERIA:STOP:')]
            context.check('lifecycle.no-kernel-failure', not failures, '\n'.join(failures[:10]))
        context.check_problems('lifecycle.no-unexpected-watchdog-report',
                               unexpected_watchdog_reports(case, console.lines))
    except CaseFailure as error:
        result.error = str(error)
    except (QmpError, OSError, ValueError) as error:
        result.error = '%s: %s' % (type(error).__name__, error)
    except Exception:  # a driver bug must not hide behind a generic pass
        result.error = traceback.format_exc()
    finally:
        _teardown(context)
        result.seconds = time.monotonic() - started
    return result


def _teardown(context: CaseContext) -> None:
    qmp, process = context.qmp, context.process
    if process.poll() is None and not qmp.closed:
        try:
            qmp.stop(timeout=5)
            qmp.screendump(context.artifact('Screenshot.png'))
        except QmpError:
            pass
        # A case that ended early keeps the kernel's evidence record as it
        # stood (a passing driver has already saved it).
        if not context.result.passed and not context.artifact(Evidence.ARTIFACT).exists():
            Evidence.save(qmp, context.console.lines, context.artifact(Evidence.ARTIFACT))
        (context.run_directory / 'Events.txt').write_text(
            ''.join('%s\n' % event for event in qmp.events()))
        qmp.quit()
    try:
        process.wait(timeout=context.seconds(15))
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
    qmp.close()
    context.console.close()
    if context.lan is not None:
        context.lan.close()
