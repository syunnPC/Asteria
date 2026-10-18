# SPDX-License-Identifier: Apache-2.0
"""Unit tests of the user-image checks (TargetChecks/UserImages.py).

Synthetic layouts and disassemblies are structured test fixtures.
"""
from __future__ import annotations

import dataclasses
import re
import unittest

from AsteriaVerify import Layout
from AsteriaVerify.ControlFlow import Code
from AsteriaVerify.Disassembly import Instruction
from AsteriaVerify.Elf import PF_R, PF_W, PF_X, PT_LOAD, SHF_ALLOC, SHF_WRITE, SHT_NOBITS, STB_GLOBAL, STT_OBJECT, \
    STV_DEFAULT, Section, Segment, Symbol
from AsteriaVerify.TargetChecks import UserImages

PROBE = UserImages.PROBE['Ia64']
FLAGS = {'R E': PF_R | PF_X, 'R': PF_R, 'RW': PF_R | PF_W, 'E': PF_X, 'RWE': PF_R | PF_W | PF_X}


def loads(text: str) -> tuple[Segment,...]:
    """readelf -Wl LOAD rows: Offset VirtAddr PhysAddr FileSiz MemSiz Flg Align."""
    result = []
    for line in text.strip().splitlines():
        fields = line.split()
        offset, virtual, physical, file_size, memory_size = (int(value, 16) for value in fields[1:6])
        result.append(Segment(PT_LOAD, FLAGS[' '.join(fields[6:-1])], offset, virtual, physical, file_size,
                              memory_size, int(fields[-1], 16)))
    return tuple(result)


def bss(address: int, size: int = 0x20) -> tuple[Section,...]:
    return (Section(4, '.bss', SHT_NOBITS, SHF_WRITE | SHF_ALLOC, address, address, size, 0, 0, 8, 0),)


def probe(value: int) -> tuple[Symbol,...]:
    return (Symbol(PROBE, value, 8, STT_OBJECT, STB_GLOBAL, STV_DEFAULT, 4),)


VALID = loads("""
  LOAD 0x001000 0x0000000000001000 0x0000000000001000 0x000100 0x000100 R E 0x1000
  LOAD 0x002000 0x0000000000002000 0x0000000000002000 0x000100 0x000100 R   0x1000
  LOAD 0x003000 0x0000000000003000 0x0000000000003000 0x000100 0x000180 RW  0x1000
""")


class ImageSetTests(unittest.TestCase):
    """The loader contract and the shared runtime asm hold for every user image; the call
    structure of the production program only for the production images."""

    def test_each_image_is_checked_against_its_own_bss_probe(self):
        self.assertEqual(UserImages.probe_name('Ia64', 'firmware-test'), UserImages.GUEST_PROBE)
        for isa in ('Ia64', 'Amd64'):
            for variant in UserImages.PRODUCTION_IMAGES:
                self.assertEqual(UserImages.probe_name(isa, variant), UserImages.PROBE[isa], (isa, variant))

    def test_the_guest_image_is_spared_only_the_production_call_structure(self):
        for check in (UserImages.UserImageHeader, UserImages.UserImageEntry, UserImages.UserImageSections,
                      UserImages.UserImageLoadGeometry, UserImages.UserImageLinkClosure,
                      UserImages.Ia64UserImageTransportBreak, UserImages.Ia64UserImageBootstrapForwarding):
            self.assertEqual(check.variants, UserImages.IMAGES, check.__name__)
        for check in (UserImages.Ia64UserImageMainCall, UserImages.Ia64UserImageRuntimeCalls):
            self.assertNotIn('firmware-test', check.variants, check.__name__)


class GeometryOracleTests(unittest.TestCase):
    """Load and BSS geometry fixtures."""

    def geometry(self, segments=VALID, sections=bss(0x3100), symbols=probe(0x3108)):
        return UserImages.UserImageLoadGeometry().verify(UserImages.GeometryFacts(segments, sections, symbols, PROBE))

    def test_valid_layout(self):
        self.assertEqual(self.geometry(), [])

    def test_rejected_layouts(self):
        cases = {
            'missing read permission': dict(segments=loads("""
  LOAD 0x001000 0x0000000000001000 0x0000000000001000 0x000100 0x000100 E   0x1000
  LOAD 0x002000 0x0000000000002000 0x0000000000002000 0x000100 0x000100 R   0x1000
  LOAD 0x003000 0x0000000000003000 0x0000000000003000 0x000100 0x000180 RW  0x1000""")),
            'memsz below filesz': dict(segments=loads("""
  LOAD 0x001000 0x0000000000001000 0x0000000000001000 0x000100 0x000100 R E 0x1000
  LOAD 0x002000 0x0000000000002000 0x0000000000002000 0x000120 0x000100 R   0x1000
  LOAD 0x003000 0x0000000000003000 0x0000000000003000 0x000100 0x000180 RW  0x1000""")),
            'wrapping memory extent': dict(segments=loads("""
  LOAD 0x001000 0x0000000000001000 0x0000000000001000 0x000100 0x000100 R E 0x1000
  LOAD 0x002000 0x0000000000002000 0x0000000000002000 0x000100 0x000100 R   0x1000
  LOAD 0x003000 0xfffffffffffffff0 0xfffffffffffffff0 0x000008 0x000020 RW  0x1000""")),
            '.bss in the read-only segment': dict(segments=loads("""
  LOAD 0x001000 0x0000000000001000 0x0000000000001000 0x000100 0x000100 R E 0x1000
  LOAD 0x002000 0x0000000000003000 0x0000000000003000 0x000120 0x000120 R   0x1000
  LOAD 0x004000 0x0000000000004000 0x0000000000004000 0x000100 0x000180 RW  0x1000""")),
            'writable executable text': dict(segments=loads("""
  LOAD 0x001000 0x0000000000001000 0x0000000000001000 0x000100 0x000100 RWE 0x1000
  LOAD 0x002000 0x0000000000002000 0x0000000000002000 0x000100 0x000100 R   0x1000
  LOAD 0x003000 0x0000000000003000 0x0000000000003000 0x000100 0x000180 RW  0x1000""")),
            'overlapping read-only and data': dict(segments=loads("""
  LOAD 0x001000 0x0000000000001000 0x0000000000001000 0x000100 0x000100 R E 0x1000
  LOAD 0x002000 0x0000000000003000 0x0000000000003000 0x000120 0x000120 R   0x1000
  LOAD 0x003000 0x0000000000003000 0x0000000000003000 0x000100 0x000180 RW  0x1000""")),
            'file-backed .bss': dict(sections=bss(0x30f0)),
            'probe outside .bss': dict(symbols=probe(0x3120)),
        }
        for label, arguments in cases.items():
            with self.subTest(label):
                self.assertNotEqual(self.geometry(**arguments), [])


def disassembly(text: str, start: int = 0x1000) -> Code:
    """`ADDR: [TPL] (pN) instruction;;` lines, with the
    objdump target `HEX <sym>` in the resolved image form `<sym>`."""
    instructions = []
    for line in text.strip().splitlines():
        address, _, body = line.strip().partition(':')
        body = re.sub(r'^\s*\[[A-Z]+\]\s*', '', body).strip()
        stop = body.endswith(';;')
        body = body[:-2] if stop else body
        predicate = None
        match = re.match(r'^\((p\d+)\)\s*(.*)$', body)
        if match:
            predicate, body = match.group(1), match.group(2)
        body = re.sub(r'(=|\s)[0-9a-f]+ <([^>]*)>', r'\1<\2>', body)
        instructions.append(Instruction(int(address, 16), body, 'MIB', 2, stop, predicate))
    return Code(start, tuple(instructions))


RUN = 'RunIa64MinimalUserImageSymbol'


class CallStructureOracleTests(unittest.TestCase):
    """Minimal user-image call-structure fixtures."""

    def main(self, text):
        return UserImages.Ia64UserImageMainCall().verify(UserImages.MainCallFacts(disassembly(text), (RUN,)))

    def runtime(self, text):
        return UserImages.Ia64UserImageRuntimeCalls().verify(UserImages.RuntimeCallFacts(
            disassembly(text), (RUN,), tuple((callee, 1) for callee, _ in UserImages.RUNTIME_CALLS)))

    VALID_MAIN = """
        1000: movl r14=2000 <%s>;;
        1010: br.call.sptk.many b0=2000 <%s>;;""" % (RUN, RUN)

    def test_main(self):
        self.assertEqual(self.main(self.VALID_MAIN), [])
        self.assertNotEqual(self.main('1000: movl r14=2000 <%s>;;' % RUN), [])
        self.assertNotEqual(self.main(self.VALID_MAIN + '\n1020: br.call.sptk.many b0=2000 <%s>;;' % RUN), [])
        self.assertNotEqual(self.main('1000: (p06) br.call.sptk.many b0=2000 <%s>;;' % RUN), [])

    def test_main_sibling_call_is_accepted(self):
        self.assertEqual(self.main('1000: br.sptk.many 2000 <%s>;;' % RUN), [])
        self.assertNotEqual(self.main('1000: (p06) br.cond.sptk.few 2000 <%s>;;' % RUN), [])

    def calls(self, *names, predicated=None, extra=()):
        helper, query, send, receive = (callee for callee, _ in UserImages.RUNTIME_CALLS)
        symbols = {'helper': helper, 'query': query, 'send': send, 'receive': receive}
        lines = []
        for index, name in enumerate(list(names) + list(extra)):
            prefix = '(p06) ' if name == predicated else ''
            lines.append('%x: %sbr.call.sptk.many b0=%x <%s>;;' % (0x1000 + 16 * index, prefix, 0x2000, symbols[name]))
        return '\n'.join(lines)

    def test_runtime(self):
        self.assertEqual(self.runtime(self.calls('helper', 'query', 'send', 'receive')), [])
        rejected = {
            'reordered': self.calls('helper', 'send', 'query', 'receive'),
            'missing query': self.calls('helper', 'send', 'receive'),
            'missing send': self.calls('helper', 'query', 'receive'),
            'missing receive': self.calls('helper', 'query', 'send'),
            'duplicate helper': self.calls('helper', 'query', 'send', 'receive', extra=['helper']),
            'duplicate send': self.calls('helper', 'query', 'send', 'receive', extra=['send']),
            'duplicate receive': self.calls('helper', 'query', 'send', 'receive', extra=['receive']),
        }
        for callee in ('helper', 'query', 'send', 'receive'):
            rejected['predicated %s' % callee] = self.calls('helper', 'query', 'send', 'receive', predicated=callee)
        for label, text in rejected.items():
            with self.subTest(label):
                self.assertNotEqual(self.runtime(text), [])

    def test_runtime_order_is_dominance_not_address(self):
        """Receive's block placed before Send's in the layout, reached only
        through Send: accepted because Send dominates Receive."""
        helper, query, send, receive = (callee for callee, _ in UserImages.RUNTIME_CALLS)
        text = '\n'.join([
            '1000: br.call.sptk.many b0=2000 <%s>;;' % helper,
            '1010: br.call.sptk.many b0=2000 <%s>;;' % query,
            '1020: br.few 1050 <synthetic+0x50>;;',
            '1030: br.call.sptk.many b0=2000 <%s>;;' % receive,
            '1040: br.ret.sptk.many b0;;',
            '1050: br.call.sptk.many b0=2000 <%s>;;' % send,
            '1060: br.few 1030 <synthetic+0x30>;;'])
        text = re.sub(r'(\d+) <synthetic\+(0x[0-9a-f]+)>', lambda m: '<+%s>' % m.group(2), text)
        self.assertEqual(self.runtime(text), [])


MAIN = UserImages.MAIN


class BootstrapForwardingOracleTests(unittest.TestCase):
    """Bootstrap forwarding fixtures, on the
    four-output entry that forwards r4-r7, and the r6/r7 forwarding defects."""

    ALLOC = '1010: [MMI] alloc r2=ar.pfs,4,0,0;;'
    MOVES = '1016: mov r32=r4\n101c: mov r33=r5\n1020: [MMI] mov r34=r6\n1026: mov r35=r7;;'
    CALL = '103c: br.call.sptk.many b0=2000 <%s>;;' % MAIN
    VALID = '\n'.join(('1000: [MLX] nop.m 0x0', ALLOC, MOVES, '1030: [MIB] nop.m 0x0', CALL))

    def forwarding(self, text):
        return UserImages.Ia64UserImageBootstrapForwarding().verify(disassembly(text))

    def entry(self, alloc=ALLOC, moves=MOVES, call=CALL, before=''):
        return '\n'.join(line for line in (before, alloc, moves, call) if line)

    def test_valid(self):
        self.assertEqual(self.forwarding(self.VALID), [])

    def test_rejected(self):
        cases = {
            'reordered call': '\n'.join((self.ALLOC, '101c: br.call.sptk.many b0=2000 <%s>;;' % MAIN,
                                          '1020: mov r32=r4\n1026: mov r33=r5\n102c: mov r34=r6',
                                          '1030: mov r35=r7;;')),
            'deleted call': self.entry(call=''),
            'predicated move': self.entry(moves=self.MOVES.replace('1016: mov', '1016: (p6) mov')),
            'clobbered output': self.entry(moves=self.MOVES.replace('mov r32=r4', 'mov r32=r4\n1018: add r32=1,r32')),
            'duplicate call': self.entry(call=self.CALL + '\n104c: br.call.sptk.many b0=2000 <%s>;;' % MAIN),
            'missing allocation stop': self.entry(alloc=self.ALLOC[:-2]),
            'missing argument stop': self.entry(moves=self.MOVES[:-2]),
            'reloaded global pointer': self.entry(before='1006: movl r1=0x1234;;'),
            'incremented global pointer': self.entry(before='1006: ld8 r14=[r1],8;;'),
            'two-output frame': self.entry(alloc=self.ALLOC.replace(',4,0,0', ',2,0,0')),
            'r6/r7 not forwarded': self.entry(moves='1016: mov r32=r4\n101c: mov r33=r5;;'),
            'swapped r6/r7 outputs': self.entry(moves=self.MOVES.replace('r34=r6', 'r34=r7').replace('r35=r7',
                                                                                                    'r35=r6')),
        }
        for label, text in cases.items():
            with self.subTest(label):
                self.assertNotEqual(self.forwarding(text), [])


class FloatingPointLaunchTests(unittest.TestCase):
    """User threads own f2-f127; the synthetic probe uses none."""

    PSR = 0x0000_1013_080E_2000    # Ia64UserModeInitialProcessorStatus
    BOOT_PSR = 0x0000_1013_0802_6000   # Ia64UserModePreemptibleProcessorStatus

    def problems(self, psr=PSR, boot_psr=BOOT_PSR) -> list[str]:
        return UserImages.Ia64UserFloatingPointLaunch().verify(UserImages.FloatingPointLaunchFacts(psr, boot_psr))

    def test_the_stated_psrs_pass(self):
        self.assertEqual(self.problems(), [])

    def test_each_half_is_judged(self):
        self.assertTrue(self.problems(boot_psr=self.BOOT_PSR | UserImages.PSR_DFL))
        self.assertTrue(self.problems(boot_psr=self.BOOT_PSR | UserImages.PSR_DFH))
        self.assertTrue(self.problems(psr=self.PSR & ~UserImages.PSR_DFL))
        self.assertTrue(self.problems(psr=self.PSR & ~UserImages.PSR_DFH))
        self.assertTrue(self.problems(psr=None))
        self.assertTrue(self.problems(boot_psr=None))

    def test_every_mutant_is_rejected(self):
        prop = UserImages.Ia64UserFloatingPointLaunch()
        facts = UserImages.FloatingPointLaunchFacts(self.PSR, self.BOOT_PSR)
        for description, mutant in prop.mutants(facts):
            self.assertTrue(prop.verify(mutant), description)


class HeaderTests(unittest.TestCase):
    def test_break_immediate(self):
        for text, value in (("inline constexpr Foundation::UInt64 Ia64SystemCallBreakImmediate{0x10'0000U};", 0x100000),
                            ('constexpr UInt64 Ia64SystemCallBreakImmediate{4096U};', 4096),
                            ('constexpr UInt64 OtherImmediate{1U};', None)):
            self.assertEqual(Layout.constants(text).get(UserImages.BREAK_IMMEDIATE), value)

    def test_transport_requires_matching_kernel_immediate(self):
        transport = Code(0, (Instruction(0x5c, 'break.i 0x100000', 'MII', 2, True),))

        def problems(code, kernel_immediate):
            return UserImages.Ia64UserImageTransportBreak().verify(
                UserImages.TransportFacts(code, 0x100000, kernel_immediate))
        self.assertEqual(problems(transport, 0x100000), [])
        self.assertNotEqual(problems(transport, 0x100001), [])
        self.assertNotEqual(problems(dataclasses.replace(transport, instructions=()), 0x100000), [])


if __name__ == '__main__':
    unittest.main()
