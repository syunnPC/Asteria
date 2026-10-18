# SPDX-License-Identifier: Apache-2.0
"""Source mutant generation.

Each mutant replaces one span of the original text. The generator works on
tokens (see Tokenizer) and is deliberately conservative: an operator is only
mutated where it is unambiguously a binary expression operator in this
codebase's style (whitespace on both sides, an operand before it), and no
mutation is made inside preprocessor directives, comments, literals,
static_assert/alignas/sizeof/alignof/decltype/noexcept/asm operands,
attributes, template parameter lists, operator names, or array bounds of
declarations.

Operators (group: mutation):
  relational a < b -> a <= b (boundary), a >= b (negation);
                    <= -> <, >; > -> >=, <=; >= -> >, <
  equality == <-> !=
  logical && <-> ||
  arithmetic binary + <-> - (not unary, ++, --, +=, -=)
  literal integer literal +1 and -1 (0 only +1), base, case,
                    width, digit separators and suffix preserved
  boolean true <-> false
  negate-condition if (c) / while (c) -> if (!(c)) / while (!(c)); skipped for
                    `if constexpr`, declarations, and conditions that are a
                    single comparison or a single `!` operand (their negation
                    equals a relational, equality or remove-not mutant)
  remove-not !x -> x
"""
from __future__ import annotations

import dataclasses
import re

from AsteriaMutation.Tokenizer import CHARACTER, IDENTIFIER, NUMBER, PUNCTUATOR, STRING, Token, code_tokens, \
    tokenize

OPERATOR_GROUPS = ('relational', 'equality', 'logical', 'arithmetic', 'literal', 'boolean',
                   'negate-condition', 'remove-not')

RELATIONAL = {
    '<': (('relational-boundary', '<='), ('relational-negation', '>=')),
    '<=': (('relational-boundary', '<'), ('relational-negation', '>')),
    '>': (('relational-boundary', '>='), ('relational-negation', '<=')),
    '>=': (('relational-boundary', '>'), ('relational-negation', '<')),
}
SWAPS = {
    '==': ('equality', '!='),
    '!=': ('equality', '=='),
    '&&': ('logical', '||'),
    '||': ('logical', '&&'),
    '+': ('arithmetic', '-'),
    '-': ('arithmetic', '+'),
}
# Keywords after which + - < > cannot be binary operators.
NON_OPERAND_KEYWORDS = frozenset({
    'return', 'case', 'throw', 'co_return', 'co_yield', 'co_await', 'delete', 'new', 'sizeof', 'alignof',
    'else', 'do', 'goto', 'and', 'or', 'not', 'xor', 'bitand', 'bitor', 'compl', 'operator', 'template',
})
# Keywords whose parenthesized operand is never mutated.
OPAQUE_OPERAND_KEYWORDS = frozenset({
    'static_assert', 'alignas', 'alignof', 'sizeof', 'decltype', 'noexcept', 'asm', '__asm__', '__asm',
    '__attribute__', '__builtin_offsetof', 'offsetof',
})
INTEGER_LITERAL = re.compile(
    r"^(?P<prefix>0[xX]|0[bB])?(?P<digits>[0-9a-fA-F']+)(?P<suffix>(?:[uU](?:ll|LL|l|L|z|Z)?|(?:ll|LL|l|L|z|Z)[uU]?)?)$")


@dataclasses.dataclass(frozen=True)
class Mutant:
    identifier: str
    operator: str
    group: str
    start: int
    end: int
    original: str
    replacement: str
    line: int
    column: int

    @property
    def description(self) -> str:
        return '%s -> %s' % (_shorten(self.original), _shorten(self.replacement) or '(removed)')


def _shorten(text: str, limit: int = 24) -> str:
    flat = ' '.join(text.split())
    return flat if len(flat) <= limit else flat[:limit - 3] + '...'


def apply(text: str, mutant: Mutant) -> str:
    """Returns `text` with the mutant applied."""
    if text[mutant.start:mutant.end] != mutant.original:
        raise ValueError('mutant %s does not match the source text' % mutant.identifier)
    return text[:mutant.start] + mutant.replacement + text[mutant.end:]


def _match(tokens: list[Token], index: int, opening: str, closing: str) -> int:
    depth = 0
    for position in range(index, len(tokens)):
        if tokens[position].text == opening:
            depth += 1
        elif tokens[position].text == closing:
            depth -= 1
            if depth == 0:
                return position
    return len(tokens) - 1


def _match_template(tokens: list[Token], index: int) -> int:
    """Index of the `>` closing the template parameter list opened at `index`."""
    depth = 0
    parentheses = 0
    for position in range(index, len(tokens)):
        text = tokens[position].text
        if text in ('(', '[', '{'):
            parentheses += 1
        elif text in (')', ']', '}'):
            parentheses -= 1
        elif parentheses == 0 and text == '<':
            depth += 1
        elif parentheses == 0 and text in ('>', '>>'):
            depth -= len(text)
            if depth <= 0:
                return position
    return len(tokens) - 1


def _is_operand_end(token: Token) -> bool:
    if token.kind in (NUMBER, STRING, CHARACTER):
        return True
    if token.kind == IDENTIFIER:
        return token.text not in NON_OPERAND_KEYWORDS
    return token.text in (')', ']', '}')


def _opaque_spans(tokens: list[Token]) -> list[tuple[int, int]]:
    """Token index ranges [first, last] that no mutation may touch."""
    spans: list[tuple[int, int]] = []
    declaration_brackets: set[int] = set()
    for index, token in enumerate(tokens):
        following = tokens[index + 1] if index + 1 < len(tokens) else None
        if token.kind == IDENTIFIER and token.text in OPAQUE_OPERAND_KEYWORDS:
            opening = index + 1
            while opening < len(tokens) and tokens[opening].text in ('volatile', '__volatile__', 'goto', 'inline'):
                opening += 1
            if opening < len(tokens) and tokens[opening].text == '(':
                spans.append((index, _match(tokens, opening, '(', ')')))
        elif token.text == 'template' and following is not None and following.text == '<':
            spans.append((index, _match_template(tokens, index + 1)))
        elif token.text == 'operator' and following is not None:
            last = index + 1
            if following.text in ('(', '[') and index + 2 < len(tokens):
                last = index + 2
            spans.append((index, last))
        elif (token.text == '[' and following is not None and following.text == '['
              and following.start == token.end):
            spans.append((index, _match(tokens, index, '[', ']')))
        elif token.text == '[' and index >= 2:
            before, name = tokens[index - 2], tokens[index - 1]
            is_declaration = (
                (name.kind == IDENTIFIER and before.kind == IDENTIFIER and before.text not in NON_OPERAND_KEYWORDS)
                or (name.kind == IDENTIFIER and before.text in ('>', '*', '&', '&&'))
                or (name.text == ']' and (index - 1) in declaration_brackets))
            if is_declaration:
                closing = _match(tokens, index, '[', ']')
                declaration_brackets.add(closing)
                spans.append((index, closing))
    return spans


def _format_integer(value: int, match: re.Match[str]) -> str:
    prefix = match.group('prefix') or ''
    digits = match.group('digits')
    plain = digits.replace("'", '')
    if prefix.lower() == '0x':
        body = '%x' % value
        if any(c in 'ABCDEF' for c in plain) or not any(c in 'abcdef' for c in plain):
            body = body.upper()
    elif prefix.lower() == '0b':
        body = format(value, 'b')
    elif len(plain) > 1 and plain.startswith('0'):
        body = '0' + format(value, 'o')
    else:
        body = str(value)
    if prefix and len(body) < len(plain):
        body = body.rjust(len(plain), '0')
    if "'" in digits and len(body) == len(plain):
        # Re-insert each separator with the same number of digits to its right.
        separated_counts = set()
        count = 0
        for character in reversed(digits):
            if character == "'":
                separated_counts.add(count)
            else:
                count += 1
        characters = []
        for count, character in enumerate(reversed(body)):
            if count in separated_counts:
                characters.append("'")
            characters.append(character)
        body = ''.join(reversed(characters))
    return prefix + body + match.group('suffix')


def _integer_value(match: re.Match[str]) -> int | None:
    prefix = (match.group('prefix') or '').lower()
    plain = match.group('digits').replace("'", '')
    try:
        if prefix == '0x':
            return int(plain, 16)
        if prefix == '0b':
            return int(plain, 2)
        if len(plain) > 1 and plain.startswith('0'):
            return int(plain, 8)
        return int(plain, 10)
    except ValueError:
        return None


def _literal_mutations(token: Token) -> list[tuple[str, str]]:
    match = INTEGER_LITERAL.match(token.text)
    if match is None:
        return []
    value = _integer_value(match)
    if value is None:
        return []
    mutations = [('literal-increment', _format_integer(value + 1, match))]
    if value > 0:
        mutations.append(('literal-decrement', _format_integer(value - 1, match)))
    return mutations


def _negatable_condition(text: str, tokens: list[Token], index: int) -> tuple[int, int] | None:
    """Span of the condition of the `if`/`while` at `index`, or None.

    `if constexpr`, declarations and assignments in conditions are skipped.
    A condition that is a single comparison or a single `!` operand is also
    skipped: its negation equals a relational, equality or remove-not mutant.
    """
    opening = index + 1
    if opening >= len(tokens) or tokens[opening].text != '(':
        return None
    closing = _match(tokens, opening, '(', ')')
    first = opening + 1
    depth = 0
    for position in range(opening + 1, closing):
        if tokens[position].text in ('(', '[', '{'):
            depth += 1
        elif tokens[position].text in (')', ']', '}'):
            depth -= 1
        elif depth == 0 and tokens[position].text == ';':
            first = position + 1
    condition = tokens[first:closing]
    if not condition or condition[0].text in ('auto', 'const', 'constexpr', 'static'):
        return None
    operators: list[Token] = []
    depth = 0
    for item in condition:
        if item.text in ('(', '[', '{'):
            depth += 1
        elif item.text in (')', ']', '}'):
            depth -= 1
        elif depth == 0 and item.kind == PUNCTUATOR and item.text not in ('.', '->', '::'):
            operators.append(item)
    texts = [item.text for item in operators]
    if '=' in texts:
        return None
    if len(operators) == 1 and texts[0] in ('<', '<=', '>', '>=', '==', '!='):
        single = operators[0]
        if text[single.start - 1].isspace() and text[single.end].isspace():
            return None
    if texts == ['!'] and condition[0].text == '!':
        return None
    return condition[0].start, condition[-1].end


def generate(text: str, groups: tuple[str,...] | None = None) -> list[Mutant]:
    """Returns the mutants of `text`, ordered by position, restricted to `groups`."""
    selected = set(groups or OPERATOR_GROUPS)
    unknown = selected - set(OPERATOR_GROUPS)
    if unknown:
        raise ValueError('unknown operator group(s): %s' % ', '.join(sorted(unknown)))
    tokens = code_tokens(tokenize(text))
    opaque = [False] * len(tokens)
    for first, last in _opaque_spans(tokens):
        for index in range(first, min(last, len(tokens) - 1) + 1):
            opaque[index] = True

    candidates: list[tuple[int, int, str, str, str, str]] = []

    def add(start: int, end: int, operator: str, group: str, replacement: str) -> None:
        if group in selected:
            candidates.append((start, end, operator, group, text[start:end], replacement))

    for index, token in enumerate(tokens):
        if opaque[index]:
            continue
        previous = tokens[index - 1] if index > 0 else None
        spaced = (token.start > 0 and text[token.start - 1].isspace()
                  and token.end < len(text) and text[token.end].isspace())
        if token.kind == PUNCTUATOR:
            binary = spaced and previous is not None and _is_operand_end(previous)
            if token.text in RELATIONAL and binary:
                for operator, replacement in RELATIONAL[token.text]:
                    add(token.start, token.end, operator, 'relational', replacement)
            elif token.text in SWAPS and (binary or (spaced and token.text in ('==', '!=', '&&', '||'))):
                group, replacement = SWAPS[token.text]
                add(token.start, token.end, group, group, replacement)
            elif token.text == '!':
                add(token.start, token.end, 'remove-not', 'remove-not', '')
        elif token.kind == NUMBER:
            glued_template = ((previous is not None and previous.text == '<' and previous.end == token.start)
                              or (index + 1 < len(tokens) and tokens[index + 1].text in ('>', '>>')
                                  and tokens[index + 1].start == token.end))
            if not glued_template:
                for operator, replacement in _literal_mutations(token):
                    add(token.start, token.end, operator, 'literal', replacement)
        elif token.kind == IDENTIFIER and token.text in ('true', 'false'):
            add(token.start, token.end, 'boolean', 'boolean', 'false' if token.text == 'true' else 'true')
        elif token.kind == IDENTIFIER and token.text in ('if', 'while'):
            span = _negatable_condition(text, tokens, index)
            if span is not None:
                start, end = span
                add(start, end, 'negate-condition', 'negate-condition', '!(' + text[start:end] + ')')

    candidates.sort(key=lambda item: (item[0], item[2], item[5]))
    mutants = []
    for number, (start, end, operator, group, original, replacement) in enumerate(candidates, 1):
        line = text.count('\n', 0, start) + 1
        column = start - (text.rfind('\n', 0, start) + 1) + 1
        mutants.append(Mutant('M%04d' % number, operator, group, start, end, original, replacement, line, column))
    return mutants
