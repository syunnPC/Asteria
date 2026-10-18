// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Boot/BootInfo.hpp"
#include "Foundation/Types.hpp"
#include "Platform/Ia64/Firmware/SalEntrypointBinder.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Boot::BootInfo;
using Boot::PhysicalMemoryKind;
using Boot::PhysicalMemoryRegion;
using Platform::Ia64::Firmware::SalEntrypointBindError;
using Platform::Ia64::Firmware::SalEntrypointBinder;
using Platform::Ia64::Firmware::SalSystemTableInformation;

constexpr Foundation::UInt32 DescriptorSize{48U};
constexpr Foundation::UInt32 RegionCount{5U};
constexpr Foundation::UInt64 WriteBackAttribute{Foundation::UInt64{1U} << 3U};
constexpr Foundation::UInt64 RuntimeAttribute{Foundation::UInt64{1U} << 63U};

constinit BootInfo g_bootInfo{};
constinit SalSystemTableInformation g_salInformation{};
Foundation::UInt8 g_bootInfoSnapshot[sizeof(BootInfo)];
Foundation::UInt8 g_salInformationSnapshot[sizeof(SalSystemTableInformation)];

[[nodiscard]] constexpr PhysicalMemoryRegion
Region(const Foundation::UInt64 physicalStart, const Foundation::UInt64 virtualStart,
       const Foundation::UInt64 pageCount, const Foundation::UInt64 attributes, const Foundation::UInt32 firmwareType,
       const PhysicalMemoryKind kind) noexcept
{
    return PhysicalMemoryRegion{
        .PhysicalStart = physicalStart,
        .VirtualStart = virtualStart,
        .PageCount = pageCount,
        .Attributes = attributes,
        .FirmwareType = firmwareType,
        .Kind = kind,
    };
}

void ResetBootInfo() noexcept
{
    g_bootInfo.Signature = Boot::BootInfoSignature;
    g_bootInfo.Version = Boot::BootInfoVersion;
    g_bootInfo.SizeBytes = static_cast<Foundation::UInt32>(sizeof(BootInfo));
    g_bootInfo.MemoryRegionCount = RegionCount;
    g_bootInfo.FirmwareDescriptorVersion = 1U;
    g_bootInfo.FirmwareDescriptorSize = DescriptorSize;
    g_bootInfo.Reserved = 0U;
    g_bootInfo.FirmwareMemoryMapSizeBytes = static_cast<Foundation::UInt64>(RegionCount) * DescriptorSize;
    g_bootInfo.MemoryRegions[0U] = Region(0U, 0U, 0x100U, WriteBackAttribute, 0U, PhysicalMemoryKind::Reserved);
    g_bootInfo.MemoryRegions[1U] = Region(0x0010'0000U, 0U, 1U, WriteBackAttribute, 13U, PhysicalMemoryKind::PalCode);
    g_bootInfo.MemoryRegions[2U] =
        Region(0x0020'0000U, 0U, 2U, WriteBackAttribute | RuntimeAttribute, 5U, PhysicalMemoryKind::RuntimeServices);
    g_bootInfo.MemoryRegions[3U] =
        Region(0x0030'0000U, 0U, 2U, WriteBackAttribute | RuntimeAttribute, 6U, PhysicalMemoryKind::RuntimeServices);
    g_bootInfo.MemoryRegions[4U] =
        Region(0x0040'0000U, 0U, 0x100U, WriteBackAttribute, 7U, PhysicalMemoryKind::Conventional);
}

void ResetSalInformation() noexcept
{
    g_salInformation = SalSystemTableInformation{
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

void ResetQemuFirmwareProfile() noexcept
{
    ResetBootInfo();
    ResetSalInformation();
    g_bootInfo.MemoryRegions[2U] =
        Region(0x0010'2000U, 0U, 0x8EU, WriteBackAttribute | RuntimeAttribute, 5U, PhysicalMemoryKind::RuntimeServices);
    g_bootInfo.MemoryRegions[3U] = Region(0x0019'0000U, 0U, 0x170U, WriteBackAttribute | RuntimeAttribute, 6U,
                                          PhysicalMemoryKind::RuntimeServices);
    g_salInformation.PalProcedurePhysicalAddress = 0x0010'0060U;
    g_salInformation.SalProcedurePhysicalAddress = 0x0010'2020U;
    g_salInformation.SalGlobalDataPhysicalAddress = 0x001D'174CU;
}

[[nodiscard]] bool HasError(const SalEntrypointBindError error) noexcept
{
    const auto result = SalEntrypointBinder::Bind(g_salInformation, g_bootInfo);
    return !result.HasValue() && (result.GetError() == error);
}

void SnapshotInputs() noexcept
{
    const auto* const bootBytes = reinterpret_cast<const Foundation::UInt8*>(&g_bootInfo);
    const auto* const salBytes = reinterpret_cast<const Foundation::UInt8*>(&g_salInformation);
    for (Foundation::Size index{0U}; index < sizeof(BootInfo); ++index)
    {
        g_bootInfoSnapshot[index] = bootBytes[index];
    }
    for (Foundation::Size index{0U}; index < sizeof(SalSystemTableInformation); ++index)
    {
        g_salInformationSnapshot[index] = salBytes[index];
    }
}

[[nodiscard]] bool InputsMatchSnapshot() noexcept
{
    const auto* const bootBytes = reinterpret_cast<const Foundation::UInt8*>(&g_bootInfo);
    const auto* const salBytes = reinterpret_cast<const Foundation::UInt8*>(&g_salInformation);
    for (Foundation::Size index{0U}; index < sizeof(BootInfo); ++index)
    {
        if (g_bootInfoSnapshot[index] != bootBytes[index])
        {
            return false;
        }
    }
    for (Foundation::Size index{0U}; index < sizeof(SalSystemTableInformation); ++index)
    {
        if (g_salInformationSnapshot[index] != salBytes[index])
        {
            return false;
        }
    }
    return true;
}

void TestSuccessfulBinding(TestContext& runner) noexcept
{
    ResetBootInfo();
    ResetSalInformation();
    SnapshotInputs();
    const auto result = SalEntrypointBinder::Bind(g_salInformation, g_bootInfo);
    ASTERIA_EXPECT(runner, "validated PAL, SAL, and GP addresses bind to exact normalized regions", result.HasValue(),
                   result.GetValue().PalProcedurePhysicalAddress == 0x0010'0000U,
                   result.GetValue().SalProcedurePhysicalAddress == 0x0020'0000U,
                   result.GetValue().SalGlobalDataPhysicalAddress == 0x0030'0000U,
                   result.GetValue().PalCodeRegion.FirmwareType == 13U,
                   result.GetValue().SalCodeRegion.FirmwareType == 5U,
                   result.GetValue().SalGlobalDataRegion.FirmwareType == 6U);
    ASTERIA_EXPECT(runner, "successful binding leaves both complete inputs byte-unchanged", InputsMatchSnapshot());

    g_salInformation.SalGlobalDataPhysicalAddress = 0x0020'0008U;
    const auto codeGpResult = SalEntrypointBinder::Bind(g_salInformation, g_bootInfo);
    ASTERIA_EXPECT(runner, "a runtime-code GP anchor is accepted for compatible SAL firmware", codeGpResult.HasValue(),
                   codeGpResult.GetValue().SalGlobalDataRegion.FirmwareType == 5U);

    g_salInformation.PalProcedurePhysicalAddress = 0x0010'0FF0U;
    g_salInformation.SalProcedurePhysicalAddress = 0x0020'1FF0U;
    g_salInformation.SalGlobalDataPhysicalAddress = 0x0030'1FF8U;
    const auto edgeResult = SalEntrypointBinder::Bind(g_salInformation, g_bootInfo);
    ASTERIA_EXPECT(runner, "the last complete aligned access unit in each region remains valid", edgeResult.HasValue());
}

void TestQemuFirmwareProfiles(TestContext& runner) noexcept
{
    ResetQemuFirmwareProfile();
    const auto standard = SalEntrypointBinder::Bind(g_salInformation, g_bootInfo);
    ASTERIA_EXPECT(runner, "the standard QEMU firmware binds its naturally unaligned GP value", standard.HasValue(),
                   standard.GetValue().PalProcedurePhysicalAddress == 0x0010'0060U,
                   standard.GetValue().SalProcedurePhysicalAddress == 0x0010'2020U,
                   standard.GetValue().SalGlobalDataPhysicalAddress == 0x001D'174CU,
                   standard.GetValue().SalGlobalDataRegion.FirmwareType == 6U);

    g_salInformation.SalGlobalDataPhysicalAddress = 0x0010'2000U;
    const auto compatible = SalEntrypointBinder::Bind(g_salInformation, g_bootInfo);
    ASTERIA_EXPECT(runner, "the compatible QEMU firmware binds its code-side GP anchor", compatible.HasValue(),
                   compatible.GetValue().SalGlobalDataRegion.FirmwareType == 5U);
}

void TestPhysicalAddressBoundary(TestContext& runner) noexcept
{
    ResetBootInfo();
    ResetSalInformation();
    g_bootInfo.MemoryRegionCount = 4U;
    g_bootInfo.FirmwareMemoryMapSizeBytes = Foundation::UInt64{4U} * DescriptorSize;
    g_bootInfo.MemoryRegions[3U] = Region(0x7FFF'FFFF'FFFF'E000ULL, 0U, 4U, WriteBackAttribute | RuntimeAttribute, 6U,
                                          PhysicalMemoryKind::RuntimeServices);
    g_salInformation.SalGlobalDataPhysicalAddress = 0x7FFF'FFFF'FFFF'FFF8ULL;
    ASTERIA_EXPECT(runner, "the last complete eight-byte GP access in 63-bit physical space binds",
                   SalEntrypointBinder::Bind(g_salInformation, g_bootInfo).HasValue());

    bool everyCrossingGpRejected{true};
    for (Foundation::UInt64 address{0x7FFF'FFFF'FFFF'FFF9ULL}; address <= 0x7FFF'FFFF'FFFF'FFFFULL; ++address)
    {
        g_salInformation.SalGlobalDataPhysicalAddress = address;
        everyCrossingGpRejected =
            everyCrossingGpRejected && HasError(SalEntrypointBindError::SalGlobalDataOutsideRuntimeStorage);
    }
    ASTERIA_EXPECT(runner, "every GP access crossing the 63-bit physical limit is rejected", everyCrossingGpRejected);

    ResetBootInfo();
    ResetSalInformation();
    g_bootInfo.MemoryRegions[4U] =
        Region(0x7FFF'FFFF'FFFF'E000ULL, 0U, 2U, WriteBackAttribute, 13U, PhysicalMemoryKind::PalCode);
    g_salInformation.PalProcedurePhysicalAddress = 0x7FFF'FFFF'FFFF'FFF0ULL;
    ASTERIA_EXPECT(runner, "the final complete PAL bundle in 63-bit physical space binds",
                   SalEntrypointBinder::Bind(g_salInformation, g_bootInfo).HasValue());

    ResetBootInfo();
    ResetSalInformation();
    g_bootInfo.MemoryRegions[4U] = Region(0x7FFF'FFFF'FFFF'E000ULL, 0U, 2U, WriteBackAttribute | RuntimeAttribute, 5U,
                                          PhysicalMemoryKind::RuntimeServices);
    g_salInformation.SalProcedurePhysicalAddress = 0x7FFF'FFFF'FFFF'FFF0ULL;
    ASTERIA_EXPECT(runner, "the final complete SAL bundle in 63-bit physical space binds",
                   SalEntrypointBinder::Bind(g_salInformation, g_bootInfo).HasValue());
}

void TestSalInputFailures(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 invalidPalAddresses[]{
        0U,
        0x0010'0008U,
        0x8000'0000'0010'0000ULL,
    };
    bool palFailures{true};
    for (const Foundation::UInt64 address : invalidPalAddresses)
    {
        ResetBootInfo();
        ResetSalInformation();
        g_salInformation.PalProcedurePhysicalAddress = address;
        palFailures = palFailures && HasError(SalEntrypointBindError::InvalidSalInformation);
    }
    ASTERIA_EXPECT(runner, "invalid PAL physical forms are rejected", palFailures);

    constexpr Foundation::UInt64 invalidSalAddresses[]{
        0U,
        0x0020'0008U,
        0x8000'0000'0020'0000ULL,
    };
    bool salFailures{true};
    for (const Foundation::UInt64 address : invalidSalAddresses)
    {
        ResetBootInfo();
        ResetSalInformation();
        g_salInformation.SalProcedurePhysicalAddress = address;
        salFailures = salFailures && HasError(SalEntrypointBindError::InvalidSalInformation);
    }
    ASTERIA_EXPECT(runner, "invalid SAL physical forms are rejected", salFailures);

    constexpr Foundation::UInt64 invalidGpAddresses[]{
        0U,
        0x8000'0000'0030'0000ULL,
    };
    bool gpFailures{true};
    for (const Foundation::UInt64 address : invalidGpAddresses)
    {
        ResetBootInfo();
        ResetSalInformation();
        g_salInformation.SalGlobalDataPhysicalAddress = address;
        gpFailures = gpFailures && HasError(SalEntrypointBindError::InvalidSalInformation);
    }
    ASTERIA_EXPECT(runner, "zero and nonphysical SAL GP values are rejected", gpFailures);

    ResetBootInfo();
    ResetSalInformation();
    g_salInformation.Reserved8 = 1U;
    g_bootInfo.Signature = 0U;
    ASTERIA_EXPECT(runner, "SAL field validation precedes normalized handoff validation",
                   HasError(SalEntrypointBindError::InvalidSalInformation));
}

void TestBootInfoFailures(TestContext& runner) noexcept
{
    bool framingFailures{true};
    ResetBootInfo();
    ResetSalInformation();
    g_bootInfo.Signature = 0U;
    framingFailures = framingFailures && HasError(SalEntrypointBindError::InvalidBootInfo);
    ResetBootInfo();
    g_bootInfo.Version = 0U;
    framingFailures = framingFailures && HasError(SalEntrypointBindError::InvalidBootInfo);
    ResetBootInfo();
    g_bootInfo.SizeBytes = 0U;
    framingFailures = framingFailures && HasError(SalEntrypointBindError::InvalidBootInfo);
    ResetBootInfo();
    g_bootInfo.MemoryRegionCount = 0U;
    framingFailures = framingFailures && HasError(SalEntrypointBindError::InvalidBootInfo);
    ResetBootInfo();
    g_bootInfo.MemoryRegionCount = static_cast<Foundation::UInt32>(Boot::MaximumMemoryRegions + 1U);
    framingFailures = framingFailures && HasError(SalEntrypointBindError::InvalidBootInfo);
    ResetBootInfo();
    g_bootInfo.FirmwareDescriptorVersion = 0U;
    framingFailures = framingFailures && HasError(SalEntrypointBindError::InvalidBootInfo);
    ResetBootInfo();
    g_bootInfo.FirmwareDescriptorSize = 39U;
    framingFailures = framingFailures && HasError(SalEntrypointBindError::InvalidBootInfo);
    ResetBootInfo();
    g_bootInfo.Reserved = 1U;
    framingFailures = framingFailures && HasError(SalEntrypointBindError::InvalidBootInfo);
    ResetBootInfo();
    ++g_bootInfo.FirmwareMemoryMapSizeBytes;
    framingFailures = framingFailures && HasError(SalEntrypointBindError::InvalidBootInfo);
    ASTERIA_EXPECT(runner, "every normalized BootInfo framing field is revalidated", framingFailures);

    bool rangeFailures{true};
    ResetBootInfo();
    g_bootInfo.MemoryRegions[1U].PageCount = 0U;
    rangeFailures = rangeFailures && HasError(SalEntrypointBindError::InvalidBootInfo);
    ResetBootInfo();
    g_bootInfo.MemoryRegions[1U].PhysicalStart += 1U;
    rangeFailures = rangeFailures && HasError(SalEntrypointBindError::InvalidBootInfo);
    ResetBootInfo();
    g_bootInfo.MemoryRegions[1U].VirtualStart = 1U;
    rangeFailures = rangeFailures && HasError(SalEntrypointBindError::InvalidBootInfo);
    ResetBootInfo();
    g_bootInfo.MemoryRegions[2U].PhysicalStart = 0x0010'0000U;
    rangeFailures = rangeFailures && HasError(SalEntrypointBindError::InvalidBootInfo);
    ResetBootInfo();
    g_bootInfo.MemoryRegions[1U].Kind = PhysicalMemoryKind::Conventional;
    rangeFailures = rangeFailures && HasError(SalEntrypointBindError::InvalidBootInfo);
    ResetBootInfo();
    g_bootInfo.MemoryRegions[4U].Attributes |= RuntimeAttribute;
    rangeFailures = rangeFailures && HasError(SalEntrypointBindError::InvalidBootInfo);
    ASTERIA_EXPECT(runner, "invalid, overlapping, reclassified, and contradictory regions are rejected", rangeFailures);

    bool ia64LayoutFailures{true};
    ResetBootInfo();
    g_bootInfo.MemoryRegions[2U].PhysicalStart += 0x1000U;
    ia64LayoutFailures = ia64LayoutFailures && HasError(SalEntrypointBindError::InvalidBootInfo);
    ResetBootInfo();
    g_bootInfo.MemoryRegions[2U].VirtualStart = 0x0080'1000U;
    ia64LayoutFailures = ia64LayoutFailures && HasError(SalEntrypointBindError::InvalidBootInfo);
    ResetBootInfo();
    g_bootInfo.MemoryRegions[2U].PageCount = 1U;
    ia64LayoutFailures = ia64LayoutFailures && HasError(SalEntrypointBindError::InvalidBootInfo);
    ResetBootInfo();
    g_bootInfo.MemoryRegions[4U] =
        Region(0x0040'0000U, 0U, 2U, RuntimeAttribute, 9U, PhysicalMemoryKind::AcpiReclaimable);
    ia64LayoutFailures = ia64LayoutFailures && HasError(SalEntrypointBindError::InvalidBootInfo);
    ASTERIA_EXPECT(runner, "IA-64 runtime and ACPI descriptor layout rules are revalidated", ia64LayoutFailures);

    constexpr Foundation::UInt32 invalidFirmwareTypes[]{
        16U,
        0x1234U,
        0x6FFF'FFFFU,
    };
    bool invalidTypeNamespaceRejected{true};
    for (const Foundation::UInt32 firmwareType : invalidFirmwareTypes)
    {
        ResetBootInfo();
        g_bootInfo.MemoryRegions[0U].FirmwareType = firmwareType;
        invalidTypeNamespaceRejected =
            invalidTypeNamespaceRejected && HasError(SalEntrypointBindError::InvalidBootInfo);
    }
    ASTERIA_EXPECT(runner, "the undefined gap between standard and OEM memory types is rejected",
                   invalidTypeNamespaceRejected);

    constexpr Foundation::UInt32 reservedNamespaceTypes[]{
        0x7000'0000U,
        0x7FFF'FFFFU,
        0x8000'0000U,
        0xFFFF'FFFFU,
    };
    bool reservedNamespacesAccepted{true};
    for (const Foundation::UInt32 firmwareType : reservedNamespaceTypes)
    {
        ResetBootInfo();
        g_bootInfo.MemoryRegions[0U].FirmwareType = firmwareType;
        reservedNamespacesAccepted =
            reservedNamespacesAccepted && SalEntrypointBinder::Bind(g_salInformation, g_bootInfo).HasValue();
    }
    ASTERIA_EXPECT(runner, "defined OEM and OS memory-type namespaces remain conservatively reserved",
                   reservedNamespacesAccepted);
}

void TestOwnershipFailures(TestContext& runner) noexcept
{
    ResetBootInfo();
    ResetSalInformation();
    g_bootInfo.MemoryRegions[1U].FirmwareType = 4U;
    g_bootInfo.MemoryRegions[1U].Kind = PhysicalMemoryKind::BootServices;
    ASTERIA_EXPECT(runner, "PAL entry must lie in EfiPalCode rather than ordinary firmware storage",
                   HasError(SalEntrypointBindError::PalProcedureOutsidePalCode));

    ResetBootInfo();
    g_salInformation.PalProcedurePhysicalAddress = 0x0010'1000U;
    ASTERIA_EXPECT(runner, "an unmapped PAL entry is rejected",
                   HasError(SalEntrypointBindError::PalProcedureOutsidePalCode));

    ResetBootInfo();
    ResetSalInformation();
    g_bootInfo.MemoryRegions[2U].FirmwareType = 4U;
    g_bootInfo.MemoryRegions[2U].Kind = PhysicalMemoryKind::BootServices;
    g_bootInfo.MemoryRegions[2U].Attributes = WriteBackAttribute;
    ASTERIA_EXPECT(runner, "SAL entry must lie in runtime-services code",
                   HasError(SalEntrypointBindError::SalProcedureOutsideRuntimeCode));

    ResetBootInfo();
    g_salInformation.SalProcedurePhysicalAddress = 0x0020'2000U;
    ASTERIA_EXPECT(runner, "an unmapped SAL entry is rejected",
                   HasError(SalEntrypointBindError::SalProcedureOutsideRuntimeCode));

    ResetBootInfo();
    ResetSalInformation();
    g_bootInfo.MemoryRegions[3U].FirmwareType = 4U;
    g_bootInfo.MemoryRegions[3U].Kind = PhysicalMemoryKind::BootServices;
    g_bootInfo.MemoryRegions[3U].Attributes = WriteBackAttribute;
    ASTERIA_EXPECT(runner, "SAL GP must lie in runtime code or runtime data",
                   HasError(SalEntrypointBindError::SalGlobalDataOutsideRuntimeStorage));

    ResetBootInfo();
    g_salInformation.SalGlobalDataPhysicalAddress = 0x0030'2000U;
    ASTERIA_EXPECT(runner, "an unmapped SAL GP is rejected",
                   HasError(SalEntrypointBindError::SalGlobalDataOutsideRuntimeStorage));

    ResetBootInfo();
    g_salInformation.SalGlobalDataPhysicalAddress = 0x0030'1FF9U;
    ASTERIA_EXPECT(runner, "a SAL GP whose complete access crosses its runtime region is rejected",
                   HasError(SalEntrypointBindError::SalGlobalDataOutsideRuntimeStorage));

    ResetBootInfo();
    ResetSalInformation();
    g_bootInfo.MemoryRegions[2U].Attributes = WriteBackAttribute;
    ASTERIA_EXPECT(runner, "runtime code containing SAL must carry EFI_MEMORY_RUNTIME",
                   HasError(SalEntrypointBindError::MissingRuntimeAttribute));

    ResetBootInfo();
    g_bootInfo.MemoryRegions[3U].Attributes = WriteBackAttribute;
    ASTERIA_EXPECT(runner, "runtime storage containing the SAL GP must carry EFI_MEMORY_RUNTIME",
                   HasError(SalEntrypointBindError::MissingRuntimeAttribute));
}

void TestFailureImmutability(TestContext& runner) noexcept
{
    ResetBootInfo();
    ResetSalInformation();
    g_bootInfo.MemoryRegions[1U].FirmwareType = 4U;
    g_bootInfo.MemoryRegions[1U].Kind = PhysicalMemoryKind::BootServices;
    SnapshotInputs();
    const bool rejected = HasError(SalEntrypointBindError::PalProcedureOutsidePalCode);
    ASTERIA_EXPECT(runner, "failed binding leaves both complete inputs byte-unchanged", rejected,
                   InputsMatchSnapshot());
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"SalEntrypointBinderTests"};
    Asteria::Test::TestSuccessfulBinding(runner);
    Asteria::Test::TestQemuFirmwareProfiles(runner);
    Asteria::Test::TestPhysicalAddressBoundary(runner);
    Asteria::Test::TestSalInputFailures(runner);
    Asteria::Test::TestBootInfoFailures(runner);
    Asteria::Test::TestOwnershipFailures(runner);
    Asteria::Test::TestFailureImmutability(runner);
    return runner.Finish();
}
