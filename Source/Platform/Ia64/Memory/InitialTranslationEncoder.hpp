// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_MEMORY_INITIAL_TRANSLATION_ENCODER_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_MEMORY_INITIAL_TRANSLATION_ENCODER_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Memory/InitialMappingPlan.hpp"

namespace Asteria::Platform::Ia64::Memory
{
constexpr Foundation::UInt8 InitialTranslationPageShift{12U};

enum class InitialTranslationEncodingError : Foundation::UInt32
{
    None,
    PlanNotSealed,
    MappingIndexOutOfBounds,
    PageIndexOutOfBounds,
    TranslationRangeOutOfBounds,
    InvalidPageShift,
    MisalignedTranslationRange,
    InvalidCapabilities,
    UnsupportedPageSize,
    InvalidRegionConfiguration,
    HardwareWalkerUnavailable,
    GlobalRegionMismatch,
    UnimplementedVirtualAddress,
    PhysicalAddressOutOfRange,
    UnsupportedCachePolicy,
    UnsupportedMemoryAttribute,
    UnsupportedPermissions,
    InternalInvariantViolation,
};

/// Captures the PAL-reported fields consumed by the initial 4 KiB encoder.
/// The activation adapter must keep PSR.vm clear for these address checks.
struct TranslationCapabilities
{
    Foundation::UInt64 InsertablePageSizeMask;
    Foundation::UInt8 PhysicalAddressBits;
    Foundation::UInt8 ImplementedVirtualAddressMostSignificantBit;
    Foundation::UInt8 RegionIdentifierBits;
    Foundation::UInt8 ProtectionKeyBits;
    Foundation::UInt8 MemoryAttributeMask;
    bool HardwareWalkerImplemented;
    Foundation::UInt16 Reserved16;
    Foundation::UInt64 Reserved64;
};

/// Selects the region-register identity used when the returned translation is inserted.
/// A shared region identifier is the IA-64 realization of the common Global permission.
struct RegionConfiguration
{
    Foundation::UInt32 RegionIdentifier;
    Foundation::UInt32 ProtectionKey;
    bool VhptWalkerEnabled;
    bool SharedAcrossAddressSpaces;
    Foundation::UInt16 Reserved16;
    Foundation::UInt32 Reserved32;
};

/// Contains the four architected words consumed by mov-to-RR and itr/itc insertion.
/// The privileged adapter must install the RR first, then IFA and ITIR, perform the
/// insertion with TranslationWord, and issue the required data/instruction serialization.
struct EncodedInitialTranslation
{
    Foundation::UInt64 TranslationWord;
    Foundation::UInt64 TranslationInsertion;
    Foundation::UInt64 FaultingAddress;
    Foundation::UInt64 RegionRegister;
};

/// Encodes one page from a sealed architecture-neutral mapping plan. It performs no
/// privileged operation and does not allocate or mutate caller-owned storage.
class InitialTranslationEncoder final
{
public:
    [[nodiscard]] static Foundation::Result<EncodedInitialTranslation, InitialTranslationEncodingError>
    EncodePage(const Kernel::Memory::InitialMappingPlan& plan, Foundation::Size mappingIndex,
               Foundation::UInt64 pageIndex, const TranslationCapabilities& capabilities,
               const RegionConfiguration& region) noexcept;

    /// Encodes one naturally aligned power-of-two range wholly contained in a
    /// mapping. firstPageIndex is expressed in the common 4 KiB base pages.
    [[nodiscard]] static Foundation::Result<EncodedInitialTranslation, InitialTranslationEncodingError>
    EncodeRange(const Kernel::Memory::InitialMappingPlan& plan, Foundation::Size mappingIndex,
                Foundation::UInt64 firstPageIndex, Foundation::UInt8 pageShift,
                const TranslationCapabilities& capabilities, const RegionConfiguration& region) noexcept;

private:
    InitialTranslationEncoder() = delete;
};

static_assert(sizeof(TranslationCapabilities) == 24U);
static_assert(alignof(TranslationCapabilities) == 8U);
static_assert(sizeof(RegionConfiguration) == 16U);
static_assert(alignof(RegionConfiguration) == 4U);
static_assert(sizeof(EncodedInitialTranslation) == 32U);
static_assert(alignof(EncodedInitialTranslation) == 8U);
static_assert(sizeof(InitialTranslationEncodingError) == 4U);
} // namespace Asteria::Platform::Ia64::Memory

#endif // ASTERIA_PLATFORM_IA64_MEMORY_INITIAL_TRANSLATION_ENCODER_HPP_INCLUDED
