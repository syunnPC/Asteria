// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Foundation/Types.hpp"
#include "Platform/Amd64/Memory/InitialPagingCapabilityCapture.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Platform::Amd64::Memory::TestSupport
{
namespace
{
constexpr Foundation::UInt32 RequiredBasicLeaf{1U};
constexpr Foundation::UInt32 MaximumExtendedLeafQuery{0x8000'0000U};
constexpr Foundation::UInt32 RequiredExtendedFeatureLeaf{0x8000'0001U};
constexpr Foundation::UInt32 RequiredAddressSizeLeaf{0x8000'0008U};

void ExecuteCpuId(const Foundation::UInt32 leaf, Foundation::UInt32& eax, Foundation::UInt32& ebx,
                  Foundation::UInt32& ecx, Foundation::UInt32& edx) noexcept
{
    constexpr Foundation::UInt32 Subleaf{0U};
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(leaf), "c"(Subleaf) : "memory");
}
} // namespace

[[nodiscard]] InitialPagingCapabilityReport CaptureNativeReport() noexcept
{
    Foundation::UInt32 eax{0U};
    Foundation::UInt32 ebx{0U};
    Foundation::UInt32 ecx{0U};
    Foundation::UInt32 edx{0U};
    InitialPagingCapabilityReport report{};

    ExecuteCpuId(0U, eax, ebx, ecx, edx);
    report.MaximumBasicLeaf = eax;
    if (report.MaximumBasicLeaf >= RequiredBasicLeaf)
    {
        ExecuteCpuId(RequiredBasicLeaf, eax, ebx, ecx, edx);
        report.BasicFeaturesEdx = edx;
    }

    ExecuteCpuId(MaximumExtendedLeafQuery, eax, ebx, ecx, edx);
    report.MaximumExtendedLeaf = eax;
    if (report.MaximumExtendedLeaf >= RequiredExtendedFeatureLeaf)
    {
        ExecuteCpuId(RequiredExtendedFeatureLeaf, eax, ebx, ecx, edx);
        report.ExtendedFeaturesEdx = edx;
    }
    if (report.MaximumExtendedLeaf >= RequiredAddressSizeLeaf)
    {
        ExecuteCpuId(RequiredAddressSizeLeaf, eax, ebx, ecx, edx);
        report.AddressSizesEax = eax;
    }
    return report;
}
} // namespace Asteria::Platform::Amd64::Memory::TestSupport

namespace Asteria::Test
{
namespace
{
using Platform::Amd64::Memory::InitialPagingCapabilityCapture;
using Platform::Amd64::Memory::InitialPagingCapabilityCaptureError;
using Platform::Amd64::Memory::InitialPagingCapabilityReport;
using Platform::Amd64::Memory::PagingCapabilities;

constexpr Foundation::UInt32 ModelSpecificRegisterBit{1U << 5U};
constexpr Foundation::UInt32 GlobalPagesBit{1U << 13U};
constexpr Foundation::UInt32 PageAttributeTableBit{1U << 16U};
constexpr Foundation::UInt32 ExecuteDisableBit{1U << 20U};
constexpr Foundation::UInt32 LongModeBit{1U << 29U};

[[nodiscard]] constexpr InitialPagingCapabilityReport FullReport() noexcept
{
    return InitialPagingCapabilityReport{
        .MaximumBasicLeaf = 1U,
        .BasicFeaturesEdx = ModelSpecificRegisterBit | GlobalPagesBit | PageAttributeTableBit,
        .MaximumExtendedLeaf = 0x8000'0008U,
        .ExtendedFeaturesEdx = ExecuteDisableBit | LongModeBit,
        .AddressSizesEax = 48U,
        .Reserved = 0U,
    };
}

[[nodiscard]] constexpr PagingCapabilities Sentinel() noexcept
{
    return PagingCapabilities{
        .PhysicalAddressBits = 0xA5U,
        .FourLevelPagingEnabled = false,
        .ExecuteDisableEnabled = false,
        .SupervisorWriteProtectEnabled = false,
        .GlobalPagesEnabled = false,
        .PatLayoutConfigured = false,
        .Reserved16 = 0x5AA5U,
        .Reserved = 0xA55A'A55AU,
    };
}

[[nodiscard]] constexpr bool EqualCapabilities(const PagingCapabilities& first,
                                               const PagingCapabilities& second) noexcept
{
    return (first.PhysicalAddressBits == second.PhysicalAddressBits) &&
           (first.FourLevelPagingEnabled == second.FourLevelPagingEnabled) &&
           (first.ExecuteDisableEnabled == second.ExecuteDisableEnabled) &&
           (first.SupervisorWriteProtectEnabled == second.SupervisorWriteProtectEnabled) &&
           (first.GlobalPagesEnabled == second.GlobalPagesEnabled) &&
           (first.PatLayoutConfigured == second.PatLayoutConfigured) && (first.Reserved16 == second.Reserved16) &&
           (first.Reserved == second.Reserved);
}

[[nodiscard]] constexpr bool IsExactPublishedCapabilities(const PagingCapabilities& capabilities,
                                                          const Foundation::UInt8 physicalAddressBits) noexcept
{
    return (capabilities.PhysicalAddressBits == physicalAddressBits) && capabilities.FourLevelPagingEnabled &&
           capabilities.ExecuteDisableEnabled && capabilities.SupervisorWriteProtectEnabled &&
           capabilities.GlobalPagesEnabled && capabilities.PatLayoutConfigured && (capabilities.Reserved16 == 0U) &&
           (capabilities.Reserved == 0U);
}

void CheckFailure(TestContext& runner, const InitialPagingCapabilityReport& report,
                  const InitialPagingCapabilityCaptureError expected, const char* const description) noexcept
{
    PagingCapabilities output = Sentinel();
    const PagingCapabilities before = output;
    const InitialPagingCapabilityCaptureError error = InitialPagingCapabilityCapture::Decode(report, output);
    ASTERIA_EXPECT(runner, description, error == expected, EqualCapabilities(output, before));
}

void TestDecode(TestContext& runner) noexcept
{
    InitialPagingCapabilityReport report = FullReport();
    constexpr Foundation::UInt8 ValidPhysicalAddressWidths[]{
        32U, 33U, 48U, 51U, 52U,
    };
    for (const Foundation::UInt8 width : ValidPhysicalAddressWidths)
    {
        PagingCapabilities output = Sentinel();
        report.AddressSizesEax = 0xABCD'0000U | width;
        const InitialPagingCapabilityCaptureError error = InitialPagingCapabilityCapture::Decode(report, output);
        ASTERIA_EXPECT(runner, "each valid physical width is published without substitution",
                       error == InitialPagingCapabilityCaptureError::None, IsExactPublishedCapabilities(output, width));
    }

    report = FullReport();
    report.Reserved = 1U;
    CheckFailure(runner, report, InitialPagingCapabilityCaptureError::InvalidReport,
                 "reserved report bits are rejected before output");
    report = FullReport();
    report.MaximumBasicLeaf = 0U;
    CheckFailure(runner, report, InitialPagingCapabilityCaptureError::RequiredBasicLeafUnavailable,
                 "a missing basic feature leaf is rejected before output");
    report = FullReport();
    report.MaximumExtendedLeaf = 0x8000'0007U;
    CheckFailure(runner, report, InitialPagingCapabilityCaptureError::RequiredExtendedLeafUnavailable,
                 "a missing address-size leaf is rejected before output");
    report = FullReport();
    report.BasicFeaturesEdx &= ~ModelSpecificRegisterBit;
    CheckFailure(runner, report, InitialPagingCapabilityCaptureError::ModelSpecificRegistersUnavailable,
                 "missing model-specific-register support is rejected");
    report = FullReport();
    report.BasicFeaturesEdx &= ~PageAttributeTableBit;
    CheckFailure(runner, report, InitialPagingCapabilityCaptureError::PageAttributeTableUnavailable,
                 "missing PAT support is rejected");
    report = FullReport();
    report.BasicFeaturesEdx &= ~GlobalPagesBit;
    CheckFailure(runner, report, InitialPagingCapabilityCaptureError::GlobalPagesUnavailable,
                 "missing global-page support is rejected");
    report = FullReport();
    report.ExtendedFeaturesEdx &= ~LongModeBit;
    CheckFailure(runner, report, InitialPagingCapabilityCaptureError::LongModeUnavailable,
                 "missing long-mode support is rejected");
    report = FullReport();
    report.ExtendedFeaturesEdx &= ~ExecuteDisableBit;
    CheckFailure(runner, report, InitialPagingCapabilityCaptureError::ExecuteDisableUnavailable,
                 "missing execute-disable support is rejected");
    report = FullReport();
    report.AddressSizesEax = 31U;
    CheckFailure(runner, report, InitialPagingCapabilityCaptureError::InvalidPhysicalAddressWidth,
                 "a physical width below the target profile is rejected");
    report = FullReport();
    report.AddressSizesEax = 53U;
    CheckFailure(runner, report, InitialPagingCapabilityCaptureError::InvalidPhysicalAddressWidth,
                 "a physical width above the architectural entry format is rejected");
}

void TestNativeCapture(TestContext& runner) noexcept
{
    const InitialPagingCapabilityReport nativeReport = Platform::Amd64::Memory::TestSupport::CaptureNativeReport();
    PagingCapabilities decoded = Sentinel();
    PagingCapabilities first = Sentinel();
    PagingCapabilities second = Sentinel();
    const InitialPagingCapabilityCaptureError decodedError =
        InitialPagingCapabilityCapture::Decode(nativeReport, decoded);
    const InitialPagingCapabilityCaptureError firstError = InitialPagingCapabilityCapture::Capture(first);
    const InitialPagingCapabilityCaptureError secondError = InitialPagingCapabilityCapture::Capture(second);
    ASTERIA_EXPECT(
        runner, "native capture matches an independent raw CPUID report exactly",
        decodedError == InitialPagingCapabilityCaptureError::None,
        firstError == InitialPagingCapabilityCaptureError::None,
        secondError == InitialPagingCapabilityCaptureError::None,
        IsExactPublishedCapabilities(first, static_cast<Foundation::UInt8>(nativeReport.AddressSizesEax & 0xFFU)),
        EqualCapabilities(first, decoded), EqualCapabilities(first, second));
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"InitialPagingCapabilityCaptureTests"};
    Asteria::Test::TestDecode(runner);
    Asteria::Test::TestNativeCapture(runner);
    return runner.Finish();
}
