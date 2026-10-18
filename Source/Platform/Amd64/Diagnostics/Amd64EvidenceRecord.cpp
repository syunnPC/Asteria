// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Diagnostics/Amd64EvidenceRecord.hpp"

#include "Platform/Synchronization/AtomicUInt64.hpp"
#include "Runtime/Memory.hpp"

namespace Asteria::Platform::Amd64::Diagnostics
{
namespace
{
// Boot-lifetime storage in the loaded image's zero-initialized data. Firmware
// and both runtime roots map the image identically, so the object's address
// is also the physical address the host reads.
alignas(8) constinit Amd64EvidenceRecord g_amd64EvidenceRecord{};
} // namespace

Amd64EvidenceRecord& GetAmd64EvidenceRecord() noexcept
{
    return g_amd64EvidenceRecord;
}

Foundation::UInt64 InitializeAmd64EvidenceRecord() noexcept
{
    static_cast<void>(memset(&g_amd64EvidenceRecord, 0, sizeof(g_amd64EvidenceRecord)));
    g_amd64EvidenceRecord.Header = Kernel::Diagnostics::EvidenceRecordHeader{
        .Magic = Kernel::Diagnostics::EvidenceRecordMagic,
        .Schema = Amd64EvidenceRecordSchema,
        .SizeBytes = static_cast<Foundation::UInt32>(sizeof(Amd64EvidenceRecord)),
    };
    return static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(&g_amd64EvidenceRecord));
}

void PublishAmd64Evidence(Foundation::UInt64& published) noexcept
{
    Synchronization::AtomicUInt64Reference{published}.StoreRelease(1U);
}
} // namespace Asteria::Platform::Amd64::Diagnostics
