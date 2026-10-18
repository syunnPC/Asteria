# SPDX-License-Identifier: Apache-2.0
"""Default separation policy must cover the actual verification defines."""
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]


class SeparationTests(unittest.TestCase):
    def test_default_policy_rejects_host_and_ui_trace(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / 'Source').mkdir()
            for macro in ('ASTERIA_HOST_TEST', 'ASTERIA_UI_TEXT_TRACE', 'ASTERIA_SESSION_LIVENESS_FAULT', 'ASTERIA_SESSION_SEAT'):
                with self.subTest(macro=macro):
                    (root / 'Source' / 'Unit.cpp').write_text('#if defined(%s)\n#endif\n' % macro)
                    result = subprocess.run([sys.executable, str(ROOT / 'Tools/Build/CheckSeparation.py'),
                                             '--root', directory], capture_output=True, text=True)
                    self.assertNotEqual(result.returncode, 0, result.stdout)
