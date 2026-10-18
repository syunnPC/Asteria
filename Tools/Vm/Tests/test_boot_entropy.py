# SPDX-License-Identifier: Apache-2.0
"""Boot entropy facts and the cross-boot authenticator check (AsteriaVm.BootEntropy)."""
from __future__ import annotations

import pathlib
import tempfile
import types
import unittest

from AsteriaVm import BootEntropy, Machine, Scenarios
from AsteriaVm.Executor import CaseResult, CheckResult

ROOT = pathlib.Path(__file__).resolve().parents[3]
BITS = BootEntropy.source_bits(ROOT)
AMD64_SOURCES = 'ASTERIA:BOOT-ENTROPY:SOURCES:0x000000000000000D'   # firmware RNG, jitter, firmware state
IA64_SOURCES = 'ASTERIA:BOOT-ENTROPY:SOURCES:0x000000000000000C'    # jitter, firmware state


def handle_line(name: str, selector: int, authenticator: int) -> str:
    return 'USER:# handle.%s: selector 0x%016x authenticator 0x%016x' % (name, selector, authenticator)


def boot(name: str, authenticators: tuple[int, int], selectors: tuple[int, int] = (0x101, 0x201)) -> CaseResult:
    case = next(case for case in Scenarios.CASES if case.name == name)
    result = CaseResult(case, pathlib.Path('/run') / name)
    result.handles = BootEntropy.handles([
        'USER:TAP version 14', handle_line('endpoint', selectors[0], authenticators[0]),
        handle_line('diagnostic', selectors[1], authenticators[1]), 'USER:ok 1 - bss.zero'])
    return result


class RecordingContext:
    """The parts of an Executor.CaseContext that `record` uses."""

    def __init__(self, name: str, lines: list[str]):
        self.case = next(case for case in Scenarios.CASES if case.name == name)
        self.console = types.SimpleNamespace(lines=lines)
        self.target = types.SimpleNamespace(root=ROOT)
        self.result = CaseResult(self.case, pathlib.Path('/run') / name)

    def check_problems(self, name: str, problems: list[str]) -> bool:
        self.result.checks.append(CheckResult(name, not problems, '\n'.join(problems)))
        return not problems


def recorded(name: str, lines: list[str]) -> CaseResult:
    context = RecordingContext(name, lines)
    BootEntropy.record(context)
    return context.result


class SourceTests(unittest.TestCase):
    def test_the_header_names_the_bits(self):
        self.assertEqual(BITS, {'FirmwareRng': 1, 'ProcessorRng': 2, 'CycleCounterJitter': 4, 'FirmwareState': 8})

    def test_one_well_formed_line(self):
        self.assertEqual(BootEntropy.sources(['x', AMD64_SOURCES, 'y']), (0xD, []))
        self.assertIsNone(BootEntropy.sources([])[0])
        self.assertIsNone(BootEntropy.sources([AMD64_SOURCES, AMD64_SOURCES])[0])
        self.assertIsNone(BootEntropy.sources(['ASTERIA:BOOT-ENTROPY:SOURCES:0xd'])[0])

    def test_credited_sources_and_the_rng_device(self):
        self.assertEqual(BootEntropy.source_problems(0xD, BITS, rng_device=True), [])
        self.assertEqual(BootEntropy.source_problems(0xC, BITS, rng_device=False), [])
        self.assertEqual(len(BootEntropy.source_problems(0xC, BITS, rng_device=True)), 1)
        self.assertEqual(len(BootEntropy.source_problems(0x8, BITS, rng_device=False)), 1)
        self.assertEqual(len(BootEntropy.source_problems(0x14, BITS, rng_device=False)), 1)

    def test_per_boot_check(self):
        amd64 = recorded('Amd64.Smp1', [AMD64_SOURCES, handle_line('endpoint', 0x101, 0x1234)])
        self.assertEqual([(check.name, check.passed) for check in amd64.checks], [(BootEntropy.CHECK, True)])
        self.assertEqual(amd64.handles, {'endpoint': (0x101, 0x1234)})
        self.assertFalse(recorded('Amd64.Smp1', [IA64_SOURCES, handle_line('endpoint', 1, 2)]).checks[0].passed)
        self.assertTrue(recorded('Ia64.Smp1', [IA64_SOURCES, handle_line('endpoint', 1, 2)]).checks[0].passed)
        self.assertFalse(recorded('Ia64.Smp1', [IA64_SOURCES]).checks[0].passed, 'the guest reported no handle')
        self.assertTrue(recorded('Ia64.Production', [IA64_SOURCES]).checks[0].passed)
        self.assertFalse(recorded('Ia64.Production', []).checks[0].passed)


class MachineTests(unittest.TestCase):
    def test_8042_can_be_disabled_without_changing_the_default_profile(self):
        with tempfile.TemporaryDirectory() as directory:
            esp = pathlib.Path(directory) / 'Esp'
            esp.mkdir()
            variables = pathlib.Path(directory) / 'vars.fd'
            variables.write_bytes(b'')
            environment = Machine.Environment('ia64-qemu', 'ia64.bin', 'bios', 'qemu-x86', 'code.fd',
                                              str(variables))
            launch = Machine.prepare('Amd64', esp, pathlib.Path(directory) / 'off', 1, '', environment,
                                     i8042=False)
            for sock in (launch.qmp, launch.debug, *launch.inherited):
                sock.close()
        self.assertIn('q35,i8042=off', launch.argv)
        self.assertIn('tcg,thread=multi', launch.argv)
        [case] = Scenarios.select([], ['Amd64.NoI8042'])
        self.assertFalse(case.i8042)

    def test_modern_platform_profiles_select_x2apic_and_remove_pit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            esp = root / 'Esp'
            esp.mkdir()
            variables = root / 'vars.fd'
            variables.write_bytes(b'')
            environment = Machine.Environment('ia64-qemu', 'ia64.bin', 'bios', 'qemu-x86', 'code.fd', str(variables))
            for suite in ('Amd64X2Apic', 'Amd64NoPit'):
                [case] = Scenarios.select([suite], [])
                launch = Machine.prepare('Amd64', esp, root / suite, case.cpus, '', environment,
                                         x2apic=case.x2apic, pit=case.pit)
                try:
                    self.assertEqual(case.cpus, 4)
                    self.assertIn('max,+x2apic' if case.x2apic else 'qemu64', launch.argv)
                    self.assertIn('q35' if case.pit else 'q35,pit=off,hpet=on', launch.argv)
                    self.assertEqual(case.preset, 'amd64-x2apic-probe' if case.x2apic else 'amd64')
                finally:
                    for sock in (launch.qmp, launch.debug, *launch.inherited):
                        sock.close()

    def test_only_the_amd64_profile_has_an_rng_device(self):
        with tempfile.TemporaryDirectory() as directory:
            esp = pathlib.Path(directory) / 'Esp'
            esp.mkdir()
            (pathlib.Path(directory) / 'vars.fd').write_bytes(b'')
            environment = Machine.Environment('ia64-qemu', 'ia64.bin', 'bios', 'qemu-x86', 'code.fd',
                                              str(pathlib.Path(directory) / 'vars.fd'))
            launches = {isa: Machine.prepare(isa, esp, pathlib.Path(directory) / isa, 1, False, environment)
                        for isa in ('Amd64', 'Ia64')}
            argvs = {isa: launch.argv for isa, launch in launches.items()}
            for launch in launches.values():
                for sock in (launch.qmp, launch.debug, *launch.inherited):
                    sock.close()
        self.assertIn(Machine.RNG_DEVICE['Amd64'], argvs['Amd64'])
        self.assertFalse(any('rng' in argument for argument in argvs['Ia64']))


class CrossBootTests(unittest.TestCase):
    def test_distinct_authenticators_pass(self):
        results = [boot('Amd64.Smp1', (1, 2)), boot('Amd64.Smp2', (3, 4)), boot('Amd64.Smp3', (5, 6))]
        [line] = BootEntropy.cross_boot(results)
        self.assertTrue(line.startswith('PASS boot entropy amd64/firmware-test: 3 boots'))
        self.assertTrue(all(result.checks[-1].name == BootEntropy.CROSS_BOOT_CHECK and result.checks[-1].passed
                            for result in results))

    def test_a_repeated_authenticator_fails_every_boot_of_the_image(self):
        results = [boot('Amd64.Smp1', (1, 2)), boot('Amd64.Smp2', (3, 4)), boot('Amd64.Smp3', (1, 6))]
        [line] = BootEntropy.cross_boot(results)
        self.assertTrue(line.startswith('FAIL'))
        self.assertEqual({result.checks[-1].passed for result in results}, {False})
        self.assertIn('Amd64.Smp1/Amd64.Smp3: handle endpoint has the same authenticator',
                      results[0].checks[-1].detail)

    def test_a_different_selector_makes_the_comparison_fail(self):
        problems = BootEntropy.distinct_problems({'A': {'endpoint': (1, 5)}, 'B': {'endpoint': (2, 6)}})
        self.assertEqual(len(problems), 1)

    def test_only_boots_of_one_image_are_compared(self):
        """Different trees (relocated) or a single boot per image give no check."""
        results = [boot('Amd64.Smp1', (1, 2)), boot('Amd64.Relocated', (1, 2)), boot('Ia64.Smp1', (1, 2))]
        self.assertEqual(BootEntropy.cross_boot(results), [])
        self.assertEqual([result.checks for result in results], [[], [], []])


if __name__ == '__main__':
    unittest.main()
