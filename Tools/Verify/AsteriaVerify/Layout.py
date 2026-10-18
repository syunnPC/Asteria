# SPDX-License-Identifier: Apache-2.0
"""Structure layouts and constants stated by C++ sources.

Checks take offsets, sizes and reviewed constants from the sources instead of
literals, so a layout change that hand-written assembly does not follow is
reported. Two forms are read:

- `static_assert(<expression> == <value>)`: `assertions` maps the
  expression, with its white space removed, to the value, for example
  `__builtin_offsetof(Ia64ExternalPreemptionFrame,Predicates)` or
  `sizeof(Ia64KernelRegisterState)`;
- `constexpr <type> Name{<expression>};` and `constexpr <type> Name = <expression>;`:
  `constants` maps each name whose expression evaluates to an integer;
- `enum class Name: <type> { A, B = <expression>,... };`: `enumerators`
  maps each enumerator of one enumeration to its value.

Values are integer literals (digit separators and u/l suffixes allowed) or
integer expressions over literals, casts such as `Foundation::UInt64{1U}`
and constants defined earlier in the same text.
"""
from __future__ import annotations

import ast
import functools
import pathlib
import re

_ASSERTION = re.compile(r'static_assert\s*\((?P<expression>[^;]*?)==\s*(?P<value>[\w\']+)\s*[,)]', re.S)
_CONSTANT = re.compile(r'constexpr\s+(?:[\w:]+\s+)+?(?P<name>\w+)\s*(?:(?P<brace>\{)|=(?P<assigned>[^;{}]+);)')
_LITERAL = re.compile(r'\b(0[xX][0-9A-Fa-f]+|\d+)[uUlL]*\b')
_CAST = re.compile(r'[A-Za-z_][\w:]*\s*\{')
_ENUMERATION = re.compile(r'\benum\s+class\s+(?P<name>\w+)\s*(?::\s*[\w:]+\s*)?\{(?P<body>[^{}]*)\}\s*;')
_COMMENT = re.compile(r'//[^\n]*|/\*.*?\*/', re.S)
_OPERATORS = {ast.Add: lambda a, b: a + b, ast.Sub: lambda a, b: a - b, ast.Mult: lambda a, b: a * b,
              ast.LShift: lambda a, b: a << b, ast.RShift: lambda a, b: a >> b, ast.BitOr: lambda a, b: a | b,
              ast.BitAnd: lambda a, b: a & b, ast.BitXor: lambda a, b: a ^ b}


class LayoutError(Exception):
    """Raised when a source no longer states a value a rule relies on."""


@functools.lru_cache(maxsize=None)
def read(path: str | pathlib.Path) -> str:
    return pathlib.Path(path).read_text()


def evaluate(expression: str, names: dict[str, int] | None = None) -> int | None:
    """The value of an integer constant expression, or None."""
    text = _LITERAL.sub(r'\1', _COMMENT.sub('', expression).replace("'", ''))
    text = _CAST.sub('(', re.sub(r'static_cast<[^>]*>', '', text)).replace('}', ')')
    try:
        # C++ permits a newline around every operator in a constant expression.
        tree = ast.parse('(' + text.strip() + ')', mode='eval')
    except SyntaxError:
        return None

    def value(node):
        if isinstance(node, ast.Constant) and isinstance(node.value, int):
            return node.value
        if isinstance(node, ast.Name):
            return (names or {}).get(node.id)
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.Invert):
            operand = value(node.operand)
            return None if operand is None else ~operand & ((1 << 64) - 1)
        if isinstance(node, ast.BinOp) and type(node.op) in _OPERATORS:
            left, right = value(node.left), value(node.right)
            return None if left is None or right is None else _OPERATORS[type(node.op)](left, right)
        return None
    return value(tree.body)


def constants(text: str) -> dict[str, int]:
    """Every constexpr integer constant of `text` (see the module docstring)."""
    result: dict[str, int] = {}
    for match in _CONSTANT.finditer(text):
        if match.group('brace'):
            depth, end = 1, match.end()
            while depth and end < len(text):
                depth += {'{': 1, '}': -1}.get(text[end], 0)
                end += 1
            expression = text[match.end():end - 1]
        else:
            expression = match.group('assigned')
        value = evaluate(expression, result)
        if value is not None:
            result[match.group('name')] = value
    return result


def enumerators(text: str, type_name: str) -> dict[str, int]:
    """The enumerators of `enum class <type_name>` in `text` and their values:
    an enumerator without an initializer is one more than the previous one."""
    matches = [match for match in _ENUMERATION.finditer(text) if match.group('name') == type_name]
    if len(matches) != 1:
        raise LayoutError('%d definitions of enum class %s, expected one' % (len(matches), type_name))
    names = constants(text)
    result: dict[str, int] = {}
    following = 0
    for item in _COMMENT.sub('', matches[0].group('body')).split(','):
        name, assigned, expression = item.strip().partition('=')
        name = name.strip()
        if not name:
            continue
        value = evaluate(expression, {**names, **result}) if assigned else following
        if value is None:
            raise LayoutError('cannot evaluate %s::%s = %s' % (type_name, name, expression.strip()))
        result[name] = value
        following = value + 1
    return result


def assertions(text: str) -> dict[str, int]:
    """Asserted values by expression (see the module docstring)."""
    names = constants(text)
    result = {}
    for match in _ASSERTION.finditer(text):
        value = evaluate(match.group('value'), names)
        if value is not None:
            result[re.sub(r'\s+', '', match.group('expression'))] = value
    return result


def offsets(text: str, type_name: str, fields=None) -> dict[str, int]:
    """The asserted `__builtin_offsetof` of the fields of `type_name`; every
    field in `fields` (or at least one) must be asserted."""
    pattern = re.compile(r'__builtin_offsetof\(%s,(\w+)\)' % re.escape(type_name))
    found = {match.group(1): value for expression, value in assertions(text).items()
             if (match := pattern.fullmatch(expression))}
    missing = sorted(set(fields or ()) - set(found))
    if missing or not found:
        raise LayoutError('no offsetof assertion for %s.%s' % (type_name, ', '.join(missing) or '*'))
    return found if fields is None else {name: found[name] for name in fields}


def size(text: str, type_name: str) -> int:
    """The asserted `sizeof(type_name)`."""
    value = assertions(text).get('sizeof(%s)' % type_name)
    if value is None:
        raise LayoutError('no sizeof assertion for %s' % type_name)
    return value
