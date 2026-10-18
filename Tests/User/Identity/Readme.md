<!-- SPDX-License-Identifier: Apache-2.0 -->

# Identity tests

| Suite | Contract |
| --- | --- |
| FormatTests | Complete slot layout, CRC integrity, and semantic validation |
| CrashTests | Write/flush failure atomicity and torn-write recovery |
| ModelTests | Seeded account/group/membership operations, reopen, and capacity limits |
| LimiterTests | Delay boundaries, cancellation, restart, clock failure, and exhaustion |
| AuthenticationTests | RFC 9106 §5.3, enrollment, verification, dummy hashes, and cancellation |

Authentication tests apply production login parameters and verify the secure erasure
of both the private workspace and the 64 MiB allocation arena. Failure outputs
retain sentinel values. Test credentials and deterministic entropy sources are
synthetic, and diagnostic messages omit secret material.
RFC 9106 test values retain BSD-3-Clause attribution alongside Apache-2.0 test logic;
see [NOTICE](../../../NOTICE).

CTest automatically discovers these test suites. Target builds compile the code
at both `-O0` and `-O2` under the 512-byte per-function stack limit. Hosted suites
do not establish root-started VM memory admission or cleanup; those behaviors
require VM verification. See [Identity](../../../Documentation/Architecture/Identity.md).
