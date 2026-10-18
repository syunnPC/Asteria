# SPDX-License-Identifier: Apache-2.0
"""Unit tests of the conservative C++ tokenizer.

Run with: python3 -m unittest discover -s Tests -t. (from Tools/Mutation)
"""
from __future__ import annotations

import unittest

from AsteriaMutation.Tokenizer import CHARACTER, COMMENT, DIRECTIVE, IDENTIFIER, NUMBER, PUNCTUATOR, STRING, \
    TokenizeError, code_tokens, tokenize


def kinds(text: str) -> list[tuple[str, str]]:
    return [(token.kind, token.text) for token in tokenize(text)]


class TokenizerTests(unittest.TestCase):
    def test_strings_with_escapes_and_prefixes(self) -> None:
        text = 'a = "x < y && \\"z\\"" + u8"q" + L"w";'
        tokens = kinds(text)
        self.assertIn((STRING, '"x < y && \\"z\\""'), tokens)
        self.assertIn((STRING, 'u8"q"'), tokens)
        self.assertIn((STRING, 'L"w"'), tokens)
        self.assertEqual([t for t in tokens if t[1] == '<'], [])

    def test_character_literals(self) -> None:
        tokens = kinds("c = '<' + '\\'' + u'x' + U'y' + L'\\n' + u8'z';")
        characters = [text for kind, text in tokens if kind == CHARACTER]
        self.assertEqual(characters, ["'<'", "'\\''", "u'x'", "U'y'", "L'\\n'", "u8'z'"])

    def test_comments(self) -> None:
        tokens = kinds('a /* x < y */ + b // c == d\n+ e')
        self.assertEqual([text for kind, text in tokens if kind == COMMENT], ['/* x < y */', '// c == d'])
        self.assertEqual([text for kind, text in tokens if kind == PUNCTUATOR], ['+', '+'])

    def test_line_comment_continuation(self) -> None:
        tokens = kinds('// comment \\\n still comment\nx')
        self.assertEqual(tokens, [(COMMENT, '// comment \\\n still comment'), (IDENTIFIER, 'x')])

    def test_raw_strings(self) -> None:
        text = 'a = R"(x < " y)"; b = u8R"d(q )" == )d"; c = LR"(v)";'
        strings = [token.text for token in tokenize(text) if token.kind == STRING]
        self.assertEqual(strings, ['R"(x < " y)"', 'u8R"d(q )" == )d"', 'LR"(v)"'])

    def test_raw_string_with_quote_inside_keeps_following_code(self) -> None:
        tokens = kinds('s = R"x(a)" b)x"; t < u;')
        self.assertIn((STRING, 'R"x(a)" b)x"'), tokens)
        self.assertIn((PUNCTUATOR, '<'), tokens)

    def test_digit_separators_and_suffixes(self) -> None:
        numbers = [text for kind, text in kinds("x = 0xC000'0080U + 1'000'000ULL + 0b1010'0101u + 1e+5;")
                   if kind == NUMBER]
        self.assertEqual(numbers, ["0xC000'0080U", "1'000'000ULL", "0b1010'0101u", '1e+5'])

    def test_preprocessor_directives_with_continuation(self) -> None:
        text = '#if A < 3\n#define M(a) \\\n    ((a) + 1)\nint x = 1 < 2;\n  #  include <x.hpp>\n'
        tokens = kinds(text)
        directives = [text for kind, text in tokens if kind == DIRECTIVE]
        self.assertEqual(directives, ['#if A < 3', '#define M(a) \\\n    ((a) + 1)', '#  include <x.hpp>'])
        self.assertEqual([text for kind, text in tokens if kind == PUNCTUATOR], ['=', '<', ';'])

    def test_hash_inside_expression_is_not_a_directive(self) -> None:
        tokens = kinds('#define S(a) #a\nx = y;')
        self.assertEqual(tokens[0], (DIRECTIVE, '#define S(a) #a'))
        self.assertEqual(tokens[1:], [(IDENTIFIER, 'x'), (PUNCTUATOR, '='), (IDENTIFIER, 'y'), (PUNCTUATOR, ';')])

    def test_maximal_munch(self) -> None:
        punctuators = [text for kind, text in kinds('a<=>b; c->d; e>>=2; f<<g; A<B<C>> h;') if kind == PUNCTUATOR]
        self.assertEqual(punctuators, ['<=>', ';', '->', ';', '>>=', ';', '<<', ';', '<', '<', '>>', ';'])

    def test_locations(self) -> None:
        tokens = tokenize('a\n  b + c')
        self.assertEqual([(t.text, t.line, t.column) for t in tokens], [('a', 1, 1), ('b', 2, 3), ('+', 2, 5),
                                                                       ('c', 2, 7)])

    def test_code_tokens_drop_comments_and_directives(self) -> None:
        self.assertEqual([t.text for t in code_tokens(tokenize('#if 1\nx // y\n/* z */ w\n#endif\n'))], ['x', 'w'])

    def test_unterminated_input_is_rejected(self) -> None:
        for text in ('"abc', "'a", '/* x', 'R"x(abc)"'):
            with self.subTest(text=text):
                with self.assertRaises(TokenizeError):
                    tokenize(text)


if __name__ == '__main__':
    unittest.main()
