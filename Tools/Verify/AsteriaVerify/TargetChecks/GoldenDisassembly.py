# SPDX-License-Identifier: Apache-2.0
"""Golden disassembly of hand-written assembly.

Every file under Tests/Golden/<Isa>/ names a symbol or a marker range and
the artifacts it is asserted against. The check disassembles the extent
from each artifact available in the build tree, normalizes it, and compares
it with the reviewed snapshot and its recorded size.

`--update-golden` rewrites the instruction lines (and the binutils version)
from the first available artifact; the other artifacts must then agree with
it, or the update fails.
"""
from __future__ import annotations

import pathlib

from AsteriaVerify import Disassembly, Golden
from AsteriaVerify.Checks import check
from AsteriaVerify.Context import VARIANTS, ContextError


def resolve(context, specification: str, symbol: str) -> pathlib.Path | None:
    """The artifact path of one golden artifact specification, or None when
    the tree does not contain that variant or was not built with the
    specification's configuration option."""
    specification, option = Golden.split_condition(specification)
    if option is not None and context.configuration.get(option) != '1':
        return None
    variant = specification.split(':', 1)[0] if ':' in specification else specification.split('-', 1)[0]
    if ':' not in specification and variant not in VARIANTS:
        raise ContextError('unknown artifact specification %s' % specification)
    if not context.has_variant(variant):
        return None
    return context.artifact(specification) if ':' in specification else context.defining_object(specification, symbol)


def normalized(context, record: Golden.GoldenRecord, path: pathlib.Path) -> tuple[list[str], int]:
    start, end = record.range or (None, None)
    decoded = Disassembly.disassemble(context.tool('objdump'), str(path), symbol=record.symbol, start_symbol=start,
                                      end_symbol=end)
    normalize = Disassembly.normalize_ia64_groups if context.isa == 'Ia64' else Disassembly.normalize_x86
    return normalize(decoded.instructions), decoded.end - decoded.start


@check('GoldenDisassembly', uses_golden=True)
def golden_disassembly(context, *, update_golden: bool = False, goldens: list[str] | None = None):
    problems = []
    directory = context.golden_directory
    files = sorted(directory.rglob('*.dis')) if directory.exists() else []
    if goldens:
        wanted = {(context.root / name).resolve() for name in goldens}
        missing = sorted(str(path) for path in wanted if not path.exists())
        if missing:
            return ['no such golden file: %s' % ', '.join(missing)]
        files = [path for path in files if path.resolve() in wanted]
    version = Disassembly.tool_version(context.tool('objdump'))
    for path in files:
        relative = path.relative_to(context.root)
        try:
            record = Golden.parse(path)
        except Golden.GoldenError as error:
            problems.append(str(error))
            continue
        if record.binutils != version and not update_golden:
            problems.append('%s was recorded with "%s" but the build uses "%s"; regenerate with '
                            '--update-golden and review the diff' % (relative, record.binutils, version))
            continue
        results = []
        for specification in record.artifacts:
            try:
                artifact = resolve(context, specification, record.symbol or record.range[0])
                if artifact is not None:
                    results.append((specification, *normalized(context, record, artifact)))
            except (ContextError, Disassembly.DisassemblyError, Golden.GoldenError) as error:
                problems.append('%s: %s: %s' % (relative, specification, error))
        if not results:
            continue
        if not update_golden:
            for specification, lines, size in results:
                problems.extend(Golden.compare(record, lines, size, specification, relative))
            continue
        first_specification, first_lines, first_size = results[0]
        disagreeing = [spec for spec, lines, _ in results[1:] if lines != first_lines]
        if disagreeing:
            problems.append('%s: %s disagree with %s; one golden cannot serve them'
                            % (relative, ', '.join(disagreeing), first_specification))
            continue
        record.binutils, record.lines = version, first_lines
        if record.size is not None:
            record.size = first_size
        path.write_text(record.render())
    return problems
