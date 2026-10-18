# SPDX-License-Identifier: Apache-2.0
"""Replay a fixture against its own captured project headers.

Live observations still require the current schema exactly. A rebase may
retain the base's binary fixtures until the final full VM run; preserving
its headers makes those recordings independently reviewable and prevents
silently relabelling or zero-extending an older evidence record.
"""
import json
import functools
from AsteriaVm import Evidence
from AsteriaVerify import Layout


@functools.lru_cache(maxsize=None)
def schema(directory, module):
    captured = json.loads((directory / 'BuildHeaders.json').read_text())['sources']
    first = captured[module.HEADERS[0]]
    return Evidence.Schema(Layout.constants(captured[Evidence.COMMON_HEADER])[Evidence.MAGIC_CONSTANT],
        Layout.constants(first)[module.SCHEMA_CONSTANT], Evidence.structure(captured, module.RECORD))


def record(directory, module, root):
    captured = {path: (root / path).read_text() for path in [*module.HEADERS, Evidence.COMMON_HEADER]}
    if module.RECORD == 'Amd64EvidenceRecord':
        path = 'Source/Platform/Amd64/Boot/Amd64RuntimePoolLayout.hpp'
        captured[path] = (root / path).read_text()
    (directory / 'BuildHeaders.json').write_text(json.dumps({'sources': captured}, indent=1) + '\n')
