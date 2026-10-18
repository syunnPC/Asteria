# SPDX-License-Identifier: Apache-2.0
"""Tests of Observers.Amd64Cpu over recorded `dump-guest-memory` cores and their mutants.

`test_h20_*` port the register part of CheckAmd64Runtime.py --self-test
use the same baseline-plus-one-mutation scheme, applied to the
decoded note state instead of HMP text.
"""
from __future__ import annotations

import dataclasses
import json
import pathlib
import socket
import tempfile
import threading
import unittest
from unittest import mock

from AsteriaVm.Drivers import Amd64Default
from AsteriaVm.Observers import Amd64Cpu
from AsteriaVm.Qmp import Qmp
from .test_amd64_fixtures import constants, core, idle_loop, note_descriptors, record, replace_cpu, set_register, snapshot

USER_STACK_TOP = 0x5000_5000
BUSY = 0x4000_0EF0   # inside AsteriaAmd64MinimalUserImageMain of GuestTests.elf, outside the idle loop


def user_mode_expectation() -> Amd64Cpu.UserModeExpectation:
    c = constants()
    return Amd64Cpu.UserModeExpectation(c.user_code_selector, c.user_data_selector,
                                        c.user_stack_start + c.user_stack_pages * 4096,
                                        c.user_rflags_required, c.user_rflags_permitted)


def policy() -> Amd64Cpu.ControlRegisterPolicy:
    return Amd64Default.control_register_policy(constants())


def bsp_problems(state: Amd64Cpu.CpuSnapshot, name: str = 'Smp4') -> dict[str, list[str]]:
    bsp = state.cpus[0]
    return {
        'idle-loop': Amd64Cpu.idle_problems(bsp, idle_loop(name)),
        'user-mode': Amd64Cpu.user_mode_problems(bsp, user_mode_expectation()),
        'control-registers': Amd64Cpu.control_register_problems(bsp, policy()),
        'root-table': Amd64Cpu.root_table_problems(bsp),
        'long-mode': Amd64Cpu.long_mode_problems(state, bsp),
    }


class CoreParsingTests(unittest.TestCase):
    def test_recorded_core_decodes_every_cpu(self):
        state = snapshot('Smp4')
        self.assertTrue(state.long_mode_attested)
        self.assertEqual([cpu.index for cpu in state.cpus], [0, 1, 2, 3])
        bsp = state.cpus[0]
        self.assertIn(bsp.rip, idle_loop().addresses)
        self.assertEqual((bsp.cs.selector, bsp.ss.selector, bsp.cpl, bsp.rsp), (0x23, 0x1B, 3, USER_STACK_TOP))
        runtime = record('Smp4').Runtime
        self.assertEqual([cpu.cr3 for cpu in state.cpus[1:]],
                         [constants().idle_root(runtime.PoolPhysicalStart, cpu) for cpu in range(1, 4)])
        self.assertEqual(bsp.cr3, runtime.UserRootPhysicalAddress)
        self.assertIn(snapshot('Ahci').cpus[0].rip, idle_loop('Ahci').addresses)
        self.assertEqual(len(snapshot('Ahci').cpus), 1)

    def test_malformed_cores_are_rejected(self):
        data = core('Smp4')
        notes = [offset for _, _, offset in note_descriptors(data)]
        mutants = {
            'truncated': data[:200],
            'not ELF': b'\0' + data[1:],
            'big-endian': data[:5] + b'\x02' + data[6:],
            'ELF class 32 (CPU 0 not in long mode)': data[:4] + b'\x01' + data[5:],
            'PRSTATUS names another CPU': data[:notes[1] + 32] + b'\x07' + data[notes[1] + 33:],
            'notes disagree on RIP': data[:notes[0] + 112 + 16 * 8] + b'\x99' + data[notes[0] + 112 + 16 * 8 + 1:],
        }
        for name, mutant in mutants.items():
            with self.subTest(name), self.assertRaises(Amd64Cpu.CpuStateError):
                Amd64Cpu.parse_core(mutant)

    def test_set_register_rewrites_both_notes(self):
        state = Amd64Cpu.parse_core(set_register(core('Smp4'), 0, 'rip', 0x4000_0B50))
        self.assertEqual(state.cpus[0].rip, 0x4000_0B50)


class BspTests(unittest.TestCase):
    def test_recorded_bsp_passes(self):
        for name in ('Smp4', 'Ahci', 'PreemptionRelocated'):
            problems = bsp_problems(snapshot(name), name)
            self.assertEqual({key: value for key, value in problems.items() if value}, {}, name)

    def test_h20_register_mutants_are_rejected(self):
        baseline = snapshot('Smp4')
        ss = baseline.cpus[0].ss
        cs = baseline.cpus[0].cs
        mutants = {
            'idle-loop': [dict(registers={'rip': 0x4000_1000}),
                          dict(registers={'rip': min(idle_loop().addresses) + 1})],  # inside `pause`
            'user-mode': [dict(segments={'ss': type(ss)(ss.selector, ss.limit, ss.flags & ~(3 << 13), ss.base)}),
                          dict(segments={'cs': type(cs)(0x8, cs.limit, cs.flags, cs.base)}),
                          dict(segments={'ss': type(ss)(0x10, ss.limit, ss.flags, ss.base)}),
                          dict(registers={'rsp': 0x5000_0000}),
                          dict(registers={'rflags': 0x3202}),
                          dict(registers={'rflags': 0x2})],
            'control-registers': [dict(control={0: baseline.cpus[0].cr0 & ~(1 << 16)}),
                                  dict(control={0: baseline.cpus[0].cr0 | 1 << 18}),
                                  dict(control={4: 0x668}), dict(control={4: 0x20}), dict(control={2: 0x1234})],
            'root-table': [dict(control={3: baseline.cpus[0].cr3 | 1}), dict(control={3: 0})],
            'long-mode': [dict(segments={'cs': type(cs)(cs.selector, cs.limit, cs.flags & ~(1 << 21), cs.base)}),
                          dict(control={4: 0x40})],
        }
        for check, changes in mutants.items():
            for change in changes:
                with self.subTest(check=check, change=change):
                    problems = bsp_problems(replace_cpu(baseline, 0, **change))
                    self.assertTrue(problems[check], problems)

    def test_the_cr4_policy_is_the_kernels(self):
        """CR4 is exactly the value the entry install writes: every other bit set, and PAE or MCE cleared, is
        rejected."""
        baseline = snapshot('Smp4')
        cr4 = baseline.cpus[0].cr4
        self.assertEqual(cr4, constants().cr4_installed)
        for bit in range(32):
            with self.subTest(bit=bit):
                problems = bsp_problems(replace_cpu(baseline, 0, control={4: cr4 ^ 1 << bit}))
                self.assertIn('CR4 %#x, expected %#x' % (cr4 ^ 1 << bit, cr4), problems['control-registers'][0])

    def test_long_mode_needs_qemus_64_bit_core(self):
        state = snapshot('Smp4')
        self.assertTrue(Amd64Cpu.long_mode_problems(dataclasses.replace(state, machine=3), state.cpus[0]))


class ApplicationProcessorTests(unittest.TestCase):
    def expectation(self, cpu):
        c, runtime = constants(), record('Smp4').Runtime
        low, high = c.idle_stack_range(runtime.PoolPhysicalStart, cpu)
        return Amd64Cpu.ParkedProcessorExpectation(c.idle_root(runtime.PoolPhysicalStart, cpu), c.kernel_code_selector,
                                                   c.kernel_data_selector, low, high,
                                                   (record('Smp4').Smp.InterruptWindowStart + 1, record('Smp4').Smp.InterruptWindowEnd))

    def test_recorded_processors_are_parked(self):
        state = snapshot('Smp4')
        c = constants()
        for cpu in state.cpus[1:]:
            self.assertEqual(Amd64Cpu.parked_processor_problems(cpu, self.expectation(cpu.index)), [])
            gdt = state.cpus[0].segments['gdt'].base + cpu.index * c.descriptor_tables_size
            self.assertEqual(Amd64Cpu.descriptor_table_problems(cpu, gdt, gdt + c.idt_offset), [])
            self.assertEqual(Amd64Cpu.long_mode_problems(state, cpu), [])

    def test_parked_processor_mutants_are_rejected(self):
        state = snapshot('Smp4')
        cs = state.cpus[1].cs
        for change in (dict(control={3: state.cpus[0].cr3}), dict(registers={'rflags': 0x246, 'rip': record('Smp4').Smp.InterruptWindowEnd}),
                       dict(registers={'rsp': self.expectation(1).stack_low}),
                       dict(registers={'rsp': self.expectation(1).stack_high + 8}),
                       dict(segments={'cs': type(cs)(0x23, cs.limit, cs.flags, cs.base)})):
            with self.subTest(change=change):
                mutated = replace_cpu(state, 1, **change).cpus[1]
                self.assertTrue(Amd64Cpu.parked_processor_problems(mutated, self.expectation(1)))
        cpu = state.cpus[2]
        gdt = cpu.segments['gdt'].base
        self.assertTrue(Amd64Cpu.descriptor_table_problems(cpu, gdt + 8192, gdt + 8192 + 4096))

    def test_interrupts_are_enabled_only_inside_the_window(self):
        state = snapshot('Smp4')
        expectation = self.expectation(1)
        begin, end = expectation.interrupt_window
        for rip in (begin, end - 1):
            with self.subTest(rip=rip):
                cpu = replace_cpu(state, 1, registers={'rflags': 0x246, 'rip': rip}).cpus[1]
                self.assertEqual(Amd64Cpu.parked_processor_problems(cpu, expectation), [])
        for rip in (begin - 1, end):
            with self.subTest(rip=rip):
                cpu = replace_cpu(state, 1, registers={'rflags': 0x246, 'rip': rip}).cpus[1]
                self.assertTrue(Amd64Cpu.parked_processor_problems(cpu, expectation))

    def test_every_processor_follows_the_control_register_policy(self):
        """The APs have the BSP's policy, not what their startup code left (CR4 0x20)."""
        for name in ('Smp4', 'PreemptionRelocated'):
            for cpu in snapshot(name).cpus:
                with self.subTest(name=name, cpu=cpu.index):
                    self.assertEqual(Amd64Cpu.control_register_problems(cpu, policy()), [])
        state = snapshot('Smp4')
        cr0 = state.cpus[1].cr0
        for change, problem in ((dict(control={4: 0x20}), 'CPU 1 CR4 0x20, expected 0x660'),
                                (dict(control={4: 0x668}), 'CPU 1 CR4 0x668, expected 0x660'),
                                (dict(control={4: 0x60}), 'CPU 1 CR4 0x60, expected 0x660'),
                                (dict(control={0: cr0 | 1 << 18}), 'forbidden bits 0x40000'),
                                (dict(control={0: cr0 | 1 << 3}), 'forbidden bits 0x8'),
                                (dict(control={0: cr0 | 1 << 2}), 'forbidden bits 0x4'),
                                (dict(control={0: cr0 & ~0x2}), 'lacks 0x2'),
                                (dict(control={0: cr0 & ~0x20}), 'lacks 0x20'),
                                (dict(control={2: 0x1000}), 'CR2 0x1000'),
                                (dict(control={2: 0x10}), 'CR2 0x10'),
                (dict(control={2: 0x4800_2000}), 'CR2 0x48002000')):
            with self.subTest(change=change):
                mutated = replace_cpu(state, 1, **change).cpus[1]
                self.assertIn(problem, '\n'.join(Amd64Cpu.control_register_problems(mutated, policy())))
        # A first touch of the lazy region, or the spinner's final read, leaves
        # its address in CR2.
        for address in (0x4800_0000, 0x4800_1FF8, 0x8):
            mutated = replace_cpu(state, 0, control={2: address}).cpus[0]
            self.assertEqual(Amd64Cpu.control_register_problems(mutated, policy()), [], hex(address))


class FakeDumpingQemu(threading.Thread):
    """Answers `dump-guest-memory` with the recorded core, CPU 0's RIP taken from a script."""

    def __init__(self, peer: socket.socket, rips: list[int]):
        super().__init__(daemon=True)
        self.peer, self.rips, self.dumps = peer, list(rips), 0

    def run(self):
        stream = self.peer.makefile('rwb')

        def send(message):
            stream.write((json.dumps(message) + '\n').encode())
            stream.flush()

        send({'QMP': {'version': {}, 'capabilities': []}})
        for raw in stream:
            request = json.loads(raw)
            if request['execute'] == 'dump-guest-memory':
                rip = self.rips[min(self.dumps, len(self.rips) - 1)]
                self.dumps += 1
                path = pathlib.Path(request['arguments']['protocol'][len('file:'):])
                path.write_bytes(set_register(core('Smp4'), 0, 'rip', rip))
            send({'return': {}, 'id': request.get('id')})
        self.peer.close()


class WaitForIdleTests(unittest.TestCase):
    def poll(self, rips, *, timeout=5.0, abort=lambda: None):
        ours, theirs = socket.socketpair()
        fake = FakeDumpingQemu(theirs, rips)
        fake.start()
        with tempfile.TemporaryDirectory() as directory:
            qmp = Qmp(ours, scratch=pathlib.Path(directory))
            qmp.handshake(timeout=5)
            try:
                return Amd64Cpu.wait_for_idle(qmp, pathlib.Path(directory) / 'Poll.elf', idle_loop(), timeout,
                                              abort=abort, interval=0.001) + (fake.dumps,)
            finally:
                qmp.close()

    def test_polls_until_the_idle_loop(self):
        busy = BUSY  # the image has not returned yet
        reached, detail, dumps = self.poll([busy, busy, min(idle_loop().addresses)])
        self.assertTrue(reached, detail)
        self.assertEqual(dumps, 3)
        self.assertIn('after 3 sample(s)', detail)

    def test_never_idle_times_out(self):
        reached, detail, _ = self.poll([BUSY], timeout=0.2)
        self.assertFalse(reached)
        self.assertIn('timeout', detail)
        self.assertIn('%#x' % BUSY, detail)

    def test_abort_ends_the_wait(self):
        reached, detail, dumps = self.poll([BUSY], abort=lambda: 'QEMU exited with status 1')
        self.assertFalse(reached)
        self.assertEqual(dumps, 0)
        self.assertIn('QEMU exited', detail)


class IdleLoopTests(unittest.TestCase):
    @staticmethod
    def decoded(*texts):
        from AsteriaVerify import Disassembly
        instructions = [Disassembly.Instruction(0x4000_0B47 + 2 * index, text) for index, text in enumerate(texts)]
        return Disassembly.DecodedRange('image', '.text', 0x4000_0B47, 0x4000_0B4B, 'Idle', instructions)

    def test_closed_loop_yields_its_instruction_addresses(self):
        with mock.patch('AsteriaVerify.Disassembly.disassemble', return_value=self.decoded('pause', 'jmp <+0x0>')):
            loop = Amd64Cpu.idle_loop('objdump', pathlib.Path('image'), 'Idle')
        self.assertEqual(loop.addresses, {0x4000_0B47, 0x4000_0B49})

    def test_open_loop_is_rejected(self):
        for texts in (('pause', 'jmp <+0x2>'), ('pause', 'ret'), ()):
            with self.subTest(texts=texts), mock.patch('AsteriaVerify.Disassembly.disassemble',
                                                       return_value=self.decoded(*texts)):
                with self.assertRaises(Amd64Cpu.CpuStateError):
                    Amd64Cpu.idle_loop('objdump', pathlib.Path('image'), 'Idle')


if __name__ == '__main__':
    unittest.main()
