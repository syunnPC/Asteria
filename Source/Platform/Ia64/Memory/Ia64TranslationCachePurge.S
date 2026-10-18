// SPDX-License-Identifier: Apache-2.0

// IA-64 local translation-cache purge (ptc.e) and its serialization for
// Ia64LocalTranslationCachePurge.

.text
.align 16
.global AsteriaIa64PurgeTranslationCacheEntry
.hidden AsteriaIa64PurgeTranslationCacheEntry
.type AsteriaIa64PurgeTranslationCacheEntry, @function
AsteriaIa64PurgeTranslationCacheEntry:
    ptc.e r32
    ;;
    br.ret.sptk.many b0
    ;;
// The bundle is emitted at the alignment, so the size follows it.
.align 16
.size AsteriaIa64PurgeTranslationCacheEntry, . - AsteriaIa64PurgeTranslationCacheEntry
.global AsteriaIa64SerializeTranslationCachePurge
.hidden AsteriaIa64SerializeTranslationCachePurge
.type AsteriaIa64SerializeTranslationCachePurge, @function
AsteriaIa64SerializeTranslationCachePurge:
    srlz.d
    ;;
    srlz.i
    ;;
    br.ret.sptk.many b0
    ;;
.align 16
.size AsteriaIa64SerializeTranslationCachePurge, . - AsteriaIa64SerializeTranslationCachePurge
