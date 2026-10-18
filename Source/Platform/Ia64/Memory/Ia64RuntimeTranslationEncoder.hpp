// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_MEMORY_IA64_RUNTIME_TRANSLATION_ENCODER_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_MEMORY_IA64_RUNTIME_TRANSLATION_ENCODER_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Memory/AddressSpaceMappingRecord.hpp"
#include "Platform/Ia64/Memory/InitialTranslationEncoder.hpp"

namespace Asteria::Platform::Ia64::Memory
{
constexpr Foundation::UInt8 Ia64RuntimeTranslationPageShift{12U};

/// Matches the bounded runtime ledger and invalidation range consumed by the
/// later mapping transaction. This encoder neither publishes nor installs it.
constexpr Foundation::UInt64 MaximumIa64RuntimeMappingPages{512U};

enum class Ia64RuntimeTranslationEncodingError : Foundation::UInt32
{
    None,
    InvalidMappingPlan,
    MappingTooLarge,
    PageIndexOutOfBounds,
    InvalidCapabilities,
    UnsupportedPageSize,
    RegionZeroReserved,
    CrossRegionMapping,
    UnimplementedVirtualAddress,
    PhysicalAddressOutOfRange,
    UnsupportedCachePolicy,
    UnsupportedMemoryAttribute,
    UnsupportedPermissions,
    DeviceExecutionDenied,
};

/// Contains the exact operands needed for one later software-refill insertion.
/// RID identity is supplied by the active region register and is not encoded in
/// TranslationWord. TranslationInsertion always selects key zero and PS 12.
struct EncodedIa64RuntimeTranslation
{
    Foundation::UInt64 TranslationWord;
    Foundation::UInt64 TranslationInsertion;
    Foundation::UInt64 FaultingAddress;
};

/// Purely encodes one 4 KiB page from an authority-free runtime mapping record.
///
/// EncodePage is bounded, allocation-free, and failure-atomic. It only produces
/// operands; publication, insertion, and activation are caller-owned.
class Ia64RuntimeTranslationEncoder final
{
public:
    [[nodiscard]] static Foundation::Result<EncodedIa64RuntimeTranslation, Ia64RuntimeTranslationEncodingError>
    EncodePage(const Kernel::Memory::AddressSpaceMappingRecord& mapping, Foundation::UInt64 pageIndex,
               const TranslationCapabilities& capabilities) noexcept;

private:
    Ia64RuntimeTranslationEncoder() = delete;
};

static_assert(sizeof(EncodedIa64RuntimeTranslation) == 24U);
static_assert(alignof(EncodedIa64RuntimeTranslation) == 8U);
static_assert(sizeof(Ia64RuntimeTranslationEncodingError) == 4U);
} // namespace Asteria::Platform::Ia64::Memory

#endif // ASTERIA_PLATFORM_IA64_MEMORY_IA64_RUNTIME_TRANSLATION_ENCODER_HPP_INCLUDED
