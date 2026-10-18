# SPDX-License-Identifier: Apache-2.0
"""Conservative C++ tokenizer for source mutation.

The tokenizer splits a translation unit into tokens with their source
offsets. It recognizes exactly what a mutation must never touch: comments,
preprocessor directives (including backslash continuations), string and
character literals with encoding prefixes and escapes, raw string literals,
and numbers with digit separators. Everything else is an identifier or a
punctuator chosen by maximal munch, so `>>`, `<=>` and `->` are single tokens.
"""
from __future__ import annotations

import dataclasses

IDENTIFIER = 'identifier'
NUMBER = 'number'
STRING = 'string'
CHARACTER = 'character'
PUNCTUATOR = 'punctuator'
COMMENT = 'comment'
DIRECTIVE = 'directive'

# Longest punctuators first so that maximal munch is a prefix search.
PUNCTUATORS = sorted([
    '<=>', '<<=', '>>=', '->*', '...',
    '::', '->', '++', '--', '<<', '>>', '<=', '>=', '==', '!=', '&&', '||',
    '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '##', '.*',
    '{', '}', '[', ']', '(', ')', ';', ':', '?', ',', '.', '<', '>', '+', '-',
    '*', '/', '%', '&', '|', '^', '!', '~', '=', '#',
], key=len, reverse=True)
ENCODING_PREFIXES = ('u8', 'u', 'U', 'L')


@dataclasses.dataclass(frozen=True)
class Token:
    kind: str
    text: str
    start: int
    end: int
    line: int
    column: int


class TokenizeError(ValueError):
    """The text is not a well-formed sequence of C++ preprocessing tokens."""


def _is_identifier_start(character: str) -> bool:
    return character.isalpha() or character == '_'


def _is_identifier_part(character: str) -> bool:
    return character.isalnum() or character == '_'


class _Scanner:
    def __init__(self, text: str) -> None:
        self.text = text
        self.position = 0
        self.line_starts = [0]
        for index, character in enumerate(text):
            if character == '\n':
                self.line_starts.append(index + 1)

    def location(self, offset: int) -> tuple[int, int]:
        low, high = 0, len(self.line_starts) - 1
        while low < high:
            middle = (low + high + 1) // 2
            if self.line_starts[middle] <= offset:
                low = middle
            else:
                high = middle - 1
        return low + 1, offset - self.line_starts[low] + 1

    def at_line_start(self, offset: int) -> bool:
        index = offset - 1
        while index >= 0 and self.text[index] in ' \t\f\v':
            index -= 1
        return index < 0 or self.text[index] == '\n'

    def skip_block_comment(self, offset: int) -> int:
        end = self.text.find('*/', offset + 2)
        if end < 0:
            raise TokenizeError('unterminated block comment at offset %d' % offset)
        return end + 2

    def skip_line_comment(self, offset: int) -> int:
        index = offset
        while index < len(self.text):
            if self.text[index] == '\n' and not self.text.endswith('\\', 0, index):
                return index
            index += 1
        return index

    def skip_directive(self, offset: int) -> int:
        index = offset
        while index < len(self.text):
            character = self.text[index]
            if character == '\n':
                if index > 0 and self.text[index - 1] == '\\':
                    index += 1
                    continue
                return index
            if self.text.startswith('/*', index):
                index = self.skip_block_comment(index)
                continue
            if self.text.startswith('//', index):
                return self.skip_line_comment(index)
            if character in '"\'':
                index = self.skip_quoted(index, character)
                continue
            index += 1
        return index

    def skip_quoted(self, offset: int, quote: str) -> int:
        index = offset + 1
        while index < len(self.text):
            character = self.text[index]
            if character == '\\':
                index += 2
                continue
            if character == quote:
                return index + 1
            if character == '\n':
                break
            index += 1
        raise TokenizeError('unterminated %s literal at offset %d' % (
            'string' if quote == '"' else 'character', offset))

    def skip_raw_string(self, quote: int) -> int:
        open_paren = self.text.find('(', quote + 1)
        delimiter = self.text[quote + 1:open_paren] if open_paren >= 0 else ''
        if open_paren < 0 or len(delimiter) > 16 or any(c in delimiter for c in ' ()\\\t\v\f\n'):
            raise TokenizeError('malformed raw string delimiter at offset %d' % quote)
        closing = ')' + delimiter + '"'
        end = self.text.find(closing, open_paren + 1)
        if end < 0:
            raise TokenizeError('unterminated raw string at offset %d' % quote)
        return end + len(closing)

    def skip_number(self, offset: int) -> int:
        # A pp-number: digits, letters, underscores, periods, digit separators
        # between alphanumerics, and signs after an exponent character.
        index = offset + 1
        while index < len(self.text):
            character = self.text[index]
            previous = self.text[index - 1]
            if _is_identifier_part(character) or character == '.':
                index += 1
            elif character in '+-' and previous in 'eEpP':
                index += 1
            elif (character == "'" and _is_identifier_part(previous)
                  and index + 1 < len(self.text) and _is_identifier_part(self.text[index + 1])):
                index += 1
            else:
                break
        return index


def tokenize(text: str) -> list[Token]:
    """Returns every token of `text` in order, comments and directives included."""
    scanner = _Scanner(text)
    tokens: list[Token] = []

    def emit(kind: str, start: int, end: int) -> None:
        line, column = scanner.location(start)
        tokens.append(Token(kind, text[start:end], start, end, line, column))

    index = 0
    length = len(text)
    while index < length:
        character = text[index]
        if character in ' \t\n\r\f\v':
            index += 1
            continue
        if character == '\\' and text.startswith('\n', index + 1):
            index += 2
            continue
        if text.startswith('//', index):
            end = scanner.skip_line_comment(index)
            emit(COMMENT, index, end)
            index = end
            continue
        if text.startswith('/*', index):
            end = scanner.skip_block_comment(index)
            emit(COMMENT, index, end)
            index = end
            continue
        if character == '#' and scanner.at_line_start(index):
            end = scanner.skip_directive(index)
            emit(DIRECTIVE, index, end)
            index = end
            continue
        if _is_identifier_start(character):
            end = index + 1
            while end < length and _is_identifier_part(text[end]):
                end += 1
            word = text[index:end]
            if end < length and text[end] == '"' and word in tuple(p + 'R' for p in ENCODING_PREFIXES) + ('R',):
                stop = scanner.skip_raw_string(end)
                emit(STRING, index, stop)
                index = stop
                continue
            if end < length and text[end] in '"\'' and word in ENCODING_PREFIXES:
                stop = scanner.skip_quoted(end, text[end])
                emit(STRING if text[end] == '"' else CHARACTER, index, stop)
                index = stop
                continue
            emit(IDENTIFIER, index, end)
            index = end
            continue
        if character.isdigit() or (character == '.' and index + 1 < length and text[index + 1].isdigit()):
            end = scanner.skip_number(index)
            emit(NUMBER, index, end)
            index = end
            continue
        if character in '"\'':
            end = scanner.skip_quoted(index, character)
            emit(STRING if character == '"' else CHARACTER, index, end)
            index = end
            continue
        for punctuator in PUNCTUATORS:
            if text.startswith(punctuator, index):
                emit(PUNCTUATOR, index, index + len(punctuator))
                index += len(punctuator)
                break
        else:
            raise TokenizeError('unexpected character %r at offset %d' % (character, index))
    return tokens


def code_tokens(tokens: list[Token]) -> list[Token]:
    """The tokens that take part in the language grammar (no comments or directives)."""
    return [token for token in tokens if token.kind not in (COMMENT, DIRECTIVE)]
