// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_FIRMWARE_SAL_SYSTEM_TABLE_PARSER_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_FIRMWARE_SAL_SYSTEM_TABLE_PARSER_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"

namespace Asteria::Platform::Ia64::Firmware
{
constexpr Foundation::Size SalSystemTableHeaderSize{96U};

enum class SalSystemTableParseError : Foundation::UInt32
{
    None,
    NullBuffer,
    HeaderTooShort,
    InvalidSignature,
    InvalidTableLength,
    TruncatedTable,
    InvalidRevisionEncoding,
    UnsupportedRevision,
    InvalidHeaderReservedField,
    InvalidChecksum,
    DescriptorTruncated,
    UnknownDescriptorType,
    InvalidDescriptorOrder,
    DuplicateDescriptor,
    InvalidEntrypointDescriptor,
    InvalidPlatformFeaturesDescriptor,
    InvalidTranslationRegisterDescriptor,
    InvalidPurgeCoherenceDescriptor,
    InvalidApWakeDescriptor,
    TableLengthMismatch,
    MissingEntrypointDescriptor,
};

/// Contains the validated SAL data needed by later invocation and resource planners.
/// Physical addresses remain inert integer values until a separate extent validator
/// proves that the referenced firmware ranges are present in the UEFI memory map.
/// The firmware ITR virtual address and page size also remain inert until checked
/// against PAL_VM_SUMMARY and PAL_VM_PAGE_SIZE for the current processor.
struct SalSystemTableInformation
{
    Foundation::UInt64 PalProcedurePhysicalAddress;
    Foundation::UInt64 SalProcedurePhysicalAddress;
    Foundation::UInt64 SalGlobalDataPhysicalAddress;
    Foundation::UInt64 TranslationRegisterVirtualAddress;
    Foundation::UInt64 TranslationRegisterEncodedPageSize;
    Foundation::UInt64 CoherenceDomainInformationPhysicalAddress;
    Foundation::UInt64 ApWakeVector;
    Foundation::UInt32 TotalTableLength;
    Foundation::UInt32 CoherenceDomainCount;
    Foundation::UInt16 SalRevision;
    Foundation::UInt16 EntryCount;
    Foundation::UInt16 SalAVersion;
    Foundation::UInt16 SalBVersion;
    Foundation::UInt16 MemoryDescriptorCount;
    Foundation::UInt8 PlatformFeatures;
    bool HasPlatformFeatures;
    bool HasTranslationRegister;
    bool HasPurgeCoherenceInformation;
    bool HasApWakeVector;
    Foundation::UInt8 Reserved8;
};

/// Parses a little-endian SAL System Table without dereferencing any address it contains.
/// availableBytes must describe the readable extent beginning at bytes, and every byte in
/// the declared table must remain stable throughout the call. An adapter facing mutable
/// firmware storage must first take an owned immutable snapshot.
class SalSystemTableParser final
{
public:
    [[nodiscard]] static Foundation::Result<SalSystemTableInformation, SalSystemTableParseError>
    Parse(const Foundation::UInt8* bytes, Foundation::Size availableBytes) noexcept;

private:
    SalSystemTableParser() = delete;
};

static_assert(sizeof(SalSystemTableInformation) == 80U);
static_assert(alignof(SalSystemTableInformation) == 8U);
static_assert(sizeof(SalSystemTableParseError) == 4U);
} // namespace Asteria::Platform::Ia64::Firmware

#endif // ASTERIA_PLATFORM_IA64_FIRMWARE_SAL_SYSTEM_TABLE_PARSER_HPP_INCLUDED
