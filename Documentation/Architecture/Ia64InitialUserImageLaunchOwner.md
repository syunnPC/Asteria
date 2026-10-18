<!-- SPDX-License-Identifier: Apache-2.0 -->

# IA-64 Initial User Image Launch Owner

## Scope

`Ia64InitialUserImageLaunchOwner` is the bounded, allocation-free preparation
boundary between the ready initial-image mappings and `Ia64UserModeLaunch`. It
does not allocate or map pages, create an AddressSpace, ProtectionDomain,
Thread, or SchedulingContext, or enqueue work. It reuses the private initial-
component root and object graph already owned by `Ia64BspKernelRuntime`.

The owner consumes no source pointer. It snapshots and cross-checks the ready
memory and mapping owners, the decoded P64 entry bundle and GP, every installed
range and guard, and the existing registry, revocation registry, capability
space, ProtectionDomain, ResourceDomain, and AddressSpace identities. It also
captures two caller-supplied `Ia64InitialUserImageBootstrapGrant`s: the
bootstrap grant, delivered in `r4`/`r5`, and the diagnostic grant, delivered in
`r6`/`r7`. Each grant names an object in the exact runtime registry, its
expected object type, exact rights, lease deadline, and sealed selector; the
two grants name distinct objects. The selected code, memory-stack,
and RSE ranges become one validated `Ia64UserModeLaunchRequest`; the BSP
runtime's embedded worker stack/RSE ranges back its fresh supervisor
continuation.

## Authority and commit cut

Complete validation and authoritative-state revalidation occur before the
first persistent authority change. Grant preflight rejects, for either grant, a
foreign registry, invalid object identity or type, invalid or empty rights,
rights without `Inspect`, a zero lease deadline, an object-type mismatch, or a
target charged to a different ResourceDomain, and it rejects a diagnostic grant
that names the bootstrap target. The errors name the grant
(`InvalidBootstrapGrant`/`BootstrapTargetRejected` or
`InvalidDiagnosticGrant`/`DiagnosticTargetRejected`). A write-only
DiagnosticChannel grant is therefore rejected: the diagnostic capability
carries `Inspect|Write`. Retained `ObjectPin`s hold the exact target generations
stable. A rejection in that phase records a diagnostic, releases the
provisional target pins, and leaves the owner `Fresh`, so the caller may
correct its inputs and retry.

Immediately before creation of the first revocation root the owner changes to
`Failed`. It creates both revocation roots, bootstrap first, before minting
either capability, so a shortage of roots never leaves the image with only one
of them. Any later rejection is terminal and conservatively retains the issued
revocation domains, capabilities, and target pins for boot lifetime. In
particular, the CapabilitySpace clock is authoritative for a finite deadline;
if it observes a grant as expired during mint, the failure is terminal after
the commit cut, and an expired diagnostic grant leaves the already minted
bootstrap capability in place.

Success mints exactly two capabilities matching the captured grants, each in
its own revocation root, queries their complete descriptors through the
required `Inspect` right, stores the bootstrap selector and authenticator in
initial `r4` and `r5` and the diagnostic ones in `r6` and `r7`, and publishes
the owner as `Ready`. The bootstrap capability is minted first, so it takes
the first selector of the fresh capability space. The ProtectionDomain remains
the execution identity even when the bootstrap target has another object type.
The request installs the decoder's GP in user `r1`; no zero-GP compatibility
assumption is made for the staged image. Preparation does not change
mappings, registry object count, scheduler state, or the existing runtime
identities.

## Boot integration and limits

With the default `ASTERIA_IA64_SYNTHETIC_USER_PROBE=0`, the translated boot
continuation calls `PrepareInitialUserImage`, reports the
`ASTERIA:STAGE:INITIAL-USER-IMAGE-LAUNCH-PREPARED` event, completes the retained
AP and shared-root checkpoints, reports `ASTERIA:STAGE:INITIAL-USER-IMAGE-LAUNCH`
(the kernel's last stage), and dispatches the existing BSP worker. The worker
performs the one-way native PL3 launch. The production `MinimalUserImage`,
staged from `USER.ELF`, observes its virtual BSS, queries the caller-selected bootstrap Endpoint, sends
one canonical frame, validates its exact loopback delivery, retains all
evidence, and remains in its stable PL3 idle loop. It ignores the
DiagnosticChannel in `r6`/`r7`, which a test image uses for its report.

Setting `ASTERIA_IA64_SYNTHETIC_USER_PROBE=1` selects the synthetic
Query/send/receive/wait-resume diagnostic instead; the two preparations are
mutually exclusive. This owner prepares only the initial image's launch.
Production boot separately attaches the [boot pager](BootPager.md), which is
admitted before that image and supplies the root service and component launch.
The corresponding AMD64 path is described in [AMD64 runtime](Amd64BootRuntime.md).

Hosted tests cover exact range selection, GP and four-word argument transport,
execution-identity and registry binding, a non-ProtectionDomain bootstrap
target, exact type/rights/deadline/sealed-selector transport for both grants,
separate revocation roots, charge ownership, mapping and guard revalidation,
retryable failures of either grant (including a write-only or shared
diagnostic grant), finite-expiry and revocation-exhaustion commit cuts at the
bootstrap and at the diagnostic step, retained terminal failures, success
postconditions, and replay rejection. The VM runner additionally requires
the two launch stages once each and in order, the TAP report in which the test
firmware's guest program checks its BSS, Query/send/receive responses, and the
delivered frame through the system-call ABI, and three stable observations of
the linked PL3 idle bundle.
