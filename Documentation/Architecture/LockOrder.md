<!-- SPDX-License-Identifier: Apache-2.0 -->

# Spin-Lock Order

## Scope

This document fixes the system-wide acquisition order of every
`Platform::Synchronization::SpinLock` in the kernel, platform, and boot sources. Component documents state their own
local orders; this document joins them into one partial order that a debug build can check.

`SpinLock` is a non-recursive atomic owner-word lock. It does not mask interrupts and gives no fairness guarantee.
Zero denotes unlocked, processor index plus one denotes its owner, and all ones denotes an unknown owner before
processor identity is installed. Contention uses `CrossProcessorWaitBudget`; after identity is installed, expiry
stops for a lost owner or an invariant violation. The order below forbids every nesting of two locks of the same
class, including two instances of that class.

Every acquisition in the tree uses the scoped `SpinLockGuard`. Nothing calls `SpinLock::Acquire` or `Release()`
directly. `Release()` calls in `ObjectRegistry.cpp` and `Interrupt.cpp` belong to a pin counter and to an
`IInterruptSource`, not to a spin lock. A lock is therefore held exactly for the lexical scope of its guard, and
nesting comes only from calls made inside that scope.

Processor placement runs under the rank-1 ThreadPool reservation. It reads atomic per-home binding counts and
reserves capacity before the pool unlocks without a separate placement lock.
Device-authority scans release Shared before inspecting a DeviceRegion.
Mapping scratch and fault continuation receipts are processor/thread local.
A pending source shootdown releases its transition receipt and returns a
rollback-safe refusal; the unlocked idle-root retry drains the actual mailbox
before trying again. Private idle-root adoption follows the same lock ranks.

## Lock classes

A lock class is one of the following:

- every instance that plays the same role; or
- one instance shared by several owners.

These locks are borrowed rather than owned:

- **Shared object/capability/revocation lock.** `ObjectRegistry`, `RevocationRegistry`, and every `CapabilitySpace`
  of one kernel hold the same lock by reference. A `CapabilitySpace` takes it from its `RevocationRegistry`. Its
  configuration is valid only if the registry's lock is the same object (`CapabilitySpace.cpp:208-212`). `Endpoint`
  holds no lock of its own: it locks its receiver space's lock. Capability transfer and Endpoint sends reject a
  source space that uses a different lock, so two instances of this class are never held together
  (`CapabilityTransfer.cpp:66`, `Endpoint.cpp:289`, `Endpoint.cpp:469`). There is one instance per kernel image:
  - IA-64: `g_ia64BspRegistryLock` (`BootApplication.cpp:216`);
  - AMD64: `BootRuntime::m_registryLock` (`Amd64BootRuntime.cpp:229`).

  `PciEcamRuntime` (`PciEcamRuntime.cpp:13`) and `Ia64BspKernelRuntime` (`Ia64BspKernelRuntime.cpp:649`) bind their
  `RevocationRegistry` to this same lock. See [CapabilityModel](CapabilityModel.md) and
  [EndpointQueue](EndpointQueue.md).
- **ResourceDomain ledger lock.** Every `ResourceDomain` of one hierarchy locks `m_ledger.m_synchronization`
  ([ResourceDomain](ResourceDomain.md)).
- **ProcessorScheduler lock.** `ProcessorSchedulerWakeFacade` locks the lock of the scheduler that owns it
  (`ProcessorScheduler.hpp:360-394`).

Every other class owns a private `m_synchronization`. The exceptions are `Notification`, whose lock is named
`m_claimLock`, and `ProcessorSet`, whose lock is named `m_lock`.

## Order

The order is a partial order. Rank 1 is outermost. The rule is:

> While a CPU holds a lock of rank *r*, it may acquire only a lock of rank greater than *r*.

Locks of the same rank are mutually unordered and must not be nested. The "acquires while held" column lists each
nesting observed in the source, including nesting through callees. Any other lock of greater rank is permitted by the
rule but does not occur in the implementation.

| Rank | Lock class | Instances / sharing | Protects | Acquires while held |
| ---: | --- | --- | --- | --- |
| 1 | `DeviceManager` | one per manager | PCI slot table; DeviceRegion and PciFunction creation, retirement, and capability minting | PciFunctionCatalog, PciFunction, DeviceRegion, ResourceDomain ledger, Shared |
| 1 | `AhciPciBootstrapBinding` | boot-only, one | one-shot AHCI endpoint selection and BAR binding | PciFunctionCatalog, PciEcamConfigurationSpace, PhysicalMemoryMappedIo, DeviceRegion |
| 1 | `IommuDomainPool` | one per pool | its IommuDomain slots, their backends and identities ([driver isolation](IommuDomain.md)) | PciFunctionControl (as `IIommuDomainQuiescer`), IommuDomain, ResourceDomainPool (a drain), ResourceDomain ledger, Shared |
| 1 | `InterruptPool` | one per pool | its Interrupt slots, their vectors and message state ([driver isolation](IommuDomain.md)) | PciFunctionControl, Interrupt, ResourceDomainPool (a drain), ResourceDomain ledger, Shared |
| 1 | `ThreadPool` | one per pool | its Thread and SchedulingContext slots, their charges and domain pins | native reservation (ProtectionDomain, AddressSpace, SchedulingContext, Thread), ResourceDomainPool (a drain), ResourceDomain ledger, Shared |
| 1 | `PageFaultIngress` | one per ingress | the prepared fault: its token, factory slot, pager delivery, and rollback | FaultTokenFactory, FaultToken, PagerFaultDispatcher, SchedulingContext, Thread, Shared |
| 2 | `PciFunctionControl` | one per ObjectManager | the four function assignments, their assignee, revoked latch and attached IommuDomain, the bus-master Command writes ([driver isolation](IommuDomain.md)), and the Frame pins drains parked with quarantined functions ([AHCI block I/O](../../Source/User/System/AhciPort.hpp)) | IommuDomain (attach, detach, quiesce, which parks pins back into this table while both locks are held), PciFunction, PortPciConfigurationSpace, ProtectionDomain (an assignee's binding), ResourceDomainPool (a drain's subtree), ResourceDomain ledger, Frame and Shared (a confirmation releasing parked pins), Shared |
| 2 | `PciFunctionCatalog` | one | discovery and queries of the immutable function catalog | PciEcamConfigurationSpace, PhysicalMemoryMappedIo, DeviceRegion |
| 2 | `PagerFaultDispatcher` | one per dispatcher | the blocked-faulter-to-pager transaction | FaultToken, ProcessorScheduler (wake facade), ReadyQueue, SchedulingContext, Thread, Shared |
| 2 | `SynchronousCallDispatcher` | one per dispatcher | the caller-side Call transaction | SynchronousReplyDispatcher, ReplyToken, ReadyQueue, SchedulingContext, Thread, Shared |
| 2 | `WaitSetThreadDispatcher` | one per CPU continuation | the WaitSet wait/suspend/wake transaction | WaitSet, Notification, ProcessorScheduler (wake facade), ReadyQueue, SchedulingContext, Thread, Shared |
| 3 | `AddressSpacePool` | one per pool | its AddressSpace slots, their charges, domain pins and provider arenas ([domain creation ABI](SystemCallAbi.md)) | the provider's AddressSpace (`Initialize`, `Quiesce`, `GetSnapshot`) and its backend, Frame, MemoryObject and DeviceRegion through the quiesce, ResourceDomainPool (a drain), ResourceDomain ledger, Shared |
| 3 | `IommuDomain` | per domain | DMA translation records, attachments, and backend ownership | PciFunction (as `IIommuDevice`), VtdDomainBackend (as `IIommuDomainBackend`), Frame, ResourceDomain ledger, Shared |
| 3 | `PciEcamConfigurationSpace` | per ECAM window | one checked configuration reader | PhysicalMemoryMappedIo, DeviceRegion |
| 3 | `PortPciConfigurationSpace` | one per AMD64 kernel | each 0xCF8/0xCFC address-then-data pair of the run-time configuration reader and writer ([driver isolation](IommuDomain.md)) | none |
| 3 | `PciEcamRuntime` AHCI command lock (`m_ahciCommandSynchronization`) | boot-lifetime, one | the IDENTIFY and READ DMA EXT one-shot command state | PhysicalMemoryMappedIo, DeviceRegion |
| 3 | `ProcessorScheduler` (+ wake facade) | per CPU | the CPU-local scheduler state machine, its embedded ReadyQueue transitions, and DispatchCoordinator use | DispatchCoordinator, ReadyQueue, SchedulingContext, Thread, Shared |
| 3 | `SynchronousReplyDispatcher` | one per dispatcher | the server-side reply/cancel transaction | ReplyToken, ReadyQueue, SchedulingContext, Thread, Shared |
| 4 | `AddressSpace` | per root | mapping records, resident mask, transition reservation, and shootdown state | AMD64/IA-64 page-table backend, Frame, MemoryObject, DeviceRegion, Shared |
| 4 | `DispatchCoordinator` | per CPU | one Thread run joined to one SchedulingContext | SchedulingContext, Thread, Shared |
| 4 | `WaitSetPool` | one per pool | its WaitSet slots, their charges and domain pins | WaitSet, ResourceDomainPool (a drain), ResourceDomain ledger, Shared |
| 4 | `EndpointPool` | one per pool | its Endpoint and readiness Notification slots, their charges and domain pins | Notification, ResourceDomainPool (a drain), ResourceDomain ledger, Shared |
| 4 | `FrameObjectPool` | one per pool | its Frame slots, including Zeroing slots, runs, charges and domain pins | PhysicalPageAllocator, Frame, DeviceRegion (a device Frame's snapshot), ResourceDomainPool (a drain), ResourceDomain ledger, Shared |
| 4 | `CapabilitySpacePool` | one per pool | its CapabilitySpace slots, their charges and domain pins | ResourceDomainPool (a drain), ResourceDomain ledger, Shared |
| 4 | `ProtectionDomainPool` | one per pool | its ProtectionDomain slots, their records' binding, charges and pins | ProtectionDomain (initialize, quiesce), ResourceDomainPool (a drain), ResourceDomain ledger, Shared |
| 4 | `MemoryObjectPool` | one per pool | its MemoryObject slots | MemoryObject, ResourceDomainPool (a drain), ResourceDomain ledger, Shared |
| 4 | `FaultTokenFactory` | one | its sixteen token slots and claim generations | FaultToken, Shared |
| 4 | `VtdDomainBackend` | one per IommuDomain slot (AMD64) | its owner, attached requesters, and second-level table pages ([driver isolation](IommuDomain.md)) | VtdUnit |
| 4 | `PciFunction` | per function | function state, its driver assignment, and IOMMU ownership | ProtectionDomain, ResourceDomain ledger, Shared |
| 4 | `PhysicalMemoryMappedIo` | per accessor | MMIO window validation and access | DeviceRegion |
| 4 | `ReadyQueue` | per CPU | queue slots, claims, and enqueue reservations | SchedulingContext, Thread, Shared |
| 4 | `ReplyToken` | per token | the armed reply state and context donation | SchedulingContext, Thread, Shared |
| 5 | `VtdUnit` | one per VT-d unit (AMD64) | the unit's registers, root and context tables, and fault counters; each register command spins a bounded time | none |
| 5 | `DeviceRegion` | per region | region state, mapping count, and charge | ResourceDomain ledger, Shared |
| 5 | `FaultToken` | per token | fault state and resolution claim | Shared |
| 5 | `Frame` | per frame | page reservations, boot subrange records and created-Frame W/X counters | Shared |
| 5 | `Interrupt` | per interrupt | the source binding (normal-context operations only) | Shared |
| 5 | `MemoryObject` | per object | backing state and charge | ResourceDomain ledger, Shared |
| 5 | `ResourceDomainPool` | one per pool | its child-domain slots, their parent charges and pins, the identity counter | ResourceDomain ledger, Shared |
| 5 | `ProtectionDomain` | per domain | domain state and device-use references | ResourceDomain ledger, Shared |
| 5 | `SchedulingContext` | per context | budget, binding, donation, and queue membership | Shared |
| 5 | `TimerQueue` | per CPU | the 32 queue entries and the earliest deadline | Timer, Shared |
| 5 | `WaitSet` | per WaitSet | membership and the readiness cursor | Notification, Shared |
| 6 | Shared object/capability/revocation lock | one per kernel (see above) | object slots and pins, capability slots, revocation epochs, Endpoint queues | none |
| 6 | ResourceDomain ledger | one per hierarchy | quota counters of every domain in the tree | none |
| 6 | `Thread` | per thread | lifecycle state and queue membership | none |
| 6 | `Timer` | per timer | deadline, generation, and queue binding | none |
| 6 | `Notification` (`m_claimLock`) | per notification | claim, commit, and cancel; `Signal` is lock-free | none |
| 6 | Address-space backends (`Amd64::RuntimeAddressSpacePageTable`, `Ia64RuntimeAddressSpacePageTable`) | one per AddressSpace | native page-table records and activation | none |
| 6 | `Ia64RegionIdentifierAllocator` | one | RID allocation | none |
| 6 | `PhysicalPageAllocator` | one per allocator; FramePool is a facade with no lock | buddy free bitmaps, dirty, allocated, claimed and retired pages | none |
| 6 | `ProcessorSet` | one runtime set | admission and contained-retirement states; masks have lock-free acquire readers | none |
| 6 | Capability-space and revocation-registry identifier locks | boot globals | identifier counters during construction | none |

Interfaces that are called while a lock is held must also stay below that lock. Each interface below is called with
the named lock held and must not acquire any lock that ranks at or above it:

| Lock held | Interface called | In-tree implementations |
| --- | --- | --- |
| `AddressSpace` | `Hal::Memory::IAddressSpacePageTable` | the two rank-6 backends |
| `IommuDomain` | `IIommuDevice` | `PciFunction` |
| `IommuDomain` | `IIommuDomainBackend` | `VtdDomainBackend` (rank 4; it reads the requester identifier before its lock and takes the rank-5 `VtdUnit`) |
| `IommuDomain` | `IDmaReservations` | `FirmwareDmaReservations` (immutable after boot; takes no lock) |
| `IommuDomainPool` | `IIommuDomainQuiescer` | `PciFunctionControl` (rank 2) |
| `IommuDomain` | `IIommuStorageParker` | `PciFunctionControl`'s drain parker (takes no lock: its caller, the drain's quiesce, already holds the rank-2 `PciFunctionControl` lock, and moving a pin takes none) |
| `AddressSpacePool` | `Memory::IAddressSpaceProvider` | AMD64 `RuntimeRoots`, `Ia64RuntimeAddressSpaceSlots` (each initializes and inspects its slot's rank-4 AddressSpace) |
| `ProtectionDomainPool` | `Protection::ICreatedDomainRecords` | `Amd64UserThreadTable`, `Ia64UserThreadTable` (lock-free; binding a record's accessor takes no lock) |
| every object pool (`DestroyCharged`) | `Resource::IResourceDomainSubtree` | `ObjectManager`'s drained subtree, which takes the rank-5 `ResourceDomainPool` lock |
| `Interrupt` | `IInterruptSource` | none in tree |
| PCI config and MMIO locks | `IMemoryMappedIo`, `IMemoryMappedIoWriter`, `IPhysicalMemoryAccess` | lock-free beyond PhysicalMemoryMappedIo and DeviceRegion |
| Shared and others | `IMonotonicClock::GetTicks` | lock-free in every implementation |

## Named paths

- **Terminal power parking ([shutdown](PowerOff.md)).** A lock-free
  fence closes placement, request and wake admission before the freeze. Each
  CPU publishes its parked bit before acknowledgement, even when NMI parking
  interrupts a guard. Pending storage is retained; no lock, remote-request
  wait, detach or reclamation runs on this terminal path. Ranks are unchanged.

- **RAM Frame creation.** FrameObjectPool reserves a slot, pins its domain,
  charges it and allocates under its rank-4 lock. It marks the slot Zeroing, releases the lock and zeroes
  dirty pages in chunks of at most 16. Neither platform scrubber takes an AddressSpace lock. Publication
  reacquires the pool lock to register and mint; Collect skips Zeroing and a subtree drain returns Busy.
  The allocator never touches physical pages and never calls back into any owner.

- **Endpoint.** Every operation holds the receiver space's Shared lock and calls only the `*Locked` forms of the
  registry and capability space, so Shared is never reacquired. Pins and capability moves happen inside that one
  critical section.
- **WaitSet wait and wake.** `WaitSetThreadDispatcher` → `WaitSet` → `Notification`. Wake goes through
  `ProcessorSchedulerWakeFacade`: `WaitSetThreadDispatcher.cpp:557` → `ProcessorScheduler.hpp:360` →
  `ReadyQueue.cpp:384` → SchedulingContext, Thread, and Shared. A `WaitSet` never calls into the scheduler.
- **Dispatch.** `ProcessorScheduler` → `DispatchCoordinator` | `ReadyQueue` → `SchedulingContext` | `Thread` |
  Shared (registry pins at `ReadyQueue.cpp:256` and `DispatchCoordinator.cpp:84`). `ProcessorRunController` and
  `ProcessorKernelRunLoop` have no lock. They acquire the scheduler and `TimerQueue` locks one at a time.
- **Timers.** `TimerQueue` → `Timer` and Shared. `DeferredTimerWorker` and `TimerNotificationDelivery` own no lock
  and take TimerQueue, Timer, Notification, and Shared one at a time. Delivery ends in the lock-free
  `Notification::Signal`.
- **AddressSpace shootdown.** `TlbShootdownDispatcher` and `FaultResolutionDispatcher` own no lock. No
  `ITlbShootdownTransport` or `ITlbShootdownDrain` call is reachable while any lock is held, so the wait for remote
  acknowledgements happens with no lock held ([AddressSpace](AddressSpace.md)). Root switches take the source and
  destination AddressSpace locks one at a time.
- **Devices.** `DeviceManager` → `PciFunction` | `DeviceRegion` → ledger | Shared. `PciFunction` → `ProtectionDomain`
  → ledger | Shared. ECAM reads nest catalog → `PciEcamConfigurationSpace` → `PhysicalMemoryMappedIo` →
  `DeviceRegion`. That last step only takes a snapshot.

## Cross-processor waits

`Tools/Verify/CheckLockOrder.py` checks GCC GIMPLE dumps through the `LockOrder`
CTest gate. Each guard constructor/destructor bounds a held scope, including
nested acquisitions. The check covers hosted, AMD64 and IA-64 variants.
Calls are closed transitively, overloads and ISA variants are merged by name,
and class dumps resolve base-subobject virtual slots. Native IA-64 slot indices
are divided by two because each entry is a code/GP descriptor pair. Identity,
clock and stop function-pointer bindings are derived from the dumps. An
unresolved pointer or virtual call reachable from a held scope fails the gate.

Forbidden targets include common progress and CrossProcessorWait polling,
every pager/mapping progress Wait, SwitchToIdleRoot, thread-request service
and kicks, AP startup/service waits,
IPI sends/delays, and shootdown transport, notifier and drain calls. Existing
MemoryMapper/PagerChannel completion paths reach those sinks transitively.
Terminal freeze uses `TrySendNmi`, which never polls, and can therefore stop a
lock holder without entering ordinary wait/progress. The foreign calls in
firmware-only `TopologyReport::FirmwarePci::Read32` are reviewed UEFI protocol
boundaries (UEFI Specification 2.10, §14.4 PCI I/O Protocol); no Asteria callback
is passed and APs have not been released.

PagerChannel's CAS transaction ownership introduces no lock rank.
ProcessorSet's admission/retirement lock reaches only local atomic state.
Mapping-intent and per-mapping use counts use the existing
AddressSpace rank-4 lock; no rank changes. `WaitForFaultMapping`,
`WaitForPinnedFrameMapping` and `WaitForUnmap` publish intent under that lock,
then poll common progress with no guard held. Intent refuses new transition
reservations and copy pins until the waiting mutation begins. Copies acquire
and release use counts in separate brief scopes; Commit/Cancel never waits.
Home-facade routing occurs before acquiring the destination scheduler lock.
Clock rendezvous polling also holds no lock; ProcessorSet's placement exclusion
updates only its atomic mask under the existing leaf lock.
ThreadRequest's single CAS word publishes its
payload and excludes retirement during a request. Run-loop binding credits
are atomic and retain pending reservations alongside live bindings. The
ThreadPool named paths reserve under a guard, call Start/Terminate without a
guard, and re-lock to publish/account. Reservation validates user input and
prepares native context storage without a cross-processor wait. Destroy and
subtree destruction refuse storage retained by an unlocked backend call;
HaveChargedExited refuses drain admission until that reservation is released.
A wake facade queues a kick while its wake transaction holds locks; unlocked
processor service sends it. ServiceThreadRequests, ServiceWaitingThreadRequests
and KickProcessor are explicit forbidden sinks, in addition to their transitive
progress/IPI paths. Home routing forwards once into a local facade operation;
IA-64 wait service has a non-switching call closure checked by the existing
borrowed-stack proof.

Debug common progress stops with InvariantViolated and the held depth when any
SpinLock is held. Cross-processor progress must run outside guard scopes.

Reproduction (after rebuilding both presets):

```
python3 Tools/Verify/CheckLockOrder.py --dumps Build/CMake/host-analyze/Source --dumps Build/CMake/ia64/Source --json Build/Tmp/LockOrder.json
```

## Interrupt context

1. **No interrupt handler acquires a spin lock.** None of the following reaches a `SpinLockGuard`:
   - `Interrupt::HandleInterrupt`;
   - `AsteriaAmd64HandleTlbShootdownInterrupt`;
   - the AMD64 timer vector in `BootRuntime::Trap`: `Amd64ApicTimer::AcknowledgeInterrupt`,
     `CaptureAmd64NativePreemption`, `Amd64WaitRuntime::OnTimerInterrupt`, and
     `Amd64ApplicationProcessorRuntime::OnTimerInterrupt`;
   - the IA-64 entries `AsteriaIa64ExternalInterruptEntry`, `AsteriaIa64ExternalInterruptDispatchEntry`,
     `AsteriaIa64ExternalPreemptionNativeEntry`, `AsteriaIa64UserInterruptClaim` (it accesses no memory), and
     `AsteriaIa64DispatchSalBootRendezvous`.

   `Notification::Signal` is the only producer that interrupt-driven delivery may call, and it is lock-free.
2. **Lock-taking trap paths start only from user mode, with local interrupts disabled.**
   - AMD64:
     - Every IDT gate is an interrupt gate (`Amd64UserEntry.cpp:149`, type `0x8E`).
     - `SFMASK` clears IF on `SYSCALL` (`Amd64UserEntry.cpp:197`).
     - `BootRuntime::Trap` reaches syscall dispatch only after `PrepareSystemCallReturn` accepts a user frame on the
       syscall vector (`Amd64UserEntry.cpp:163`). Any other vector fails stop.
     - A timer or reschedule IPI reaches `PreemptUser` only with CPL3 in the
       saved CS, on the owning processor's entry stack, with IF cleared by
       the interrupt gate. AP user timer interrupts follow the same path.
       Kernel-mode delivery stays in the lock-free acknowledge/pending path.
   - IA-64: the syscall entry sets only `PSR.ic` before it calls the bridge (`Ia64SystemCallEntry.S:605`), so
     `PSR.i` stays 0. The user-fault entry and the user interrupt entry, which a PL3 timer interrupt reaches through
     the dispatcher and `AsteriaIa64UserInterruptClaim`, share one body that does the same (`Ia64UserFaultEntry.S:831`).
3. **Preemption coordinators take locks only after a proven lock-free interruption.**
   - AMD64: `Probe::Capture` admits only a RIP inside `AsteriaAmd64PreemptibleLoop`
     (`Amd64NativePreemptionProbe.cpp:228`). That loop runs with IF set by design. Capture then IRETs into
     `AsteriaAmd64PreemptionCoordinatorEntry` with a synthesized `RFLAGS=2`, that is, IF clear
     (`Amd64NativePreemptionProbe.cpp:253`). The coordinator's IF=0 therefore comes from this rewrite, not from the
     interrupted context.
   - IA-64: `AsteriaIa64BspNativePreemptionCoordinator` is entered with `PSR.i=0`
     (`Ia64ExternalPreemptionEntry.S:1168-1181`), and only from the proven sentinel window
     (`Ia64ExternalPreemptionEntry.hpp:212-219`).

   Both coordinators then take the scheduler, ReadyQueue, DispatchCoordinator, AddressSpace, TimerQueue, and Shared
   locks in normal order.
4. **Kernel interrupt-enable windows hold no lock.** No guard scope can reach any of these windows:
   - `sti; pause; cli` in `Amd64WaitRuntime.cpp:178` and `Amd64ApplicationProcessorRuntime.cpp:211`;
   - the preemptible loop (`Amd64NativePreemptionProbe.S:33`);
   - the IA-64 sentinel landing (`Ia64BspKernelRuntime.S:260-271`);
   - the IA-64 polling leaf (`Ia64ExternalInterruptEntry.S:385`).

   The `(p8) ssm psr.i` in `Ia64PhysicalMemoryAccess.S:47/98` only restores the entry state of `PSR.i`. It runs
   under the PhysicalMemoryMappedIo lock and is correct only because callers enter with `PSR.i=0`
   ([PciEcamConfigurationSpace](PciEcamConfigurationSpace.md)).
5. **Native runtime paths acquire every lock with IF or `PSR.i` clear. AMD64 `PrepareAmd64BootRuntime` is the single
   exception.**
   - IA-64: runtime construction runs inside `AsteriaIa64RunOwnedExecutionContext`. `InitialExecutionContextSwitcher.S`
     rejects a visible `PSR.i=1` before it enters that context; its PSR mask includes bit 14.
   - AMD64: after the handoff `cli` (`InitialAddressSpacePreparation.S:85`), every acquisition runs with IF clear.
     Nothing between the return from `ExitBootServices` and that `cli` takes a lock (`RunPostExitBootstrap`
     reaches none).
   - AMD64 exception: `PrepareAmd64BootRuntime` runs from the final-memory-map callback before `ExitBootServices`
     (`BootTransition.hpp:73-74`, `BootApplication.cpp:1761/1789/5068`). It acquires Shared, AddressSpace, Frame,
     MemoryObject, DeviceRegion, ledger, SchedulingContext, Thread, Timer, Notification, backend, and identifier
     locks with the firmware's interrupt state. This is safe because only the BSP runs and no firmware handler can
     reach an Asteria lock.

## Known gaps

- **Interrupt masking is a convention, not part of the lock.** `SpinLock` does not mask interrupts. Safety depends
  on rules 1–4: no handler takes a lock, and interrupts are enabled only in proven lock-free windows. General kernel
  preemption, or any handler that takes a lock, would need an interrupt-masking guard or a preempt-disable discipline
  first ([ProcessorKernelRunLoop](ProcessorKernelRunLoop.md), [Ia64KernelContext](Ia64KernelContext.md)).
- **The AMD64 pre-exit phase acquires locks with IF possibly set.** It runs only on the BSP, before native
  interrupt and scheduler consumers can use those locks.
- **The Call/Reply dispatchers bypass the ProcessorScheduler lock.** `SynchronousReplyDispatcher` binds a raw
  `Scheduler::ReadyQueue&` (`SynchronousReplyDispatcher.hpp:243`, used at `.cpp:489`) instead of
  `ProcessorSchedulerWakeFacade`. Production code does not wire either dispatcher, and a scheduler's own ReadyQueue is
  private, so no current path breaks the scheduler's exclusion ([DispatchCoordinator](DispatchCoordinator.md)
  §Concurrency). Direct wake-queue access is limited to this standalone dispatcher contract.
- **Interface implementations must preserve lock order.** An implementation called
  with an IommuDomain or Interrupt lock held must acquire no lock at rank ≤ 3
  or ≤ 5 respectively.
- **The AHCI command lock is held across bounded device polling.** The `PciEcamRuntime` lock stays held through the
  `PollBudget` loops. This is acceptable for the boot-only path but must not be copied for runtime drivers.
- **The analysis merges overloads and ISA variants by qualified name.** This can
  add conservative edges. Unresolved calls reachable under a guard fail the gate.

## Port I/O and legacy interrupt lines

PortIo looks up and leases the DeviceRegion, then
retains its register access under the existing rank-5 region lock. The hardware
operation runs after that lock is released, followed by release of the retained
access. Policies are immutable publisher-owned data. InterruptPool's rank-1
lock also covers legacy reservations and their authority pins; it reaches the
same rank-5 Interrupt and rank-6 registry as PCI Interrupt creation. The AMD64
shared vector allocator is atomic. IOAPIC select/window pairs run only on the
BSP with interrupts disabled, through permanently mapped UC pages, and take no
lock, including in the source's mask/acknowledge handler. No temporary page-table
mapping or boot MMIO aperture is reached by this handler.
