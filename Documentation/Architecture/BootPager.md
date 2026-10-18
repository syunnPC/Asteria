<!-- SPDX-License-Identifier: Apache-2.0 -->

# Boot Pager

The boot pager is the user program that serves the first lazily backed
region of the static boot manifest
([execution model](PagerChannel.md)). It backs a
client's lazy region with one Frame and maps each faulting page from the
matching page of that Frame through the
[FaultResolve](SystemCallAbi.md#faultresolve-semantics) call of the
[pager channel](PagerChannel.md). It maps only a missing translation of a
data access in that region; every other fault of a thread it supervises, a
protection fault, an instruction fetch or an exception there included, it
terminates ([IA-64 fault delivery](Ia64UserFaultEntry.md)).

## Start block

The kernel writes twenty-two little-endian words at the top of the pager's
initial stack and passes their address as the entry argument
(`Abi/PagerBootstrap.hpp`):

| Words | Content |
| --- | --- |
| 0-1 | pager Endpoint (Read) |
| 2-3 | WaitSet (Wait, and Manage with bit 4) holding the readiness Notification |
| 4-5 | DiagnosticChannel (Write) |
| 6-7 | the client AddressSpace (Map) |
| 8-9 | the lazy region's MemoryObject (Map) |
| 10-11 | the lazy region's Frame (Map) |
| 12 | the client virtual address of the lazy region (page aligned) |
| 13 | its page count (nonzero) |
| 14-15 | a ResourceDomain (Manage, Inspect, Code) that `ObjectCreate` charges, or zero ([object management ABI](SystemCallAbi.md)) |
| 16 | the features of that grant: bit 0 (`PagerManagementFrames`) when the kernel has a frame pool, bit 1 (`PagerManagementMapping`) when MemoryMap maps created memory, bit 2 (`PagerManagementThreads`) when it creates threads, bit 3 (`PagerManagementDomains`) when it creates domains, bit 4 (`PagerManagementWaitSets`) when it creates WaitSets and WaitSetBind binds created Endpoints to them and to the pager's own WaitSet |
| 17 | zero |
| 18-19 | with bit 1, the pager's own AddressSpace (Map); otherwise zero |
| 20 | with bit 1, the start of a window of that space nothing maps (page aligned); otherwise zero |
| 21 | its page count, or zero |

Each handle is a selector word followed by an authenticator word.
`DecodePagerBootstrap` rejects a misaligned or empty region, or one that would
wrap the address space, and a misaligned or wrapping window.

## Object-management self-test

A nonzero management handle (words 14–15) enables the self-test before fault
serving. Management feature bits and native launch entries select these checks:

- **Endpoints:** loop back the canonical attachment-free 88-byte frame, require
  sequence 1 and unchanged bytes, then destroy the Endpoint. A dead handle must
  return ObjectNotFound before it is closed; another create/destroy checks reuse.
- **Frames and mapping:** create a two-page RAM Frame and writable user
  MemoryObject. With MemoryMap, map the pair in the pager window, check zeroed
  page boundaries and pattern round trips, then unmap before destruction. The
  pager never touches the retired window, because it has no supervisor.
- **Threads, on both ISAs:** a same-domain thread checks its extended-state
  pattern after observing the pager's ObjectDestroy refusal counter advance.
  On the shared self-test processor, that observation proves preemption. AMD64
  checks XMM/MXCSR using an 8 KiB stack; IA-64 checks f32–f127 and uses a 32 KiB
  stack/RSE region with function-descriptor entries. The platform clock bounds
  the probe. Page-zero and illegal-instruction threads must reach the pager and
  terminate with FaultTerminatedExitCode. A spinner must run before termination;
  a second terminate succeeds without changing its KilledExitCode.
- **WaitSets, on both ISAs:** an unbound WaitSet expires a thread's 20 ms wait.
  The pager serves faults between 10 ms polling waits, reclaims the thread and
  requires both PagerWaitingThreadExitCode and a TimedOut result. A second thread
  publishes Waiting and blocks indefinitely; after a 100 ms settling wait, the
  pager terminates and reclaims it with KilledExitCode before destroying the
  WaitSet. Aborted wait completion never returns to user mode.
- **Optional domain cycles:** when requested by the launch, create a tagged
  AddressSpace/CapabilitySpace/ProtectionDomain, map and unmap a fresh pair in
  the inactive space, then destroy backing objects and spaces in order. Both
  native launches currently select zero cycles; root-service scenarios cover
  domain reclamation instead.

Shared progress fields `PagerThreadControl.Polls`,
`PagerWaitingThreadControl.Waiting` and `Result`, and `PagerProgram.m_spins`
are 8-byte-aligned UInt64 storage. Every C++ access uses AtomicUInt64Reference:
publish/reset with release stores or RMWs, and observe with acquire loads.
The native spinner uses AMD64 `lock incq` or IA-64 `fetchadd8.rel`. Control
metadata is initialized before thread creation; progress fields reset before
each launch, and their storage remains alive until the thread is reclaimed.
Ordinary loads/stores and volatile access cannot replace this atomic protocol.

Each reclaimed handle must fail Query with ObjectNotFound before Close.
An unexpected result returns ManagementFailed before ordinary fault serving.
The same self-test remains in product and test pager links
([test separation](../../README.md)); only the extended-state
thread emits its diagnostic result line.

## Root service

[product and test boot profiles](../../README.md) give the pager two links.
The product `RootService` starts the display broker, reads and prints the boot
inventory through `DeviceInventory.cpp`, and, when i8042, xHCI or a USB host controller is published,
the input broker and its input drivers. PS/2 starts first; xHCI follows in PCI
catalog order and then the USB host group when component slots allow ([xHCI component](XhciDriver.md),
[USB host group](UsbHostDriver.md)). It prints `# init: ready` naming the services
and keeps them resident. `InputTerminal.cpp` echoes keys on a US layout and
button changes with the pointer position; Backspace erases. Screen writes are
immediate, while serial receives complete edited printable lines, as required
by DiagnosticChannel. Its credit waits use `AwaitMessage` and the supervisor's
`Step`, serving faults and reaping components. Display, broker, PS/2, one
xHCI and the USB host group fit the five-component pools; without PS/2, two
xHCI instances and the group fit. An xHCI driver or the group that fails while
serving is recovered (`RecoverInputDrivers`): its devices proven quiet by the
Device Manager, its broker source registered again and the driver restarted,
at most three times ([driver isolation](IommuDomain.md),
EHCI functions reserved by an RMRR).
Without either input controller, init keeps display only and returns to `ServeOnce`.

The test `TestRootService`, under `Tests/Guest/Pager`, uses the product's
protected lifecycle helpers and `RootContext.hpp` interfaces. The following
scenario sequence belongs to this link; it tears down display and ends with
`# root service: done`. Its input script falls back to the product terminal
when real input arrives or the script is absent.

The boot application reads component module files named by BOOT.CFG and constructs a table in its retained
LoaderData region ([kernel image contract](KernelElf.md)). A count and zero word precede
entries of 16-byte zero-padded names, 64-bit offsets from the table and byte sizes; module bytes follow.
The kernel maps only that page-isolated slice read-only into the pager: AMD64 at 0x44000000 through one
additional initial range, IA-64 at 0x2000000007000000 through an ordinary pinned mapping without copying or
adding a TR. The aggregate bound is 2 MiB. No other address space maps those frames and they are never
reclaimed. Start-block words 49 and 50 carry the base and size (words 0–50). Four xHCI handle pairs append at 51–66 and clock scale at 67
([input contract](InputBroker.md)), for 68 words total. Product media list component
and directory; test media additionally list client, the ISA's foreign interpreter stub and three foreign samples.
The pager links carry neither module bytes nor weak component-table symbols. Shared sealed image records
retain stable table-address keys.

With `PagerManagementDomains` in word 16 and the `component` module the kernel maps (both ISAs), the pager's self-test is
followed by its root-service role ([domain creation ABI](SystemCallAbi.md),
[object retirement](SystemCallAbi.md#object-management-semantics), `RootService`): it creates a report Endpoint, then builds each
incarnation of `ComponentIncarnations` (crash, hang, report) in a domain it creates (an AddressSpace, a
CapabilitySpace and a ProtectionDomain whose supervision tag is the incarnation's number). Read-only PT_LOAD segments use immutable per-image records
([shared executable segments](Elf64Image.md)): one grant-owned Frame and sealed MemoryObject per segment. The loader
copies through a temporary writable object in the pager window, flushes executable bytes once on IA-64, waits for
MemoryUnmap's synchronous shootdown completion, destroys that temporary object, then creates the sealed object
under the Code-bearing grant. Later incarnations map the same Frame and MemoryObject without copying or flushing.
Constant pointer tables, IA-64 function descriptors and the GOT belong to sealed read-only data. A component's
mapping records mark these handles Shared, so teardown and abandonment never close or destroy them. Image records
retain their generation and Map-only derived handles, and stay resident through every drain for this run.
They do not track incarnation membership. An outstanding staging-window unmap is recorded and retried before
abandonment completes or device recovery reuses the window.
Writable zero-filled segments and stacks remain private and charge the child's ResourceDomain; child page limits
exclude shared segments. One private Frame holds the stack and,
right above it, the start block, which names the incarnation, a send-only copy of the
report Endpoint (CapabilityGrant), the clock ticks a crashing incarnation spins and what the incarnation does. Where segments may lie, how the entry is named (on IA-64 the ELF entry is a function descriptor, whose {ip, gp} the
root service copies into its own memory for the thread's entry), the stack's place and size and the flush are the
pager image's `ComponentPlatform`; AMD64's stack is two pages, IA-64's a 32 KiB region in region 1 (the RSE backing
store, then the memory stack; `Abi::Ia64ComponentStackStart`), and IA-64 flushes copied code with `fc.i`. The
pager then creates the component's thread in that domain and alternates ObjectDestroy with its service loop. A
crashing incarnation spins (AMD64: past a time slice; IA-64: 200M ITC ticks, 0.5 s or ten slices, sharing the processor
round-robin with the CPU-bound initial image) and executes an instruction its ISA refuses (`ud2`;
`mov r8=psr`, privileged at PL3); the kernel delivers the exception with its tag, and
the pager terminates every tagged fault, wherever it lies. A hanging one spins and never reports; every wait of the
root service lasts at most the platform's watchdog period (`ComponentPlatform::WatchdogMicroseconds`: 2 s on AMD64,
10 s on IA-64), and one that times out with the thread alive is the
watchdog's expiry: the pager kills the thread with ThreadTerminate (or, should it just have faulted, lets the fault's
termination end it) and reclaims it with `Abi::KilledExitCode`. A reporting one sends the canonical frame on its report
Endpoint and exits; its exit wakes the pager's wait, which reclaims it with `Abi::ComponentExitCode` and requires the
report. Every way the pager reclaims the thread, unmaps and destroys the memory, then destroys the ProtectionDomain,
the CapabilitySpace and the AddressSpace, which the destroying calls' collections reclaim, and writes
`# component: crashed, terminated and torn down`, `# component: hung, killed and torn down` or
`# component: reported, exited and torn down`. The three
incarnations share the two space slots, each arena prepared again by the next creation. Any other result ends the
root service, which `RunPager` reports once as `Bail out! root service stopped serving: error 0xEE call 0xCC`; the
pager then goes on serving faults, since a component the root service left running still needs them.

Where word 16 announces `PagerManagementWaitSets` (AMD64), a fourth incarnation follows, a service
([service IPC](IpcWireFormat.md)). The root service creates its service Endpoint receiving in the
component's CapabilitySpace and a WaitSet, binds the Endpoint to the WaitSet (WaitSetBind, tag `EchoWaitTag`) as the
only binder, and grants the component a `Read` copy of the Endpoint (start-block words 8 and 9) and a `Wait` copy of
the WaitSet (words 6 and 7; the block has 16 words). It creates a one-page Frame and sends the canonical frame with two attachments
(`User/Component/EchoFrame.hpp`, built by hand on both sides): a copy of a send capability on a reply Endpoint of its
own (`Invoke`; unbound and polled, apart from the report Endpoint) and the Frame, moved (`Transfer`, `Manage`,
`Inspect`). The service Endpoint is created with the sealed
selector `EchoSealedSelector`, which the component requires in every delivery's header. The component
waits on its WaitSet, receives until the Endpoint is empty, and answers on the copied capability with the Frame moved
back; the root service polls its reply Endpoint between steps of its loop (a 10 ms wait, then the reap of any
component that ended, so a crash ends the wait), requires that the Frame came back under a new handle while the one it left by is
gone, and destroys it. It then waits 100 ms, longer than a time slice, so the component blocks waiting for the next
request, kills it, which aborts that wait, reclaims it with `Abi::KilledExitCode` and tears it down, its WaitSet
before its Endpoint, then writes `# component: echoed, killed while waiting and torn down`.

Where the manifest also lists the `directory` module (both ISAs), the root service then runs two services side by side, each
in a domain of its own ([service IPC](IpcWireFormat.md)): the Object Directory (`DirectoryProgram`, its
Endpoint sealed with `DirectorySealedSelector`, its service core and buffers in its `.bss`) and Echo. As the
Directory's only binder, with the `Manage` its capability on the Directory's Endpoint holds, it binds `echo`
(BindIfAbsent; the binding holds an `Invoke`|`Duplicate` copy of a send capability on Echo's Endpoint; every deadline
infinite) and requires the Directory's reply on its reply Endpoint, then writes `# directory: bound echo`. Where the
table also has the `client` image, it builds a client as a task under its watchdog (`ClientProgram`): an unsealed
Endpoint of its own bound to its WaitSet under `ClientWaitTag` (start-block words 6 to 9), an `Invoke`|`Duplicate` send
capability on that Endpoint for its reply attachments (words 12 and 13) and an `Invoke`-only copy of the root's
capability on the Directory's Endpoint (words 10 and 11), which cannot bind. The client resolves `echo`, sends Echo three
requests through the capability the Directory returned, each echoed back on its Endpoint with the request's transaction
and payload, and reports `{resolved, echoes, generation, 0}` on the report Endpoint; the root service requires
`{1, 3, Echo's tag, 0}` and writes `# client: resolved echo and got 3 echoes`. While the client waits for its go without
a deadline, the root service kills Echo while it waits, tears it down, which destroys its Endpoint and every
capability on it, builds a fresh Echo in a fresh domain under the next tag and replaces the binding (ReplaceIfVersion
with the version its bind returned and the new tag as the service generation), writing
`# directory: echo restarted and rebound`, then sends the client its go on the client's Endpoint and restores its
watchdog. The client's send through the capability it resolved first fails, it closes it, resolves `echo` again, gets
three echoes from the new Echo, reports `{1, 3, the new tag, ObjectNotFound}` and exits; the root service requires the
exit code and that report, tears the client down and writes
`# client: stale send refused, resolved again, 3 echoes, torn down`. After 100 ms, longer than a time slice, it kills
Echo and then the Directory while they wait for requests, reclaims and tears down both (each WaitSet before its
Endpoint), and writes `# services: directory and echo killed while waiting and torn down`. With several components
live a crash is not attributed to one of them, so any crash fails the demonstration.

Last, where the table also holds the loader's demonstration (AMD64; [executable image contract](Elf64Image.md)),
the root service gives it three foreign executables the build writes (`Tools/Build/WriteForeignSamples.py`). Each is
described with `DescribeExecutable` under AMD64's rules; one the native loader refuses as another machine's or system's
goes to the interpreter registry (`LoaderDemo.cpp`, which registers `ia64-stub` for IA-64 ELF), whose interpreter must
itself be a native image. `ia64-sample` resolves to the stub. The root service builds the stub as a task in a fresh
domain, stages the sample's bytes into one Frame mapped read-only and never executable at `ComponentForeignImageAddress`
(`0x6000'0000`), and writes the extension record (its address, size and identity) after the start block's 16 words. The
stub describes the guest itself with the loader's handlers under IA-64's rules, requires the record's identity, reports
the guest's machine, entry ip and gp, segments and size, and exits; the root service requires that report, as it
describes the guest itself, and writes `# loader: ia64 executable ran under its registered interpreter`. `aarch64-sample`
finds no interpreter (`# loader: aarch64 executable refused, no interpreter registered`) and `pe-sample`, a PE32+ for
AMD64, is a format the native loader does not support (`# loader: pe32+ executable refused as an unsupported format`).

IA-64 loads Directory, Client and a native `amd64-stub` and runs the same service sequence and loader
sequence. Its guest is `amd64-sample`, staged read-only at
`Ia64ComponentForeignImageAddress` (`0x2000'0000'0600'0000`), and the interpreted line names `amd64`.
Its PE32+ sample names IA-64, retaining the native `UnsupportedFormat` refusal. The display broker stays
resident throughout; the grant and the 20-page segment/window limit are sized from the IA-64 link maps
([service IPC](IpcWireFormat.md)).
The grant checker and observer derive page/pair totals and retained shared-image
residue from the linked ELFs. They require that residue, once-only staging and equal
shared-object handles after crash and echo restart; `# shared image:` lines report the generation, segment, tag,
Frame selector/authenticator, MemoryObject selector/authenticator and whether this launch staged the segment.

The root service keeps each component it built in a record of its own (`MaximumComponents`, bounded by the
kernel's domain pools; [service IPC](IpcWireFormat.md)) holding the domain's handles, its mappings, its
entry descriptor and its watchdog, and supervises them in one loop: each wait lasts the earliest watchdog of a running
component, a wait that times out kills the running components, and every running component whose thread ObjectDestroy
reclaims is reaped with its exit code. The bounded table supports concurrent live components.

The root service separates component construction, transport and scenario state:

- `ComponentLoader` builds a component in a domain of its own: its ResourceDomain within the first manifest, its
  spaces and ProtectionDomain, its segments and an interpreter's guest, the stack and start block with the capabilities
  its behavior needs, and its thread. It owns the immutable shared image records for the run.
- `ComponentSupervisor` owns the records: it starts a component through the loader (a task under the watchdog, a
  service without an exit deadline), runs the supervision loop, stops a waiting service, and tears a component down,
  draining its ResourceDomain and having the device confirmed quiet after a driver's drain.
- `DeviceManagerRole` grants the assignable device, and confirms a quarantined one quiet.
- The scenarios (`Tests/Guest/Pager/RootScenarios.hpp`: the incarnations and Echo; `ServiceDemo.cpp`, `LoaderDemo.cpp`,
  `DriverDemo.cpp`) are free functions given a `ScenarioContext`: the parts, used through their own interfaces, the
  images, the report and reply Endpoints, the next tag and the buffers they share. `ObjectCalls` holds the request
  buffer and the object calls every part shares.

`RootService` owns the parts, image discovery and display lifecycle. `TestRootService` creates
the shared Endpoints and runs the scenario sequence through those interfaces.

A failure has a scope. A request a service cannot serve is dropped and counted by that service. A scenario that fails (a component that ended otherwise than asked, a refused call, a wrong reply) ends the
root service's run: the supervisor abandons every component it holds, killing each one still running, supervising it
to its end under the watchdog and tearing every record down (a build that failed part way included, since a record
is `Building` until its thread starts and its own ResourceDomain holds what it made), and the root service writes
`# root service: <scenario> failed, components torn down`. The scenarios after it are not run on what the failure may
have damaged. Should the abandonment fail too (a kill refused, a drain or a device that does not finish), the line
ends `components left standing` and the records stay. Either way `RunPager` reports the root service's bail-out and the
pager goes on serving faults.

## Service loop

`PagerProgram::ServeOnce` waits once on the WaitSet and requires the
readiness tag (`PagerReadinessTag`). It then receives from the Endpoint until
`WouldBlock`. Each delivery must be exactly one fault message of the
[pager protocol](PagerProtocol.md) version 1.1: the 208-byte delivery whose frame
header carries the `AIPC` magic, a 40-byte payload (the fault record and the
supervision tag, zero for every boot domain the pager serves) and one
attachment, the FaultToken handle. The faulting address selects the lazy page; the pager maps that one
page from the same page offset of the MemoryObject and Frame with Read, Write
and User permissions, then closes its token handle, which no longer resolves
after the resolution.

A fault outside the lazy region is not the pager's to back. As the boot
domains' supervisor ([supervised fault termination](ThreadLifecycle.md))
the pager ends the faulting thread with `FaultResolve` Terminate, closes the
token handle and keeps serving.

A wait on any other tag, a malformed message, or a rejected call (a refused Map
included: it would hide a kernel or pager bug) stops the service. `RunPager` then writes the TAP line
`Bail out! pager stopped serving faults: error 0xEE call 0xCC` once through
its DiagnosticChannel, with the `PagerServeError` and the system-call error as
hex bytes, and returns, so the image idles. The faulting thread stays blocked, and the
guest-test harness reports the bail-out. The pager writes nothing while it
serves, since text before the TAP version line is itself a TAP error.

`PagerProgram` keeps its 552-byte delivery buffer, 88-byte request and 40-byte Endpoint request in its
own object, which `RunPager` places in static storage, so the service loop
stays within the user stack budget.

## Images

The AMD64 firmware build links `PagerImage.elf` from `ASTERIA_AMD64_PAGER_SOURCES`
(`Amd64PagerImage.lds`, based at `0x40000000` like the initial image) and
installs its stripped bytes as `EFI/ASTERIA/INIT.ELF` in the production ESP.
The guest-test ESP installs `PagerTestImage.elf`, a second link of the shared objects with the test
entry and scenario objects. Product init is excluded from that link. The product
table holds `component` and `directory`; the test table adds `client`, the
interpreter and the foreign samples. The component image is shared. Compile
checks build product and test code at O0 and O2 within the same stack budget. Its entry stub
calls the C++ main with the start-block address and idles if it returns.

On AMD64 the boot runtime stages the pager image into its own root, gives the
pager thread its own kernel stack, and serves the initial image's lazy region
`0x48000000-0x48001fff` (`InitialUserLazyStart`, two pages), whose pages start
with `LazyPagePattern` and their index
([AMD64 runtime](Amd64BootRuntime.md#boot-pager)). The guest test program reads
and writes both pages (`pager.read`, `pager.write`, `pager.resident`).

The IA-64 firmware build makes the same product/test split from `ASTERIA_IA64_PAGER_SOURCES`
(`Ia64PagerImage.lds`, a P64 image based like the initial image in region 1)
and installs its stripped bytes as `EFI/ASTERIA/INIT.ELF`. On both ISAs,
the boot application reads and checks the files named in `BOOT.CFG` into a retained
LoaderData region before reservations; the kernel stages the pager and initial user image
from that region ([boot file contract](BootContract.md)).
The IA-64 pager's entry stub takes the
start-block address in r4, moves the memory stack below the block and calls
the C++ main.

On IA-64 the boot stages, materializes and maps the pager image into its own
private root (region-identifier block 3) and allocates the two-page lazy Frame
of the initial image's region at `Ia64InitialUserLazyVirtualStart`, then the
frame pool's `Ia64FramePoolPages` pages, which it zeroes while physical
addressing is still in effect ([memory management ABI](SystemCallAbi.md)).
`Ia64PagerRuntime` then creates the pager's domain like `Amd64PagerRuntime`.
The domain consists of:

- its capability space and the FaultToken space;
- the Endpoint and the WaitSet holding the readiness Notification;
- the lazy MemoryObject and Frame;
- the PagerChannel;
- its ProtectionDomain, Thread and SchedulingContext;
- its management grant, whose MemoryMap and MemoryUnmap use the BSP's
  shootdown dispatcher.

The pager's kernel context runs on its own entry storage, which its
system-call entry context also names, so its WaitSetWait blocks there through
`FinishWaitAndDispatch` (`IIa64SystemCallThread`). The initial-image dispatch
admits the pager first. The pager's kernel context writes the start block below
its stack top through its own mapping, selects its entry contexts and enters
PL3. The pager waits, and the initial image's launch selects its own contexts
again.

The initial image's faults reach the pager in the following order:

1. An access to the lazy region misses in the data TLB, and the refill handler
   finds no ledger record, so it enters the
   [user fault entry](Ia64UserFaultEntry.md). That entry saves the complete
   user state on the image's entry storage and calls the runtime's
   `IIa64UserFaultHandler`.
2. The handler delivers the fault through `PagerChannel` and blocks the image
   through `FinishFaultAndDispatch` on its bound kernel context. After the
   one-way launch, that context runs on the image's entry storage.
3. With both threads blocked, the idle loop polls the pager's wait. The
   readiness signal of the delivery wakes the pager, which resolves the fault.
   The mapping lands in the image's ledger.
4. When the pager waits again, the image resumes in the handler, selects its
   entry contexts and returns. The entry restores the user state, and `rfi`
   re-executes the access, which the refill now satisfies.

A fault of the pager itself fail-stops. The guest test program checks
`pager.read`, `pager.write` and `pager.resident` on IA-64 too.
