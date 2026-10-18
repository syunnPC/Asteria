// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>
#include <cstdlib>

#include "Foundation/Types.hpp"
#include "Kernel/Memory/InitialMappingPlan.hpp"
#include "Platform/Amd64/Memory/InitialAddressSpacePreparation.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

extern "C" [[noreturn]] void AsteriaAmd64FailStopInterrupt() noexcept;

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
using Platform::Amd64::Memory::BootstrapDescriptorTableStoragePageCount;
using Platform::Amd64::Memory::BootstrapInitialMappingCount;
using Platform::Amd64::Memory::BootstrapMappingPlanSummary;
using Platform::Amd64::Memory::BootstrapPageTableStoragePageCount;
using Platform::Amd64::Memory::BootstrapReservedPageCount;
using Platform::Amd64::Memory::BootstrapStackPageCount;
using Platform::Amd64::Memory::InitialAddressSpacePreparation;
using Platform::Amd64::Memory::InitialAddressSpacePreparationError;
using Platform::Amd64::Memory::InitialAddressSpacePreparationStatus;
using Platform::Amd64::Memory::InitialProcessorStateCaptureError;
using Platform::Amd64::Memory::InitialProcessorStateReport;
using Platform::Amd64::Memory::PageTableEntriesPerPage;
using Platform::Amd64::Memory::PageTableStorage;
using Platform::Amd64::Memory::PagingCapabilities;
using Platform::Amd64::Memory::WritableInitialDescriptorTableView;
using Platform::Amd64::Memory::WritableInitialMemoryStackView;

constexpr Foundation::Size PageBytes{4096U};
constexpr Foundation::Size ReservedBytes{static_cast<Foundation::Size>(BootstrapReservedPageCount) * PageBytes};
constexpr Foundation::Size TableEntries{static_cast<Foundation::Size>(BootstrapPageTableStoragePageCount) *
                                        PageTableEntriesPerPage};
constexpr Foundation::Size DescriptorEntries{static_cast<Foundation::Size>(BootstrapDescriptorTableStoragePageCount) *
                                             PageTableEntriesPerPage};
constexpr Foundation::UInt64 CodeDescriptor{0x00AF'9A00'0000'FFFFULL};
constexpr Foundation::UInt64 DataDescriptor{0x00CF'9200'0000'FFFFULL};
constexpr Foundation::UInt8 InterruptGateAttributes{0x8EU};
constexpr Foundation::UInt8 Sentinel{0xA5U};
constexpr Foundation::Size ExtendedControlStateCaseCount{5U};

struct alignas(4096) ReservedStorage final
{
    Foundation::UInt8 Guard[PageBytes];
    Foundation::UInt8 Stack[BootstrapStackPageCount * PageBytes];
    Foundation::UInt64 Tables[TableEntries];
    Foundation::UInt64 Descriptors[DescriptorEntries];
};

static_assert(__builtin_offsetof(ReservedStorage, Stack) == PageBytes);
static_assert(__builtin_offsetof(ReservedStorage, Tables) == 5U * PageBytes);
static_assert(__builtin_offsetof(ReservedStorage, Descriptors) == 21U * PageBytes);
static_assert(sizeof(ReservedStorage) == ReservedBytes);

ReservedStorage g_reserved{};
Foundation::UInt8 g_reservedSnapshot[ReservedBytes]{};
Foundation::UInt8 g_planSnapshot[sizeof(InitialMappingPlan)]{};
constinit InitialMappingPlan g_plan;
constinit InitialMappingPlan g_globalPlan;
constinit InitialMappingPlan g_guardMappingPlan;
constinit InitialMappingPlan g_shortPageTablePlan;
constinit InitialAddressSpacePreparation g_invalidStackPreparation;
constinit InitialAddressSpacePreparation g_invalidTablePreparation;
constinit InitialAddressSpacePreparation g_invalidDescriptorPreparation;
constinit InitialAddressSpacePreparation g_invalidProcessorPreparation;
constinit InitialAddressSpacePreparation g_pksProcessorPreparation;
constinit InitialAddressSpacePreparation g_highCr4Preparation;
constinit InitialAddressSpacePreparation g_trapFlagPreparation;
constinit InitialAddressSpacePreparation g_globalMappingPreparation;
constinit InitialAddressSpacePreparation g_guardMappingPreparation;
constinit InitialAddressSpacePreparation g_shortPageTablePreparation;
constinit InitialAddressSpacePreparation g_extendedControlStatePreparations[ExtendedControlStateCaseCount];
constinit InitialAddressSpacePreparation g_aliasPreparation;
constinit InitialAddressSpacePreparation g_statusFlagsPreparation;
constinit InitialAddressSpacePreparation g_successPreparation;
constexpr Foundation::Size MemoryTypeCaseCount{11U};
constinit InitialAddressSpacePreparation g_memoryTypePreparations[MemoryTypeCaseCount];
const Platform::Amd64::Memory::InitialAddressSpacePreparationSummary* g_expectedActivationSummary{nullptr};
Foundation::UInt32 g_adapterCalls{0U};
bool g_adapterArgumentsExact{false};

template <typename TObject> void CopyObjectBytes(const TObject& object, Foundation::UInt8* const output) noexcept
{
    const auto* const bytes = reinterpret_cast<const Foundation::UInt8*>(&object);
    for (Foundation::Size index{0U}; index < sizeof(object); ++index)
    {
        output[index] = bytes[index];
    }
}

template <typename TObject>
[[nodiscard]] bool ObjectBytesEqual(const TObject& object, const Foundation::UInt8* const expected) noexcept
{
    const auto* const bytes = reinterpret_cast<const Foundation::UInt8*>(&object);
    for (Foundation::Size index{0U}; index < sizeof(object); ++index)
    {
        if (bytes[index] != expected[index])
        {
            return false;
        }
    }
    return true;
}

void FillReserved(const Foundation::UInt8 value) noexcept
{
    auto* const bytes = reinterpret_cast<Foundation::UInt8*>(&g_reserved);
    for (Foundation::Size index{0U}; index < sizeof(g_reserved); ++index)
    {
        bytes[index] = value;
    }
}

[[nodiscard]] bool ReservedBytesEqual(const Foundation::UInt8 value) noexcept
{
    const auto* const bytes = reinterpret_cast<const Foundation::UInt8*>(&g_reserved);
    for (Foundation::Size index{0U}; index < sizeof(g_reserved); ++index)
    {
        if (bytes[index] != value)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr MappingPermissions ReadOnly() noexcept
{
    return MappingPermissions{MappingPermission::Read};
}

[[nodiscard]] constexpr MappingPermissions ReadWrite() noexcept
{
    return ReadOnly().With(MappingPermission::Write);
}

[[nodiscard]] constexpr MappingPermissions ReadExecute() noexcept
{
    return ReadOnly().With(MappingPermission::Execute);
}

[[nodiscard]] InitialMapping Mapping(const Foundation::UInt64 start, const Foundation::UInt64 pages,
                                     const MappingPermissions permissions, const InitialMappingOwner owner) noexcept
{
    return InitialMapping{
        .VirtualStart = VirtualAddress{start},
        .PhysicalStart = PhysicalAddress{start},
        .Pages = PageCount{pages},
        .Permissions = permissions,
        .CachePolicy = MemoryCachePolicy::WriteBack,
        .Owner = owner,
        .Reserved = 0U,
    };
}

[[nodiscard]] bool Add(InitialMappingPlan& plan, const InitialMapping& mapping) noexcept
{
    return plan.AddMapping(mapping) == InitialMappingPlanError::None;
}

[[nodiscard]] Foundation::UInt64 ReservedStart() noexcept
{
    return static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(&g_reserved));
}

[[nodiscard]] bool BuildPlan(InitialMappingPlan& plan, const Foundation::UInt64 imageStart = 0x0040'0000U,
                             const MappingPermissions codePermissions = ReadExecute(),
                             const Foundation::UInt64 pageTablePages = BootstrapPageTableStoragePageCount) noexcept
{
    const Foundation::UInt64 reserved = ReservedStart();
    return Add(plan, Mapping(imageStart, 2U, codePermissions, InitialMappingOwner::NucleusCode)) &&
           Add(plan,
               Mapping(imageStart + (2U * PageBytes), 1U, ReadOnly(), InitialMappingOwner::NucleusReadOnlyData)) &&
           Add(plan,
               Mapping(imageStart + (3U * PageBytes), 1U, ReadOnly(), InitialMappingOwner::NucleusReadOnlyData)) &&
           Add(plan,
               Mapping(imageStart + (4U * PageBytes), 2U, ReadWrite(), InitialMappingOwner::NucleusWritableData)) &&
           Add(plan,
               Mapping(imageStart + (6U * PageBytes), 1U, ReadOnly(), InitialMappingOwner::NucleusReadOnlyData)) &&
           Add(plan,
               Mapping(imageStart + (7U * PageBytes), 1U, ReadOnly(), InitialMappingOwner::NucleusReadOnlyData)) &&
           Add(plan,
               Mapping(imageStart + (8U * PageBytes), 1U, ReadWrite(), InitialMappingOwner::NucleusWritableData)) &&
           Add(plan, Mapping(reserved + PageBytes, BootstrapStackPageCount, ReadWrite(),
                             InitialMappingOwner::NucleusStack)) &&
           Add(plan,
               Mapping(reserved + (5U * PageBytes), pageTablePages, ReadWrite(), InitialMappingOwner::PageTable)) &&
           Add(plan, Mapping(reserved + (21U * PageBytes), BootstrapDescriptorTableStoragePageCount, ReadWrite(),
                             InitialMappingOwner::NucleusDescriptorTables)) &&
           (plan.Seal() == InitialMappingPlanError::None) && (plan.GetMappingCount() == BootstrapInitialMappingCount);
}

[[nodiscard]] bool BuildPlan() noexcept
{
    return BuildPlan(g_plan);
}

[[nodiscard]] constexpr PagingCapabilities Capabilities() noexcept
{
    return PagingCapabilities{
        .PhysicalAddressBits = 52U,
        .FourLevelPagingEnabled = true,
        .ExecuteDisableEnabled = true,
        .SupervisorWriteProtectEnabled = true,
        .GlobalPagesEnabled = true,
        .PatLayoutConfigured = true,
        .Reserved16 = 0U,
        .Reserved = 0U,
    };
}

[[nodiscard]] BootstrapMappingPlanSummary MappingSummary() noexcept
{
    const Foundation::UInt64 reserved = ReservedStart();
    return BootstrapMappingPlanSummary{
        .ReservedPhysicalStart = reserved,
        .StackPhysicalStart = reserved + PageBytes,
        .PageTableStoragePhysicalStart = reserved + (5U * PageBytes),
        .DescriptorTableStoragePhysicalStart = reserved + (21U * PageBytes),
        .MappingCount = static_cast<Foundation::UInt16>(BootstrapInitialMappingCount),
        .Reserved16 = 0U,
        .Reserved32 = 0U,
    };
}

[[nodiscard]] PageTableStorage Tables() noexcept
{
    return PageTableStorage{
        .PhysicalStart = PhysicalAddress{MappingSummary().PageTableStoragePhysicalStart},
        .Entries = g_reserved.Tables,
        .PageCapacity = BootstrapPageTableStoragePageCount,
        .Reserved = 0U,
    };
}

[[nodiscard]] WritableInitialMemoryStackView Stack() noexcept
{
    return WritableInitialMemoryStackView{
        .Data = g_reserved.Stack,
        .SizeBytes = sizeof(g_reserved.Stack),
        .PhysicalStart = MappingSummary().StackPhysicalStart,
    };
}

[[nodiscard]] WritableInitialDescriptorTableView DescriptorTables() noexcept
{
    return WritableInitialDescriptorTableView{
        .Data = g_reserved.Descriptors,
        .SizeBytes = sizeof(g_reserved.Descriptors),
        .PhysicalStart = MappingSummary().DescriptorTableStoragePhysicalStart,
    };
}

[[nodiscard]] bool AllZero(const Foundation::UInt8* const bytes, const Foundation::Size size) noexcept
{
    for (Foundation::Size index{0U}; index < size; ++index)
    {
        if (bytes[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] Foundation::UInt16 ReadLimit(const Foundation::UInt64 registerAddress) noexcept
{
    const auto* const bytes =
        reinterpret_cast<const Foundation::UInt8*>(static_cast<Foundation::UIntPointer>(registerAddress));
    return static_cast<Foundation::UInt16>(bytes[0U]) |
           static_cast<Foundation::UInt16>(static_cast<Foundation::UInt16>(bytes[1U]) << 8U);
}

[[nodiscard]] Foundation::UInt64 ReadBase(const Foundation::UInt64 registerAddress) noexcept
{
    const auto* const bytes =
        reinterpret_cast<const Foundation::UInt8*>(static_cast<Foundation::UIntPointer>(registerAddress));
    Foundation::UInt64 value{0U};
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        value |= static_cast<Foundation::UInt64>(bytes[index + 2U]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] Foundation::UInt64 InterruptGateLow(const Foundation::UInt64 handler,
                                                  const Foundation::UInt16 selector) noexcept
{
    return (handler & 0xFFFFU) | (static_cast<Foundation::UInt64>(selector) << 16U) |
           (static_cast<Foundation::UInt64>(InterruptGateAttributes) << 40U) | (((handler >> 16U) & 0xFFFFU) << 48U);
}

[[nodiscard]] bool
DescriptorTablesAreExact(const Platform::Amd64::Memory::InitialAddressSpacePreparationSummary& summary) noexcept
{
    const Foundation::Size codeIndex = summary.CodeSelector >> 3U;
    const Foundation::Size stackIndex = summary.StackSelector >> 3U;
    for (Foundation::Size index{0U}; index < 512U; ++index)
    {
        const Foundation::UInt64 expected =
            index == codeIndex ? CodeDescriptor : (index == stackIndex ? DataDescriptor : 0U);
        if (g_reserved.Descriptors[index] != expected)
        {
            return false;
        }
    }
    const Foundation::UInt64 handler =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(&AsteriaAmd64FailStopInterrupt));
    const Foundation::UInt64 low = InterruptGateLow(handler, summary.CodeSelector);
    const Foundation::UInt64 high = handler >> 32U;
    for (Foundation::Size index{0U}; index < 256U; ++index)
    {
        if ((g_reserved.Descriptors[512U + (index * 2U)] != low) ||
            (g_reserved.Descriptors[513U + (index * 2U)] != high))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool ObserveActivation(const Foundation::UInt64 rootPhysicalAddress,
                                     const Foundation::UInt64 initialStackPointer,
                                     const Foundation::UInt64 globalDescriptorRegisterAddress,
                                     const Foundation::UInt64 interruptDescriptorRegisterAddress,
                                     const Foundation::UInt64 currentRootPhysicalAddress,
                                     const Foundation::UInt64 packedSelectors) noexcept
{
    ++g_adapterCalls;
    if (g_expectedActivationSummary == nullptr)
    {
        return false;
    }
    const auto& summary = *g_expectedActivationSummary;
    const Foundation::UInt16 codeSelector = static_cast<Foundation::UInt16>(packedSelectors >> 16U);
    const Foundation::UInt16 stackSelector = static_cast<Foundation::UInt16>(packedSelectors);
    g_adapterArgumentsExact =
        (rootPhysicalAddress == summary.RootPhysicalAddress.GetValue()) &&
        (initialStackPointer == summary.InitialStackPointer) && (currentRootPhysicalAddress == 0x0010'0000U) &&
        (codeSelector == summary.CodeSelector) && (stackSelector == summary.StackSelector) &&
        (ReadLimit(globalDescriptorRegisterAddress) == summary.GlobalDescriptorTableLimit) &&
        (ReadBase(globalDescriptorRegisterAddress) == summary.GlobalDescriptorTableAddress) &&
        (ReadLimit(interruptDescriptorRegisterAddress) == summary.InterruptDescriptorTableLimit) &&
        (ReadBase(interruptDescriptorRegisterAddress) == summary.InterruptDescriptorTableAddress) &&
        AllZero(g_reserved.Stack, sizeof(g_reserved.Stack)) && DescriptorTablesAreExact(summary);
    return g_adapterArgumentsExact;
}

void TestCapture(TestContext& runner) noexcept
{
    InitialProcessorStateReport output{};
    auto* const bytes = reinterpret_cast<Foundation::UInt8*>(&output);
    for (Foundation::Size index{0U}; index < sizeof(output); ++index)
    {
        bytes[index] = 0x5AU;
    }
    Foundation::UInt8 snapshot[sizeof(output)]{};
    CopyObjectBytes(output, snapshot);
    PagingCapabilities invalid = Capabilities();
    invalid.PhysicalAddressBits = 31U;
    ASTERIA_EXPECT(runner, "a rejected processor capture leaves every output byte unchanged",
                   InitialAddressSpacePreparation::CaptureProcessorState(invalid, output) ==
                       InitialProcessorStateCaptureError::InvalidCapabilities,
                   ObjectBytesEqual(output, snapshot));

    ASTERIA_EXPECT(runner, "a valid host capture publishes the deterministic bootstrap profile",
                   InitialAddressSpacePreparation::CaptureProcessorState(Capabilities(), output) ==
                       InitialProcessorStateCaptureError::None,
                   output.Cr0 == 0x8001'0033U, output.Cr3 == 0x0010'0000U, output.Cr4 == 0x0000'0668U,
                   output.Efer == 0x0000'0D00U, output.Pat == Platform::Amd64::Memory::ExpectedInitialPatValue,
                   output.MtrrDefaultType == 0x806U, output.CodeSelector == 0x38U, output.StackSelector == 0x30U);
}

// The control state the activation establishes over a firmware's
// (EstablishInitialControlState): only WP, NXE, PGE and PCIDE change.
void TestEstablishedControlState(TestContext& runner) noexcept
{
    using Platform::Amd64::Memory::EstablishInitialControlState;
    using Platform::Amd64::Memory::InitialControlState;
    // A firmware that leaves write protection and execute-disable off and
    // global pages and PCIDs on (CR4 also has PAE, MCE, OSFXSR, OSXMMEXCPT).
    constexpr InitialControlState firmware{.Cr0 = 0x8000'0033U, .Cr4 = 0x0002'06E0U, .Efer = 0x0000'0500U};
    constexpr InitialControlState established = EstablishInitialControlState(firmware);
    ASTERIA_EXPECT(runner, "the activation sets WP and NXE and clears PGE and PCIDE, leaving every other bit",
                   established.Cr0 == 0x8001'0033U, established.Cr4 == 0x0000'0660U, established.Efer == 0x0000'0D00U);
    constexpr InitialControlState again = EstablishInitialControlState(established);
    ASTERIA_EXPECT(runner, "establishing an established state changes nothing", again.Cr0 == established.Cr0,
                   again.Cr4 == established.Cr4, again.Efer == established.Efer);
    constexpr InitialControlState ovmf{.Cr0 = 0x8001'0033U, .Cr4 = 0x0000'0668U, .Efer = 0x0000'0D00U};
    constexpr InitialControlState kept = EstablishInitialControlState(ovmf);
    ASTERIA_EXPECT(runner, "a firmware state that already complies is kept as it is", kept.Cr0 == ovmf.Cr0,
                   kept.Cr4 == ovmf.Cr4, kept.Efer == ovmf.Efer);
}

void TestPrecommitFailures(TestContext& runner) noexcept
{
    InitialProcessorStateReport processor{};
    static_cast<void>(InitialAddressSpacePreparation::CaptureProcessorState(Capabilities(), processor));
    const BootstrapMappingPlanSummary summary = MappingSummary();
    const PageTableStorage tables = Tables();
    const WritableInitialMemoryStackView stack = Stack();
    const WritableInitialDescriptorTableView descriptors = DescriptorTables();

    FillReserved(Sentinel);
    WritableInitialMemoryStackView shortStack = stack;
    --shortStack.SizeBytes;
    const auto stackResult =
        g_invalidStackPreparation.Prepare(g_plan, summary, Capabilities(), tables, shortStack, descriptors, processor);
    ASTERIA_EXPECT(runner, "invalid stack shape fails before any reserved byte is written", !stackResult.HasValue(),
                   stackResult.GetError() == InitialAddressSpacePreparationError::InvalidMemoryStack,
                   g_invalidStackPreparation.HasFailed(), ReservedBytesEqual(Sentinel));

    FillReserved(Sentinel);
    PageTableStorage invalidTables = tables;
    invalidTables.PhysicalStart = PhysicalAddress{tables.PhysicalStart.GetValue() + PageBytes};
    const auto tableResult = g_invalidTablePreparation.Prepare(g_plan, summary, Capabilities(), invalidTables, stack,
                                                               descriptors, processor);
    ASTERIA_EXPECT(runner, "non-identity page-table storage fails before any reserved write", !tableResult.HasValue(),
                   tableResult.GetError() == InitialAddressSpacePreparationError::InvalidPageTableStorage,
                   g_invalidTablePreparation.HasFailed(), ReservedBytesEqual(Sentinel));

    FillReserved(Sentinel);
    WritableInitialDescriptorTableView shortDescriptors = descriptors;
    --shortDescriptors.SizeBytes;
    const auto descriptorResult = g_invalidDescriptorPreparation.Prepare(g_plan, summary, Capabilities(), tables, stack,
                                                                         shortDescriptors, processor);
    ASTERIA_EXPECT(runner, "invalid descriptor storage fails before page-table or stack writes",
                   !descriptorResult.HasValue(),
                   descriptorResult.GetError() == InitialAddressSpacePreparationError::InvalidDescriptorTableStorage,
                   g_invalidDescriptorPreparation.HasFailed(), ReservedBytesEqual(Sentinel));

    FillReserved(Sentinel);
    InitialProcessorStateReport invalidProcessor = processor;
    invalidProcessor.Cr4 |= 1U << 7U;
    const auto processorResult = g_invalidProcessorPreparation.Prepare(g_plan, summary, Capabilities(), tables, stack,
                                                                       descriptors, invalidProcessor);
    ASTERIA_EXPECT(runner, "forbidden processor state fails before any output write", !processorResult.HasValue(),
                   processorResult.GetError() == InitialAddressSpacePreparationError::InvalidProcessorState,
                   g_invalidProcessorPreparation.HasFailed(), ReservedBytesEqual(Sentinel));

    FillReserved(Sentinel);
    InitialProcessorStateReport pksProcessor = processor;
    pksProcessor.Cr4 |= Foundation::UInt64{1U} << 24U;
    const auto pksResult =
        g_pksProcessorPreparation.Prepare(g_plan, summary, Capabilities(), tables, stack, descriptors, pksProcessor);
    ASTERIA_EXPECT(runner, "supervisor protection keys are rejected before commit", !pksResult.HasValue(),
                   pksResult.GetError() == InitialAddressSpacePreparationError::InvalidProcessorState,
                   g_pksProcessorPreparation.HasFailed(), ReservedBytesEqual(Sentinel));

    FillReserved(Sentinel);
    InitialProcessorStateReport highCr4Processor = processor;
    highCr4Processor.Cr4 |= Foundation::UInt64{1U} << 32U;
    const auto highCr4Result =
        g_highCr4Preparation.Prepare(g_plan, summary, Capabilities(), tables, stack, descriptors, highCr4Processor);
    ASTERIA_EXPECT(runner, "unknown high CR4 state is rejected before commit", !highCr4Result.HasValue(),
                   highCr4Result.GetError() == InitialAddressSpacePreparationError::InvalidProcessorState,
                   g_highCr4Preparation.HasFailed(), ReservedBytesEqual(Sentinel));

    FillReserved(Sentinel);
    InitialProcessorStateReport trapProcessor = processor;
    trapProcessor.Rflags |= Foundation::UInt64{1U} << 8U;
    const auto trapResult =
        g_trapFlagPreparation.Prepare(g_plan, summary, Capabilities(), tables, stack, descriptors, trapProcessor);
    ASTERIA_EXPECT(runner, "single-step state is rejected before entering the adapter", !trapResult.HasValue(),
                   trapResult.GetError() == InitialAddressSpacePreparationError::InvalidProcessorState,
                   g_trapFlagPreparation.HasFailed(), ReservedBytesEqual(Sentinel));

    const bool builtGlobalPlan = BuildPlan(g_globalPlan, 0x0040'0000U, ReadExecute().With(MappingPermission::Global));
    FillReserved(Sentinel);
    const auto globalResult = g_globalMappingPreparation.Prepare(g_globalPlan, summary, Capabilities(), tables, stack,
                                                                 descriptors, processor);
    ASTERIA_EXPECT(runner, "global leaves are rejected while CR4.PGE remains cleared", builtGlobalPlan,
                   !globalResult.HasValue(),
                   globalResult.GetError() == InitialAddressSpacePreparationError::GlobalMappingUnsupported,
                   g_globalMappingPreparation.HasFailed(), ReservedBytesEqual(Sentinel));

    const bool builtGuardMappingPlan = BuildPlan(g_guardMappingPlan, ReservedStart() - (8U * PageBytes));
    FillReserved(Sentinel);
    const auto guardMappingResult = g_guardMappingPreparation.Prepare(g_guardMappingPlan, summary, Capabilities(),
                                                                      tables, stack, descriptors, processor);
    ASTERIA_EXPECT(runner, "the reserved guard page cannot be covered by an image mapping", builtGuardMappingPlan,
                   !guardMappingResult.HasValue(),
                   guardMappingResult.GetError() == InitialAddressSpacePreparationError::InvalidActivationMapping,
                   g_guardMappingPreparation.HasFailed(), ReservedBytesEqual(Sentinel));

    const bool builtShortPageTablePlan =
        BuildPlan(g_shortPageTablePlan, 0x0040'0000U, ReadExecute(), BootstrapPageTableStoragePageCount - 1U);
    FillReserved(Sentinel);
    const auto shortPageTableResult = g_shortPageTablePreparation.Prepare(g_shortPageTablePlan, summary, Capabilities(),
                                                                          tables, stack, descriptors, processor);
    ASTERIA_EXPECT(runner, "an inexact page-table mapping is rejected in bounded preflight", builtShortPageTablePlan,
                   !shortPageTableResult.HasValue(),
                   shortPageTableResult.GetError() == InitialAddressSpacePreparationError::InvalidActivationMapping,
                   g_shortPageTablePreparation.HasFailed(), ReservedBytesEqual(Sentinel));

    constexpr Foundation::UInt8 ExtendedControlStateBits[ExtendedControlStateCaseCount]{12U, 17U, 23U, 27U, 28U};
    bool extendedControlStateRejected{true};
    for (Foundation::Size index{0U}; index < ExtendedControlStateCaseCount; ++index)
    {
        FillReserved(Sentinel);
        InitialProcessorStateReport extendedProcessor = processor;
        extendedProcessor.Cr4 |= Foundation::UInt64{1U} << ExtendedControlStateBits[index];
        const auto extendedResult = g_extendedControlStatePreparations[index].Prepare(
            g_plan, summary, Capabilities(), tables, stack, descriptors, extendedProcessor);
        extendedControlStateRejected =
            extendedControlStateRejected && !extendedResult.HasValue() &&
            (extendedResult.GetError() == InitialAddressSpacePreparationError::InvalidProcessorState) &&
            g_extendedControlStatePreparations[index].HasFailed() && ReservedBytesEqual(Sentinel);
    }
    ASTERIA_EXPECT(runner, "LA57, PCID, CET, LASS, and LAM are rejected before commit", extendedControlStateRejected);

    FillReserved(Sentinel);
    WritableInitialDescriptorTableView alias = descriptors;
    alias.Data = g_reserved.Tables;
    alias.PhysicalStart = tables.PhysicalStart.GetValue();
    const auto aliasResult =
        g_aliasPreparation.Prepare(g_plan, summary, Capabilities(), tables, stack, alias, processor);
    ASTERIA_EXPECT(runner, "cross-output aliasing is rejected while the owner remains fresh", !aliasResult.HasValue(),
                   aliasResult.GetError() == InitialAddressSpacePreparationError::StorageAliasesPreparation,
                   !g_aliasPreparation.HasFailed(), !g_aliasPreparation.IsPrepared(), ReservedBytesEqual(Sentinel));

    FillReserved(Sentinel);
    InitialProcessorStateReport mutableStatusFlags = processor;
    mutableStatusFlags.Rflags = 0x246U;
    const auto statusFlagsResult = g_statusFlagsPreparation.Prepare(g_plan, summary, Capabilities(), tables, stack,
                                                                    descriptors, mutableStatusFlags);
    ASTERIA_EXPECT(runner, "ordinary arithmetic and interrupt-status flags do not stale preparation",
                   statusFlagsResult.HasValue(), g_statusFlagsPreparation.IsPrepared(),
                   g_statusFlagsPreparation.ValidatePreparedState() == InitialAddressSpacePreparationError::None);
}

void TestSuccessfulPreparationAndActivation(TestContext& runner) noexcept
{
    InitialProcessorStateReport processor{};
    static_cast<void>(InitialAddressSpacePreparation::CaptureProcessorState(Capabilities(), processor));
    const BootstrapMappingPlanSummary summary = MappingSummary();
    const PagingCapabilities capabilities = Capabilities();
    const PageTableStorage tables = Tables();
    const WritableInitialMemoryStackView stack = Stack();
    const WritableInitialDescriptorTableView descriptors = DescriptorTables();
    Foundation::UInt8 summarySnapshot[sizeof(summary)]{};
    Foundation::UInt8 capabilitiesSnapshot[sizeof(capabilities)]{};
    Foundation::UInt8 tablesSnapshot[sizeof(tables)]{};
    Foundation::UInt8 stackSnapshot[sizeof(stack)]{};
    Foundation::UInt8 descriptorSnapshot[sizeof(descriptors)]{};
    Foundation::UInt8 processorSnapshot[sizeof(processor)]{};
    CopyObjectBytes(g_plan, g_planSnapshot);
    CopyObjectBytes(summary, summarySnapshot);
    CopyObjectBytes(capabilities, capabilitiesSnapshot);
    CopyObjectBytes(tables, tablesSnapshot);
    CopyObjectBytes(stack, stackSnapshot);
    CopyObjectBytes(descriptors, descriptorSnapshot);
    CopyObjectBytes(processor, processorSnapshot);
    FillReserved(Sentinel);

    const auto result =
        g_successPreparation.Prepare(g_plan, summary, capabilities, tables, stack, descriptors, processor);
    const auto preparedSummary = g_successPreparation.TryGetSummary();
    const auto pageTables = g_successPreparation.TryGetPageTablePreparation();
    ASTERIA_EXPECT(runner, "one valid call publishes a completely revalidated owner", result.HasValue(),
                   result.GetValue() == InitialAddressSpacePreparationStatus::Prepared,
                   g_successPreparation.IsPrepared(), preparedSummary.HasValue(), pageTables.HasValue(),
                   g_successPreparation.ValidatePreparedState() == InitialAddressSpacePreparationError::None);

    const auto& published = *preparedSummary.GetValue();
    bool guardUnchanged{true};
    for (const Foundation::UInt8 byte : g_reserved.Guard)
    {
        if (byte != Sentinel)
        {
            guardUnchanged = false;
        }
    }
    ASTERIA_EXPECT(runner, "success clears only owned outputs and builds exact GDT/IDT state", guardUnchanged,
                   AllZero(g_reserved.Stack, sizeof(g_reserved.Stack)), DescriptorTablesAreExact(published),
                   published.RootPhysicalAddress.GetValue() == summary.PageTableStoragePhysicalStart,
                   published.StackPhysicalStart == summary.StackPhysicalStart,
                   published.InitialStackPointer == summary.StackPhysicalStart + sizeof(g_reserved.Stack),
                   published.GlobalDescriptorTableAddress == summary.DescriptorTableStoragePhysicalStart,
                   published.InterruptDescriptorTableAddress == summary.DescriptorTableStoragePhysicalStart + PageBytes,
                   published.GlobalDescriptorTableLimit == 63U, published.InterruptDescriptorTableLimit == 4095U);

    ASTERIA_EXPECT(runner, "success preserves every caller-owned input and descriptor byte",
                   ObjectBytesEqual(g_plan, g_planSnapshot), ObjectBytesEqual(summary, summarySnapshot),
                   ObjectBytesEqual(capabilities, capabilitiesSnapshot), ObjectBytesEqual(tables, tablesSnapshot),
                   ObjectBytesEqual(stack, stackSnapshot), ObjectBytesEqual(descriptors, descriptorSnapshot),
                   ObjectBytesEqual(processor, processorSnapshot));

    auto* const reservedBytes = reinterpret_cast<Foundation::UInt8*>(&g_reserved);
    for (Foundation::Size index{0U}; index < sizeof(g_reserved); ++index)
    {
        g_reservedSnapshot[index] = reservedBytes[index];
    }
    const auto repeated =
        g_successPreparation.Prepare(g_plan, summary, capabilities, tables, stack, descriptors, processor);
    bool repeatedUnchanged{true};
    for (Foundation::Size index{0U}; index < sizeof(g_reserved); ++index)
    {
        if (reservedBytes[index] != g_reservedSnapshot[index])
        {
            repeatedUnchanged = false;
        }
    }
    ASTERIA_EXPECT(runner, "a repeated prepare is rejected without changing external state", !repeated.HasValue(),
                   repeated.GetError() == InitialAddressSpacePreparationError::AlreadyPrepared, repeatedUnchanged);

    g_reserved.Stack[0U] = 1U;
    ASTERIA_EXPECT(runner, "a post-publication stack mutation is detected",
                   g_successPreparation.ValidatePreparedState() ==
                       InitialAddressSpacePreparationError::MemoryStackNotZero,
                   g_successPreparation.Activate() == InitialAddressSpacePreparationError::MemoryStackNotZero,
                   g_adapterCalls == 0U, g_successPreparation.IsPrepared());
    g_reserved.Stack[0U] = 0U;
    g_reserved.Stack[sizeof(g_reserved.Stack) - 1U] = 1U;
    ASTERIA_EXPECT(runner, "the final post-publication stack byte is revalidated",
                   g_successPreparation.ValidatePreparedState() ==
                       InitialAddressSpacePreparationError::MemoryStackNotZero);
    g_reserved.Stack[sizeof(g_reserved.Stack) - 1U] = 0U;
    g_reserved.Descriptors[0U] = 1U;
    ASTERIA_EXPECT(runner, "a post-publication GDT mutation is detected",
                   g_successPreparation.ValidatePreparedState() ==
                       InitialAddressSpacePreparationError::DescriptorTablesCorrupt,
                   g_successPreparation.Activate() == InitialAddressSpacePreparationError::DescriptorTablesCorrupt,
                   g_adapterCalls == 0U, g_successPreparation.IsPrepared());
    g_reserved.Descriptors[0U] = 0U;
    g_reserved.Descriptors[511U] = 1U;
    ASTERIA_EXPECT(runner, "the final GDT entry is revalidated",
                   g_successPreparation.ValidatePreparedState() ==
                       InitialAddressSpacePreparationError::DescriptorTablesCorrupt);
    g_reserved.Descriptors[511U] = 0U;
    const Foundation::UInt64 savedRoot = g_reserved.Tables[0U];
    g_reserved.Tables[0U] ^= 1U << 1U;
    ASTERIA_EXPECT(runner, "a post-publication native page-table mutation is detected",
                   g_successPreparation.ValidatePreparedState() ==
                       InitialAddressSpacePreparationError::PageTableVerificationRejected,
                   g_successPreparation.Activate() ==
                       InitialAddressSpacePreparationError::PageTableVerificationRejected,
                   g_adapterCalls == 0U, g_successPreparation.IsPrepared());
    g_reserved.Tables[0U] = savedRoot;
    const Foundation::UInt64 savedLastTable = g_reserved.Tables[TableEntries - 1U];
    g_reserved.Tables[TableEntries - 1U] = 1U;
    ASTERIA_EXPECT(runner, "the final unused page-table word is revalidated",
                   g_successPreparation.ValidatePreparedState() ==
                       InitialAddressSpacePreparationError::PageTableVerificationRejected);
    g_reserved.Tables[TableEntries - 1U] = savedLastTable;

    g_expectedActivationSummary = &published;
    const InitialAddressSpacePreparationError activationError = g_successPreparation.Activate();
    ASTERIA_EXPECT(runner, "activation passes only retained exact state to the private adapter",
                   activationError == InitialAddressSpacePreparationError::UnsupportedProcessorState,
                   g_adapterCalls == 1U, g_adapterArgumentsExact, g_successPreparation.HasFailed(),
                   g_successPreparation.GetFailure().AdapterStatus == 3U);
}

// MTRR types for the kernel's pages: write-back or uncached (MTRRs disabled
// Included, as Hyper-V presents them) keep them ordered and are accepted; write-combining or
// write-through ranges over them, and low reserved bits, are refused. Reserved bits at and above
// the physical width (SDM figure 12-7) are ignored; hardware and virtual-machine profiles can leave
// them set.
void TestMemoryTypes(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 Enable{1U << 11U};
    constexpr Foundation::UInt64 Valid{1U << 11U};
    constexpr Foundation::UInt64 WriteBack{6U};
    constexpr Foundation::UInt64 Uncached{0U};
    constexpr Foundation::UInt64 WriteCombining{1U};
    constexpr Foundation::UInt64 WriteThrough{4U};
    struct Case
    {
        const char* Name;
        Foundation::UInt64 DefaultType;
        Foundation::UInt8 Count;
        Foundation::UInt64 Base[2U];
        Foundation::UInt64 Mask[2U];
        bool Accepted;
    };
    // A mask of only the valid bit covers every physical address.
    const Case cases[MemoryTypeCaseCount]{
        {"MTRRs disabled", 0U, 0U, {0U, 0U}, {0U, 0U}, true},
        {"uncached default without ranges", Enable | Uncached, 0U, {0U, 0U}, {0U, 0U}, true},
        {"a write-back range over an uncached default", Enable | Uncached, 1U, {WriteBack, 0U}, {Valid, 0U}, true},
        {"uncached overlapping write-back", Enable | WriteBack, 2U, {WriteBack, Uncached}, {Valid, Valid}, true},
        {"reserved bits above the physical width",
         Enable | Uncached,
         1U,
         {WriteBack | (Foundation::UInt64{1U} << 62U), 0U},
         {Valid | (Foundation::UInt64{1U} << 63U), 0U},
         true},
        // Project Hyper-V generation 2 boot observation captured with CPUID's 48-bit width; SDM
        // section 12.11.2.3 compares address AND mask to base AND mask.
        {"a base with bits its mask leaves out",
         Enable | Uncached,
         2U,
         {WriteBack, 0x1'0000'0000U | WriteBack},
         {0x000F'FFFF'0000'0000U | Valid, 0x000F'FFF0'0000'0000U | Valid},
         true},
        {"a write-combining range", Enable | WriteBack, 1U, {WriteCombining, 0U}, {Valid, 0U}, false},
        {"a write-through default", Enable | WriteThrough, 0U, {0U, 0U}, {0U, 0U}, false},
        {"write-through overlapping write-back",
         Enable | WriteBack,
         2U,
         {WriteBack, WriteThrough},
         {Valid, Valid},
         false},
        {"a low reserved base bit", Enable | Uncached, 1U, {WriteBack | 0x100U, 0U}, {Valid, 0U}, false},
        {"a low reserved mask bit", Enable | Uncached, 1U, {WriteBack, 0U}, {Valid | 1U, 0U}, false},
    };
    for (Foundation::Size index{0U}; index < MemoryTypeCaseCount; ++index)
    {
        InitialProcessorStateReport processor{};
        PagingCapabilities capabilities = Capabilities();
        capabilities.PhysicalAddressBits = 48U;
        static_cast<void>(InitialAddressSpacePreparation::CaptureProcessorState(capabilities, processor));
        processor.MtrrDefaultType = cases[index].DefaultType;
        processor.VariableMtrrCount = cases[index].Count;
        processor.MtrrCapability = cases[index].Count; // The capability names the variable count
        for (Foundation::Size range{0U}; range < 2U; ++range)
        {
            processor.VariableMtrrBase[range] = cases[index].Base[range];
            processor.VariableMtrrMask[range] = cases[index].Mask[range];
        }
        FillReserved(Sentinel);
        const auto result = g_memoryTypePreparations[index].Prepare(g_plan, MappingSummary(), capabilities, Tables(),
                                                                    Stack(), DescriptorTables(), processor);
        ASTERIA_EXPECT(runner, cases[index].Name, result.HasValue() == cases[index].Accepted,
                       cases[index].Accepted ||
                           (result.GetError() == InitialAddressSpacePreparationError::InvalidMemoryTypeConfiguration));
    }
}
} // Namespace
} // Namespace Asteria::Test

extern "C" Asteria::Foundation::UInt64
AsteriaAmd64ActivateInitialAddressSpace(const Asteria::Foundation::UInt64 rootPhysicalAddress,
                                        const Asteria::Foundation::UInt64 initialStackPointer,
                                        const Asteria::Foundation::UInt64 globalDescriptorRegisterAddress,
                                        const Asteria::Foundation::UInt64 interruptDescriptorRegisterAddress,
                                        const Asteria::Foundation::UInt64 currentRootPhysicalAddress,
                                        const Asteria::Foundation::UInt64 packedSelectors) noexcept
{
    return Asteria::Test::ObserveActivation(rootPhysicalAddress, initialStackPointer, globalDescriptorRegisterAddress,
                                            interruptDescriptorRegisterAddress, currentRootPhysicalAddress,
                                            packedSelectors)
               ? 3U
               : 1U;
}

extern "C" [[noreturn]] void AsteriaAmd64FailStopInterrupt() noexcept
{
    std::abort();
}

int main()
{
    TestContext runner{"InitialAddressSpacePreparationTests"};
    ASTERIA_EXPECT(runner, "the preparation fixture seals", Asteria::Test::BuildPlan());
    Asteria::Test::TestCapture(runner);
    Asteria::Test::TestEstablishedControlState(runner);
    Asteria::Test::TestPrecommitFailures(runner);
    Asteria::Test::TestSuccessfulPreparationAndActivation(runner);
    Asteria::Test::TestMemoryTypes(runner);
    return runner.Finish();
}
