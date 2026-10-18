// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Boot/BootInfo.hpp"
#include "Boot/Uefi/BootTransition.hpp"
#include "Boot/Uefi/UefiTypes.hpp"
#include "Foundation/Types.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
constexpr Foundation::Size TestDescriptorSize{48U};
constexpr Foundation::Size MaximumEvents{16U};
constexpr Foundation::Size MaximumMockCalls{8U};
constexpr Foundation::Size SmallMapCapacity{4096U};
constexpr Foundation::Size LargeMapBytes = Boot::Uefi::MinimumMemoryDescriptorSize * (Boot::MaximumMemoryRegions + 1U);

enum class FirmwareEvent : Foundation::UInt8
{
    GetMemoryMap,
    PrepareMemoryMap,
    ExitBootServices,
};

struct MockFirmwareState
{
    const Foundation::UInt8* Map;
    Foundation::Size MapSize;
    Foundation::Size DescriptorSize;
    Foundation::UInt32 DescriptorVersion;
    Foundation::Size ReportedMapSizes[MaximumMockCalls];
    bool OverrideFirstDescriptor[MaximumMockCalls];
    Foundation::UInt32 OverrideTypes[MaximumMockCalls];
    Foundation::UInt64 OverridePhysicalStarts[MaximumMockCalls];
    Foundation::UInt64 OverrideVirtualStarts[MaximumMockCalls];
    Foundation::UInt64 OverridePageCounts[MaximumMockCalls];
    Foundation::UInt64 OverrideAttributes[MaximumMockCalls];
    Boot::Uefi::Status GetStatuses[MaximumMockCalls];
    Boot::Uefi::Status ExitStatuses[MaximumMockCalls];
    bool PreparationStatuses[MaximumMockCalls];
    Foundation::Size MapKeys[MaximumMockCalls];
    Foundation::Size ObservedExitKeys[MaximumMockCalls];
    FirmwareEvent Events[MaximumEvents];
    Foundation::Size EventCount;
    Foundation::Size GetCallCount;
    Foundation::Size PreparationCallCount;
    Foundation::Size ExitCallCount;
    bool PreparationSawNormalizedMap;
    Foundation::Size PreparationClearedAcpiCount;
    Foundation::UInt64 LastPublishedRsdpPhysicalAddress;
    Foundation::Size PreparationSawEntropyCount;
};

alignas(8) Foundation::UInt8 g_smallMap[SmallMapCapacity];
alignas(8) Foundation::UInt8 g_largeMap[LargeMapBytes];
Boot::BootInfo g_bootInfo;
Boot::Uefi::BootServices g_bootServices;
MockFirmwareState g_mock;
// The record the entropy tests hand to the transition.
Boot::BootEntropy g_entropy;

[[nodiscard]] bool EntropyEquals(const Boot::BootEntropy& first, const Boot::BootEntropy& second) noexcept
{
    for (Foundation::Size index{0U}; index < Boot::BootEntropySeedBytes; ++index)
    {
        if (first.Seed[index] != second.Seed[index])
        {
            return false;
        }
    }
    return (first.Sources == second.Sources) && (first.Reserved == second.Reserved);
}

void ClearBytes(Foundation::UInt8* const bytes, const Foundation::Size size) noexcept
{
    for (Foundation::Size index{0U}; index < size; ++index)
    {
        bytes[index] = 0U;
    }
}

void StoreLittleEndian32(Foundation::UInt8* const bytes, const Foundation::UInt32 value) noexcept
{
    for (Foundation::UInt32 index{0U}; index < 4U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> (index * 8U));
    }
}

void StoreLittleEndian64(Foundation::UInt8* const bytes, const Foundation::UInt64 value) noexcept
{
    for (Foundation::UInt32 index{0U}; index < 8U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> (index * 8U));
    }
}

void WriteDescriptor(Foundation::UInt8* const map, const Foundation::Size descriptorSize,
                     const Foundation::Size descriptorIndex, const Foundation::UInt32 type,
                     const Foundation::UInt64 physicalStart, const Foundation::UInt64 virtualStart,
                     const Foundation::UInt64 pageCount, const Foundation::UInt64 attributes) noexcept
{
    Foundation::UInt8* const descriptor = map + (descriptorIndex * descriptorSize);
    ClearBytes(descriptor, descriptorSize);
    StoreLittleEndian32(descriptor, type);
    StoreLittleEndian64(descriptor + 8U, physicalStart);
    StoreLittleEndian64(descriptor + 16U, virtualStart);
    StoreLittleEndian64(descriptor + 24U, pageCount);
    StoreLittleEndian64(descriptor + 32U, attributes);
    for (Foundation::Size index{Boot::Uefi::MinimumMemoryDescriptorSize}; index < descriptorSize; ++index)
    {
        descriptor[index] = 0xA5U;
    }
}

void RecordEvent(const FirmwareEvent event) noexcept
{
    if (g_mock.EventCount < MaximumEvents)
    {
        g_mock.Events[g_mock.EventCount] = event;
    }
    ++g_mock.EventCount;
}

Boot::Uefi::Status ASTERIA_UEFI_API MockGetMemoryMap(Foundation::Size* const memoryMapSize, void* const memoryMap,
                                                     Foundation::Size* const mapKey,
                                                     Foundation::Size* const descriptorSize,
                                                     Foundation::UInt32* const descriptorVersion) noexcept
{
    RecordEvent(FirmwareEvent::GetMemoryMap);
    const Foundation::Size callIndex = g_mock.GetCallCount;
    ++g_mock.GetCallCount;
    if ((memoryMapSize == nullptr) || (mapKey == nullptr) || (descriptorSize == nullptr) ||
        (descriptorVersion == nullptr) || (callIndex >= MaximumMockCalls))
    {
        return Boot::Uefi::InvalidParameter;
    }

    const Boot::Uefi::Status configuredStatus = g_mock.GetStatuses[callIndex];
    if (configuredStatus != Boot::Uefi::Success)
    {
        if (configuredStatus == Boot::Uefi::BufferTooSmall)
        {
            *memoryMapSize = g_mock.MapSize;
        }
        return configuredStatus;
    }
    if ((*memoryMapSize < g_mock.MapSize) || (memoryMap == nullptr))
    {
        *memoryMapSize = g_mock.MapSize;
        return Boot::Uefi::BufferTooSmall;
    }

    auto* const destination = static_cast<Foundation::UInt8*>(memoryMap);
    for (Foundation::Size index{0U}; index < g_mock.MapSize; ++index)
    {
        destination[index] = g_mock.Map[index];
    }
    if (g_mock.OverrideFirstDescriptor[callIndex] && (g_mock.MapSize >= g_mock.DescriptorSize) &&
        (g_mock.DescriptorSize >= Boot::Uefi::MinimumMemoryDescriptorSize))
    {
        WriteDescriptor(destination, g_mock.DescriptorSize, 0U, g_mock.OverrideTypes[callIndex],
                        g_mock.OverridePhysicalStarts[callIndex], g_mock.OverrideVirtualStarts[callIndex],
                        g_mock.OverridePageCounts[callIndex], g_mock.OverrideAttributes[callIndex]);
    }
    *memoryMapSize = g_mock.ReportedMapSizes[callIndex] == 0U ? g_mock.MapSize : g_mock.ReportedMapSizes[callIndex];
    *mapKey = g_mock.MapKeys[callIndex];
    *descriptorSize = g_mock.DescriptorSize;
    *descriptorVersion = g_mock.DescriptorVersion;
    return Boot::Uefi::Success;
}

Boot::Uefi::Status ASTERIA_UEFI_API MockExitBootServices(Boot::Uefi::Handle, const Foundation::Size mapKey) noexcept
{
    RecordEvent(FirmwareEvent::ExitBootServices);
    const Foundation::Size callIndex = g_mock.ExitCallCount;
    ++g_mock.ExitCallCount;
    if (callIndex >= MaximumMockCalls)
    {
        return Boot::Uefi::InvalidParameter;
    }
    g_mock.ObservedExitKeys[callIndex] = mapKey;
    return g_mock.ExitStatuses[callIndex];
}

bool MockPrepareFinalMemoryMap(Boot::BootInfo& bootInfo) noexcept
{
    RecordEvent(FirmwareEvent::PrepareMemoryMap);
    const Foundation::Size callIndex = g_mock.PreparationCallCount;
    ++g_mock.PreparationCallCount;
    if (callIndex >= MaximumMockCalls)
    {
        return false;
    }
    g_mock.PreparationSawNormalizedMap =
        g_mock.PreparationSawNormalizedMap ||
        ((bootInfo.Signature == Boot::BootInfoSignature) && (bootInfo.Version == Boot::BootInfoVersion) &&
         (bootInfo.SizeBytes == sizeof(Boot::BootInfo)) && (bootInfo.MemoryRegionCount == 1U));
    if ((bootInfo.Acpi.RsdpPhysicalAddress == 0U) && (bootInfo.Acpi.ProcessorCount == 0U) &&
        (bootInfo.Acpi.IoControllerCount == 0U) && (bootInfo.Acpi.McfgPhysicalAddress == 0U) &&
        (bootInfo.Acpi.PciConfigurationRegionCount == 0U))
    {
        ++g_mock.PreparationClearedAcpiCount;
    }
    if (EntropyEquals(bootInfo.Entropy, g_entropy))
    {
        ++g_mock.PreparationSawEntropyCount;
    }
    const Foundation::UInt64 rsdpAddress = 0xAC00U + callIndex;
    bootInfo.Acpi.RsdpPhysicalAddress = rsdpAddress;
    g_mock.LastPublishedRsdpPhysicalAddress = rsdpAddress;
    return g_mock.PreparationStatuses[callIndex];
}

void ResetMock() noexcept
{
    g_mock = MockFirmwareState{};
    ClearBytes(g_smallMap, sizeof(g_smallMap));
    WriteDescriptor(g_smallMap, TestDescriptorSize, 0U, 7U, 0x1000U, 0U, 2U, 8U);
    g_mock.Map = g_smallMap;
    g_mock.MapSize = TestDescriptorSize;
    g_mock.DescriptorSize = TestDescriptorSize;
    g_mock.DescriptorVersion = Boot::Uefi::MemoryDescriptorVersion;
    for (Foundation::Size index{0U}; index < MaximumMockCalls; ++index)
    {
        g_mock.MapKeys[index] = 0x100U + index;
        g_mock.PreparationStatuses[index] = true;
    }
    g_bootServices.GetMemoryMapFunction = MockGetMemoryMap;
    g_bootServices.ExitBootServicesFunction = MockExitBootServices;
}

[[nodiscard]] Boot::Uefi::Handle GetTestImageHandle() noexcept
{
    return reinterpret_cast<Boot::Uefi::Handle>(Foundation::UIntPointer{0x1234U});
}

void TestNormalizationAndSorting(TestContext& runner) noexcept
{
    g_bootInfo.Acpi.RsdpPhysicalAddress = 0xA5A5U;
    g_bootInfo.Acpi.ProcessorCount = 4U;
    g_bootInfo.Acpi.McfgPhysicalAddress = 0x5A5AU;
    g_bootInfo.Acpi.PciConfigurationRegionCount = 8U;
    ClearBytes(g_smallMap, sizeof(g_smallMap));
    WriteDescriptor(g_smallMap, TestDescriptorSize, 0U, 11U, 0x5000U, 0U, 1U, 1U);
    WriteDescriptor(g_smallMap, TestDescriptorSize, 1U, 7U, 0x1000U, 0U, 2U, 2U);
    WriteDescriptor(g_smallMap, TestDescriptorSize, 2U, 5U, 0x3000U, 0x9000U, 2U, 4U);
    const bool valid = Boot::Uefi::NormalizeMemoryMap(g_smallMap, TestDescriptorSize * 3U, TestDescriptorSize,
                                                      Boot::Uefi::MemoryDescriptorVersion, g_bootInfo);
    ASTERIA_EXPECT(runner, "variable-stride memory map is accepted", valid);
    ASTERIA_EXPECT(runner, "boot handoff metadata is normalized", g_bootInfo.Signature == Boot::BootInfoSignature,
                   g_bootInfo.Version == Boot::BootInfoVersion, g_bootInfo.SizeBytes == sizeof(Boot::BootInfo),
                   g_bootInfo.MemoryRegionCount == 3U, g_bootInfo.FirmwareDescriptorSize == TestDescriptorSize,
                   g_bootInfo.FirmwareMemoryMapSizeBytes == TestDescriptorSize * 3U,
                   g_bootInfo.Acpi.RsdpPhysicalAddress == 0U, g_bootInfo.Acpi.ProcessorCount == 0U,
                   g_bootInfo.Acpi.McfgPhysicalAddress == 0U, g_bootInfo.Acpi.PciConfigurationRegionCount == 0U);
    ASTERIA_EXPECT(runner, "regions are sorted without losing type or virtual-address data",
                   g_bootInfo.MemoryRegions[0U].PhysicalStart == 0x1000U,
                   g_bootInfo.MemoryRegions[0U].Kind == Boot::PhysicalMemoryKind::Conventional,
                   g_bootInfo.MemoryRegions[1U].PhysicalStart == 0x3000U,
                   g_bootInfo.MemoryRegions[1U].VirtualStart == 0x9000U,
                   g_bootInfo.MemoryRegions[1U].Kind == Boot::PhysicalMemoryKind::RuntimeServices,
                   g_bootInfo.MemoryRegions[2U].PhysicalStart == 0x5000U,
                   g_bootInfo.MemoryRegions[2U].Kind == Boot::PhysicalMemoryKind::MemoryMappedIo);
}

void TestMemoryTypeMapping(TestContext& runner) noexcept
{
    constexpr Boot::PhysicalMemoryKind ExpectedKinds[]{
        Boot::PhysicalMemoryKind::Reserved,        Boot::PhysicalMemoryKind::Loader,
        Boot::PhysicalMemoryKind::Loader,          Boot::PhysicalMemoryKind::BootServices,
        Boot::PhysicalMemoryKind::BootServices,    Boot::PhysicalMemoryKind::RuntimeServices,
        Boot::PhysicalMemoryKind::RuntimeServices, Boot::PhysicalMemoryKind::Conventional,
        Boot::PhysicalMemoryKind::Unusable,        Boot::PhysicalMemoryKind::AcpiReclaimable,
        Boot::PhysicalMemoryKind::AcpiNonVolatile, Boot::PhysicalMemoryKind::MemoryMappedIo,
        Boot::PhysicalMemoryKind::MemoryMappedIo,  Boot::PhysicalMemoryKind::PalCode,
        Boot::PhysicalMemoryKind::Persistent,      Boot::PhysicalMemoryKind::Unaccepted,
    };
    ClearBytes(g_smallMap, sizeof(g_smallMap));
    for (Foundation::Size index{0U}; index < 16U; ++index)
    {
        WriteDescriptor(g_smallMap, Boot::Uefi::MinimumMemoryDescriptorSize, index,
                        static_cast<Foundation::UInt32>(index), static_cast<Foundation::UInt64>((index + 1U) << 12U),
                        0U, 1U, 0U);
    }
    bool valid = Boot::Uefi::NormalizeMemoryMap(g_smallMap, Boot::Uefi::MinimumMemoryDescriptorSize * 16U,
                                                Boot::Uefi::MinimumMemoryDescriptorSize,
                                                Boot::Uefi::MemoryDescriptorVersion, g_bootInfo);
    for (Foundation::Size index{0U}; valid && (index < 16U); ++index)
    {
        valid = g_bootInfo.MemoryRegions[index].Kind == ExpectedKinds[index];
    }
    ASTERIA_EXPECT(runner, "all standard UEFI memory types map to fixed boot kinds", valid);

    constexpr Foundation::UInt32 reservedNamespaceTypes[]{
        0x7000'0000U,
        0x7FFF'FFFFU,
        0x8000'0000U,
        0xFFFF'FFFFU,
    };
    bool reservedNamespacesAccepted{true};
    for (const Foundation::UInt32 firmwareType : reservedNamespaceTypes)
    {
        WriteDescriptor(g_smallMap, TestDescriptorSize, 0U, firmwareType, 0x1000U, 0U, 1U, 0U);
        reservedNamespacesAccepted = reservedNamespacesAccepted &&
                                     Boot::Uefi::NormalizeMemoryMap(g_smallMap, TestDescriptorSize, TestDescriptorSize,
                                                                    Boot::Uefi::MemoryDescriptorVersion, g_bootInfo) &&
                                     (g_bootInfo.MemoryRegions[0U].Kind == Boot::PhysicalMemoryKind::Reserved);
    }
    ASTERIA_EXPECT(runner, "defined OEM and OS memory types remain reserved", reservedNamespacesAccepted);

    constexpr Foundation::UInt32 invalidFirmwareTypes[]{
        16U,
        0x1234U,
        0x6FFF'FFFFU,
    };
    bool invalidTypeNamespaceRejected{true};
    for (const Foundation::UInt32 firmwareType : invalidFirmwareTypes)
    {
        WriteDescriptor(g_smallMap, TestDescriptorSize, 0U, firmwareType, 0x1000U, 0U, 1U, 0U);
        invalidTypeNamespaceRejected =
            invalidTypeNamespaceRejected &&
            !Boot::Uefi::NormalizeMemoryMap(g_smallMap, TestDescriptorSize, TestDescriptorSize,
                                            Boot::Uefi::MemoryDescriptorVersion, g_bootInfo);
    }
    ASTERIA_EXPECT(runner, "the undefined gap between standard and OEM memory types is rejected",
                   invalidTypeNamespaceRejected);
}

void TestBoundaryMemoryMaps(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 FullAddressSpacePages{Foundation::UInt64{1U} << 52U};
    WriteDescriptor(g_smallMap, TestDescriptorSize, 0U, 0U, 0xFFFF'FFFF'FFFF'F000ULL, 0U, 1U, 0U);
    bool valid = Boot::Uefi::NormalizeMemoryMap(g_smallMap, TestDescriptorSize, TestDescriptorSize,
                                                Boot::Uefi::MemoryDescriptorVersion, g_bootInfo);
    ASTERIA_EXPECT(runner, "the final physical page is represented without endpoint wrap", valid,
                   g_bootInfo.MemoryRegions[0U].PhysicalStart == 0xFFFF'FFFF'FFFF'F000ULL,
                   g_bootInfo.MemoryRegions[0U].PageCount == 1U);

    WriteDescriptor(g_smallMap, TestDescriptorSize, 0U, 0U, 0U, 0U, FullAddressSpacePages, 0U);
    valid = Boot::Uefi::NormalizeMemoryMap(g_smallMap, TestDescriptorSize, TestDescriptorSize,
                                           Boot::Uefi::MemoryDescriptorVersion, g_bootInfo);
    ASTERIA_EXPECT(runner, "a descriptor spanning the complete 64-bit physical space is accepted", valid,
                   g_bootInfo.MemoryRegions[0U].PhysicalStart == 0U,
                   g_bootInfo.MemoryRegions[0U].PageCount == FullAddressSpacePages);
}

// The span the loader asks KernelQuery::MetadataArenaPages for: the end of the highest managed
// range, in whole GiB.
void TestManagedSpan(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 WriteBack{8U};
    constexpr Foundation::UInt64 Runtime{Foundation::UInt64{1U} << 63U};
    constexpr Foundation::UInt64 SpecificPurpose{Foundation::UInt64{1U} << 18U};
    constexpr Foundation::UInt64 GiB{Foundation::UInt64{1U} << 30U};
    const auto span = [](const Foundation::Size count) noexcept
    {
        return Boot::Uefi::ComputeManagedSpanGiB(g_smallMap, count * TestDescriptorSize, TestDescriptorSize);
    };
    WriteDescriptor(g_smallMap, TestDescriptorSize, 0U, 7U, 0U, 0U, 159U, WriteBack | 1U);
    WriteDescriptor(g_smallMap, TestDescriptorSize, 1U, 4U, 4U * GiB, 0U, 256U, WriteBack);
    // Not managed: uncached, runtime, specific-purpose, runtime-services
    // and loader memory, and a range whose end wraps.
    WriteDescriptor(g_smallMap, TestDescriptorSize, 2U, 7U, 64U * GiB, 0U, 1U, 1U);
    WriteDescriptor(g_smallMap, TestDescriptorSize, 3U, 7U, 65U * GiB, 0U, 1U, WriteBack | Runtime);
    WriteDescriptor(g_smallMap, TestDescriptorSize, 4U, 7U, 66U * GiB, 0U, 1U, WriteBack | SpecificPurpose);
    WriteDescriptor(g_smallMap, TestDescriptorSize, 5U, 6U, 67U * GiB, 0U, 1U, WriteBack);
    WriteDescriptor(g_smallMap, TestDescriptorSize, 6U, 2U, 68U * GiB, 0U, 1U, WriteBack);
    WriteDescriptor(g_smallMap, TestDescriptorSize, 7U, 7U, 0xFFFF'FFFF'FFFF'F000ULL, 0U, 2U, WriteBack);
    const Foundation::UInt64 mixed = span(8U);
    WriteDescriptor(g_smallMap, TestDescriptorSize, 1U, 3U, GiB, 0U, GiB / 4096U, WriteBack);
    const Foundation::UInt64 exact = span(2U);
    ASTERIA_EXPECT(runner, "the managed span ends at the highest write-back conventional or boot-services byte",
                   mixed == 5U, exact == 2U, span(1U) == 1U, span(0U) == 0U,
                   Boot::Uefi::ComputeManagedSpanGiB(nullptr, TestDescriptorSize, TestDescriptorSize) == 0U,
                   Boot::Uefi::ComputeManagedSpanGiB(g_smallMap, TestDescriptorSize + 1U, TestDescriptorSize) == 0U,
                   Boot::Uefi::ComputeManagedSpanGiB(g_smallMap, 32U, 32U) == 0U);
}

void TestInvalidMemoryMaps(TestContext& runner) noexcept
{
    WriteDescriptor(g_smallMap, TestDescriptorSize, 0U, 7U, 0x1000U, 0U, 1U, 0U);
    ASTERIA_EXPECT(runner, "null memory map is rejected",
                   !Boot::Uefi::NormalizeMemoryMap(nullptr, TestDescriptorSize, TestDescriptorSize,
                                                   Boot::Uefi::MemoryDescriptorVersion, g_bootInfo));
    ASTERIA_EXPECT(runner, "empty memory map is rejected",
                   !Boot::Uefi::NormalizeMemoryMap(g_smallMap, 0U, TestDescriptorSize,
                                                   Boot::Uefi::MemoryDescriptorVersion, g_bootInfo));
    ASTERIA_EXPECT(runner, "short descriptor is rejected",
                   !Boot::Uefi::NormalizeMemoryMap(g_smallMap, TestDescriptorSize,
                                                   Boot::Uefi::MinimumMemoryDescriptorSize - 1U,
                                                   Boot::Uefi::MemoryDescriptorVersion, g_bootInfo));
    ASTERIA_EXPECT(runner, "unknown descriptor version is rejected",
                   !Boot::Uefi::NormalizeMemoryMap(g_smallMap, TestDescriptorSize, TestDescriptorSize,
                                                   Boot::Uefi::MemoryDescriptorVersion + 1U, g_bootInfo));
    ASTERIA_EXPECT(runner, "partial final descriptor is rejected",
                   !Boot::Uefi::NormalizeMemoryMap(g_smallMap, TestDescriptorSize - 1U, TestDescriptorSize,
                                                   Boot::Uefi::MemoryDescriptorVersion, g_bootInfo));

    WriteDescriptor(g_smallMap, TestDescriptorSize, 0U, 7U, 0x1000U, 0U, 0U, 0U);
    ASTERIA_EXPECT(runner, "zero-page descriptor is rejected",
                   !Boot::Uefi::NormalizeMemoryMap(g_smallMap, TestDescriptorSize, TestDescriptorSize,
                                                   Boot::Uefi::MemoryDescriptorVersion, g_bootInfo));
    WriteDescriptor(g_smallMap, TestDescriptorSize, 0U, 7U, 0x1001U, 0U, 1U, 0U);
    ASTERIA_EXPECT(runner, "unaligned physical range is rejected",
                   !Boot::Uefi::NormalizeMemoryMap(g_smallMap, TestDescriptorSize, TestDescriptorSize,
                                                   Boot::Uefi::MemoryDescriptorVersion, g_bootInfo));
    WriteDescriptor(g_smallMap, TestDescriptorSize, 0U, 7U, 0x1000U, 0x1001U, 1U, 0U);
    ASTERIA_EXPECT(runner, "unaligned nonzero virtual range is rejected",
                   !Boot::Uefi::NormalizeMemoryMap(g_smallMap, TestDescriptorSize, TestDescriptorSize,
                                                   Boot::Uefi::MemoryDescriptorVersion, g_bootInfo));
    WriteDescriptor(g_smallMap, TestDescriptorSize, 0U, 7U, 0x1000U, 0xFFFF'FFFF'FFFF'F000ULL, 2U, 0U);
    ASTERIA_EXPECT(runner, "nonzero virtual end overflow is rejected",
                   !Boot::Uefi::NormalizeMemoryMap(g_smallMap, TestDescriptorSize, TestDescriptorSize,
                                                   Boot::Uefi::MemoryDescriptorVersion, g_bootInfo));
    WriteDescriptor(g_smallMap, TestDescriptorSize, 0U, 7U, 0xFFFF'FFFF'FFFF'F000ULL, 0U, 2U, 0U);
    ASTERIA_EXPECT(runner, "physical end overflow is rejected",
                   !Boot::Uefi::NormalizeMemoryMap(g_smallMap, TestDescriptorSize, TestDescriptorSize,
                                                   Boot::Uefi::MemoryDescriptorVersion, g_bootInfo));

    WriteDescriptor(g_smallMap, TestDescriptorSize, 0U, 7U, 0x1000U, 0U, 3U, 0U);
    WriteDescriptor(g_smallMap, TestDescriptorSize, 1U, 7U, 0x2000U, 0U, 1U, 0U);
    ASTERIA_EXPECT(runner, "overlapping normalized ranges are rejected",
                   !Boot::Uefi::NormalizeMemoryMap(g_smallMap, TestDescriptorSize * 2U, TestDescriptorSize,
                                                   Boot::Uefi::MemoryDescriptorVersion, g_bootInfo));

    for (Foundation::Size index{0U}; index <= Boot::MaximumMemoryRegions; ++index)
    {
        WriteDescriptor(g_largeMap, Boot::Uefi::MinimumMemoryDescriptorSize, index, 7U,
                        static_cast<Foundation::UInt64>((index + 1U) << 12U), 0U, 1U, 0U);
    }
    ASTERIA_EXPECT(runner, "normalized region capacity is enforced",
                   !Boot::Uefi::NormalizeMemoryMap(g_largeMap, sizeof(g_largeMap),
                                                   Boot::Uefi::MinimumMemoryDescriptorSize,
                                                   Boot::Uefi::MemoryDescriptorVersion, g_bootInfo),
                   g_bootInfo.MemoryRegionCount == 0U);
}

void TestSuccessfulTransition(TestContext& runner) noexcept
{
    ResetMock();
    const Boot::Uefi::BootTransitionResult result = Boot::Uefi::ExitBootServicesAndCaptureMemoryMap(
        GetTestImageHandle(), g_bootServices, g_largeMap, sizeof(g_largeMap), g_bootInfo);
    ASTERIA_EXPECT(runner, "successful transition reports one completed exit attempt", result.IsSuccessful(),
                   result.HasExitedBootServices(), result.ExitAttempts == 1U,
                   result.FirmwareStatus == Boot::Uefi::Success);
    ASTERIA_EXPECT(runner, "final memory map is immediately followed by ExitBootServices", g_mock.EventCount == 2U,
                   g_mock.Events[0U] == FirmwareEvent::GetMemoryMap,
                   g_mock.Events[1U] == FirmwareEvent::ExitBootServices,
                   g_mock.ObservedExitKeys[0U] == g_mock.MapKeys[0U]);
    ASTERIA_EXPECT(runner, "successful exit publishes the normalized final map", g_bootInfo.MemoryRegionCount == 1U,
                   g_bootInfo.MemoryRegions[0U].Kind == Boot::PhysicalMemoryKind::Conventional);
}

void TestPreparedTransition(TestContext& runner) noexcept
{
    ResetMock();
    const Boot::Uefi::BootTransitionResult prepared = Boot::Uefi::ExitBootServicesAndCaptureMemoryMap(
        GetTestImageHandle(), g_bootServices, g_largeMap, sizeof(g_largeMap), g_bootInfo, nullptr,
        MockPrepareFinalMemoryMap);
    ASTERIA_EXPECT(
        runner, "normalized-map preparation completes before ExitBootServices", prepared.IsSuccessful(),
        prepared.HasExitedBootServices(), g_mock.PreparationCallCount == 1U, g_mock.PreparationSawNormalizedMap,
        g_mock.PreparationClearedAcpiCount == 1U, g_bootInfo.Acpi.RsdpPhysicalAddress == 0xAC00U,
        g_mock.EventCount == 3U, g_mock.Events[0U] == FirmwareEvent::GetMemoryMap,
        g_mock.Events[1U] == FirmwareEvent::PrepareMemoryMap, g_mock.Events[2U] == FirmwareEvent::ExitBootServices);

    ResetMock();
    g_mock.ExitStatuses[0U] = Boot::Uefi::InvalidParameter;
    const Boot::Uefi::BootTransitionResult retried = Boot::Uefi::ExitBootServicesAndCaptureMemoryMap(
        GetTestImageHandle(), g_bootServices, g_largeMap, sizeof(g_largeMap), g_bootInfo, nullptr,
        MockPrepareFinalMemoryMap);
    ASTERIA_EXPECT(
        runner, "each stale-key retry revalidates before its exit attempt", retried.IsSuccessful(),
        retried.ExitAttempts == 2U, g_mock.PreparationCallCount == 2U, g_mock.PreparationClearedAcpiCount == 2U,
        g_mock.LastPublishedRsdpPhysicalAddress == 0xAC01U, g_bootInfo.Acpi.RsdpPhysicalAddress == 0xAC01U,
        g_mock.EventCount == 6U, g_mock.Events[0U] == FirmwareEvent::GetMemoryMap,
        g_mock.Events[1U] == FirmwareEvent::PrepareMemoryMap, g_mock.Events[2U] == FirmwareEvent::ExitBootServices,
        g_mock.Events[3U] == FirmwareEvent::GetMemoryMap, g_mock.Events[4U] == FirmwareEvent::PrepareMemoryMap,
        g_mock.Events[5U] == FirmwareEvent::ExitBootServices);

    ResetMock();
    g_mock.PreparationStatuses[0U] = false;
    const Boot::Uefi::BootTransitionResult rejected = Boot::Uefi::ExitBootServicesAndCaptureMemoryMap(
        GetTestImageHandle(), g_bootServices, g_largeMap, sizeof(g_largeMap), g_bootInfo, nullptr,
        MockPrepareFinalMemoryMap);
    ASTERIA_EXPECT(runner, "initial preparation rejection makes no exit attempt",
                   rejected.Error == Boot::Uefi::BootTransitionError::MemoryMapPreparationRejected,
                   rejected.ExitAttempts == 0U, g_mock.GetCallCount == 1U, g_mock.PreparationCallCount == 1U,
                   g_mock.ExitCallCount == 0U, g_bootInfo.MemoryRegionCount == 0U,
                   g_bootInfo.Acpi.RsdpPhysicalAddress == 0U);

    ResetMock();
    g_mock.ExitStatuses[0U] = Boot::Uefi::InvalidParameter;
    g_mock.PreparationStatuses[1U] = false;
    const Boot::Uefi::BootTransitionResult retryRejected = Boot::Uefi::ExitBootServicesAndCaptureMemoryMap(
        GetTestImageHandle(), g_bootServices, g_largeMap, sizeof(g_largeMap), g_bootInfo, nullptr,
        MockPrepareFinalMemoryMap);
    ASTERIA_EXPECT(runner, "retry preparation rejection preserves the prior exit-attempt count",
                   retryRejected.Error == Boot::Uefi::BootTransitionError::MemoryMapPreparationRejected,
                   retryRejected.ExitAttempts == 1U, g_mock.GetCallCount == 2U, g_mock.PreparationCallCount == 2U,
                   g_mock.ExitCallCount == 1U, g_bootInfo.MemoryRegionCount == 0U,
                   g_bootInfo.Acpi.RsdpPhysicalAddress == 0U);
}

void TestEntropyPublication(TestContext& runner) noexcept
{
    g_entropy = Boot::BootEntropy{};
    for (Foundation::Size index{0U}; index < Boot::BootEntropySeedBytes; ++index)
    {
        g_entropy.Seed[index] = static_cast<Foundation::UInt8>(0x40U + index);
    }
    g_entropy.Sources = Boot::GetBootEntropySourceMask(Boot::BootEntropySource::CycleCounterJitter);

    ResetMock();
    g_mock.ExitStatuses[0U] = Boot::Uefi::InvalidParameter;
    const Boot::Uefi::BootTransitionResult published = Boot::Uefi::ExitBootServicesAndCaptureMemoryMap(
        GetTestImageHandle(), g_bootServices, g_largeMap, sizeof(g_largeMap), g_bootInfo, nullptr,
        MockPrepareFinalMemoryMap, &g_entropy);
    ASTERIA_EXPECT(runner,
                   "every candidate carries the entropy record before its preparation, and the handoff keeps it",
                   published.IsSuccessful(), published.ExitAttempts == 2U, g_mock.PreparationCallCount == 2U,
                   g_mock.PreparationSawEntropyCount == 2U, EntropyEquals(g_bootInfo.Entropy, g_entropy));

    ResetMock();
    g_mock.PreparationStatuses[0U] = false;
    const Boot::Uefi::BootTransitionResult rejected = Boot::Uefi::ExitBootServicesAndCaptureMemoryMap(
        GetTestImageHandle(), g_bootServices, g_largeMap, sizeof(g_largeMap), g_bootInfo, nullptr,
        MockPrepareFinalMemoryMap, &g_entropy);
    ASTERIA_EXPECT(runner, "a rejected candidate leaves no entropy in the handoff",
                   rejected.Error == Boot::Uefi::BootTransitionError::MemoryMapPreparationRejected,
                   g_mock.PreparationSawEntropyCount == 1U, EntropyEquals(g_bootInfo.Entropy, Boot::BootEntropy{}));

    ResetMock();
    const Boot::Uefi::BootTransitionResult withoutEntropy = Boot::Uefi::ExitBootServicesAndCaptureMemoryMap(
        GetTestImageHandle(), g_bootServices, g_largeMap, sizeof(g_largeMap), g_bootInfo);
    ASTERIA_EXPECT(runner, "without a record the handoff entropy stays zero, which no kernel accepts",
                   withoutEntropy.IsSuccessful(), EntropyEquals(g_bootInfo.Entropy, Boot::BootEntropy{}));

    Boot::BootEntropy unpublished = g_entropy;
    unpublished.Sources = Boot::GetBootEntropySourceMask(Boot::BootEntropySource::FirmwareState);
    Boot::BootEntropy degenerate = g_entropy;
    for (Foundation::UInt8& value : degenerate.Seed)
    {
        value = 0U;
    }
    const Boot::BootEntropy* const unusable[]{&unpublished, &degenerate};
    for (const Boot::BootEntropy* const entropy : unusable)
    {
        ResetMock();
        g_bootInfo.Entropy = g_entropy;
        const Boot::Uefi::BootTransitionResult result = Boot::Uefi::ExitBootServicesAndCaptureMemoryMap(
            GetTestImageHandle(), g_bootServices, g_largeMap, sizeof(g_largeMap), g_bootInfo, nullptr,
            MockPrepareFinalMemoryMap, entropy);
        ASTERIA_EXPECT(runner, "an unpublished or degenerate record is rejected before any firmware call",
                       result.Error == Boot::Uefi::BootTransitionError::EntropyRejected,
                       !result.HasExitedBootServices(), result.ExitAttempts == 0U,
                       result.FirmwareStatus == Boot::Uefi::InvalidParameter, result.RequiredMemoryMapBytes == 0U,
                       g_mock.EventCount == 0U, EntropyEquals(g_bootInfo.Entropy, Boot::BootEntropy{}));
    }
}

void TestRetryTransition(TestContext& runner) noexcept
{
    ResetMock();
    g_mock.ExitStatuses[0U] = Boot::Uefi::InvalidParameter;
    g_mock.OverrideFirstDescriptor[1U] = true;
    g_mock.OverrideTypes[1U] = 4U;
    g_mock.OverridePhysicalStarts[1U] = 0x2000U;
    g_mock.OverrideVirtualStarts[1U] = 0U;
    g_mock.OverridePageCounts[1U] = 3U;
    g_mock.OverrideAttributes[1U] = 0x55U;
    const Boot::Uefi::BootTransitionResult result = Boot::Uefi::ExitBootServicesAndCaptureMemoryMap(
        GetTestImageHandle(), g_bootServices, g_largeMap, sizeof(g_largeMap), g_bootInfo);
    ASTERIA_EXPECT(runner, "invalid map key publishes only the successful retry snapshot", result.IsSuccessful(),
                   result.HasExitedBootServices(), result.ExitAttempts == 2U, g_mock.GetCallCount == 2U,
                   g_mock.ExitCallCount == 2U, g_bootInfo.MemoryRegionCount == 1U,
                   g_bootInfo.MemoryRegions[0U].PhysicalStart == 0x2000U, g_bootInfo.MemoryRegions[0U].PageCount == 3U,
                   g_bootInfo.MemoryRegions[0U].Attributes == 0x55U, g_bootInfo.MemoryRegions[0U].FirmwareType == 4U,
                   g_bootInfo.MemoryRegions[0U].Kind == Boot::PhysicalMemoryKind::BootServices);
    ASTERIA_EXPECT(
        runner, "retry path invokes no unrelated firmware callback", g_mock.EventCount == 4U,
        g_mock.Events[0U] == FirmwareEvent::GetMemoryMap, g_mock.Events[1U] == FirmwareEvent::ExitBootServices,
        g_mock.Events[2U] == FirmwareEvent::GetMemoryMap, g_mock.Events[3U] == FirmwareEvent::ExitBootServices,
        g_mock.ObservedExitKeys[1U] == g_mock.MapKeys[1U]);

    ResetMock();
    for (Foundation::Size index{0U}; index < Boot::Uefi::MaximumExitBootServicesAttempts; ++index)
    {
        g_mock.ExitStatuses[index] = Boot::Uefi::InvalidParameter;
    }
    const Boot::Uefi::BootTransitionResult exhausted = Boot::Uefi::ExitBootServicesAndCaptureMemoryMap(
        GetTestImageHandle(), g_bootServices, g_largeMap, sizeof(g_largeMap), g_bootInfo);
    ASTERIA_EXPECT(
        runner, "repeated stale keys stop at the fixed retry bound",
        exhausted.Error == Boot::Uefi::BootTransitionError::ExitBootServicesRetryLimit,
        !exhausted.HasExitedBootServices(), exhausted.ExitAttempts == Boot::Uefi::MaximumExitBootServicesAttempts,
        g_mock.EventCount == (Boot::Uefi::MaximumExitBootServicesAttempts * 2U), g_bootInfo.MemoryRegionCount == 0U);

    ResetMock();
    g_mock.ExitStatuses[0U] = Boot::Uefi::InvalidParameter;
    g_mock.OverrideFirstDescriptor[1U] = true;
    g_mock.OverrideTypes[1U] = 7U;
    g_mock.OverridePhysicalStarts[1U] = 0x2000U;
    g_mock.OverridePageCounts[1U] = 0U;
    const Boot::Uefi::BootTransitionResult invalidRetry = Boot::Uefi::ExitBootServicesAndCaptureMemoryMap(
        GetTestImageHandle(), g_bootServices, g_largeMap, sizeof(g_largeMap), g_bootInfo);
    ASTERIA_EXPECT(runner, "malformed retry snapshot preserves the prior exit-attempt state",
                   invalidRetry.Error == Boot::Uefi::BootTransitionError::InvalidMemoryMap,
                   invalidRetry.ExitAttempts == 1U, g_mock.GetCallCount == 2U, g_mock.ExitCallCount == 1U,
                   g_bootInfo.MemoryRegionCount == 0U);

    ResetMock();
    g_mock.ExitStatuses[0U] = Boot::Uefi::InvalidParameter;
    g_mock.ReportedMapSizes[1U] = sizeof(g_largeMap) + 1U;
    const Boot::Uefi::BootTransitionResult oversizedRetry = Boot::Uefi::ExitBootServicesAndCaptureMemoryMap(
        GetTestImageHandle(), g_bootServices, g_largeMap, sizeof(g_largeMap), g_bootInfo);
    ASTERIA_EXPECT(runner, "oversized retry snapshot preserves the prior exit-attempt state",
                   oversizedRetry.Error == Boot::Uefi::BootTransitionError::MemoryMapBufferTooSmall,
                   oversizedRetry.ExitAttempts == 1U, g_mock.GetCallCount == 2U, g_mock.ExitCallCount == 1U,
                   g_bootInfo.MemoryRegionCount == 0U);
}

void TestTransitionFailures(TestContext& runner) noexcept
{
    ResetMock();
    const Boot::Uefi::BootTransitionResult invalidArgument = Boot::Uefi::ExitBootServicesAndCaptureMemoryMap(
        nullptr, g_bootServices, g_largeMap, sizeof(g_largeMap), g_bootInfo);
    ASTERIA_EXPECT(runner, "invalid transition arguments invoke no firmware callback",
                   invalidArgument.Error == Boot::Uefi::BootTransitionError::InvalidArgument, g_mock.EventCount == 0U);

    ResetMock();
    g_mock.MapSize = sizeof(g_largeMap) + 1U;
    const Boot::Uefi::BootTransitionResult tooSmall = Boot::Uefi::ExitBootServicesAndCaptureMemoryMap(
        GetTestImageHandle(), g_bootServices, g_largeMap, sizeof(g_largeMap), g_bootInfo);
    ASTERIA_EXPECT(runner, "oversized final map is reported without an exit attempt",
                   tooSmall.Error == Boot::Uefi::BootTransitionError::MemoryMapBufferTooSmall,
                   tooSmall.RequiredMemoryMapBytes == (sizeof(g_largeMap) + 1U), g_mock.ExitCallCount == 0U);

    ResetMock();
    g_mock.ReportedMapSizes[0U] = sizeof(g_largeMap) + 1U;
    const Boot::Uefi::BootTransitionResult invalidSuccessSize = Boot::Uefi::ExitBootServicesAndCaptureMemoryMap(
        GetTestImageHandle(), g_bootServices, g_largeMap, sizeof(g_largeMap), g_bootInfo);
    ASTERIA_EXPECT(runner, "success with an oversized reported map is rejected before exit",
                   invalidSuccessSize.Error == Boot::Uefi::BootTransitionError::MemoryMapBufferTooSmall,
                   invalidSuccessSize.RequiredMemoryMapBytes == (sizeof(g_largeMap) + 1U), g_mock.GetCallCount == 1U,
                   g_mock.ExitCallCount == 0U);

    ResetMock();
    g_mock.GetStatuses[0U] = Boot::Uefi::DeviceError;
    const Boot::Uefi::BootTransitionResult getFailure = Boot::Uefi::ExitBootServicesAndCaptureMemoryMap(
        GetTestImageHandle(), g_bootServices, g_largeMap, sizeof(g_largeMap), g_bootInfo);
    ASTERIA_EXPECT(runner, "GetMemoryMap failure is preserved",
                   getFailure.Error == Boot::Uefi::BootTransitionError::GetMemoryMapFailed,
                   getFailure.FirmwareStatus == Boot::Uefi::DeviceError, g_mock.ExitCallCount == 0U);

    ResetMock();
    g_mock.ExitStatuses[0U] = Boot::Uefi::Unsupported;
    const Boot::Uefi::BootTransitionResult exitFailure = Boot::Uefi::ExitBootServicesAndCaptureMemoryMap(
        GetTestImageHandle(), g_bootServices, g_largeMap, sizeof(g_largeMap), g_bootInfo);
    ASTERIA_EXPECT(runner, "non-retryable ExitBootServices failure is preserved",
                   exitFailure.Error == Boot::Uefi::BootTransitionError::ExitBootServicesFailed,
                   exitFailure.FirmwareStatus == Boot::Uefi::Unsupported, !exitFailure.HasExitedBootServices());

    ResetMock();
    WriteDescriptor(g_smallMap, TestDescriptorSize, 0U, 7U, 0x1000U, 0U, 0U, 0U);
    const Boot::Uefi::BootTransitionResult invalidMap = Boot::Uefi::ExitBootServicesAndCaptureMemoryMap(
        GetTestImageHandle(), g_bootServices, g_largeMap, sizeof(g_largeMap), g_bootInfo);
    ASTERIA_EXPECT(runner, "malformed final map is rejected before an exit attempt",
                   invalidMap.Error == Boot::Uefi::BootTransitionError::InvalidMemoryMap,
                   !invalidMap.HasExitedBootServices(), invalidMap.ExitAttempts == 0U, g_mock.EventCount == 1U,
                   g_mock.ExitCallCount == 0U, g_bootInfo.MemoryRegionCount == 0U);

    // An RSDP the map does not list is read, and its missing signature is
    // the reason the result carries, so the boot can report it.
    alignas(8) static constinit Foundation::UInt8 blankRsdp[36U]{};
    const Boot::Uefi::AcpiCaptureRequest capture{
        .RsdpPointer = blankRsdp,
        .TargetArchitecture = Boot::Acpi::AcpiTargetArchitecture::Amd64,
        .Reserved = 0U,
    };
    ResetMock();
    const Boot::Uefi::BootTransitionResult acpiRejected = Boot::Uefi::ExitBootServicesAndCaptureMemoryMap(
        GetTestImageHandle(), g_bootServices, g_largeMap, sizeof(g_largeMap), g_bootInfo, &capture);
    ASTERIA_EXPECT(runner, "a rejected ACPI topology carries the parser's reason and makes no exit attempt",
                   acpiRejected.Error == Boot::Uefi::BootTransitionError::AcpiTopologyRejected,
                   acpiRejected.AcpiError == Boot::Acpi::AcpiTableParseError::InvalidRsdp,
                   acpiRejected.ExitAttempts == 0U, g_mock.ExitCallCount == 0U, g_bootInfo.MemoryRegionCount == 0U);
    ASTERIA_EXPECT(runner, "a transition that fails elsewhere carries no ACPI reason",
                   invalidMap.AcpiError == Boot::Acpi::AcpiTableParseError::None);
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"BootTransitionTests"};
    Asteria::Test::TestNormalizationAndSorting(runner);
    Asteria::Test::TestMemoryTypeMapping(runner);
    Asteria::Test::TestBoundaryMemoryMaps(runner);
    Asteria::Test::TestManagedSpan(runner);
    Asteria::Test::TestInvalidMemoryMaps(runner);
    Asteria::Test::TestSuccessfulTransition(runner);
    Asteria::Test::TestPreparedTransition(runner);
    Asteria::Test::TestEntropyPublication(runner);
    Asteria::Test::TestRetryTransition(runner);
    Asteria::Test::TestTransitionFailures(runner);
    return runner.Finish();
}
