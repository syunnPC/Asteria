// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Foundation/Types.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeTranslationCacheInserter.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
enum class RecordedInsertionKind : Foundation::UInt8
{
    None,
    Data,
    Instruction,
};

Foundation::Size g_dataInsertionCount{0U};
Foundation::Size g_instructionInsertionCount{0U};
Foundation::UInt64 g_recordedTranslationWord{0U};
Foundation::UInt64 g_recordedFaultingAddress{0U};
Foundation::UInt64 g_recordedTranslationInsertion{0U};
RecordedInsertionKind g_recordedInsertionKind{RecordedInsertionKind::None};
bool g_helperCompleted{false};
} // namespace Asteria::Test

extern "C" void
AsteriaIa64InsertRuntimeDataTranslationCache(const Asteria::Foundation::UInt64 translationWord,
                                             const Asteria::Foundation::UInt64 faultingAddress,
                                             const Asteria::Foundation::UInt64 translationInsertion) noexcept
{
    ++Asteria::Test::g_dataInsertionCount;
    Asteria::Test::g_recordedTranslationWord = translationWord;
    Asteria::Test::g_recordedFaultingAddress = faultingAddress;
    Asteria::Test::g_recordedTranslationInsertion = translationInsertion;
    Asteria::Test::g_recordedInsertionKind = Asteria::Test::RecordedInsertionKind::Data;
    Asteria::Test::g_helperCompleted = true;
}

extern "C" void
AsteriaIa64InsertRuntimeInstructionTranslationCache(const Asteria::Foundation::UInt64 translationWord,
                                                    const Asteria::Foundation::UInt64 faultingAddress,
                                                    const Asteria::Foundation::UInt64 translationInsertion) noexcept
{
    ++Asteria::Test::g_instructionInsertionCount;
    Asteria::Test::g_recordedTranslationWord = translationWord;
    Asteria::Test::g_recordedFaultingAddress = faultingAddress;
    Asteria::Test::g_recordedTranslationInsertion = translationInsertion;
    Asteria::Test::g_recordedInsertionKind = Asteria::Test::RecordedInsertionKind::Instruction;
    Asteria::Test::g_helperCompleted = true;
}

namespace Asteria::Test
{
namespace
{
using Platform::Ia64::Memory::EncodedIa64RuntimeTranslation;
using Platform::Ia64::Memory::Ia64RuntimeTranslationCacheInserter;
using Platform::Ia64::Memory::Ia64RuntimeTranslationCacheInsertionError;
using Platform::Ia64::Memory::Ia64RuntimeTranslationCacheInsertionKind;

constexpr Foundation::UInt64 PresentBit{Foundation::UInt64{1U} << 0U};
constexpr Foundation::UInt64 AccessedBit{Foundation::UInt64{1U} << 5U};
constexpr Foundation::UInt64 DirtyBit{Foundation::UInt64{1U} << 6U};
constexpr Foundation::UInt64 ExceptionDeferralBit{Foundation::UInt64{1U} << 52U};
constexpr Foundation::UInt64 PhysicalPageNumberMask{0x0003'FFFF'FFFF'F000ULL};
constexpr Foundation::UInt64 ValidFaultingAddress{0xA000'0000'0040'0000ULL};
constexpr Foundation::UInt64 ValidPhysicalPage{0x0001'2345'6789'A000ULL};
constexpr Foundation::UInt64 ValidTranslationInsertion{0x30U};

[[nodiscard]] constexpr Foundation::UInt64
MakeTranslationWord(const Foundation::UInt64 memoryAttribute = 0U, const Foundation::UInt64 privilegeLevel = 0U,
                    const Foundation::UInt64 accessRights = 0U,
                    const Foundation::UInt64 physicalPage = ValidPhysicalPage) noexcept
{
    return (physicalPage & PhysicalPageNumberMask) | PresentBit | (memoryAttribute << 2U) | AccessedBit |
           ((accessRights == 2U) ? DirtyBit : 0U) | (privilegeLevel << 7U) | (accessRights << 9U) |
           ExceptionDeferralBit;
}

[[nodiscard]] constexpr EncodedIa64RuntimeTranslation
MakeTranslation(const Foundation::UInt64 translationWord = MakeTranslationWord(),
                const Foundation::UInt64 translationInsertion = ValidTranslationInsertion,
                const Foundation::UInt64 faultingAddress = ValidFaultingAddress) noexcept
{
    return EncodedIa64RuntimeTranslation{
        .TranslationWord = translationWord,
        .TranslationInsertion = translationInsertion,
        .FaultingAddress = faultingAddress,
    };
}

void ResetRecording() noexcept
{
    g_dataInsertionCount = 0U;
    g_instructionInsertionCount = 0U;
    g_recordedTranslationWord = 0U;
    g_recordedFaultingAddress = 0U;
    g_recordedTranslationInsertion = 0U;
    g_recordedInsertionKind = RecordedInsertionKind::None;
    g_helperCompleted = false;
}

void CheckRejected(TestContext& runner, const EncodedIa64RuntimeTranslation& translation,
                   const Ia64RuntimeTranslationCacheInsertionKind kind,
                   const Ia64RuntimeTranslationCacheInsertionError expectedError,
                   const char* const description) noexcept
{
    ResetRecording();
    const Ia64RuntimeTranslationCacheInsertionError error =
        Ia64RuntimeTranslationCacheInserter::Insert(translation, kind);
    ASTERIA_EXPECT(runner, description, error == expectedError, g_dataInsertionCount == 0U,
                   g_instructionInsertionCount == 0U, g_recordedInsertionKind == RecordedInsertionKind::None,
                   !g_helperCompleted);
}

void CheckAccepted(TestContext& runner, const EncodedIa64RuntimeTranslation& translation,
                   const Ia64RuntimeTranslationCacheInsertionKind kind, const char* const description) noexcept
{
    ResetRecording();
    const Ia64RuntimeTranslationCacheInsertionError error =
        Ia64RuntimeTranslationCacheInserter::Insert(translation, kind);
    const bool expectsData = kind == Ia64RuntimeTranslationCacheInsertionKind::Data;
    ASTERIA_EXPECT(
        runner, description, error == Ia64RuntimeTranslationCacheInsertionError::None,
        g_dataInsertionCount == (expectsData ? 1U : 0U), g_instructionInsertionCount == (expectsData ? 0U : 1U),
        g_recordedInsertionKind == (expectsData ? RecordedInsertionKind::Data : RecordedInsertionKind::Instruction),
        g_recordedTranslationWord == translation.TranslationWord,
        g_recordedFaultingAddress == translation.FaultingAddress,
        g_recordedTranslationInsertion == translation.TranslationInsertion, g_helperCompleted);
}

void TestInsertionKindValidation(TestContext& runner) noexcept
{
    for (Foundation::UInt16 rawKind{2U}; rawKind <= 0xFFU; ++rawKind)
    {
        CheckRejected(runner, MakeTranslation(), static_cast<Ia64RuntimeTranslationCacheInsertionKind>(rawKind),
                      Ia64RuntimeTranslationCacheInsertionError::InvalidInsertionKind,
                      "every unknown insertion-kind representation is rejected before commit");
    }
}

void TestTranslationInsertionValidation(TestContext& runner) noexcept
{
    for (Foundation::UInt8 bit{0U}; bit < 64U; ++bit)
    {
        CheckRejected(
            runner, MakeTranslation(MakeTranslationWord(), ValidTranslationInsertion ^ (Foundation::UInt64{1U} << bit)),
            Ia64RuntimeTranslationCacheInsertionKind::Data,
            Ia64RuntimeTranslationCacheInsertionError::InvalidTranslationInsertion,
            "every single-bit ITIR mutation is rejected before commit");
    }
}

void TestFaultingAddressValidation(TestContext& runner) noexcept
{
    for (Foundation::UInt8 bit{0U}; bit < 12U; ++bit)
    {
        CheckRejected(runner,
                      MakeTranslation(MakeTranslationWord(), ValidTranslationInsertion,
                                      ValidFaultingAddress | (Foundation::UInt64{1U} << bit)),
                      Ia64RuntimeTranslationCacheInsertionKind::Data,
                      Ia64RuntimeTranslationCacheInsertionError::InvalidFaultingAddress,
                      "every nonzero page-offset bit is rejected before commit");
    }
    CheckRejected(runner, MakeTranslation(MakeTranslationWord(), ValidTranslationInsertion, 0x0000'0000'0040'0000ULL),
                  Ia64RuntimeTranslationCacheInsertionKind::Data,
                  Ia64RuntimeTranslationCacheInsertionError::InvalidFaultingAddress,
                  "region-zero IFA is rejected before commit");
}

void TestRequiredAndReservedTranslationBits(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 RequiredBits[]{
        PresentBit,
        AccessedBit,
        ExceptionDeferralBit,
    };
    for (const Foundation::UInt64 bit : RequiredBits)
    {
        CheckRejected(runner, MakeTranslation(MakeTranslationWord() & ~bit),
                      Ia64RuntimeTranslationCacheInsertionKind::Data,
                      Ia64RuntimeTranslationCacheInsertionError::InvalidTranslationWord,
                      "every required P/A/ED bit is checked before commit");
    }

    constexpr Foundation::UInt8 CheckedZeroBits[]{1U, 50U, 51U};
    for (const Foundation::UInt8 bit : CheckedZeroBits)
    {
        CheckRejected(runner, MakeTranslation(MakeTranslationWord() | (Foundation::UInt64{1U} << bit)),
                      Ia64RuntimeTranslationCacheInsertionKind::Data,
                      Ia64RuntimeTranslationCacheInsertionError::InvalidTranslationWord,
                      "every checked-zero translation bit is rejected before commit");
    }

    for (Foundation::UInt8 bit{53U}; bit < 64U; ++bit)
    {
        CheckRejected(runner, MakeTranslation(MakeTranslationWord() | (Foundation::UInt64{1U} << bit)),
                      Ia64RuntimeTranslationCacheInsertionKind::Data,
                      Ia64RuntimeTranslationCacheInsertionError::InvalidTranslationWord,
                      "every ignored high translation bit must remain zero");
    }
}

void TestTranslationFieldValidation(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 InvalidMemoryAttributes[]{1U, 2U, 3U, 5U, 7U};
    for (const Foundation::UInt64 memoryAttribute : InvalidMemoryAttributes)
    {
        CheckRejected(runner, MakeTranslation(MakeTranslationWord(memoryAttribute)),
                      Ia64RuntimeTranslationCacheInsertionKind::Data,
                      Ia64RuntimeTranslationCacheInsertionError::InvalidTranslationWord,
                      "every unsupported memory attribute is rejected before commit");
    }

    constexpr Foundation::UInt64 InvalidPrivilegeLevels[]{1U, 2U};
    for (const Foundation::UInt64 privilegeLevel : InvalidPrivilegeLevels)
    {
        CheckRejected(runner, MakeTranslation(MakeTranslationWord(0U, privilegeLevel)),
                      Ia64RuntimeTranslationCacheInsertionKind::Data,
                      Ia64RuntimeTranslationCacheInsertionError::InvalidTranslationWord,
                      "every non-PL0/non-PL3 translation is rejected before commit");
    }

    for (Foundation::UInt64 accessRights{3U}; accessRights <= 7U; ++accessRights)
    {
        CheckRejected(runner, MakeTranslation(MakeTranslationWord(0U, 0U, accessRights)),
                      Ia64RuntimeTranslationCacheInsertionKind::Data,
                      Ia64RuntimeTranslationCacheInsertionError::InvalidTranslationWord,
                      "every access-right encoding beyond AR2 is rejected before commit");
    }

    CheckRejected(runner, MakeTranslation(MakeTranslationWord(0U, 0U, 0U) | DirtyBit),
                  Ia64RuntimeTranslationCacheInsertionKind::Data,
                  Ia64RuntimeTranslationCacheInsertionError::InvalidTranslationWord,
                  "AR0 with D set is rejected before commit");
    CheckRejected(runner, MakeTranslation(MakeTranslationWord(0U, 0U, 1U) | DirtyBit),
                  Ia64RuntimeTranslationCacheInsertionKind::Data,
                  Ia64RuntimeTranslationCacheInsertionError::InvalidTranslationWord,
                  "AR1 with D set is rejected before commit");
    CheckRejected(runner, MakeTranslation(MakeTranslationWord(0U, 0U, 2U) & ~DirtyBit),
                  Ia64RuntimeTranslationCacheInsertionKind::Data,
                  Ia64RuntimeTranslationCacheInsertionError::InvalidTranslationWord,
                  "AR2 with D clear is rejected before commit");
}

void TestDeterministicValidationOrder(TestContext& runner) noexcept
{
    const EncodedIa64RuntimeTranslation allOperandsInvalid{
        .TranslationWord = 0U,
        .TranslationInsertion = 0U,
        .FaultingAddress = 0U,
    };
    CheckRejected(runner, allOperandsInvalid, static_cast<Ia64RuntimeTranslationCacheInsertionKind>(0xFFU),
                  Ia64RuntimeTranslationCacheInsertionError::InvalidInsertionKind,
                  "kind validation deterministically precedes every operand check");
    CheckRejected(runner, allOperandsInvalid, Ia64RuntimeTranslationCacheInsertionKind::Data,
                  Ia64RuntimeTranslationCacheInsertionError::InvalidTranslationInsertion,
                  "ITIR validation deterministically precedes IFA and translation checks");
    CheckRejected(runner, MakeTranslation(0U, ValidTranslationInsertion, 0U),
                  Ia64RuntimeTranslationCacheInsertionKind::Data,
                  Ia64RuntimeTranslationCacheInsertionError::InvalidFaultingAddress,
                  "IFA validation deterministically precedes translation-word checks");
    CheckRejected(runner, MakeTranslation(MakeTranslationWord(), 0U),
                  Ia64RuntimeTranslationCacheInsertionKind::Instruction,
                  Ia64RuntimeTranslationCacheInsertionError::InvalidTranslationInsertion,
                  "invalid instruction ITIR operands cannot reach either helper");
    CheckRejected(runner, MakeTranslation(MakeTranslationWord(), ValidTranslationInsertion, 0U),
                  Ia64RuntimeTranslationCacheInsertionKind::Instruction,
                  Ia64RuntimeTranslationCacheInsertionError::InvalidFaultingAddress,
                  "invalid instruction IFA operands cannot reach either helper");
    CheckRejected(runner, MakeTranslation(0U), Ia64RuntimeTranslationCacheInsertionKind::Instruction,
                  Ia64RuntimeTranslationCacheInsertionError::InvalidTranslationWord,
                  "invalid instruction translation words cannot reach either helper");
}

void TestValidFormsAndExactForwarding(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 MemoryAttributes[]{0U, 4U, 6U};
    constexpr Foundation::UInt64 PrivilegeLevels[]{0U, 3U};
    for (const Foundation::UInt64 memoryAttribute : MemoryAttributes)
    {
        for (const Foundation::UInt64 privilegeLevel : PrivilegeLevels)
        {
            for (Foundation::UInt64 accessRights{0U}; accessRights <= 2U; ++accessRights)
            {
                CheckAccepted(runner,
                              MakeTranslation(MakeTranslationWord(memoryAttribute, privilegeLevel, accessRights)),
                              Ia64RuntimeTranslationCacheInsertionKind::Data,
                              "data insertion accepts every valid cache/PL/AR form exactly once");
                CheckAccepted(runner,
                              MakeTranslation(MakeTranslationWord(memoryAttribute, privilegeLevel, accessRights)),
                              Ia64RuntimeTranslationCacheInsertionKind::Instruction,
                              "instruction insertion accepts every valid cache/PL/AR form exactly once");
            }
        }
    }

    constexpr Foundation::UInt64 PhysicalPages[]{
        0U,
        Foundation::UInt64{1U} << 12U,
        Foundation::UInt64{1U} << 49U,
        PhysicalPageNumberMask,
    };
    for (const Foundation::UInt64 physicalPage : PhysicalPages)
    {
        CheckAccepted(runner, MakeTranslation(MakeTranslationWord(6U, 3U, 2U, physicalPage)),
                      Ia64RuntimeTranslationCacheInsertionKind::Data,
                      "PPN boundary forms are forwarded only in translation bits 49:12");
    }

    constexpr Foundation::UInt64 RegionBases[]{
        0x2000'0000'0000'0000ULL, 0x4000'0000'0000'0000ULL, 0x6000'0000'0000'0000ULL, 0x8000'0000'0000'0000ULL,
        0xA000'0000'0000'0000ULL, 0xC000'0000'0000'0000ULL, 0xE000'0000'0000'0000ULL,
    };
    for (const Foundation::UInt64 faultingAddress : RegionBases)
    {
        CheckAccepted(runner,
                      MakeTranslation(MakeTranslationWord(0U, 0U, 1U), ValidTranslationInsertion, faultingAddress),
                      Ia64RuntimeTranslationCacheInsertionKind::Instruction,
                      "every nonzero aligned IA-64 region is accepted and forwarded exactly");
    }
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64RuntimeTranslationCacheInserterTests"};
    Asteria::Test::TestInsertionKindValidation(runner);
    Asteria::Test::TestTranslationInsertionValidation(runner);
    Asteria::Test::TestFaultingAddressValidation(runner);
    Asteria::Test::TestRequiredAndReservedTranslationBits(runner);
    Asteria::Test::TestTranslationFieldValidation(runner);
    Asteria::Test::TestDeterministicValidationOrder(runner);
    Asteria::Test::TestValidFormsAndExactForwarding(runner);
    return runner.Finish();
}
