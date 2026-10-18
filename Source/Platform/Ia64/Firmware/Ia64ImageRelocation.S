// SPDX-License-Identifier: Apache-2.0

// Publish every copied bundle before entering the relocated descriptor.
.text
.align 16
.global AsteriaIa64MakeImageInstructionCoherent
.hidden AsteriaIa64MakeImageInstructionCoherent
.type AsteriaIa64MakeImageInstructionCoherent, @function
.proc AsteriaIa64MakeImageInstructionCoherent
AsteriaIa64MakeImageInstructionCoherent:
    mov r14 = r32
    add r15 = r32, r33
    ;;
1:
    fc.i r14
    add r14 = 16, r14
    ;;
    cmp.ltu p6, p7 = r14, r15
    ;;
(p6) br.cond.sptk 1b
    ;;
    sync.i
    ;;
    srlz.i
    ;;
    br.ret.sptk.many b0
    ;;
.endp AsteriaIa64MakeImageInstructionCoherent
.size AsteriaIa64MakeImageInstructionCoherent, . - AsteriaIa64MakeImageInstructionCoherent

// IMM64 operands are themselves rebased; no GOT cell for an absolute symbol.
.align 16
.global _ZN7Asteria8Platform4Ia648Firmware25GetIa64ExecutingImageBaseEv
.hidden _ZN7Asteria8Platform4Ia648Firmware25GetIa64ExecutingImageBaseEv
.type _ZN7Asteria8Platform4Ia648Firmware25GetIa64ExecutingImageBaseEv, @function
_ZN7Asteria8Platform4Ia648Firmware25GetIa64ExecutingImageBaseEv:
    movl r8 = AsteriaImageBase
    ;;
    br.ret.sptk.many b0
    ;;
.align 16
.size _ZN7Asteria8Platform4Ia648Firmware25GetIa64ExecutingImageBaseEv, . - _ZN7Asteria8Platform4Ia648Firmware25GetIa64ExecutingImageBaseEv
.align 16
.global _ZN7Asteria8Platform4Ia648Firmware24GetIa64ExecutingImageEndEv
.hidden _ZN7Asteria8Platform4Ia648Firmware24GetIa64ExecutingImageEndEv
.type _ZN7Asteria8Platform4Ia648Firmware24GetIa64ExecutingImageEndEv, @function
_ZN7Asteria8Platform4Ia648Firmware24GetIa64ExecutingImageEndEv:
    movl r8 = AsteriaIa64LoadedImageEnd
    ;;
    br.ret.sptk.many b0
    ;;
.align 16
.size _ZN7Asteria8Platform4Ia648Firmware24GetIa64ExecutingImageEndEv, . - _ZN7Asteria8Platform4Ia648Firmware24GetIa64ExecutingImageEndEv

.align 16
.global _ZN7Asteria8Platform4Ia648Firmware31GetIa64RelocatedEntryDescriptorEv
.hidden _ZN7Asteria8Platform4Ia648Firmware31GetIa64RelocatedEntryDescriptorEv
.type _ZN7Asteria8Platform4Ia648Firmware31GetIa64RelocatedEntryDescriptorEv, @function
_ZN7Asteria8Platform4Ia648Firmware31GetIa64RelocatedEntryDescriptorEv:
    movl r8 = AsteriaIa64RelocatedEntryDescriptor
    ;;
    br.ret.sptk.many b0
    ;;
.align 16
.size _ZN7Asteria8Platform4Ia648Firmware31GetIa64RelocatedEntryDescriptorEv, . - _ZN7Asteria8Platform4Ia648Firmware31GetIa64RelocatedEntryDescriptorEv

// Keep the firmware entry independent of compiler descriptor emission order.
.section .asteria_efi_entry, "a", @progbits
.align 16
.quad EfiMain
.quad __gp
.global AsteriaIa64RelocatedEntryDescriptor
.hidden AsteriaIa64RelocatedEntryDescriptor
AsteriaIa64RelocatedEntryDescriptor:
.quad EfiMainAligned
.quad __gp
