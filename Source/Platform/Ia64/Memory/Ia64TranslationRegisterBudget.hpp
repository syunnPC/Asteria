// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_MEMORY_IA64_TRANSLATION_REGISTER_BUDGET_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_MEMORY_IA64_TRANSLATION_REGISTER_BUDGET_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Platform::Ia64::Memory
{
/// IA-64 guarantees eight instruction and eight data translation registers;
/// the kernel's mandatory set fits them
constexpr Foundation::UInt16 Ia64ArchitectedTranslationRegisterCount{8U};
/// The mandatory set fits the architectural 8 DTRs and 8 ITRs, with
/// ITR(0) for the firmware included. A
/// processor reporting more registers does not raise it.
constexpr Foundation::UInt16 Ia64MandatoryDataTranslationRegisterBudget{8U};
constexpr Foundation::UInt16 Ia64MandatoryInstructionTranslationRegisterBudget{8U};

static_assert(Ia64MandatoryDataTranslationRegisterBudget >= Ia64ArchitectedTranslationRegisterCount);
static_assert(Ia64MandatoryInstructionTranslationRegisterBudget >= Ia64ArchitectedTranslationRegisterCount);

enum class Ia64TranslationRegisterBudgetError : Foundation::UInt32
{
    None,
    CapacityBelowArchitectedMinimum,
    DataRegistersOverBudget,
    InstructionRegistersOverBudget,
    DataRegistersOverCapacity,
    InstructionRegistersOverCapacity,
};

/// The boot's check of its translation-register plan against the processor's
/// PAL-reported counts and the mandatory budget. Spare registers are the ones
/// the processor has beyond the plan (for an opportunistic allocator that
/// correctness never relies on); WithinArchitectedMinimum records whether the
/// plan already fits the eight-plus-eight guarantee.
struct Ia64TranslationRegisterBudgetCheck
{
    Ia64TranslationRegisterBudgetError Error;
    Foundation::UInt16 SpareDataRegisters;
    Foundation::UInt16 SpareInstructionRegisters;
    bool WithinArchitectedMinimum;
};

/// Planned* count the plan's registers (the instruction count includes the firmware's ITR(0));
/// *Capacity are PAL_VM_SUMMARY's counts. The plan must fit both the processor and the mandatory
/// budget, and the processor must report at least the architected eight of each kind.
[[nodiscard]] constexpr Ia64TranslationRegisterBudgetCheck
CheckIa64TranslationRegisterBudget(const Foundation::UInt16 dataCapacity, const Foundation::UInt16 instructionCapacity,
                                   const Foundation::UInt16 plannedData,
                                   const Foundation::UInt16 plannedInstruction) noexcept
{
    Ia64TranslationRegisterBudgetCheck check{
        .Error = Ia64TranslationRegisterBudgetError::None,
        .SpareDataRegisters = 0U,
        .SpareInstructionRegisters = 0U,
        .WithinArchitectedMinimum = false,
    };
    if ((dataCapacity < Ia64ArchitectedTranslationRegisterCount) ||
        (instructionCapacity < Ia64ArchitectedTranslationRegisterCount))
    {
        check.Error = Ia64TranslationRegisterBudgetError::CapacityBelowArchitectedMinimum;
        return check;
    }
    if (plannedData > Ia64MandatoryDataTranslationRegisterBudget)
    {
        check.Error = Ia64TranslationRegisterBudgetError::DataRegistersOverBudget;
        return check;
    }
    if (plannedInstruction > Ia64MandatoryInstructionTranslationRegisterBudget)
    {
        check.Error = Ia64TranslationRegisterBudgetError::InstructionRegistersOverBudget;
        return check;
    }
    if (plannedData > dataCapacity)
    {
        check.Error = Ia64TranslationRegisterBudgetError::DataRegistersOverCapacity;
        return check;
    }
    if (plannedInstruction > instructionCapacity)
    {
        check.Error = Ia64TranslationRegisterBudgetError::InstructionRegistersOverCapacity;
        return check;
    }
    check.SpareDataRegisters = static_cast<Foundation::UInt16>(dataCapacity - plannedData);
    check.SpareInstructionRegisters = static_cast<Foundation::UInt16>(instructionCapacity - plannedInstruction);
    check.WithinArchitectedMinimum = (plannedData <= Ia64ArchitectedTranslationRegisterCount) &&
                                     (plannedInstruction <= Ia64ArchitectedTranslationRegisterCount);
    return check;
}

static_assert(sizeof(Ia64TranslationRegisterBudgetError) == 4U);
static_assert(sizeof(Ia64TranslationRegisterBudgetCheck) == 12U);
} // Namespace Asteria::Platform::Ia64::Memory

#endif // ASTERIA_PLATFORM_IA64_MEMORY_IA64_TRANSLATION_REGISTER_BUDGET_HPP_INCLUDED
