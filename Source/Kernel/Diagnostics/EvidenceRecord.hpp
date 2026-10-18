// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_DIAGNOSTICS_EVIDENCE_RECORD_HPP_INCLUDED
#define ASTERIA_KERNEL_DIAGNOSTICS_EVIDENCE_RECORD_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Kernel::Diagnostics
{
/// Common header of each ISA's evidence record. Where the boot path runs a scripted diagnostic
/// scenario (a device read, an application-processor timer, a TLB shootdown, a preemption proof),
/// the kernel stores what it observed in one plain, fixed-layout record instead of comparing the
/// outcome with values the scenario chose. The kernel states facts; a host observer reads the
/// record (the VM runner, through QMP) and judges them. Genuine invariant or API failures still
/// fail-stop with their diagnostics. Every ISA record starts with this header. The kernel
/// zero-fills the record, writes the header, and then reports the record's address once on the
/// diagnostic channel as `ASTERIA:EVIDENCE:0x<16 upper-case hex digits>`. The kernel image is
/// identity-mapped, so that address is also the record's physical address. Each sub-record ends in
/// writes of plain values followed by a release store of its publication word; a reader that sees
/// the word non-zero sees the complete sub-record. - Magic: EvidenceRecordMagic ("ASTEVIDR" in
/// memory order). - Schema: the version of the ISA's record layout; it changes with every layout
/// change, so a reader never decodes a record it does not know. - SizeBytes: sizeof the complete
/// ISA record.
struct EvidenceRecordHeader
{
    Foundation::UInt64 Magic;
    Foundation::UInt32 Schema;
    Foundation::UInt32 SizeBytes;
};

inline constexpr Foundation::UInt64 EvidenceRecordMagic{0x5244'4956'4554'5341ULL};

static_assert(sizeof(EvidenceRecordHeader) == 16U);
static_assert(alignof(EvidenceRecordHeader) == 8U);
static_assert(__is_standard_layout(EvidenceRecordHeader));
static_assert(__is_trivially_copyable(EvidenceRecordHeader));
static_assert(__builtin_offsetof(EvidenceRecordHeader, Magic) == 0U);
static_assert(__builtin_offsetof(EvidenceRecordHeader, Schema) == 8U);
static_assert(__builtin_offsetof(EvidenceRecordHeader, SizeBytes) == 12U);
} // Namespace Asteria::Kernel::Diagnostics

#endif // ASTERIA_KERNEL_DIAGNOSTICS_EVIDENCE_RECORD_HPP_INCLUDED
