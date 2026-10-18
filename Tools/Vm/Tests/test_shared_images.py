# SPDX-License-Identifier: Apache-2.0
"""Reject re-staging and object replacement across component restarts."""
import unittest

from AsteriaVm import Guest
from AsteriaVm.Observers import SharedImages


class SharedImageTests(unittest.TestCase):
    def test_restart_identity_and_staging_mutants(self):
        behaviors = ('crash', 'report', 'directory', 'service', 'client', 'service', 'display')
        images = (('component', (2, 1)), ('directory', (2, 1)), ('client', (1, 1)), ('display', (2, 1)))
        facts = []
        seen = set()
        for tag in (256, 1, 2, 3, 4, 5, 6):
            name = 'display' if tag == 256 else SharedImages.image_name(behaviors[tag - 1])
            generation = {'component': 1, 'directory': 2, 'client': 3, 'display': 4}[name]
            for segment in range(dict(images)[name][0]):
                handle = generation * 100 + segment * 10
                facts.append('%s# shared image: %d %d %d %d 11 %d 12 %d' % (
                    Guest.USER_PREFIX, generation, segment, tag, handle, handle + 1, int(name not in seen)))
            seen.add(name)
        self.assertEqual(SharedImages.problems(facts, behaviors, images), [])
        # Each mutation breaks a distinct observable shared-record promise.
        last = facts[-1].split()
        for position, value in ((3, '9'), (7, '999'), (9, '999'), (10, '1')):
            with self.subTest(position=position):
                changed = last.copy()
                changed[position] = value
                self.assertTrue(SharedImages.problems(facts[:-1] + [' '.join(changed)], behaviors, images))
        self.assertTrue(SharedImages.problems(facts[:-1], behaviors, images))
        self.assertTrue(SharedImages.problems(facts + facts[-1:], behaviors, images))


if __name__ == '__main__':
    unittest.main()
