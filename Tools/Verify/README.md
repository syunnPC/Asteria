<!-- SPDX-License-Identifier: Apache-2.0 -->

# Target verification

`VerifyTarget.py` inspects target build artifacts, including unoptimized (`-O0`)
and optimized (`-O2`) objects and link closures, production and guest-test
firmware images, and user binaries. CMake registers `TargetVerification` in each
target build tree, generating an `AsteriaVerify.json` manifest that enumerates
every artifact and object eligible for inspection.

```
python3 Tools/Verify/VerifyTarget.py --build Build/CMake/ia64 [--check NAME]... [--verbose]
python3 Tools/Verify/VerifyTarget.py --build Build/CMake/ia64 --update-golden --check GoldenDisassembly --golden <file>
python3 -m unittest discover -s Tools/Verify/Tests -t Tools/Verify # library unit tests
```

## Kinds of checks

**Goldens** compare hand-written assembly with stored disassembly references.
Files under `Tests/Golden/<Isa>/` record normalized disassembly for a symbol or
a marker-delimited range, matching object groups (`O0 O2 firmware`) or artifact
keys (`O2:user_elf`). An option tag such as `[option]` restricts verification to
trees where that manifest option is `1`; for example,
`firmware[ia64_native_preemption_probe]` applies only when
`ASTERIA_IA64_NATIVE_PREEMPTION_PROBE=1`.

IA-64 disassembly lines use `stopBefore:stopAfter:instruction`; AMD64 lines
contain normalized instructions. NOP instructions are omitted while IA-64
instruction-group stops remain. Branch targets
within the extent become `<+0xOFF>`, targets outside become `<Symbol+0xOFF>`, and
relocations are normalized to `<reloc:TYPE target>`. Golden files record the
binutils version used to generate them; goldens created with a different binutils
version are marked stale.

To add a golden, copy an existing header (`golden`, `binutils`, `symbol` or
`range`, `size`, `artifacts`), leave the body empty, run
`--update-golden --check GoldenDisassembly --golden <file>`, and review the diff.

**Properties** check structural invariants such as serialized control-register
writes, exactly one unpredicated call, or symbol closure. Subclass `Checks.Property`:

- `extract(context, subject)` extracts raw facts from the artifact: records,
  tuples, or the decoded control-flow instructions of a symbol (`ControlFlow.Code`).
- `verify(facts)` inspects the extracted facts and returns descriptions of any
  problems found.
- `mutants(facts)` yields `(description, broken facts)` pairs that must each fail verification.
- `summary(facts)` optionally describes passing facts; `--verbose` prints it.

`@register` registers a property under its class name unless `name` overrides it.
`subjects(context)` selects the artifacts to inspect. Bases in `Subjects` cover
symbol code (`CodeProperty`), function-symbol contracts (`SymbolContract`) and
hand-written helpers (`HelperSymbols`, `HelperExtents`, `HelperRelocations`).
`Checks.ItemProperty` evaluates named items via `item_problems` and `item_mutants`.

Every run requires real facts to pass and every synthesized mutant to fail.
A mutant identical to real facts is treated as an error.
Always use property checks rather than assembly goldens for compiler-generated
code; compiler or optimization flag changes invalidate disassembly goldens.

## Library

The verification library (`Tools/Verify/AsteriaVerify`) includes:

- `Checks`: Base property classes, mutant generators, and property registries.
- `Subjects`: Artifact selection and reusable property bases.
- `Disassembly`, `ControlFlow`: Instruction decoding, normalization and control flow analysis.
- `Elf`, `Pe`: Binary readers; `Relocations` names machine-specific relocation types.
- `Golden`: Golden file format; `TargetChecks/GoldenDisassembly` compares or updates it.
- `Context`: Manifest, object, artifact and tool access.
- `Properties`, `Mutate`, `Layout`, `CallGraph`: Shared checks, mutant builders, ABI layouts and call-chain analysis.
- `TargetChecks`: Registered checks for both ISAs.
