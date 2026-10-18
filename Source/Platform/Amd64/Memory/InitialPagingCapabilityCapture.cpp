// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Memory/InitialPagingCapabilityCapture.hpp"

namespace Asteria::Platform::Amd64::Memory
{
namespace
{
constexpr Foundation::UInt32 RequiredBasicLeaf{1U};
constexpr Foundation::UInt32 MaximumExtendedLeafQuery{0x8000'0000U};
constexpr Foundation::UInt32 RequiredExtendedFeatureLeaf{0x8000'0001U};
constexpr Foundation::UInt32 RequiredAddressSizeLeaf{0x8000'0008U};
constexpr Foundation::UInt32 ModelSpecificRegisterBit{1U << 5U};
constexpr Foundation::UInt32 GlobalPagesBit{1U << 13U};
constexpr Foundation::UInt32 PageAttributeTableBit{1U << 16U};
constexpr Foundation::UInt32 ExecuteDisableBit{1U << 20U};
constexpr Foundation::UInt32 LongModeBit{1U << 29U};
constexpr Foundation::UInt32 PhysicalAddressWidthMask{0xFFU};

void ExecuteCpuId(const Foundation::UInt32 leaf, Foundation::UInt32& eax, Foundation::UInt32& ebx,
                  Foundation::UInt32& ecx, Foundation::UInt32& edx) noexcept
{
    constexpr Foundation::UInt32 Subleaf{0U};
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(leaf), "c"(Subleaf) : "memory");
}
} // namespace

InitialPagingCapabilityCaptureError InitialPagingCapabilityCapture::Decode(const InitialPagingCapabilityReport& report,
                                                                           PagingCapabilities& output) noexcept
{
    if (report.Reserved != 0U)
    {
        return InitialPagingCapabilityCaptureError::InvalidReport;
    }
    if (report.MaximumBasicLeaf < RequiredBasicLeaf)
    {
        return InitialPagingCapabilityCaptureError::RequiredBasicLeafUnavailable;
    }
    if (report.MaximumExtendedLeaf < RequiredAddressSizeLeaf)
    {
        return InitialPagingCapabilityCaptureError::RequiredExtendedLeafUnavailable;
    }
    if ((report.BasicFeaturesEdx & ModelSpecificRegisterBit) == 0U)
    {
        return InitialPagingCapabilityCaptureError::ModelSpecificRegistersUnavailable;
    }
    if ((report.BasicFeaturesEdx & PageAttributeTableBit) == 0U)
    {
        return InitialPagingCapabilityCaptureError::PageAttributeTableUnavailable;
    }
    if ((report.BasicFeaturesEdx & GlobalPagesBit) == 0U)
    {
        return InitialPagingCapabilityCaptureError::GlobalPagesUnavailable;
    }
    if ((report.ExtendedFeaturesEdx & LongModeBit) == 0U)
    {
        return InitialPagingCapabilityCaptureError::LongModeUnavailable;
    }
    if ((report.ExtendedFeaturesEdx & ExecuteDisableBit) == 0U)
    {
        return InitialPagingCapabilityCaptureError::ExecuteDisableUnavailable;
    }

    const Foundation::UInt32 physicalAddressBits = report.AddressSizesEax & PhysicalAddressWidthMask;
    if ((physicalAddressBits < 32U) || (physicalAddressBits > 52U))
    {
        return InitialPagingCapabilityCaptureError::InvalidPhysicalAddressWidth;
    }

    output = PagingCapabilities{
        .PhysicalAddressBits = static_cast<Foundation::UInt8>(physicalAddressBits),
        .FourLevelPagingEnabled = true,
        .ExecuteDisableEnabled = true,
        .SupervisorWriteProtectEnabled = true,
        .GlobalPagesEnabled = true,
        .PatLayoutConfigured = true,
        .Reserved16 = 0U,
        .Reserved = 0U,
    };
    return InitialPagingCapabilityCaptureError::None;
}

InitialPagingCapabilityCaptureError InitialPagingCapabilityCapture::Capture(PagingCapabilities& output) noexcept
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

    return Decode(report, output);
}
} // namespace Asteria::Platform::Amd64::Memory
