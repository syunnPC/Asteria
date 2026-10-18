// SPDX-License-Identifier: Apache-2.0

// Entry of the IA-64 boot pager image: the P64 entry descriptor the ELF
// entry names, the startup that calls AsteriaIa64PagerImageMain with the
// address of its start block, and the idle loop it falls into if the pager
// stops serving.

.explicit
.text
.align 16
.global AsteriaIa64PagerImageEntry
.hidden AsteriaIa64PagerImageEntry
.type AsteriaIa64PagerImageEntry, @function
.proc AsteriaIa64PagerImageEntry
AsteriaIa64PagerImageEntry:
// The launch boundary consumes the descriptor below and installs its
// second word in r1. Startup must preserve that loader-provided GP.
    alloc r2 = ar.pfs, 0, 0, 1, 0
    ;;
// The launch boundary passes the address of the start block in r4. The
// kernel wrote the block just below the initial memory stack pointer, so
// the stack continues below the block, with its 16-byte scratch area.
    adds r12 = -16, r4
    mov r32 = r4
    ;;
    br.call.sptk.many b0 = AsteriaIa64PagerImageMain
    ;;
.global AsteriaIa64PagerImageIdle
.hidden AsteriaIa64PagerImageIdle
.type AsteriaIa64PagerImageIdle, @function
AsteriaIa64PagerImageIdle:
    hint @pause
    br.sptk.few AsteriaIa64PagerImageIdle
    ;;
.endp AsteriaIa64PagerImageEntry
.size AsteriaIa64PagerImageIdle, . - AsteriaIa64PagerImageIdle
.size AsteriaIa64PagerImageEntry, . - AsteriaIa64PagerImageEntry
.section .AsteriaUserEntryDescriptor, "a", @progbits
.align 16
.global AsteriaIa64PagerImageEntryDescriptor
.hidden AsteriaIa64PagerImageEntryDescriptor
.type AsteriaIa64PagerImageEntryDescriptor, @object
AsteriaIa64PagerImageEntryDescriptor:
    data8 AsteriaIa64PagerImageEntry
    data8 __gp
.size AsteriaIa64PagerImageEntryDescriptor, 16
.previous

// The pager's self-test threads. The kernel launches each through the function descriptor its
// ObjectCreate names, so these entries need no descriptor of their own.
.text
.align 16
.global AsteriaIa64PagerThreadEntry
.hidden AsteriaIa64PagerThreadEntry
.type AsteriaIa64PagerThreadEntry, @function
.proc AsteriaIa64PagerThreadEntry
AsteriaIa64PagerThreadEntry:
// The launch passes the control block's address in r4 and the gp of the
// descriptor in r1; the memory stack pointer is already the top less 16.
// The thread loads its own f32-f127 pattern first (unlike the guest's),
// which AsteriaIa64PagerThreadStateIntact checks after it was preempted.
    alloc r2 = ar.pfs, 0, 0, 1, 0
    movl r14 = 0x5448524541440000
    ;;
    .irp index, 32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127
    adds r15 = \index, r14
    ;;
    setf.sig f\index = r15
    ;;
    .endr
    mov r32 = r4
    ;;
    br.call.sptk.many b0 = AsteriaIa64PagerThreadMain
    ;;
.Lasteria_ia64_pager_thread_idle:
    hint @pause
    br.sptk.few .Lasteria_ia64_pager_thread_idle
    ;;
.endp AsteriaIa64PagerThreadEntry
.size AsteriaIa64PagerThreadEntry, . - AsteriaIa64PagerThreadEntry

// The waiting self-test thread: the launch passes its control block's address in r4.
.align 16
.global AsteriaIa64PagerWaitingThreadEntry
.hidden AsteriaIa64PagerWaitingThreadEntry
.type AsteriaIa64PagerWaitingThreadEntry, @function
.proc AsteriaIa64PagerWaitingThreadEntry
AsteriaIa64PagerWaitingThreadEntry:
    alloc r2 = ar.pfs, 0, 0, 1, 0
    mov r32 = r4
    ;;
    br.call.sptk.many b0 = AsteriaIa64PagerWaitingThreadMain
    ;;
.Lasteria_ia64_pager_waiting_thread_idle:
    hint @pause
    br.sptk.few .Lasteria_ia64_pager_waiting_thread_idle
    ;;
.endp AsteriaIa64PagerWaitingThreadEntry
.size AsteriaIa64PagerWaitingThreadEntry, . - AsteriaIa64PagerWaitingThreadEntry

// The second self-test thread only reads page zero, a fault the pager does
// not back, so the pager terminates it; it has no other path.
.align 16
.global AsteriaIa64PagerFaultingThreadEntry
.hidden AsteriaIa64PagerFaultingThreadEntry
.type AsteriaIa64PagerFaultingThreadEntry, @function
.proc AsteriaIa64PagerFaultingThreadEntry
AsteriaIa64PagerFaultingThreadEntry:
    ld8 r8 = [r0]
    ;;
.Lasteria_ia64_pager_faulting_thread_idle:
    br.sptk.few .Lasteria_ia64_pager_faulting_thread_idle
    ;;
.endp AsteriaIa64PagerFaultingThreadEntry
.size AsteriaIa64PagerFaultingThreadEntry, . - AsteriaIa64PagerFaultingThreadEntry

// The third self-test thread reads the PSR, a privileged operation at PL3: the General Exception
// reaches the pager like a fault, which terminates it.
.align 16
.global AsteriaIa64PagerExceptionThreadEntry
.hidden AsteriaIa64PagerExceptionThreadEntry
.type AsteriaIa64PagerExceptionThreadEntry, @function
.proc AsteriaIa64PagerExceptionThreadEntry
AsteriaIa64PagerExceptionThreadEntry:
    mov r8 = psr
    ;;
.Lasteria_ia64_pager_exception_thread_idle:
    br.sptk.few .Lasteria_ia64_pager_exception_thread_idle
    ;;
.endp AsteriaIa64PagerExceptionThreadEntry
.size AsteriaIa64PagerExceptionThreadEntry, . - AsteriaIa64PagerExceptionThreadEntry

// The fourth self-test thread counts in the word its argument (r4) names and never stops; the pager
// kills it once it counted. The aligned 64-bit increment is atomic and publishes to the pager's
// acquire load.
.align 16
.global AsteriaIa64PagerSpinningThreadEntry
.hidden AsteriaIa64PagerSpinningThreadEntry
.type AsteriaIa64PagerSpinningThreadEntry, @function
.proc AsteriaIa64PagerSpinningThreadEntry
AsteriaIa64PagerSpinningThreadEntry:
    fetchadd8.rel r14 = [r4], 1
    ;;
    hint @pause
    br.sptk.few AsteriaIa64PagerSpinningThreadEntry
    ;;
.endp AsteriaIa64PagerSpinningThreadEntry
.size AsteriaIa64PagerSpinningThreadEntry, . - AsteriaIa64PagerSpinningThreadEntry

// Returns the interval time counter, the first thread's clock.
.align 16
.global AsteriaIa64PagerThreadClock
.hidden AsteriaIa64PagerThreadClock
.type AsteriaIa64PagerThreadClock, @function
.proc AsteriaIa64PagerThreadClock
AsteriaIa64PagerThreadClock:
    mov r8 = ar.itc
    br.ret.sptk.many b0
    ;;
.endp AsteriaIa64PagerThreadClock
.size AsteriaIa64PagerThreadClock, . - AsteriaIa64PagerThreadClock

// Returns 1 in r8 when f32-f127 still hold the first thread's pattern.
.align 16
.global AsteriaIa64PagerThreadStateIntact
.hidden AsteriaIa64PagerThreadStateIntact
.type AsteriaIa64PagerThreadStateIntact, @function
.proc AsteriaIa64PagerThreadStateIntact
AsteriaIa64PagerThreadStateIntact:
    movl r14 = 0x5448524541440000
    mov r8 = 1
    ;;
    .irp index, 32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127
    getf.sig r15 = f\index
    adds r16 = \index, r14
    ;;
    cmp.ne p6, p0 = r15, r16
    ;;
(p6) mov r8 = 0
    ;;
    .endr
    br.ret.sptk.many b0
    ;;
.endp AsteriaIa64PagerThreadStateIntact
.size AsteriaIa64PagerThreadStateIntact, . - AsteriaIa64PagerThreadStateIntact

// Makes the bytes the root service copied through its window coherent with
// instruction fetch before it maps them executable (SystemCallAbi.md,
// MemoryMap): fc.i on every 32-byte line of [address, address + bytes),
// then sync.i and srlz.i. The address is in r32 and the byte count in r33.
.align 16
.global AsteriaIa64PagerFlushCode
.hidden AsteriaIa64PagerFlushCode
.type AsteriaIa64PagerFlushCode, @function
.proc AsteriaIa64PagerFlushCode
AsteriaIa64PagerFlushCode:
    add r9 = r32, r33
    ;;
.Lasteria_ia64_pager_flush_line:
    cmp.ltu p6, p0 = r32, r9
    ;;
(p6) fc.i r32
(p6) adds r32 = 32, r32
(p6) br.cond.sptk.few .Lasteria_ia64_pager_flush_line
    ;;
    sync.i
    ;;
    srlz.i
    ;;
    br.ret.sptk.many b0
    ;;
.endp AsteriaIa64PagerFlushCode
.size AsteriaIa64PagerFlushCode, . - AsteriaIa64PagerFlushCode
