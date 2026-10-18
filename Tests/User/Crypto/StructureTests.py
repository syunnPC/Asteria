# SPDX-License-Identifier: Apache-2.0
"""Regression mutations must invalidate structural review coverage."""
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location('crypto_structure', ROOT / 'Tools/Verify/CheckCryptoStructure.py')
CHECKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKER)


class CryptoStructure(unittest.TestCase):
    def test_review_current(self):
        self.assertEqual(CHECKER.check(ROOT), [])

    def test_review_mutations(self):
        mutations = (
            ('Source/User/Crypto/Symmetric.cpp', 'Sub(w.AesState[', 'Sub(w.AesKey['),
            ('Source/User/Crypto/Curve.cpp', 'for (UInt32 bit = 256U;', 'for (UInt32 bit = 255U;'),
            ('Source/User/Crypto/Argon2.cpp', 'pass == 0U && slice < 2U', 'pass == 0U && slice < 4U'),
            ('Source/User/Crypto/Hash.cpp', 'volatile UInt8*', 'UInt8*'),
            ('Source/User/Crypto/Internal.hpp', 'volatile UInt32 opaque', 'UInt32 opaque'),
        )
        for name, old, new in mutations:
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                for relative in CHECKER.FILES + ('Tools/Verify/CryptoReview.json', 'Source/User/Crypto/SharedSha256.cmake'):
                    dest = root / relative
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    text = (ROOT / relative).read_text()
                    if relative == name:
                        self.assertIn(old, text)
                        text = text.replace(old, new, 1)
                    dest.write_text(text)
                self.assertTrue(CHECKER.check(root))


if __name__ == '__main__':
    unittest.main()
