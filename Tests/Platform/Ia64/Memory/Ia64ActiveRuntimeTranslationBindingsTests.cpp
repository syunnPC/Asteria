// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Foundation/Types.hpp"
#include "Kernel/Memory/AddressSpace.hpp"
#include "Kernel/Memory/FaultFrameMapping.hpp"
#include "Platform/Ia64/Memory/Ia64ActiveRuntimeTranslationBindings.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeRegionSet.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeTranslationLedger.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
Foundation::Size g_activationCallCount{0U};
Foundation::UInt64 g_activationStatus{0U};
} // namespace Asteria::Test

extern "C" Asteria::Foundation::UInt64
AsteriaIa64ActivateRuntimeRegions(Asteria::Foundation::UInt64, Asteria::Foundation::UInt64, Asteria::Foundation::UInt64,
                                  Asteria::Foundation::UInt64, Asteria::Foundation::UInt64, Asteria::Foundation::UInt64,
                                  Asteria::Foundation::UInt64) noexcept
{
    ++Asteria::Test::g_activationCallCount;
    return Asteria::Test::g_activationStatus;
}

namespace Asteria::Test
{
namespace
{
using Kernel::Memory::AddressSpaceMappingRecord;
using Kernel::Memory::FrameKind;
using Kernel::Memory::MemoryCachePolicy;
using Kernel::Memory::PageCount;
using Kernel::Memory::PagePermission;
using Kernel::Memory::PagePermissions;
using Kernel::Memory::PhysicalAddress;
using Kernel::Memory::VirtualAddress;
using Kernel::Scheduler::CpuId;
using Platform::Ia64::Memory::Ia64ActiveRuntimeTranslationBindingError;
using Platform::Ia64::Memory::Ia64ActiveRuntimeTranslationBindings;
using Platform::Ia64::Memory::Ia64ActiveRuntimeTranslationBindingsWireStorage;
using Platform::Ia64::Memory::Ia64BootstrapPreferredPageShift;
using Platform::Ia64::Memory::Ia64BootstrapRegionRegisterWords;
using Platform::Ia64::Memory::Ia64RuntimeRegionCount;
using Platform::Ia64::Memory::Ia64RuntimeRegionSet;
using Platform::Ia64::Memory::Ia64RuntimeRegionSetError;
using Platform::Ia64::Memory::Ia64RuntimeTranslationEncodingError;
using Platform::Ia64::Memory::Ia64RuntimeTranslationLedger;
using Platform::Ia64::Memory::Ia64RuntimeTranslationLedgerError;
using Platform::Ia64::Memory::Ia64RuntimeTranslationLedgerFailure;
using Platform::Ia64::Memory::Ia64SharedBootstrapRegionIdentifier;
using Platform::Ia64::Memory::TranslationCapabilities;

constexpr Foundation::UInt64 AdapterSuccess{0U};
constexpr Foundation::UInt64 AdapterRejectedBeforeCommit{1U};
constexpr Foundation::UInt64 AdapterCommittedStateUnknown{2U};
constexpr Foundation::UInt64 RegionFiveBase{0xA000'0000'0010'0000ULL};
constexpr Foundation::UInt64 PhysicalBase{0x0000'0000'0020'0000ULL};

constinit Ia64ActiveRuntimeTranslationBindings g_constinitBindings;
constinit Ia64RuntimeTranslationLedger g_constinitLedger;

static_assert(sizeof(Ia64ActiveRuntimeTranslationBindingsWireStorage) == 56U);
static_assert(offsetof(Ia64ActiveRuntimeTranslationBindingsWireStorage, Slots) == 0U);
static_assert(offsetof(Ia64ActiveRuntimeTranslationBindingsWireStorage, RuntimeObjectStart) == 32U);
static_assert(offsetof(Ia64ActiveRuntimeTranslationBindingsWireStorage, RuntimeObjectEnd) == 40U);
static_assert(offsetof(Ia64ActiveRuntimeTranslationBindingsWireStorage, AdmissionOwnerAddress) == 48U);

[[nodiscard]] constexpr TranslationCapabilities FullCapabilities() noexcept
{
    return TranslationCapabilities{
        .InsertablePageSizeMask = Foundation::UInt64{1U} << 12U,
        .PhysicalAddressBits = 50U,
        .ImplementedVirtualAddressMostSignificantBit = 60U,
        .RegionIdentifierBits = 24U,
        .ProtectionKeyBits = 24U,
        .MemoryAttributeMask = static_cast<Foundation::UInt8>(
            (Foundation::UInt8{1U} << 0U) | (Foundation::UInt8{1U} << 4U) | (Foundation::UInt8{1U} << 5U) |
            (Foundation::UInt8{1U} << 6U) | (Foundation::UInt8{1U} << 7U)),
        .HardwareWalkerImplemented = true,
        .Reserved16 = 0U,
        .Reserved64 = 0U,
    };
}

[[nodiscard]] constexpr TranslationCapabilities RegionCapabilities() noexcept
{
    TranslationCapabilities capabilities{FullCapabilities()};
    capabilities.PhysicalAddressBits = 0U;
    capabilities.ImplementedVirtualAddressMostSignificantBit = 0U;
    capabilities.ProtectionKeyBits = 0U;
    capabilities.MemoryAttributeMask = 0U;
    capabilities.HardwareWalkerImplemented = false;
    return capabilities;
}

[[nodiscard]] constexpr Foundation::UInt64 RegionRegisterWord(const Foundation::UInt32 regionIdentifier) noexcept
{
    return (static_cast<Foundation::UInt64>(regionIdentifier) << 8U) |
           (static_cast<Foundation::UInt64>(Ia64BootstrapPreferredPageShift) << 2U);
}

[[nodiscard]] constexpr Ia64BootstrapRegionRegisterWords BootstrapWords() noexcept
{
    Ia64BootstrapRegionRegisterWords words{};
    for (Foundation::Size index{0U}; index < Ia64RuntimeRegionCount; ++index)
    {
        words.RegionRegisters[index] =
            RegionRegisterWord(Ia64SharedBootstrapRegionIdentifier + static_cast<Foundation::UInt32>(index));
    }
    return words;
}

[[nodiscard]] bool BuildRegionSet(Ia64RuntimeRegionSet& output) noexcept
{
    return Ia64RuntimeRegionSet::AdoptBootstrap(BootstrapWords(), RegionCapabilities(), output) ==
           Ia64RuntimeRegionSetError::None;
}

[[nodiscard]] constexpr AddressSpaceMappingRecord MappingPlan() noexcept
{
    return AddressSpaceMappingRecord{
        .MemoryObject = {},
        .DeviceRegion = {},
        .Frame = {},
        .VirtualStart = VirtualAddress{RegionFiveBase},
        .PhysicalStart = PhysicalAddress{PhysicalBase},
        .Pages = PageCount{1U},
        .MemoryObjectOffsetPages = 0U,
        .Permissions = PagePermissions{PagePermission::Read},
        .CachePolicy = MemoryCachePolicy::WriteBack,
        .Kind = FrameKind::Ram,
        .Commitment = {},
        .RuntimeMapping = true,
        .Reserved = {},
    };
}

[[nodiscard]] constexpr bool LedgerOperationSucceeded(const Ia64RuntimeTranslationLedgerFailure failure) noexcept
{
    return (failure.Code == Ia64RuntimeTranslationLedgerError::None) && (failure.Reserved[0U] == 0U) &&
           (failure.Reserved[1U] == 0U) && (failure.Reserved[2U] == 0U) &&
           (failure.EncodingError == Ia64RuntimeTranslationEncodingError::None);
}

[[nodiscard]] bool InitializeLedger(Ia64RuntimeTranslationLedger& ledger) noexcept
{
    return LedgerOperationSucceeded(ledger.Initialize(FullCapabilities())) &&
           LedgerOperationSucceeded(ledger.Append(MappingPlan()));
}

template <typename... Objects>
void ConfigureInstalledBindingFixture(Ia64ActiveRuntimeTranslationBindings& bindings, const CpuId processor,
                                      const Objects&... objects) noexcept
{
    Foundation::UInt64 start = static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(&bindings));
    Foundation::UInt64 end = start + sizeof(bindings);
    const auto include = [&start, &end](const auto& object) noexcept
    {
        const Foundation::UInt64 objectStart =
            static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(&object));
        const Foundation::UInt64 objectEnd = objectStart + sizeof(object);
        start = objectStart < start ? objectStart : start;
        end = objectEnd > end ? objectEnd : end;
    };
    (include(objects), ...);
    constexpr Foundation::UInt64 PageMask{4095U};
    start &= ~PageMask;
    end = (end + PageMask) & ~PageMask;

    // This test consumes the explicitly named assembly wire ABI as an input
    // fixture. Production admission remains private to the installed IVT
    // owner and is exercised by Ia64RuntimeInterruptVectorTableTests.
    auto* const wire = reinterpret_cast<Ia64ActiveRuntimeTranslationBindingsWireStorage*>(&bindings);
    wire->RuntimeObjectStart = start;
    wire->RuntimeObjectEnd = end;
    wire->AdmissionOwnerAddress = static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(&bindings));
    wire->Slots[processor.GetValue()].StoreRelease(1U);
}

void ResetAdapter(const Foundation::UInt64 status) noexcept
{
    g_activationCallCount = 0U;
    g_activationStatus = status;
}

[[nodiscard]] bool HasError(
    const Foundation::Result<const Ia64RuntimeTranslationLedger*, Ia64ActiveRuntimeTranslationBindingError>& result,
    const Ia64ActiveRuntimeTranslationBindingError error) noexcept
{
    return !result.HasValue() && (result.GetError() == error);
}

void TestInvalidProcessorsAndConstinit(TestContext& runner) noexcept
{
    const CpuId invalidDefault{};
    const CpuId invalidFour{4U};
    Ia64RuntimeRegionSet regionSet;
    const bool fixturesReady = BuildRegionSet(regionSet) && InitializeLedger(g_constinitLedger);

    ResetAdapter(AdapterSuccess);
    ASTERIA_EXPECT(runner, "default and value-four CPU IDs cannot reach activation or its slot", fixturesReady,
                   g_constinitBindings.ActivateAndBind(invalidDefault, regionSet, g_constinitLedger) ==
                       Ia64ActiveRuntimeTranslationBindingError::InvalidProcessor,
                   g_constinitBindings.ActivateAndBind(invalidFour, regionSet, g_constinitLedger) ==
                       Ia64ActiveRuntimeTranslationBindingError::InvalidProcessor,
                   g_activationCallCount == 0U);

    ASTERIA_EXPECT(runner, "default and value-four CPU IDs are safe acquire failures",
                   HasError(g_constinitBindings.AcquireActiveLedger(invalidDefault),
                            Ia64ActiveRuntimeTranslationBindingError::InvalidProcessor),
                   HasError(g_constinitBindings.AcquireActiveLedger(invalidFour),
                            Ia64ActiveRuntimeTranslationBindingError::InvalidProcessor));

    ASTERIA_EXPECT(runner, "default and value-four CPU IDs are rejected before clear indexing",
                   g_constinitBindings.ClearActiveLedger(invalidDefault, g_constinitLedger) ==
                       Ia64ActiveRuntimeTranslationBindingError::InvalidProcessor,
                   g_constinitBindings.ClearActiveLedger(invalidFour, g_constinitLedger) ==
                       Ia64ActiveRuntimeTranslationBindingError::InvalidProcessor);

    ASTERIA_EXPECT(runner, "constinit zero storage begins with CPU zero unadmitted",
                   HasError(g_constinitBindings.AcquireActiveLedger(CpuId{0U}),
                            Ia64ActiveRuntimeTranslationBindingError::ProcessorNotAdmitted));
    ConfigureInstalledBindingFixture(g_constinitBindings, CpuId{0U}, regionSet, g_constinitLedger);
    ASTERIA_EXPECT(runner, "the installed-owner wire fixture publishes only sentinel one",
                   HasError(g_constinitBindings.AcquireActiveLedger(CpuId{0U}),
                            Ia64ActiveRuntimeTranslationBindingError::NoActiveLedger));

    ResetAdapter(AdapterSuccess);
    const auto activation = g_constinitBindings.ActivateAndBind(CpuId{0U}, regionSet, g_constinitLedger);
    const auto acquired = g_constinitBindings.AcquireActiveLedger(CpuId{0U});
    const bool visibleLedgerData = acquired.HasValue() && (acquired.GetValue() == &g_constinitLedger) &&
                                   acquired.GetValue()->Lookup(RegionFiveBase).HasValue();
    ASTERIA_EXPECT(runner, "successful release publication exposes the exact initialized ledger",
                   activation == Ia64ActiveRuntimeTranslationBindingError::None, g_activationCallCount == 1U,
                   visibleLedgerData);
    ASTERIA_EXPECT(runner, "constinit binding is quiesced and cleared before ledger destruction",
                   g_constinitBindings.ClearActiveLedger(CpuId{0U}, g_constinitLedger) ==
                       Ia64ActiveRuntimeTranslationBindingError::None,
                   !g_constinitBindings.IsBound(g_constinitLedger));
}

void TestAdmissionAndUnadmittedActivation(TestContext& runner) noexcept
{
    Ia64ActiveRuntimeTranslationBindings bindings;
    Ia64RuntimeRegionSet regionSet;
    Ia64RuntimeTranslationLedger ledger;
    const bool fixturesReady = BuildRegionSet(regionSet) && InitializeLedger(ledger);

    ConfigureInstalledBindingFixture(bindings, CpuId{0U}, regionSet, ledger);
    ConfigureInstalledBindingFixture(bindings, CpuId{3U}, regionSet, ledger);
    ASTERIA_EXPECT(
        runner, "installed-owner wire input admits CPU zero and CPU three as empty",
        HasError(bindings.AcquireActiveLedger(CpuId{0U}), Ia64ActiveRuntimeTranslationBindingError::NoActiveLedger),
        HasError(bindings.AcquireActiveLedger(CpuId{3U}), Ia64ActiveRuntimeTranslationBindingError::NoActiveLedger));

    ResetAdapter(AdapterSuccess);
    ASTERIA_EXPECT(runner, "an unadmitted CPU cannot call the native helper or gain a binding", fixturesReady,
                   bindings.ActivateAndBind(CpuId{1U}, regionSet, ledger) ==
                       Ia64ActiveRuntimeTranslationBindingError::ProcessorNotAdmitted,
                   g_activationCallCount == 0U,
                   HasError(bindings.AcquireActiveLedger(CpuId{1U}),
                            Ia64ActiveRuntimeTranslationBindingError::ProcessorNotAdmitted));
    ASTERIA_EXPECT(runner, "clear reports an unadmitted slot without retaining the ledger",
                   bindings.ClearActiveLedger(CpuId{1U}, ledger) ==
                       Ia64ActiveRuntimeTranslationBindingError::ProcessorNotAdmitted,
                   !bindings.IsBound(ledger));
}

void TestActivationTransitionsAndClear(TestContext& runner) noexcept
{
    Ia64ActiveRuntimeTranslationBindings bindings;
    Ia64RuntimeRegionSet regionSet;
    Ia64RuntimeRegionSet invalidRegionSet;
    Ia64RuntimeTranslationLedger firstLedger;
    Ia64RuntimeTranslationLedger secondLedger;
    const bool fixturesReady =
        BuildRegionSet(regionSet) && InitializeLedger(firstLedger) && InitializeLedger(secondLedger);
    ConfigureInstalledBindingFixture(bindings, CpuId{0U}, regionSet, firstLedger, secondLedger);

    ResetAdapter(AdapterSuccess);
    const auto firstActivation = bindings.ActivateAndBind(CpuId{0U}, regionSet, firstLedger);
    const auto firstAcquire = bindings.AcquireActiveLedger(CpuId{0U});
    ASTERIA_EXPECT(runner, "exact success binds from sentinel one to the first ledger", fixturesReady,
                   firstActivation == Ia64ActiveRuntimeTranslationBindingError::None, firstAcquire.HasValue(),
                   firstAcquire.GetValue() == &firstLedger);

    ResetAdapter(AdapterSuccess);
    const auto replacement = bindings.ActivateAndBind(CpuId{0U}, regionSet, secondLedger);
    const auto replacementAcquire = bindings.AcquireActiveLedger(CpuId{0U});
    ASTERIA_EXPECT(runner, "a second exact success replaces the prior pointer truthfully",
                   replacement == Ia64ActiveRuntimeTranslationBindingError::None, replacementAcquire.HasValue(),
                   replacementAcquire.GetValue() == &secondLedger, !bindings.IsBound(firstLedger),
                   bindings.IsBound(secondLedger));

    ResetAdapter(AdapterSuccess);
    const auto invalidSet = bindings.ActivateAndBind(CpuId{0U}, invalidRegionSet, firstLedger);
    const auto afterInvalidSet = bindings.AcquireActiveLedger(CpuId{0U});
    ASTERIA_EXPECT(runner, "invalid region-set validation preserves the old pointer",
                   invalidSet == Ia64ActiveRuntimeTranslationBindingError::InvalidRegionSet,
                   g_activationCallCount == 0U, afterInvalidSet.HasValue(),
                   afterInvalidSet.GetValue() == &secondLedger);

    ResetAdapter(AdapterRejectedBeforeCommit);
    const auto rejected = bindings.ActivateAndBind(CpuId{0U}, regionSet, firstLedger);
    const auto afterRejected = bindings.AcquireActiveLedger(CpuId{0U});
    ASTERIA_EXPECT(runner, "native precommit rejection preserves the old pointer",
                   rejected == Ia64ActiveRuntimeTranslationBindingError::ActivationRejectedBeforeCommit,
                   g_activationCallCount == 1U, afterRejected.HasValue(), afterRejected.GetValue() == &secondLedger);

    ResetAdapter(AdapterCommittedStateUnknown);
    const auto uncertain = bindings.ActivateAndBind(CpuId{0U}, regionSet, firstLedger);
    ASTERIA_EXPECT(
        runner, "committed-state uncertainty clears the old pointer to sentinel one",
        uncertain == Ia64ActiveRuntimeTranslationBindingError::ActivationStateUnknown,
        HasError(bindings.AcquireActiveLedger(CpuId{0U}), Ia64ActiveRuntimeTranslationBindingError::NoActiveLedger),
        !bindings.IsBound(firstLedger), !bindings.IsBound(secondLedger));

    ResetAdapter(AdapterSuccess);
    const bool rebound =
        bindings.ActivateAndBind(CpuId{0U}, regionSet, secondLedger) == Ia64ActiveRuntimeTranslationBindingError::None;
    ResetAdapter(0xFFFFU);
    const auto unknownStatus = bindings.ActivateAndBind(CpuId{0U}, regionSet, firstLedger);
    ASTERIA_EXPECT(
        runner, "an unknown native status conservatively clears the old pointer", rebound,
        unknownStatus == Ia64ActiveRuntimeTranslationBindingError::ActivationStateUnknown,
        HasError(bindings.AcquireActiveLedger(CpuId{0U}), Ia64ActiveRuntimeTranslationBindingError::NoActiveLedger),
        !bindings.IsBound(secondLedger));

    ResetAdapter(AdapterSuccess);
    const bool boundForClear =
        bindings.ActivateAndBind(CpuId{0U}, regionSet, firstLedger) == Ia64ActiveRuntimeTranslationBindingError::None;
    const auto mismatchedClear = bindings.ClearActiveLedger(CpuId{0U}, secondLedger);
    const auto afterMismatch = bindings.AcquireActiveLedger(CpuId{0U});
    ASTERIA_EXPECT(runner, "mismatched clear preserves the exact active pointer", boundForClear,
                   mismatchedClear == Ia64ActiveRuntimeTranslationBindingError::ActiveLedgerMismatch,
                   afterMismatch.HasValue(), afterMismatch.GetValue() == &firstLedger);
    ASTERIA_EXPECT(
        runner, "exact clear leaves the admitted slot with neither ledger pointer bound",
        bindings.ClearActiveLedger(CpuId{0U}, firstLedger) == Ia64ActiveRuntimeTranslationBindingError::None,
        HasError(bindings.AcquireActiveLedger(CpuId{0U}), Ia64ActiveRuntimeTranslationBindingError::NoActiveLedger),
        !bindings.IsBound(firstLedger), !bindings.IsBound(secondLedger));
}

void TestPhysicalBootstrapActivation(TestContext& runner) noexcept
{
    Ia64ActiveRuntimeTranslationBindings bindings;
    Ia64RuntimeRegionSet regionSet;
    Ia64RuntimeRegionSet invalidRegionSet;
    Ia64RuntimeTranslationLedger firstLedger;
    Ia64RuntimeTranslationLedger secondLedger;
    const bool fixturesReady =
        BuildRegionSet(regionSet) && InitializeLedger(firstLedger) && InitializeLedger(secondLedger);

    ResetAdapter(AdapterSuccess);
    ASTERIA_EXPECT(runner, "physical bootstrap rejects invalid and unadmitted CPUs before native state", fixturesReady,
                   bindings.ActivateAndBindPhysicalBootstrap(CpuId{}, regionSet, firstLedger) ==
                       Ia64ActiveRuntimeTranslationBindingError::InvalidProcessor,
                   bindings.ActivateAndBindPhysicalBootstrap(CpuId{1U}, regionSet, firstLedger) ==
                       Ia64ActiveRuntimeTranslationBindingError::ProcessorNotAdmitted,
                   g_activationCallCount == 0U);

    ConfigureInstalledBindingFixture(bindings, CpuId{2U}, regionSet, invalidRegionSet, firstLedger, secondLedger);

    ResetAdapter(AdapterSuccess);
    const auto invalid = bindings.ActivateAndBindPhysicalBootstrap(CpuId{2U}, invalidRegionSet, firstLedger);
    ASTERIA_EXPECT(
        runner, "physical bootstrap validates the RegionSet before activation or publication",
        invalid == Ia64ActiveRuntimeTranslationBindingError::InvalidRegionSet, g_activationCallCount == 0U,
        HasError(bindings.AcquireActiveLedger(CpuId{2U}), Ia64ActiveRuntimeTranslationBindingError::NoActiveLedger));

    ResetAdapter(AdapterRejectedBeforeCommit);
    const auto rejected = bindings.ActivateAndBindPhysicalBootstrap(CpuId{2U}, regionSet, firstLedger);
    ASTERIA_EXPECT(
        runner, "physical precommit rejection preserves the admitted-empty sentinel",
        rejected == Ia64ActiveRuntimeTranslationBindingError::ActivationRejectedBeforeCommit,
        g_activationCallCount == 1U,
        HasError(bindings.AcquireActiveLedger(CpuId{2U}), Ia64ActiveRuntimeTranslationBindingError::NoActiveLedger));

    ResetAdapter(AdapterSuccess);
    const auto activation = bindings.ActivateAndBindPhysicalBootstrap(CpuId{2U}, regionSet, firstLedger);
    const auto active = bindings.AcquireActiveLedger(CpuId{2U});
    ASTERIA_EXPECT(runner, "exact physical activation release-publishes the target ledger",
                   activation == Ia64ActiveRuntimeTranslationBindingError::None, g_activationCallCount == 1U,
                   active.HasValue(), active.GetValue() == &firstLedger);

    ResetAdapter(AdapterSuccess);
    const auto repeated = bindings.ActivateAndBindPhysicalBootstrap(CpuId{2U}, regionSet, secondLedger);
    const auto afterRepeat = bindings.AcquireActiveLedger(CpuId{2U});
    ASTERIA_EXPECT(runner, "a repeated physical bootstrap cannot replace or reactivate a ledger",
                   repeated == Ia64ActiveRuntimeTranslationBindingError::ActiveLedgerMismatch,
                   g_activationCallCount == 0U, afterRepeat.HasValue(), afterRepeat.GetValue() == &firstLedger);

    ResetAdapter(AdapterSuccess);
    const auto translatedReplacement = bindings.ActivateAndBind(CpuId{2U}, regionSet, secondLedger);
    const auto afterTranslatedReplacement = bindings.AcquireActiveLedger(CpuId{2U});
    ASTERIA_EXPECT(runner, "ordinary translated activation retains its existing replacement semantics",
                   translatedReplacement == Ia64ActiveRuntimeTranslationBindingError::None, g_activationCallCount == 1U,
                   afterTranslatedReplacement.HasValue(), afterTranslatedReplacement.GetValue() == &secondLedger);

    const bool cleared =
        bindings.ClearActiveLedger(CpuId{2U}, secondLedger) == Ia64ActiveRuntimeTranslationBindingError::None;
    Ia64ActiveRuntimeTranslationBindings uncertaintyBindings;
    ConfigureInstalledBindingFixture(uncertaintyBindings, CpuId{2U}, regionSet, firstLedger, secondLedger);
    ResetAdapter(AdapterCommittedStateUnknown);
    const auto uncertain = uncertaintyBindings.ActivateAndBindPhysicalBootstrap(CpuId{2U}, regionSet, firstLedger);
    ASTERIA_EXPECT(runner, "physical committed uncertainty publishes no ledger and retains fail-stop ambiguity",
                   cleared, uncertain == Ia64ActiveRuntimeTranslationBindingError::ActivationStateUnknown,
                   g_activationCallCount == 1U,
                   HasError(uncertaintyBindings.AcquireActiveLedger(CpuId{2U}),
                            Ia64ActiveRuntimeTranslationBindingError::NoActiveLedger),
                   !uncertaintyBindings.IsBound(firstLedger), !uncertaintyBindings.IsBound(secondLedger),
                   !bindings.IsBound(firstLedger), !bindings.IsBound(secondLedger));
}

void TestAllFourSlots(TestContext& runner) noexcept
{
    Ia64ActiveRuntimeTranslationBindings bindings;
    Ia64RuntimeRegionSet regionSet;
    Ia64RuntimeTranslationLedger ledger;
    bool prepared = BuildRegionSet(regionSet) && InitializeLedger(ledger);
    for (Foundation::UInt8 index{0U}; index < Kernel::Scheduler::MaximumProcessorCount; ++index)
    {
        ConfigureInstalledBindingFixture(bindings, CpuId{index}, regionSet, ledger);
        ResetAdapter(AdapterSuccess);
        prepared = prepared && (bindings.ActivateAndBind(CpuId{index}, regionSet, ledger) ==
                                Ia64ActiveRuntimeTranslationBindingError::None);
    }
    ASTERIA_EXPECT(runner, "the bounded guard finds one ledger across all four active slots", prepared,
                   bindings.IsBound(ledger));

    bool firstThreeCleared{true};
    for (Foundation::UInt8 index{0U}; index < 3U; ++index)
    {
        firstThreeCleared = firstThreeCleared && (bindings.ClearActiveLedger(CpuId{index}, ledger) ==
                                                  Ia64ActiveRuntimeTranslationBindingError::None);
    }
    ASTERIA_EXPECT(runner, "the guard scans through the last slot after three exact clears", firstThreeCleared,
                   bindings.IsBound(ledger));
    ASTERIA_EXPECT(runner, "all four bindings are quiesced and cleared before ledger destruction",
                   bindings.ClearActiveLedger(CpuId{3U}, ledger) == Ia64ActiveRuntimeTranslationBindingError::None,
                   !bindings.IsBound(ledger));
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64ActiveRuntimeTranslationBindingsTests"};
    Asteria::Test::TestInvalidProcessorsAndConstinit(runner);
    Asteria::Test::TestAdmissionAndUnadmittedActivation(runner);
    Asteria::Test::TestActivationTransitionsAndClear(runner);
    Asteria::Test::TestPhysicalBootstrapActivation(runner);
    Asteria::Test::TestAllFourSlots(runner);
    return runner.Finish();
}
