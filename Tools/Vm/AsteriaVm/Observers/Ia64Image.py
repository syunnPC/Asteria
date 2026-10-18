# SPDX-License-Identifier: Apache-2.0
"""Where the IA-64 kernel and user images are, from the build tree and the run.

The linked geometry comes from the ELF files of the firmware a case boots
(the production or the test firmware of the preset tree) through
`AsteriaVerify.Elf` (sections and symbols). The loaded base comes from the kernel's
pre-ExitBootServices fact line ` image-base=0x<16 hex> image-size=0x<16 hex>`
(BootApplication.cpp, from the executing image’s own relocated linker symbols), the host's only anchor for the
relocation delta. Both kernels run identity-mapped, so a
relocated kernel address is also its physical address.

`ImageFacts` is a plain record of every ELF-derived value the IA-64 checks
use. Its JSON form (the linked IVT bytes are kept out of it and identified by
their SHA-256) lets a recorded run be evaluated again without the build tree;
the unit tests do exactly that.
"""
from __future__ import annotations

import dataclasses
import hashlib
import re

# Sections of the linked kernel image, in address order (Ia64LoadedImageLayout;
# The image checks require page alignment and contiguous rounded pages).
KERNEL_SECTIONS = ('.text', '.note.asteria', 'ReadOnlyBody', 'ReadOnlyTail', '.data', '.bss')
KERNEL_SYMBOLS = (
    "AsteriaIa64StopFromIvt",
    'AsteriaImageBase',                              # Linked image base (absolute)
    'AsteriaIa64KernelIdle',                         # Landing routine, inside.text
    'AsteriaIa64RuntimeInterruptVectorTableImage',   # The runtime IVT (32 KiB object in.text)
    'AsteriaIa64UserBreakEntry',
    'AsteriaIa64SystemCallEntry',                    # Break vector target
    'AsteriaIa64ExternalInterruptDispatchEntry',     # External-interrupt vector target
    'AsteriaIa64ExternalInterruptEntry',             # Legacy external entry the IVT must not use
    'AsteriaIa64PageFaultNativeEntry',               # Page-fault vectors' target
    'AsteriaIa64UserExceptionEntry',                 # General Exception vector target
    'AsteriaIa64PollForExternalInterruptSequence',   # The timer-polling leaf IIP range
    'AsteriaIa64UserExecutionProbePayloadStart',     # Synthetic probe payload (copied to the user code page)
    'AsteriaIa64UserExecutionProbePostSystemCallLoop',
    'AsteriaIa64UserExecutionProbeSpillChild',
)
KERNEL_OBJECTS = (
    # The probe's canonical AIPC frame (Ia64BspKernelRuntime.cpp, anonymous namespace).
    'UserProbeCanonicalFrame',
)
USER_SYMBOLS = (
    'AsteriaIa64MinimalUserImageIdle',               # The entry stub's idle loop (both user images)
)
USER_OBJECTS = (
    # The production runtime's canonical wire buffers (Ia64MinimalUserImageRuntime.cpp,
    # anonymous namespace), from which the synthetic probe's expected delivery is built.
    'CanonicalIpcFrame',
    'CanonicalIpcDelivery',
)

_GEOMETRY_PREFIX = '  image-base='
_GEOMETRY = re.compile(r'^  image-base=0x(?P<base>[0-9A-Fa-f]{16}) image-size=0x(?P<size>[0-9A-Fa-f]{16})$')


class ImageError(Exception):
    """The build tree lacks a symbol or section the checks need."""


@dataclasses.dataclass(frozen=True)
class Symbol:
    value: int
    size: int


@dataclasses.dataclass(frozen=True)
class ImageFacts:
    kernel_sections: dict[str, tuple[int, int]]       # Name -> (linked start, size)
    kernel_symbols: dict[str, Symbol]
    kernel_objects: dict[str, str]                    # Local object name -> bytes (hex)
    ivt_image: bytes                                  # The linked IVT object's bytes (empty: not recorded)
    ivt_image_sha256: str                             # The linked IVT object's hash
    user_symbols: dict[str, Symbol]
    user_objects: dict[str, str]

    @property
    def linked_base(self) -> int:
        return self.kernel_symbols['AsteriaImageBase'].value

    def kernel(self, name: str) -> Symbol:
        return self.kernel_symbols[name]

    def user(self, name: str) -> Symbol:
        return self.user_symbols[name]

    def to_json(self) -> dict:
        return {'kernel_sections': {name: list(extent) for name, extent in self.kernel_sections.items()},
                'kernel_symbols': {name: [symbol.value, symbol.size] for name, symbol in self.kernel_symbols.items()},
                'kernel_objects': self.kernel_objects,
                'ivt_image_sha256': self.ivt_image_sha256,
                'user_symbols': {name: [symbol.value, symbol.size] for name, symbol in self.user_symbols.items()},
                'user_objects': self.user_objects}

    @classmethod
    def from_json(cls, data: dict, ivt_image: bytes | None) -> 'ImageFacts':
        """The inverse of `to_json`; `ivt_image` must be the table the JSON
        identifies, or None for a recording that keeps no IVT (the facts then
        carry an empty table, which no IVT check accepts)."""
        if ivt_image is None:
            ivt_image = b''
        elif hashlib.sha256(ivt_image).hexdigest() != data['ivt_image_sha256']:
            raise ImageError('the supplied IVT bytes are not the linked table the facts identify')
        return cls({name: tuple(extent) for name, extent in data['kernel_sections'].items()},
                   {name: Symbol(*pair) for name, pair in data['kernel_symbols'].items()},
                   dict(data['kernel_objects']), bytes(ivt_image), data['ivt_image_sha256'],
                   {name: Symbol(*pair) for name, pair in data['user_symbols'].items()},
                   dict(data['user_objects']))

    @classmethod
    def from_target(cls, target, firmware: str) -> 'ImageFacts':
        """Reads the kernel and user ELF files of one firmware of the preset
        tree (`firmware` or `firmware-test`)."""
        from AsteriaVerify.Elf import SHN_UNDEF, STT_FILE, STT_SECTION, ElfFile

        def defined(elf, name: str) -> Symbol:
            symbols = [symbol for symbol in elf.symbols_named(name)
                       if symbol.section_index != SHN_UNDEF and symbol.type not in (STT_FILE, STT_SECTION)]
            if len(symbols) != 1:
                raise ImageError('%s defines %s %d times, expected once' % (elf.path, name, len(symbols)))
            return Symbol(symbols[0].value, symbols[0].size)

        def local_object(elf, name: str) -> str:
            """A file-local constant object, whose mangled name ends in `<length><name>E`."""
            suffix = '%d%sE' % (len(name), name)
            symbols = [symbol for symbol in elf.symbols if symbol.name.endswith(suffix) and symbol.size]
            if len(symbols) != 1:
                raise ImageError('%s has %d objects named %s, expected one' % (elf.path, len(symbols), name))
            return elf.symbol_bytes(symbols[0]).hex()

        kernel = ElfFile(target.artifact('%s:kernel_elf' % firmware))
        user = ElfFile(target.artifact('%s:user_elf' % firmware))
        from AsteriaVerify.KernelElfNote import ownership
        sections = {name: (start, size) for name, start, size in ownership(kernel)}
        kernel_symbols = {name: defined(kernel, name) for name in KERNEL_SYMBOLS}
        ivt = kernel_symbols['AsteriaIa64RuntimeInterruptVectorTableImage']
        ivt_bytes = kernel.read_address(ivt.value, ivt.size)
        def optional_objects(elf, names) -> dict[str, str]:
            """The objects the image links (a default kernel does not embed the probe)."""
            found = {}
            for name in names:
                try:
                    found[name] = local_object(elf, name)
                except ImageError:
                    pass
            return found
        return cls(sections, kernel_symbols, optional_objects(kernel, KERNEL_OBJECTS), ivt_bytes,
                   hashlib.sha256(ivt_bytes).hexdigest(),
                   {name: defined(user, name) for name in USER_SYMBOLS}, optional_objects(user, USER_OBJECTS))


@dataclasses.dataclass(frozen=True)
class LoadedImage:
    base: int
    size: int
    firmware_base: int


def loaded_image(lines: list[str]) -> tuple[LoadedImage | None, list[str]]:
    """The single well-formed geometry fact line of the debug channel."""
    candidates = [line for line in lines if line.startswith(_GEOMETRY_PREFIX)]
    if len(candidates) != 1:
        return None, ['%d loaded-image geometry lines (%r), expected exactly one' % (len(candidates),
                                                                                    _GEOMETRY_PREFIX)]
    match = _GEOMETRY.match(candidates[0])
    if match is None:
        return None, ['malformed loaded-image geometry line %r' % candidates[0]]
    base, size = int(match.group('base'), 16), int(match.group('size'), 16)
    if size == 0:
        return None, ['the loaded image size is zero']
    from AsteriaVm.Observers.Amd64Image import kernel_base_problems
    problems = kernel_base_problems(lines, base)
    if problems:
        return None, problems
    firmware_lines = [line for line in lines if line.startswith('  firmware-image-base=')]
    if len(firmware_lines) != 1:
        return None, ['expected exactly one firmware-source geometry line']
    source = _GEOMETRY.fullmatch(firmware_lines[0].replace('firmware-image-base=', 'image-base=', 1))
    if source is None or int(source.group('size'), 16) != size:
        return None, ['malformed or inconsistent firmware-source geometry']
    from AsteriaVm import Ia64Layout
    alignment = Ia64Layout.image_alignment()
    if base % alignment:
        return None, ['the executing copy base %#x is not %#x-aligned' % (base, alignment)]
    firmware_base = int(source.group('base'), 16)
    if firmware_base == base:
        return None, ['the executing image was not copied out of the firmware allocation']
    image = LoadedImage(base, size, firmware_base)
    return image, []


@dataclasses.dataclass(frozen=True)
class KernelLayout:
    """The kernel as loaded: linked facts shifted by the loaded base."""
    facts: ImageFacts
    loaded: LoadedImage

    @property
    def delta(self) -> int:
        return self.loaded.base - self.facts.linked_base

    def address(self, symbol: str) -> int:
        return self.facts.kernel(symbol).value + self.delta

    def extent(self, symbol: str) -> tuple[int, int]:
        """[start, end) of a symbol's relocated extent."""
        start = self.address(symbol)
        return start, start + self.facts.kernel(symbol).size

    def section(self, name: str) -> tuple[int, int]:
        """[start, end) of a relocated section, the end rounded up to 4 KiB."""
        start, size = self.facts.kernel_sections[name]
        return start + self.delta, round_up(start + size, 0x1000) + self.delta


def round_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def relocation_problems(loaded: LoadedImage, preferred_base: int) -> list[str]:
    """The firmware could not honour the preset's preferred base, so it relocated the image."""
    if loaded.firmware_base == preferred_base:
        return ['the image was loaded at its preferred base %#018x, so no base relocation was applied'
                % preferred_base]
    return []
