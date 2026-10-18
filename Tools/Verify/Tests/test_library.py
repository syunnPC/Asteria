# SPDX-License-Identifier: Apache-2.0
"""Unit tests of the verification library that need no build tree.

Run with: python3 -m unittest discover -s Tools/Verify/Tests -t Tools/Verify
"""
from __future__ import annotations

import dataclasses
import json
import pathlib
import tempfile
import unittest

from AsteriaVerify import Checks, Disassembly, Golden, Ia64, Layout, Mutate, Properties
from AsteriaVerify.Context import TargetContext, source_of_object
from AsteriaVerify.Disassembly import Instruction
from AsteriaVerify.Elf import SHF_ALLOC, SHF_EXECINSTR, SHF_WRITE, SHT_PROGBITS, STB_GLOBAL, STB_LOCAL, STB_WEAK, \
    STT_FUNC, STV_DEFAULT, STV_HIDDEN, STV_PROTECTED, Section, Symbol
from AsteriaVerify.Properties import RelocationFact
from AsteriaVerify.Subjects import Leaf

OBJDUMP_TEXT = """\
0000000000000000 <AsteriaIa64StartKernelContext>:
       0:\t[MII]       alloc r16=ar.pfs,3,2,0
       6:\t            mov r17=r4
       c:\t            mov r18=r5
      10:\t[MMI]       mov r34=r6;;
      16:\t            mov r1=r18
      1c:\t            mov b6=r17;;
      20:\t[MIB]       nop.m 0x0
      26:\t            nop.i 0x0
      2c:\t            br.call.sptk.many b0=b6;;
      30:\t[MMI]       rsm 0x4000;;
      36:\t            srlz.d
      3c:\t            nop.i 0x0;;
      40:\t[MMI]       hint.m 0;;
      46:\t            nop.m 0x0
      4c:\t            nop.i 0x0
      50:\t[MIB]       nop.m 0x0
      56:\t            nop.i 0x0
      5c:\t            br.few 40 <AsteriaIa64StartKernelContext+0x40>;;
"""


def instructions(*texts: str, template: str = 'MII', start: int = 0) -> list[Instruction]:
    """Builds instructions from `text`, `(pN) text` or `text;;`, three slots
    per bundle from `start`."""
    result = []
    for index, text in enumerate(texts):
        stop = text.endswith(';;')
        body = text[:-2] if stop else text
        predicate = None
        if body.startswith('('):
            predicate, body = body[1:].split(') ', 1)
        address = start + (index // 3) * 16 + 6 * (index % 3)
        result.append(Instruction(address, body, template, index % 3, stop, predicate))
    return result


class DecoderTests(unittest.TestCase):
    def test_bundles_slots_and_stops(self):
        decoded = Disassembly._decode_lines(OBJDUMP_TEXT, True)
        self.assertEqual(len(decoded), 18)
        self.assertEqual((decoded[0].template, decoded[0].slot), ('MII', 0))
        self.assertEqual((decoded[5].template, decoded[5].slot, decoded[5].stop_after), ('MMI', 2, True))
        self.assertEqual(decoded[17].text, 'br.few 40 <AsteriaIa64StartKernelContext+0x40>')

    def test_group_normalization_carries_stops_over_nops(self):
        decoded = Disassembly._decode_lines(OBJDUMP_TEXT, True)
        lines = Disassembly.normalize_ia64_groups(decoded)
        self.assertEqual(lines[:4], ['0:0:alloc r16=ar.pfs,3,2,0', '0:0:mov r17=r4', '0:0:mov r18=r5',
                                     '0:1:mov r34=r6'])
        # rsm ends its group, so srlz.d starts one; the stop on the nop.i
        # after srlz.d becomes stopBefore of hint.m.
        self.assertEqual(lines[7:10], ['1:1:rsm 0x4000', '1:0:srlz.d', '1:1:hint.m 0'])
        self.assertEqual(lines[-1], '1:1:br.few 40 <AsteriaIa64StartKernelContext+0x40>')

    def test_group_indices(self):
        decoded = Disassembly._decode_lines(OBJDUMP_TEXT, True)
        groups = Disassembly.group_indices(decoded)
        self.assertEqual(groups[:6], [0, 0, 0, 0, 1, 1])
        self.assertEqual(groups[9], 3)


class OperandTests(unittest.TestCase):
    def parse(self, text: str, predicate: str | None = None) -> Ia64.Operation:
        return Ia64.parse(Instruction(0, text, 'MII', 0, False, predicate))

    def test_post_increment_load_writes_its_base(self):
        operation = self.parse('ld8 r29=[r24],8')
        self.assertEqual(operation.memory, 'load')
        self.assertEqual(operation.writes, {'r29', 'r24'})
        self.assertIn('r24', operation.reads)

    def test_plain_load_does_not_write_its_base(self):
        self.assertEqual(self.parse('ld8.acq r30=[r17]').writes, {'r30'})

    def test_store_reads_value_and_base(self):
        operation = self.parse('st8.spill [r14]=r4,16')
        self.assertEqual(operation.memory, 'store')
        self.assertEqual(operation.writes, {'r14'})
        self.assertEqual(operation.reads, {'r14', 'r4'})

    def test_compare_writes_predicates(self):
        operation = self.parse('cmp.eq p7,p6=r14,r32')
        self.assertEqual(operation.writes, {'p7', 'p6'})
        self.assertEqual(operation.reads, {'r14', 'r32'})

    def test_predicate_prefix_is_canonical_and_read(self):
        operation = self.parse('br.cond.dpnt.few <+0x130>', 'p06')
        self.assertEqual(operation.predicate, 'p6')
        self.assertIn('p6', operation.reads)
        self.assertTrue(operation.is_direct_branch)
        self.assertEqual(operation.target, Ia64.Target('internal', None, 0x130))

    def test_calls_and_returns(self):
        call = self.parse('br.call.sptk.many b0=<reloc:PCREL21B AsteriaIa64SystemCallBridge>')
        self.assertTrue(call.is_call and call.is_direct_branch)
        self.assertEqual(call.target, Ia64.Target('reloc', 'AsteriaIa64SystemCallBridge', 0, 'PCREL21B'))
        self.assertIn('b0', call.writes)
        indirect = self.parse('br.call.sptk.many b0=b6')
        self.assertTrue(indirect.is_call and indirect.is_indirect_branch)
        self.assertIn('b6', indirect.reads)
        self.assertTrue(self.parse('br.ret.sptk.many b0').is_return)

    def test_system_register_writes(self):
        self.assertTrue(Ia64.is_serialization_write(self.parse('mov cr.itm=r32')))
        self.assertTrue(Ia64.is_serialization_write(self.parse('rsm 0x4000')))
        self.assertTrue(Ia64.is_serialization_write(self.parse('mov rr[r15]=r14')))
        self.assertTrue(Ia64.is_serialization_write(self.parse('itr.d dtr[r14]=r15')))
        self.assertFalse(Ia64.is_serialization_write(self.parse('mov r14=cr.itm')))
        self.assertIn('r15', self.parse('mov rr[r15]=r14').reads)

    def test_alloc_frame(self):
        operation = self.parse('alloc r16=ar.pfs,3,2,0')
        self.assertEqual(Ia64.alloc_frame(operation), (3, 2, 0))
        self.assertTrue(Ia64.touches_rse(operation))

    def test_relocated_immediate(self):
        operation = self.parse('movl r1=<reloc:IMM64 __gp>')
        self.assertEqual(operation.target, Ia64.Target('reloc', '__gp', 0, 'IMM64'))
        self.assertEqual(operation.writes, {'r1'})

    def test_negative_symbol_offset(self):
        self.assertEqual(Ia64.parse_target('addl r14=<reloc:GPREL22 Table-0x8>,r1'),
                         Ia64.Target('reloc', 'Table', -8, 'GPREL22'))


class PropertyFunctionTests(unittest.TestCase):
    def test_serialization_requires_a_later_group(self):
        good = instructions('mov cr.itm=r32;;', 'srlz.d;;', 'mov r14=cr.itm')
        self.assertEqual(Properties.serialization_problems(good), [])
        same_group = instructions('mov cr.itm=r32', 'srlz.d;;', 'mov r14=cr.itm')
        self.assertTrue(Properties.serialization_problems(same_group))
        missing = instructions('mov cr.itm=r32;;', 'mov r14=cr.itm')
        self.assertTrue(Properties.serialization_problems(missing))
        never = instructions('mov cr.itm=r32;;', 'nop.i 0x0')
        self.assertTrue(Properties.serialization_problems(never))

    def test_leaf(self):
        self.assertEqual(Properties.leaf_problems(instructions('mov r8=r32', 'br.ret.sptk.many b0;;')), [])
        self.assertTrue(Properties.leaf_problems(instructions('ld8 r8=[r32]', 'br.ret.sptk.many b0;;')))
        self.assertTrue(Properties.leaf_problems(instructions('adds r12=-16,r12', 'br.ret.sptk.many b0;;')))
        allowed = Properties.leaf_problems(instructions('ld8 r8=[r32]'), allow=lambda op: op.memory == 'load')
        self.assertEqual(allowed, [])

    def test_serialization_rejects_a_dependent_in_the_srlz_group(self):
        self.assertEqual(Properties.serialization_problems(instructions(
            'mov cr.iva=r32;;', 'srlz.i;;', 'mov r14=cr.iva;;', 'br.ret.sptk.many b0;;')), [])
        problems = Properties.serialization_problems(instructions('mov cr.iva=r32;;', 'srlz.i', 'mov r14=cr.iva;;'))
        self.assertIn('in the group of its srlz', problems[0])

    def test_serialization_branch_exemption_is_the_default_only(self):
        branch = instructions('rsm 0x4000', 'br.ret.sptk.many b0;;', 'srlz.d;;')
        self.assertEqual(Properties.serialization_problems(branch), [])
        strict = Properties.serialization_problems(branch, is_dependent=lambda write, later: later.is_branch)
        self.assertIn('share an instruction group', strict[0])


CALLEE = 'Allocate'


def bundles(*groups, start=0x100):
    """Instructions from bundles (template, slot0, slot1, slot2)."""
    result = []
    for number, (template, *slots) in enumerate(groups):
        for slot, text in enumerate(slots):
            stop = text.endswith(';;')
            text = text[:-2] if stop else text
            predicate = None
            if text.startswith('('):
                predicate, text = text[1:].split(') ', 1)
            result.append(Instruction(start + 16 * number + 6 * slot, text, template, slot, stop, predicate))
    return result


class CallSiteTests(unittest.TestCase):
    CALL = ('MIB', 'nop.m 0x0', 'nop.i 0x0', 'br.call.sptk.many b0=<reloc:PCREL21B %s>;;' % CALLEE)

    def problems(self, *groups, **options):
        return Properties.call_problems(bundles(*groups), CALLEE, **options)

    def test_object_call_through_slot_two(self):
        self.assertEqual(self.problems(self.CALL), [])
        self.assertEqual(Properties.call_form(bundles(self.CALL), CALLEE), 'call')

    def test_rejected_call_forms(self):
        cases = {
            'second relocation': (self.CALL, self.CALL),
            'relocation in slot 1': (('MIB', 'nop.m 0x0', 'nop.i 0x0 <reloc:PCREL21B %s>' % CALLEE,
                                      'br.call.sptk.many b0=<+0x0>;;'),),
            'tail jump': (('MIB', 'nop.m 0x0', 'nop.i 0x0', 'br.cond.sptk.many <reloc:PCREL21B %s>;;' % CALLEE),),
            'call through b6': (('MIB', 'nop.m 0x0', 'nop.i 0x0', 'br.call.sptk.many b6=<reloc:PCREL21B %s>;;'
                                 % CALLEE),),
            'predicated call': (('MIB', 'nop.m 0x0', 'nop.i 0x0', '(p06) br.call.sptk.many b0=<reloc:PCREL21B %s>;;'
                                 % CALLEE),),
            'missing call': (('MIB', 'nop.m 0x0', 'nop.i 0x0', 'nop.b 0x0;;'),),
        }
        for label, groups in cases.items():
            with self.subTest(label):
                self.assertNotEqual(self.problems(*groups), [])

    def test_tail_branch_only_when_allowed(self):
        tail = ('MIB', 'nop.m 0x0', 'nop.i 0x0', 'br.sptk.many <%s>;;' % CALLEE)
        self.assertNotEqual(self.problems(tail), [])
        self.assertEqual(self.problems(tail, allow_tail_branch=True), [])
        self.assertEqual(Properties.call_form(bundles(tail), CALLEE), 'tail branch')

    def test_image_form_and_order(self):
        stream = bundles(('MIB', 'nop.m 0x0', 'nop.i 0x0', 'br.call.sptk.many b0=<First>;;'),
                         ('MIB', 'nop.m 0x0', 'nop.i 0x0', 'br.call.sptk.many b0=<Second>;;'))
        self.assertEqual(Properties.call_problems(stream, 'First'), [])
        self.assertEqual(Properties.order_problems(stream, ('First', 'Second'), ('First', 'Second')), [])
        self.assertNotEqual(Properties.order_problems(stream, ('Second', 'First'), ('Second', 'First')), [])
        self.assertEqual(Properties.branch_references(stream, 'First+0x10'), [])


class ExtentTests(unittest.TestCase):
    HELPER = bundles(('MII', 'mov r14=r1', 'mov r15=r0;;', 'nop.i 0x0'),
                     ('MII', 'st8 [r32]=r14,8;;', 'st8 [r32]=r15', 'nop.i 0x0;;'),
                     ('MIB', 'nop.m 0x0', 'nop.i 0x0', 'br.ret.sptk.many b0;;'), start=0x80)
    PADDING = bundles(('MMI', 'nop.m 0x0', 'nop.m 0x0', 'nop.i 0x0'), start=0xb0)

    def leaf(self, size, limit, instructions):
        return Leaf('H', 0x80, size, limit, max(0x80 + size, limit or 0), tuple(instructions), ())

    def test_exact_extent_with_padding_passes(self):
        self.assertEqual(Properties.leaf_extent_problems('H', self.leaf(0x30, 0xc0, self.HELPER + self.PADDING)), [])

    def test_truncated_size_is_reported(self):
        # `.size` evaluated before GAS emitted the last bundles.
        problems = Properties.leaf_extent_problems('H', self.leaf(0x10, 0xb0, self.HELPER))
        self.assertTrue(any('falls off the end' in problem for problem in problems))
        self.assertTrue(any('misses 3 instructions' in problem for problem in problems))

    def test_trailing_padding_inside_the_extent_is_reported(self):
        problems = Properties.leaf_extent_problems('H', self.leaf(0x40, 0xc0, self.HELPER + self.PADDING))
        self.assertTrue(any('ends 0x10 bytes after' in problem for problem in problems))

    def test_section_tail_limit_catches_an_undersized_last_helper(self):
        leaf = self.leaf(0x30, 0xb0, self.HELPER)
        self.assertEqual(Properties.leaf_extent_problems('H', leaf), [])
        self.assertTrue(Properties.leaf_extent_problems('H', dataclasses.replace(leaf, size=0x20)))

    def test_every_mutant_is_rejected(self):
        leaf = self.leaf(0x30, 0xc0, self.HELPER + self.PADDING)
        self.assertEqual(all_rejected(self, lambda mutant: Properties.leaf_extent_problems('H', mutant), leaf,
                                      Mutate.leaf_extent_mutants(leaf)), 5)
        self.assertEqual(leaf.code.instructions, tuple(self.HELPER))


def all_rejected(test, judge, facts, mutants) -> int:
    count = 0
    for description, mutant in mutants:
        with test.subTest(mutant=description):
            test.assertNotEqual(mutant, facts)
            test.assertTrue(judge(mutant), description)
        count += 1
    return count


class TilingTests(unittest.TestCase):
    def test_extent(self):
        code = [Instruction(0x100, 'mov r8=r0', 'MII', 0), Instruction(0x106, 'nop.i 0x0', 'MII', 1),
                Instruction(0x10c, 'nop.i 0x0', 'MII', 2), Instruction(0x110, 'br.ret.sptk.many b0', 'MIB', 0)]
        self.assertEqual(Properties.extent_problems(code, 0x100, 0x20), [])
        self.assertTrue(Properties.extent_problems(code, 0x100, 0x30))
        self.assertTrue(Properties.extent_problems(code, 0x100, 0x18))

    def test_sections_and_undefined(self):
        self.assertEqual(Properties.forbidden_section_problems(['.text', '.opd'], 'Ia64', linked=True), [])
        self.assertTrue(Properties.forbidden_section_problems(['.opd'], 'Ia64', linked=False))
        self.assertTrue(Properties.forbidden_section_problems(['.eh_frame'], 'Amd64', linked=True))
        self.assertEqual(Properties.undefined_problems(['memcpy'], ['memcpy', 'memset']), [])
        self.assertTrue(Properties.undefined_problems(['memcpy'], ['memcpy', 'memset'], exact=True))

    def test_relocation_mutants_all_differ(self):
        actual = (RelocationFact(1, 'IMM64', 'AsteriaIa64StartKernelContext', 0),)
        mutants = list(Mutate.relocation_mutants(actual, ('AsteriaIa64SwitchKernelContext',)))
        self.assertEqual(len(mutants), 8)
        for description, mutant in mutants:
            self.assertNotEqual(Properties.multiset_problems(mutant, actual, 'relocation'), [], description)


class FunctionContractTests(unittest.TestCase):
    SECTION = Section(1, '.text', SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, 0, 0x40, 0x100, 0, 0, 32, 0)

    def facts(self, **changes):
        symbol = dataclasses.replace(Symbol('F', 0x20, 0x40, STT_FUNC, STB_GLOBAL, STV_DEFAULT, 1), **changes)
        return (Properties.SymbolFacts(symbol, self.SECTION, True),)

    def test_exported_contract(self):
        contract = Properties.FunctionContract(Properties.EXPORTED, alignment=16)
        self.assertEqual(contract.problems(self.facts(), 'F'), [])
        for changes in ({'binding': STB_LOCAL}, {'size': 0}, {'value': 0x28}, {'value': 0xE0}):
            self.assertTrue(contract.problems(self.facts(**changes), 'F'), changes)
        writable = dataclasses.replace(self.facts()[0], section=dataclasses.replace(
            self.SECTION, flags=self.SECTION.flags | SHF_WRITE))
        self.assertTrue(contract.problems((writable,), 'F'))
        self.assertTrue(contract.problems(self.facts() * 2, 'F'))

    def test_linkage_sets(self):
        for linkage, accepted, rejected in (
                (Properties.INTERNAL, ((STB_LOCAL, STV_DEFAULT), (STB_GLOBAL, STV_HIDDEN)),
                 ((STB_GLOBAL, STV_DEFAULT), (STB_WEAK, STV_DEFAULT), (STB_GLOBAL, STV_PROTECTED))),
                (Properties.CALLABLE, ((STB_GLOBAL, STV_DEFAULT), (STB_GLOBAL, STV_HIDDEN)),
                 ((STB_LOCAL, STV_DEFAULT),))):
            contract = Properties.FunctionContract(linkage, alignment=16)
            for binding, visibility in accepted:
                self.assertEqual(contract.problems(self.facts(binding=binding, visibility=visibility), 'F'), [])
            for binding, visibility in rejected:
                self.assertTrue(contract.problems(self.facts(binding=binding, visibility=visibility), 'F'))

    def test_every_mutant_is_rejected(self):
        contract = Properties.FunctionContract(Properties.INTERNAL, alignment=16, size_multiple=16, section='.text')
        for facts in (self.facts(binding=STB_LOCAL), self.facts(visibility=STV_HIDDEN)):
            count = all_rejected(self, lambda mutant: contract.problems(mutant, 'F'), facts, contract.mutants(facts))
            self.assertEqual(count, 20)


class BundleTests(unittest.TestCase):
    ENTRY_ADDRESS = 0x400000000018d940

    def test_movl_immediates(self):
        for raw, value in (('05000000010008100000000002c00167', 0x10080ee000),
                           ('05000000010000000000000002000068', 0x8000000000000000),
                           ('05000000010000000000402000000360', 0x4000000000180000)):
            self.assertEqual(Ia64.movl_immediate(bytes.fromhex(raw)), value)

    def test_branch_targets(self):
        call = bytes.fromhex('11000000010000000002000018230050')
        self.assertEqual(Ia64.branch_target(call, 2, self.ENTRY_ADDRESS + 0xd30), 0x4000000000190980)
        backward = bytes.fromhex('110000000100000000020000f0ffff48')
        self.assertEqual(Ia64.branch_target(backward, 2, self.ENTRY_ADDRESS + 0x1180), self.ENTRY_ADDRESS + 0x1170)
        self_loop = bytes.fromhex('11000000010000000002000000000040')
        self.assertEqual(Ia64.branch_target(self_loop, 2, 0x1230), 0x1230)
        self.assertIsNone(Ia64.branch_target(bytes(16), 2, 0x1000))
        loop = int.from_bytes(self_loop, 'little')
        for displacement in (0x10, -0x10, 0xfffff0, -0x1000000):
            immediate = (displacement >> 4) & ((1 << 21) - 1)
            slot = (loop >> 87) & ~((0xfffff << 13) | (1 << 36)) | (immediate & 0xfffff) << 13 | (immediate >> 20) << 36
            raw = ((loop & ((1 << 87) - 1)) | slot << 87).to_bytes(16, 'little')
            self.assertEqual(Ia64.branch_target(raw, 2, 0x4000000000100000), 0x4000000000100000 + displacement)
        with self.assertRaises(ValueError):
            Ia64.bundle_slots(b'\0' * 15)

    def test_operand_split_and_render(self):
        for text in ('st8 [r16]=r18,8', 'br.call.sptk.many b0=b6', 'movl r14=<reloc:IMM64 Symbol>', 'rfi',
                     'mov pr=r31,0xfffffffffffffffe', 'cmpxchg8.acq r16=[r17],r25,ar.ccv'):
            self.assertEqual(Ia64.render(*Ia64.split(text)), text)
        self.assertEqual(Ia64.split('st8 [r16]=r18,8'), ('st8', ['[r16]'], ['r18', '8']))


HEADER = """
constexpr Foundation::Size LegacySizeBytes{
    56U};
constexpr Foundation::UInt64 CorridorBytes{16U * 1024U};
inline constexpr Foundation::UInt64 Limit{Foundation::UInt64{1U} << 61U};
struct Evidence { UInt64 Sequence; UInt64 Acknowledged; };
static_assert(__builtin_offsetof(
    Evidence,
    Sequence) == 0U);
static_assert(__builtin_offsetof(Evidence, Acknowledged)
    == LegacySizeBytes);
static_assert(__builtin_offsetof(Frame, Floating) == 1'024U);
static_assert(
    __builtin_offsetof(Frame, General) + (15U * 8U)
    == 488U);
static_assert(sizeof(Evidence) == 0x10ULL, "two words");
"""


class LayoutTests(unittest.TestCase):
    def test_cpp_expression_line_breaks_and_comments_preserve_constant_values(self):
        text = '''constexpr Foundation::UInt64 First{1U << 4U};
constexpr Foundation::UInt64 Mask{First |
    /* the second flag */ (1U << 5U)};
constexpr Foundation::UInt64 Shifted = Mask
    << 2U;'''
        self.assertEqual(Layout.constants(text), {'First': 16, 'Mask': 48, 'Shifted': 192})
        self.assertIsNone(Layout.evaluate('1U\n2U'))

    def test_offsets_sizes_and_constants(self):
        self.assertEqual(Layout.offsets(HEADER, 'Evidence'), {'Sequence': 0, 'Acknowledged': 56})
        self.assertEqual(Layout.offsets(HEADER, 'Frame'), {'Floating': 1024})
        self.assertEqual(Layout.size(HEADER, 'Evidence'), 16)
        self.assertEqual(Layout.constants(HEADER), {'LegacySizeBytes': 56, 'CorridorBytes': 16384, 'Limit': 1 << 61})
        self.assertEqual(Layout.evaluate('~Foundation::UInt64{0U}'), (1 << 64) - 1)

    def test_missing_assertions_are_errors(self):
        with self.assertRaises(Layout.LayoutError):
            Layout.offsets(HEADER, 'Evidence', ('Missing',))
        with self.assertRaises(Layout.LayoutError):
            Layout.size(HEADER, 'Frame')

    def test_enumerators(self):
        text = ('constexpr Foundation::UInt8 Base{4U};\n'
                'enum class State : Foundation::UInt8\n{\n    None, // first\n    /// documented\n    Armed,\n'
                '    Faulted = Base + 1U,\n    Last,\n};\n'
                'enum class Mask : Foundation::UInt64 { A = 1U << 0U, B = 1U << 3U };\n'
                'enum class Twice { X };\nenum class Twice { Y };\n')
        self.assertEqual(Layout.enumerators(text, 'State'), {'None': 0, 'Armed': 1, 'Faulted': 5, 'Last': 6})
        self.assertEqual(Layout.enumerators(text, 'Mask'), {'A': 1, 'B': 8})
        for name in ('Missing', 'Twice'):
            with self.assertRaises(Layout.LayoutError):
                Layout.enumerators(text, name)
        with self.assertRaises(Layout.LayoutError):
            Layout.enumerators('enum class Bad { A = Unknown };', 'Bad')


class GoldenTests(unittest.TestCase):
    def test_round_trip_and_compare(self):
        record = Golden.GoldenRecord('GNU objdump 2.46.1', 'F', None, 0x20, ['O0', 'O2', 'firmware'],
                                     ['0:1:mov r8=r0', '1:1:br.ret.sptk.many b0'])
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'F.dis'
            path.write_text(record.render())
            parsed = Golden.parse(path)
        self.assertEqual(parsed, record)
        self.assertEqual(Golden.compare(record, record.lines, 0x20, 'O2', path), [])
        self.assertTrue(Golden.compare(record, record.lines, 0x30, 'O2', path))
        self.assertTrue(Golden.compare(record, record.lines[:1], 0x20, 'O2', path))

    def test_range_header(self):
        record = Golden.GoldenRecord('v', None, ('Start', 'End'), None, ['O2'], [])
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'R.dis'
            path.write_text(record.render())
            self.assertEqual(Golden.parse(path).range, ('Start', 'End'))
            path.write_text('# golden: 2\n# binutils: v\n# size: any\n# artifacts: O2\n')
            with self.assertRaises(Golden.GoldenError):
                Golden.parse(path)

    def test_conditional_artifact(self):
        self.assertEqual(Golden.split_condition('firmware[ia64_native_preemption_probe]'),
                         ('firmware', 'ia64_native_preemption_probe'))
        self.assertEqual(Golden.split_condition('O2:user_elf'), ('O2:user_elf', None))
        for malformed in ('[option]', 'firmware[]'):
            with self.assertRaises(Golden.GoldenError):
                Golden.split_condition(malformed)


class RunnerTests(unittest.TestCase):
    class Positive(Checks.Property):
        name = 'Positive'

        def subjects(self, context):
            return ['subject']

        def extract(self, context, subject):
            return 5

        def verify(self, facts):
            return [] if facts > 0 else ['not positive']

    def test_rejected_mutants_are_counted(self):
        prop = self.Positive()
        prop.mutants = lambda facts: [('zero', 0), ('negative', -1)]
        outcome = Checks._run_property(prop, None)
        self.assertEqual((outcome.problems, outcome.mutants_rejected), ([], 2))

    def test_passing_facts_are_summarized(self):
        prop = self.Positive()
        prop.summary = lambda facts: 'value %d' % facts
        self.assertEqual(Checks._run_property(prop, None).notes, ['subject: value 5'])

    def test_item_properties_prefix_item_names(self):
        class Items(Checks.ItemProperty):
            def item_problems(self, item):
                return [] if item > 0 else ['not positive']

            def item_mutants(self, item):
                yield 'negated', -item
        prop = Items()
        self.assertEqual(prop.verify({'a': 1, 'b': -1}), ['b: not positive'])
        self.assertEqual(list(prop.mutants({'a': 1})), [('a: negated', {'a': -1})])

    def test_accepted_or_identical_mutants_fail(self):
        prop = self.Positive()
        prop.mutants = lambda facts: [('larger', 6), ('same', 5)]
        outcome = Checks._run_property(prop, None)
        self.assertEqual(len(outcome.problems), 2)
        self.assertIn('accepted', outcome.problems[0])
        self.assertIn('does not change', outcome.problems[1])


class ContextTests(unittest.TestCase):
    def test_object_paths_map_to_repository_sources(self):
        base = pathlib.Path('/b/Source/CMakeFiles')
        self.assertEqual(source_of_object(base / 'AsteriaKernelObjects.dir/Kernel/Ipc/Endpoint.cpp.obj'),
                         'Source/Kernel/Ipc/Endpoint.cpp')
        self.assertEqual(source_of_object(base / 'X.dir/__/Tests/Target/AbiContractTests.cpp.o'),
                         'Tests/Target/AbiContractTests.cpp')
        self.assertIsNone(source_of_object(pathlib.Path('/b/NucleusCore.o')))

    def test_the_test_firmware_is_a_variant_with_the_guest_objects(self):
        kernel = '/b/Source/CMakeFiles/AsteriaKernelObjects.dir/./Kernel/Ipc/Endpoint.cpp.obj'
        guest = '/b/Source/CMakeFiles/AsteriaGuestObjects.dir/./__/Tests/Guest/GuestTests.cpp.obj'
        with tempfile.TemporaryDirectory() as directory:
            build = pathlib.Path(directory)
            (build / 'AsteriaVerify.json').write_text(json.dumps({
                'format': 2, 'isa': 'Amd64', 'root': '/r', 'compile_checks': False, 'stack_budget_bytes': 512,
                'tools': {}, 'configuration': {}, 'artifacts': {'firmware-test:user_elf': '/b/GuestTests.elf'},
                'objects': {'firmware': kernel, 'firmware-test': kernel, 'firmware-test-user': guest}}))
            context = TargetContext.load(build)
        self.assertEqual(context.variants, ('firmware', 'firmware-test'))
        self.assertEqual(list(context.objects('firmware-test-user')), ['Tests/Guest/GuestTests.cpp'])
        self.assertEqual(list(context.objects('firmware-test')), ['Source/Kernel/Ipc/Endpoint.cpp'])


if __name__ == '__main__':
    unittest.main()
