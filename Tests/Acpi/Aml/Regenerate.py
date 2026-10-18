#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compile only Asteria-authored ASL, keeping iasl temporaries in Build/Tmp."""
import os
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[3]
fixtures = Path(__file__).resolve().parent / 'Fixtures'
scratch = root / 'Build' / 'Tmp' / 'AmlFixtures'
scratch.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, TMPDIR=str(scratch))
compiler = os.environ.get('IASL', 'iasl')
if not shutil.which(compiler):
    raise SystemExit(f'ASL compiler unavailable: {compiler}; set IASL or add iasl to PATH')
for source in sorted(fixtures.glob('*.asl')):
    prefix = scratch / source.stem
    subprocess.run([compiler, '-oa', '-p', str(prefix), str(source)], env=env, check=True)
    shutil.copyfile(prefix.with_suffix('.aml'), source.with_suffix('.aml'))
