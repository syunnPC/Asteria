// SPDX-License-Identifier: Apache-2.0

// IA-64 user-execution probe payload, the user code the kernel copies into the
// probe page, and its address query. Addresses, offsets, system-call numbers
// and expected responses come from the generated C++ layout.

#include "Generated/Platform/Ia64/Ia64UserExecutionProbePayloadAsmLayout.h"

.text
.explicit
.align 16
.global AsteriaIa64UserExecutionProbePayloadStart
.hidden AsteriaIa64UserExecutionProbePayloadStart
.type AsteriaIa64UserExecutionProbePayloadStart, @function
AsteriaIa64UserExecutionProbePayloadStart:
    alloc r17 = ar.pfs, 0, 4, 0, 0
    ;;
    adds r14 = 0, r12
    movl r18 = ASTERIA_IA64_USER_EXECUTION_PROBE_SENTINEL
    ;;
    st8 [r14] = r18
    mov r32 = r18
    adds r33 = 1, r18
    ;;
    adds r34 = 2, r18
    adds r35 = 3, r18
    ;;
    mf
    ;;
    br.call.sptk.many b0 = AsteriaIa64UserExecutionProbeSpillChild
    ;;
    xor r20 = r32, r18
    adds r21 = 1, r18
    ;;
    xor r21 = r33, r21
    ;;
    or r20 = r20, r21
    adds r21 = 2, r18
    ;;
    xor r21 = r34, r21
    ;;
    or r20 = r20, r21
    adds r21 = 3, r18
    ;;
    xor r21 = r35, r21
    ;;
    or r20 = r20, r21
    ;;
    cmp.eq p6, p7 = r20, r0
    ;;
(p6) mov r22 = r18
(p7) mov r22 = r0
    ;;
    movl r16 = ASTERIA_IA64_USER_EXECUTION_PROBE_RSE_SENTINEL_ADDRESS
    ;;
    st8 [r16] = r22
    ;;
    mov r14 = r0
    mov r16 = r0
    mov r17 = r0
    ;;
    mov r18 = r0
    mov r20 = r0
    mov r21 = r0
    ;;
    mov r22 = r0
    ;;
    mf
    ;;
.global AsteriaIa64UserExecutionProbePostSystemCallLoop
.hidden AsteriaIa64UserExecutionProbePostSystemCallLoop
.type AsteriaIa64UserExecutionProbePostSystemCallLoop, @function
AsteriaIa64UserExecutionProbePostSystemCallLoop:
.Lasteria_ia64_user_execution_probe_post_system_call_loop:
    br.sptk.few .Lasteria_ia64_user_execution_probe_post_system_call_loop
    ;;
.align 16
.size AsteriaIa64UserExecutionProbePostSystemCallLoop, . - AsteriaIa64UserExecutionProbePostSystemCallLoop
.local AsteriaIa64UserExecutionProbeSpillChild
.type AsteriaIa64UserExecutionProbeSpillChild, @function
AsteriaIa64UserExecutionProbeSpillChild:
    alloc r19 = ar.pfs, 0, 0, 6, 0
    mov r20 = b0
    ;;
    mov r28 = r14
    mov r29 = r15
    ;;
    flushrs
    ;;
// Completion remains zero unless both IPC response triples and the staged
// selector agree exactly. The staged authenticator is copied opaquely.
    movl r16 = ASTERIA_IA64_USER_EXECUTION_PROBE_IPC_EVIDENCE_VIRTUAL_ADDRESS
    ;;
    adds r17 = ASTERIA_IA64_USER_EXECUTION_PROBE_IPC_COMPLETION_OFFSET_BYTES, r16
    ;;
    st8 [r17] = r0
    ;;
    mov r15 = ASTERIA_IA64_USER_EXECUTION_PROBE_CAPABILITY_QUERY_SYSTEM_CALL_NUMBER
    mov r32 = r4
    mov r33 = r5
    ;;
    mov r34 = r0
    mov r35 = r0
    mov r36 = r0
    ;;
    mov r37 = r0
    ;;
    { .mii
        nop.m 0
        nop.i 0
        break.i ASTERIA_IA64_SYSTEM_CALL_BREAK_IMMEDIATE
    }
    ;;
// Save the live Query response before preparing either IPC frame. These
// values are reloaded immediately before the child restores its frame.
    adds r27 = -ASTERIA_IA64_USER_EXECUTION_PROBE_SYSTEM_CALL_RESPONSE_OFFSET_FROM_STACK_POINTER_BYTES, r12
    ;;
    st8 [r27] = r8, 8
    ;;
    st8 [r27] = r9, 8
    ;;
    st8 [r27] = r10
    ;;
    movl r14 = ASTERIA_IA64_USER_EXECUTION_PROBE_IPC_BOOTSTRAP_VIRTUAL_ADDRESS
    ;;
    ld8 r21 = [r14], 8
    ;;
    ld8 r22 = [r14], 8
    ;;
    mov r24 = ASTERIA_IA64_USER_EXECUTION_PROBE_EXPECTED_ENDPOINT_CAPABILITY_SELECTOR
    ;;
    xor r23 = r21, r24
    ;;
    mov r15 = ASTERIA_IA64_USER_EXECUTION_PROBE_IPC_SEND_SYSTEM_CALL_NUMBER
    mov r32 = r21
    mov r33 = r22
    ;;
    mov r34 = r14
    mov r35 = ASTERIA_IA64_USER_EXECUTION_PROBE_IPC_BOOTSTRAP_FRAME_SIZE_BYTES
    mov r36 = r0
    ;;
    mov r37 = r0
    ;;
    { .mii
        nop.m 0
        nop.i 0
        break.i ASTERIA_IA64_SYSTEM_CALL_BREAK_IMMEDIATE
    }
    ;;
    st8 [r16] = r8, 8
    ;;
    st8 [r16] = r9, 8
    ;;
    st8 [r16] = r10, 8
    ;;
    or r24 = r8, r10
    adds r25 = -ASTERIA_IA64_USER_EXECUTION_PROBE_EXPECTED_IPC_INFORMATION, r9
    ;;
    or r24 = r24, r25
    ;;
    or r23 = r23, r24
    ;;
    mov r15 = ASTERIA_IA64_USER_EXECUTION_PROBE_IPC_RECEIVE_SYSTEM_CALL_NUMBER
    mov r32 = r21
    mov r33 = r22
    ;;
    movl r34 = ASTERIA_IA64_USER_EXECUTION_PROBE_IPC_DELIVERY_VIRTUAL_ADDRESS
    ;;
    mov r35 = ASTERIA_IA64_USER_EXECUTION_PROBE_IPC_DELIVERY_CAPACITY_BYTES
    mov r36 = r0
    mov r37 = r0
    ;;
    { .mii
        nop.m 0
        nop.i 0
        break.i ASTERIA_IA64_SYSTEM_CALL_BREAK_IMMEDIATE
    }
    ;;
    st8 [r16] = r8, 8
    ;;
    st8 [r16] = r9, 8
    ;;
    st8 [r16] = r10, 8
    ;;
    adds r24 = -ASTERIA_IA64_USER_EXECUTION_PROBE_EXPECTED_IPC_ERROR, r8
    adds r25 = -ASTERIA_IA64_USER_EXECUTION_PROBE_EXPECTED_IPC_INFORMATION, r9
    adds r26 = -ASTERIA_IA64_USER_EXECUTION_PROBE_EXPECTED_RECEIVE_TRANSFERRED_BYTES, r10
    ;;
    or r24 = r24, r25
    or r26 = r26, r23
    ;;
    or r23 = r24, r26
    ;;
// Wait on the separately staged WaitSet handle. This return is the
// target-level proof that the syscall frame was suspended and restored.
    movl r14 = ASTERIA_IA64_USER_EXECUTION_PROBE_IPC_BOOTSTRAP_VIRTUAL_ADDRESS \
        + ASTERIA_IA64_USER_EXECUTION_PROBE_IPC_BOOTSTRAP_WAIT_SET_SELECTOR_OFFSET_BYTES
    ;;
    ld8 r21 = [r14], 8
    ;;
    ld8 r22 = [r14]
    ;;
    mov r24 = ASTERIA_IA64_USER_EXECUTION_PROBE_EXPECTED_WAIT_SET_CAPABILITY_SELECTOR
    ;;
    xor r24 = r21, r24
    ;;
    or r23 = r23, r24
    ;;
    mov r15 = ASTERIA_IA64_USER_EXECUTION_PROBE_WAIT_SET_WAIT_SYSTEM_CALL_NUMBER
    mov r32 = r21
    mov r33 = r22
    ;;
    mov r34 = r0
    mov r35 = r0
    mov r36 = r0
    ;;
    mov r37 = r0
    ;;
    { .mii
        nop.m 0
        nop.i 0
        break.i ASTERIA_IA64_SYSTEM_CALL_BREAK_IMMEDIATE
    }
    ;;
    movl r14 = ASTERIA_IA64_USER_EXECUTION_PROBE_WAIT_RESPONSE_VIRTUAL_ADDRESS
    ;;
    st8 [r14] = r8, 8
    ;;
    st8 [r14] = r9, 8
    ;;
    st8 [r14] = r10
    ;;
    adds r24 = -ASTERIA_SYSTEM_CALL_ERROR_NONE, r8
    movl r25 = ASTERIA_IA64_USER_EXECUTION_PROBE_EXPECTED_WAIT_TAG
    ;;
    xor r25 = r9, r25
    adds r26 = -ASTERIA_IA64_USER_EXECUTION_PROBE_EXPECTED_WAIT_REGISTRATION_SEQUENCE, r10
    ;;
    or r24 = r24, r25
    or r26 = r26, r23
    ;;
    or r23 = r24, r26
    ;;
    cmp.eq p6, p7 = r23, r0
    ;;
    mf
    ;;
(p6) st8 [r16] = r18
    ;;
    mf
    ;;
    adds r27 = -ASTERIA_IA64_USER_EXECUTION_PROBE_SYSTEM_CALL_RESPONSE_OFFSET_FROM_STACK_POINTER_BYTES, r12
    ;;
    ld8 r8 = [r27], 8
    ;;
    ld8 r9 = [r27], 8
    ;;
    ld8 r10 = [r27]
    ;;
    mov r14 = r28
    mov r15 = r29
    ;;
    mov b0 = r20
    mov ar.pfs = r19
    ;;
    mov r16 = r0
    mov r17 = r0
    mov r19 = r0
    ;;
    mov r20 = r0
    mov r21 = r0
    mov r22 = r0
    ;;
    mov r23 = r0
    mov r24 = r0
    mov r25 = r0
    ;;
    mov r26 = r0
    mov r27 = r0
    mov r28 = r0
    ;;
    mov r29 = r0
    ;;
    br.ret.sptk.many b0
    ;;
.align 16
.size AsteriaIa64UserExecutionProbeSpillChild, . - AsteriaIa64UserExecutionProbeSpillChild
.size AsteriaIa64UserExecutionProbePayloadStart, . - AsteriaIa64UserExecutionProbePayloadStart
.align 16
.global AsteriaIa64GetUserExecutionProbePayloadAddress
.hidden AsteriaIa64GetUserExecutionProbePayloadAddress
.type AsteriaIa64GetUserExecutionProbePayloadAddress, @function
AsteriaIa64GetUserExecutionProbePayloadAddress:
    movl r8 = AsteriaIa64UserExecutionProbePayloadStart
    br.ret.sptk.many b0
    ;;
.align 16
.size AsteriaIa64GetUserExecutionProbePayloadAddress, . - AsteriaIa64GetUserExecutionProbePayloadAddress
.explicit
.text
