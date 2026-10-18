#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check source whitespace and per-file SPDX notices without a Git dependency.

Formatting choices are in .clang-format; semantic rules still require review.
Opaque fixtures have adjacent .license files so their parsers and bytes stay intact.
"""
import argparse
from pathlib import Path
import re


ROOT_FILES = ('.gitignore', '.clang-format', '.clang-format-ignore', '.editorconfig', 'CMakeLists.txt',
              'CMakePresets.json', 'README.md', 'NOTICE', 'LICENSE')
SOURCE_SUFFIXES = {'.cpp', '.hpp', '.h', '.S', '.inc', '.lds', '.asl', '.py', '.cmake', '.md'}
SPDX = re.compile(r'SPDX-License-Identifier:\s*(\S[^\r\n]*?)(?:\s*-->)?\s*$')


def source_files(root):
    for name in ROOT_FILES:
        path = root / name
        if path.is_file():
            yield path
            sidecar = Path(str(path) + '.license')
            if sidecar.is_file():
                yield sidecar
    for name in ('Source', 'Tests', 'Tools', 'Documentation', 'Data', 'LICENSES'):
        for path in sorted((root / name).rglob('*')):
            if path.is_file() and '__pycache__' not in path.parts:
                yield path


def check_file(path, root):
    if path.suffix == '.license':
        return []
    problems = []
    relative = path.relative_to(root)
    text_source = (path.suffix in SOURCE_SUFFIXES or path.name in ROOT_FILES
                   or path.name == 'CMakeLists.txt' or relative.parts[0] == 'LICENSES')
    if text_source:
        try:
            text = path.read_bytes().decode('utf-8')
        except UnicodeDecodeError:
            return [f'{relative}: source must be UTF-8']
        if text and not text.endswith('\n'):
            problems.append(f'{relative}: missing final newline')
        for number, line in enumerate(text.splitlines(keepends=True), 1):
            content = line.rstrip('\r\n')
            if '\r' in line:
                problems.append(f'{relative}:{number}: use LF line endings')
            if '\t' in content:
                problems.append(f'{relative}:{number}: use spaces instead of tabs')
            if content.rstrip() != content:
                problems.append(f'{relative}:{number}: trailing whitespace')
        notice = '\n'.join(text.splitlines()[:5])
    else:
        # JSON, logs, goldens and binary formats cannot safely accept a comment.
        notice = ''
    sidecar = Path(str(path) + '.license')
    if sidecar.is_file():
        notice += '\n' + sidecar.read_text(encoding='utf-8')
    if not any(SPDX.search(line) for line in notice.splitlines()):
        problems.append(f'{relative}: missing SPDX header or .license sidecar')
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    root = args.root.resolve()
    paths = list(source_files(root))
    problems = [problem for path in paths for problem in check_file(path, root)]
    for problem in problems:
        print(problem)
    print(f'Style/SPDX: checked {len(paths)} files; {len(problems)} violations')
    return bool(problems)


if __name__ == '__main__':
    raise SystemExit(main())
