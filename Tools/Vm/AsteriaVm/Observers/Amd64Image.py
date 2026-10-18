# SPDX-License-Identifier: Apache-2.0
"""Placed kernel geometry reported by the independent UEFI loader.

The exact image-base/image-size lines anchor privileged observations to the
placed ELF. kernel-base must agree. Kernel placement is independent of the
loader PE preferred base; relocated presets additionally exercise firmware
relocation of that loader before it places and enters the kernel.
"""
from __future__ import annotations

import dataclasses
import re

_GEOMETRY_PREFIX = '  image-base='
_GEOMETRY = re.compile(r'^image-base=0x(?P<base>[0-9A-Fa-f]{16}) image-size=0x(?P<size>[0-9A-Fa-f]{16})$')


@dataclasses.dataclass(frozen=True)
class LoadedImage:
    base: int
    size: int


def loaded_image(lines: list[str]) -> tuple[LoadedImage | None, list[str]]:
    """The single well-formed geometry line (two fields, 16 hex digits each)."""
    candidates = [line for line in lines if line.startswith(_GEOMETRY_PREFIX)]
    if len(candidates) != 1:
        return None, ['%d image geometry lines, expected exactly one' % len(candidates)]
    match = _GEOMETRY.match(' '.join(candidates[0].split()))
    if match is None or len(candidates[0].split()) != 2:
        return None, ['malformed image geometry line %r' % candidates[0]]
    base = int(match.group('base'), 16)
    problems = kernel_base_problems(lines, base)
    if problems:
        return None, problems
    return LoadedImage(base, int(match.group('size'), 16)), []


def relocation_problems(lines: list[str], preferred_base: int) -> list[str]:
    image, problems = loaded_image(lines)
    if image is None:
        return problems
    if image.size == 0:
        problems.append('the loaded image size is zero')
    sources = [line for line in lines if line.startswith('  firmware-image-base=')]
    if len(sources) != 1:
        return problems + ['expected exactly one firmware-source geometry line']
    source = _GEOMETRY.fullmatch(' '.join(sources[0].replace('firmware-image-base=', 'image-base=', 1).split()))
    if source is None or int(source.group('size'), 16) == 0:
        return problems + ['malformed firmware-source geometry']
    if int(source.group('base'), 16) == preferred_base:
        problems.append('the loader was loaded at its preferred base %#018x, so no firmware relocation was applied'
                        % preferred_base)
    return problems


def kernel_base_problems(lines: list[str], base: int) -> list[str]:
    """Both ISAs report exactly one matching, full-width kernel-base fact."""
    candidates = [line for line in lines if line.startswith('  kernel-base=')]
    if len(candidates) != 1 or not re.fullmatch(r'  kernel-base=0x[0-9A-Fa-f]{16}', candidates[0]):
        return ['missing, duplicate or malformed kernel-base fact']
    if int(candidates[0].split('0x')[1], 16) != base:
        return ['kernel-base differs from placed image-base']
    return []
