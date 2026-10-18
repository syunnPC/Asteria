// SPDX-License-Identifier: Apache-2.0
// Project-owned fault generators, from Intel SDM rev. 2.3, Volumes 2/3.
// No memory stack or structure offsets are consumed by these raw thread
// entries. r4 is their operand address, from the ThreadCreate argument ABI.
.explicit
.text

.macro AsteriaIa64FaultBegin name
    .align 16
    .global \name
    .hidden \name
    .type \name, @function
    .proc \name
\name:
.endm
.macro AsteriaIa64FaultEnd name
    .endp \name
    .size \name, . - \name
.endm
.macro AsteriaIa64FaultExit
    alloc r2 = ar.pfs, 0, 0, 1, 0
    ;;
    mov r32 = r8
    ;;
    br.call.sptk.many b0 = AsteriaIa64FaultScenarioExit
    ;;
1:
    br.sptk.few 1b
    ;;
.endm

AsteriaIa64FaultBegin AsteriaIa64FaultBreak
.global AsteriaIa64FaultBreakSite
AsteriaIa64FaultBreakSite:
{ .mii
    nop.m 0
    break.i 0
    nop.i 0
}
    ;;
AsteriaIa64FaultEnd AsteriaIa64FaultBreak

// PL3 can set this state itself (SDM rev. 1.1 Vol. 1 §3.1.8.2).
AsteriaIa64FaultBegin AsteriaIa64FaultEager
    alloc r2 = ar.pfs, 0, 0, 6, 0
    mov r15 = 15
    ;;
    mov ar.rsc = r15
    ;;
    .align 16
.global AsteriaIa64FaultEagerSite
AsteriaIa64FaultEagerSite:
{ .mii
    nop.m 0
    break.i 1048576
    nop.i 0
}
    ;;
AsteriaIa64FaultEnd AsteriaIa64FaultEager

// PL3 can set this state itself (SDM rev. 1.1 Vol. 1 §3.1.8.2).
AsteriaIa64FaultBegin AsteriaIa64FaultBigEndian
    alloc r2 = ar.pfs, 0, 0, 6, 0
    mov r15 = 28
    ;;
    mov ar.rsc = r15
    ;;
    .align 16
.global AsteriaIa64FaultBigEndianSite
AsteriaIa64FaultBigEndianSite:
{ .mii
    nop.m 0
    break.i 1048576
    nop.i 0
}
    ;;
AsteriaIa64FaultEnd AsteriaIa64FaultBigEndian

// PL3 can set this state itself (SDM rev. 1.1 Vol. 1 §3.1.8.2).
AsteriaIa64FaultBegin AsteriaIa64FaultLoadDistance
    alloc r2 = ar.pfs, 0, 0, 6, 0
    mov r15 = 65548
    ;;
    mov ar.rsc = r15
    ;;
    .align 16
.global AsteriaIa64FaultLoadDistanceSite
AsteriaIa64FaultLoadDistanceSite:
{ .mii
    nop.m 0
    break.i 1048576
    nop.i 0
}
    ;;
AsteriaIa64FaultEnd AsteriaIa64FaultLoadDistance

// PL3 can set this state itself (SDM rev. 1.1 Vol. 1 §3.1.8.2).
AsteriaIa64FaultBegin AsteriaIa64FaultShortFrame
    alloc r2 = ar.pfs, 0, 0, 0, 0
    mov r15 = 12
    ;;
    mov ar.rsc = r15
    ;;
    .align 16
.global AsteriaIa64FaultShortFrameSite
AsteriaIa64FaultShortFrameSite:
{ .mii
    nop.m 0
    break.i 1048576
    nop.i 0
}
    ;;
AsteriaIa64FaultEnd AsteriaIa64FaultShortFrame

// PL3 can set this state itself (SDM rev. 1.1 Vol. 1 §3.1.8.2).
AsteriaIa64FaultBegin AsteriaIa64FaultLocals
    alloc r2 = ar.pfs, 0, 1, 6, 0
    mov r15 = 12
    ;;
    mov ar.rsc = r15
    ;;
    .align 16
.global AsteriaIa64FaultLocalsSite
AsteriaIa64FaultLocalsSite:
{ .mii
    nop.m 0
    break.i 1048576
    nop.i 0
}
    ;;
AsteriaIa64FaultEnd AsteriaIa64FaultLocals

// PL3 can set this state itself (SDM rev. 1.1 Vol. 1 §3.1.8.2).
AsteriaIa64FaultBegin AsteriaIa64FaultRotating
    alloc r2 = ar.pfs, 0, 0, 8, 8
    mov r15 = 12
    ;;
    mov ar.rsc = r15
    ;;
    .align 16
.global AsteriaIa64FaultRotatingSite
AsteriaIa64FaultRotatingSite:
{ .mii
    nop.m 0
    break.i 1048576
    nop.i 0
}
    ;;
AsteriaIa64FaultEnd AsteriaIa64FaultRotating

// PL3 can set this state itself (SDM rev. 1.1 Vol. 1 §3.1.8.2).
AsteriaIa64FaultBegin AsteriaIa64FaultEagerBreak
    alloc r2 = ar.pfs, 0, 0, 0, 0
    mov r15 = 15
    ;;
    mov ar.rsc = r15
    ;;
    .align 16
.global AsteriaIa64FaultEagerBreakSite
AsteriaIa64FaultEagerBreakSite:
{ .mii
    nop.m 0
    break.i 0
    nop.i 0
}
    ;;
AsteriaIa64FaultEnd AsteriaIa64FaultEagerBreak

// PSR.di is privileged and set for every launch: br.ia must fault while
// still native (SDM rev. 1.1 Vol. 3, "br", Operation).
AsteriaIa64FaultBegin AsteriaIa64FaultIsaTransition
    mov r15 = ar.bsp
    ;;
    mov ar.bspstore = r15
    mov b6 = r0
    ;;
    .align 16
.global AsteriaIa64FaultIsaTransitionSite
AsteriaIa64FaultIsaTransitionSite:
{ .mib
    nop.m 0
    nop.i 0
    br.ia.sptk b6
}
    ;;
AsteriaIa64FaultEnd AsteriaIa64FaultIsaTransition

// PFS.pfm is writable even when its rotation exceeds the frame; br.ret
// copies it to CFM (SDM rev. 1.1 Vol. 1 §3.1.8.10; Vol. 3 "mov ar" / "br").
AsteriaIa64FaultBegin AsteriaIa64FaultFrameMarker
    movl r15 = 0xC000000000004000
    ;;
    movl r14 = AsteriaIa64FaultFrameMarkerSite
    ;;
    mov b6 = r14
    ;;
    .align 16
.global AsteriaIa64FaultFrameMarkerWrite
AsteriaIa64FaultFrameMarkerWrite:
{ .mii
    nop.m 0
    mov.i ar.pfs = r15
    nop.i 0
}
    ;;
    br.ret.sptk.many b6
    ;;
    .align 16
.global AsteriaIa64FaultFrameMarkerSite
AsteriaIa64FaultFrameMarkerSite:
{ .mii
    nop.m 0
    break.i 0
    nop.i 0
}
    ;;
AsteriaIa64FaultEnd AsteriaIa64FaultFrameMarker

// The tick (or a device interrupt) that finds this eager RSC is serviced first, then supervised as
// the thread's Exception. Every interruption in the loop lands on this bundle.
AsteriaIa64FaultBegin AsteriaIa64FaultEagerSpin
    mov r15 = 15
    ;;
    .align 16
.global AsteriaIa64FaultEagerSpinSite
AsteriaIa64FaultEagerSpinSite:
{ .mib
    mov.m ar.rsc = r15
    nop.i 0
    br.sptk.few AsteriaIa64FaultEagerSpinSite
}
    ;;
AsteriaIa64FaultEnd AsteriaIa64FaultEagerSpin

AsteriaIa64FaultBegin AsteriaIa64FaultPrivileged
    .align 16
.global AsteriaIa64FaultPrivilegedSite
AsteriaIa64FaultPrivilegedSite:
{ .mii
    mov r8 = psr
    nop.i 0
    nop.i 0
}
    ;;
AsteriaIa64FaultEnd AsteriaIa64FaultPrivileged

AsteriaIa64FaultBegin AsteriaIa64FaultNatRegister
    mov r15 = -1
    ;;
    mov ar.unat = r15
    ;;
    ld8.fill r15 = [r4]
    ;;
    .align 16
.global AsteriaIa64FaultNatRegisterSite
AsteriaIa64FaultNatRegisterSite:
{ .mii
    st8 [r4] = r15
    nop.i 0
    nop.i 0
}
    ;;
AsteriaIa64FaultEnd AsteriaIa64FaultNatRegister

AsteriaIa64FaultBegin AsteriaIa64FaultUnaligned
    sum psr.ac
    adds r15 = 1, r4
    ;;
    .align 16
.global AsteriaIa64FaultUnalignedSite
AsteriaIa64FaultUnalignedSite:
{ .mii
    ld8 r8 = [r15]
    nop.i 0
    nop.i 0
}
    ;;
AsteriaIa64FaultEnd AsteriaIa64FaultUnaligned

AsteriaIa64FaultBegin AsteriaIa64FaultDataRights
    .align 16
.global AsteriaIa64FaultDataRightsSite
AsteriaIa64FaultDataRightsSite:
{ .mii
    st8 [r4] = r0
    nop.i 0
    nop.i 0
}
    ;;
AsteriaIa64FaultEnd AsteriaIa64FaultDataRights

// A control-speculation check of a NaT register: chk.s branches to its
// recovery code, which a processor that does not implement that branch
// leaves to the Speculation vector (SDM Vol. 2, Speculation vector).
AsteriaIa64FaultBegin AsteriaIa64FaultSpeculation
    mov r15 = -1
    ;;
    mov ar.unat = r15
    ;;
    ld8.fill r15 = [r4]
    ;;
{ .mii
    nop.m 0
    chk.s.i r15, .Lasteria_ia64_fault_speculation_recovered
    nop.i 0
}
    ;;
    mov r8 = 0
    ;;
    br.sptk.few .Lasteria_ia64_fault_speculation_exit
    ;;
.Lasteria_ia64_fault_speculation_recovered:
    mov r8 = 77
    ;;
.Lasteria_ia64_fault_speculation_exit:
    AsteriaIa64FaultExit
AsteriaIa64FaultEnd AsteriaIa64FaultSpeculation

AsteriaIa64FaultBegin AsteriaIa64FaultInstruction
    mov b6 = r4
    ;;
    br.call.sptk.many b0 = b6
    ;;
    AsteriaIa64FaultExit
AsteriaIa64FaultEnd AsteriaIa64FaultInstruction

// The lazy executable page contains a position-independent leaf; its caller
// supplies b0 and GP. It is copied from these project-owned bytes, flushed,
// unmapped, and mapped RX only after its instruction miss reaches the pager.
.align 16
.global AsteriaIa64FaultLazyCode
.global AsteriaIa64FaultLazyCodeEnd
AsteriaIa64FaultLazyCode:
    mov r8 = 77
    ;;
    br.ret.sptk.many b0
    ;;
AsteriaIa64FaultLazyCodeEnd:

AsteriaIa64FaultBegin AsteriaIa64FaultRse
// Launch has CFM.sof=0. Rebind in enforced-lazy mode before allocating any
// frame; the new user backing store is initially entirely unmapped.
    mov ar.rsc = r0
    ;;
    mov ar.bspstore = r4
    ;;
    mov ar.rnat = r0
    mov r15 = 12
    ;;
    mov ar.rsc = r15
    ;;
    alloc r2 = ar.pfs, 0, 0, 1, 0
    ;;
    mov r32 = 12
    ;;
    br.call.sptk.many b0 = AsteriaIa64FaultRseGrow
    ;;
    AsteriaIa64FaultExit
AsteriaIa64FaultEnd AsteriaIa64FaultRse

// Bounded recursion: twelve frames of 82 logical registers, below 12 KiB
// including RNAT slots, on four lazy backing-store pages. No memory-stack
// recursion. At the leaf, unmap the existing RBS pages before returning so
// mandatory fills fault with ISR.rs/ir and an incomplete current frame.
AsteriaIa64FaultBegin AsteriaIa64FaultRseGrow
    alloc r34 = ar.pfs, 1, 80, 1, 0
    ;;
    mov r35 = b0
    mov r36 = r32
    adds r113 = -1, r32
    ;;
    cmp.ne p6, p7 = r113, r0
    ;;
(p6) br.cond.sptk.few .Lasteria_ia64_fault_rse_recurse
    ;;
    br.call.sptk.many b0 = AsteriaIa64FaultScenarioUnmapRse
    ;;
    br.sptk.few .Lasteria_ia64_fault_rse_return
    ;;
.Lasteria_ia64_fault_rse_recurse:
    br.call.sptk.many b0 = AsteriaIa64FaultRseGrow
    ;;
.Lasteria_ia64_fault_rse_return:
    cmp.ne p6, p7 = r36, r32
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_fault_rse_lost
    ;;
    mov r8 = 77
    mov ar.pfs = r34
    mov b0 = r35
    ;;
    br.ret.sptk.many b0
    ;;
.Lasteria_ia64_fault_rse_lost:
    break.i 0
    ;;
AsteriaIa64FaultEnd AsteriaIa64FaultRseGrow
