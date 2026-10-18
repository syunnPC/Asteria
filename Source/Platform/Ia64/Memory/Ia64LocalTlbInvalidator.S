// SPDX-License-Identifier: Apache-2.0

// IA-64 local translation purge (ptc.l) and its instruction serialization
// for Ia64LocalTlbInvalidator.

.text
.align 16
.global AsteriaIa64PurgeLocalTranslation
.hidden AsteriaIa64PurgeLocalTranslation
.type AsteriaIa64PurgeLocalTranslation, @function
AsteriaIa64PurgeLocalTranslation:
    ptc.l r32, r33
    ;;
    br.ret.sptk.many b0
    ;;
// Flush the complete bundle before recording the checked preemption corridor's extent.
.align 16
.size AsteriaIa64PurgeLocalTranslation, . - AsteriaIa64PurgeLocalTranslation
.align 16
.global AsteriaIa64SerializeLocalTranslationPurge
.hidden AsteriaIa64SerializeLocalTranslationPurge
.type AsteriaIa64SerializeLocalTranslationPurge, @function
AsteriaIa64SerializeLocalTranslationPurge:
    srlz.i
    ;;
    br.ret.sptk.many b0
    ;;
// Flush the complete bundle before recording the checked preemption corridor's extent.
.align 16
.size AsteriaIa64SerializeLocalTranslationPurge, . - AsteriaIa64SerializeLocalTranslationPurge
