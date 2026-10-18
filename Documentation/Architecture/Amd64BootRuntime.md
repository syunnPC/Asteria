<!-- SPDX-License-Identifier: Apache-2.0 -->

# AMD64 native boot and initial user execution

The AMD64 runtime connects the owned UEFI activation boundary to the common
capability kernel.
It includes one through four CPUs, local timer delivery, IPI-driven shootdown,
AP user-thread scheduling, and repeated CPL3 WaitSet suspension/resume.
PCI ECAM and AHCI IDENTIFY/READ DMA EXT are connected to the same common device
kernel used by IA-64.
An optional BSP diagnostic also proves one native kernel preemption and resume.
A boot pager thread in its own domain serves the initial image's lazily backed
region: a CPL3 page fault there blocks the image until the pager maps the page
([boot pager](BootPager.md)).

## Ownership and publication

Firmware reserves 23 bootstrap pages, a 3,529-page runtime pool below 4 GiB, and
2 startup pages below 1 MiB. No final-map callback allocates firmware memory.
The callback validates LoaderData/WB provenance, prepares the boot root
arenas, stages the bounded initial ELF and the boot pager ELF, registers their
Frame/MemoryObject owners, completes pinned mappings into each image's private
root, writes the lazy region's pages, and constructs descriptor tables. It
constructs the runtime's capability space with the `BootstrapSpace` key derived from
`BootInfo::Entropy`, which the transition publishes before the callback runs;
a record that yields no key rejects the preparation
(`AMD64-RUNTIME-PREPARE:FAIL:CAPABILITY-KEY`), and the PCI/ECAM runtime
derives its `DeviceSpace` key the same way after exit. The pager's space and
its channel's FaultToken space take the `PagerSpace` and `FaultTokenSpace`
keys, the test firmware's spinner space the `SpinnerSpace` key. A retry retains that owner and checks the same addresses, image sources,
paging capabilities, topology, and initial mappings. Failed preparation cannot be restarted on partial state.

The runtime pool layout in 4 KiB pages is (named in `Amd64RuntimePoolLayout.hpp`,
which the VM observers read too):

| Pages | Use |
| --- | --- |
| 0–63 | shared-root arena |
| 64–127 | initial image's private-root arena |
| 128–191 | boot pager's private-root arena |
| 192–255 | spinner's private-root arena (test firmware) |
| 256–351 | four 24-page per-CPU stack extents |
| 352 | shared-root shootdown proof page |
| 353–355 | permanent AHCI IDENTIFY, READ command, READ data pages |
| 357–360 | boot pager's kernel stack (356 and 361 are guards) |
| 362–365 | spinner's kernel stack (366 is a guard) |
| 367–368 | the lazy region's Frame |
| 369–372 | the four per-CPU scrub slot pages, each mapped supervisor RW/NX in every root at `0x70001000` plus four KiB per CPU ([memory management ABI](SystemCallAbi.md)) |
| 384–511 | initial image's staged PT_LOAD pages and four user stack pages |
| 512–639 | boot pager's staged PT_LOAD pages and four user stack pages |
| 640–767 | spinner's staged PT_LOAD pages and four user stack pages |
| 768–1087 | five page-table arenas reserved for AddressSpaces created at run time, mapped as page tables in every root |
| 1089–1092, 1094–1097, 1099–1102, 1104–1107, 1109–1112 | the five kernel stacks of created threads, identity-mapped supervisor RW/NX in every root (1088, 1093, 1098, 1103, 1108 and 1113–1119 are guards; [thread creation ABI](SystemCallAbi.md), [service IPC](IpcWireFormat.md)) |
| 1120–1779 | the VT-d tables, identity-mapped writable in every root: four per-unit root/context/user table sets and eight 64-page firmware domain sets ([driver isolation](IommuDomain.md)) |
| 1780–2293 | stop aperture PDPT, PD and 512 PTs, identity-mapped writable/NX in root 0, attached to its PML4 only at the terminal stop |
| 2296–2307 | the per-CPU NMI stacks, two pages each between guards |
| 2308–2824 | the AMD-Vi device table, command and event buffers and completion page ([AMD-Vi](AmdVi.md)) |
| 2825–3336 | two 256-page large root arenas, seed-mapped supervisor RW/NX and selected by flagged AddressSpace requests |
| 3337–3528 | three 64-page AP idle-root arenas |

The runtime reservation contains 3,529 pages. The physical allocator supplies
RAM Frames from the imported memory map.

A separate LoaderData metadata arena below 1 GiB is sized by the pure
`MetadataArenaPages` query, 2 MiB-rounded and capped at 4,096 pages (16 MiB).
Its page layout is derived in `Amd64MetadataArena.hpp`:

| Arena pages | Use |
| --- | --- |
| 0–703 | eleven further 64-page created-root arenas |
| 704–839 | twenty-seven further kernel stacks, each four mapped pages and one unmapped guard; leading guard at 704 |
| 840–1095 | 16,384 ObjectRegistry entries, 64 bytes each |
| 1096–1343 | 4,096 CreatedFrame pool slots |
| 1344–1495 | 4,096 MemoryObject pool slots |
| 1496–1801 | sixteen created-root owner objects |
| 1802–end | allocator bitmap budget |

The arena's inherited seed is supervisor RW/NX. The eight-page table budget
allows eight 2 MiB blocks of 4 KiB leaves; hosted tests prove the incremental
cost, and each VM checks that its remaining seed plus the capped arena fits
64 table pages. `RuntimeRoots` publishes and withdraws each live root in the
scrubber's acquire/release table (seven boot roots, including AP idle roots, plus sixteen created roots).
Every processor uses its own scrub slot. Pool and arena size/overlap assertions
check the layout. A large-arena AddressSpace request selects one of the two
large arenas through `LargePageTableArenaPool`, under the `AddressSpacePool` lock. A flagged root costs
`sizeof(AddressSpace) + 256 * 4096` metadata bytes and one large-arena unit in
its ResourceDomain and ancestors. A quiesced, unpinned root's collection
withdraws it from the scrubber before freeing the arena; a failed preparation
spends its storage. The root grant includes the extra cost of two large roots,
allowing compositor restart overlap.

Each CPU extent leaves offsets 0, 5, 6, 11, 12, 17, 18, 21 unmapped. Idle,
worker, syscall/RSP0, double-fault, and NMI/machine-check stacks start at
1, 7, 13, 19, 22 respectively. All mapped stack pages are supervisor RW/NX.
The pager's and the spinner's kernel stacks are supervisor RW/NX in every
root, like the per-CPU stacks. The user stack occupies `0x50001000–0x50004fff`; adjacent pages are
unmapped. The lazy region (`0x48000000–0x48001fff`, between the image window
and the stack) has no mapping until the pager resolves a fault there.
User ELF segments occupy a bounded window beginning at `0x40000000`. An empty
`PT_LOAD` record (zero `p_memsz`) is a valid ELF record that describes no
memory: as on IA-64, it receives no range, no staged page and no window check.
User backing pages have no writable identity alias in the shared or private
root. The root verifier supports at most 64 table pages and rejects shared
parents, unexpected leaves, missing leaves, and nonzero unused storage.

After the initial activation verifies CR0/CR3/CR4/EFER/PAT, it calls the fixed
runtime entry. The common ProcessorAddressSpaceController adopts the shared
root. The run loop selects the registered user ProtectionDomain, switches to
its private CR3, enters a prepared kernel continuation, and restores the
initial CPL3 frame with IRETQ. No firmware calls occur after exit.

## Native entry contract

Kernel code/data selectors are 8/16, user data/code selectors 27/35, and TSS
selector 40. All IDT gates are DPL0 interrupt gates. Double fault uses IST1;
NMI/machine check use IST2 and are terminal. The TSS I/O bitmap is beyond its
limit, denying CPL3 I/O.

The entry install gives every CPU, the BSP after its initial activation and
each AP right after its startup code, the same control-register policy
(`Amd64UserEntry.hpp`, [extended processor state preservation](Amd64KernelContext.md#switch-boundary)). CR0.MP/NE are set and
CR0.EM/TS cleared, so x87 and SSE run and report errors as exceptions, and CR0.AM is cleared, so the RFLAGS.AC a
program may set enables no alignment check. CR4 is written whole: exactly PAE (IA-32e paging), MCE (a machine check
reaches its terminal IST2 handler instead of shutting the processor down), OSFXSR and OSXMMEXCPT. Every other CR4 bit
is clear, whatever firmware set on the BSP or the startup code on an AP: no global pages, PCIDs or five-level paging
(the root backend), no FSGSBASE, no SMEP/SMAP (not enabled by this profile), no OSXSAVE (XSAVE is unsupported), and no DE,
TSD, PCE, UMIP, VMXE, SMXE, PKE, CET, PKS, LASS or LAM. The preparation requires CPUID to report FXSR, SSE and SSE2
(`CPU-FEATURES` otherwise); the runner pins QEMU's `qemu64` model, which has them and no XSAVE.
Other CR0 bits keep what the activation checked (PE, WP and PG set, CD and
NW clear) or the AP startup code chose.

SYSCALL uses STAR/LSTAR/FMASK and SWAPGS/LFENCE to reach the CPU-owned entry
stack. The stub synthesizes an IRET frame, saves all integer registers,
clears DF, aligns the C++ call, and invokes the common syscall ingress through
the active run-loop ProtectionDomain. RAX/RDI/RSI/RDX/R10/R8/R9 carry the
request; RAX/RDX/R8 carry the response; RCX and R11 return the address after
SYSCALL and the RFLAGS image SYSCALL saved. Every other register, RSP
included, returns unchanged.

RFLAGS and RSP at SYSCALL are the program's own state, and any value the
program can reach is legal: a system call never stops the kernel because of
them. The return restores RSP unchanged, whatever its value (IRETQ accepts
any; a stack the program cannot use faults in the program). It restores the
sanitized RFLAGS (`SanitizeUserFlags`): CF, PF, AF, ZF, SF, DF, OF, NT, AC and
ID, the flags a CPL3 program changes with POPF except TF, come back as the
program left them; IF and bit 1 are set; TF (the profile admits no
single-stepping of user code; a debug trap a program raises itself is a
supervised Exception that terminates that thread), IOPL, RF, VM, VIF, VIP
and the reserved bits are clear. The kernel
still checks what it produces itself: the exact user selectors the entry stub
pushes. Their mismatch is a kernel invariant failure and stops the machine.
RIP must occupy the lower canonical half above page zero, shared with mapping
and thread start by `UserAddressPolicy.hpp`. SYSCALL at its final two bytes
legally saves a noncanonical next RIP (Intel *64 and IA-32 Architectures
Software Developer's Manual*, 325462-093US, September 2026, Vol. 2B,
`SYSCALL`, Operation). Such a created thread enters its supervisor as an
Exception with the unusable next RIP and a `#GP(0)` syndrome before syscall
dispatch or IRETQ. Only Terminate resolves it. A busy
supervisor yields and retries from the kernel continuation, which never
returns that invalid frame to PL3. The initial client and spinner use their
page-fault supervisor too. The trusted root pager
has no supervisor, so the same states of its own still stop. Kernel GS always addresses the entry context; user GS
and FS bases are zero. A CPL3 interrupt or page fault returns under the same
policy: its frame holds the RFLAGS the processor pushed, which may carry RF
(set in a fault's image, and seen set in a timer interrupt's under QEMU), and
is sanitized and checked the same way before IRETQ.

The entry contract follows Intel's *64 and IA-32 Architectures
Software Developer's Manual*, 325462-093US, September 2026, Vol. 2B
(`MOV`, `SYSCALL`, `IRETQ`), Vol. 2D (`XSETBV`), and Vol. 3A §6.8.8.
SYSCALL constructs CS/SS from the kernel's STAR value; the GDT offers PL3
one code selector and one writable data selector, and loading null SS at
CPL3 faults before changing it. DS/ES/FS/GS selector changes use zero-base
user descriptors; SWAPGS selects the privileged kernel GS base. WRMSR is
privileged, CR4.FSGSBASE is clear, and CR4.OSXSAVE is clear, so PL3 cannot
alter the kernel GS base or XCR0. Those attempted instructions fault in
user mode and use the supervisor path. All legal user RFLAGS are sanitized;
FMASK clears TF/DF/NT/AC/IOPL/IF before kernel execution. RSP is restored as a
value; IRETQ checks its own kernel stack accesses, not the new user RSP.
The guest tests both a rejected XSETBV and user-set TF/DF/NT/AC/ID followed
by SYSCALL and a supervised UD2, then continue with the next cases.
An upper-half branch can fault with its unusable target as RIP. When that
fault yields behind a busy supervisor, `ResumeUserOn` uses the same
`IsUserReturnAddressFault` classification to deliver #GP(0), retrying in the
kernel until termination. The guest retains an XSETBV fault token while
another thread takes that branch, then terminates and reclaims both threads.

Each user thread owns its kernel stack: the initial thread CPU0's
syscall/RSP0 stack, the pager and the spinner their own pool stacks. Before every CPL3 return the
trap path makes the returning thread's stack both TSS RSP0 and the SYSCALL
stack top of the entry context (`Amd64UserEntry::SelectKernelStack`); it reads
that stack at trap entry, since a trap enters on the stack of the thread it
returns to and a blocking call lets another thread select its own. Each
thread's launch selects its stack before its first IRETQ. A thread blocked in
a wait or a fault keeps its frames on its own stack while the other runs. IRQ
entry preserves all integer state, masks/acknowledges the local timer, and
returns.

A CPL3 page fault of the initial thread is decoded (`Amd64PageFaultDecoder`)
and delivered to the boot pager through `PagerChannel`:
`ProcessorKernelRunLoop::FinishFaultAndDispatch` blocks the thread, the
channel delivers the fault message and signals the pager's readiness
Notification, and the trap returns to the faulting instruction only after the
pager's FaultResolve woke the thread. Every supervised thread's other CPL3 exceptions
(every vector below 32 except the page fault, NMI, double fault and machine
check) reach its domain's supervisor the same way, described by
`DescribeUserException` with the `Exception` cause, the vector and error code in
the syndrome and the faulting RIP as both addresses; no Map resolves them, so
the supervisor can only terminate the thread
([domain creation ABI](SystemCallAbi.md)). Every other
exception (a fault of the pager, a CPL0 fault, any other vector) saves the frame and CR2
and stops.

The initial thread's kernel context starts on its syscall/RSP0 stack. Its
launch never returns, so those launch frames are dead before the first syscall
reuses the stack. For syscall 8, the common WaitSet ingress resolves authority
and active Thread/SchedulingContext generations, and retains the exact wait.
FinishWaitAndDispatch commits the scheduler-created block token before saving
that stack and switching to the shared-root idle coordinator. The CPU-owned
GS context remains selected throughout kernel execution.

The bootstrap diagnostic arms a TimerQueue Timer for each suspended wait.
The IRQ masks/acknowledges the local deadline and release-publishes a counter;
it does not touch registry locks or perform scheduler work. If the vCPU was
not scheduled for the whole delay, the timer is due before the idle dispatch
can program it; the controller reports the nonterminal `DeadlineReached`, no
interrupt can come, and the idle loop arms the timer again (at most eight
times for one wait) instead of stopping, so each resumed wait still observes
one interrupt after its deadline. A `DeadlineReached` dispatch while the wait's
own deadline has not passed was another admission's run stop: it is retried
and does not count against the wait. Normal idle context
drains the Timer into its Notification, refreshes the hardware deadline, polls
the WaitSet dispatcher, and dispatches the same thread on the private root.
ConsumeCompletion verifies its new run generation before the original syscall
stack returns through IRETQ. The response is passed through unchanged: the
runtime records each resumed wait's drain receipt, the dispatcher's completion
tag and registration sequence, and the response in the evidence record
(`Amd64WaitEvidence`) instead of comparing them with the fixture's tag
(`BootstrapWaitTag`). The initial ELF receives its WaitSet capability in RDX/RCX
at entry and waits twice; the guest test program checks both responses. This is
one fixed bootstrap timer binding, not a general user timer-creation ABI or
multi-thread user process service.

## Boot pager

`Amd64PagerRuntime` owns the pager's domain on CPU0: its capability space
(`PagerSpace` key) with the start-block capabilities, the kernel-only
FaultToken space (`FaultTokenSpace` key), the pager Endpoint whose receiver is
the pager's space, a WaitSet holding the readiness Notification
(`PagerReadinessTag`), the lazy region's 2-page MemoryObject and Frame, the
`PagerChannel`, and the pager's ProtectionDomain, Thread, SchedulingContext and
kernel context. The pager's ProtectionDomain names its own private root; it
holds Map authority over the initial image's AddressSpace and the lazy
MemoryObject and Frame, nothing else of the initial image.

The pager thread is queued before the initial thread with the same
configuration, so it runs first: its launch writes the start block below its
user stack top through its own stack mapping, passes the block's address in
RDI, and enters the pager image, which waits on its WaitSet. A system call of
the pager goes to its own WaitSet dispatcher (WaitSetWait) or to the common
ingress with FaultResolve bound to the channel and ObjectCreate and ObjectDestroy to the runtime's
`ObjectManager`; a rejected FaultResolve writes
`ASTERIA:AMD64-PAGER:RESOLVE-REJECTED:` with the resolver's failure
(dispatcher, preparation, AddressSpace, page-table and pin errors, one byte
each) as a diagnostic line.

When nothing is runnable, the idle loop first retries a deferred delivery and
polls the pager's blocked wait, which wakes the pager once a fault was
delivered. Only when the pager is not woken does it resume the initial
thread's timer wait. With neither, it waits, bounded, for the next timer tick;
the boot stops with `IDLE-STALL` if none arrives. Dispatch results that idle
also accepts:

- `TimerWorkPending`: timer work the idle path drains;
- an entry rejected because the run stop passed before entry, which is retried
  like a deadline that fell due early.

## Object management

Before the pager, both firmware variants create the pager's management grant
([object management ABI](SystemCallAbi.md),
[product and test boot profiles](../../README.md)). Product init needs the same component pools
as the test pager; the reserved spinner root is prepared even when no spinner file is supplied,
so the management grant's scrubber has every reserved root. The grant is a child of the boot ResourceDomain (charge owner 2) whose
limits are the `PagerGrant*` constants of `Amd64BootRuntime.cpp`, checked against the linked component images, registered and
minted into start-block words 14-15 with Manage, Inspect and Code ([shared executable segments](Elf64Image.md)); word 16 announces the frame pool, which the same step hands to the
`ObjectManager` ([memory management ABI](SystemCallAbi.md)), and MemoryMap, for which the step gives the
`ObjectManager`'s mapper CPU0's shootdown dispatcher, the mailbox transport and a 10 µs progress wait. The pager runtime
then mints the pager's own AddressSpace with Map into words 18-19 and names the 16-page window at
`InitialUserMappingWindowStart` (0x4C000000) in words 20-21. The runtime's `ObjectManager` owns the pools behind the
pager's ObjectCreate, ObjectDestroy, MemoryMap and MemoryUnmap; the client and the spinner have no grant and no
management. After every such call of the pager (a destruction or an unmap reclaims what it released before it
returns), the trap path records the pools' counts, the grant's current and peak charge, the mappings MemoryMap and
MemoryUnmap completed and the last map's range in the pager's space in `Amd64ObjectManagementEvidence`. The pager's
root is resident only on CPU0, where its calls run, so each invalidation is local; the runner walks that root and
requires the window unmapped once the pager unmapped it. The same step gives the `ObjectManager` the frame pool's
scrubber (`Amd64FrameScrubber`), which zeroes each destroyed Frame's run through CPU0's scrub slot during the
management call that reclaims it: the pager's root is then CR3, Ready and resident on CPU0 alone, so the slot leaf can
be repointed and invalidated locally. The pool ends the self-test fully free, and the runner
reads the run the pager wrote through and requires it zero. Once the boot domains are admitted, the runtime requires the object registry to
still hold the pager channel's 16 FaultTokens and full Endpoint, Frame and MemoryObject pools (`REGISTRY-BUDGET`
otherwise) and records the registered objects, the charges of one Endpoint, Frame and MemoryObject, the Frames and
MemoryObjects created and reclaimed, the grant's committed and resident pages, and the frame pool's free and dirty
pages there.

## Created threads

The same step configures `Amd64UserThreadTable`, the platform's thread backend
([thread creation ABI](SystemCallAbi.md)), with the five seeded thread stacks and CPU0's descriptor tables and
entry context, and gives it to the `ObjectManager`'s thread pool; once the pager is initialized, its domain record (its
ProtectionDomain, image mappings, PagerChannel and the `ObjectManager`) is the one domain the table admits, and word 16
announces threads. A created thread's native slot holds a kernel context rebuilt for each start, its FXSAVE area
(initial x87 and SSE state), one thread stack and a launch frame: the context enters the requested entry at CPL3 on the
domain's root with the argument in RDI, after selecting the slot's stack for the thread's traps. The trap paths
classify the run loop's active thread as a created thread before any boot thread: its system calls go through the
common ingress with its domain's record, except ThreadExit, which finishes its run Exited, and WaitSetWait, which is
`InvalidConfiguration`; its user tick ends its run Ready like a boot thread's; its page fault goes to its domain's
supervisor, the pager, through the record's `IAmd64FaultSupervisor`. A supervised fault (a created thread's, the
client's or the spinner's) that finds the pager's channel holding another lets the pager progress, yields through
its thread's preemption path and faults again when it next runs, without a deferral-count limit. `Amd64SupervisionEvidence` counts the
boot threads' deferrals and `Amd64ThreadEvidence` the created threads' faults and deferrals. Before every ObjectCreate, ObjectDestroy, MemoryMap and MemoryUnmap, and on every pass of the idle loop, the table
unbinds each exited created thread and frees its native slot, so the ObjectDestroy that reclaims an exited thread also
frees its pool slot, `Threads` charge and metadata. `Amd64ThreadEvidence` records the pool's and the table's counts, the
last exit code, the free slots, the grant's `Threads` usage, the user ticks that ended a created thread's run, the
ObjectDestroy calls refused while it ran and one thread's charge (one `Thread`, one `SchedulingContext`, the native
slot and its 16 KiB stack); the runner requires the pager's first thread created, preempted, refused at least once,
exited, reclaimed and freed, and its `# thread: done` line once, its second thread faulted, terminated, reclaimed and
freed, and its third thread's `ud2` delivered as an exception (`Exceptions`), terminated (the last exit code
`FaultTerminatedExitCode`), reclaimed and freed.

## Created domains

With the test firmware's grant, `InitializeManagement` also prepares the domains user code may create
([domain creation ABI](SystemCallAbi.md), [object retirement](SystemCallAbi.md#object-management-semantics)). `RuntimeRoots`
is the `ObjectManager`'s `IAddressSpaceProvider` over sixteen arenas: five in
the runtime pool and eleven in the metadata arena. The pool layout is documented
[above](#ownership-and-publication). ObjectCreate type
2 prepares a free slot's arena from the retained seed plan exactly as `PrepareRoot` prepares a boot root and
initializes its AddressSpace charged to the creator's domain (the AddressSpace and its selected 64- or 256-page arena in kernel
metadata); once a destroyed space quiesced, `ReleaseSlot` ends its AddressSpace, page table and preparation so the
next creation prepares the arena again. Each CapabilitySpace slot gets its key from `RuntimeSpaceKeyPurpose(slot)`,
derived from the boot seed. `Amd64UserThreadTable` is the
`ICreatedDomainRecords` of the ProtectionDomain pool: beside the pager's boot record it holds bounded created-domain
records, each with its own `RecordUserMemoryAccess` bound to the domain's AddressSpace, the `ObjectManager`, no pager
resolution, the pager as supervisor and the domain's supervision tag, which the table puts in every fault delivery of
the domain's threads; a record retires only once no native slot holds a thread of it. A created domain's thread that
ends with ThreadExit signals the pager's readiness Notification (`OnExit`). The pager's WaitSetWait may time out
([wait timeout](SystemCallAbi.md#waitset-suspension-semantics)): `InitializePager` uses the calibrated TSC frequency to configure the timeout's microseconds; `ProgressFromIdle`, reached at every user tick and idle pass, expires a
blocked wait whose deadline passed with nothing ready, and `WaitForTickFromIdle` stops at that deadline, its stall
bound counted from there. `Amd64ComponentEvidence` reports the spaces created, still live and reclaimed
with their charges and free slots, the arenas prepared, the ProtectionDomains created, destroyed and reclaimed, the
faults of created domains' threads with the last tag, their exits and the pager's timed-out waits; the runner requires
the pager's component to have been built three times, crashed under tag 1, killed once by the watchdog after a
timed-out wait and exited once, every domain and space reclaimed, three arenas prepared and every slot of each space
free.

The same step configures the `ObjectManager`'s ResourceDomain pool on the boot ledger with identities from 3 on, and
word 16 announces it ([resource accounting](ResourceDomain.md)): the root service creates each
component's own ResourceDomain under the grant, charges everything it builds for the component to it, and tears the
component down by draining it, which destroys every object charged to it and takes its final mappings with its
AddressSpace. The grant's `ChildDomains` limit matches `ResourceDomainPool::Capacity`; its
metadata budget includes those ResourceDomains. The boot domain accounts for
the grant and its children, and the registry budget includes the pool. `Amd64ComponentEvidence` records the ResourceDomains created, destroyed,
reclaimed and still in a slot; the runner requires one per component, each drained and reclaimed, none live, and
counts MemoryUnmap calls only for the staging mappings. A fourth incarnation, after the reporting one, gets copies of
its own domain's capability (Manage and Inspect) and of its AddressSpace's (Map) and a two-page window at 0x68000000,
which its domain's page limit covers besides its image (start-block words 24-29): it creates an Endpoint, a Frame and
a MemoryObject there, maps them into the window, writes and reads each page, is refused one page more
(`ResourceExhausted`), reports and exits, and its drain takes what it created; the runner counts its objects, its
mapping and its exit, and requires its line.

The test firmware's cases attach QEMU's `edu`, and the loader captures its BAR0 through the PCI I/O protocol beside
the AHCI ABAR ([driver isolation](IommuDomain.md)). After the ECAM scan the boot publishes that BAR as a
read-write uncached DeviceRegion once it decodes to exactly the captured range on a published function that decodes
memory, and the pager runtime mints the function and the region into start-block words 22-25 (Inspect, Invoke,
Manage, Duplicate, Transfer; Inspect, Map, Read, Write, Duplicate, Transfer) with `PagerManagementDevices`. A fifth
incarnation, a driver, gets copies of its own domain and space and the window as the fourth does, and copies of the
region (Map, Read, Write) and function (Invoke) in words 30-33, and the root service assigns the function to its
ProtectionDomain (DeviceControl). The driver finds bus mastering refused (`InvalidConfiguration`, no IommuDomain yet)
and turns it off itself, which writes `edu`'s Command register through the run-time configuration access, legacy
ports 0xCF8/0xCFC, since the boot's ECAM window is closed before the APs join. It then creates a device Frame of the
region's first page and an uncached MemoryObject in its own domain, maps them into its window, reads `edu`'s
identification, checks its liveness word and computes 10! by polling.

Then it confines its DMA ([driver isolation](IommuDomain.md)):
1. It creates an IommuDomain of 32 bits in its own domain and attaches `edu`'s function.
2. It maps two pages of its own after the register page of its window (the window's two pages, which its domain's
   limit covers) and maps them for the device: one device-readable at 0x100000, one device-writable at 0x200000.
3. It makes `edu` a bus master and copies 256 bytes from the first page through `edu`'s buffer into the second.
4. It unmaps the second page from the device and repeats the copy with new bytes. The write faults, the page keeps
   the first bytes, and the domain's fault count grows by one.
5. It maps the page again, and the copy lands.
6. It stops bus mastering, detaches, reports and exits.

Its drain quiesces the IommuDomain, releases the assignment and takes the mappings. The driver polls `edu`'s command
register sparsely: each register access takes QEMU's global lock, which `edu`'s DMA timer needs too. The runner counts
its three Frames, MemoryObjects and mappings and its exit. It requires the line, one IommuDomain created and reclaimed,
and the unit's one fault: `edu`'s requester, a write, at 0x200000, reason 5 (QEMU's report of a zero leaf).

## DMA remapping

When the ACPI DMAR names exactly one remapping unit, on segment 0, with no flag but INCLUDE_PCI_ALL and no reserved
memory region, the seed maps the unit's register page uncached at its identity address
([driver isolation](IommuDomain.md)). After the boot's device path, and before the APs join,
the runtime does the following:
1. It turns off the bus mastering of every function the ECAM scan published, through the legacy configuration ports.
2. It gives the `VtdUnit` the root table and the four context tables of the pool, and the requesters the unit's
   one-hop endpoint scopes name (or every function under INCLUDE_PCI_ALL).
3. It enables translation with every context entry not present.

A unit the firmware left translating, one without a 39- or 48-bit walk, or a command that does not complete leaves the
machine without remapping, so no driver gets bus mastering. The test firmware's cases attach QEMU's `intel-iommu`
(interrupt remapping off) before `edu`. `Amd64EvidenceRecord` includes `Iommu`, refreshed with the component
evidence after polling the unit's faults. It records why no unit is used, the register base, the served and swept
function counts, the configuration and enabling errors, the levels, the version, capability, extended capability and
global status at handoff and now, the faults, and the IommuDomains created and reclaimed. The runner requires that every
published function is served and swept, and that the driver's stray write is the only fault.

Device interrupts ([driver isolation](IommuDomain.md)) arrive on vectors 0x40-0x4F of the BSP:
the runtime points `Amd64DeviceVectors` at the BSP's physical APIC ID and gives the ObjectManager's Interrupt pool those vectors,
so an Interrupt user code creates programs its function's MSI to 0xFEE00000 | (BSP id << 12) with the vector as data.
The evidence record reports Interrupt delivery and reclamation, device
assignment/quarantine, object collection and scrub work. The runner checks
that all created Interrupts and objects are reclaimed, quarantines are lifted
only after devices become quiet, and each scrubbed run contains at most 16 pages.

## User preemption

Every boot thread runs with a time slice: a budget equal to a 40M-TSC-tick
period (`MakeAmd64UserSchedulingConfiguration`), below the wait fixture's timer
delay ([run-loop policy](ProcessorKernelRunLoop.md)). The handling of a
timer interrupt that acknowledges a real deadline while CPL3 runs:

1. The interrupt is a scheduling point on the interrupted thread's own kernel
   stack.
2. It retries pager delivery and polls the pager's wait, as idle does.
3. It refreshes the active run's deadline. A deadline that has not fired leaves
   the thread running.
4. A fired deadline ends the run Ready through `FinishAndDispatch`, on the
   client's worker context or the pager's or spinner's own. When the fired deadline is the
   TimerQueue's, the tick is the wait fixture's interrupt: the fixture counts
   it, the next dispatch is rejected while the queue is due, and the idle loop
   drains the timer and wakes the waiting client (`HasObservedInterrupt`)
   instead of arming the timer again.
5. The thread returns to CPL3 once readmitted, with its kernel stack selected
   again.

The budget is replenished at the run stop, so a preempted thread queues behind
every other Ready thread, and a lone thread continues. Ticks taken at CPL0 keep
counting only; every one of them reaches the wait fixture, a user tick only when
the TimerQueue caused it, so a resumed wait records the interrupt of its own
timer. A late deadline the idle loop retries (an admission whose run stop
passed before entry) is bounded only while no timer interrupt shows that a
thread ran, and a suspended wait's timer is armed again at most eight times.

The test firmware adds a third boot domain to show that preemption works: the
spinner (`Amd64SpinnerRuntime`, image `Source/User/Platform/Amd64/Spinner`) in
its own root, with a `SpinnerSpace` capability space holding a
DiagnosticChannel capability and a WaitSet with no registration, its own kernel
stack and a time slice. It loads its own patterns into `xmm0`-`xmm15` and
MXCSR, counts 2^31 iterations without entering the kernel, checks the patterns,
writes `# spinner: done` (a TAP comment), or `# spinner: extended state lost`
if they changed, and reads page zero, which no root maps: the pager terminates
it. The guest loads different patterns at entry and checks them as its last TAP
point (`abi.extended-state`), after its preemptions, faults, system calls and
waits, so each of the two threads shows that the context switch keeps its
extended state ([extended processor state preservation](Amd64KernelContext.md#switch-boundary)); with
the restore disabled the spinner writes the second line and the runner fails the
case. The
queue order is the pager, the spinner, then the client, so without preemption
the client could not run before the spinner's line; the runner requires the
guest's complete TAP stream to come first. The client's and the spinner's page
faults reach the pager, their supervisor
([supervised fault termination](ThreadLifecycle.md)): each blocks on its
own thread, kernel context, AddressSpace and SchedulingContext, and the trap
path records a Terminate the pager made in `Amd64SupervisionEvidence` and
announces it as `ASTERIA:AMD64-PAGER:TERMINATED:<count>`. The production firmware
embeds an empty spinner image and has no spinner. `Amd64UserPreemptionEvidence`
records the user ticks, those whose fired deadline was the TimerQueue's, the
runs they ended per domain, and the spinner's presence, line and final wait.

## Application processors and shared-root publication

The BSP validates every ACPI processor record and starts the enabled ones
(`Amd64ProcessorTopology`). A record that is only online-capable describes a
processor that may be added later; since the runtime has no hotplug it is
validated and omitted, as on IA-64, instead of rejecting the topology. The BSP
locates itself using the enabled local APIC's full-width ID and assigns logical CPUs, itself first, without
assuming the first MADT entry is the BSP; enabled xAPIC IDs must be at most
254 in xAPIC mode; x2APIC uses distinct 32-bit IDs (excluding broadcast). The runtime, its shootdown mailbox and the evidence record
count the enabled processors. Startup uses a
serialized INIT/SIPI/SIPI exchange and bounded delays using the calibrated TSC. The first
low page contains relocation-free 16/64-bit startup code and is RX; the next is
an RW/NX mailbox containing the shared CR3, temporary GDT, stack and fixed entry.
An AP installs its own GDT/IDT/TSS/GS and joins the common AddressSpace controller
before releasing readiness. Only then may the BSP reuse the startup mailbox.

`Amd64LocalApic` observes IA32_APIC_BASE on the BSP and every AP without
changing the mode. The timer, EOI, INIT/SIPI and stop NMI paths share its MMIO
(xAPIC) or MSR (x2APIC) register backend. x2APIC ICR writes combine the full
destination and command into one ordered 64-bit MSR write; they do not poll
a delivery-status bit. The existing shootdown notifier shares this writer;
its unlinked interrupt binding and TSC-deadline adapter also use the accessor.
APs verify their selected hardware ID before publishing online state.

Before firmware exit, TSC calibration tries the Hyper-V TSC frequency MSR,
a complete CPUID 0x15 crystal ratio, the ACPI PM timer, HPET, then PIT
channel 2. The MSR (HV_X64_MSR_TSC_FREQUENCY, 0x40000022, in Hz; Microsoft
Hypervisor TLFS 6.0b Appendix C) is read only when CPUID.01H:ECX[31] reports
a hypervisor (TLFS section 2.2), leaf 0x40000000 reaches 0x40000003 and holds
"Microsoft Hv", leaf 0x40000001 holds "Hv#1", and leaf 0x40000003 sets both
EAX bit 11 (AccessFrequencyRegs, section 4.2.2) and EDX bit 8 (timer
frequencies available). The hypervisor presents the partition's TSC, so its
report precedes the architectural leaf. Hyper-V generation 2 environments may
offer no other usable source: CPUID 0x15, the PM timer and HPET can be absent,
and PIT reads can fail to advance.
Intel SDM volume 3B section 20.7.3 and volume 2 leaf 15H define the crystal
ratio. Leaf 0x16's nominal core frequency is not a TSC source, and
hypervisor leaf 0x40000010 is not an accepted frequency source
([hardware provenance](../Provenance.md)). Reference measurements preserve
IF, disable interrupts, retry TSC brackets wider than 50,000 cycles, and take
the minimum of three 50 ms runs. Ten thousand unchanged reads reject a stuck
counter. HPET uses 32-bit register accesses. If all sources fail, the firmware
console reports both present candidates and attempted sources (PIT is a
legacy candidate until measured).
AMD base/core frequency is not used as a TSC frequency. The PM timer is read-only
and handles 24/32-bit wrap at 3.579545 MHz. HPET uses its capabilities' period
and restores the prior enable configuration after a bounded counter sample.
Missing/stuck counters fall through. The boot line reports `AMD64-TSC:SOURCE`
and Hz, and `AMD64-APIC:MODE`; the evidence record appends TscFrequencyHz,
TscSource and X2Apic, covered by Runtime publication. No AP startup or runtime
delay uses PIT after calibration. Calibration failure and APIC-state rejection
are retained for the firmware console's handoff failure report.

Device interrupts still target only the BSP. Without interrupt remapping,
IOAPIC and MSI target admission rejects IDs >= 255 (255 is broadcast), even
when the IPI transport can address wider IDs. A BSP with such an ID fails
handoff explicitly rather than truncating a destination.

`amd64-x2apic-probe` changes only the test firmware's link-time entry. It sets
EXTD before calling the production entry and requires `-cpu max,+x2apic`.
`RunVmTests.py --suite Amd64X2Apic --jobs 2` exercises the accessor and checks
its mode evidence with four CPUs; the test-only link-time entry wrappers
set EXTD on the BSP and every AP before entering the common runtime. Every AP
independently rejects a mode mismatch with its BSP. `--suite Amd64NoPit` removes PIT and checks
that calibration selects another source. These are separate from the normal
Amd64 suite. Production sources have no test-only configuration dependency.

Each AP owns a common ProcessorRunController and a persistent cooperative worker.
The first dispatch blocks; a release/acquire service request wakes the exact
block token and dispatches the saved continuation. Each AP also calibrates its
local APIC countdown against its local TSC, enables interrupts in a bounded polling
corridor and requires one actual deadline interrupt before reporting readiness.
A probe deadline that falls due before the controller can program it (the vCPU
was not scheduled for the whole delay, `DeadlineReached`) is armed again, at
most eight times, instead of failing the AP.
The timer rejects a disabled APIC or a mismatched xAPIC base; long deadlines use bounded
countdown chunks and early interrupts rearm until the TSC deadline is reached.

Each AP also owns a registered Timer/Notification binding. Its common run
controller selects the TimerQueue deadline while the worker is blocked. The IRQ
only acknowledges the source and publishes a counter. Back on the idle stack,
the DeferredTimerWorker drains the queue, the AP claims and consumes the
Notification, and the controller refreshes the deadline. The AP fail-stops
(`ASTERIA:STOP:...:ApplicationProcessorStage`, with the stable AP stage in P1) only when an operation is rejected or no interrupt
arrives in time; it records the drain receipt, the claim, the final deadline
state and the LVT and initial-count read-back
(`Amd64ApplicationProcessorTimerObservation`) before it reports serving.
Readiness also requires one worker Wake/Block service cycle. The BSP copies each
AP's observations and counters into the evidence record and reports
`ASTERIA:STAGE:AMD64-AP-RUNTIME` once every AP serves; the VM observer judges each
AP's record (one interrupt after its deadline, one delivery of the AP's own
timer, claim sequence 1, the masked LVT, worker runs 1 and 2) and rejects
records of absent CPUs.

After every CPU is resident, the BSP maps pool page 224 at `0x60000000` through
Frame/MemoryObject ownership and the common pinned-mapping transaction. All
remote CPUs perform INVLPG through a bounded mailbox drain in their persistent
service loops. CompleteMapping runs only after the exact resident mask has
acknowledged. Every AP is dispatched again to read the new page. The BSP
records the probe page, the value it stored, the shootdown's masks and the word
each processor read (`Amd64SharedRootShootdownEvidence`) and reports
`ASTERIA:STAGE:AMD64-SHOOTDOWN`; the host compares the words with
`SharedProbeValue` and walks both roots. The private user root does not inherit
this diagnostic mapping. Service and shootdown
notification remain cooperative, as in the IA-64 runtime; there is no AP
scheduler preemption, migration or hotplug.

## PCI and AHCI bootstrap

Before final-map preparation, the AMD64 UEFI adapter captures one allocated
AHCI BAR5 resource through PCI I/O and frees both firmware buffers. Only its
scalar BDF/base/length survives firmware exit and successful retry checks.
After shared-root activation, the device owner enumerates the complete ACPI
ECAM bus ranges, publishes DeviceRegion/PciFunction authority, and binds AHCI
through the same catalog, BAR decoder, snapshot reader and command executors as
IA-64. No BAR sizing write or PCI configuration write is performed.

The AMD64 MMIO backend validates each retained DeviceRegion, access width,
permission, range and active-reference lifecycle. A BSP-only aperture at
`0x70000000` temporarily selects one physical page with supervisor RW/UC/NX
permissions, executes ordered DWORD I/O, restores the original leaf and
invalidates the local translation. The original seed is restored before every
return; mapping transactions and IRQs cannot overlap the aperture. It is closed
before AP startup. This permits the full 256-bus Q35 ECAM range without growing
the 64-page root arena or leaving a large MMIO identity map.

The Q35 bootstrap profile has coherent DMA, no IOMMU, one xAPIC controller and
at most one active SATA disk. The three DMA pages remain supervisor RW/NX and
permanently allocated, including every terminal failure requiring live DMA
storage. IDENTIFY establishes the port, LBA48 support and disk geometry; READ
DMA EXT derives those facts from the immutable receipt and reads one 512-byte
sector at LBA 8. Both operations check restoration receipts. ATAPI, ambiguous
active disks, and other command failures are rejected. No disk is an explicit
nonfatal outcome. The kernel implements no general storage service.

The device owner records the ECAM scan (regions, buses, published functions,
the first and the AHCI function), the IDENTIFY data and the read's receipt and
buffer address in the evidence record, and reports the stage events
`AMD64-PCI`, `AMD64-AHCI-IDENTIFY` and `AMD64-AHCI-READ`; it does not look at the
sector's contents. Firmware tests boot from VirtIO, disable default ATAPI devices
and separately attach a 2 MiB SATA test disk. QMP compares all 512 DMA bytes at
the recorded buffer with the source sector and verifies the restored MMIO leaf.

## One-shot native kernel preemption

`ASTERIA_AMD64_NATIVE_PREEMPTION_PROBE=1` selects a BSP-only diagnostic before
user admission. A common Thread/SchedulingContext (a 250M-tick budget in a
500M-tick period) runs a fixed assembly loop with interrupts enabled and no
locks or callbacks. The local timer interrupts that loop after its admitted
budget expires. Capture accepts only the expected RIP interval, worker stack,
selectors, deadline and masked/acknowledged APIC state. It retains the complete
integer frame and redirects IRETQ to a dedicated coordinator stack with IF
clear. Registry and scheduler operations run only after leaving the interrupt.

The proof's verdict does not depend on how fast the host runs the vCPU. When
the vCPU is not scheduled for longer than the admitted run between the
admission sample and the timer program or final entry check, the common run
loop rolls the idle dispatch back as Ready (`DeadlineReached`), and the next
dispatch finds the budget spent until the next replenishment. The probe then
waits for that replenishment and dispatches again, at most eight times. A
deadline that passed before the worker enabled interrupts leaves the timer
interrupt pending, and it preempts the loop's first instruction: that is still
the preemption being proven. The coordinator's readmission is the one
dispatch the common run loop does not roll back; the budget is its margin.

The coordinator completes the stopped run as Ready, verifies budget exhaustion,
waits for replenishment with a bounded poll, and readmits the same retained
frame. The common run loop checks its token, stopped/admitted run sequences and
resume generation. Before commit, the AMD64 context replaces the abandoned idle
continuation with a prepared fresh landing on a third, disjoint stack. IRETQ
then restores the captured frame; the loop stores all sixteen integer
registers, including RSP, before exiting through the common scheduler. Fresh
idle verifies Exited and unbinds the probe before ordinary CPL3 launch. The
coordinator stack can then become the user thread's exclusive syscall stack.
The probe records the captured frame, the stored registers, its capture, resume
and exit counts and the run loop's receipt (`Amd64NativePreemptionEvidence`) and
reports `ASTERIA:STAGE:AMD64-NATIVE-PREEMPTION`; the host compares the registers
with the frame and with the loop's sentinels, which `Amd64NativePreemptionProbe.hpp`
names and the loop's assembly takes from its generated layout header.

The default build disables this diagnostic. It proves a fixed lock-free CPL0
loop, integer-only state and one capture/resume/exit. Arbitrary kernel/user
preemption, periodic scheduling, AP preemption and extended-state preservation
remain outside this profile.

## Verification

TargetVerification checks O0/O2 and firmware kernel/user closures, per-function
stack bounds, unresolved symbols and final relocations. The closed ELF link
resolves COMDAT and localizes weak definitions before PE emission;
`FirmwareAmd64PeRelocations` proves exact DIR64 coverage without rewriting bytes.
`RuntimeMemoryLeaves` rejects compiler-generated calls from memory routines
back into themselves; kernel and user images use `-fno-builtin`.

Hosted tests cover staging/BSS, permissions, descriptor and stack geometry,
SYSCALL return sanitization, processor selection, UEFI retry and full arenas.
Guest tests report ABI observations through TAP. The runner checks stage order,
QMP evidence and exact stopped idle state, walks page tables for user/kernel
permissions, shared-root exclusion and guards, and inspects each CPU's
CR0/CR3/CR4, GDT/IDT/TSS, GS and parked state. Rejection oracles mutate captured
registers, tables and evidence, including ancestor U/S and R/W restrictions.
Evidence.bin and read pages remain under `Build/Vm/<run>/<case>/`.

Native-preemption presets additionally require the preemption stage before user
launch and validate capture/resume evidence while retaining SMP/device checks.
Default builds must contain no proof; variant presets use separate trees.

## Terminal system stop

BSP/AP runtime Fail stages and terminal traps use the
[processor stop path](ProcessorStops.md): per-CPU records, an NMI freeze,
bounded confirmation and the terminal framebuffer aperture. It matches the broker's last retained policy (WC normally, UC
after fallback), with WC buffers drained before Frozen and DrawPublished. Pool
pages 1780–2293 reserve its 514 table pages. Normal roots leave its PML4 branch
absent until the latch winner draws. The record's current schema is defined by `Amd64EvidenceRecord.hpp`.

Dedicated NMI stacks occupy pages 2296–2307 with guards, separate from the
machine-check IST3 stacks. See [Processor stops](ProcessorStops.md) for the
mode-aware LAPIC, physical-destination freeze and blue stop screen.

The unlinked TSC-deadline timer owns a cached accessor on its owning CPU.
The x2APIC-only shootdown notifier and binding cache an immutable MSR backend
at initialization; their all-x2APIC admission contract makes that backend
valid on each calling CPU without re-reading CPUID or IA32_APIC_BASE.
Stop identity lookup is independent of GS and reads the full x2APIC ID MSR
when EXTD is set, so IDs above 255 cannot alias another CPU's stop record.
