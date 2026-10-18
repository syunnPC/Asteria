// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>

#include "Foundation/Types.hpp"
#include "Kernel/Memory/InitialMappingPlan.hpp"
#include "Platform/Ia64/Firmware/SalSystemTableParser.hpp"
#include "Platform/Ia64/Memory/InitialTranslationInstaller.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
enum class InstallEventKind : Foundation::UInt8
{
    Begin,
    PurgeCache,
    SerializeBoth,
    PurgeFirmwareItr,
    SerializeInstruction,
    WriteRegion,
    ReadRegion,
    FailStop,
    InsertData,
    InsertInstruction,
    End,
};

struct InstallEvent
{
    InstallEventKind Kind;
    Foundation::UInt8 Reserved[7];
    Foundation::UInt64 Argument1;
    Foundation::UInt64 Argument2;
    Foundation::UInt64 Argument3;
    Foundation::UInt64 Argument4;
};

constexpr Foundation::Size MaximumInstallEvents{600U};
InstallEvent g_installEvents[MaximumInstallEvents];
Foundation::Size g_installEventCount{0U};
Foundation::UInt64 g_beginStatus{0U};
Platform::Ia64::Memory::PalTranslationCachePurgePlan* g_purgeToMutate{nullptr};
Foundation::Size g_mismatchedReadRegion{~Foundation::Size{0U}};
std::jmp_buf g_failStopJump;
bool g_failStopArmed{false};
bool g_failStopObserved{false};

void RecordInstallEvent(const InstallEventKind kind, const Foundation::UInt64 argument1 = 0U,
                        const Foundation::UInt64 argument2 = 0U, const Foundation::UInt64 argument3 = 0U,
                        const Foundation::UInt64 argument4 = 0U) noexcept
{
    if (g_installEventCount < MaximumInstallEvents)
    {
        g_installEvents[g_installEventCount] = InstallEvent{
            .Kind = kind,
            .Reserved = {},
            .Argument1 = argument1,
            .Argument2 = argument2,
            .Argument3 = argument3,
            .Argument4 = argument4,
        };
    }
    ++g_installEventCount;
}
} // namespace Asteria::Test

extern "C" Asteria::Foundation::UInt64 AsteriaIa64BeginInitialTranslationInstall() noexcept
{
    Asteria::Test::RecordInstallEvent(Asteria::Test::InstallEventKind::Begin);
    if (Asteria::Test::g_purgeToMutate != nullptr)
    {
        Asteria::Test::g_purgeToMutate->BaseAddress = 0xDEAD'BEEF'0000U;
        Asteria::Test::g_purgeToMutate->LastPurgeAddress = 0xDEAD'BEEF'0000U;
    }
    return Asteria::Test::g_beginStatus;
}

extern "C" void AsteriaIa64PurgeLocalTranslationCache(const Asteria::Foundation::UInt64 address) noexcept
{
    Asteria::Test::RecordInstallEvent(Asteria::Test::InstallEventKind::PurgeCache, address);
}

extern "C" void AsteriaIa64SerializeDataAndInstruction() noexcept
{
    Asteria::Test::RecordInstallEvent(Asteria::Test::InstallEventKind::SerializeBoth);
}

extern "C" void AsteriaIa64PurgeInstructionTranslation(const Asteria::Foundation::UInt64 virtualAddress,
                                                       const Asteria::Foundation::UInt64 encodedPageSize) noexcept
{
    Asteria::Test::RecordInstallEvent(Asteria::Test::InstallEventKind::PurgeFirmwareItr, virtualAddress,
                                      encodedPageSize);
}

extern "C" void AsteriaIa64SerializeInstruction() noexcept
{
    Asteria::Test::RecordInstallEvent(Asteria::Test::InstallEventKind::SerializeInstruction);
}

extern "C" void AsteriaIa64WriteRegionRegister(const Asteria::Foundation::UInt64 virtualAddress,
                                               const Asteria::Foundation::UInt64 regionRegister) noexcept
{
    Asteria::Test::RecordInstallEvent(Asteria::Test::InstallEventKind::WriteRegion, virtualAddress, regionRegister);
}

extern "C" Asteria::Foundation::UInt64
AsteriaIa64ReadRegionRegister(const Asteria::Foundation::UInt64 virtualAddress) noexcept
{
    Asteria::Test::RecordInstallEvent(Asteria::Test::InstallEventKind::ReadRegion, virtualAddress);
    const Asteria::Foundation::UInt64 regionIndex = virtualAddress >> 61U;
    const Asteria::Foundation::UInt64 regionRegister = ((0x2000U + regionIndex) << 8U) | (12U << 2U);
    return regionIndex == Asteria::Test::g_mismatchedReadRegion ? regionRegister ^ 1U : regionRegister;
}

extern "C" [[noreturn]] void AsteriaIa64FailStopInitialTranslationInstall() noexcept
{
    Asteria::Test::RecordInstallEvent(Asteria::Test::InstallEventKind::FailStop);
    Asteria::Test::g_failStopObserved = true;
    if (Asteria::Test::g_failStopArmed)
    {
        std::longjmp(Asteria::Test::g_failStopJump, 1);
    }
    std::abort();
}

extern "C" void AsteriaIa64InsertDataTranslation(const Asteria::Foundation::UInt64 registerIndex,
                                                 const Asteria::Foundation::UInt64 translationWord,
                                                 const Asteria::Foundation::UInt64 faultingAddress,
                                                 const Asteria::Foundation::UInt64 translationInsertion) noexcept
{
    Asteria::Test::RecordInstallEvent(Asteria::Test::InstallEventKind::InsertData, registerIndex, translationWord,
                                      faultingAddress, translationInsertion);
}

extern "C" void AsteriaIa64InsertInstructionTranslation(const Asteria::Foundation::UInt64 registerIndex,
                                                        const Asteria::Foundation::UInt64 translationWord,
                                                        const Asteria::Foundation::UInt64 faultingAddress,
                                                        const Asteria::Foundation::UInt64 translationInsertion) noexcept
{
    Asteria::Test::RecordInstallEvent(Asteria::Test::InstallEventKind::InsertInstruction, registerIndex,
                                      translationWord, faultingAddress, translationInsertion);
}

extern "C" void AsteriaIa64EndInitialTranslationInstall() noexcept
{
    Asteria::Test::RecordInstallEvent(Asteria::Test::InstallEventKind::End);
}

namespace Asteria::Test
{
namespace
{
using Kernel::Memory::InitialMapping;
using Kernel::Memory::InitialMappingOwner;
using Kernel::Memory::InitialMappingPlan;
using Kernel::Memory::InitialMappingPlanError;
using Kernel::Memory::MappingPermission;
using Kernel::Memory::MappingPermissions;
using Kernel::Memory::MemoryCachePolicy;
using Kernel::Memory::PageCount;
using Kernel::Memory::PhysicalAddress;
using Kernel::Memory::VirtualAddress;
using Platform::Ia64::Firmware::SalSystemTableInformation;
using Platform::Ia64::Memory::ArchitectedMinimumInsertablePageSizeMask;
using Platform::Ia64::Memory::ArchitectedMinimumPurgePageSizeMask;
using Platform::Ia64::Memory::InitialTranslationInstallError;
using Platform::Ia64::Memory::InitialTranslationInstaller;
using Platform::Ia64::Memory::InitialTranslationRegionCount;
using Platform::Ia64::Memory::InitialTranslationRegionSet;
using Platform::Ia64::Memory::InitialTranslationResourceEntry;
using Platform::Ia64::Memory::InitialTranslationResourcePlan;
using Platform::Ia64::Memory::InitialTranslationResourcePlanError;
using Platform::Ia64::Memory::InitialTranslationResourcePlanner;
using Platform::Ia64::Memory::PalTranslationCachePurgePlan;
using Platform::Ia64::Memory::PalVirtualMemoryCapabilities;
using Platform::Ia64::Memory::RegionConfiguration;

constexpr Foundation::UInt64 VirtualBase{0x0400'0000U};
constexpr Foundation::UInt64 PhysicalBase{0x0200'0000U};
constexpr Foundation::Size PlanBytes{sizeof(InitialTranslationResourcePlan)};

constinit InitialMappingPlan g_mappingPlan;
constinit InitialTranslationResourcePlan g_resourcePlan;
constinit InitialMappingPlan g_wideRegionMappingPlan;
constinit InitialTranslationResourcePlan g_wideRegionResourcePlan;
constinit InitialMappingPlan g_wideFirmwareMappingPlan;
constinit InitialTranslationResourcePlan g_wideFirmwareResourcePlan;
constinit InitialMappingPlan g_highPhysicalMappingPlan;
constinit InitialTranslationResourcePlan g_highPhysicalResourcePlan;
constinit InitialTranslationResourcePlan g_unbuiltPlan;
Foundation::UInt8 g_planSnapshot[PlanBytes];

[[nodiscard]] constexpr MappingPermissions ReadExecute() noexcept
{
    return MappingPermissions{MappingPermission::Read}.With(MappingPermission::Execute);
}

[[nodiscard]] constexpr MappingPermissions ReadWrite() noexcept
{
    return MappingPermissions{MappingPermission::Read}.With(MappingPermission::Write);
}

[[nodiscard]] InitialMapping MakeMapping(const Foundation::UInt64 virtualStart, const Foundation::UInt64 physicalStart,
                                         const MappingPermissions permissions, const InitialMappingOwner owner) noexcept
{
    return InitialMapping{
        .VirtualStart = VirtualAddress{virtualStart},
        .PhysicalStart = PhysicalAddress{physicalStart},
        .Pages = PageCount{1U},
        .Permissions = permissions,
        .CachePolicy = MemoryCachePolicy::WriteBack,
        .Owner = owner,
        .Reserved = 0U,
    };
}

[[nodiscard]] constexpr PalVirtualMemoryCapabilities FullCapabilities() noexcept
{
    return PalVirtualMemoryCapabilities{
        .Translation =
            Platform::Ia64::Memory::TranslationCapabilities{
                .InsertablePageSizeMask = ArchitectedMinimumInsertablePageSizeMask,
                .PhysicalAddressBits = 50U,
                .ImplementedVirtualAddressMostSignificantBit = 60U,
                .RegionIdentifierBits = 24U,
                .ProtectionKeyBits = 24U,
                .MemoryAttributeMask =
                    static_cast<Foundation::UInt8>((Foundation::UInt8{1U} << 0U) | (Foundation::UInt8{1U} << 4U) |
                                                   (Foundation::UInt8{1U} << 5U) | (Foundation::UInt8{1U} << 6U)),
                .HardwareWalkerImplemented = true,
                .Reserved16 = 0U,
                .Reserved64 = 0U,
            },
        .PurgePageSizeMask = ArchitectedMinimumPurgePageSizeMask,
        .InstructionTranslationRegisterCount = 8U,
        .DataTranslationRegisterCount = 8U,
        .ProtectionKeyRegisterCount = 16U,
        .MaximumConcurrentPurgesEncoding = 1U,
        .HashTagIdentifier = 8U,
        .UniqueTranslationCacheCount = 2U,
        .TranslationCacheLevelCount = 1U,
        .Reserved8 = 0U,
        .Reserved32 = 0U,
    };
}

[[nodiscard]] constexpr SalSystemTableInformation FirmwareInformation() noexcept
{
    return SalSystemTableInformation{
        .PalProcedurePhysicalAddress = 0x0010'0000U,
        .SalProcedurePhysicalAddress = 0x0020'0000U,
        .SalGlobalDataPhysicalAddress = 0x0030'0000U,
        .TranslationRegisterVirtualAddress = 0U,
        .TranslationRegisterEncodedPageSize = 22U << 2U,
        .CoherenceDomainInformationPhysicalAddress = 0U,
        .ApWakeVector = 0xFFU,
        .TotalTableLength = 192U,
        .CoherenceDomainCount = 0U,
        .SalRevision = 0x0340U,
        .EntryCount = 3U,
        .SalAVersion = 0x0100U,
        .SalBVersion = 0x0100U,
        .MemoryDescriptorCount = 0U,
        .PlatformFeatures = 0U,
        .HasPlatformFeatures = true,
        .HasTranslationRegister = true,
        .HasPurgeCoherenceInformation = false,
        .HasApWakeVector = true,
        .Reserved8 = 0U,
    };
}

[[nodiscard]] constexpr InitialTranslationRegionSet NativeRegions() noexcept
{
    InitialTranslationRegionSet regions{};
    for (Foundation::Size index{0U}; index < InitialTranslationRegionCount; ++index)
    {
        regions.Regions[index] = RegionConfiguration{
            .RegionIdentifier = static_cast<Foundation::UInt32>(0x2000U + index),
            .ProtectionKey = 0U,
            .VhptWalkerEnabled = false,
            .SharedAcrossAddressSpaces = false,
            .Reserved16 = 0U,
            .Reserved32 = 0U,
        };
    }
    return regions;
}

[[nodiscard]] constexpr PalTranslationCachePurgePlan ValidPurgePlan() noexcept
{
    return PalTranslationCachePurgePlan{
        .BaseAddress = 0x1000U,
        .OperationCount = 6U,
        .LastPurgeAddress = 0x11A0U,
        .OuterCount = 2U,
        .InnerCount = 3U,
        .OuterStride = 0x100U,
        .InnerStride = 0x20U,
    };
}

[[nodiscard]] bool PrepareResourcePlanFixture(InitialMappingPlan& mappingPlan,
                                              InitialTranslationResourcePlan& resourcePlan,
                                              const Foundation::UInt64 physicalBase,
                                              const PalVirtualMemoryCapabilities& capabilities,
                                              const SalSystemTableInformation& firmware,
                                              const InitialTranslationRegionSet& regions) noexcept
{
    return (mappingPlan.AddMapping(MakeMapping(VirtualBase, physicalBase, ReadExecute(),
                                               InitialMappingOwner::NucleusCode)) == InitialMappingPlanError::None) &&
           (mappingPlan.AddMapping(MakeMapping(VirtualBase + 0x1000U, physicalBase + 0x1000U, ReadWrite(),
                                               InitialMappingOwner::NucleusStack)) == InitialMappingPlanError::None) &&
           (mappingPlan.AddMapping(MakeMapping(VirtualBase + 0x2000U, physicalBase + 0x2000U, ReadWrite(),
                                               InitialMappingOwner::PageTable)) == InitialMappingPlanError::None) &&
           (mappingPlan.Seal() == InitialMappingPlanError::None) &&
           InitialTranslationResourcePlanner::Build(mappingPlan, capabilities, firmware, regions, resourcePlan)
               .HasValue();
}

[[nodiscard]] bool PrepareResourcePlans() noexcept
{
    PalVirtualMemoryCapabilities wideFirmwareCapabilities = FullCapabilities();
    wideFirmwareCapabilities.Translation.InsertablePageSizeMask |= Foundation::UInt64{1U} << 32U;
    SalSystemTableInformation wideFirmware = FirmwareInformation();
    wideFirmware.TranslationRegisterEncodedPageSize = 32U << 2U;

    InitialTranslationRegionSet wideRegions = NativeRegions();
    for (Foundation::Size index{0U}; index < InitialTranslationRegionCount; ++index)
    {
        wideRegions.Regions[index].RegionIdentifier = static_cast<Foundation::UInt32>(0x40000U + index);
    }

    return PrepareResourcePlanFixture(g_mappingPlan, g_resourcePlan, PhysicalBase, FullCapabilities(),
                                      FirmwareInformation(), NativeRegions()) &&
           PrepareResourcePlanFixture(g_wideRegionMappingPlan, g_wideRegionResourcePlan, PhysicalBase,
                                      FullCapabilities(), FirmwareInformation(), wideRegions) &&
           PrepareResourcePlanFixture(g_wideFirmwareMappingPlan, g_wideFirmwareResourcePlan, PhysicalBase,
                                      wideFirmwareCapabilities, wideFirmware, NativeRegions()) &&
           PrepareResourcePlanFixture(g_highPhysicalMappingPlan, g_highPhysicalResourcePlan, 0x0000'0100'0000'0000ULL,
                                      FullCapabilities(), FirmwareInformation(), NativeRegions());
}

void ResetEvents() noexcept
{
    for (Foundation::Size index{0U}; index < MaximumInstallEvents; ++index)
    {
        g_installEvents[index] = InstallEvent{};
    }
    g_installEventCount = 0U;
    g_beginStatus = 0U;
    g_purgeToMutate = nullptr;
    g_mismatchedReadRegion = ~Foundation::Size{0U};
    g_failStopArmed = false;
    g_failStopObserved = false;
}

[[nodiscard]] bool EventEquals(const Foundation::Size index, const InstallEventKind kind,
                               const Foundation::UInt64 argument1 = 0U, const Foundation::UInt64 argument2 = 0U,
                               const Foundation::UInt64 argument3 = 0U,
                               const Foundation::UInt64 argument4 = 0U) noexcept
{
    if ((index >= g_installEventCount) || (index >= MaximumInstallEvents))
    {
        return false;
    }
    const InstallEvent& event = g_installEvents[index];
    return (event.Kind == kind) && (event.Argument1 == argument1) && (event.Argument2 == argument2) &&
           (event.Argument3 == argument3) && (event.Argument4 == argument4);
}

void TestValidationBeforeCommit(TestContext& runner) noexcept
{
    const PalVirtualMemoryCapabilities capabilities = FullCapabilities();
    PalTranslationCachePurgePlan purgePlan = ValidPurgePlan();

    ResetEvents();
    purgePlan.OperationCount = 5U;
    auto result = InitialTranslationInstaller::Install(g_resourcePlan, capabilities, purgePlan);
    ASTERIA_EXPECT(runner, "a forged PAL purge plan is rejected before the commit primitive", !result.HasValue(),
                   result.GetError().Error == InitialTranslationInstallError::InvalidPurgePlan,
                   result.GetError().Reserved == 0U, result.GetError().EntryIndex == ~Foundation::Size{0U},
                   g_installEventCount == 0U);

    ResetEvents();
    purgePlan = ValidPurgePlan();
    result = InitialTranslationInstaller::Install(g_unbuiltPlan, capabilities, purgePlan);
    ASTERIA_EXPECT(runner, "an unbuilt resource plan cannot reach privileged code", !result.HasValue(),
                   result.GetError().Error == InitialTranslationInstallError::ResourcePlanNotBuilt,
                   result.GetError().Reserved == 0U, result.GetError().EntryIndex == ~Foundation::Size{0U},
                   g_installEventCount == 0U);

    ResetEvents();
    PalVirtualMemoryCapabilities invalidCapabilities = capabilities;
    invalidCapabilities.Reserved32 = 1U;
    result = InitialTranslationInstaller::Install(g_resourcePlan, invalidCapabilities, purgePlan);
    ASTERIA_EXPECT(runner, "forged processor capabilities cannot reach privileged code", !result.HasValue(),
                   result.GetError().Error == InitialTranslationInstallError::InvalidCapabilities,
                   result.GetError().Reserved == 0U, result.GetError().EntryIndex == ~Foundation::Size{0U},
                   g_installEventCount == 0U);

    ResetEvents();
    g_beginStatus = 1U;
    result = InitialTranslationInstaller::Install(g_resourcePlan, capabilities, purgePlan);
    ASTERIA_EXPECT(runner, "an incompatible PSR state fails at the non-mutating commit boundary", !result.HasValue(),
                   result.GetError().Error == InitialTranslationInstallError::UnsupportedProcessorState,
                   result.GetError().Reserved == 0U, result.GetError().EntryIndex == ~Foundation::Size{0U},
                   g_installEventCount == 1U, EventEquals(0U, InstallEventKind::Begin));
}

void TestCapabilityNarrowingBeforeCommit(TestContext& runner) noexcept
{
    PalVirtualMemoryCapabilities capabilities = FullCapabilities();
    capabilities.Translation.RegionIdentifierBits = 18U;
    capabilities.Translation.ProtectionKeyBits = 18U;
    ResetEvents();
    auto result = InitialTranslationInstaller::Install(g_wideRegionResourcePlan, capabilities, ValidPurgePlan());
    ASTERIA_EXPECT(runner, "narrowed RID capability rejects a formerly encodable region set", !result.HasValue(),
                   result.GetError().Error == InitialTranslationInstallError::InvalidRegionConfiguration,
                   result.GetError().Reserved == 0U, result.GetError().EntryIndex == ~Foundation::Size{0U},
                   g_installEventCount == 0U);

    capabilities = FullCapabilities();
    ResetEvents();
    result = InitialTranslationInstaller::Install(g_wideFirmwareResourcePlan, capabilities, ValidPurgePlan());
    ASTERIA_EXPECT(runner, "narrowed page-size capability rejects the retained firmware ITR", !result.HasValue(),
                   result.GetError().Error == InitialTranslationInstallError::InvalidFirmwareTranslation,
                   result.GetError().Reserved == 0U, result.GetError().EntryIndex == ~Foundation::Size{0U},
                   g_installEventCount == 0U);

    capabilities = FullCapabilities();
    capabilities.Translation.PhysicalAddressBits = 32U;
    ResetEvents();
    result = InitialTranslationInstaller::Install(g_highPhysicalResourcePlan, capabilities, ValidPurgePlan());
    ASTERIA_EXPECT(runner, "narrowed physical width rejects the first unencodable resource entry", !result.HasValue(),
                   result.GetError().Error == InitialTranslationInstallError::InvalidResourceEntry,
                   result.GetError().Reserved == 0U, result.GetError().EntryIndex == 0U, g_installEventCount == 0U);
}

void TestRegionRegisterReadbackFailStop(TestContext& runner) noexcept
{
    ResetEvents();
    g_mismatchedReadRegion = 4U;
    g_failStopArmed = true;
    if (setjmp(g_failStopJump) == 0)
    {
        static_cast<void>(InitialTranslationInstaller::Install(g_resourcePlan, FullCapabilities(), ValidPurgePlan()));
        g_failStopArmed = false;
        ASTERIA_EXPECT(runner, "a mismatched region-register readback cannot return", false);
        return;
    }
    g_failStopArmed = false;
    ASTERIA_EXPECT(runner, "a region-register readback mismatch fail-stops before translation insertion",
                   g_failStopObserved, g_installEventCount == 25U,
                   EventEquals(23U, InstallEventKind::ReadRegion, Foundation::UInt64{4U} << 61U),
                   EventEquals(24U, InstallEventKind::FailStop));
}

void TestExactInstallSequence(TestContext& runner) noexcept
{
    const PalVirtualMemoryCapabilities capabilities = FullCapabilities();
    const PalTranslationCachePurgePlan purgePlan = ValidPurgePlan();
    const auto* const planBytes = reinterpret_cast<const Foundation::UInt8*>(&g_resourcePlan);
    for (Foundation::Size index{0U}; index < PlanBytes; ++index)
    {
        g_planSnapshot[index] = planBytes[index];
    }

    ResetEvents();
    const auto result = InitialTranslationInstaller::Install(g_resourcePlan, capabilities, purgePlan);
    ASTERIA_EXPECT(runner, "a valid plan returns the exact installed-resource summary", result.HasValue(),
                   result.GetValue().PurgeOperationCount == 6U, result.GetValue().EntryCount == 3U,
                   result.GetValue().DataRegisterCount == 3U, result.GetValue().InstructionRegisterCount == 2U,
                   result.GetValue().Reserved == 0U);

    Foundation::Size eventIndex{0U};
    bool exact = EventEquals(eventIndex++, InstallEventKind::Begin);
    constexpr Foundation::UInt64 purgeAddresses[]{
        0x1000U, 0x1020U, 0x1040U, 0x1160U, 0x1180U, 0x11A0U,
    };
    for (const Foundation::UInt64 address : purgeAddresses)
    {
        exact = EventEquals(eventIndex++, InstallEventKind::PurgeCache, address) && exact;
    }
    exact = EventEquals(eventIndex++, InstallEventKind::SerializeBoth) && exact;
    exact = EventEquals(eventIndex++, InstallEventKind::PurgeFirmwareItr, 0U, 22U << 2U) && exact;
    exact = EventEquals(eventIndex++, InstallEventKind::SerializeInstruction) && exact;
    for (Foundation::Size regionIndex{0U}; regionIndex < InitialTranslationRegionCount; ++regionIndex)
    {
        const auto regionRegister = g_resourcePlan.GetRegionRegister(regionIndex);
        exact = regionRegister.HasValue() &&
                EventEquals(eventIndex++, InstallEventKind::WriteRegion,
                            static_cast<Foundation::UInt64>(regionIndex) << 61U, regionRegister.GetValue()) &&
                exact;
    }
    exact = EventEquals(eventIndex++, InstallEventKind::SerializeBoth) && exact;
    for (Foundation::Size regionIndex{0U}; regionIndex < InitialTranslationRegionCount; ++regionIndex)
    {
        exact = EventEquals(eventIndex++, InstallEventKind::ReadRegion,
                            static_cast<Foundation::UInt64>(regionIndex) << 61U) &&
                exact;
    }
    for (Foundation::Size entryIndex{0U}; entryIndex < g_resourcePlan.GetEntryCount(); ++entryIndex)
    {
        const auto entryResult = g_resourcePlan.GetEntry(entryIndex);
        if (!entryResult.HasValue())
        {
            exact = false;
            continue;
        }
        const InitialTranslationResourceEntry entry = entryResult.GetValue();
        exact = EventEquals(eventIndex++, InstallEventKind::InsertData, entry.DataRegisterIndex,
                            entry.Translation.TranslationWord, entry.Translation.FaultingAddress,
                            entry.Translation.TranslationInsertion) &&
                exact;
        if (entry.HasInstructionRegister)
        {
            exact = EventEquals(eventIndex++, InstallEventKind::InsertInstruction, entry.InstructionRegisterIndex,
                                entry.Translation.TranslationWord, entry.Translation.FaultingAddress,
                                entry.Translation.TranslationInsertion) &&
                    exact;
        }
    }
    exact = EventEquals(eventIndex++, InstallEventKind::SerializeBoth) && exact;
    exact = EventEquals(eventIndex++, InstallEventKind::End) && exact;
    exact = exact && (eventIndex == g_installEventCount);
    ASTERIA_EXPECT(runner, "the privileged adapter receives the exact purge, RR, DTR, and ITR order", exact);

    bool unchanged{true};
    for (Foundation::Size index{0U}; index < PlanBytes; ++index)
    {
        unchanged = unchanged && (planBytes[index] == g_planSnapshot[index]);
    }
    ASTERIA_EXPECT(runner, "successful installation leaves the sealed plan byte-unchanged", unchanged);
}

void TestPurgeSnapshot(TestContext& runner) noexcept
{
    PalTranslationCachePurgePlan purgePlan = ValidPurgePlan();
    ResetEvents();
    g_purgeToMutate = &purgePlan;
    const auto result = InitialTranslationInstaller::Install(g_resourcePlan, FullCapabilities(), purgePlan);
    ASTERIA_EXPECT(runner, "commit uses the prevalidated PAL purge snapshot exclusively", result.HasValue(),
                   purgePlan.BaseAddress == 0xDEAD'BEEF'0000U, g_installEventCount > 1U,
                   EventEquals(1U, InstallEventKind::PurgeCache, 0x1000U));
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"InitialTranslationInstallerTests"};
    ASTERIA_EXPECT(runner, "translation-install resource fixtures build", Asteria::Test::PrepareResourcePlans());
    Asteria::Test::TestValidationBeforeCommit(runner);
    Asteria::Test::TestCapabilityNarrowingBeforeCommit(runner);
    Asteria::Test::TestRegionRegisterReadbackFailStop(runner);
    Asteria::Test::TestExactInstallSequence(runner);
    Asteria::Test::TestPurgeSnapshot(runner);
    return runner.Finish();
}
