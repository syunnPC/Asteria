// SPDX-License-Identifier: Apache-2.0

// Cooperative IA-64 kernel-context entry, address query and switch.
// Offsets into Ia64KernelRegisterState come from the generated C++ layout.

#include "Generated/Platform/Ia64/Ia64KernelContextAsmLayout.h"

.text
.align 16
.global AsteriaIa64StartKernelContext
.hidden AsteriaIa64StartKernelContext
.type AsteriaIa64StartKernelContext, @function
AsteriaIa64StartKernelContext:
    alloc r16 = ar.pfs, 0, 2, 1, 0
    mov r17 = r4
    mov r18 = r5
    mov r34 = r6
    ;;
    mov r1 = r18
    mov b6 = r17
    ;;
    br.call.sptk.many b0 = b6
    ;;
    rsm psr.i
    ;;
    srlz.d
    ;;
.Lasteria_ia64_returned_context_halt:
    hint @pause
    ;;
    br.cond.sptk .Lasteria_ia64_returned_context_halt
    ;;
// Force GAS to emit the pending branch bundle before fixing the ELF extent.
.align 16
.size AsteriaIa64StartKernelContext, . - AsteriaIa64StartKernelContext
.align 16
.global AsteriaIa64GetStartKernelContextAddress
.hidden AsteriaIa64GetStartKernelContextAddress
.type AsteriaIa64GetStartKernelContextAddress, @function
AsteriaIa64GetStartKernelContextAddress:
// The cooperative trampoline does not read through gp before replacing
// it with the prepared entry descriptor's gp. Return its raw bundle
// address without constructing another function descriptor/GOT entry.
    movl r8 = AsteriaIa64StartKernelContext
    ;;
    br.ret.sptk.many b0
    ;;
// Force GAS to emit the pending return bundle before fixing the ELF extent.
.align 16
.size AsteriaIa64GetStartKernelContextAddress, . - AsteriaIa64GetStartKernelContextAddress
.align 16
.global AsteriaIa64SwitchKernelContext
.hidden AsteriaIa64SwitchKernelContext
.type AsteriaIa64SwitchKernelContext, @function
AsteriaIa64SwitchKernelContext:
    mov r14 = r32
    mov r15 = r33
    mov r16 = ar.rsc
    mov r17 = ar.bsp
    mov r18 = ar.pfs
    mov r19 = ar.unat
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_GLOBAL_POINTER, r14
    ;;
    st8 [r20] = r1
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_STACK_POINTER, r14
    ;;
    st8 [r20] = r12
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_THREAD_POINTER, r14
    ;;
    st8 [r20] = r13
    mov r21 = b0
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_INSTRUCTION_POINTER, r14
    ;;
    st8 [r20] = r21
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_PREVIOUS_FUNCTION_STATE, r14
    ;;
    st8 [r20] = r18
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_RSE_CONFIGURATION, r14
    ;;
    st8 [r20] = r16
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_RSE_BACKING_STORE_POINTER, r14
    ;;
    st8 [r20] = r17
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_USER_NAT_COLLECTION, r14
    ;;
    st8 [r20] = r19
    ;;
    mov r16 = ar.lc
    mov r17 = ar.fpsr
    mov r18 = pr
    mov r19 = b1
    mov r21 = b2
    mov r22 = b3
    mov r23 = b4
    mov r24 = b5
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_LOOP_COUNTER, r14
    ;;
    st8 [r20] = r16
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_FLOATING_POINT_STATUS, r14
    ;;
    st8 [r20] = r17
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_PREDICATES, r14
    ;;
    st8 [r20] = r18
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_B1, r14
    ;;
    st8 [r20] = r19
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_B2, r14
    ;;
    st8 [r20] = r21
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_B3, r14
    ;;
    st8 [r20] = r22
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_B4, r14
    ;;
    st8 [r20] = r23
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_B5, r14
    ;;
    st8 [r20] = r24
    ;;
    flushrs
    ;;
    mov ar.rsc = r0
    ;;
    mov r16 = ar.rnat
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_RSE_NAT_COLLECTION, r14
    ;;
    st8 [r20] = r16
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_R4, r14
    ;;
    st8.spill [r20] = r4, 8
    ;;
    st8.spill [r20] = r5, 8
    ;;
    st8.spill [r20] = r6, 8
    ;;
    st8.spill [r20] = r7, 8
    ;;
    mov r16 = ar.unat
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_PRESERVED_GENERAL_NAT_COLLECTION, r14
    ;;
    st8 [r20] = r16
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_FLOATING_REGISTERS, r14
    ;;
    stf.spill [r20] = f2, 16
    ;;
    stf.spill [r20] = f3, 16
    ;;
    stf.spill [r20] = f4, 16
    ;;
    stf.spill [r20] = f5, 16
    ;;
    stf.spill [r20] = f16, 16
    ;;
    stf.spill [r20] = f17, 16
    ;;
    stf.spill [r20] = f18, 16
    ;;
    stf.spill [r20] = f19, 16
    ;;
    stf.spill [r20] = f20, 16
    ;;
    stf.spill [r20] = f21, 16
    ;;
    stf.spill [r20] = f22, 16
    ;;
    stf.spill [r20] = f23, 16
    ;;
    stf.spill [r20] = f24, 16
    ;;
    stf.spill [r20] = f25, 16
    ;;
    stf.spill [r20] = f26, 16
    ;;
    stf.spill [r20] = f27, 16
    ;;
    stf.spill [r20] = f28, 16
    ;;
    stf.spill [r20] = f29, 16
    ;;
    stf.spill [r20] = f30, 16
    ;;
    stf.spill [r20] = f31, 16
    ;;
// Make the complete saved image visible before another owned context can
// publish or migrate it on a weakly ordered IA-64 system.
    mf
    ;;
    invala
    ;;
// Complete the instruction-stream side of the context publication before
// restoring registers that can resume the incoming execution stream.
    sync.i
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_RSE_BACKING_STORE_POINTER, r15
    ;;
    ld8 r16 = [r20]
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_RSE_NAT_COLLECTION, r15
    ;;
    ld8 r17 = [r20]
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_PREVIOUS_FUNCTION_STATE, r15
    ;;
    ld8 r18 = [r20]
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_RSE_CONFIGURATION, r15
    ;;
    ld8 r19 = [r20]
    ;;
    mov ar.bspstore = r16
    ;;
    mov ar.rnat = r17
    mov ar.pfs = r18
    ;;
    mov ar.rsc = r19
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_FLOATING_REGISTERS, r15
    ;;
    ldf.fill f2 = [r20], 16
    ;;
    ldf.fill f3 = [r20], 16
    ;;
    ldf.fill f4 = [r20], 16
    ;;
    ldf.fill f5 = [r20], 16
    ;;
    ldf.fill f16 = [r20], 16
    ;;
    ldf.fill f17 = [r20], 16
    ;;
    ldf.fill f18 = [r20], 16
    ;;
    ldf.fill f19 = [r20], 16
    ;;
    ldf.fill f20 = [r20], 16
    ;;
    ldf.fill f21 = [r20], 16
    ;;
    ldf.fill f22 = [r20], 16
    ;;
    ldf.fill f23 = [r20], 16
    ;;
    ldf.fill f24 = [r20], 16
    ;;
    ldf.fill f25 = [r20], 16
    ;;
    ldf.fill f26 = [r20], 16
    ;;
    ldf.fill f27 = [r20], 16
    ;;
    ldf.fill f28 = [r20], 16
    ;;
    ldf.fill f29 = [r20], 16
    ;;
    ldf.fill f30 = [r20], 16
    ;;
    ldf.fill f31 = [r20], 16
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_PRESERVED_GENERAL_NAT_COLLECTION, r15
    ;;
    ld8 r16 = [r20]
    ;;
    mov ar.unat = r16
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_R4, r15
    ;;
    ld8.fill r4 = [r20], 8
    ;;
    ld8.fill r5 = [r20], 8
    ;;
    ld8.fill r6 = [r20], 8
    ;;
    ld8.fill r7 = [r20], 8
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_USER_NAT_COLLECTION, r15
    ;;
    ld8 r16 = [r20]
    ;;
    mov ar.unat = r16
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_LOOP_COUNTER, r15
    ;;
    ld8 r16 = [r20]
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_FLOATING_POINT_STATUS, r15
    ;;
    ld8 r17 = [r20]
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_PREDICATES, r15
    ;;
    ld8 r18 = [r20]
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_B1, r15
    ;;
    ld8 r19 = [r20]
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_B2, r15
    ;;
    ld8 r21 = [r20]
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_B3, r15
    ;;
    ld8 r22 = [r20]
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_B4, r15
    ;;
    ld8 r23 = [r20]
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_B5, r15
    ;;
    ld8 r24 = [r20]
    ;;
    mov ar.lc = r16
    mov ar.fpsr = r17
    mov pr = r18, -1
    mov b1 = r19
    mov b2 = r21
    mov b3 = r22
    mov b4 = r23
    mov b5 = r24
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_INSTRUCTION_POINTER, r15
    ;;
    ld8 r16 = [r20]
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_GLOBAL_POINTER, r15
    ;;
    ld8 r1 = [r20]
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_STACK_POINTER, r15
    ;;
    ld8 r12 = [r20]
    ;;
    add r20 = ASTERIA_IA64_KERNEL_REGISTER_STATE_THREAD_POINTER, r15
    ;;
    ld8 r13 = [r20]
    ;;
    mov b6 = r16
    ;;
// Br.ret restores CFM from the retained ar.pfs before resuming the saved call frame or entering the
// fresh zero-sized trampoline frame.
    br.ret.sptk.many b6
    ;;
// Force GAS to emit the pending return bundle before fixing the ELF extent.
.align 16
.size AsteriaIa64SwitchKernelContext, . - AsteriaIa64SwitchKernelContext

.align 16
.global AsteriaIa64SaveHighFloatingPartition
.hidden AsteriaIa64SaveHighFloatingPartition
.type AsteriaIa64SaveHighFloatingPartition, @function
// Spills f32-f127 into the 96 16-byte slots at r32.
AsteriaIa64SaveHighFloatingPartition:
    mov r14 = r32
    add r15 = 16, r32
    ;;
    stf.spill [r14] = f32, 32
    stf.spill [r15] = f33, 32
    ;;
    stf.spill [r14] = f34, 32
    stf.spill [r15] = f35, 32
    ;;
    stf.spill [r14] = f36, 32
    stf.spill [r15] = f37, 32
    ;;
    stf.spill [r14] = f38, 32
    stf.spill [r15] = f39, 32
    ;;
    stf.spill [r14] = f40, 32
    stf.spill [r15] = f41, 32
    ;;
    stf.spill [r14] = f42, 32
    stf.spill [r15] = f43, 32
    ;;
    stf.spill [r14] = f44, 32
    stf.spill [r15] = f45, 32
    ;;
    stf.spill [r14] = f46, 32
    stf.spill [r15] = f47, 32
    ;;
    stf.spill [r14] = f48, 32
    stf.spill [r15] = f49, 32
    ;;
    stf.spill [r14] = f50, 32
    stf.spill [r15] = f51, 32
    ;;
    stf.spill [r14] = f52, 32
    stf.spill [r15] = f53, 32
    ;;
    stf.spill [r14] = f54, 32
    stf.spill [r15] = f55, 32
    ;;
    stf.spill [r14] = f56, 32
    stf.spill [r15] = f57, 32
    ;;
    stf.spill [r14] = f58, 32
    stf.spill [r15] = f59, 32
    ;;
    stf.spill [r14] = f60, 32
    stf.spill [r15] = f61, 32
    ;;
    stf.spill [r14] = f62, 32
    stf.spill [r15] = f63, 32
    ;;
    stf.spill [r14] = f64, 32
    stf.spill [r15] = f65, 32
    ;;
    stf.spill [r14] = f66, 32
    stf.spill [r15] = f67, 32
    ;;
    stf.spill [r14] = f68, 32
    stf.spill [r15] = f69, 32
    ;;
    stf.spill [r14] = f70, 32
    stf.spill [r15] = f71, 32
    ;;
    stf.spill [r14] = f72, 32
    stf.spill [r15] = f73, 32
    ;;
    stf.spill [r14] = f74, 32
    stf.spill [r15] = f75, 32
    ;;
    stf.spill [r14] = f76, 32
    stf.spill [r15] = f77, 32
    ;;
    stf.spill [r14] = f78, 32
    stf.spill [r15] = f79, 32
    ;;
    stf.spill [r14] = f80, 32
    stf.spill [r15] = f81, 32
    ;;
    stf.spill [r14] = f82, 32
    stf.spill [r15] = f83, 32
    ;;
    stf.spill [r14] = f84, 32
    stf.spill [r15] = f85, 32
    ;;
    stf.spill [r14] = f86, 32
    stf.spill [r15] = f87, 32
    ;;
    stf.spill [r14] = f88, 32
    stf.spill [r15] = f89, 32
    ;;
    stf.spill [r14] = f90, 32
    stf.spill [r15] = f91, 32
    ;;
    stf.spill [r14] = f92, 32
    stf.spill [r15] = f93, 32
    ;;
    stf.spill [r14] = f94, 32
    stf.spill [r15] = f95, 32
    ;;
    stf.spill [r14] = f96, 32
    stf.spill [r15] = f97, 32
    ;;
    stf.spill [r14] = f98, 32
    stf.spill [r15] = f99, 32
    ;;
    stf.spill [r14] = f100, 32
    stf.spill [r15] = f101, 32
    ;;
    stf.spill [r14] = f102, 32
    stf.spill [r15] = f103, 32
    ;;
    stf.spill [r14] = f104, 32
    stf.spill [r15] = f105, 32
    ;;
    stf.spill [r14] = f106, 32
    stf.spill [r15] = f107, 32
    ;;
    stf.spill [r14] = f108, 32
    stf.spill [r15] = f109, 32
    ;;
    stf.spill [r14] = f110, 32
    stf.spill [r15] = f111, 32
    ;;
    stf.spill [r14] = f112, 32
    stf.spill [r15] = f113, 32
    ;;
    stf.spill [r14] = f114, 32
    stf.spill [r15] = f115, 32
    ;;
    stf.spill [r14] = f116, 32
    stf.spill [r15] = f117, 32
    ;;
    stf.spill [r14] = f118, 32
    stf.spill [r15] = f119, 32
    ;;
    stf.spill [r14] = f120, 32
    stf.spill [r15] = f121, 32
    ;;
    stf.spill [r14] = f122, 32
    stf.spill [r15] = f123, 32
    ;;
    stf.spill [r14] = f124, 32
    stf.spill [r15] = f125, 32
    ;;
    stf.spill [r14] = f126, 32
    stf.spill [r15] = f127, 32
    ;;
    br.ret.sptk.many b0
    ;;
// Force GAS to emit the pending return bundle before fixing the ELF extent.
.align 16
.size AsteriaIa64SaveHighFloatingPartition, . - AsteriaIa64SaveHighFloatingPartition

.align 16
.global AsteriaIa64RestoreHighFloatingPartition
.hidden AsteriaIa64RestoreHighFloatingPartition
.type AsteriaIa64RestoreHighFloatingPartition, @function
// Fills f32-f127 from the 96 16-byte slots at r32.
AsteriaIa64RestoreHighFloatingPartition:
    mov r14 = r32
    add r15 = 16, r32
    ;;
    ldf.fill f32 = [r14], 32
    ldf.fill f33 = [r15], 32
    ;;
    ldf.fill f34 = [r14], 32
    ldf.fill f35 = [r15], 32
    ;;
    ldf.fill f36 = [r14], 32
    ldf.fill f37 = [r15], 32
    ;;
    ldf.fill f38 = [r14], 32
    ldf.fill f39 = [r15], 32
    ;;
    ldf.fill f40 = [r14], 32
    ldf.fill f41 = [r15], 32
    ;;
    ldf.fill f42 = [r14], 32
    ldf.fill f43 = [r15], 32
    ;;
    ldf.fill f44 = [r14], 32
    ldf.fill f45 = [r15], 32
    ;;
    ldf.fill f46 = [r14], 32
    ldf.fill f47 = [r15], 32
    ;;
    ldf.fill f48 = [r14], 32
    ldf.fill f49 = [r15], 32
    ;;
    ldf.fill f50 = [r14], 32
    ldf.fill f51 = [r15], 32
    ;;
    ldf.fill f52 = [r14], 32
    ldf.fill f53 = [r15], 32
    ;;
    ldf.fill f54 = [r14], 32
    ldf.fill f55 = [r15], 32
    ;;
    ldf.fill f56 = [r14], 32
    ldf.fill f57 = [r15], 32
    ;;
    ldf.fill f58 = [r14], 32
    ldf.fill f59 = [r15], 32
    ;;
    ldf.fill f60 = [r14], 32
    ldf.fill f61 = [r15], 32
    ;;
    ldf.fill f62 = [r14], 32
    ldf.fill f63 = [r15], 32
    ;;
    ldf.fill f64 = [r14], 32
    ldf.fill f65 = [r15], 32
    ;;
    ldf.fill f66 = [r14], 32
    ldf.fill f67 = [r15], 32
    ;;
    ldf.fill f68 = [r14], 32
    ldf.fill f69 = [r15], 32
    ;;
    ldf.fill f70 = [r14], 32
    ldf.fill f71 = [r15], 32
    ;;
    ldf.fill f72 = [r14], 32
    ldf.fill f73 = [r15], 32
    ;;
    ldf.fill f74 = [r14], 32
    ldf.fill f75 = [r15], 32
    ;;
    ldf.fill f76 = [r14], 32
    ldf.fill f77 = [r15], 32
    ;;
    ldf.fill f78 = [r14], 32
    ldf.fill f79 = [r15], 32
    ;;
    ldf.fill f80 = [r14], 32
    ldf.fill f81 = [r15], 32
    ;;
    ldf.fill f82 = [r14], 32
    ldf.fill f83 = [r15], 32
    ;;
    ldf.fill f84 = [r14], 32
    ldf.fill f85 = [r15], 32
    ;;
    ldf.fill f86 = [r14], 32
    ldf.fill f87 = [r15], 32
    ;;
    ldf.fill f88 = [r14], 32
    ldf.fill f89 = [r15], 32
    ;;
    ldf.fill f90 = [r14], 32
    ldf.fill f91 = [r15], 32
    ;;
    ldf.fill f92 = [r14], 32
    ldf.fill f93 = [r15], 32
    ;;
    ldf.fill f94 = [r14], 32
    ldf.fill f95 = [r15], 32
    ;;
    ldf.fill f96 = [r14], 32
    ldf.fill f97 = [r15], 32
    ;;
    ldf.fill f98 = [r14], 32
    ldf.fill f99 = [r15], 32
    ;;
    ldf.fill f100 = [r14], 32
    ldf.fill f101 = [r15], 32
    ;;
    ldf.fill f102 = [r14], 32
    ldf.fill f103 = [r15], 32
    ;;
    ldf.fill f104 = [r14], 32
    ldf.fill f105 = [r15], 32
    ;;
    ldf.fill f106 = [r14], 32
    ldf.fill f107 = [r15], 32
    ;;
    ldf.fill f108 = [r14], 32
    ldf.fill f109 = [r15], 32
    ;;
    ldf.fill f110 = [r14], 32
    ldf.fill f111 = [r15], 32
    ;;
    ldf.fill f112 = [r14], 32
    ldf.fill f113 = [r15], 32
    ;;
    ldf.fill f114 = [r14], 32
    ldf.fill f115 = [r15], 32
    ;;
    ldf.fill f116 = [r14], 32
    ldf.fill f117 = [r15], 32
    ;;
    ldf.fill f118 = [r14], 32
    ldf.fill f119 = [r15], 32
    ;;
    ldf.fill f120 = [r14], 32
    ldf.fill f121 = [r15], 32
    ;;
    ldf.fill f122 = [r14], 32
    ldf.fill f123 = [r15], 32
    ;;
    ldf.fill f124 = [r14], 32
    ldf.fill f125 = [r15], 32
    ;;
    ldf.fill f126 = [r14], 32
    ldf.fill f127 = [r15], 32
    ;;
    br.ret.sptk.many b0
    ;;
// Force GAS to emit the pending return bundle before fixing the ELF extent.
.align 16
.size AsteriaIa64RestoreHighFloatingPartition, . - AsteriaIa64RestoreHighFloatingPartition
