<!-- SPDX-License-Identifier: Apache-2.0 -->

# IA-64 BSP runtime-memory owner

`Ia64BspRuntimeMemory` is the page-aligned, allocation-free integration owner
for the boot processor's boot-integrated runtime-translation state and common
bootstrap/idle root. It has boot lifetime, is BSP-owned, and freezes the exact
capability profile of every enabled processor. Only the selected BSP is
initially installed in the runtime IVT and resident in the common root. It
owns the system's unique monotonic region-identifier allocator, active
runtime-translation bindings, alternate-IVT object and its `ar.k3` contexts,
one common runtime page-table backend and `AddressSpace`, one private
initial-component backend and `AddressSpace`, one BSP
`ProcessorAddressSpaceController`, and fixed per-AP controller storage.
Independent owners or allocators are
forbidden because they could issue duplicate private RIDs or publish
conflicting refill state.

The owner derives the exact Region-0 runtime-object corridor from its
own page-aligned, page-multiple object extent. Callers cannot widen, relocate,
or substitute that corridor. The complete owner, embedded common objects,
bindings, alternate-IVT contexts, backend, region set, and private ledger must
lie inside it, and the retained initial preparation must prove complete pinned
shared-Global data coverage for the entire extent. Numerical placement alone
is insufficient. The owner, preparation and sealed plans, supplied
`ObjectRegistry`, and its `RootCapabilityIssuer` remain immovable and alive for
the complete boot lifetime and none may occupy a target stack.

`PrepareAndAdmit` first performs the complete IVT preflight without changing
the owner. A rejected preflight leaves `Fresh` retryable. After that boundary,
the transaction is terminal on every failure. Its primary entry accepts the
full enabled-processor mask and fixed four-slot PAL array, validates and freezes
every selected record, and derives the common minima/intersections. The common
RID width initializes the unique RID allocator. The owner then allocates one
never-reused eight-RID receipt, initializes the common backend against the exact
sealed preparation, retrieves that same seed plan, and has the embedded common
`AddressSpace` claim it with the supplied resource charge owner. It installs
the alternate IVT and admits only the BSP, registers the common object through
the supplied root issuer, binds the registry-backed identity, and calls
`ProcessorAddressSpaceController::Initialize` to activate that exact object as
the BSP's bootstrap/idle root. Before `Ready`, the embedded
`Ia64RuntimeAddressSpaceOwner` consumes the allocator's next complete
eight-RID receipt, initializes and claims a distinct backend with the same
sealed shared-Global Region-0 seed, and registers/binds a second AddressSpace.
That private root starts unresident and the controller's idle root remains the
common object. A compatibility overload synthesizes the same
profile for an SMP1/BSP-only caller.

Success is not inferred from the common receipt alone. Before publishing
`Ready`, the owner checks the exact RID receipt and allocator snapshot, common
`AddressSpace` identity, mapping count, claimed backend and BSP residency,
controller receipt and snapshot, the common backend's private ledger binding
for that exact processor, the allocator's exact two-receipt count, and the
private root's distinct identity, canonical seed count, and zero residency. A
proven precommit activation rejection and a
commit-unknown activation are both terminal for this bootstrap owner. On the
proven precommit path, the common controller may release only its provisional
destination-transition reservation; the earlier backend claim, IVT admission,
object registration, and RID allocation remain retained. A commit-unknown
activation retains conservative residency and every possibly live native
object. The owner does not roll back committed or possibly committed state,
destroy or quiesce the common object, reclaim resources, release the backend,
or reuse the private RIDs. The owner, registered object, registry, controller,
backend, and preparation are retained for the boot lifetime.

After that `Ready` commit, `BorrowReadyRoot` supplies the BSP kernel-runtime
coordinator with one `Ia64BspRuntimeRootView`. The passive view names the exact
embedded `AddressSpace` storage and its registry-backed identity, the exact
embedded `ProcessorAddressSpaceController`, and the admitted BSP processor.
The pointers are mutable, non-owning boot-lifetime borrows: the view neither
transfers ownership nor pins, registers, copies, or allocates either object.
Fresh and Failed owners return `NotReady`. A nominally Ready owner returns
`InvariantMismatch` instead of lending authority unless the embedded root is
Ready with the published identity and the Ready controller still names that
same root as its initial idle and active address space for the same processor.
The caller externally serializes the borrow and all later controller use,
keeps execution on the owning BSP with the required interrupt discipline, and
must not treat the view as AP, cross-CPU, or lifetime authority.

`BorrowReadyInitialComponentRoot` returns the distinct private AddressSpace
with that same BSP controller and processor. It accepts only either stable
state: common active/private unresident, or private active only on the BSP.
Only the common root is the controller's idle object. The private owner and
its backend are embedded before IVT admission inside the enlarged
`Ia64BspRuntimeMemory` object corridor, so every activation ledger and region
set is covered by the already verified pinned Region-0 mapping.

The Ready owner also retains independently release-published IVT transports.
The existing break transport owns its validated entry frame,
16-KiB memory stack, and 32-KiB RSE range. The external-interrupt transport owns
one aligned 32-byte context and one aligned 64-byte timer-evidence mailbox for
each of the four processor slots. `PublishExternalInterruptEntryContext`
accepts any profiled, resident processor with an installed IVT context, proves
both exact ranges remain inside the owner's locked runtime corridor, requires
the evidence to be zero, and then publishes that processor-local context
through the fourth `ar.k3` word while interruption delivery remains disabled.
Snapshot acquisition observes the evidence sequence with acquire semantics and
never returns partially published non-sequence fields. The no-processor
snapshot and acknowledgement wrappers retain the independent BSP timer path by
selecting slot zero; AP calls use the explicit processor overloads. These
additional objects remain inside the runtime owner.

`BootApplication` defines the BSP registry lock, `ObjectRegistry`, and
`RootCapabilityIssuer` as namespace-scope `constinit` objects. The owner's
boot-lifetime storage is the run-time memory reservation
([capacity](#slot-capacity-and-the-translation-register-budget)), allocated
after `ExitBootServices`, including the bootstrap data prefix and page-end guards. After the physical
execution-context handoff, the AP takeover path acquire-validates every enabled
AP's terminal PAL snapshot and retains a fixed four-slot profile plus exact
mask. The continuation clears only the owner extent between the live 128 KiB bootstrap data
prefix and the page-top 32 KiB RSE, placement-constructs `Ia64BspRuntimeMemory` there and passes that complete profile.
Before constructing the runtime-memory owner, the continuation constructs,
initializes, and registers the real root `ResourceDomain`. The BSP call passes
that root's registered `ResourceDomainId`, so the bootstrap/idle
`AddressSpace` is charged to the same real resource hierarchy later used by
`Ia64BspKernelRuntime`. A successful
`PrepareAndAdmit` therefore executes the ordinary common path: backend claim,
alternate-IVT install and admission, root-capability registration,
registry-backed identity binding, controller activation, and exact private-
ledger postcondition verification. The continuation then calls
`ConfigureTranslationCachePurge` with the BSP's retained PAL_PTCE_INFO loop,
which names one `Ia64LocalTranslationCachePurge` as the purge of the common,
initial-component and pager roots: a root that unmapped while it was not
current purges the local translation cache when it next activates
([backend](Ia64RuntimeAddressSpacePageTable.md)). The caller then placement-constructs the
separate initial-user memory owner and materializes the staged arena as one
registered aggregate Frame and registered pinned MemoryObjects for its
nonempty ranges against the same root ResourceDomain. This object step maps and
launches nothing. A separate retained mapping owner then installs those ranges
in the nonresident private initial-component root while preserving the stack
and RSE guard holes; it still creates no launch state. Each transaction
reports its stage event when it completes
(`ASTERIA:STAGE:BSP-RUNTIME-MEMORY`,
`ASTERIA:STAGE:INITIAL-USER-IMAGE-OBJECTS`, and
`ASTERIA:STAGE:INITIAL-USER-IMAGE-MAPPINGS`), and translation activation
follows only after all three succeed.

The caller next invokes `InitialTranslationActivator`. Its no-return `rfi`
enables translation and enters `AsteriaIa64InitialTranslatedExecution`, which
calls the translated kernel continuation. That continuation first maps each
AP's physical idle and worker storage into fixed Region-1 aliases in the shared
root. It then uses the retained real resource root, constructs the separate BSP
kernel-runtime owner, and prepares either the default initial-image request or
the explicit synthetic CPL3 diagnostic. After that preparation, the
BSP sequentially calls `AdmitApplicationProcessorPhysicalRoot` for CPUs 1
through 3. Before each pivot this owner publishes that AP's Region-0 interrupt
transport; the AP context owner then pivots the admitted CPU to its mapped idle
stack/RSE for the one-shot local-timer proof. With `PSR.i` clear afterward, the
AP borrows the exact admitted shared root/controller, constructs its fixed-
storage kernel-only scheduling graph with an empty `TimerQueue` and
`ProcessorRunController` in `HardwareDeadline` mode, and uses
`Ia64ProcessorRunLoop` to admit one retained worker at run sequence one. The
worker blocks and idle validates canonical no-runnable state, the empty queue,
disarmed timer, and unchanged root. The AP then polls its reusable BSP-to-AP
service mailbox in a boot-lifetime `IdleServing` loop with `PSR.ic=1` and
`PSR.i=0`. The owning AP acquire-observes one external request and uses the
retained worker's exact block token to wake and re-enqueue it, resumes the same
native context at sequence two, and lets it block again. After validating this
service proof against the kernel's contracts and recording it in the evidence
record, the BSP acknowledges the observed `0xe8` sequence and publishes the
separate one-shot timer-delivery request. Each AP handles it
in `IdleServing`, drains exactly one Timer-to-Notification delivery at exact
`0xe8` sequence two through boot-lifetime per-AP common timer objects, consumes the
Notification, and restores the graph to empty/disarmed without changing the
blocked worker or scheduler-service counters. The BSP validates the immutable
completion, records it, and acknowledges the observed sequence; the VM runner
requires the recorded pairs `1/1` and `2/2`. Only after all APs
publish this proof and remain `IdleServing` does the default BSP use the common
processor run loop to enter `MinimalUserImage` in the private initial-component
root. QEMU proves virtual BSS initialization, retained Query/send/receive
responses, the canonical outbound frame and trusted loopback delivery, the
completion sentinel, and the linked PL3 idle loop. The synthetic option instead executes
the BSP direct local-timer proof; its Wait syscall
restores the supervisor; idle code handles a second timer sequence, wakes and
readmits the worker through the retained deferred-worker/WaitSet composition,
and resumes the original PL3 syscall. The default QEMU boundary is the stopped
`AsteriaIa64MinimalUserImageIdle` loop, not the legacy kernel-idle or synthetic
probe loop. Hosted tests still cover the
successful registered first-root transaction, retryable preflight rejection,
invalid and heterogeneous full-processor profiles, common RID-width reduction,
invalid charge ownership, precommit activation rejection, and commit-unknown
retention without executing privileged instructions. This is a live BSP
bootstrap/idle `AddressSpace` handoff plus one BSP run-loop/user transition,
not general SMP scheduling support. The
implemented SAL/Local-SAPIC path completes AP physical stack/RSE, fail-stop IVA,
PAL-capability takeover, and sealed initial RR/TR installation before parking.
The later translated sequence publishes one dedicated Region-0
external-interrupt context/mailbox before each AP pivot, installs and admits
the shared runtime IVT/root, performs one exact-sequence-one AP-local-timer
proof, and establishes the persistent cooperative scheduler service described
above. After stable acknowledgement of sequence one, the separate AP timer-
delivery request uses that processor's boot-lifetime `Timer`, `Notification`,
`TimerQueue`, `ProcessorDeadlineController`, and `DeferredTimerWorker` in the
cooperatively polling idle context. This does not supply timer-driven AP
scheduling, periodic policy, a general timer service, general interrupt routing,
interrupt preemption, IPI wake, migration, load balancing, cross-processor
shootdown, general mapping, hotplug, or teardown.
The BSP's two bounded local-timer deliveries around its CPL3 launch remain an
independent slot-zero transaction. Kernel copies to the private user virtual
stack occur only during a bounded private-root selection and always restore
the common idle root before scheduler work resumes.

## Slot capacity and the translation-register budget

The owner holds sixteen [run-time AddressSpace slots](Ia64RuntimeAddressSpaceSlots.md),
one per `AddressSpacePool` slot, and thirty-four entry slots: the initial user's,
the boot pager's and one per created-thread slot (32, following `ThreadPool`).
Each entry owns a 16 KiB memory stack, a 32 KiB RSE backing store, contexts and
frames. The runtime IVT has thirty-four selectable context pairs per processor.
The pager grant admits up to 32 created threads. The Frame/MemoryObject and
ObjectRegistry slabs live after this owner in the same DTR0 reservation.
Every slot stays in this owner because the IVT reaches it with `PSR.ic` clear: the refill handler reads
the bound root's ledger and region set, and the break, user-fault and PL3-interrupt entries switch to
the slot's stack and RSE store and write its frame before collection is back. A miss there cannot be
taken: the VHPT walker is off, and the alternate-TLB refill inserts only what the active root's ledger
records. So nothing an entry touches can move to a refilled mapping, and the whole owner must be
mapped by locked DTRs.

The owner therefore lives in its own reservation, not in the image: one naturally aligned page of a
PAL-insertable size that one locked DTR maps
([translation-register budget](Ia64TranslationRegisterBudget.md)). After `ExitBootServices` the boot
allocates it from the final map's Conventional memory, aligned to its own size: the 16 MiB page
when a guarded free range holds it, otherwise the smallest architected insertable size holding the
128 KiB bootstrap prefix plus the owner, slabs, separation page and page-top 32 KiB RSE (`SelectRuntimeMemoryReservationPageShifts`). The bootstrap
mapping plan maps that whole page as one RW/NX Global identity mapping, and every AP installs the
same plan. The stack, RSE and translation storage share its DTR; the owner retains an exact corridor
at page +128 KiB and never clears the live prefix. Unmapped guards outside both page ends remain
allocator-owned and excluded from every seed ledger. See the budget document for the retained
alignment slack, layout and fallback rules.

The owner is 834 pages (3,416,064 bytes). The full layout needs 6,270,976
bytes and refuses a 4 MiB fallback; native ELF placement retains the preferred
16 MiB runtime page. `Ia64RuntimeMetadataLayout.hpp` statically budgets
2,686,976 slab bytes and 10,506,240 bitmap bytes between the owner and RSE. Image placement and bootstrap consolidation make the complete native
plan exactly **4 DTRs and 2 ITRs**, including reserved firmware ITR0, independently of source load
base. The residue proof runs both production planners with an 8+8 PAL report at every source
residue and requires that constant bound. The old hosted disjoint-reservation geometry remains a
characterization of the generic physical bootstrap interface; it does not select the native plan.
