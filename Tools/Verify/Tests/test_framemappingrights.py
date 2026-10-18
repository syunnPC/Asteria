# SPDX-License-Identifier: Apache-2.0
"""The Frame mapping inventory and its source/facts mutations need no target build."""
import pathlib
import types
import unittest
from unittest import mock

from AsteriaVerify.TargetChecks.FrameMappingRights import FrameMappingRights, sites, INVENTORY

ROOT = pathlib.Path(__file__).resolve().parents[3]


class FrameMappingRightsTests(unittest.TestCase):
    def test_inventory_and_mutations(self):
        prop = FrameMappingRights()
        facts = prop.extract(types.SimpleNamespace(root=ROOT), 'firmware')
        self.assertEqual(prop.verify(facts), [])
        for description, mutant in prop.mutants(facts):
            with self.subTest(description=description):
                self.assertTrue(prop.verify(mutant))

    def test_new_call_in_a_reviewed_function_is_detected(self):
        path = 'Source/User/Component/FilesystemService.cpp'
        source = (ROOT / path).read_text()
        before = sites(source)
        # Add a second CPU mapping at the actual client mapping call.
        changed = source.replace('return System::MapWindow(', 'static_cast<void>(System::MapWindow());\n    return System::MapWindow(', 1)
        after = sites(changed)
        self.assertNotEqual(before, after)
        self.assertEqual(after['MapClient', 'MapWindow'], 2)
        self.assertNotIn((path, 'MapClient', 'MapWindow', 2), INVENTORY)

    def test_direct_syscall_selectors_are_inventoried(self):
        self.assertEqual(sites('void f() noexcept { Invoke(Abi::SystemCallNumber::MemoryMap, {}); }'),
                         {('f', 'MemoryMap syscall'): 1})

    def test_source_rights_mutations(self):
        mutations = (
            ('Source/User/Component/FsFrame.hpp', 'FsDataRights{0x0118U}', 'FsDataRights{0x0110U}'),
            ('Source/User/Component/NetFrame.hpp', 'NicMapRight{0x0018U}', 'NicMapRight{0x0010U}'),
            ('Source/User/Component/BlockFrame.hpp', 'BlockMapRight{0x0018U}', 'BlockMapRight{0x0010U}'),
            ('Source/User/Session/SessionFrame.hpp', 'SessionMapRight{0x0010U}', 'SessionMapRight{0x0018U}'),
            ('Source/Kernel/Memory/FrameObjectPool.hpp', '.With(Object::CapabilityRight::Write)', ''),
            ('Source/Kernel/Memory/FrameObjectPool.hpp', '.With(Object::CapabilityRight::Read)', ''),
            ('Source/Platform/Amd64/System/Amd64PagerRuntime.cpp',
             'map.With(CapabilityRight::Write)', 'map'),
            ('Source/Platform/Ia64/System/Ia64PagerRuntime.cpp',
             'map.With(CapabilityRight::Write)', 'map'),
            ('Source/User/Component/DisplaySession.cpp', 'ReadUser{0x09U}', 'ReadUser{0x0BU}'),
            ('Source/User/Component/SessionCompositor.cpp', 'ReadUser{0x09U}', 'ReadUser{0x0BU}'),
            ('Source/User/Pager/DeviceInventory.cpp', 'ReadPermissions{9U}', 'ReadPermissions{11U}'),
        )
        original_read = pathlib.Path.read_text
        for relative, before, after in mutations:
            with self.subTest(source=relative, mutation=before):
                self.assertIn(before, (ROOT / relative).read_text())
                def changed_read(path, *args, **kwargs):
                    text = original_read(path, *args, **kwargs)
                    return text.replace(before, after, 1) if path == ROOT / relative else text
                with mock.patch.object(pathlib.Path, 'read_text', changed_read):
                    facts = FrameMappingRights().extract(types.SimpleNamespace(root=ROOT), 'firmware')
                self.assertTrue(FrameMappingRights().verify(facts))

    def test_comments_literals_and_declarations_are_not_calls(self):
        self.assertEqual(sites('void MapWindow() noexcept; // MapWindow()\n'), {})
        self.assertEqual(sites('void f() noexcept { const char* s = "MapMemory()"; /* MapWindow() */ MapWindow(); }'),
                         {('f', 'MapWindow'): 1})
