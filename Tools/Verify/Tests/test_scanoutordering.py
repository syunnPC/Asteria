# SPDX-License-Identifier: Apache-2.0
import unittest
from types import SimpleNamespace

from AsteriaVerify.TargetChecks.ScanoutOrdering import ia64_callback_bound, publication_pass_facts


class PublicationPassTests(unittest.TestCase):
    def test_drawing_fences_and_presented_require_render_but_credit_precedes_it(self):
        good = ['DisplaySession10SendCredit', 'BrokerRender', 'DisplaySession9Presented', 'L12RetryAnswers']
        facts = lambda names: publication_pass_facts([SimpleNamespace(text=name) for name in names], 'BrokerRender')
        self.assertTrue(all(facts(good).values()))
        for names in ([good[0], good[2], good[3]],
                      [good[0], good[3], good[1], good[2]],
                      [good[0], good[2], good[1], good[3]],
                      [good[1], good[0], good[2], good[3]]):
            self.assertFalse(all(facts(names).values()), names)


class CallbackImage:
    """A final GOT pointer followed by a separate IA-64 function descriptor."""

    def __init__(self, got_size=24, descriptor_size=16, entry=0x3000):
        self.sections = [SimpleNamespace(address=0x1000, size=got_size),
                         SimpleNamespace(address=0x2000, size=descriptor_size)]
        self.words = {0x1010: 0x2000, 0x2000: entry, 0x2008: 0x1000}

    def symbol(self, name):
        assert name == '__gp'
        return SimpleNamespace(value=0x1000)

    def section_containing(self, address):
        return next((section for section in self.sections
                     if section.address <= address < section.address + section.size), None)

    def read_address(self, address, size):
        section = self.section_containing(address)
        assert section and address + size <= section.address + section.size
        assert size == 8
        return self.words[address].to_bytes(8, 'little')


class Ia64CallbackTests(unittest.TestCase):
    code = [SimpleNamespace(text='addl r46=16,r1')]

    def test_last_got_slot_binds_the_ordering_adapter(self):
        self.assertTrue(ia64_callback_bound(CallbackImage(), self.code, 0x3000))

    def test_truncated_pointer_or_descriptor_cannot_bind(self):
        for image in (CallbackImage(got_size=23), CallbackImage(descriptor_size=15)):
            self.assertFalse(ia64_callback_bound(image, self.code, 0x3000))

    def test_another_entry_cannot_prove_ordering(self):
        self.assertFalse(ia64_callback_bound(CallbackImage(entry=0x3010), self.code, 0x3000))

    def test_missing_descriptor_cannot_prove_ordering(self):
        image = CallbackImage()
        image.words[0x1010] = 0x4000
        self.assertFalse(ia64_callback_bound(image, self.code, 0x3000))
