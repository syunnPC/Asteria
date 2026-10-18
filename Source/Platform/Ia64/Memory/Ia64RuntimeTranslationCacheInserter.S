// SPDX-License-Identifier: Apache-2.0

// IA-64 runtime data and instruction translation-cache insertion (itc.d and
// itc.i) for Ia64RuntimeTranslationCacheInserter.

.text
.align 16
.global AsteriaIa64InsertRuntimeDataTranslationCache
.hidden AsteriaIa64InsertRuntimeDataTranslationCache
.type AsteriaIa64InsertRuntimeDataTranslationCache, @function
AsteriaIa64InsertRuntimeDataTranslationCache:
    mov cr.ifa = r33
    mov cr.itir = r34
    ;;
    itc.d r32
    ;;
    srlz.d
    ;;
    br.ret.sptk.many b0
    ;;
.align 16
.size AsteriaIa64InsertRuntimeDataTranslationCache, . - AsteriaIa64InsertRuntimeDataTranslationCache
.align 16
.global AsteriaIa64InsertRuntimeInstructionTranslationCache
.hidden AsteriaIa64InsertRuntimeInstructionTranslationCache
.type AsteriaIa64InsertRuntimeInstructionTranslationCache, @function
AsteriaIa64InsertRuntimeInstructionTranslationCache:
    mov cr.ifa = r33
    mov cr.itir = r34
    ;;
    itc.i r32
    ;;
    srlz.i
    ;;
    br.ret.sptk.many b0
    ;;
.align 16
.size AsteriaIa64InsertRuntimeInstructionTranslationCache, . - AsteriaIa64InsertRuntimeInstructionTranslationCache
