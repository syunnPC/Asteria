<!-- SPDX-License-Identifier: Apache-2.0 -->

# Pure AML and ACPI facts

This library parses AML, evaluates hardware-independent expressions and reports
boot-time ACPI facts. Host and native builds check `-O0`/`-O2` stack budgets.
Production code has no allocation, recursion, mutable globals or hardware I/O.
Sources follow the [ACPI 6.5 specification](https://uefi.org/sites/default/files/resources/ACPI_Spec_6_5_Aug29.pdf),
sections 5.3, 6.4, 19, and 20, plus Appendix C's legacy Processor/Unload encodings.
Compatibility strings are facts derived from
[Microsoft's OSI documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/acpi/winacpi-osi).

## Ownership and interfaces

Callers construct `Arena` in caller-owned pages or hosted memory, then instantiate
`Library` from its borrowed `Get()` view. Complete DSDT/SSDT table bytes must
remain alive and immutable until `Reset` or destruction. Tables must be loaded
in platform firmware order, followed by a single invocation of `FinishLoading`.
Arena contents and capacities must not be modified while active. Table and object
IDs are context-local and invalidated by `Reset`. All parser and evaluator
operations have bounded execution budgets and are not suitable for interrupt
contexts. Keep `Arena` and `ReportStorage` instances off native kernel stacks.

`GetNode`, `GetSyntax`, and `Table` expose checked namespace and syntax metadata.
Methods retain body spans, argument counts, serialization and sync levels.
Region/field declarations retain their original encodings, and field units retain
bit offsets and lengths. Name initializers are lazy. The first definition wins;
later definitions are diagnosed. External objects supply signatures until a real
definition arrives. Scope placeholders permit references into later tables.
Only relative single NameSeg references search ancestors; explicit parents and
multiple segments do not. Method bodies are parsed after all declarations load.
Module-level conditionals and loops then run in table order using the pure
evaluator. Only selected declarations become active. An unknown branch leaves
its potential declarations with an Unknown load verdict, rather than publishing
them as present devices.

`Evaluate` accepts up to seven argument IDs, made with `Integer` or `String`.
`EvaluationResult::Error` distinguishes Unknown from known data. A successful
method without Return has `Value == NoId`, a void result, not a guessed integer.
Packages may retain name references. Successful stores persist in the arena's
model so `_PIC(1)` can select subsequent `_PRT` results. Failed evaluations roll
back Names, elements, buffers and lazy caches. Diagnostics, effects, OSI queries
and charged pool storage remain; Reset reclaims that storage for a new analysis.

`Facts::Prepare` runs `\_PIC(1)` once if present. `NextDevice`/`ReadDevice` stream
properties into caller scratch. Missing `_STA` defaults to present; an absent
ancestor overrides presence and an unknown ancestor prevents a present verdict.
Other missing properties remain explicit. PCI routing is Unknown if `_PIC`
fails. Keyboard/mouse candidate flags match IDs and authorize no I/O. `Sleep`
reads `_S5`'s two sleep types.

`Report::Print` streams **Asteria AML facts v1** for the boot ACPI.TXT writer.
Its `IReportSink` consumes bytes synchronously; false stops reporting. Strings escape control bytes, quotes and backslashes as `\xHH`.
Paths retain NameSeg padding. Resource flags/attributes are numeric; IRQs retain
masks and ExtendedIRQ prints all interrupt numbers and its source.

## Grammar and evaluator

The parser covers section 20's one-byte and 0x5B argument shapes, PkgLength,
root/parent/dual/multi/null names, field access/connection entries, namespace
containers and nested objects. Reserved or truncated encodings stop that parse
with a diagnostic and retain earlier nodes. Malformed method bodies cannot run.
Parser, evaluator, package-copy and call stacks are explicit arena arrays.

Evaluation supports integers (32 bits for revision-1 tables, otherwise 64),
strings, buffers, nested fixed/variable packages, references, method calls, Args
and Locals, method-local Names, Return, If/Else, bounded While, Break/Continue,
Store/CopyObject, logical/comparison/arithmetic/bitwise operators, increment and
decrement, find-set-bit, Index/DerefOf/RefOf, SizeOf/ObjectType/CondRefOf,
Concatenate, ToHexString/ToDecimalString/ToBuffer/ToInteger/ToString, Mid, and the
CreateField family. Integer Match predicates are supported; other Match operand
types yield Unknown. Fields wider than 64 bits yield buffers.

Acquire returns zero (success). Release, Notify, Sleep, Stall, Signal, Wait and
Reset are recorded without action; simulated Wait returns zero. Noop is accepted.
Debug writes are benign recorded effects (ACPI 6.5 19.6.26); ObjectType(Debug)
is 16. Timer produces an unknown value without reading a clock: discarded
debug output can consume it, while a result or predicate requiring it is Unknown.
Region/Field/IndexField/BankField access, Load/LoadTable/Unload, Fatal, BCD
conversions, ConcatenateResTemplate and other operations outside
this subset yield Unknown. Runtime namespace declarations other than Names and
buffer fields also yield Unknown. `_REV` is 2 and `_OS` is `Microsoft Windows NT`.
`_OSI` recognizes the 22 Microsoft-documented Windows version strings from
Windows 2000 through Windows 2022. Linux and unknown strings return zero; every
query is retained.

Method parsing tracks local Name and buffer-field declarations in bounded
scratch, so they shadow ancestor methods without inventing call arguments.
Later External declarations and reopened Scopes preserve the original defining
body and table. Module TermLists recover at independent statement boundaries
from hardware-dependent or unsupported execution: the failed statement's stores
and activations roll back, its unselected declarations (both If alternatives)
remain Unknown, and later declarations can load. Resource exhaustion and malformed
execution still stop loading. No hardware value is guessed.

## Limits

All capacities are constants in `Types.hpp`. Arena occupies **34,493,824 bytes**
(about 32.89 MiB) on supported 64-bit ABIs. Each namespace node is **64 bytes**,
checked by static assertion; 32,768 nodes occupy 2 MiB. Syntax records are 56 bytes
and objects 24 bytes. ReportStorage occupies 158,624 bytes. `ReducedArena` provides 8,192 nodes in
10,114,432 bytes; its five storage pools are proportionally smaller. The library
clamps caller limits to actual backing capacity, and the formatter reports those
limits. `AmlFacts --reduced-arena` reproduces the reduced boot report.

| Resource | Bound |
| --- | ---: |
| Tables / bytes per table | 256 / 1 MiB |
| Namespace nodes / hash slots | 32,768 / 65,536 |
| Syntax / objects / element slots | 262,144 each |
| Byte pool / bytes per object | 8 MiB / 65,536 |
| Elements per package / routing entries | 4,096 each |
| Parser / evaluator frames | 256 each |
| Call frames, including outer activation | 32 |
| Temporary names / journal entries per evaluation | 1,024 / 131,072 |
| Diagnostics / effects / OSI queries | 1,024 / 4,096 / 4,096 |
| Resources per template / IDs per property | 256 / 64 |
| Load operations / evaluation operations per call | 16,777,216 / 100,000 |
| Fact operations per Facts instance | 16,777,216 by default |

The caller may lower pool limits before Reset and lower evaluation/facts budgets.
Evaluation budgets count interpreter steps and reference traversals; byte loops
are also bounded by object/pool capacities, and wide field bit loops charge
operations. Hash probes are bounded by the named slot capacity. Overflow retains
a partial namespace plus diagnostics, or produces Unknown. `DroppedDiagnostics`
counts records beyond the diagnostic capacity. Records contain reason, table,
offset, opcode and node where available. Reports include pool use and depth peaks.

## Host tool and tests

Build with any host preset, then supply table files in order:

```sh
export TMPDIR="$PWD/Build/Tmp"
cmake --preset host-debug
cmake --build --preset host-debug
Build/CMake/host-debug/Tools/AmlFacts DSDT.AML SSDT1.AML SSDT2.AML > ACPI.TXT
```

Exit 2 indicates command-line usage or file read failure; exit 1 indicates table
parsing, grammar, or output formatting failure. Unresolved or unknown facts remain
standard report entries. The host tool owns its input buffers and arena.

```sh
python3 Tests/Acpi/Aml/Regenerate.py
ctest --preset host-debug -R 'Acpi/Aml|Fuzz/Aml' --output-on-failure
```

[Fixtures](../../../Tests/Acpi/Aml/Fixtures/Readme.md) and generated AML are
project-authored. Tests cover namespace capacity, malformed input, cross-table
references, evaluation/rollback, budgets, device facts and resource routing.
`Tests/Fuzz/AmlFuzz.cpp` checks bounds and determinism for tables, repaired
bodies and resource templates. Hosted presets replay 20,000 mutations;
`host-fuzz` enables libFuzzer.

`QemuTablesTests` reads q35/pc DSDTs and sorted unqualified SSDTs from
`ASTERIA_QEMU_ACPI_DIRECTORY`, skipping when absent. External tables are never
copied into the repository. Hardware-dependent facts remain Unknown.

Boot capture, ACPI.TXT file writing and hardware policy are outside this library;
see [AML boot facts](../../../Documentation/Architecture/AmlBootFacts.md).
