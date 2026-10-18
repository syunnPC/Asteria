# SPDX-License-Identifier: Apache-2.0
"""Generate terminal-screen identity outside verified code and goldens."""
import pathlib
import subprocess
import sys

root, output = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
identity = subprocess.check_output(['git', 'rev-parse', '--short', 'HEAD'], cwd=root, text=True).strip()
dirty = bool(subprocess.check_output(['git', '--no-optional-locks', 'status', '--porcelain', '--untracked-files=normal'], cwd=root))
text = '// SPDX-License-Identifier: Apache-2.0\n#define ASTERIA_STOP_BUILD_IDENTITY "%s%s"\n' % (
    identity, '+dirty' if dirty else '')
output.parent.mkdir(parents=True, exist_ok=True)
if not output.exists() or output.read_text() != text:
    output.write_text(text)
