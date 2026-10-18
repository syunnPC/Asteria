// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Memory/Ia64RuntimeTranslationCacheInserter.hpp"

extern "C" void AsteriaIa64InsertRuntimeDataTranslationCache(Asteria::Foundation::UInt64 translationWord,
                                                             Asteria::Foundation::UInt64 faultingAddress,
                                                             Asteria::Foundation::UInt64 translationInsertion) noexcept;

extern "C" void
AsteriaIa64InsertRuntimeInstructionTranslationCache(Asteria::Foundation::UInt64 translationWord,
                                                    Asteria::Foundation::UInt64 faultingAddress,
                                                    Asteria::Foundation::UInt64 translationInsertion) noexcept;

// The target entry points are in Ia64RuntimeTranslationCacheInserter.S.

namespace Asteria::Platform::Ia64::Memory
{
namespace
{
constexpr Foundation::UInt64 PageOffsetMask{0xFFFU};
constexpr Foundation::UInt64 RegionMask{Foundation::UInt64{7U} << 61U};
constexpr Foundation::UInt64 RequiredTranslationInsertion{0x30U};
constexpr Foundation::UInt64 PresentBit{Foundation::UInt64{1U} << 0U};
constexpr Foundation::UInt64 MemoryAttributeMask{Foundation::UInt64{7U} << 2U};
constexpr Foundation::UInt64 AccessedBit{Foundation::UInt64{1U} << 5U};
constexpr Foundation::UInt64 DirtyBit{Foundation::UInt64{1U} << 6U};
constexpr Foundation::UInt64 PrivilegeLevelMask{Foundation::UInt64{3U} << 7U};
constexpr Foundation::UInt64 AccessRightsMask{Foundation::UInt64{7U} << 9U};
constexpr Foundation::UInt64 PhysicalPageNumberMask{0x0003'FFFF'FFFF'F000ULL};
constexpr Foundation::UInt64 ExceptionDeferralBit{Foundation::UInt64{1U} << 52U};
constexpr Foundation::UInt64 RequiredTranslationBits{PresentBit | AccessedBit | ExceptionDeferralBit};
constexpr Foundation::UInt64 AllowedTranslationWordMask{PresentBit | MemoryAttributeMask | AccessedBit | DirtyBit |
                                                        PrivilegeLevelMask | AccessRightsMask | PhysicalPageNumberMask |
                                                        ExceptionDeferralBit};
constexpr Foundation::UInt64 ReadWriteAccessRights{2U};

[[nodiscard]] constexpr bool IsKnownInsertionKind(const Ia64RuntimeTranslationCacheInsertionKind kind) noexcept
{
    switch (kind)
    {
    case Ia64RuntimeTranslationCacheInsertionKind::Data:
    case Ia64RuntimeTranslationCacheInsertionKind::Instruction:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool IsMemoryAttributeValid(const Foundation::UInt64 memoryAttribute) noexcept
{
    return (memoryAttribute == 0U) || (memoryAttribute == 4U) || (memoryAttribute == 6U);
}

[[nodiscard]] constexpr bool IsPrivilegeLevelValid(const Foundation::UInt64 privilegeLevel) noexcept
{
    return (privilegeLevel == 0U) || (privilegeLevel == 3U);
}

[[nodiscard]] constexpr bool IsTranslationWordValid(const Foundation::UInt64 translationWord) noexcept
{
    if (((translationWord & RequiredTranslationBits) != RequiredTranslationBits) ||
        ((translationWord & ~AllowedTranslationWordMask) != 0U))
    {
        return false;
    }

    const Foundation::UInt64 memoryAttribute = (translationWord & MemoryAttributeMask) >> 2U;
    const Foundation::UInt64 privilegeLevel = (translationWord & PrivilegeLevelMask) >> 7U;
    const Foundation::UInt64 accessRights = (translationWord & AccessRightsMask) >> 9U;
    const bool isDirty = (translationWord & DirtyBit) != 0U;
    return IsMemoryAttributeValid(memoryAttribute) && IsPrivilegeLevelValid(privilegeLevel) &&
           (accessRights <= ReadWriteAccessRights) && (isDirty == (accessRights == ReadWriteAccessRights));
}
} // namespace

Ia64RuntimeTranslationCacheInsertionError
Ia64RuntimeTranslationCacheInserter::Insert(const EncodedIa64RuntimeTranslation& translation,
                                            const Ia64RuntimeTranslationCacheInsertionKind kind) noexcept
{
    const Foundation::UInt64 translationWord = translation.TranslationWord;
    const Foundation::UInt64 translationInsertion = translation.TranslationInsertion;
    const Foundation::UInt64 faultingAddress = translation.FaultingAddress;

    if (!IsKnownInsertionKind(kind))
    {
        return Ia64RuntimeTranslationCacheInsertionError::InvalidInsertionKind;
    }
    if (translationInsertion != RequiredTranslationInsertion)
    {
        return Ia64RuntimeTranslationCacheInsertionError::InvalidTranslationInsertion;
    }
    if (((faultingAddress & PageOffsetMask) != 0U) || ((faultingAddress & RegionMask) == 0U))
    {
        return Ia64RuntimeTranslationCacheInsertionError::InvalidFaultingAddress;
    }
    if (!IsTranslationWordValid(translationWord))
    {
        return Ia64RuntimeTranslationCacheInsertionError::InvalidTranslationWord;
    }

    switch (kind)
    {
    case Ia64RuntimeTranslationCacheInsertionKind::Data:
        AsteriaIa64InsertRuntimeDataTranslationCache(translationWord, faultingAddress, translationInsertion);
        break;

    case Ia64RuntimeTranslationCacheInsertionKind::Instruction:
        AsteriaIa64InsertRuntimeInstructionTranslationCache(translationWord, faultingAddress, translationInsertion);
        break;
    }

    return Ia64RuntimeTranslationCacheInsertionError::None;
}
} // namespace Asteria::Platform::Ia64::Memory
