# SPDX-License-Identifier: Apache-2.0
"""ACPI dump oracle rejects content, naming and metadata faults."""
import pathlib
import tempfile
import unittest

from AsteriaVm import AcpiDump


def table(signature, body=b''):
    data = bytearray(36 + len(body))
    data[:4] = signature.encode('ascii')
    data[4:8] = len(data).to_bytes(4, 'little')
    data[8] = 2
    data[16:24] = b'QEMU    '
    data[36:] = body
    data[9] = -sum(data) % 256
    return bytes(data)


class AcpiDumpTests(unittest.TestCase):
    def test_dump_oracle(self):
        with tempfile.TemporaryDirectory() as temporary:
            esp = pathlib.Path(temporary)
            directory = esp / 'efi/asteria/acpi'
            directory.mkdir(parents=True)
            memory = {}
            lines = []
            for index, signature in enumerate(('DSDT', 'FACP', 'APIC', 'SSDT', 'SSDT')):
                address = 0x10000 + index * 0x1000
                data = table(signature, bytes([index]) * (70000 if signature == 'DSDT' else 1))
                memory[address] = data
                name = signature + (str(index - 2) if signature == 'SSDT' else '') + '.aml'
                (directory / name).write_bytes(data)
                lines.append('ACPI %s OEM-table=QEMU     address=0x%016x length=%d revision=2 checksum=valid'
                             % (signature, address, len(data)))
            report = '\r\n'.join(lines + ['ACPI dump saved=5 failed=0']) + '\r\n'
            read = lambda address, size: memory[address][:size]
            self.assertEqual(AcpiDump.problems(esp, report, read), [])
            dsdt = directory / 'DSDT.aml'
            original = dsdt.read_bytes()
            for corrupt in (original[:-1], original + b'\0', table('DSDT', b'other')):
                dsdt.write_bytes(corrupt)
                self.assertTrue(AcpiDump.problems(esp, report, read))
            dsdt.write_bytes(original)
            (directory / 'SSDT2.aml').rename(directory / 'SSDT.aml')
            self.assertIn('missing dump SSDT2.AML', AcpiDump.problems(esp, report, read))
            self.assertTrue(AcpiDump.problems(esp, '', read))

    def test_physical_comparison_is_independent(self):
        with tempfile.TemporaryDirectory() as temporary:
            esp = pathlib.Path(temporary)
            directory = esp / 'EFI/ASTERIA/ACPI'
            directory.mkdir(parents=True)
            lines = []
            for signature in sorted(AcpiDump.REQUIRED):
                data = table(signature, b'good')
                (directory / (signature + '.AML')).write_bytes(data)
                lines.append('ACPI %s OEM-table=QEMU     address=0x0000000000010000 '
                             'length=%d revision=2 checksum=valid' % (signature, len(data)))
            report = '\n'.join(lines + ['ACPI dump saved=3 failed=0'])
            errors = AcpiDump.problems(esp, report, lambda address, size: b'\0' * size)
            self.assertEqual(len(errors), 3)
            self.assertTrue(all('guest physical' in error for error in errors))
