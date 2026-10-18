# SPDX-License-Identifier: Apache-2.0
"""Exercise the real CMake linker function with a custom-named script."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]


class UserImageGateTests(unittest.TestCase):
    def test_custom_script_is_registered_on_both_isas(self):
        cmake = (ROOT / 'Tools/CMake/AsteriaTarget.cmake').read_text()
        start = cmake.index('function(asteria_link_user_image ')
        function = cmake[start:cmake.index('endfunction()', start) + len('endfunction()')]
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / 'Custom.lds').write_text('PHDRS { Text PT_LOAD FLAGS(5); ReadOnlyData PT_LOAD FLAGS(4); }\nSECTIONS { .text : { *(.text*) } :Text }')
            for isa, coalesced, split in ((isa, combined, split) for isa in ('Amd64', 'Ia64')
                                          for combined in (False, True) for split in (False, True)):
                readonly = ('InputText' if split else 'Text') if coalesced else 'ReadOnlyData'
                descriptors = ''.join(' .%s : ALIGN(16) { *(.%s) } :%s\n' % (name, name, readonly)
                                      for name in ('AsteriaUserEntryDescriptor', 'opd', 'got', 'data', 'init_array'))
                (root / 'Custom.lds').write_text('PHDRS { Text PT_LOAD FLAGS(5); ' +
                    ('InputText PT_LOAD FLAGS(5); ' if split else '') +
                    ('' if coalesced else 'ReadOnlyData PT_LOAD FLAGS(4); ') +
                    '}\nSECTIONS { .text : { *(.text*) } :Text\n' +
                    (' .input_text : { *(.input_text*) } :InputText\n' if split else '') +
                    ' .rodata : { *(.rodata*) } :' +
                    readonly + '\n' + descriptors + ' }\n')
                (root / 'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.28)\nproject(Gate NONE)\n'
                    'set(ASTERIA_ROOT "%s")\nset(ASTERIA_TARGET %s)\nset(ASTERIA_TARGET_LD ld)\n'
                    'set(Python3_EXECUTABLE python3)\n%s\n'
                    'asteria_link_user_image("${CMAKE_BINARY_DIR}/Custom.elf" "${CMAKE_BINARY_DIR}/Custom.map" '
                    'SCRIPT "${CMAKE_SOURCE_DIR}/Custom.lds" OBJECTS dummy.o)\n'
                    'add_custom_target(Image ALL DEPENDS "${CMAKE_BINARY_DIR}/Custom.elf")\n' % (ROOT, isa, function))
                build = root / ('%s-%s-%s' % (isa, coalesced, split))
                result = subprocess.run(['cmake', '-G', 'Ninja', '-S', str(root), '-B', str(build)],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                with self.subTest(isa=isa, coalesced=coalesced, split=split):
                    self.assertIn('Custom.elf.TextGate', (build / 'build.ninja').read_text())
                    gate = (build / 'Custom.elf.TextGate.lds').read_text()
                    self.assertEqual(gate.count('ReadOnlyData PT_LOAD FLAGS(6);'), 1)
                    self.assertIn('.rodata : { *(.rodata*) } :ReadOnlyData', gate)
                    self.assertIn('.text : { *(.text*) } :Text', gate)
                    for name in ('AsteriaUserEntryDescriptor', 'opd', 'got', 'data', 'init_array'):
                        self.assertIn('.%s : ALIGN(16) { *(.%s) } :ReadOnlyData' % (name, name), gate)
                    if split:
                        self.assertIn('InputText PT_LOAD FLAGS(5);', gate)
                        self.assertIn('.input_text : { *(.input_text*) } :InputText', gate)
