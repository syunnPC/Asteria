# SPDX-License-Identifier: Apache-2.0
"""Unit tests of the corridor check's data and cross-checks (no build tree).

Run with: python3 -m unittest discover -s Tools/Verify/Tests -t Tools/Verify
"""
from __future__ import annotations

import dataclasses
import unittest

from AsteriaVerify import CallGraph
from AsteriaVerify.TargetChecks import Ia64PreemptionCorridor as Corridor


def facts(**changes) -> Corridor.CorridorFacts:
    empty = Corridor.CorridorFacts(
        manifest=(('Source/A.cpp', '/b/A.cpp.ci', 10), ('Source/B.cpp', '/b/B.cpp.ci', 20)),
        graph=CallGraph.Graph((), ()), image=CallGraph.Image((), {}),
        header_bounds=tuple((name, 16384) for name in Corridor.BOUND_CONSTANTS),
        bridges=tuple(Corridor.BridgeCost(root.title, root.bridge, 1, root.memory_bytes, root.registers)
                      for root in Corridor.POLICY.roots))
    return dataclasses.replace(empty, **changes)


class ManifestTests(unittest.TestCase):
    def test_every_object_has_one_non_empty_graph(self):
        self.assertEqual(Corridor.manifest_problems(facts().manifest), [])
        manifest = facts().manifest
        self.assertTrue(Corridor.manifest_problems(manifest + manifest[:1]))            # listed twice
        self.assertTrue(Corridor.manifest_problems((('Source/A.cpp', '/b/A.cpp.ci', -1),)))  # missing
        self.assertTrue(Corridor.manifest_problems((('Source/A.cpp', '/b/A.cpp.ci', 0),)))   # empty
        self.assertTrue(Corridor.manifest_problems(()))


class CrossCheckTests(unittest.TestCase):
    HEADER = """
constexpr Foundation::UInt64 Ia64ExternalPreemptionCoordinatorStackSizeBytes{
    16U * 1024U};
constexpr Foundation::UInt64
    Ia64ExternalPreemptionCoordinatorRseTransitionSizeBytes{16U * 1024U};
"""

    def test_header_bounds(self):
        self.assertEqual(Corridor.header_bounds(self.HEADER),
                         tuple((name, 16384) for name in Corridor.BOUND_CONSTANTS))
        self.assertEqual(Corridor.header_bounds('')[0][1], None)

    def test_bounds_and_bridges_match_the_data(self):
        self.assertEqual(Corridor.cross_check_problems(facts(), Corridor.POLICY), [])
        halved = tuple((name, 8192) for name in Corridor.BOUND_CONSTANTS)
        self.assertTrue(Corridor.cross_check_problems(facts(header_bounds=halved), Corridor.POLICY))
        bridges = facts().bridges
        for change in (dict(memory_bytes=bridges[0].memory_bytes + 16), dict(registers=2), dict(indirect_calls=0)):
            changed = (dataclasses.replace(bridges[0], **change),) + bridges[1:]
            self.assertTrue(Corridor.cross_check_problems(facts(bridges=changed), Corridor.POLICY), change)

    def test_policy_data(self):
        policy = Corridor.POLICY
        callers = [binding.caller for binding in policy.bindings]
        self.assertEqual(len(callers), len(set(callers)))
        self.assertTrue(all(binding.count == len(binding.targets) for binding in policy.bindings))
        self.assertEqual(len({symbol for symbol, _ in policy.leaves}), len(policy.leaves))
        self.assertEqual(policy.bound_bytes, 16 * 1024)
        self.assertEqual([root.title for root in policy.roots],
                         ['AsteriaIa64BspNativePreemptionCoordinator', 'AsteriaIa64BspNativePreemptionIdleLanding'])


if __name__ == '__main__':
    unittest.main()
