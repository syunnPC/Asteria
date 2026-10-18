// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_MEMORY_PAL_VIRTUAL_MEMORY_DECODER_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_MEMORY_PAL_VIRTUAL_MEMORY_DECODER_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Platform/Ia64/Memory/InitialTranslationEncoder.hpp"

namespace Asteria::Platform::Ia64::Memory
{
constexpr Foundation::UInt64 ArchitectedMinimumInsertablePageSizeMask{
    (Foundation::UInt64{1U} << 12U) | (Foundation::UInt64{1U} << 13U) | (Foundation::UInt64{1U} << 14U) |
    (Foundation::UInt64{1U} << 16U) | (Foundation::UInt64{1U} << 18U) | (Foundation::UInt64{1U} << 20U) |
    (Foundation::UInt64{1U} << 22U) | (Foundation::UInt64{1U} << 24U) | (Foundation::UInt64{1U} << 26U) |
    (Foundation::UInt64{1U} << 28U)};
constexpr Foundation::UInt64 ArchitectedMinimumPurgePageSizeMask{ArchitectedMinimumInsertablePageSizeMask |
                                                                 (Foundation::UInt64{1U} << 32U)};

enum class PalVirtualMemoryDecodeError : Foundation::UInt32
{
    None,
    SummaryCallFailed,
    PageSizeCallFailed,
    MemoryAttributeCallFailed,
    SummaryReservedResult,
    PageSizeReservedResult,
    MemoryAttributeReservedResult,
    InvalidAddressCapabilities,
    InvalidProtectionCapabilities,
    InvalidTranslationRegisterCapabilities,
    InvalidTranslationCacheTopology,
    MissingMandatoryInsertablePageSize,
    MissingMandatoryPurgePageSize,
    PageSizeBelowArchitectedMinimum,
    InsertablePageSizeNotPurgeable,
    InvalidMemoryAttributeCapabilities,
    MissingRequiredMemoryAttribute,
};

/// Contains the four registers returned by a static PAL procedure.
/// Status is interpreted as a signed PAL status; every successful call has value zero.
struct PalStaticCallResult
{
    Foundation::Int64 Status;
    Foundation::UInt64 Result1;
    Foundation::UInt64 Result2;
    Foundation::UInt64 Result3;
};

/// Retains the validated PAL virtual-memory summary used by later resource planners.
/// MaximumConcurrentPurgesEncoding preserves PAL's special zero and 0xffff encodings.
struct PalVirtualMemoryCapabilities
{
    TranslationCapabilities Translation;
    Foundation::UInt64 PurgePageSizeMask;
    Foundation::UInt16 InstructionTranslationRegisterCount;
    Foundation::UInt16 DataTranslationRegisterCount;
    Foundation::UInt16 ProtectionKeyRegisterCount;
    Foundation::UInt16 MaximumConcurrentPurgesEncoding;
    Foundation::UInt8 HashTagIdentifier;
    Foundation::UInt8 UniqueTranslationCacheCount;
    Foundation::UInt8 TranslationCacheLevelCount;
    Foundation::UInt8 Reserved8;
    Foundation::UInt32 Reserved32;
};

/// Validates PAL_VM_SUMMARY, PAL_VM_PAGE_SIZE, and PAL_MEM_ATTRIB return registers.
/// The PAL invocation adapter remains separate and must pass results from successful,
/// correctly serialized static calls made on the processor being initialized.
class PalVirtualMemoryDecoder final
{
public:
    [[nodiscard]] static Foundation::Result<PalVirtualMemoryCapabilities, PalVirtualMemoryDecodeError>
    Decode(const PalStaticCallResult& summary, const PalStaticCallResult& pageSizes,
           const PalStaticCallResult& memoryAttributes) noexcept;

private:
    PalVirtualMemoryDecoder() = delete;
};

static_assert(sizeof(PalStaticCallResult) == 32U);
static_assert(alignof(PalStaticCallResult) == 8U);
static_assert(sizeof(PalVirtualMemoryCapabilities) == 48U);
static_assert(alignof(PalVirtualMemoryCapabilities) == 8U);
static_assert(sizeof(PalVirtualMemoryDecodeError) == 4U);
} // namespace Asteria::Platform::Ia64::Memory

#endif // ASTERIA_PLATFORM_IA64_MEMORY_PAL_VIRTUAL_MEMORY_DECODER_HPP_INCLUDED
