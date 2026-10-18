// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Diagnostics/Ia64EvidenceRecord.hpp"

#include "Platform/Synchronization/AtomicUInt64.hpp"
#include "Runtime/Memory.hpp"

namespace Asteria::Platform::Ia64::Diagnostics
{
namespace
{
// Boot-lifetime storage in the loaded image's zero-initialized data. The
// image is identity-mapped before and after translation activation, so the
// object's address is also the physical address the host reads.
alignas(8) constinit Ia64EvidenceRecord g_ia64EvidenceRecord{};
} // namespace

Ia64EvidenceRecord& GetIa64EvidenceRecord() noexcept
{
    return g_ia64EvidenceRecord;
}

Foundation::UInt64 InitializeIa64EvidenceRecord() noexcept
{
    static_cast<void>(memset(&g_ia64EvidenceRecord, 0, sizeof(g_ia64EvidenceRecord)));
    g_ia64EvidenceRecord.Header = Kernel::Diagnostics::EvidenceRecordHeader{
        .Magic = Kernel::Diagnostics::EvidenceRecordMagic,
        .Schema = Ia64EvidenceRecordSchema,
        .SizeBytes = static_cast<Foundation::UInt32>(sizeof(Ia64EvidenceRecord)),
    };
    return static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(&g_ia64EvidenceRecord));
}

void PublishIa64Evidence(Foundation::UInt64& published) noexcept
{
    Synchronization::AtomicUInt64Reference{published}.StoreRelease(1U);
}
} // namespace Asteria::Platform::Ia64::Diagnostics
