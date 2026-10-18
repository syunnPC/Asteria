// SPDX-License-Identifier: Apache-2.0

// IA-64 leaves of the initial translation install: PSR admission and bank
// selection, local purges, region-register access, translation-register
// inserts, serialization and the fail-stop.

.text
.align 16
.global AsteriaIa64BeginInitialTranslationInstall
.hidden AsteriaIa64BeginInitialTranslationInstall
.type AsteriaIa64BeginInitialTranslationInstall, @function
AsteriaIa64BeginInitialTranslationInstall:
    mov r8 = psr
    ;;
// Reject the architecturally visible incompatible state. CPL0 and the
// physical SAL environment remain caller preconditions because ordinary
// mov-from-PSR does not expose every inherited PSR field.
    movl r14 = 0x0000401B0F064002
    ;;
    and r14 = r8, r14
    ;;
    cmp.ne p6, p7 = r14, r0
    ;;
(p6) br.cond.dpnt.few 1f
    ;;
// Select the SAL handoff register bank explicitly. PSR.bn is not part of
// the architecturally guaranteed mov-from-PSR readback subset, so it must
// not be used as a validation input.
    bsw.1
    ;;
    rsm psr.ic
    ;;
    srlz.d
    ;;
    srlz.i
    ;;
    mov r8 = r0
    br.ret.sptk.many b0
    ;;
1:
    mov r8 = 1
    br.ret.sptk.many b0
    ;;
.align 16
.size AsteriaIa64BeginInitialTranslationInstall, . - AsteriaIa64BeginInitialTranslationInstall
.align 16
.global AsteriaIa64PurgeLocalTranslationCache
.hidden AsteriaIa64PurgeLocalTranslationCache
.type AsteriaIa64PurgeLocalTranslationCache, @function
AsteriaIa64PurgeLocalTranslationCache:
    ptc.e r32
    ;;
    br.ret.sptk.many b0
    ;;
.align 16
.size AsteriaIa64PurgeLocalTranslationCache, . - AsteriaIa64PurgeLocalTranslationCache
.align 16
.global AsteriaIa64SerializeDataAndInstruction
.hidden AsteriaIa64SerializeDataAndInstruction
.type AsteriaIa64SerializeDataAndInstruction, @function
AsteriaIa64SerializeDataAndInstruction:
    srlz.d
    ;;
    srlz.i
    ;;
    br.ret.sptk.many b0
    ;;
.align 16
.size AsteriaIa64SerializeDataAndInstruction, . - AsteriaIa64SerializeDataAndInstruction
.align 16
.global AsteriaIa64PurgeInstructionTranslation
.hidden AsteriaIa64PurgeInstructionTranslation
.type AsteriaIa64PurgeInstructionTranslation, @function
AsteriaIa64PurgeInstructionTranslation:
    ptr.i r32, r33
    ;;
    br.ret.sptk.many b0
    ;;
.align 16
.size AsteriaIa64PurgeInstructionTranslation, . - AsteriaIa64PurgeInstructionTranslation
.align 16
.global AsteriaIa64SerializeInstruction
.hidden AsteriaIa64SerializeInstruction
.type AsteriaIa64SerializeInstruction, @function
AsteriaIa64SerializeInstruction:
    srlz.i
    ;;
    br.ret.sptk.many b0
    ;;
.align 16
.size AsteriaIa64SerializeInstruction, . - AsteriaIa64SerializeInstruction
.align 16
.global AsteriaIa64WriteRegionRegister
.hidden AsteriaIa64WriteRegionRegister
.type AsteriaIa64WriteRegionRegister, @function
AsteriaIa64WriteRegionRegister:
    mov rr[r32] = r33
    ;;
    br.ret.sptk.many b0
    ;;
.align 16
.size AsteriaIa64WriteRegionRegister, . - AsteriaIa64WriteRegionRegister
.align 16
.global AsteriaIa64ReadRegionRegister
.hidden AsteriaIa64ReadRegionRegister
.type AsteriaIa64ReadRegionRegister, @function
AsteriaIa64ReadRegionRegister:
    mov r8 = rr[r32]
    ;;
    br.ret.sptk.many b0
    ;;
.align 16
.size AsteriaIa64ReadRegionRegister, . - AsteriaIa64ReadRegionRegister
.align 16
.global AsteriaIa64FailStopInitialTranslationInstall
.hidden AsteriaIa64FailStopInitialTranslationInstall
.type AsteriaIa64FailStopInitialTranslationInstall, @function
AsteriaIa64FailStopInitialTranslationInstall:
1:
    hint @pause
    ;;
    br.cond.sptk 1b
    ;;
.align 16
.size AsteriaIa64FailStopInitialTranslationInstall, . - AsteriaIa64FailStopInitialTranslationInstall
.align 16
.global AsteriaIa64InsertDataTranslation
.hidden AsteriaIa64InsertDataTranslation
.type AsteriaIa64InsertDataTranslation, @function
AsteriaIa64InsertDataTranslation:
    mov cr.ifa = r34
    mov cr.itir = r35
    ;;
    itr.d dtr[r32] = r33
    ;;
    br.ret.sptk.many b0
    ;;
.align 16
.size AsteriaIa64InsertDataTranslation, . - AsteriaIa64InsertDataTranslation
.align 16
.global AsteriaIa64InsertInstructionTranslation
.hidden AsteriaIa64InsertInstructionTranslation
.type AsteriaIa64InsertInstructionTranslation, @function
AsteriaIa64InsertInstructionTranslation:
    mov cr.ifa = r34
    mov cr.itir = r35
    ;;
    itr.i itr[r32] = r33
    ;;
    br.ret.sptk.many b0
    ;;
.align 16
.size AsteriaIa64InsertInstructionTranslation, . - AsteriaIa64InsertInstructionTranslation
.align 16
.global AsteriaIa64EndInitialTranslationInstall
.hidden AsteriaIa64EndInitialTranslationInstall
.type AsteriaIa64EndInitialTranslationInstall, @function
AsteriaIa64EndInitialTranslationInstall:
    srlz.d
    ;;
    srlz.i
    ;;
    br.ret.sptk.many b0
    ;;
.align 16
.size AsteriaIa64EndInitialTranslationInstall, . - AsteriaIa64EndInitialTranslationInstall
