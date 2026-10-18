# SPDX-License-Identifier: Apache-2.0
"""Recorded IA-64 runs (Fixtures/Ia64/<case>) and the helpers the IA-64 tests share.

Each fixture directory is the evaluation input of one passing run of the VM
runner: the debug channel (Debug.log, with the kernel's stage events, its
evidence fact line and, for the test firmware, the guest's TAP stream), the
kernel's evidence record (Evidence.bin), the Smp4 console screendump (Screen.ppm), CPU 0's `info registers` text after
the stop (Registers.txt), the other memory dumps (*.bin), QEMU's CPU and PCI
view (Cpus.json, Pci.json), the loop wait's outcome (LoopWait.json), the LAN
peer's capture where the case attaches the e1000e (Lan.pcap, Lan.json) and the
expectation it was judged against (Expectation.json, with the ELF facts and
C++ constants of the build it came from), so the tests need neither QEMU nor
a build tree. Evidence.bin is decoded with the layout of the current headers: a
record of another schema fails its header check, and the fixtures are then
recorded again. Smp1-3 keep only their Debug.log (the test firmware's TAP
stream and stage events); NativePreemption keeps no IVT and no loop wait,
which its driver does not judge. Every full fixture keeps the register dumps
needed to check the consolidated translation plan on each CPU.

To record again from a passing run of every IA-64 case:
  PYTHONPATH=Tools/Vm python3 -m Tests.test_ia64_fixtures --record Build/Vm/<run id>
"""
from __future__ import annotations

import copy
import dataclasses
from Tests import recorded_schema
import json
import pathlib
import shutil
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

from AsteriaVm import Build, Lan, Scenarios
from AsteriaVm.Drivers import Ia64Default
from AsteriaVm.Observers import Ia64Capture, Ia64Evidence, Ia64Image, Ia64State

ROOT = pathlib.Path(__file__).resolve().parents[3]
FIXTURES = pathlib.Path(__file__).resolve().parent / 'Fixtures' / 'Ia64'
FULL_CASES = ('Smp4', 'Ahci', 'Relocated', 'SyntheticProbe', 'NativePreemption')
ALL_CASES = ('Smp1', 'Smp2', 'Smp3', 'Smp4', 'Ahci', 'Relocated', 'SyntheticProbe', 'NativePreemption')
IVT_FILE = Ia64Default.IVT_DUMP + '.bin'


def directory(name: str) -> pathlib.Path:
    return FIXTURES / name


def case(name: str) -> Scenarios.Case:
    return next(case for case in Scenarios.CASES if case.name == 'Ia64.' + name)


def observation(name: str) -> Ia64Capture.Observation:
    return Ia64Capture.load(directory(name))


def lines(name: str) -> list[str]:
    return observation(name).lines


def registers(name: str) -> str:
    return (directory(name) / 'Registers.txt').read_text()


def state(name: str) -> Ia64State.CpuState:
    return Ia64State.parse(registers(name))


def evidence_bytes(name: str) -> bytes:
    return (directory(name) / (Ia64Evidence.DUMP + '.bin')).read_bytes()


def expectation_json(name: str) -> dict:
    return json.loads((directory(name) / 'Expectation.json').read_text())


def expectation(name: str, data: dict | None = None) -> Ia64Default.Expectation:
    table = directory(name) / IVT_FILE
    ivt = table.read_bytes() if table.exists() else None
    expected = Ia64Default.Expectation.from_json(copy.deepcopy(data if data is not None else expectation_json(name)), ivt, ROOT)
    return dataclasses.replace(expected, schema=fixture_schema())


def fixture_schema():
    return recorded_schema.schema(FIXTURES, Ia64Evidence)


def failing(checks: list) -> dict[str, list[str]]:
    """Name -> problems of every failing check."""
    return {check[0]: check[1] for check in checks if check[1]}


class FixtureTests(unittest.TestCase):
    def test_every_ia64_case_has_a_recording(self):
        for name in ALL_CASES:
            self.assertTrue((directory(name) / 'Debug.log').is_file(), name)
            self.assertTrue(lines(name), name)

    def test_recorded_expectations_match_the_scenario_table(self):
        for name in FULL_CASES:
            recorded, scenario = expectation(name), case(name)
            self.assertEqual((recorded.cpus, recorded.disk is not None, recorded.relocated, recorded.recipe),
                             (scenario.cpus, scenario.has_disk, scenario.relocated, scenario.disk), name)

    def test_network_recordings_carry_their_capture(self):
        for name in FULL_CASES:
            self.assertEqual(observation(name).lan is not None, expectation(name).network, name)

    def test_recorded_evidence_matches_its_captured_headers(self):
        schema = fixture_schema()
        for name in FULL_CASES:
            self.assertEqual(schema.header_problems(evidence_bytes(name)), [], name)

    def test_console_capture_and_a_changed_pixel(self):
        # Exercise the driver's QMP capture and verdict: dropping its comparison
        # or feeding it a different transcript cannot pass this replay.
        ppm = (directory('Smp4') / 'Screen.ppm').read_bytes()
        for changed in (False, True):
            pixels = ppm[:-1] + bytes([ppm[-1] ^ 1]) if changed else ppm
            with tempfile.TemporaryDirectory() as scratch:
                path = pathlib.Path(scratch) / 'Screen.ppm'
                context = SimpleNamespace(
                    artifact=lambda name: path,
                    target=SimpleNamespace(root=ROOT),
                    console=SimpleNamespace(lines=lines('Smp4')),
                    qmp=SimpleNamespace(screendump=lambda target, **options: target.write_bytes(pixels)),
                    check=mock.Mock(), check_problems=mock.Mock())
                Ia64Default._check_display(context)
                context.check.assert_not_called()
                name, problems = context.check_problems.call_args.args
                self.assertEqual(name, 'host.ia64.display')
                self.assertEqual(bool(problems), changed)

    def test_the_recorded_ivt_is_the_linked_table(self):
        # Expectation.from_json verifies the dump against the linked table's hash.
        data = expectation_json('Smp4')
        with self.assertRaises(Exception):
            Ia64Default.Expectation.from_json(data, bytes(0x8000), ROOT)


def record(run: pathlib.Path) -> None:
    """Copies the evaluation input of every IA-64 case of `run` (a
    Build/Vm/<run id> directory) into FIXTURES, with the expectation it was
    judged against (read from the case's preset tree)."""
    recorded_schema.record(FIXTURES, Ia64Evidence, ROOT)
    for name in ALL_CASES:
        source, target = run / ('Ia64.' + name), directory(name)
        if target.exists():
            shutil.rmtree(target)
        target.mkdir(parents=True)
        shutil.copyfile(source / Ia64Capture.DEBUG_LOG, target / Ia64Capture.DEBUG_LOG)
        if name not in FULL_CASES:
            continue
        kept = [source / Ia64Capture.PCI, source / Ia64Capture.CPUS, *sorted(source.glob('*.bin'))]
        kept += [path for path in (source / Lan.PCAP_ARTIFACT, source / Lan.JSON_ARTIFACT) if path.exists()]
        if name == 'Smp4':
            kept += [source / 'Screen.ppm']
        kept += [source / Ia64Capture.REGISTERS, *sorted(source.glob('Registers.Cpu*.txt'))]
        if name != 'NativePreemption':
            kept += [source / Ia64Capture.LOOP_WAIT]
        for path in kept:
            shutil.copyfile(path, target / path.name)
        tree = Build.TargetContext.load(Build.tree(ROOT, case(name).preset))
        facts = Ia64Image.ImageFacts.from_target(tree, case(name).firmware)
        live = source / IVT_FILE
        if live.exists() and live.read_bytes() != facts.ivt_image:
            raise SystemExit('%s: the live IVT differs from the linked one; not a passing run' % name)
        expected = Ia64Default.expectation(case(name), tree, facts, source / 'Disk.raw')
        (target / 'Expectation.json').write_text(json.dumps(expected.to_json(), indent=1, sort_keys=True) + '\n')


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--record':
        record(pathlib.Path(sys.argv[2]).resolve())
    else:
        unittest.main()
