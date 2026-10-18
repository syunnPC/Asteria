# SPDX-License-Identifier: Apache-2.0
"""The guest test program's TAP report.

The initial user image of the test firmware (Tests/Guest) checks everything
the system-call ABI shows and reports a TAP 14 stream through its
DiagnosticChannel; the kernel frames every line as `USER:<text>` on the
debug channel. A driver of a test-firmware case waits, after the user
launch, until the stream is complete (the plan and every planned point, or
a bail-out), then for the guest's idle loop, and records the stream as
checks:

- `guest.tap-stream`: the stream is valid: the version line first, one
  plan, every planned point present and numbered in order, no bail-out, and
  no line that is not TAP (the AMD64 pager's created thread writes its own
  `# thread:` line, which is not the guest's and is checked on its own);
- `guest.<n>.<id>` for test point n, where <id> is its identifier (the
  description up to the first colon or blank): passed for `ok`, failed for
  `not ok` whatever its directive, skipped with the reason for `ok # SKIP`.
  The detail is the description and the point's diagnostics.
"""
from __future__ import annotations

from AsteriaVm import Tap
from AsteriaVm.Console import USER_PREFIX
from AsteriaVm.Executor import CaseFailure, CheckResult

# The guest phase: from the user launch to the complete stream.
SECONDS = 40.0
STREAM_CHECK = 'guest.tap-stream'
# The AMD64 test firmware's spinner (Source/User/Spinner/SpinnerProgram.hpp,
# SpinnerDoneLine): one TAP comment, written once it has counted. It must
# follow the guest's complete stream, which only preemption allows, since the
# kernel queues the spinner before the guest.
SPINNER_LINE = '# spinner: done'
# The line it writes instead when its xmm registers or MXCSR changed while it
# counted (SpinnerStateLostLine, Documentation/Architecture/ThreadLifecycle.md).
SPINNER_STATE_LOST_LINE = '# spinner: extended state lost'
SPINNER_SECONDS = 120.0
SPINNER_CHECK = 'guest.spinner-after-stream'
# The thread the AMD64 test firmware's pager creates (Source/User/Pager/
# PagerProgram.hpp, PagerThreadDoneLine; Documentation/Architecture/SystemCallAbi.md): one TAP
# comment, written while the pager tests object management, which may come
# before the guest's version line, so the guest's stream leaves these lines out.
THREAD_PREFIX = '# thread:'
THREAD_LINE = '# thread: done'
THREAD_CHECK = 'guest.created-thread'
# The lines the boot pager writes as root service while it builds, crashes
# and restarts its component (Tests/Guest/Pager/RootScenarios.hpp).
COMPONENT_PREFIX = '# component:'
# `# init:` and `# usb:` are the product recovery's, which the test pager's
# USB driver recovery runs (Drivers.UsbRecovery, Drivers.Xhci).
# Watchdog.md: normal service scenarios must answer liveness Queries. Keep
# unexpected reports in the ordered audit rather than ignoring TAP comments.
SERVICE_PREFIXES = ('# directory:', '# client:', '# services:', '# loader:', '# driver:', '# block:', '# fs:', '# fs v2:',
                    '# net:', '# ports:', '# input:', '# devices:', '# init:', '# usb:', '# watchdog:')
# The root service's last line, written once it destroyed everything it
# created; its failures share the prefix.
ROOT_PREFIX = '# root service:'
ROOT_DONE_LINE = '# root service: done'
AMD64_ENTRY_STATE_PREFIX = '# amd64 entry state: '
AMD64_ENTRY_STATE_LINE = '# amd64 entry state: XSETBV supervised, terminated, reclaimed'
IA64_ENTRY_STATE_PREFIX = '# entry state fault: '
AMD64_RETURN_BOUNDARY_LINE = '# amd64 return boundary: SYSCALL at last two bytes supervised, terminated, reclaimed'
FRAME_RIGHTS_LINE = '# frame rights: Map-only copy read, writable mapping refused, reclaimed'
# Every line the root service writes, which it mirrors on the system console
# while the display broker runs; the guest's own
# lines share the debug channel but not the screen.
DISPLAY_PREFIX = '# display:'
DISPLAY_METRICS_PREFIX = '# display: scanout '
IA64_DISPLAY_LINE = '# display: 640x400 bgrx, console 80x25'
DISPLAY_STOP_LINE = '# display: stopped'
ROOT_SERVICE_PREFIXES = (COMPONENT_PREFIX, ROOT_PREFIX, DISPLAY_PREFIX, '# session-probe:', '# session:', '# session-test:', '# session-input:', '# window-event:') + SERVICE_PREFIXES
# The AMD64 incarnation that creates in its own ResourceDomain
# before the echo.
ACCOUNT_LINE = '# component: created in its own domain, refused beyond it and executable memory, drained'
# The AMD64 driver of the assignable device, after it.
DRIVER_INTX_LINE = '# driver: 10! computed, DMA confined, stray write faulted, INTx waited, drained'
DRIVER_LINE = '# driver: 10! computed, DMA confined, stray write faulted, MSIs waited, drained'
# The IA-64 driver's, on a kernel without an IOMMU: registers
# only, and its INTx line.
DRIVER_REGISTERS_LINE = '# driver: 10! computed, bus master refused without an IOMMU, INTx waited, drained'
# Its recovery: the driver service crashed with a
# transfer running and restarted, after the driver task.
DRIVER_RECOVERED_LINE = '# driver: crashed mid-transfer, restarted, DriverRestarted, served'
# The IA-64 recovery's, whose service computes factorials since its device may
# not master the bus: it crashed with a factorial running.
DRIVER_FACTORIAL_RECOVERED_LINE = '# driver: crashed mid-factorial, restarted, DriverRestarted, served'
# The AMD64 AHCI driver's demonstration, after the
# driver's recovery: with the disk, the root service asks for the throttle,
# then the driver crashes reading; without it, it serves nothing.
BLOCK_THROTTLE_LINE = "# block: throttle the disk for the crash's read"
BLOCK_SERVED_LINE = ('# block: token read, queued read Cancelled, TooLate, written and read back, crashed reading, '
                     'Frame destroyed, reset, drained')
BLOCK_NO_DISK_LINE = '# block: no disk, queued read Cancelled, served nothing, drained'
# The filesystem service's demonstration (filesystem protocol, FAT volume contract
# ), right before the block driver's, by the case's disk recipe:
# FAT volumes read and written (the fat16 one's readme asks for a commit
# crashed before its directory entry), an exFAT volume read and written
# the raw disk formatted as AsteriaFS, or no disk.
FS_FAT_LINE = '# fs: fat volume, readme.txt read, hello committed and read back'
FS_FAT_CRASHED_LINE = FS_FAT_LINE + ', crash before the entry, restarted, crash.txt not found'
FS_ASTERIA_LINE = ('# fs: asteriafs formatted, hello committed and read back, crash before the superblock '
                   'NotCommitted, crash after Committed')
FS_EXFAT_LINE = '# fs: exfat volume, readme.txt read, hello committed and read back'
FS_NO_DISK_LINE = '# fs: no disk, driver killed while it waits'
FS_LINES = {'': FS_NO_DISK_LINE, 'raw': FS_ASTERIA_LINE, 'fat12': FS_FAT_LINE, 'fat16': FS_FAT_CRASHED_LINE,
            'fat32': FS_FAT_LINE, 'exfat': FS_EXFAT_LINE, 'fat32-v2': FS_FAT_LINE, 'exfat-v2': FS_EXFAT_LINE}
# The network demonstration, between the filesystem's
# and the block driver's: resolution, echo, refusal and counted drops.
NET_LINES = ['# net: nic 52:54:00:a5:7e:01, 10.0.2.15/24',
             '# net: resolved 10.0.2.2',
             '# net: listening on udp 7',
             '# net: echoed 16 bytes to 10.0.2.2:40000',
             '# net: send to 10.0.2.3 refused',
             '# net: drops not-for-us 1, bad-icmp 1, unauthorized 1',
             '# net: stopped']
INPUT_LINE = "# input: typed 'asteria', pointer moved (5,-3), left button, secure attention reported, focus switched"
ECHO_LINE = '# component: echoed, killed while waiting and torn down'
# The services both root services run side by side and their client
SERVICE_LINES = ['# directory: bound echo', '# client: resolved echo and got 3 echoes',
                 '# directory: echo restarted and rebound',
                 '# client: stale send refused, resolved again, 3 echoes, torn down',
                 '# services: directory and echo killed while waiting and torn down']
# The AMD64 loader's demonstration, last.
LOADER_LINES = ['# loader: ia64 executable ran under its registered interpreter',
                '# loader: aarch64 executable refused, no interpreter registered',
                '# loader: pe32+ executable refused as an unsupported format']
PORTS_LINE = '# ports: i8042 status read, reset refused, keyboard line armed and masked'
PORTS_SKIPPED_LINE = '# ports: i8042 not published, demo skipped'
COMPONENT_LINES = ['# component: crashed, terminated and torn down', '# component: hung, killed and torn down',
                   '# component: reported, exited and torn down']
COMPONENT_CHECK = 'guest.component'
# The sectors the AHCI driver's Describe reports for each disk recipe
# (DiskRecipes), zero without a disk.
DESCRIBED_SECTORS = {'': 0, 'raw': 4096, 'fat12': 4096, 'fat16': 131072, 'fat32': 131072, 'exfat': 131072, 'fat32-v2': 131072, 'exfat-v2': 131072}


def storage_lines(recipe: str, network: bool = False) -> list[str]:
    """The filesystem demonstration's lines for a disk recipe (its Describe
    line first), the network demonstration's where the platform has a NIC,
    then the block driver's."""
    describe = '# fs: Describe 512-byte blocks, sectors 0x%016X' % DESCRIBED_SECTORS[recipe]
    v2 = ['# fs v2: views, confinement, slots, UTF-8 tree and eight pages passed'] if recipe.endswith('-v2') else []
    return ([describe, *v2, FS_LINES[recipe]] + (NET_LINES if network else [])
            + ([BLOCK_THROTTLE_LINE, BLOCK_SERVED_LINE] if recipe else [BLOCK_NO_DISK_LINE]))


# IA-64's after its three incarnations: the account incarnation's, then the
# driver's, which confines edu's DMA behind the virtio-iommu (or, without
# one, drives it through its registers only), then its recovery, then the
# filesystem's, the network's where the case attaches the e1000e
# and the AHCI driver's demonstrations
# then the echo service's.
IA64_LOADER_LINES = ['# loader: amd64 executable ran under its registered interpreter'] + LOADER_LINES[1:]


def ia64_component_lines(recipe: str = '', iommu: bool = True, network: bool = False) -> list[str]:
    """IA-64's root-service lines between its three incarnations' and its
    last, for a case's disk `recipe`, with or without an IOMMU, with or
    without the NIC."""
    return ([ACCOUNT_LINE, DRIVER_INTX_LINE if iommu else DRIVER_REGISTERS_LINE, DRIVER_FACTORIAL_RECOVERED_LINE]
            + storage_lines(recipe, network=network) + [ECHO_LINE] + SERVICE_LINES + IA64_LOADER_LINES
            + [INPUT_LINE, '# input: i8042 not published, PS/2 demo skipped'])


# The diskless default case's, behind the IOMMU with the e1000e attached;
# without an IOMMU (registers only) the case attaches no NIC.
IA64_COMPONENT_LINES = ia64_component_lines(network=True)


def channel_lines(lines: list[str]) -> list[str]:
    """Every line user code wrote to the debug channel, without the prefix."""
    return [line[len(USER_PREFIX):] for line in lines if line.startswith(USER_PREFIX)]


def user_lines(lines: list[str]) -> list[str]:
    """The guest's lines of a debug-channel transcript, without the prefix:
    every user line but the created thread's and the root service's."""
    return [line for line in channel_lines(lines)
            if not line.startswith(THREAD_PREFIX) and not line.startswith(COMPONENT_PREFIX)
            and not line.startswith(AMD64_ENTRY_STATE_PREFIX) and not line.startswith(SERVICE_PREFIXES) and not line.startswith("# pointer:")
            and line not in ("asteria", FRAME_RIGHTS_LINE, AMD64_RETURN_BOUNDARY_LINE, AMD64_ENTRY_STATE_LINE)]


def thread_problems(lines: list[str]) -> list[str]:
    """Why the created thread did not report exactly once that it was
    preempted with its registers intact."""
    found = [line for line in channel_lines(lines) if line.startswith(THREAD_PREFIX)]
    if found != [THREAD_LINE]:
        return ['the created thread wrote %r, expected %r once' % (found, THREAD_LINE)]
    return []


def root_service_lines(lines: list[str]) -> list[str]:
    """The channel lines the root service wrote, in order."""
    return [line for line in channel_lines(lines) if line.startswith(ROOT_SERVICE_PREFIXES) or line.startswith("# pointer:") or line == "asteria"]


IA64_REGISTERS_COMPONENT_LINES = ia64_component_lines(iommu=False)


def ps2_ready_line(lines: list[str]) -> bool:
    return '# input: PS/2 ready, QMP may inject' in lines


def component_problems(lines: list[str], echo: bool = False, extra: list[str] | None = None,
                       disk: str | bool = '', ports: bool = True, ia64: bool = False, edu: bool = True,
                       network: bool = True) -> list[str]:
    """Why the root service did not report its component's incarnations,
    crashing, hanging until its watchdog killed it, and then reporting, each
    torn down, in that order; with `echo` (AMD64), then an incarnation that
    created in its own ResourceDomain, a service that echoed a
    request and was killed while it waited, the services
    and the loader's demonstration, the AHCI driver's after the driver's
    recovery (served with a `disk`, whose recipe also decides the filesystem
    demonstration's line before it); otherwise the lines of `extra` (IA-64's,
    `ia64_component_lines`, between its display broker's first and last
    lines); and last that it is done. An AMD64 machine without `edu` runs
    neither the driver nor its recovery, one without the NIC (`network`) no
    network demonstration (the trusted-driver profile's cases)."""
    recipe = ('raw' if disk is True else '') if isinstance(disk, bool) else disk
    block = storage_lines(recipe, network=network)
    ports_line = PORTS_LINE if ports else PORTS_SKIPPED_LINE
    driver = [DRIVER_LINE, DRIVER_RECOVERED_LINE] if edu else []
    expected = COMPONENT_LINES + ([ACCOUNT_LINE] + driver + block + [ports_line, ECHO_LINE]
                                  + SERVICE_LINES + LOADER_LINES + [INPUT_LINE]
                                  + (["# input: PS/2 ready, QMP may inject", "asteria", "# input: typed 'asteria', pointer moved (5,-3), left button, PS/2, secure attention reported, second client empty"]
                                     if ports else ['# input: i8042 not published, PS/2 demo skipped']) if echo else list(extra or [])) + [ROOT_DONE_LINE]
    found = [line for line in channel_lines(lines)
             if any(line.startswith(prefix) for prefix in (COMPONENT_PREFIX, ROOT_PREFIX) + SERVICE_PREFIXES) or line == "asteria"]
    if ia64 or extra in (IA64_COMPONENT_LINES, IA64_REGISTERS_COMPONENT_LINES) or (extra and ps2_ready_line(extra)):
        expected = [IA64_DISPLAY_LINE] + expected[:-1] + [DISPLAY_STOP_LINE, ROOT_DONE_LINE]
        found = root_service_lines(lines)
    # Throughput diagnostics are optional and may refresh during product input.
    # Keep the exact mode/stopped lines in the scenario's ordered comparison.
    found = [line for line in found if not line.startswith(("# devices:", "# pointer:", DISPLAY_METRICS_PREFIX))]
    if found != expected:
        return ['the root service wrote %r, expected %r' % (found, expected)]
    return []


def identifier(description: str) -> str:
    """A test point's identifier: its description up to the first colon or blank."""
    words = description.split()
    return (words[0].rstrip(':') if words else '') or 'point'


def checks(lines: list[str]) -> list[CheckResult]:
    """The stream check and one check per test point of a transcript."""
    stream = Tap.parse(user_lines(lines))
    problems = stream.structure_problems()
    results = [CheckResult(STREAM_CHECK, not problems, '\n'.join(problems))]
    for point in stream.points:
        detail = '\n'.join([point.description] + point.diagnostics)
        skipped = (point.reason or 'no reason given') if point.skipped else ''
        results.append(CheckResult('guest.%d.%s' % (point.number, identifier(point.description)), point.passed,
                                   detail, skipped))
    return results


def record(context, lines: list[str]) -> None:
    for result in checks(lines):
        context.check(result.name, result.passed, result.detail, skipped=result.skipped)


def spinner_problems(lines: list[str]) -> list[str]:
    """Why the spinner's line is not exactly once after the guest's complete stream."""
    guest = user_lines(lines)
    found = [index for index, line in enumerate(guest) if line == SPINNER_LINE]
    if len(found) != 1:
        return ['the spinner line %r appears %d times' % (SPINNER_LINE, len(found))]
    before = [line for index, line in enumerate(guest) if index < found[0] and line != SPINNER_LINE]
    if not Tap.parse(before).complete:
        return ['the spinner line arrived after %d guest lines, before the stream was complete' % len(before)]
    return []


def wait_for_spinner(context, timeout: float = SPINNER_SECONDS, *, since: int = 0) -> int:
    """Waits until the spinner wrote its line and returns that line's index
    (`CaseFailure` otherwise, and at once when it reports its extended state
    lost)."""
    def spinner(line: str) -> bool:
        return line in (USER_PREFIX + SPINNER_LINE, USER_PREFIX + SPINNER_STATE_LOST_LINE)
    spinner.__name__ = 'the spinner line'
    index, line = context.wait_for_line(spinner, timeout, since=since)
    if line == USER_PREFIX + SPINNER_STATE_LOST_LINE:
        raise CaseFailure('the spinner reported its extended state lost: a context switch did not keep its '
                          'xmm registers or MXCSR')
    return index


def wait(context, timeout: float = SECONDS, *, since: int = 0) -> None:
    """Waits until the guest's stream is complete. A kernel failure line, QEMU
    exiting or the deadline ends the case (`CaseFailure`)."""
    console = context.console
    parsed: dict[int, bool] = {}     # Completeness by number of guest lines, parsed once per new line

    def complete(line: str) -> bool:
        if not line.startswith(USER_PREFIX):
            return False
        lines = user_lines(console.lines)
        if len(lines) not in parsed:
            parsed.clear()
            parsed[len(lines)] = Tap.parse(lines).complete
        return parsed[len(lines)]
    complete.__name__ = 'the complete guest TAP stream'
    context.wait_for_line(complete, timeout, since=since)
