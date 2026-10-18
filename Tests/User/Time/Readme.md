<!-- SPDX-License-Identifier: Apache-2.0 -->

# Time library verification

CTest discovers `CivilTests`, `TimeZoneTests` and `TzifValidationTests`.
Fixtures generate TZif bytes without external timezone data or host APIs.
Civil models count years/months from 1970; zone models scan transitions and
calendar days independently of production formulas.

Validation covers RFC 8536 §§3.1–3.3.1, truncation, count limits, integer extrema
and inverse-conversion containment. Seeded random/mutated inputs exercise both
parser rejection and successful conversions. Default seed ranges are Civil
8536..8543, zone 28536..28543 and validation 38536..38543;
`ASTERIA_TEST_SEED` selects one seed. Failures report seed and step.

After building:

```sh
ctest --preset host-debug -R 'User/Time/' -j 4
```

`host-release`, `host-thread` and `host-analyze` provide optimized, ThreadSanitizer
and static-analysis checks. AMD64/IA-64 builds enforce O0/O2 stack budgets.
