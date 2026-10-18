# SPDX-License-Identifier: Apache-2.0
"""What the IA-64 host observers compare against, from the build's sources.

Values the kernel chooses are read from the C++ sources of the tree through
AsteriaVerify.Layout (`constexpr` constants, `static_assert(name == value)`
pins, `enum class` enumerators), never copied into Python: user and probe
virtual layouts, launch PSR/RSC/FPSR, the timer vector and the interrupted
PSR contract, the bootstrap reservation's page geometry, the run-time
memory reservation's page, region identifiers, IVT vector offsets, the page-table entry flags of both
translation encoders, the AP runtime's local-timer parameters, the
shared-root shootdown page and the native-preemption worker's exit code.
A declaration that disappears or stops being a plain integer expression
raises `LayoutError` instead of being guessed.

The remaining literals are facts outside the kernel's sources, each naming
its origin: QEMU's Montecito PAL translation-register capacities and page
sizes, the architectural fail-stop IVT bundle, the ELF symbol names the
observers look up, and the stable-loop criterion of the runner itself.
Architectural IA-64 bit positions (PSR.ri, PTE and RR formats) live in the
observers that decode them.
"""
from __future__ import annotations

import dataclasses
import functools
import pathlib

from AsteriaVm import Build  # Noqa: F401 (puts Tools/Verify on sys.path)
from AsteriaVerify import Layout
from AsteriaVerify.Layout import LayoutError

# --- Facts outside the kernel's sources --------------------------------------

# QEMU Montecito PAL facts (qemu-system-ia64 target/ia64/cpu.c itr_count/dtr_count,
# cpu.h IA64_INSERTABLE_PAGE_SIZE_MASK): 32 ITRs, 32 DTRs, one ITR kept by the
# firmware. The checked page sizes omit QEMU's 1 GiB and 4 GiB sizes,
# which no kernel TR uses.
INSTRUCTION_TR_CAPACITY = 32
DATA_TR_CAPACITY = 32
FIRMWARE_INSTRUCTION_TRS = 1
INSERTABLE_PAGE_SIZES = (0x10000000, 0x04000000, 0x01000000, 0x00400000, 0x00100000,
                         0x00040000, 0x00010000, 0x00004000, 0x00002000, 0x00001000)

# The runtime IVT (Ia64RuntimeInterruptVectorTable.S): the fail-stop bundle
# (macro AsteriaIa64RuntimeFailStopBundle, `.byte` data in the assembly source)
# fills every unimplemented bundle; the break, external-interrupt, page-fault
# and General Exception vectors are one MIB trampoline to their entries.
IVT_FAIL_STOP_BUNDLE = bytes.fromhex('11000000010000000002000000000040')
IVT_SYSTEM_CALL_ENTRY = 'AsteriaIa64SystemCallEntry'
IVT_EXTERNAL_INTERRUPT_ENTRY = 'AsteriaIa64ExternalInterruptDispatchEntry'
IVT_LEGACY_EXTERNAL_ENTRY = 'AsteriaIa64ExternalInterruptEntry'
IVT_PAGE_FAULT_ENTRY = 'AsteriaIa64PageFaultNativeEntry'
IVT_USER_EXCEPTION_ENTRY = 'AsteriaIa64UserExceptionEntry'

# Symbols (names only; values come from the ELF files).
USER_IDLE_SYMBOL = 'AsteriaIa64MinimalUserImageIdle'
POLL_LEAF_SYMBOL = 'AsteriaIa64PollForExternalInterruptSequence'
PROBE_PAYLOAD_SYMBOL = 'AsteriaIa64UserExecutionProbePayloadStart'
PROBE_LOOP_SYMBOL = 'AsteriaIa64UserExecutionProbePostSystemCallLoop'
PROBE_CANONICAL_FRAME_OBJECT = 'UserProbeCanonicalFrame'
USER_CANONICAL_FRAME_OBJECT = 'CanonicalIpcFrame'
USER_CANONICAL_DELIVERY_OBJECT = 'CanonicalIpcDelivery'

# The runner's stable-loop criterion: three
# consecutive samples at the loop, within a 10 s budget.
LOOP_SAMPLES = 3
LOOP_BUDGET_SECONDS = 10.0

# The initial image's and the probe's user address space is the second region-
# identifier allocation (the shared kernel runtime takes the first), so region r
# uses Ia64FirstRuntimeRegionIdentifier + 8 * 1 + r (Ia64RegionIdentifierAllocator.cpp,
# an allocation-order fact the kernel does not name).
USER_ADDRESS_SPACE_ORDINAL = 1

# --- Sources ------------------------------------------------------------------

_LAUNCH = 'Source/Platform/Ia64/Scheduler/Ia64UserModeLaunch.hpp'
_KERNEL_CONTEXT = 'Source/Platform/Ia64/Scheduler/Ia64KernelContext.hpp'
_EXTERNAL = 'Source/Platform/Ia64/Interrupt/Ia64ExternalInterruptEntry.hpp'
_LAYOUT = 'Source/Platform/Ia64/Ia64UserVirtualLayout.hpp'
_PAYLOAD = 'Source/Platform/Ia64/Scheduler/Ia64UserExecutionProbePayload.hpp'
_BOOTSTRAP = 'Source/Platform/Ia64/Memory/BootstrapMappingPlan.hpp'
_REGION_SET = 'Source/Platform/Ia64/Memory/Ia64RuntimeRegionSet.hpp'
_REGION_ALLOCATOR = 'Source/Platform/Ia64/Memory/Ia64RegionIdentifierAllocator.hpp'
_IVT = 'Source/Platform/Ia64/Memory/Ia64RuntimeInterruptVectorTable.hpp'
_INITIAL_ENCODER = 'Source/Platform/Ia64/Memory/InitialTranslationEncoder.cpp'
_RUNTIME_ENCODER = 'Source/Platform/Ia64/Memory/Ia64RuntimeTranslationEncoder.cpp'
_AP_RUNTIME = 'Source/Platform/Ia64/Scheduler/Ia64ApplicationProcessorKernelRuntime.cpp'
_BSP_RUNTIME = 'Source/Platform/Ia64/Scheduler/Ia64BspKernelRuntime.hpp'
_BSP_RUNTIME_SOURCE = 'Source/Platform/Ia64/Scheduler/Ia64BspKernelRuntime.cpp'
_DELIVERY_CODEC = 'Source/Kernel/Ipc/IpcDeliveryCodec.cpp'
_DEADLINE = 'Source/Kernel/Time/ProcessorDeadlineController.hpp'
_TIMER = 'Source/Kernel/Time/Timer.hpp'
_NOTIFICATION = 'Source/Kernel/Ipc/Notification.hpp'
_LOCAL_TIMER = 'Source/Platform/Ia64/Time/Ia64LocalTimer.hpp'
_ABI = 'Source/Abi/SystemCallAbi.hpp'
_RUNTIME_MEMORY = 'Source/Platform/Ia64/Memory/Ia64BspRuntimeMemory.hpp'
_PAGER_RUNTIME = 'Source/Platform/Ia64/System/Ia64PagerRuntime.hpp'
_USER_FAULT = 'Source/Platform/Ia64/Interrupt/Ia64UserFaultEntry.hpp'
_ADDRESS_SPACE_POOL = 'Source/Kernel/Memory/AddressSpacePool.hpp'
_ADDRESS_SPACE_SLOTS = 'Source/Platform/Ia64/Memory/Ia64RuntimeAddressSpaceSlots.hpp'
_THREAD_POOL = 'Source/Kernel/Scheduler/ThreadPool.hpp'
_USER_PREEMPTION = 'Source/Platform/Ia64/System/Ia64UserPreemption.hpp'
_COMPONENT = 'Source/Abi/ComponentBootstrap.hpp'


class _Sources:
    """The constants, pins and enumerators of the tree's sources, read once each."""

    def __init__(self, root: pathlib.Path):
        self._root = root

    @functools.lru_cache(maxsize=None)
    def text(self, path: str) -> str:
        return Layout.read(self._root / path)

    @functools.lru_cache(maxsize=None)
    def constants(self, path: str) -> dict[str, int]:
        return Layout.constants(self.text(path))

    @functools.lru_cache(maxsize=None)
    def pins(self, path: str) -> dict[str, int]:
        return Layout.assertions(self.text(path))

    def value(self, path: str, name: str) -> int:
        """`constexpr... name{...}` of `path`, or its `static_assert(name == value)` pin."""
        value = self.constants(path).get(name, self.pins(path).get(name))
        if value is None:
            from AsteriaVm import ObjectLimits
            value = ObjectLimits.capacity(self._root, self.text(path), name)
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
    # CPU state (Ia64UserModeLaunch.hpp, Ia64KernelContext.hpp, Ia64ExternalInterruptEntry.hpp): the boot
    # domains' launch PSR (PL3 timer ticks enabled) and the synthetic user probe's (interrupts masked)
    user_psr: int
    probe_user_psr: int
    user_rsc: int
    user_stack_scratch: int
    user_fpsr: int
    timer_vector: int
    interrupted_psr: int            # Ia64ExternalInterruptInterruptedProcessorStatusValue
    interrupted_psr_mask: int       # Under Ia64ExternalInterruptInterruptedProcessorStatusMask
    # Probe user layout (Ia64UserVirtualLayout.hpp, Ia64UserExecutionProbePayload.hpp)
    page_size: int
    probe_code: int
    probe_stack: int
    probe_stack_pages: int
    probe_rse: int
    probe_rse_sentinel: int
    probe_parent_resume: int
    probe_sentinel: int
    probe_sentinel_count: int
    probe_query_response: int
    probe_query_information: int
    probe_lease: int
    probe_domain_selector: int
    probe_endpoint_selector: int
    probe_waitset_selector: int
    probe_bootstrap: int
    probe_bootstrap_selector: int
    probe_bootstrap_authenticator: int
    probe_bootstrap_frame: int
    probe_bootstrap_waitset_selector: int
    probe_bootstrap_waitset_authenticator: int
    probe_bootstrap_size: int
    probe_ipc_evidence: int
    probe_ipc_evidence_size: int
    probe_ipc_error: int
    probe_ipc_information: int
    probe_send_bytes: int
    probe_receive_bytes: int
    probe_query_error: int
    probe_delivery: int
    probe_delivery_size: int
    probe_wait_response: int
    probe_wait_response_size: int
    probe_wait_tag: int
    probe_wait_sequence: int
    probe_endpoint_rights: int      # UserProbeEndpointRights (Ia64BspKernelRuntime.cpp, pinned)
    delivery_rights_offset: int     # EffectiveRightsOffset of the AIPD header (IpcDeliveryCodec.cpp)
    # Bootstrap reservation (BootstrapMappingPlan.hpp; page offsets and counts)
    bootstrap_stack: tuple[int, int]
    bootstrap_storage: tuple[int, int]
    bootstrap_ivt: tuple[int, int]
    bootstrap_rse: tuple[int, int]
    bootstrap_alignment_pages: int
    # Region identifiers (Ia64RuntimeRegionSet.hpp, Ia64RegionIdentifierAllocator.hpp)
    kernel_rid: int
    region_page_shift: int
    first_runtime_rid: int
    rids_per_address_space: int
    # Implemented IVT vectors (Ia64RuntimeInterruptVectorTable.hpp)
    ivt_alternate_itlb: int
    ivt_alternate_dtlb: int
    ivt_break: int
    ivt_external: int
    ivt_page_faults: tuple[int, int, int, int]
    ivt_general_exception: int          # The General Exception vector
    ivt_floating_point_fault: int       # The Floating-point Fault and Trap vectors
    ivt_floating_point_trap: int
    user_exception_selector: int        # Its r16 selector for the user exception entry
    # Translation words the encoders produce (InitialTranslationEncoder.cpp for the
    # kernel's TRs, Ia64RuntimeTranslationEncoder.cpp for user TCs), without the
    # physical page number; the exception-deferral bit is kept apart.
    pte_kernel_rx: int
    pte_kernel_r: int
    pte_kernel_rw: int
    pte_user_rx: int
    pte_user_rw: int
    pte_exception_deferral: int
    # The AP runtime's local-timer proof (Ia64ApplicationProcessorKernelRuntime.cpp)
    ap_task_priority: int
    ap_poll_grace_ticks: int
    # BSP runtime (Ia64BspKernelRuntime.hpp)
    shootdown_virtual_start: int
    native_preemption_exit_code: int
    # Enumerators the probe's timer-queue evidence records as bytes
    deadline_armed: int
    deadline_disarmed: int
    deadline_source_timer_queue: int
    timer_disarmed: int
    timer_expired: int
    notification_ready: int
    local_timer_ready: int
    # Created threads
    fault_terminated_exit_code: int     # The exit code of a thread its pager ended (Abi/SystemCallAbi.hpp)
    killed_exit_code: int               # The exit code of a thread ThreadTerminate ended (Abi/SystemCallAbi.hpp)
    thread_slots: int                   # Created-thread entry slots (Ia64BspRuntimeMemory.hpp)
    thread_pool_slots: int              # The kernel's thread pool slots
    address_space_slots: int            # IA-64's run-time AddressSpace slots (Ia64RuntimeAddressSpaceSlots.hpp)
    domain_slots: int                   # Each domain pool's slots (AddressSpace, CapabilitySpace, ProtectionDomain)
    slice_ticks: int                    # A user time slice in ITC ticks, 50 ms of the emulator's ITC
    component_exit_code: int            # A reporting component incarnation's exit code
    account_exit_code: int              # An account incarnation's
    driver_exit_code: int               # A driver's without an IOMMU
    component_window_start: int         # The window an account incarnation maps into
    component_window_pages: int
    component_stack_start: int          # The component's stack region and start block (Abi/ComponentBootstrap.hpp)
    component_stack_pages: int
    # Created memory
    frame_pool_pages: int               # The frame pool the pager's Frames draw from (Ia64PagerRuntime.hpp)
    mapping_window: int                 # The pager's own window its MemoryMap uses (Ia64UserVirtualLayout.hpp)

    # Derived values ------------------------------------------------------------

    @property
    def probe_stack_page(self) -> int:
        """The last page of the probe's memory stack (bootstrap record and results)."""
        return self.probe_stack + self.probe_stack_pages * self.page_size - self.page_size

    @property
    def probe_initial_stack_pointer(self) -> int:
        return self.probe_stack + self.probe_stack_pages * self.page_size - self.user_stack_scratch

    def user_rid(self, region: int) -> int:
        return self.first_runtime_rid + self.rids_per_address_space * USER_ADDRESS_SPACE_ORDINAL + region


def _translation_words(sources: _Sources) -> dict[str, int]:
    """Page-table-entry flag words as the two encoders build them (write-back
    memory attribute 0): P|A|AR, plus D when writable, plus PL3 for user pages."""
    initial, runtime = sources.constants(_INITIAL_ENCODER), sources.constants(_RUNTIME_ENCODER)
    try:
        kernel = initial['PresentBit'] | initial['AccessedBit']
        user = runtime['PresentBit'] | runtime['AccessedBit'] | runtime['UserPrivilegeLevel']
        return {
            'pte_kernel_rx': kernel | initial['ReadExecuteAccessRights'],
            'pte_kernel_r': kernel | initial['ReadOnlyAccessRights'],
            'pte_kernel_rw': kernel | initial['DirtyBit'] | initial['ReadWriteAccessRights'],
            'pte_user_rx': user | runtime['ReadExecuteAccessRights'],
            'pte_user_rw': user | runtime['DirtyBit'] | runtime['ReadWriteAccessRights'],
            'pte_exception_deferral': initial['ExceptionDeferralBit'],
        }
    except KeyError as error:
        raise LayoutError('the translation encoders no longer define %s' % error) from None


@functools.lru_cache(maxsize=None)
def image_alignment() -> int:
    root = pathlib.Path(__file__).resolve().parents[3]
    return _Sources(root).value('Source/Platform/Ia64/Firmware/Ia64LoadedImageLayout.hpp',
                                'Ia64RelocatedImageAlignmentBytes')


@functools.lru_cache(maxsize=None)
def runtime_memory_page(root: pathlib.Path) -> int:
    """The run-time memory reservation's page in bytes, the preferred one
    (BootstrapMappingPlan.hpp): the runner's 512 MiB guest always has a free
    range for it, so the boot never takes the fallback there. Read from the
    tree at `root` rather than recorded with a run's HeaderConstants."""
    return 1 << _Sources(pathlib.Path(root)).value(_BOOTSTRAP, 'RuntimeMemoryReservationPreferredPageShift')


@functools.lru_cache(maxsize=None)
def headers(root: pathlib.Path) -> HeaderConstants:
    """Every value of HeaderConstants, read from the tree at `root`."""
    sources = _Sources(pathlib.Path(root))
    v = sources.value
    if runtime_deferral := sources.constants(_RUNTIME_ENCODER).get('ExceptionDeferralBit'):
        if runtime_deferral != sources.constants(_INITIAL_ENCODER).get('ExceptionDeferralBit'):
            raise LayoutError('the two translation encoders disagree on the exception-deferral bit')
    return HeaderConstants(
        user_psr=v(_LAUNCH, 'Ia64UserModePreemptibleProcessorStatus'),
        probe_user_psr=v(_LAUNCH, 'Ia64UserModeInitialProcessorStatus'),
        user_rsc=v(_LAUNCH, 'Ia64UserModeInitialRseConfiguration'),
        user_stack_scratch=v(_LAUNCH, 'Ia64UserModeInitialStackScratchBytes'),
        user_fpsr=v(_KERNEL_CONTEXT, 'Ia64KernelDefaultFloatingPointStatus'),
        timer_vector=v(_EXTERNAL, 'Ia64ExternalInterruptTimerVector'),
        interrupted_psr=v(_EXTERNAL, 'Ia64ExternalInterruptInterruptedProcessorStatusValue'),
        interrupted_psr_mask=v(_EXTERNAL, 'Ia64ExternalInterruptInterruptedProcessorStatusMask'),
        page_size=v(_LAYOUT, 'Ia64UserPageSizeBytes'),
        probe_code=v(_LAYOUT, 'Ia64UserExecutionProbeCodeVirtualStart'),
        probe_stack=v(_LAYOUT, 'Ia64UserExecutionProbeMemoryStackVirtualStart'),
        probe_stack_pages=v(_LAYOUT, 'Ia64UserExecutionProbeMemoryStackPageCount'),
        probe_rse=v(_LAYOUT, 'Ia64UserExecutionProbeRseBackingStoreVirtualStart'),
        probe_rse_sentinel=v(_PAYLOAD, 'Ia64UserExecutionProbeRseSentinelAddress'),
        probe_parent_resume=v(_PAYLOAD, 'Ia64UserExecutionProbeParentResumeOffsetBytes'),
        probe_sentinel=v(_PAYLOAD, 'Ia64UserExecutionProbeSentinel'),
        probe_sentinel_count=v(_PAYLOAD, 'Ia64UserExecutionProbeStackedSentinelCount'),
        probe_query_response=v(_PAYLOAD, 'Ia64UserExecutionProbeQueryResponseVirtualAddress'),
        probe_query_information=v(_PAYLOAD, 'Ia64UserExecutionProbeExpectedCapabilityQueryInformation'),
        probe_lease=v(_PAYLOAD, 'Ia64UserExecutionProbeExpectedCapabilityQueryLeaseDeadlineTicks'),
        probe_domain_selector=v(_PAYLOAD, 'Ia64UserExecutionProbeExpectedProtectionDomainCapabilitySelector'),
        probe_endpoint_selector=v(_PAYLOAD, 'Ia64UserExecutionProbeExpectedEndpointCapabilitySelector'),
        probe_waitset_selector=v(_PAYLOAD, 'Ia64UserExecutionProbeExpectedWaitSetCapabilitySelector'),
        probe_bootstrap=v(_PAYLOAD, 'Ia64UserExecutionProbeIpcBootstrapVirtualAddress'),
        probe_bootstrap_selector=v(_PAYLOAD, 'Ia64UserExecutionProbeIpcBootstrapSelectorOffsetBytes'),
        probe_bootstrap_authenticator=v(_PAYLOAD, 'Ia64UserExecutionProbeIpcBootstrapAuthenticatorOffsetBytes'),
        probe_bootstrap_frame=v(_PAYLOAD, 'Ia64UserExecutionProbeIpcBootstrapFrameOffsetBytes'),
        probe_bootstrap_waitset_selector=v(_PAYLOAD, 'Ia64UserExecutionProbeIpcBootstrapWaitSetSelectorOffsetBytes'),
        probe_bootstrap_waitset_authenticator=v(
            _PAYLOAD, 'Ia64UserExecutionProbeIpcBootstrapWaitSetAuthenticatorOffsetBytes'),
        probe_bootstrap_size=v(_PAYLOAD, 'Ia64UserExecutionProbeIpcBootstrapRecordSizeBytes'),
        probe_ipc_evidence=v(_PAYLOAD, 'Ia64UserExecutionProbeIpcEvidenceVirtualAddress'),
        probe_ipc_evidence_size=v(_PAYLOAD, 'Ia64UserExecutionProbeIpcEvidenceSizeBytes'),
        probe_ipc_error=v(_PAYLOAD, 'Ia64UserExecutionProbeExpectedIpcError'),
        probe_ipc_information=v(_PAYLOAD, 'Ia64UserExecutionProbeExpectedIpcInformation'),
        probe_send_bytes=v(_PAYLOAD, 'Ia64UserExecutionProbeExpectedSendTransferredBytes'),
        probe_receive_bytes=v(_PAYLOAD, 'Ia64UserExecutionProbeExpectedReceiveTransferredBytes'),
        probe_query_error=v(_PAYLOAD, 'Ia64UserExecutionProbeExpectedCapabilityQueryError'),
        probe_delivery=v(_PAYLOAD, 'Ia64UserExecutionProbeIpcDeliveryVirtualAddress'),
        probe_delivery_size=v(_PAYLOAD, 'Ia64UserExecutionProbeIpcDeliveryCapacityBytes'),
        probe_wait_response=v(_PAYLOAD, 'Ia64UserExecutionProbeWaitResponseVirtualAddress'),
        probe_wait_response_size=v(_PAYLOAD, 'Ia64UserExecutionProbeWaitResponseSizeBytes'),
        probe_wait_tag=v(_PAYLOAD, 'Ia64UserExecutionProbeExpectedWaitTag'),
        probe_wait_sequence=v(_PAYLOAD, 'Ia64UserExecutionProbeExpectedWaitRegistrationSequence'),
        probe_endpoint_rights=v(_BSP_RUNTIME_SOURCE, 'UserProbeEndpointRights.GetRawValue()'),
        delivery_rights_offset=v(_DELIVERY_CODEC, 'EffectiveRightsOffset'),
        bootstrap_stack=(v(_BOOTSTRAP, 'BootstrapStackPageOffset'), v(_BOOTSTRAP, 'BootstrapStackPageCount')),
        bootstrap_storage=(v(_BOOTSTRAP, 'BootstrapTranslationStoragePageOffset'),
                           v(_BOOTSTRAP, 'BootstrapTranslationStoragePageCount')),
        bootstrap_ivt=(v(_BOOTSTRAP, 'BootstrapInterruptVectorTablePageOffset'),
                       v(_BOOTSTRAP, 'BootstrapInterruptVectorTablePageCount')),
        bootstrap_rse=(v(_BOOTSTRAP, 'BootstrapRseBackingStorePageOffset'),
                       v(_BOOTSTRAP, 'BootstrapRseBackingStorePageCount')),
        bootstrap_alignment_pages=v(_BOOTSTRAP, 'BootstrapReservedAlignmentPages'),
        kernel_rid=v(_REGION_SET, 'Ia64SharedBootstrapRegionIdentifier'),
        region_page_shift=v(_REGION_SET, 'Ia64BootstrapPreferredPageShift'),
        first_runtime_rid=v(_REGION_ALLOCATOR, 'Ia64FirstRuntimeRegionIdentifier'),
        rids_per_address_space=v(_REGION_ALLOCATOR, 'Ia64RegionIdentifierCountPerAddressSpace'),
        ivt_alternate_itlb=v(_IVT, 'Ia64RuntimeAlternateInstructionTlbOffset'),
        ivt_alternate_dtlb=v(_IVT, 'Ia64RuntimeAlternateDataTlbOffset'),
        ivt_break=v(_IVT, 'Ia64RuntimeBreakOffset'),
        ivt_external=v(_IVT, 'Ia64RuntimeExternalInterruptOffset'),
        ivt_page_faults=(v(_IVT, 'Ia64RuntimePageNotPresentOffset'), v(_IVT, 'Ia64RuntimeKeyPermissionOffset'),
                         v(_IVT, 'Ia64RuntimeInstructionAccessRightsOffset'),
                         v(_IVT, 'Ia64RuntimeDataAccessRightsOffset')),
        ivt_general_exception=v(_IVT, 'Ia64RuntimeGeneralExceptionOffset'),
        ivt_floating_point_fault=v(_IVT, 'Ia64RuntimeFloatingPointFaultOffset'),
        ivt_floating_point_trap=v(_IVT, 'Ia64RuntimeFloatingPointTrapOffset'),
        user_exception_selector=v(_USER_FAULT, 'Ia64UserExceptionSelector'),
        **_translation_words(sources),
        ap_task_priority=v(_AP_RUNTIME, 'LocalTimerTaskPriority'),
        ap_poll_grace_ticks=v(_AP_RUNTIME, 'LocalTimerPollGraceTicks'),
        shootdown_virtual_start=v(_BSP_RUNTIME, 'Ia64BspSharedRootShootdownVirtualStart'),
        native_preemption_exit_code=v(_BSP_RUNTIME, 'Ia64BspNativePreemptionExitCode'),
        deadline_armed=sources.enumerator(_DEADLINE, 'ProcessorDeadlineState', 'Armed'),
        deadline_disarmed=sources.enumerator(_DEADLINE, 'ProcessorDeadlineState', 'Disarmed'),
        deadline_source_timer_queue=sources.enumerator(_DEADLINE, 'ProcessorDeadlineSource', 'TimerQueue'),
        timer_disarmed=sources.enumerator(_TIMER, 'TimerState', 'Disarmed'),
        timer_expired=sources.enumerator(_TIMER, 'TimerState', 'Expired'),
        notification_ready=sources.enumerator(_NOTIFICATION, 'NotificationState', 'Ready'),
        local_timer_ready=sources.enumerator(_LOCAL_TIMER, 'Ia64LocalTimerState', 'Ready'),
        fault_terminated_exit_code=v(_ABI, 'FaultTerminatedExitCode'),
        killed_exit_code=v(_ABI, 'KilledExitCode'),
        thread_slots=v(_RUNTIME_MEMORY, 'Ia64BspRuntimeThreadEntrySlots'),
        thread_pool_slots=v(_THREAD_POOL, 'Capacity'),
        address_space_slots=v(_ADDRESS_SPACE_SLOTS, 'Ia64RuntimeAddressSpaceSlotCount'),
        domain_slots=v(_ADDRESS_SPACE_POOL, 'Capacity'),
        slice_ticks=v(_USER_PREEMPTION, 'Ia64UserSchedulingPeriodTicks'),
        component_exit_code=v(_COMPONENT, 'ComponentExitCode'),
        account_exit_code=v(_COMPONENT, 'ComponentAccountExitCode'),
        driver_exit_code=v(_COMPONENT, 'ComponentDriverRegistersExitCode'),
        component_window_start=v(_COMPONENT, 'Ia64ComponentWindowStart'),
        component_window_pages=v(_COMPONENT, 'Ia64ComponentWindowPages'),
        component_stack_start=v(_COMPONENT, 'Ia64ComponentStackStart'),
        component_stack_pages=v(_COMPONENT, 'Ia64ComponentStackPages'),
        frame_pool_pages=v(_PAGER_RUNTIME, 'Ia64FramePoolPages'),
        mapping_window=v(_LAYOUT, 'Ia64InitialUserMappingWindowStart'))
