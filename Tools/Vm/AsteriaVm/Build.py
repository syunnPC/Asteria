# SPDX-License-Identifier: Apache-2.0
"""Build trees of the presets the selected cases boot.

Each preset is configured and built once (`cmake --preset`, `cmake --build
--preset`), and its static checks (`ctest -L static`) run before any VM
boots its firmware. The tree's `AsteriaVerify.json` then gives the runner
the ESP, the kernel and user ELF files and the configuration.
"""
from __future__ import annotations

import pathlib
import subprocess
import sys

VERIFY = pathlib.Path(__file__).resolve().parents[2] / 'Verify'
if str(VERIFY) not in sys.path:
    sys.path.insert(0, str(VERIFY))

from AsteriaVerify.Context import TargetContext  # noqa: E402


class BuildError(Exception):
    pass


def tree(root: pathlib.Path, preset: str) -> pathlib.Path:
    return root / 'Build' / 'CMake' / preset


def prepare(root: pathlib.Path, presets: list[str], *, build: bool, static_checks: bool,
            log: pathlib.Path) -> dict[str, TargetContext]:
    contexts = {}
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open('a') as output:
        for preset in presets:
            directory = tree(root, preset)
            steps = []
            if build:
                if not (directory / 'CMakeCache.txt').exists():
                    steps.append(['cmake', '--preset', preset])
                steps.append(['cmake', '--build', '--preset', preset])
            if static_checks:
                steps.append(['ctest', '--preset', preset, '-L', 'static', '--output-on-failure'])
            for command in steps:
                output.write('$ %s\n' % ' '.join(command))
                output.flush()
                completed = subprocess.run(command, cwd=root, stdout=output, stderr=subprocess.STDOUT)
                if completed.returncode != 0:
                    raise BuildError('%s failed for preset %s; see %s' % (' '.join(command[:2]), preset, log))
            if not (directory / 'AsteriaVerify.json').exists():
                raise BuildError('preset %s has no build tree at %s' % (preset, directory))
            contexts[preset] = TargetContext.load(directory)
    return contexts
