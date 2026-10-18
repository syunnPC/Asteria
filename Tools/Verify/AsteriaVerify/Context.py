# SPDX-License-Identifier: Apache-2.0
"""Build-tree context for target verification checks.

CMake writes `AsteriaVerify.json` into each target build tree. It records the
ISA, the toolchain binaries, the configuration variables, every artifact the
checks inspect, and the object files of every object group, so checks never
guess paths or hard-code build-directory layouts.

Variants
    `firmware` is the firmware build (O2 with the firmware flags), and
    `firmware-test` the test firmware linked from the same kernel objects
    with the guest test program (Tests/Guest) as its initial user image.
    `O0` and `O2` are the compile-check builds, present when the tree was
    configured with ASTERIA_COMPILE_CHECKS=ON.

Artifact keys
    `<variant>:<name>`, for example `firmware:kernel_elf`,
    `firmware-test:user_elf`, `O2:nucleus_elf` or `O0:user_elf`.

Object groups
    `<variant>` holds the production kernel translation units of that
    variant (`firmware-test` the same objects as `firmware`). Compile-check
    variants add `-executive`, `-runtime`, `-fixtures`, `-user` and, on
    IA-64, `-probe` groups; each firmware adds the objects of its user image
    as `firmware-user` and `firmware-test-user` (the production user objects
    without the image's C++ entry, plus the guest objects). Objects are
    looked up by their repository-relative source path (`Source/...` or
    `Tests/...`) or by a symbol they define, never by a file-name suffix.
"""
from __future__ import annotations

import dataclasses
import json
import pathlib
import subprocess

from AsteriaVerify.Elf import SHN_UNDEF, STT_FILE, STT_SECTION, ElfFile

VARIANTS = ('firmware', 'firmware-test', 'O0', 'O2')


class ContextError(Exception):
    """Raised when the manifest lacks a requested artifact or object."""


@dataclasses.dataclass
class TargetContext:
    isa: str
    build: pathlib.Path
    root: pathlib.Path
    tools: dict[str, str]
    configuration: dict[str, str]
    artifacts: dict[str, pathlib.Path]
    groups: dict[str, dict[str, pathlib.Path]]
    compile_checks: bool
    stack_budget_bytes: int
    _symbol_indices: dict[str, dict[str, list[pathlib.Path]]] = dataclasses.field(
        default_factory=dict, repr=False, compare=False)

    @classmethod
    def load(cls, build: pathlib.Path) -> 'TargetContext':
        manifest = json.loads((build / 'AsteriaVerify.json').read_text())
        if manifest.get('format') != 2:
            raise ContextError('%s/AsteriaVerify.json has an unsupported format; reconfigure the tree' % build)
        root = pathlib.Path(manifest['root'])
        groups: dict[str, dict[str, pathlib.Path]] = {}
        for group, paths in manifest['objects'].items():
            objects: dict[str, pathlib.Path] = {}
            for path in paths.split(';') if paths else []:
                source = source_of_object(pathlib.Path(path))
                if source is None:
                    raise ContextError('cannot map object %s to its source' % path)
                if source in objects:
                    raise ContextError('group %s compiles %s twice' % (group, source))
                objects[source] = pathlib.Path(path)
            groups[group] = objects
        return cls(manifest['isa'], build, root, manifest['tools'], manifest['configuration'],
                   {key: pathlib.Path(value) for key, value in manifest['artifacts'].items()},
                   groups, bool(manifest['compile_checks']), int(manifest['stack_budget_bytes']))

    # Variants -----------------------------------------------------------

    @property
    def variants(self) -> tuple[str, ...]:
        """Variants built in this tree, in canonical order."""
        return tuple(variant for variant in VARIANTS if variant in self.groups)

    def has_variant(self, variant: str) -> bool:
        return variant in self.groups

    def option(self, name: str) -> str:
        return self.configuration[name]

    # Tools and artifacts -------------------------------------------------

    def tool(self, name: str) -> str:
        return self.tools[name]

    def demangle(self, names) -> tuple[str, ...]:
        """Demangles with the toolchain's c++filt, which sits next to its nm."""
        nm = self.tool('nm')
        if not nm.endswith('nm'):
            raise ContextError('cannot locate c++filt next to %s' % nm)
        names = tuple(names)
        result = subprocess.run([nm[:-2] + 'c++filt'], input='\n'.join(names) + '\n', capture_output=True,
                                text=True, check=True)
        lines = tuple(result.stdout.splitlines())
        if len(lines) != len(names):
            raise ContextError('c++filt returned %d names for %d' % (len(lines), len(names)))
        return lines

    def artifact(self, key: str) -> pathlib.Path:
        """Resolves `<variant>:<name>` to a path."""
        if key not in self.artifacts or not str(self.artifacts[key]):
            raise ContextError('unknown artifact %s in %s' % (key, self.build))
        return self.artifacts[key]

    def has_artifact(self, key: str) -> bool:
        return key in self.artifacts and bool(str(self.artifacts[key]))

    # Objects ------------------------------------------------------------

    def objects(self, group: str) -> dict[str, pathlib.Path]:
        """Source path → object path for every object of a group."""
        if group not in self.groups:
            raise ContextError('unknown object group %s in %s' % (group, self.build))
        return self.groups[group]

    def object(self, group: str, source: str) -> pathlib.Path:
        """The object compiled from `source` (repository-relative) in `group`."""
        objects = self.objects(group)
        if source not in objects:
            raise ContextError('group %s does not compile %s' % (group, source))
        return objects[source]

    def defining_object(self, group: str, symbol: str) -> pathlib.Path:
        """The unique object of `group` that defines `symbol`."""
        owners = self._symbol_index(group).get(symbol, [])
        if len(owners) != 1:
            raise ContextError('%d objects of group %s define %s%s' % (
                len(owners), group, symbol,
                '' if not owners else ': ' + ', '.join(str(owner) for owner in owners)))
        return owners[0]

    def side_file(self, object_path: pathlib.Path, suffix: str) -> pathlib.Path:
        """GCC auxiliary output next to an object, e.g. `.su` or `.ci`."""
        name = object_path.name
        for extension in ('.obj', '.o'):
            if name.endswith(extension):
                return object_path.with_name(name[:-len(extension)] + suffix)
        raise ContextError('%s is not an object file name' % object_path)

    def _symbol_index(self, group: str) -> dict[str, list[pathlib.Path]]:
        if group in self._symbol_indices:
            return self._symbol_indices[group]
        index: dict[str, list[pathlib.Path]] = {}
        for path in self.objects(group).values():
            for symbol in ElfFile(path).symbols:
                if (symbol.name and symbol.section_index != SHN_UNDEF
                        and symbol.type not in (STT_FILE, STT_SECTION)):
                    owners = index.setdefault(symbol.name, [])
                    if path not in owners:
                        owners.append(path)
        self._symbol_indices[group] = index
        return index

    # Goldens ------------------------------------------------------------

    @property
    def golden_directory(self) -> pathlib.Path:
        return self.root / 'Tests' / 'Golden' / self.isa


def source_of_object(path: pathlib.Path) -> str | None:
    """Maps a CMake object path to the repository-relative source path.

    CMake places the object of `<root>/Source/X/Y.cpp` compiled by a target
    declared in Source/CMakeLists.txt at `.../<target>.dir/X/Y.cpp.obj`, and
    the object of a source outside Source/ at `.../<target>.dir/__/<path>`.
    """
    parts = path.parts
    for index, part in enumerate(parts):
        if part.endswith('.dir'):
            relative = pathlib.PurePosixPath(*parts[index + 1:]).as_posix()
            for suffix in ('.obj', '.o'):
                if relative.endswith(suffix):
                    relative = relative[:-len(suffix)]
                    break
            else:
                return None
            if relative.startswith('__/'):
                return relative[len('__/'):]
            return 'Source/' + relative
    return None
