// SPDX-License-Identifier: Apache-2.0
#include "Foundation/Types.hpp"
#include "Platform/Ia64/Memory/Ia64TranslationRegisterBudget.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Platform::Ia64::Memory::CheckIa64TranslationRegisterBudget;
using Platform::Ia64::Memory::Ia64ArchitectedTranslationRegisterCount;
using Platform::Ia64::Memory::Ia64MandatoryDataTranslationRegisterBudget;
using Platform::Ia64::Memory::Ia64MandatoryInstructionTranslationRegisterBudget;
using Platform::Ia64::Memory::Ia64TranslationRegisterBudgetCheck;
using Platform::Ia64::Memory::Ia64TranslationRegisterBudgetError;

constexpr Foundation::UInt16 Montecito{32U};
constexpr Foundation::UInt16 Architected{Ia64ArchitectedTranslationRegisterCount};

void TestFittingPlans(TestContext& runner) noexcept
{
    constexpr Ia64TranslationRegisterBudgetCheck Today =
        CheckIa64TranslationRegisterBudget(Montecito, Montecito, 4U, 2U);
    constexpr Ia64TranslationRegisterBudgetCheck Full =
        CheckIa64TranslationRegisterBudget(Montecito, Montecito, Ia64MandatoryDataTranslationRegisterBudget,
                                           Ia64MandatoryInstructionTranslationRegisterBudget);
    constexpr Ia64TranslationRegisterBudgetCheck Target =
        CheckIa64TranslationRegisterBudget(Architected, Architected, Architected, Architected);
    constexpr Ia64TranslationRegisterBudgetCheck LargerProcessor = CheckIa64TranslationRegisterBudget(64U, 48U, 8U, 8U);
    ASTERIA_EXPECT(runner, "a plan within the budget and the processor's counts passes with its spare registers",
                   Today.Error == Ia64TranslationRegisterBudgetError::None, Today.SpareDataRegisters == 28U,
                   Today.SpareInstructionRegisters == 30U, Today.WithinArchitectedMinimum,
                   Full.Error == Ia64TranslationRegisterBudgetError::None, Full.SpareDataRegisters == 24U,
                   Full.SpareInstructionRegisters == 24U, Target.Error == Ia64TranslationRegisterBudgetError::None,
                   Target.WithinArchitectedMinimum, Target.SpareDataRegisters == 0U,
                   LargerProcessor.Error == Ia64TranslationRegisterBudgetError::None,
                   LargerProcessor.SpareDataRegisters == 56U, LargerProcessor.SpareInstructionRegisters == 40U,
                   LargerProcessor.WithinArchitectedMinimum);
}

void TestRejectedPlans(TestContext& runner) noexcept
{
    constexpr Ia64TranslationRegisterBudgetCheck SmallDataCapacity =
        CheckIa64TranslationRegisterBudget(Architected - 1U, Montecito, 4U, 4U);
    constexpr Ia64TranslationRegisterBudgetCheck SmallInstructionCapacity =
        CheckIa64TranslationRegisterBudget(Montecito, Architected - 1U, 4U, 4U);
    // A processor with more registers does not raise the budget.
    constexpr Ia64TranslationRegisterBudgetCheck DataOverBudget =
        CheckIa64TranslationRegisterBudget(64U, 64U, Ia64MandatoryDataTranslationRegisterBudget + 1U, 4U);
    constexpr Ia64TranslationRegisterBudgetCheck InstructionOverBudget =
        CheckIa64TranslationRegisterBudget(64U, 64U, 4U, Ia64MandatoryInstructionTranslationRegisterBudget + 1U);
    constexpr Ia64TranslationRegisterBudgetCheck DataOverCapacity =
        CheckIa64TranslationRegisterBudget(Architected, Montecito, Architected + 1U, 4U);
    constexpr Ia64TranslationRegisterBudgetCheck InstructionOverCapacity =
        CheckIa64TranslationRegisterBudget(Montecito, Architected, 4U, Architected + 1U);
    ASTERIA_EXPECT(
        runner,
        "the check rejects a processor below eight of either kind, a plan over the budget and one over capacity",
        SmallDataCapacity.Error == Ia64TranslationRegisterBudgetError::CapacityBelowArchitectedMinimum,
        SmallInstructionCapacity.Error == Ia64TranslationRegisterBudgetError::CapacityBelowArchitectedMinimum,
        DataOverBudget.Error == Ia64TranslationRegisterBudgetError::DataRegistersOverBudget,
        InstructionOverBudget.Error == Ia64TranslationRegisterBudgetError::InstructionRegistersOverBudget,
        DataOverCapacity.Error == Ia64TranslationRegisterBudgetError::DataRegistersOverBudget,
        InstructionOverCapacity.Error == Ia64TranslationRegisterBudgetError::InstructionRegistersOverBudget,
        DataOverCapacity.SpareDataRegisters == 0U, !DataOverCapacity.WithinArchitectedMinimum);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64TranslationRegisterBudgetTests"};
    Asteria::Test::TestFittingPlans(runner);
    Asteria::Test::TestRejectedPlans(runner);
    return runner.Finish();
}
