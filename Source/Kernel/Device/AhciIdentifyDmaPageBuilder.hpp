// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_DEVICE_AHCI_IDENTIFY_DMA_PAGE_BUILDER_HPP_INCLUDED
#define ASTERIA_KERNEL_DEVICE_AHCI_IDENTIFY_DMA_PAGE_BUILDER_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"

namespace Asteria::Kernel::Device
{
enum class AhciIdentifyDmaPageError : Foundation::UInt8
{
    None,
    NullBuffer,
    InvalidBufferSize,
    InvalidVirtualAlignment,
    PhysicalAddressWrap,
    InvalidPhysicalAlignment,
    ControllerAddressWidthExceeded,
};

struct AhciDmaPageWritableView
{
    Foundation::UInt8* Data;
    Foundation::Size SizeBytes;
    Foundation::UInt64 PhysicalAddress;
};

struct AhciIdentifyDmaPageLayout
{
    Foundation::UInt64 CommandListPhysicalAddress;
    Foundation::UInt64 ReceivedFisPhysicalAddress;
    Foundation::UInt64 CommandTablePhysicalAddress;
    Foundation::UInt64 IdentifyDataPhysicalAddress;
};

using AhciIdentifyDmaPageBuildResult = Foundation::Result<AhciIdentifyDmaPageLayout, AhciIdentifyDmaPageError>;

/// Builds one allocation-free, polling-oriented ATA IDENTIFY DEVICE command. The caller supplies
/// exactly one virtually and physically 4-KiB-aligned, contiguous DMA page. PhysicalAddress is the
/// controller-visible address (the physical address when no IOMMU translation is active). The fixed
/// page layout is: 0x000..0x3ff command list (slot 0 at 0x000) 0x400..0x4ff received-FIS buffer
/// 0x500..0x58f command table with one PRDT entry 0x590..0x5ff untouched 0x600..0x7ff 512-byte
/// IDENTIFY data buffer 0x800..0xfff untouched Success clears every active region before encoding
/// slot 0, so a repeated build also clears stale received FISes, byte counts, table state, and
/// data. The two unused ranges remain byte-for-byte unchanged. Every rejection is detected before
/// the first write and leaves all caller storage unchanged. This class performs no MMIO, command
/// issue, completion polling, or reset.
class AhciIdentifyDmaPageBuilder final
{
public:
    static constexpr Foundation::Size PageSizeBytes{4096U};
    static constexpr Foundation::Size PageAlignmentBytes{4096U};
    static constexpr Foundation::Size CommandListOffsetBytes{0x000U};
    static constexpr Foundation::Size CommandListSizeBytes{1024U};
    static constexpr Foundation::Size CommandListAlignmentBytes{1024U};
    static constexpr Foundation::Size ReceivedFisOffsetBytes{0x400U};
    static constexpr Foundation::Size ReceivedFisSizeBytes{256U};
    static constexpr Foundation::Size ReceivedFisAlignmentBytes{256U};
    static constexpr Foundation::Size CommandTableOffsetBytes{0x500U};
    static constexpr Foundation::Size CommandTableSizeBytes{144U};
    static constexpr Foundation::Size CommandTableAlignmentBytes{128U};
    static constexpr Foundation::Size IdentifyDataOffsetBytes{0x600U};
    static constexpr Foundation::Size IdentifyDataSizeBytes{512U};
    static constexpr Foundation::Size IdentifyDataAlignmentBytes{2U};

    AhciIdentifyDmaPageBuilder() = delete;

    [[nodiscard]] static AhciIdentifyDmaPageBuildResult Build(AhciDmaPageWritableView page,
                                                              bool supports64BitAddressing) noexcept;
};

static_assert(sizeof(AhciIdentifyDmaPageError) == 1U);
static_assert(sizeof(AhciDmaPageWritableView) == 24U);
static_assert(alignof(AhciDmaPageWritableView) == 8U);
static_assert(sizeof(AhciIdentifyDmaPageLayout) == 32U);
static_assert(alignof(AhciIdentifyDmaPageLayout) == 8U);
static_assert(sizeof(AhciIdentifyDmaPageBuildResult) == 40U);
static_assert(alignof(AhciIdentifyDmaPageBuildResult) == 8U);
static_assert((AhciIdentifyDmaPageBuilder::CommandListOffsetBytes %
               AhciIdentifyDmaPageBuilder::CommandListAlignmentBytes) == 0U);
static_assert((AhciIdentifyDmaPageBuilder::ReceivedFisOffsetBytes %
               AhciIdentifyDmaPageBuilder::ReceivedFisAlignmentBytes) == 0U);
static_assert((AhciIdentifyDmaPageBuilder::CommandTableOffsetBytes %
               AhciIdentifyDmaPageBuilder::CommandTableAlignmentBytes) == 0U);
static_assert((AhciIdentifyDmaPageBuilder::IdentifyDataOffsetBytes %
               AhciIdentifyDmaPageBuilder::IdentifyDataAlignmentBytes) == 0U);
static_assert((AhciIdentifyDmaPageBuilder::CommandListOffsetBytes + AhciIdentifyDmaPageBuilder::CommandListSizeBytes) <=
              AhciIdentifyDmaPageBuilder::ReceivedFisOffsetBytes);
static_assert((AhciIdentifyDmaPageBuilder::ReceivedFisOffsetBytes + AhciIdentifyDmaPageBuilder::ReceivedFisSizeBytes) <=
              AhciIdentifyDmaPageBuilder::CommandTableOffsetBytes);
static_assert((AhciIdentifyDmaPageBuilder::CommandTableOffsetBytes +
               AhciIdentifyDmaPageBuilder::CommandTableSizeBytes) <=
              AhciIdentifyDmaPageBuilder::IdentifyDataOffsetBytes);
static_assert((AhciIdentifyDmaPageBuilder::IdentifyDataOffsetBytes +
               AhciIdentifyDmaPageBuilder::IdentifyDataSizeBytes) <= AhciIdentifyDmaPageBuilder::PageSizeBytes);
} // Namespace Asteria::Kernel::Device

#endif // ASTERIA_KERNEL_DEVICE_AHCI_IDENTIFY_DMA_PAGE_BUILDER_HPP_INCLUDED
