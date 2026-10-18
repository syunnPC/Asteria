# SPDX-License-Identifier: Apache-2.0
"""Boot entropy facts and the cross-boot authenticator check.

The capability-space validation keys come from entropy the UEFI application
gathers before ExitBootServices.
Two facts make that observable without trusting the kernel's own verdict:

- the application reports which entropy sources it credited as one line
  `ASTERIA:BOOT-ENTROPY:SOURCES:0x<16 hex digits>`, whose bits are the
  `Boot::BootEntropySource` enumerators of Source/Boot/BootEntropy.hpp;
- the guest test program reports every bootstrap capability handle it
  received as the TAP diagnostic
  `# handle.<name>: selector 0x<16 hex> authenticator 0x<16 hex>`.

Checks:

- per boot, `lifecycle.boot-entropy` (`record`): the source line appears
  exactly once and names a credited source, and where the machine profile
  provides an RNG device (`Machine.RNG_DEVICE`) the firmware RNG is among
  them; a test-firmware boot also reports its bootstrap handles;
- across boots of one image, `boot-entropy.distinct-authenticators`
  (`cross_boot`): every handle two boots both report has the same selector
  in both (the kernel mints the bootstrap handles deterministically) and a
  different authenticator. Equal authenticators would mean a repeated key;
  no verdict depends on a particular value.
"""
from __future__ import annotations

import itertools
import pathlib
import re

from AsteriaVm import Machine
from AsteriaVm.Console import USER_PREFIX
from AsteriaVm.Executor import CheckResult

SOURCES_PREFIX = 'ASTERIA:BOOT-ENTROPY:SOURCES:'
_SOURCES = re.compile(r'^ASTERIA:BOOT-ENTROPY:SOURCES:0x(?P<mask>[0-9A-F]{16})$')
_HANDLE = re.compile(r'^# handle\.(?P<name>[a-z]+): selector 0x(?P<selector>[0-9a-f]{16}) '
                     r'authenticator 0x(?P<authenticator>[0-9a-f]{16})$')
_ENUMERATOR = re.compile(r'^\s*(?P<name>[A-Za-z]+) = 1U << (?P<shift>\d+)U,$', re.MULTILINE)
CHECK = 'lifecycle.boot-entropy'
CROSS_BOOT_CHECK = 'boot-entropy.distinct-authenticators'
# The sources that may qualify a seed on their own (the others are mixed only).
CREDITED = ('FirmwareRng', 'ProcessorRng', 'CycleCounterJitter')

Handles = dict[str, tuple[int, int]]


def source_bits(root: pathlib.Path) -> dict[str, int]:
    """The `BootEntropySource` bit of every source, from the C++ header."""
    text = (pathlib.Path(root) / 'Source' / 'Boot' / 'BootEntropy.hpp').read_text()
    body = text[text.index('enum class BootEntropySource'):]
    body = body[:body.index('};')]
    return {match['name']: 1 << int(match['shift']) for match in _ENUMERATOR.finditer(body)}


def sources(lines: list[str]) -> tuple[int | None, list[str]]:
    """(the reported source mask, problems) of one boot's debug lines."""
    reported = [line for line in lines if line.startswith(SOURCES_PREFIX)]
    if len(reported) != 1:
        return None, ['%d boot entropy source lines, expected one' % len(reported)]
    match = _SOURCES.match(reported[0])
    if match is None:
        return None, ['malformed boot entropy source line %r' % reported[0]]
    return int(match['mask'], 16), []


def source_problems(mask: int, bits: dict[str, int], rng_device: bool) -> list[str]:
    known = sum(bits.values())
    credited = sum(bits[name] for name in CREDITED)
    problems = []
    if mask & ~known:
        problems.append('unknown source bits %#x' % (mask & ~known))
    if not mask & credited:
        problems.append('no credited entropy source (sources %#x)' % mask)
    if rng_device and not mask & bits['FirmwareRng']:
        problems.append('the machine has an RNG device but the firmware RNG was not credited (sources %#x)' % mask)
    return problems


def handles(lines: list[str]) -> Handles:
    """name -> (selector, authenticator) of the handles the guest reported."""
    found: Handles = {}
    for line in lines:
        if not line.startswith(USER_PREFIX):
            continue
        match = _HANDLE.match(line[len(USER_PREFIX):])
        if match:
            found[match['name']] = (int(match['selector'], 16), int(match['authenticator'], 16))
    return found


def record(context) -> None:
    """The per-boot check; keeps the guest's handles for `cross_boot`."""
    lines = context.console.lines
    mask, problems = sources(lines)
    if mask is not None:
        problems += source_problems(mask, source_bits(context.target.root),
                                    context.case.isa in Machine.RNG_DEVICE)
    reported = handles(lines)
    if context.case.guest and getattr(context.case, "reaches_userspace", True) and not reported:
        problems.append('the guest reported no bootstrap handle')
    context.result.handles = reported
    context.check_problems(CHECK, problems)


def distinct_problems(boots: dict[str, Handles]) -> list[str]:
    """Problems of one image's boots (case name -> handles): a handle whose
    selector differs between two boots, or whose authenticator repeats."""
    problems = []
    for (first, first_handles), (second, second_handles) in itertools.combinations(sorted(boots.items()), 2):
        for name in sorted(first_handles.keys() & second_handles.keys()):
            (selector, authenticator), (other_selector, other_authenticator) = (first_handles[name],
                                                                                second_handles[name])
            if selector != other_selector:
                problems.append('%s: handle %s has selector %#x in one boot and %#x in the other' % (
                    '/'.join((first, second)), name, selector, other_selector))
            elif authenticator == other_authenticator:
                problems.append('%s: handle %s has the same authenticator in both boots' % (
                    '/'.join((first, second)), name))
    return problems


def cross_boot(results) -> list[str]:
    """Adds the cross-boot check to every boot of an image that was booted
    at least twice with the guest reporting its handles; returns one console
    line per image."""
    groups: dict[tuple[str, str], list] = {}
    for result in results:
        if getattr(result, 'handles', None):
            groups.setdefault((result.case.preset, result.case.firmware), []).append(result)
    lines = []
    for (preset, firmware), boots in sorted(groups.items()):
        if len(boots) < 2:
            continue
        problems = distinct_problems({result.case.name: result.handles for result in boots})
        detail = '\n'.join(problems) or 'authenticators differ across %s' % ', '.join(
            sorted(result.case.name for result in boots))
        for result in boots:
            result.checks.append(CheckResult(CROSS_BOOT_CHECK, not problems, detail))
        lines.append('%s boot entropy %s/%s: %d boots, %s' % (
            'FAIL' if problems else 'PASS', preset, firmware, len(boots),
            '%d repeated or inconsistent handles' % len(problems) if problems else
            'every bootstrap authenticator differs between boots'))
    return lines
