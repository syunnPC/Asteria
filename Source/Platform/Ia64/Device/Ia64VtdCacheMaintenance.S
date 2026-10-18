// SPDX-License-Identifier: Apache-2.0

// VT-d table publication on IA-64. A remapping unit that reports ECAP.C = 0 reads its root, context
// and page tables from memory without snooping the processor caches, so every table line the kernel
// changed is written back before the register write (or, for a new mapping, the return) that makes
// the line reachable. The shared VtdUnit calls these leaves through Ia64VtdCacheMaintenance. Intel
// IA-64 Architecture Software Developer's Manual, Volume 3, revision 1.1 (245319-002): - fc: "The
// line size affected is at least 32-bytes (aligned on a 32-byte boundary)."... "fc is an unordered
// operation, and is not affected by a memory fence (mf) instruction. It is ordered with respect to
// the sync.i instruction." - sync.i: "A sync.i and a previous fc must be in separate instruction
// groups. If semantically required, the programmer must explicitly insert ordered data references
// (acquire, release or fence type) to appropriately constrain sync.i (and hence fc) visibility to
// the data stream on other processors."... "An instruction serialize operation must be used to
// ensure synchronization initiated by sync.i on the local processor has been observed by a given
// point in program execution." Hence fc on every 32-byte line, then; sync.i; srlz.i; mf. The
// sequence must not be shortened: mf alone does not order fc, and the unit's next register access
// is an uncached access the mf orders after the flushes. Both leaves run with data translation on:
// the tables are in the identity- mapped image, so fc translates through the existing image
// translation.

#include "Generated/Platform/Ia64/Ia64VtdCacheMaintenanceAsmLayout.h"

// Void AsteriaIa64VtdFlushTables(const volatile void* start, UInt64 bytes) in0 = start, in1 = bytes
// (nonzero; start + bytes does not wrap).
.text
.align 32
.global AsteriaIa64VtdFlushTables
.hidden AsteriaIa64VtdFlushTables
.type AsteriaIa64VtdFlushTables, @function
AsteriaIa64VtdFlushTables:
    add r16 = r32, r33
    dep r17 = 0, r32, 0, ASTERIA_IA64_VTD_FLUSH_LINE_SHIFT
    ;;
.LAsteriaIa64VtdFlushLine:
    cmp.ltu p6, p7 = r17, r16
    ;;
(p6) fc r17
(p6) adds r17 = ASTERIA_IA64_VTD_FLUSH_LINE_BYTES, r17
(p6) br.cond.dptk.few .LAsteriaIa64VtdFlushLine
    ;;
    sync.i
    ;;
    srlz.i
    ;;
    mf
    ;;
    br.ret.sptk.many b0
    ;;
.align 16
.size AsteriaIa64VtdFlushTables, . - AsteriaIa64VtdFlushTables

// Void AsteriaIa64VtdFence: orders every earlier store and write-back before every later memory and
// register access.
.text
.align 32
.global AsteriaIa64VtdFence
.hidden AsteriaIa64VtdFence
.type AsteriaIa64VtdFence, @function
AsteriaIa64VtdFence:
    mf
    ;;
    br.ret.sptk.many b0
    ;;
.align 16
.size AsteriaIa64VtdFence, . - AsteriaIa64VtdFence
