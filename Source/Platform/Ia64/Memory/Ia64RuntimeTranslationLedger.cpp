// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Memory/Ia64RuntimeTranslationLedger.hpp"

namespace Asteria::Platform::Ia64::Memory
{
namespace
{
using Kernel::Memory::AddressSpaceMappingRecord;
using Kernel::Memory::FrameKind;
using Kernel::Memory::MemoryCachePolicy;
using Kernel::Memory::PageCount;
using Kernel::Memory::PagePermission;
using Kernel::Memory::PagePermissions;
using Kernel::Memory::PhysicalAddress;
using Kernel::Memory::VirtualAddress;

constexpr Foundation::UInt64 PageSizeBytes{Foundation::UInt64{1U} << Ia64RuntimeTranslationPageShift};
constexpr Foundation::UInt64 PageOffsetMask{PageSizeBytes - 1U};
constexpr Foundation::UInt64 PhysicalPageNumberMask{0x0003'FFFF'FFFF'F000ULL};
constexpr Foundation::UInt64 RuntimeTranslationInsertion{Foundation::UInt64{Ia64RuntimeTranslationPageShift} << 2U};
constexpr Foundation::UInt64 CapabilityValidationVirtualStart{0x2000'0000'0000'0000ULL};

[[nodiscard]] constexpr Ia64RuntimeTranslationLedgerFailure Succeed() noexcept
{
    return Ia64RuntimeTranslationLedgerFailure{
        .Code = Ia64RuntimeTranslationLedgerError::None,
        .Reserved = {},
        .EncodingError = Ia64RuntimeTranslationEncodingError::None,
    };
}

[[nodiscard]] constexpr Ia64RuntimeTranslationLedgerFailure Fail(const Ia64RuntimeTranslationLedgerError code) noexcept
{
    return Ia64RuntimeTranslationLedgerFailure{
        .Code = code,
        .Reserved = {},
        .EncodingError = Ia64RuntimeTranslationEncodingError::None,
    };
}

[[nodiscard]] constexpr Ia64RuntimeTranslationLedgerFailure
EncodingFail(const Ia64RuntimeTranslationEncodingError error) noexcept
{
    return Ia64RuntimeTranslationLedgerFailure{
        .Code = Ia64RuntimeTranslationLedgerError::EncodingRejected,
        .Reserved = {},
        .EncodingError = error,
    };
}

// Acquire the valid state before either payload word.
[[nodiscard]] Foundation::UInt64 LoadPageCount(const Ia64RuntimeTranslationLedgerWireRecord& record) noexcept
{
    return __atomic_load_n(&record.PageCount, __ATOMIC_ACQUIRE);
}

[[nodiscard]] constexpr MemoryCachePolicy
SelectCapabilityValidationPolicy(const TranslationCapabilities& capabilities) noexcept
{
    if ((capabilities.MemoryAttributeMask & (Foundation::UInt8{1U} << 0U)) != 0U)
    {
        return MemoryCachePolicy::WriteBack;
    }
    if ((capabilities.MemoryAttributeMask & (Foundation::UInt8{1U} << 4U)) != 0U)
    {
        return MemoryCachePolicy::Uncached;
    }
    if ((capabilities.MemoryAttributeMask & (Foundation::UInt8{1U} << 6U)) != 0U)
    {
        return MemoryCachePolicy::WriteCombining;
    }
    return MemoryCachePolicy::WriteBack;
}

[[nodiscard]] constexpr AddressSpaceMappingRecord
CapabilityValidationMapping(const TranslationCapabilities& capabilities) noexcept
{
    return AddressSpaceMappingRecord{
        .MemoryObject = {},
        .DeviceRegion = {},
        .Frame = {},
        .VirtualStart = VirtualAddress{CapabilityValidationVirtualStart},
        .PhysicalStart = PhysicalAddress{0U},
        .Pages = PageCount{1U},
        .MemoryObjectOffsetPages = 0U,
        .Permissions = PagePermissions{PagePermission::Read},
        .CachePolicy = SelectCapabilityValidationPolicy(capabilities),
        .Kind = FrameKind::Ram,
        .Commitment = {},
        .RuntimeMapping = true,
        .Reserved = {},
    };
}
} // Namespace

Ia64RuntimeTranslationLedgerFailure
Ia64RuntimeTranslationLedger::Initialize(const TranslationCapabilities& capabilities) noexcept
{
    if (m_wire.PublicationWord.LoadAcquire() != 0U)
    {
        return Fail(Ia64RuntimeTranslationLedgerError::AlreadyInitialized);
    }

    const auto validation =
        Ia64RuntimeTranslationEncoder::EncodePage(CapabilityValidationMapping(capabilities), 0U, capabilities);
    if (!validation.HasValue())
    {
        return EncodingFail(validation.GetError());
    }

    m_wire.Capabilities = capabilities;
    m_wire.PublicationWord.StoreRelease(1U);
    return Succeed();
}

Ia64RuntimeTranslationLedgerFailure
Ia64RuntimeTranslationLedger::Append(const AddressSpaceMappingRecord& mapping) noexcept
{
    const Foundation::UInt64 publicationWord = m_wire.PublicationWord.LoadAcquire();
    if (publicationWord == 0U)
    {
        return Fail(Ia64RuntimeTranslationLedgerError::NotInitialized);
    }
    const Foundation::UInt64 publishedRecordCount = publicationWord - 1U;

    const auto baseEncoding = Ia64RuntimeTranslationEncoder::EncodePage(mapping, 0U, m_wire.Capabilities);
    if (!baseEncoding.HasValue())
    {
        return EncodingFail(baseEncoding.GetError());
    }

    const Foundation::UInt64 virtualStart = mapping.VirtualStart.GetValue();
    const Foundation::UInt64 pageCount = mapping.Pages.GetValue();
    const Foundation::UInt64 newLastPage = virtualStart + ((pageCount - 1U) << Ia64RuntimeTranslationPageShift);
    Foundation::UInt64 freeSlot{publishedRecordCount};
    for (Foundation::UInt64 index{0U}; index < publishedRecordCount; ++index)
    {
        const Ia64RuntimeTranslationLedgerWireRecord& existing = m_wire.Records[index];
        const Foundation::UInt64 existingPageCount = LoadPageCount(existing);
        if (existingPageCount == 0U)
        {
            if (freeSlot == publishedRecordCount)
            {
                freeSlot = index;
            }
            continue;
        }
        const Foundation::UInt64 existingLastPage =
            existing.VirtualStart + ((existingPageCount - 1U) << Ia64RuntimeTranslationPageShift);
        if ((virtualStart <= existingLastPage) && (existing.VirtualStart <= newLastPage))
        {
            return Fail(Ia64RuntimeTranslationLedgerError::OverlappingVirtualRange);
        }
    }

    if (freeSlot >= Kernel::Memory::MaximumRuntimeAddressSpaceMappings)
    {
        return Fail(Ia64RuntimeTranslationLedgerError::CapacityExhausted);
    }
    // AddressSpace refuses Map while an unmap is pending. Thus every resident reader has
    // acknowledged before reuse. Intel IA-64 SDM Vol. 1 rev. 1.1 section 4.4.7 orders these words
    // before the count, including within an already published prefix.
    Ia64RuntimeTranslationLedgerWireRecord& record = m_wire.Records[freeSlot];
    record.VirtualStart = virtualStart;
    record.BaseTranslationWord = baseEncoding.GetValue().TranslationWord;
    __atomic_store_n(&record.PageCount, pageCount, __ATOMIC_RELEASE);
    if (freeSlot == publishedRecordCount)
    {
        m_wire.PublicationWord.StoreRelease(publicationWord + 1U);
    }
    return Succeed();
}

Ia64RuntimeTranslationLedgerFailure
Ia64RuntimeTranslationLedger::Remove(const AddressSpaceMappingRecord& mapping) noexcept
{
    const Foundation::UInt64 publicationWord = m_wire.PublicationWord.LoadAcquire();
    if (publicationWord == 0U)
    {
        return Fail(Ia64RuntimeTranslationLedgerError::NotInitialized);
    }
    const auto baseEncoding = Ia64RuntimeTranslationEncoder::EncodePage(mapping, 0U, m_wire.Capabilities);
    if (!baseEncoding.HasValue())
    {
        return EncodingFail(baseEncoding.GetError());
    }

    const Foundation::UInt64 publishedRecordCount = publicationWord - 1U;
    const Foundation::UInt64 virtualStart = mapping.VirtualStart.GetValue();
    const Foundation::UInt64 pageCount = mapping.Pages.GetValue();
    const Foundation::UInt64 translationWord = baseEncoding.GetValue().TranslationWord;
    for (Foundation::UInt64 index{0U}; index < publishedRecordCount; ++index)
    {
        Ia64RuntimeTranslationLedgerWireRecord& record = m_wire.Records[index];
        if ((record.VirtualStart == virtualStart) && (LoadPageCount(record) == pageCount) &&
            (record.BaseTranslationWord == translationWord))
        {
            __atomic_store_n(&record.PageCount, 0U, __ATOMIC_RELEASE);
            return Succeed();
        }
    }
    return Fail(Ia64RuntimeTranslationLedgerError::MappingNotFound);
}

Foundation::Result<EncodedIa64RuntimeTranslation, Ia64RuntimeTranslationLedgerFailure>
Ia64RuntimeTranslationLedger::Lookup(const Foundation::UInt64 faultAddress) const noexcept
{
    using ResultType = Foundation::Result<EncodedIa64RuntimeTranslation, Ia64RuntimeTranslationLedgerFailure>;

    const Foundation::UInt64 publicationWord = m_wire.PublicationWord.LoadAcquire();
    if (publicationWord == 0U)
    {
        return ResultType::Failure(Fail(Ia64RuntimeTranslationLedgerError::NotInitialized));
    }
    const Foundation::UInt64 publishedRecordCount = publicationWord - 1U;
    const Foundation::UInt64 pageAddress = faultAddress & ~PageOffsetMask;

    for (Foundation::UInt64 index{0U}; index < publishedRecordCount; ++index)
    {
        const Ia64RuntimeTranslationLedgerWireRecord& record = m_wire.Records[index];
        const Foundation::UInt64 count = LoadPageCount(record);
        if ((count == 0U) || (pageAddress < record.VirtualStart))
        {
            continue;
        }

        const Foundation::UInt64 pageIndex = (pageAddress - record.VirtualStart) >> Ia64RuntimeTranslationPageShift;
        if (pageIndex >= count)
        {
            continue;
        }
        const Foundation::UInt64 pageOffset = pageIndex << Ia64RuntimeTranslationPageShift;
        const Foundation::UInt64 physicalPage = (record.BaseTranslationWord & PhysicalPageNumberMask) + pageOffset;
        const Foundation::UInt64 invariantAttributes = record.BaseTranslationWord & ~PhysicalPageNumberMask;
        return ResultType::Success(EncodedIa64RuntimeTranslation{
            .TranslationWord = physicalPage | invariantAttributes,
            .TranslationInsertion = RuntimeTranslationInsertion,
            .FaultingAddress = pageAddress,
        });
    }

    return ResultType::Failure(Fail(Ia64RuntimeTranslationLedgerError::MappingNotFound));
}
} // Namespace Asteria::Platform::Ia64::Memory
