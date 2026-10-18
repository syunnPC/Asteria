// SPDX-License-Identifier: Apache-2.0

// IA-64 switch from the inherited SAL register stack to the owned initial
// memory stack and RSE backing store, entering the fixed owned continuation.

.text
.align 16
.global AsteriaIa64SwitchInitialExecutionContext
.hidden AsteriaIa64SwitchInitialExecutionContext
.type AsteriaIa64SwitchInitialExecutionContext, @function
AsteriaIa64SwitchInitialExecutionContext:
// Reject visible PSR state outside the physical, interruption-disabled
// install profile. The inherited CPL0/SAL bank contract remains a caller
// precondition because mov-from-PSR does not expose every PSR field.
// ar.rsc.loadrs may describe the inherited SAL frame that cover/flushrs
// drains below. Only mode and BE must already describe enforced-lazy,
// little-endian operation. PL is irrelevant while PSR.rt is clear.
// Reserved fields are ignored: no behavior depends on them, and ar.rsc is
// replaced before BSPSTORE is changed.
    mov r8 = psr
    mov r17 = ar.rsc
    ;;
    and r17 = 19, r17
    movl r16 = 0x0000401B0F064002
    ;;
    and r16 = r8, r16
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
    cmp.ne.or p6, p7 = r17, r0
    ;;
(p6) br.cond.dpnt.few 2f
    ;;
// Copy the stacked ABI inputs into static registers before cover creates
// a zero-sized current frame and abandons the caller's stacked inputs.
    mov r14 = r32
    mov r15 = r33
    ;;
    bsw.1
    ;;
// cover must end its group. flushrs must begin the next group and drains
// all dirty registers to the old firmware backing store before it is
// detached. The new zero-sized frame survives the transition.
    cover
    ;;
    flushrs
    ;;
    mov ar.rsc = r0
    ;;
    invala
    ;;
// A BSPSTORE write makes RNAT undefined, so install the fresh zero RNAT
// afterwards. PFS and RSC describe the new empty enforced-lazy context.
    mov ar.bspstore = r15
    ;;
    mov ar.rnat = r0
    mov ar.pfs = r0
    ;;
    mf
    ;;
    mov r12 = r14
    ;;
    br.call.sptk.many b0 = AsteriaIa64RunOwnedExecutionContext
    ;;
// Returning from the fixed continuation would expose an abandoned caller
// context, so this edge is an architecturally terminal fail-stop.
1:
    hint @pause
    ;;
    br.cond.sptk 1b
    ;;
2:
    mov r8 = 1
    br.ret.sptk.many b0
    ;;
.align 16
.size AsteriaIa64SwitchInitialExecutionContext, . - AsteriaIa64SwitchInitialExecutionContext
