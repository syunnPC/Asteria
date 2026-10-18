// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Foundation/Types.hpp"
#include "Platform/Ia64/Firmware/SalSystemTableLocator.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Boot::Uefi::ConfigurationTable;
using Boot::Uefi::Guid;
using Platform::Ia64::Firmware::MaximumFirmwareConfigurationTables;
using Platform::Ia64::Firmware::SalSystemTableGuid;
using Platform::Ia64::Firmware::SalSystemTableLocateError;
using Platform::Ia64::Firmware::SalSystemTableLocator;

constexpr Foundation::Size PageSizeBytes{4096U};
constexpr Foundation::Size Ia64RuntimeAlignmentBytes{8192U};
constexpr Foundation::Size TableStorageSize{PageSizeBytes * 2U};
constexpr Foundation::Size DefaultTableOffset{128U};
constexpr Foundation::Size MaximumMapBytes{512U};
constexpr Foundation::Size MaximumMemoryDescriptorCount{4096U};
constexpr Foundation::UInt64 RuntimeAttribute{Foundation::UInt64{1U} << 63U};
constexpr Foundation::UInt64 ReadProtectedAttribute{Foundation::UInt64{1U} << 13U};
constexpr Foundation::UInt64 WriteBackAttribute{Foundation::UInt64{1U} << 3U};
constexpr Foundation::UInt32 BootServicesDataType{4U};
constexpr Foundation::UInt32 RuntimeCodeType{5U};
constexpr Foundation::UInt32 RuntimeDataType{6U};
constexpr Foundation::UInt32 AcpiReclaimType{9U};
constexpr Foundation::UInt32 AcpiNonVolatileType{10U};

alignas(Ia64RuntimeAlignmentBytes) Foundation::UInt8 g_tableStorage[TableStorageSize];

void WriteLittleEndian32(Foundation::UInt8* const bytes, const Foundation::UInt32 value) noexcept
{
    for (Foundation::UInt8 index{0U}; index < 4U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> static_cast<Foundation::UInt8>(index * 8U));
    }
}

void WriteLittleEndian64(Foundation::UInt8* const bytes, const Foundation::UInt64 value) noexcept
{
    for (Foundation::UInt8 index{0U}; index < 8U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> static_cast<Foundation::UInt8>(index * 8U));
    }
}

[[nodiscard]] Foundation::UInt64 TableStorageAddress() noexcept
{
    return static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(g_tableStorage));
}

[[nodiscard]] Foundation::UInt8* DefaultTablePointer() noexcept
{
    return g_tableStorage + DefaultTableOffset;
}

class MemoryMapFixture final
{
public:
    explicit MemoryMapFixture(const Foundation::Size descriptorSize = 40U) noexcept : m_descriptorSize{descriptorSize}
    {
    }

    void Add(const Foundation::UInt32 type, const Foundation::UInt64 physicalStart, const Foundation::UInt64 pageCount,
             const Foundation::UInt64 attributes, const Foundation::UInt64 virtualStart = 0U) noexcept
    {
        Foundation::UInt8* const descriptor = m_bytes + m_size;
        WriteLittleEndian32(descriptor, type);
        WriteLittleEndian64(descriptor + 8U, physicalStart);
        WriteLittleEndian64(descriptor + 16U, virtualStart);
        WriteLittleEndian64(descriptor + 24U, pageCount);
        WriteLittleEndian64(descriptor + 32U, attributes);
        m_size += m_descriptorSize;
    }

    [[nodiscard]] Foundation::UInt8* Bytes() noexcept
    {
        return m_bytes;
    }

    [[nodiscard]] const Foundation::UInt8* Bytes() const noexcept
    {
        return m_bytes;
    }

    [[nodiscard]] Foundation::Size Size() const noexcept
    {
        return m_size;
    }

    [[nodiscard]] Foundation::Size DescriptorSize() const noexcept
    {
        return m_descriptorSize;
    }

private:
    Foundation::UInt8 m_bytes[MaximumMapBytes]{};
    Foundation::Size m_size{0U};
    Foundation::Size m_descriptorSize;
};

[[nodiscard]] ConfigurationTable MakeSalConfiguration(void* const table = DefaultTablePointer()) noexcept
{
    return ConfigurationTable{
        .VendorGuid = SalSystemTableGuid,
        .VendorTable = table,
    };
}

[[nodiscard]] MemoryMapFixture MakeValidMemoryMap(const Foundation::Size descriptorSize = 40U) noexcept
{
    MemoryMapFixture memoryMap{descriptorSize};
    memoryMap.Add(RuntimeDataType, TableStorageAddress(), 2U, RuntimeAttribute | WriteBackAttribute);
    return memoryMap;
}

[[nodiscard]] bool HasError(const ConfigurationTable* const configurationTables,
                            const Foundation::Size configurationTableCount, const MemoryMapFixture& memoryMap,
                            const SalSystemTableLocateError expected, const Foundation::Size memoryMapSize = 0U,
                            const Foundation::Size descriptorSize = 0U,
                            const Foundation::UInt32 descriptorVersion = 1U) noexcept
{
    const Foundation::Size selectedMapSize = memoryMapSize == 0U ? memoryMap.Size() : memoryMapSize;
    const Foundation::Size selectedDescriptorSize = descriptorSize == 0U ? memoryMap.DescriptorSize() : descriptorSize;
    const auto result = SalSystemTableLocator::Locate(configurationTables, configurationTableCount, memoryMap.Bytes(),
                                                      selectedMapSize, selectedDescriptorSize, descriptorVersion);
    return !result.HasValue() && (result.GetError() == expected);
}

void TestExactLocation(TestContext& runner) noexcept
{
    const ConfigurationTable configuration = MakeSalConfiguration();
    const MemoryMapFixture memoryMap = MakeValidMemoryMap();
    const auto result = SalSystemTableLocator::Locate(&configuration, 1U, memoryMap.Bytes(), memoryMap.Size(),
                                                      memoryMap.DescriptorSize(), 1U);
    ASTERIA_EXPECT(runner, "the unique SAL pointer is bound to its exact runtime-data extent", result.HasValue(),
                   result.GetValue().Bytes == DefaultTablePointer(),
                   result.GetValue().AvailableBytes == (TableStorageSize - DefaultTableOffset),
                   result.GetValue().PhysicalAddress == (TableStorageAddress() + DefaultTableOffset),
                   result.GetValue().MemoryAttributes == (RuntimeAttribute | WriteBackAttribute),
                   result.GetValue().FirmwareMemoryType == RuntimeDataType, result.GetValue().Reserved == 0U);

    ConfigurationTable configurations[2U]{
        ConfigurationTable{
            .VendorGuid =
                Guid{
                    .Data1 = 1U,
                    .Data2 = 2U,
                    .Data3 = 3U,
                    .Data4 = {},
                },
            .VendorTable = nullptr,
        },
        MakeSalConfiguration(),
    };
    const auto afterUnrelated = SalSystemTableLocator::Locate(configurations, 2U, memoryMap.Bytes(), memoryMap.Size(),
                                                              memoryMap.DescriptorSize(), 1U);
    ASTERIA_EXPECT(runner, "unrelated configuration tables are skipped", afterUnrelated.HasValue(),
                   afterUnrelated.GetValue().Bytes == DefaultTablePointer());

    const MemoryMapFixture wideMap = MakeValidMemoryMap(48U);
    const auto wideResult = SalSystemTableLocator::Locate(&configuration, 1U, wideMap.Bytes(), wideMap.Size(),
                                                          wideMap.DescriptorSize(), 1U);
    ASTERIA_EXPECT(runner, "a larger UEFI descriptor stride is accepted without reading its tail",
                   wideResult.HasValue(),
                   wideResult.GetValue().AvailableBytes == (TableStorageSize - DefaultTableOffset));
}

void TestConfigurationValidation(TestContext& runner) noexcept
{
    const MemoryMapFixture memoryMap = MakeValidMemoryMap();
    ASTERIA_EXPECT(runner, "an empty configuration-table array reports SAL absence",
                   HasError(nullptr, 0U, memoryMap, SalSystemTableLocateError::SalSystemTableNotFound));
    ASTERIA_EXPECT(runner, "a null nonempty configuration-table array is rejected",
                   HasError(nullptr, 1U, memoryMap, SalSystemTableLocateError::InvalidConfigurationTableArray));

    ConfigurationTable configuration = MakeSalConfiguration();
    ASTERIA_EXPECT(runner, "an unbounded configuration-table count is rejected before iteration",
                   HasError(&configuration, MaximumFirmwareConfigurationTables + 1U, memoryMap,
                            SalSystemTableLocateError::InvalidConfigurationTableArray));

    configuration = MakeSalConfiguration(nullptr);
    ASTERIA_EXPECT(runner, "a null SAL vendor-table pointer is rejected",
                   HasError(&configuration, 1U, memoryMap, SalSystemTableLocateError::NullSalSystemTable));

    ConfigurationTable duplicates[2U]{
        MakeSalConfiguration(),
        MakeSalConfiguration(),
    };
    ASTERIA_EXPECT(runner, "duplicate SAL GUID instances violate the UEFI singleton contract",
                   HasError(duplicates, 2U, memoryMap, SalSystemTableLocateError::DuplicateSalSystemTable));

    bool everyGuidFieldCompared{true};
    for (Foundation::Size field{0U}; field < 11U; ++field)
    {
        configuration = MakeSalConfiguration();
        if (field == 0U)
        {
            configuration.VendorGuid.Data1 ^= 1U;
        }
        else if (field == 1U)
        {
            configuration.VendorGuid.Data2 ^= 1U;
        }
        else if (field == 2U)
        {
            configuration.VendorGuid.Data3 ^= 1U;
        }
        else
        {
            configuration.VendorGuid.Data4[field - 3U] ^= 1U;
        }
        everyGuidFieldCompared = everyGuidFieldCompared && HasError(&configuration, 1U, memoryMap,
                                                                    SalSystemTableLocateError::SalSystemTableNotFound);
    }
    ASTERIA_EXPECT(runner, "every SAL GUID field is compared exactly", everyGuidFieldCompared);
}

void TestMemoryMapMetadata(TestContext& runner) noexcept
{
    const ConfigurationTable configuration = MakeSalConfiguration();
    MemoryMapFixture memoryMap = MakeValidMemoryMap();
    const auto nullMap =
        SalSystemTableLocator::Locate(&configuration, 1U, nullptr, memoryMap.Size(), memoryMap.DescriptorSize(), 1U);
    ASTERIA_EXPECT(runner, "a null memory-map buffer is rejected", !nullMap.HasValue(),
                   nullMap.GetError() == SalSystemTableLocateError::InvalidMemoryMap);
    const auto emptyMap =
        SalSystemTableLocator::Locate(&configuration, 1U, memoryMap.Bytes(), 0U, memoryMap.DescriptorSize(), 1U);
    ASTERIA_EXPECT(runner, "an empty memory-map extent is rejected", !emptyMap.HasValue(),
                   emptyMap.GetError() == SalSystemTableLocateError::InvalidMemoryMap);
    ASTERIA_EXPECT(
        runner, "a descriptor smaller than the UEFI prefix is rejected",
        HasError(&configuration, 1U, memoryMap, SalSystemTableLocateError::InvalidMemoryMap, memoryMap.Size(), 39U));
    ASTERIA_EXPECT(
        runner, "a partial trailing descriptor is rejected",
        HasError(&configuration, 1U, memoryMap, SalSystemTableLocateError::InvalidMemoryMap, memoryMap.Size() - 1U));
    ASTERIA_EXPECT(runner, "an unsupported UEFI memory-descriptor version is rejected",
                   HasError(&configuration, 1U, memoryMap, SalSystemTableLocateError::InvalidMemoryMap,
                            memoryMap.Size(), memoryMap.DescriptorSize(), 2U));
    ASTERIA_EXPECT(runner, "the descriptor-count bound is enforced before traversing a claimed map",
                   HasError(&configuration, 1U, memoryMap, SalSystemTableLocateError::InvalidMemoryMap,
                            (MaximumMemoryDescriptorCount + 1U) * memoryMap.DescriptorSize()));

    bool malformedRangesRejected{true};
    memoryMap = MemoryMapFixture{};
    memoryMap.Add(RuntimeDataType, TableStorageAddress(), 0U, RuntimeAttribute);
    malformedRangesRejected =
        malformedRangesRejected && HasError(&configuration, 1U, memoryMap, SalSystemTableLocateError::InvalidMemoryMap);
    memoryMap = MemoryMapFixture{};
    memoryMap.Add(BootServicesDataType, ~Foundation::UInt64{0U} - 0xFFFU, 2U, WriteBackAttribute);
    malformedRangesRejected =
        malformedRangesRejected && HasError(&configuration, 1U, memoryMap, SalSystemTableLocateError::InvalidMemoryMap);
    memoryMap = MemoryMapFixture{};
    memoryMap.Add(RuntimeDataType, TableStorageAddress() + 1U, 1U, RuntimeAttribute);
    malformedRangesRejected =
        malformedRangesRejected && HasError(&configuration, 1U, memoryMap, SalSystemTableLocateError::InvalidMemoryMap);
    memoryMap = MemoryMapFixture{};
    memoryMap.Add(RuntimeDataType, TableStorageAddress(), 2U, RuntimeAttribute, 1U);
    malformedRangesRejected =
        malformedRangesRejected && HasError(&configuration, 1U, memoryMap, SalSystemTableLocateError::InvalidMemoryMap);
    memoryMap = MemoryMapFixture{};
    memoryMap.Add(RuntimeDataType, TableStorageAddress(), 2U, RuntimeAttribute, ~Foundation::UInt64{0U} - 0xFFFU);
    malformedRangesRejected =
        malformedRangesRejected && HasError(&configuration, 1U, memoryMap, SalSystemTableLocateError::InvalidMemoryMap);
    ASTERIA_EXPECT(runner, "zero, unaligned, and wrapping physical or virtual ranges are rejected",
                   malformedRangesRejected);

    memoryMap = MemoryMapFixture{};
    memoryMap.Add(BootServicesDataType, ~Foundation::UInt64{0U} - 0xFFFU, 1U, WriteBackAttribute);
    ASTERIA_EXPECT(runner, "the architecturally final physical page is a valid inclusive descriptor",
                   HasError(&configuration, 1U, memoryMap, SalSystemTableLocateError::TableAddressNotInMemoryMap));

    memoryMap = MemoryMapFixture{};
    memoryMap.Add(RuntimeDataType, 0U, Foundation::UInt64{1U} << 52U, RuntimeAttribute | WriteBackAttribute);
    const auto fullRange = SalSystemTableLocator::Locate(&configuration, 1U, memoryMap.Bytes(), memoryMap.Size(),
                                                         memoryMap.DescriptorSize(), 1U);
    ASTERIA_EXPECT(
        runner, "a full-width UEFI physical descriptor is represented without end wrap", fullRange.HasValue(),
        fullRange.GetValue().PhysicalAddress ==
            static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(DefaultTablePointer())));
}

void TestExtentAndAttributeValidation(TestContext& runner) noexcept
{
    ConfigurationTable configuration = MakeSalConfiguration();
    MemoryMapFixture memoryMap;
    memoryMap.Add(RuntimeDataType, TableStorageAddress() + TableStorageSize, 2U, RuntimeAttribute);
    ASTERIA_EXPECT(runner, "a SAL pointer outside every descriptor is rejected",
                   HasError(&configuration, 1U, memoryMap, SalSystemTableLocateError::TableAddressNotInMemoryMap));

    memoryMap = MemoryMapFixture{};
    memoryMap.Add(RuntimeDataType, TableStorageAddress(), 2U, RuntimeAttribute);
    memoryMap.Add(RuntimeDataType, TableStorageAddress(), 2U, RuntimeAttribute);
    ASTERIA_EXPECT(runner, "overlapping descriptors cannot ambiguously own the SAL pointer",
                   HasError(&configuration, 1U, memoryMap, SalSystemTableLocateError::AmbiguousMemoryDescriptor));

    configuration = MakeSalConfiguration(g_tableStorage + PageSizeBytes - 64U);
    memoryMap = MemoryMapFixture{};
    memoryMap.Add(RuntimeDataType, TableStorageAddress(), 2U, RuntimeAttribute);
    memoryMap.Add(BootServicesDataType, TableStorageAddress() + PageSizeBytes, 1U, WriteBackAttribute);
    ASTERIA_EXPECT(runner, "a descriptor intersecting only the later header bytes is still ambiguous",
                   HasError(&configuration, 1U, memoryMap, SalSystemTableLocateError::AmbiguousMemoryDescriptor));

    memoryMap = MemoryMapFixture{};
    memoryMap.Add(BootServicesDataType, TableStorageAddress(), 1U, WriteBackAttribute);
    ASTERIA_EXPECT(runner, "the complete 96-byte SAL header must fit in one descriptor",
                   HasError(&configuration, 1U, memoryMap, SalSystemTableLocateError::HeaderOutsideMemoryDescriptor));

    configuration = MakeSalConfiguration();
    constexpr Foundation::UInt32 supportedTypes[5U]{
        BootServicesDataType, RuntimeCodeType, RuntimeDataType, AcpiReclaimType, AcpiNonVolatileType,
    };
    constexpr Foundation::UInt64 supportedAttributes[5U]{
        WriteBackAttribute,
        RuntimeAttribute | WriteBackAttribute,
        RuntimeAttribute | WriteBackAttribute,
        WriteBackAttribute,
        WriteBackAttribute,
    };
    bool everyBootTimeTypeAccepted{true};
    for (Foundation::Size index{0U}; index < 5U; ++index)
    {
        memoryMap = MemoryMapFixture{};
        memoryMap.Add(supportedTypes[index], TableStorageAddress(), 2U, supportedAttributes[index]);
        const auto typeResult = SalSystemTableLocator::Locate(&configuration, 1U, memoryMap.Bytes(), memoryMap.Size(),
                                                              memoryMap.DescriptorSize(), 1U);
        everyBootTimeTypeAccepted = everyBootTimeTypeAccepted && typeResult.HasValue() &&
                                    (typeResult.GetValue().FirmwareMemoryType == supportedTypes[index]);
    }
    ASTERIA_EXPECT(runner, "SAL tables in data or combined runtime code/data descriptors are accepted",
                   everyBootTimeTypeAccepted);

    memoryMap = MemoryMapFixture{};
    memoryMap.Add(3U, TableStorageAddress(), 2U, WriteBackAttribute);
    ASTERIA_EXPECT(runner, "boot-services code is not admitted as runtime table storage",
                   HasError(&configuration, 1U, memoryMap, SalSystemTableLocateError::UnsupportedMemoryType));

    memoryMap = MemoryMapFixture{};
    memoryMap.Add(RuntimeCodeType, TableStorageAddress(), 2U, WriteBackAttribute);
    ASTERIA_EXPECT(runner, "combined runtime code/data storage still requires the runtime attribute",
                   HasError(&configuration, 1U, memoryMap, SalSystemTableLocateError::MissingRuntimeAttribute));

    memoryMap = MemoryMapFixture{};
    memoryMap.Add(RuntimeCodeType, TableStorageAddress(), 1U, RuntimeAttribute);
    ASTERIA_EXPECT(runner, "runtime code table storage still requires IA-64 8 KiB geometry",
                   HasError(&configuration, 1U, memoryMap, SalSystemTableLocateError::InvalidMemoryMap));

    memoryMap = MemoryMapFixture{};
    memoryMap.Add(RuntimeDataType, TableStorageAddress(), 2U, WriteBackAttribute);
    ASTERIA_EXPECT(runner, "SAL table storage must survive ExitBootServices",
                   HasError(&configuration, 1U, memoryMap, SalSystemTableLocateError::MissingRuntimeAttribute));

    memoryMap = MemoryMapFixture{};
    memoryMap.Add(RuntimeDataType, TableStorageAddress(), 2U, RuntimeAttribute | ReadProtectedAttribute);
    const auto capabilityAttributes = SalSystemTableLocator::Locate(&configuration, 1U, memoryMap.Bytes(),
                                                                    memoryMap.Size(), memoryMap.DescriptorSize(), 1U);
    ASTERIA_EXPECT(runner, "UEFI protection capability bits are not treated as current settings",
                   capabilityAttributes.HasValue(),
                   (capabilityAttributes.GetValue().MemoryAttributes & ReadProtectedAttribute) != 0U);

    bool ia64AlignmentRulesEnforced{true};
    configuration = MakeSalConfiguration(g_tableStorage + PageSizeBytes + DefaultTableOffset);
    memoryMap = MemoryMapFixture{};
    memoryMap.Add(RuntimeDataType, TableStorageAddress() + PageSizeBytes, 2U, RuntimeAttribute);
    ia64AlignmentRulesEnforced = ia64AlignmentRulesEnforced &&
                                 HasError(&configuration, 1U, memoryMap, SalSystemTableLocateError::InvalidMemoryMap);
    configuration = MakeSalConfiguration();
    memoryMap = MemoryMapFixture{};
    memoryMap.Add(RuntimeDataType, TableStorageAddress(), 1U, RuntimeAttribute);
    ia64AlignmentRulesEnforced = ia64AlignmentRulesEnforced &&
                                 HasError(&configuration, 1U, memoryMap, SalSystemTableLocateError::InvalidMemoryMap);
    memoryMap = MemoryMapFixture{};
    memoryMap.Add(RuntimeDataType, TableStorageAddress(), 2U, RuntimeAttribute, PageSizeBytes);
    ia64AlignmentRulesEnforced = ia64AlignmentRulesEnforced &&
                                 HasError(&configuration, 1U, memoryMap, SalSystemTableLocateError::InvalidMemoryMap);
    memoryMap = MemoryMapFixture{};
    memoryMap.Add(AcpiReclaimType, TableStorageAddress(), 1U, WriteBackAttribute);
    ia64AlignmentRulesEnforced = ia64AlignmentRulesEnforced &&
                                 HasError(&configuration, 1U, memoryMap, SalSystemTableLocateError::InvalidMemoryMap);
    memoryMap = MemoryMapFixture{};
    memoryMap.Add(AcpiNonVolatileType, TableStorageAddress(), 2U, RuntimeAttribute);
    ia64AlignmentRulesEnforced = ia64AlignmentRulesEnforced &&
                                 HasError(&configuration, 1U, memoryMap, SalSystemTableLocateError::InvalidMemoryMap);
    ASTERIA_EXPECT(runner, "runtime and ACPI descriptors obey IA-64 8 KiB alignment and extent rules",
                   ia64AlignmentRulesEnforced);

    configuration = MakeSalConfiguration(g_tableStorage + PageSizeBytes + DefaultTableOffset);
    memoryMap = MemoryMapFixture{};
    memoryMap.Add(BootServicesDataType, TableStorageAddress() + PageSizeBytes, 1U, WriteBackAttribute);
    const auto fourKibibyteBootData = SalSystemTableLocator::Locate(&configuration, 1U, memoryMap.Bytes(),
                                                                    memoryMap.Size(), memoryMap.DescriptorSize(), 1U);
    ASTERIA_EXPECT(runner, "ordinary boot-services data retains the generic 4 KiB alignment rule",
                   fourKibibyteBootData.HasValue());
}

void TestInputsRemainUnchanged(TestContext& runner) noexcept
{
    ConfigurationTable configuration = MakeSalConfiguration();
    MemoryMapFixture memoryMap = MakeValidMemoryMap();
    ConfigurationTable configurationBefore = configuration;
    Foundation::UInt8 mapBefore[MaximumMapBytes]{};
    for (Foundation::Size index{0U}; index < MaximumMapBytes; ++index)
    {
        mapBefore[index] = memoryMap.Bytes()[index];
    }

    const auto result = SalSystemTableLocator::Locate(&configuration, 1U, memoryMap.Bytes(), memoryMap.Size(),
                                                      memoryMap.DescriptorSize(), 1U);
    bool unchanged = result.HasValue() && (configurationBefore.VendorTable == configuration.VendorTable) &&
                     (configurationBefore.VendorGuid.Data1 == configuration.VendorGuid.Data1) &&
                     (configurationBefore.VendorGuid.Data2 == configuration.VendorGuid.Data2) &&
                     (configurationBefore.VendorGuid.Data3 == configuration.VendorGuid.Data3);
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        unchanged = unchanged && (configurationBefore.VendorGuid.Data4[index] == configuration.VendorGuid.Data4[index]);
    }
    for (Foundation::Size index{0U}; index < MaximumMapBytes; ++index)
    {
        unchanged = unchanged && (mapBefore[index] == memoryMap.Bytes()[index]);
    }
    ASTERIA_EXPECT(runner, "location leaves configuration and memory-map bytes unchanged", unchanged);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"SalSystemTableLocatorTests"};
    Asteria::Test::TestExactLocation(runner);
    Asteria::Test::TestConfigurationValidation(runner);
    Asteria::Test::TestMemoryMapMetadata(runner);
    Asteria::Test::TestExtentAndAttributeValidation(runner);
    Asteria::Test::TestInputsRemainUnchanged(runner);
    return runner.Finish();
}
