# SPDX-License-Identifier: Apache-2.0
"""What the AMD64 host observers compare against, from the build's sources.

Values the kernel chooses are read from the C++ sources of the tree through
AsteriaVerify.Layout (`constexpr` constants, `static_assert(name == value)`
pins, `enum class` enumerators), never copied into Python: the initial user
stack, the kernel and user selectors, the per-CPU descriptor-table and
entry-context layout, the RFLAGS a CPL3 return may carry, the control-register
policy of the initial activation and of the per-CPU entry install (CR0 bits
set and cleared, the exact CR4 of every CPU), the MMIO
window, the initial image's lazy region, the pager's MemoryMap window and the pattern it writes there, the
runtime pool layout (root arenas, the frame pool, per-CPU stack extents, the probe and AHCI pages), the shared-root shootdown probe, the local APIC timer
vector and its masked LVT entry, the wait fixture's tag, the preemption
loop's register sentinels, and the enumerators the evidence record stores as
numbers. A declaration that disappears or stops being a plain integer
expression raises `LayoutError` instead of being guessed.

The remaining literals are facts outside the kernel's sources, each naming
its origin: QEMU's q35 ECAM bus range and the ELF symbol name of the user
images' idle loop. Architectural x86 bits (page-table entries, CR0/CR4,
RFLAGS, descriptors) live in the observers that decode them.
"""
from __future__ import annotations

import dataclasses
import functools
import pathlib

from AsteriaVm import Build  # Noqa: F401 (puts Tools/Verify on sys.path)
from AsteriaVerify import Layout
from AsteriaVerify.Layout import LayoutError

# --- Facts outside the kernel's sources --------------------------------------

# QEMU q35: its MCFG describes one ECAM segment for buses 0-255
# (the captured MCFG describes a 256 MiB window), which the kernel scans whole.
Q35_ECAM_BUSES = 256

# The user images' idle loop (Amd64MinimalUserImage.S entry stub, linked into
# both the production image and the guest test program).
USER_IDLE_SYMBOL = 'AsteriaAmd64MinimalUserImageIdle'

# --- Sources ------------------------------------------------------------------

_PAT = 'Source/Platform/Amd64/Memory/InitialPageTableBuilder.hpp'
_USER_IMAGE = 'Source/Platform/Amd64/Executable/Amd64InitialUserImage.hpp'
_USER_ENTRY = 'Source/Platform/Amd64/Interrupt/Amd64UserEntry.hpp'
_ACTIVATION = 'Source/Platform/Amd64/Memory/InitialAddressSpacePreparation.cpp'
_MMIO_WINDOW = 'Source/Platform/Amd64/Device/Amd64MmioWindow.hpp'
_POOL = 'Source/Platform/Amd64/Boot/Amd64RuntimePoolLayout.hpp'
_BOOT_RUNTIME = 'Source/Platform/Amd64/Boot/Amd64BootRuntime.hpp'
_APIC_TIMER = 'Source/Platform/Amd64/Time/Amd64ApicTimer.hpp'
_WAIT = 'Source/Abi/BootstrapFixture.hpp'
_PREEMPTION = 'Source/Platform/Amd64/Scheduler/Amd64NativePreemptionProbe.hpp'
_PCI_RUNTIME = 'Source/Platform/Device/PciEcamRuntime.hpp'
_DRAIN = 'Source/Kernel/Time/DeferredTimerWorker.hpp'
_NOTIFICATION = 'Source/Kernel/Ipc/Notification.hpp'
_DEADLINE = 'Source/Kernel/Time/ProcessorDeadlineController.hpp'
_ABI = 'Source/Abi/SystemCallAbi.hpp'
_SPINNER = 'Source/User/Spinner/SpinnerProgram.hpp'
_PAGER = 'Source/Abi/PagerBootstrap.hpp'
_COMPONENT = 'Source/Abi/ComponentBootstrap.hpp'
_PAGER_PROGRAM = 'Source/User/Pager/PagerProgram.hpp'
_ADDRESS_SPACE_POOL = 'Source/Kernel/Memory/AddressSpacePool.hpp'
_THREAD_POOL = 'Source/Kernel/Scheduler/ThreadPool.hpp'
_REGISTRY = 'Source/Kernel/Object/ObjectRegistry.hpp'
_FRAME_POOL = 'Source/Kernel/Memory/FrameObjectPool.hpp'
_MEMORY_OBJECT_POOL = 'Source/Kernel/Memory/MemoryObjectPool.hpp'

# The preemption loop's sentinels in the order the loop stores the registers
# (Amd64NativePreemptionProbe.S): rax, rbx, rcx, rdx, rbp, r8-r15.
SENTINEL_REGISTERS = ('Rax', 'Rbx', 'Rcx', 'Rdx', 'Rbp', 'R8', 'R9', 'R10', 'R11', 'R12', 'R13', 'R14', 'R15')


class _Sources:
    """The constants, pins and enumerators of the tree's sources, read once each."""

    def __init__(self, root: pathlib.Path):
        self._root = root

    @functools.lru_cache(maxsize=None)
    def text(self, path: str) -> str:
        return Layout.read(self._root / path)

    def value(self, path: str, name: str) -> int:
        """`constexpr... name{...}` of `path`, or its `static_assert(name == value)` pin."""
        text = self.text(path)
        value = Layout.constants(text).get(name, Layout.assertions(text).get(name))
        if value is None:
            from AsteriaVm import ObjectLimits
            value = ObjectLimits.capacity(self._root, text, name)
        if value is None:
            raise LayoutError('%s states no integer value for %s' % (path, name))
        return value

    def enumerator(self, path: str, enumeration: str, name: str) -> int:
        values = Layout.enumerators(self.text(path), enumeration)
        if name not in values:
            raise LayoutError('%s: %s has no enumerator %s' % (path, enumeration, name))
        return values[name]


@dataclasses.dataclass(frozen=True)
class HeaderConstants:
    # The initial user image and the CPL3 entry (Amd64InitialUserImage.hpp, Amd64UserEntry.hpp)
    expected_pat: int
    reserved_arena_first_page: int
    scanout_address: int
    user_stack_start: int
    user_stack_pages: int
    lazy_start: int        # The initial image's lazily backed region, which the boot pager serves
    lazy_pages: int
    mapping_window_start: int  # The window of the pager's own space its MemoryMap fills
    mapping_window_pages: int
    mapped_page_pattern: int   # The first word of each page the pager maps (Abi/PagerBootstrap.hpp)
    kernel_code_selector: int
    kernel_data_selector: int
    user_code_selector: int
    user_data_selector: int
    descriptor_tables_size: int
    idt_offset: int
    entry_context_size: int
    user_rflags_required: int
    user_rflags_permitted: int
    # Control registers every CPU keeps: the initial activation's CR0
    # requirements (InitialAddressSpacePreparation.cpp) plus what the entry
    # install sets and clears, and the exact CR4 the install gives every CPU
    cr0_required: int
    cr0_forbidden: int
    cr4_installed: int
    # Devices (Amd64MmioWindow.hpp, Amd64ApicTimer.hpp)
    mmio_window_address: int
    timer_vector: int
    timer_masked_lvt: int
    # The runtime pool (Amd64RuntimePoolLayout.hpp), in pages
    page_size: int
    pool_pages: int
    frame_pool_first_page: int         # The frame pool created Frames draw from
    frame_pool_pages: int
    root_arena_pages: int
    ap_idle_root_first_page: int
    shared_root_index: int
    user_root_index: int
    pager_root_index: int
    spinner_root_index: int            # Built only by the test firmware, the one with a spinner
    scrub_slot_first_page: int         # Each processor's scrub slot page
    scrub_slot_virtual_address: int    # Where every root maps processor 0's slot; processor p's is p pages on
    thread_stack_slots: int            # Created threads' kernel stacks in the runtime pool
    reserved_arenas: int               # Run-time AddressSpaces' arenas in the runtime pool
    domain_slots: int                  # Each domain pool's slots
    thread_slots: int                  # The thread pool's slots, as many as the native slots
    # The metadata arena, in arena pages: the
    # arenas and stacks of the slots beyond the runtime pool's
    arena_root_arena_first_page: int
    arena_root_arenas: int
    arena_thread_stack_first_page: int
    arena_thread_stack_slots: int
    arena_slab_first_page: int
    arena_maximum_pages: int
    registry_capacity: int             # The ObjectRegistry's entries
    frame_slots: int                   # The Frame pool's slots
    memory_object_slots: int           # The MemoryObject pool's slots
    thread_stack_first_page: int
    thread_stack_stride_pages: int
    kernel_stack_pages: int
    stack_first_page: int
    stack_extent_pages: int
    stack_slots: int
    idle_stack: tuple[int, int]
    worker_stack: tuple[int, int]
    system_call_stack: tuple[int, int]
    double_fault_stack: tuple[int, int]
    machine_check_stack: tuple[int, int]
    shared_probe_page: int
    ahci_read_data_page: int
    # Diagnostics (Amd64BootRuntime.hpp, Amd64WaitRuntime.hpp, Amd64NativePreemptionProbe.hpp)
    shared_probe_address: int
    shared_probe_value: int
    wait_tag: int
    preemption_sentinels: tuple[int,...]
    # Enumerators the evidence record stores as numbers
    drain_stop_empty: int
    notification_ready: int
    deadline_disarmed: int
    ahci_ready: int
    system_call_error_none: int
    fault_terminated_exit_code: int     # The exit code of a thread its pager ended (Abi/SystemCallAbi.hpp)
    killed_exit_code: int               # The exit code of a thread ThreadTerminate ended (Abi/SystemCallAbi.hpp)
    waiting_exit_code: int              # The pager's waiting thread's, its wait timed out (PagerProgram.hpp)
    spinner_fault_address: int          # The word the spinner reads after its line (SpinnerProgram.hpp)
    # The component the pager builds in created domains (Abi/ComponentBootstrap.hpp).
    component_exit_code: int            # Its second incarnation's exit code
    interpreted_exit_code: int          # An interpreter's, after its report
    component_window_start: int
    component_stack_start: int          # Its stack, under the start-block page
    component_stack_pages: int

    # Derived values ------------------------------------------------------------

    @property
    def stacks(self) -> tuple[tuple[int, int],...]:
        """Every (page offset, pages) stack run of a per-CPU extent; the other pages are guards."""
        return (self.idle_stack, self.worker_stack, self.system_call_stack, self.double_fault_stack,
                self.machine_check_stack)

    def pool_page(self, pool: int, page: int) -> int:
        return pool + page * self.page_size

    def root(self, pool: int, index: int) -> int:
        """The physical root table of page-table arena `index`."""
        return self.pool_page(pool, index * self.root_arena_pages)

    def stack_extent(self, pool: int, cpu: int) -> int:
        """The first page of CPU `cpu`'s kernel stack extent."""
        return self.pool_page(pool, self.stack_first_page + cpu * self.stack_extent_pages)

    def thread_stacks(self, pool: int) -> list[int]:
        """The first physical page of each created-thread kernel stack slot in the runtime pool."""
        return [self.pool_page(pool, self.thread_stack_first_page + slot * self.thread_stack_stride_pages)
                for slot in range(self.thread_stack_slots)]

    def arena_thread_stacks(self, arena: int) -> list[int]:
        """The first physical page of each created-thread kernel stack slot in the metadata arena."""
        return [arena + (self.arena_thread_stack_first_page + slot * self.thread_stack_stride_pages) * self.page_size
                for slot in range(self.arena_thread_stack_slots)]

    def idle_root(self, pool: int, cpu: int) -> int:
        return self.root(pool, self.shared_root_index) if cpu == 0 else self.pool_page(
            pool, self.ap_idle_root_first_page + (cpu - 1) * self.root_arena_pages)

    def arena_roots(self, arena: int) -> list[int]:
        """The root table of each created AddressSpace's page-table arena in the metadata arena."""
        return [arena + (self.arena_root_arena_first_page + slot * self.root_arena_pages) * self.page_size
                for slot in range(self.arena_root_arenas)]

    def idle_stack_range(self, pool: int, cpu: int) -> tuple[int, int]:
        """[low, high) of CPU `cpu`'s idle stack (an AP's parked context)."""
        base = self.stack_extent(pool, cpu)
        offset, pages = self.idle_stack
        return base + offset * self.page_size, base + (offset + pages) * self.page_size

    def entry_stack_top(self, pool: int, cpu: int) -> int:
        """RSP0 and the SYSCALL stack of CPU `cpu`: the end of its system-call stack."""
        offset, pages = self.system_call_stack
        return self.stack_extent(pool, cpu) + (offset + pages) * self.page_size


@functools.lru_cache(maxsize=None)
def headers(root: pathlib.Path) -> HeaderConstants:
    """Every value of HeaderConstants, read from the tree at `root`."""
    sources = _Sources(pathlib.Path(root))
    v = sources.value

    def stack(name: str, pages: str) -> tuple[int, int]:
        return v(_POOL, name), v(_POOL, pages)

    return HeaderConstants(
        expected_pat=v(_PAT, 'ExpectedInitialPatValue'),
        reserved_arena_first_page=v(_POOL, 'ReservedArenaFirstPage'),
        scanout_address=v(_COMPONENT, 'ComponentScanoutAddress'),
        user_stack_start=v(_USER_IMAGE, 'InitialUserStackStart'),
        user_stack_pages=v(_USER_IMAGE, 'InitialUserStackPages'),
        lazy_start=v(_USER_IMAGE, 'InitialUserLazyStart'),
        lazy_pages=v(_USER_IMAGE, 'InitialUserLazyPages'),
        mapping_window_start=v(_USER_IMAGE, 'InitialUserMappingWindowStart'),
        mapping_window_pages=v(_USER_IMAGE, 'InitialUserMappingWindowPages'),
        mapped_page_pattern=v(_PAGER, 'MappedPagePattern'),
        kernel_code_selector=v(_USER_ENTRY, 'KernelCodeSelector'),
        kernel_data_selector=v(_USER_ENTRY, 'KernelDataSelector'),
        user_code_selector=v(_USER_ENTRY, 'UserCodeSelector'),
        user_data_selector=v(_USER_ENTRY, 'UserDataSelector'),
        descriptor_tables_size=v(_USER_ENTRY, 'sizeof(Amd64DescriptorTables)'),
        idt_offset=v(_USER_ENTRY, '__builtin_offsetof(Amd64DescriptorTables,Idt)'),
        entry_context_size=v(_USER_ENTRY, 'sizeof(Amd64EntryContext)'),
        user_rflags_required=v(_USER_ENTRY, 'UserReturnRequiredFlags'),
        user_rflags_permitted=v(_USER_ENTRY, 'UserReturnPermittedFlags'),
        cr0_required=v(_ACTIVATION, 'RequiredControlRegister0Bits') | v(_USER_ENTRY, 'InstalledControlRegister0Set'),
        cr0_forbidden=v(_USER_ENTRY, 'InstalledControlRegister0Clear'),
        cr4_installed=v(_USER_ENTRY, 'InstalledControlRegister4'),
        mmio_window_address=v(_MMIO_WINDOW, 'MmioWindowVirtualAddress'),
        timer_vector=v(_APIC_TIMER, 'ApicTimerVector'),
        timer_masked_lvt=v(_APIC_TIMER, 'ApicTimerMaskedLvt'),
        page_size=v(_POOL, 'RuntimePoolPageBytes'),
        pool_pages=v(_POOL, 'RuntimePoolPages'),
        frame_pool_first_page=v(_POOL, 'FramePoolFirstPage'),
        frame_pool_pages=v(_POOL, 'FramePoolPages'),
        root_arena_pages=v(_POOL, 'RootArenaPages'),
        ap_idle_root_first_page=v(_POOL, 'ApIdleRootFirstPage'),
        shared_root_index=v(_POOL, 'SharedRootIndex'),
        user_root_index=v(_POOL, 'UserRootIndex'),
        pager_root_index=v(_POOL, 'PagerRootIndex'),
        spinner_root_index=v(_POOL, 'SpinnerRootIndex'),
        scrub_slot_first_page=v(_POOL, 'ScrubSlotFirstPage'),
        scrub_slot_virtual_address=v(_POOL, 'ScrubSlotVirtualAddress'),
        thread_stack_slots=v(_POOL, 'ThreadStackSlots'),
        reserved_arenas=v(_POOL, 'ReservedArenaCount'),
        domain_slots=v(_ADDRESS_SPACE_POOL, 'Capacity'),
        thread_slots=v(_THREAD_POOL, 'Capacity'),
        arena_root_arena_first_page=v(_POOL, 'MetadataArenaRootArenaFirstPage'),
        arena_root_arenas=v(_POOL, 'MetadataArenaRootArenaCount'),
        arena_thread_stack_first_page=v(_POOL, 'MetadataArenaThreadStackFirstPage'),
        arena_thread_stack_slots=v(_POOL, 'MetadataArenaThreadStackSlots'),
        arena_slab_first_page=v(_POOL, 'MetadataArenaSlabFirstPage'),
        arena_maximum_pages=v(_POOL, 'MetadataArenaTablePages') * v(_POOL, 'MetadataArenaAlignmentPages'),
        registry_capacity=v(_REGISTRY, 'MaximumObjects'),
        frame_slots=v(_FRAME_POOL, 'Capacity'),
        memory_object_slots=v(_MEMORY_OBJECT_POOL, 'Capacity'),
        thread_stack_first_page=v(_POOL, 'ThreadStackFirstPage'),
        thread_stack_stride_pages=v(_POOL, 'ThreadStackStridePages'),
        kernel_stack_pages=v(_POOL, 'KernelStackPages'),
        stack_first_page=v(_POOL, 'ProcessorStackFirstPage'),
        stack_extent_pages=v(_POOL, 'ProcessorStackExtentPages'),
        stack_slots=v(_POOL, 'ProcessorStackSlots'),
        idle_stack=stack('IdleStackPage', 'KernelStackPages'),
        worker_stack=stack('WorkerStackPage', 'KernelStackPages'),
        system_call_stack=stack('SystemCallStackPage', 'KernelStackPages'),
        double_fault_stack=stack('DoubleFaultStackPage', 'InterruptStackPages'),
        machine_check_stack=stack('MachineCheckStackPage', 'InterruptStackPages'),
        shared_probe_page=v(_POOL, 'SharedProbePage'),
        ahci_read_data_page=v(_POOL, 'AhciReadDataPage'),
        shared_probe_address=v(_BOOT_RUNTIME, 'SharedProbeVirtualAddress'),
        shared_probe_value=v(_BOOT_RUNTIME, 'SharedProbeValue'),
        wait_tag=v(_WAIT, 'BootstrapWaitTag'),
        preemption_sentinels=tuple(v(_PREEMPTION, 'NativePreemptionSentinel' + register)
                                   for register in SENTINEL_REGISTERS),
        drain_stop_empty=sources.enumerator(_DRAIN, 'DeferredTimerDrainStop', 'Empty'),
        notification_ready=sources.enumerator(_NOTIFICATION, 'NotificationState', 'Ready'),
        deadline_disarmed=sources.enumerator(_DEADLINE, 'ProcessorDeadlineState', 'Disarmed'),
        ahci_ready=sources.enumerator(_PCI_RUNTIME, 'AhciPciAvailability', 'Ready'),
        system_call_error_none=sources.enumerator(_ABI, 'SystemCallError', 'None'),
        fault_terminated_exit_code=v(_ABI, 'FaultTerminatedExitCode'),
        killed_exit_code=v(_ABI, 'KilledExitCode'),
        waiting_exit_code=v(_PAGER_PROGRAM, 'PagerWaitingThreadExitCode'),
        spinner_fault_address=v(_SPINNER, 'SpinnerFaultAddress'),
        component_exit_code=v(_COMPONENT, 'ComponentExitCode'),
        interpreted_exit_code=v(_COMPONENT, 'ComponentInterpretedExitCode'),
        component_window_start=v(_COMPONENT, 'ComponentWindowStart'),
        component_stack_start=v(_COMPONENT, 'ComponentStackStart'),
        component_stack_pages=v(_COMPONENT, 'ComponentStackPages'))
