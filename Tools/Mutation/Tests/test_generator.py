# SPDX-License-Identifier: Apache-2.0
"""Unit tests of the mutant generator.

Run with: python3 -m unittest discover -s Tests -t. (from Tools/Mutation)
"""
from __future__ import annotations

import unittest

from AsteriaMutation import Generator


def mutations(text: str, groups: tuple[str,...] | None = None) -> list[tuple[str, str, str]]:
    return [(m.operator, m.original, m.replacement) for m in Generator.generate(text, groups)]


def mutated_texts(text: str, groups: tuple[str,...] | None = None) -> list[str]:
    return [Generator.apply(text, mutant) for mutant in Generator.generate(text, groups)]


class OperatorTests(unittest.TestCase):
    def test_relational_boundary_and_negation(self) -> None:
        self.assertEqual(mutations('x = a < b;', ('relational',)),
                         [('relational-boundary', '<', '<='), ('relational-negation', '<', '>=')])
        self.assertEqual(mutations('x = a <= b;', ('relational',)),
                         [('relational-boundary', '<=', '<'), ('relational-negation', '<=', '>')])
        self.assertEqual(mutations('x = a > b;', ('relational',)),
                         [('relational-boundary', '>', '>='), ('relational-negation', '>', '<=')])
        self.assertEqual(mutations('x = a >= b;', ('relational',)),
                         [('relational-boundary', '>=', '>'), ('relational-negation', '>=', '<')])

    def test_equality_logical_and_arithmetic_swaps(self) -> None:
        self.assertEqual(mutations('x = (a == b) && (c != d) || e;', ('equality', 'logical')), [
            ('equality', '==', '!='), ('logical', '&&', '||'), ('equality', '!=', '=='), ('logical', '||', '&&')])
        self.assertEqual(mutations('x = a + b - c;', ('arithmetic',)),
                         [('arithmetic', '+', '-'), ('arithmetic', '-', '+')])

    def test_operator_at_line_start_is_binary(self) -> None:
        self.assertEqual(mutations('return first\n    < second;', ('relational',)),
                         [('relational-boundary', '<', '<='), ('relational-negation', '<', '>=')])
        self.assertEqual(mutations('ok = (a)\n    || (b);', ('logical',)), [('logical', '||', '&&')])

    def test_unary_and_compound_operators_are_not_arithmetic(self) -> None:
        for text in ('x = -y;', 'return - y;', 'x += 1;', 'x -= y;', '++x;', 'x--;', 'p->q;', 'x = (+y);',
                     'case - 1:'):
            with self.subTest(text=text):
                self.assertEqual(mutations(text, ('arithmetic',)), [])

    def test_boolean_literals(self) -> None:
        self.assertEqual(mutations('a = true; b = false;', ('boolean',)),
                         [('boolean', 'true', 'false'), ('boolean', 'false', 'true')])
        self.assertEqual(mutations('bool trueValue{};', ('boolean',)), [])

    def test_integer_literals_keep_base_width_separators_and_suffix(self) -> None:
        cases = {
            'x = 0U;': ['1U'],
            'x = 7;': ['8', '6'],
            'x = 0x0FU;': ['0x10U', '0x0EU'],
            'x = 0xff;': ['0x100', '0xfe'],
            'x = 0XFFFF;': ['0X10000', '0XFFFE'],
            'x = 0b1010;': ['0b1011', '0b1001'],
            'x = 10ULL;': ['11ULL', '9ULL'],
            "x = 0xC000'0080U;": ["0xC000'0081U", "0xC000'007FU"],
            "x = 1'000'000;": ["1'000'001", "999999"],
            'x = 017;': ['020', '016'],
        }
        for text, expected in cases.items():
            with self.subTest(text=text):
                self.assertEqual(sorted(m[2] for m in mutations(text, ('literal',))), sorted(expected))

    def test_non_integer_numbers_are_not_mutated(self) -> None:
        for text in ('x = 1.5;', 'x = 1e5;', 'x = 0x1p3;', 'x = 10_ms;'):
            with self.subTest(text=text):
                self.assertEqual(mutations(text, ('literal',)), [])

    def test_condition_negation(self) -> None:
        self.assertEqual(mutated_texts('if (a && b) { f(); }', ('negate-condition',)),
                         ['if (!(a && b)) { f(); }'])
        self.assertEqual(mutated_texts('while (x || y) {}', ('negate-condition',)), ['while (!(x || y)) {}'])
        self.assertEqual(mutated_texts('if (int v = f(); v || w) {}', ('negate-condition',)),
                         ['if (int v = f(); !(v || w)) {}'])
        self.assertEqual(mutated_texts('if (f(a, b)) {}', ('negate-condition',)), ['if (!(f(a, b))) {}'])

    def test_condition_negation_skips_redundant_and_unsafe_conditions(self) -> None:
        for text in ('if (a < b) {}', 'if (a == b) {}', 'if (!ok) {}', 'if (!(a && b)) {}',
                     'if constexpr (a && b) {}', 'if (auto x = f()) {}', 'if (const T* p = g()) {}',
                     'if (T* p = g()) {}'):
            with self.subTest(text=text):
                self.assertEqual(mutations(text, ('negate-condition',)), [])

    def test_remove_not(self) -> None:
        self.assertEqual(mutated_texts('if (!token.IsValid() || x) {}', ('remove-not',)),
                         ['if (token.IsValid() || x) {}'])
        self.assertEqual(mutations('x = a != b;', ('remove-not',)), [])


class SkipRuleTests(unittest.TestCase):
    def test_template_brackets_are_not_relational_operators(self) -> None:
        for text in ('x = static_cast<UInt8>(y);', 'Result<A, B> r{};', 'template <typename T> struct S;',
                     'A<B<C>> x;', 'std::array<int, 3> values;', 'return Foo<T>::Value;',
                     'x = static_cast<Foundation::UInt16>(a) | b;'):
            with self.subTest(text=text):
                self.assertEqual(mutations(text, ('relational',)), [])

    def test_ambiguous_unspaced_comparisons_are_skipped(self) -> None:
        self.assertEqual(mutations('x = a<b;', ('relational',)), [])
        self.assertEqual(mutations('x = a >b;', ('relational',)), [])

    def test_template_arguments_and_parameter_lists_keep_their_literals(self) -> None:
        self.assertEqual(mutations('using T = Foo<3>;', ('literal',)), [])
        self.assertEqual(mutations('template <int N = 4> struct S;', ('literal', 'relational')), [])

    def test_opaque_operands(self) -> None:
        for text in ('static_assert(A < B + 1, "x");', 'alignas(16U) UInt8 b;', 'n = sizeof(a < b);',
                     '__asm__ volatile("mov %0 = r1" : "=r"(x) : "i"(1 + 2));', 'offsetof(T, a) == 0;'):
            with self.subTest(text=text):
                self.assertEqual([m for m in mutations(text) if m[1] not in ('==', '0')], [])

    def test_attributes_and_operator_names(self) -> None:
        self.assertEqual(mutations('[[nodiscard, gnu::section("a")]] bool operator<(const A& b) const;'), [])
        self.assertEqual(mutations('bool operator==(const A&) const = default;'), [])

    def test_array_bounds_of_declarations(self) -> None:
        self.assertEqual(mutations('UInt8 buffer[16]{};', ('literal',)), [])
        self.assertEqual(mutations('static constexpr UInt64 Table[2][3]{};', ('literal',)), [])
        self.assertEqual(mutations('const char* names[4];', ('literal',)), [])
        self.assertEqual([m[2] for m in mutations('return bytes[1];', ('literal',))], ['0', '2'])
        self.assertEqual([m[2] for m in mutations('x = bytes[0] | y;', ('literal',))], ['1'])

    def test_comments_strings_and_directives_are_never_mutated(self) -> None:
        text = ('#if A < 1 && B\n#endif\n// x < y\n/* a == b */\nconst char* s = "a < b";\n'
                'char c = \'<\';\nauto r = R"(x || y)";\n')
        self.assertEqual(mutations(text), [])


class MutantTests(unittest.TestCase):
    def test_identifiers_are_ordered_and_located(self) -> None:
        mutants = Generator.generate('int f()\n{\n    return a < b;\n}\n', ('relational',))
        self.assertEqual([m.identifier for m in mutants], ['M0001', 'M0002'])
        self.assertEqual([(m.line, m.column) for m in mutants], [(3, 14), (3, 14)])

    def test_apply_rejects_a_mismatched_mutant(self) -> None:
        mutant = Generator.generate('x = a + b;', ('arithmetic',))[0]
        self.assertEqual(Generator.apply('x = a + b;', mutant), 'x = a - b;')
        with self.assertRaises(ValueError):
            Generator.apply('x = a * b;', mutant)

    def test_unknown_operator_group(self) -> None:
        with self.assertRaises(ValueError):
            Generator.generate('x;', ('shift',))


if __name__ == '__main__':
    unittest.main()
