// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_MEMORY_INITIAL_TRANSLATION_RESOURCE_PLAN_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_MEMORY_INITIAL_TRANSLATION_RESOURCE_PLAN_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Memory/InitialMappingPlan.hpp"
#include "Platform/Ia64/Firmware/SalSystemTableParser.hpp"
#include "Platform/Ia64/Memory/InitialTranslationEncoder.hpp"
#include "Platform/Ia64/Memory/PalVirtualMemoryDecoder.hpp"

namespace Asteria::Platform::Ia64::Memory
{
constexpr Foundation::Size InitialTranslationRegionCount{8U};
constexpr Foundation::Size MaximumInitialTranslationEntries{256U};

enum class InitialTranslationResourcePlanError : Foundation::UInt32
{
    None,
    OutputAlreadyBuilt,
    PlanNotSealed,
    InvalidCapabilities,
    InvalidRegionConfiguration,
    MissingFirmwareInstructionTranslationRegister,
    InvalidFirmwareInstructionTranslationRegister,
    UnimplementedFirmwareTranslationAddress,
    FirmwareInstructionTranslationRegisterOverlap,
    ActiveRegionIdentifierReservedForFirmware,
    ActiveRegionIdentifierConflict,
    DataTranslationRegisterCapacityExceeded,
    InstructionTranslationRegisterCapacityExceeded,
    TranslationEncodingFailed,
    IndexOutOfBounds,
    InternalInvariantViolation,
};

struct InitialTranslationRegionSet
{
    RegionConfiguration Regions[InitialTranslationRegionCount];
};

struct InitialTranslationResourceEntry
{
    EncodedInitialTranslation Translation;
    Foundation::UInt64 MappingPageIndex;
    Foundation::UInt16 MappingIndex;
    Foundation::UInt16 DataRegisterIndex;
    Foundation::UInt16 InstructionRegisterIndex;
    Foundation::UInt8 PageShift;
    bool HasInstructionRegister;
};

struct InitialTranslationResourcePlanSummary
{
    Foundation::UInt16 EntryCount;
    Foundation::UInt16 DataRegisterCount;
    Foundation::UInt16 InstructionRegisterCount;
    Foundation::UInt16 Reserved;
};

struct InitialTranslationResourcePlanFailure
{
    InitialTranslationResourcePlanError Error;
    InitialTranslationEncodingError EncodingError;
    Foundation::Size MappingIndex;
    Foundation::UInt64 MappingPageIndex;
};

/// Retains an immutable, bounded sequence of native translation-register writes.
/// ITR(0) remains reserved for the firmware mapping and is never assigned here.
class InitialTranslationResourcePlan final
{
public:
    constexpr InitialTranslationResourcePlan() noexcept = default;

    InitialTranslationResourcePlan(const InitialTranslationResourcePlan&) = delete;
    InitialTranslationResourcePlan& operator=(const InitialTranslationResourcePlan&) = delete;
    InitialTranslationResourcePlan(InitialTranslationResourcePlan&&) = delete;
    InitialTranslationResourcePlan& operator=(InitialTranslationResourcePlan&&) = delete;

    [[nodiscard]] Foundation::Result<InitialTranslationResourceEntry, InitialTranslationResourcePlanError>
    GetEntry(Foundation::Size index) const noexcept;

    [[nodiscard]] Foundation::Size GetEntryCount() const noexcept;
    [[nodiscard]] Foundation::UInt16 GetDataRegisterCount() const noexcept;
    /// Includes the firmware-owned ITR(0) reserved by the handoff contract.
    [[nodiscard]] Foundation::UInt16 GetInstructionRegisterCount() const noexcept;
    [[nodiscard]] Foundation::UInt64 GetFirmwareTranslationVirtualAddress() const noexcept;
    [[nodiscard]] Foundation::UInt64 GetFirmwareTranslationEncodedPageSize() const noexcept;
    [[nodiscard]] Foundation::Result<Foundation::UInt64, InitialTranslationResourcePlanError>
    GetRegionRegister(Foundation::Size regionIndex) const noexcept;
    [[nodiscard]] bool IsBuilt() const noexcept;

private:
    friend class InitialTranslationResourcePlanner;

    InitialTranslationResourceEntry m_entries[MaximumInitialTranslationEntries]{};
    Foundation::UInt64 m_firmwareTranslationVirtualAddress{0U};
    Foundation::UInt64 m_firmwareTranslationEncodedPageSize{0U};
    Foundation::UInt64 m_regionRegisters[InitialTranslationRegionCount]{};
    Foundation::Size m_entryCount{0U};
    Foundation::UInt16 m_dataRegisterCount{0U};
    Foundation::UInt16 m_instructionRegisterCount{0U};
    bool m_built{false};
    [[maybe_unused]] Foundation::UInt8 m_reserved[3]{};
};

/// Converts a sealed common mapping plan into the smallest deterministic set of
/// PAL-supported register translations. It allocates no storage and performs no
/// privileged operation. All inputs must remain stable throughout the call. The
/// instruction-register count includes the firmware-owned ITR(0). Active native
/// regions may not reuse the SAL handoff RIDs 0x1000 through 0x1003. This
/// bootstrap profile requires protection key zero and a disabled VHPT walker;
/// PKR, PTA, and VHPT ownership are established by a later activation stage.
class InitialTranslationResourcePlanner final
{
public:
    [[nodiscard]] static Foundation::Result<InitialTranslationResourcePlanSummary,
                                            InitialTranslationResourcePlanFailure>
    Build(const Kernel::Memory::InitialMappingPlan& mappings, const PalVirtualMemoryCapabilities& capabilities,
          const Firmware::SalSystemTableInformation& firmware, const InitialTranslationRegionSet& regions,
          InitialTranslationResourcePlan& output) noexcept;

private:
    [[nodiscard]] static bool
    ValidateAndMeasure(const Kernel::Memory::InitialMappingPlan& mappings,
                       const PalVirtualMemoryCapabilities& capabilities, const InitialTranslationRegionSet& regions,
                       Foundation::UInt64 firmwareTranslationLowStart, Foundation::UInt64 firmwareTranslationLowEnd,
                       Foundation::UInt32 firmwareRegionIdentifier, Foundation::Size& entryCount,
                       Foundation::UInt16& instructionRegisterCount,
                       InitialTranslationResourcePlanFailure& failure) noexcept;

    [[nodiscard]] static bool Populate(const Kernel::Memory::InitialMappingPlan& mappings,
                                       const PalVirtualMemoryCapabilities& capabilities,
                                       const InitialTranslationRegionSet& regions, Foundation::Size entryCount,
                                       Foundation::UInt16 instructionRegisterCount,
                                       InitialTranslationResourcePlan& output,
                                       InitialTranslationResourcePlanFailure& failure) noexcept;

    InitialTranslationResourcePlanner() = delete;
};

static_assert(sizeof(InitialTranslationRegionSet) == 128U);
static_assert(alignof(InitialTranslationRegionSet) == 4U);
static_assert(sizeof(InitialTranslationResourceEntry) == 48U);
static_assert(alignof(InitialTranslationResourceEntry) == 8U);
static_assert(sizeof(InitialTranslationResourcePlanSummary) == 8U);
static_assert(sizeof(InitialTranslationResourcePlanFailure) == 24U);
static_assert(alignof(InitialTranslationResourcePlanFailure) == 8U);
static_assert(sizeof(InitialTranslationResourcePlan) == 12'384U);
static_assert(alignof(InitialTranslationResourcePlan) == 8U);
static_assert(sizeof(InitialTranslationResourcePlanError) == 4U);
} // namespace Asteria::Platform::Ia64::Memory

#endif // ASTERIA_PLATFORM_IA64_MEMORY_INITIAL_TRANSLATION_RESOURCE_PLAN_HPP_INCLUDED
