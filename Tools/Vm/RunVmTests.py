#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Boot the firmware under QEMU and check each VM case.

Usage:
  RunVmTests.py [--suite All|Ia64|Ia64Ahci|Ia64I2000|Ia64SyntheticProbe|Ia64NativePreemption|Ia64Vtd|Ia64Trusted
                 |Amd64|Amd64NativePreemption|Amd64Stops|Amd64StopEarly|Amd64X2Apic|Amd64NoPit|Amd64Trusted
                 |Amd64UsbHid|Ia64UsbHid]...
                [--case NAME-OR-GLOB]... [--jobs N] [--no-build] [--no-static-checks]
                [--timeout-scale X] [--junit FILE] [--json FILE] [--list]

Every case boots one firmware of one CMake preset tree (built first unless
--no-build) in its own run directory under Build/Vm/<run id>/<case>: the
test firmware, whose guest test program reports TAP, or for the smoke and
diagnostic-probe cases the production firmware. QEMU,
firmware and BIOS locations come from IA64_QEMU, IA64_FIRMWARE,
IA64_BIOS_DIRECTORY, AMD64_QEMU, AMD64_FIRMWARE_CODE and
AMD64_FIRMWARE_VARIABLES; IA-64 cases that name a machine (the Ia64Vtd
suite's `itanium2-vpc-iommu`) use the fork's VT-d branch build from
IA64_VTD_QEMU, IA64_VTD_FIRMWARE and IA64_VTD_BIOS_DIRECTORY instead. The
QEMU executables default to PATH lookup; IA-64 firmware must be supplied.
An omitted BIOS directory uses QEMU's installed search path. AMD64 firmware
uses an installed 4 MiB OVMF code/variables pair when available, or the
explicit overrides. The exit status is 0 only if every selected case passed.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import datetime
import os
import pathlib
import sys
import threading

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from AsteriaVm import BootEntropy, Build, Machine, Report, Scenarios  # Noqa: E402
from AsteriaVm.Executor import run_case  # Noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[2]


class WeightedSlots:
    """A semaphore that admits cases up to a total weight."""

    def __init__(self, capacity: int):
        self._capacity = capacity
        self._used = 0
        self._condition = threading.Condition()

    def acquire(self, weight: int) -> None:
        weight = min(weight, self._capacity)
        with self._condition:
            self._condition.wait_for(lambda: self._used + weight <= self._capacity)
            self._used += weight

    def release(self, weight: int) -> None:
        with self._condition:
            self._used -= min(weight, self._capacity)
            self._condition.notify_all()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--suite', action='append', default=[])
    parser.add_argument('--case', action='append', default=[])
    parser.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) // 2))
    parser.add_argument('--no-build', action='store_true')
    parser.add_argument('--no-static-checks', action='store_true')
    parser.add_argument('--timeout-scale', type=float, default=1.0)
    parser.add_argument('--junit', type=pathlib.Path)
    parser.add_argument('--json', type=pathlib.Path)
    parser.add_argument('--list', action='store_true')
    arguments = parser.parse_args()

    try:
        cases = Scenarios.select(arguments.suite or ([] if arguments.case else ['All']), arguments.case)
    except ValueError as error:
        print('error: %s' % error, file=sys.stderr)
        return 2
    if arguments.list:
        for case in cases:
            print(Scenarios.describe(case))
        return 0
    if not cases:
        print('error: no cases selected', file=sys.stderr)
        return 2

    environment = Machine.Environment.from_environment()
    try:
        for isa, machine in sorted({(case.isa, case.machine) for case in cases}):
            environment.validate(isa, machine)
    except ValueError as error:
        print('error: %s' % error, file=sys.stderr)
        return 2

    run_id = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    run_root = ROOT / 'Build' / 'Vm' / run_id
    presets = sorted({case.preset for case in cases})
    try:
        targets = Build.prepare(ROOT, presets, build=not arguments.no_build,
                                static_checks=not arguments.no_static_checks, log=run_root / 'Build.log')
    except Build.BuildError as error:
        print('error: %s' % error, file=sys.stderr)
        return 2

    slots = WeightedSlots(max(1, arguments.jobs))
    print_lock = threading.Lock()

    def execute(case: Scenarios.Case):
        slots.acquire(case.weight)
        try:
            result = run_case(case, targets[case.preset], run_root / case.name, environment,
                              arguments.timeout_scale)
        finally:
            slots.release(case.weight)
        with print_lock:
            print(Report.console_line(result), flush=True)
        return result

    with concurrent.futures.ThreadPoolExecutor(max_workers=len(cases)) as pool:
        results = list(pool.map(execute, cases))
    # Boots of one image must not repeat a bootstrap authenticator (the
    # validation keys come from boot entropy); this adds a check to each, and
    # a case it fails is reported again with its new status.
    passed_before = [result.passed for result in results]
    for line in BootEntropy.cross_boot(results):
        print(line, flush=True)
    for passed, result in zip(passed_before, results):
        if passed and not result.passed:
            print(Report.console_line(result), flush=True)

    failed = [result for result in results if not result.passed]
    for result in failed:
        print(Report.failure_details(result), file=sys.stderr)
    if arguments.junit:
        Report.write_junit(results, arguments.junit)
    if arguments.json:
        Report.write_json(results, arguments.json)
    print('%d of %d cases passed; artifacts in %s' % (len(results) - len(failed), len(results), run_root))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
