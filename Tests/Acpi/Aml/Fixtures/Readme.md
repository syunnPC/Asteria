<!-- SPDX-License-Identifier: Apache-2.0 -->

# AML fixtures

ASL fixtures are authored from ACPI specification facts. Adjacent AML is generated
with `iasl` (fixture generator version 20251212). `../Regenerate.py` uses `-oa`
to preserve operators, writes temporary output to `Build/Tmp/AmlFixtures` and
copies generated AML here. Intentional recursion/temporary-name remarks are
expected; warnings are not suppressed.

| Fixtures | Coverage |
| --- | --- |
| Core, More, Resources | Devices, methods, IDs, routing, resources, operators, fields, rollback and effects |
| Forward, Later, ExternalAfter | Cross-table loading and declaration order |
| Conditional, ConditionalUnknown, ModuleRecovery | Branches/loops, unknown predicates and statement rollback |
| LocalShadow, DebugOutput | Local names/fields and debugger effects with unknown Timer values |
| Ps2Evidence, Ps2Signaling | Device IDs, ancestor/status vetoes, I/O bounds and IRQ signaling |
| FloatingKeyboard | SSDT keyboard declaration for a VM without a 0x60/0x64 decoder |

Parser/fuzz tests also generate malformed and boundary byte encodings directly.
External firmware tables are read from configured paths at test time and are
never copied into these fixtures.
