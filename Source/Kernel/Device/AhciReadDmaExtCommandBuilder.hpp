// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_DEVICE_AHCI_READ_DMA_EXT_COMMAND_BUILDER_HPP_INCLUDED
#define ASTERIA_KERNEL_DEVICE_AHCI_READ_DMA_EXT_COMMAND_BUILDER_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"

namespace Asteria::Kernel::Device
{
enum class AhciReadDmaExtCommandError : Foundation::UInt8
{
    None,
    NullCommandPage,
    InvalidCommandPageSize,
    InvalidCommandPageVirtualAlignment,
    CommandPagePhysicalAddressWrap,
    InvalidCommandPagePhysicalAlignment,
    ZeroSectorCount,
    SectorCountNotEncodable,
    DeviceDoesNotSupport48BitAddressing,
    UnsupportedLogicalSectorSize,
    TransferByteCountOverflow,
    TransferExceedsPrdtEntry,
    LbaNotEncodable,
    TransferOutsideDeviceCapacity,
    InvalidDataBufferSize,
    DataBufferPhysicalAddressWrap,
    InvalidDataBufferPhysicalAlignment,
    ControllerAddressWidthExceeded,
    CommandPageOverlapsDataBuffer,
    CommandPageOverlapsRequest,
};

struct AhciReadDmaExtCommandPageWritableView
{
    Foundation::UInt8* Data;
    Foundation::Size SizeBytes;
    Foundation::UInt64 PhysicalAddress;
};

/// An exact destination span in the controller-visible DMA address space.
/// The builder records this span in the PRDT but never dereferences or clears
/// destination storage.
struct AhciReadDmaExtDataBufferSpan
{
    Foundation::UInt64 PhysicalAddress;
    Foundation::UInt64 SizeBytes;
};

struct AhciReadDmaExtCommandRequest
{
    AhciReadDmaExtCommandPageWritableView CommandPage;
    AhciReadDmaExtDataBufferSpan DataBuffer;
    Foundation::UInt64 DeviceSectorCount;
    Foundation::UInt64 StartingLba;
    Foundation::UInt64 LogicalSectorSizeBytes;
    Foundation::UInt64 SectorCount;
    bool Supports64BitAddressing;
    bool Supports48BitAddressing;
    Foundation::UInt8 Reserved[6];
};

struct AhciReadDmaExtCommandLayout
{
    Foundation::UInt64 CommandListPhysicalAddress;
    Foundation::UInt64 ReceivedFisPhysicalAddress;
    Foundation::UInt64 CommandTablePhysicalAddress;
    Foundation::UInt64 DataBufferPhysicalAddress;
    Foundation::UInt64 TransferSizeBytes;
};

using AhciReadDmaExtCommandBuildResult = Foundation::Result<AhciReadDmaExtCommandLayout, AhciReadDmaExtCommandError>;

/// Builds one allocation-free ATA READ DMA EXT command in AHCI slot 0. CommandPage is exactly one
/// virtually and controller-address aligned 4-KiB page. Its fixed layout is: 0x000..0x3ff command
/// list (slot 0 at 0x000) 0x400..0x4ff received-FIS buffer 0x500..0x58f command table with one PRDT
/// entry 0x590..0xfff untouched DataBuffer is an external, exact-size controller-visible
/// destination. It must not overlap any byte of CommandPage. The request object must also be
/// virtually disjoint from the complete command page because the builder continues reading it while
/// encoding. LogicalSectorSizeBytes accepts even ATA logical-sector sizes of at least 512 bytes;
/// the single-PRDT limit bounds the largest usable size. SectorCount is the semantic count from 1
/// through 65,536; ATA encodes 65,536 as 0000h. The device must advertise 48-bit ATA addressing.
/// Success clears only the command list, received-FIS buffer, and command table before encoding the
/// command; it never accesses destination storage. Every failure is detected before the first write
/// and leaves the complete command page and request unchanged. This class performs no allocation,
/// MMIO, command issue, polling, or completion work.
class AhciReadDmaExtCommandBuilder final
{
public:
    static constexpr Foundation::Size CommandPageSizeBytes{4096U};
    static constexpr Foundation::Size CommandPageAlignmentBytes{4096U};
    static constexpr Foundation::Size CommandListOffsetBytes{0x000U};
    static constexpr Foundation::Size CommandListSizeBytes{1024U};
    static constexpr Foundation::Size CommandListAlignmentBytes{1024U};
    static constexpr Foundation::Size ReceivedFisOffsetBytes{0x400U};
    static constexpr Foundation::Size ReceivedFisSizeBytes{256U};
    static constexpr Foundation::Size ReceivedFisAlignmentBytes{256U};
    static constexpr Foundation::Size CommandTableOffsetBytes{0x500U};
    static constexpr Foundation::Size CommandTableSizeBytes{144U};
    static constexpr Foundation::Size CommandTableAlignmentBytes{128U};
    static constexpr Foundation::Size DataBufferAlignmentBytes{2U};
    static constexpr Foundation::UInt64 MaximumPrdtTransferSizeBytes{4U * 1024U * 1024U};
    static constexpr Foundation::UInt64 MaximumLba{0xFFFF'FFFF'FFFFULL};
    static constexpr Foundation::UInt64 MaximumSectorCount{0x1'0000U};
    static constexpr Foundation::UInt64 MinimumLogicalSectorSizeBytes{512U};
    static constexpr Foundation::UInt64 LogicalSectorSizeAlignmentBytes{2U};

    AhciReadDmaExtCommandBuilder() = delete;

    [[nodiscard]] static AhciReadDmaExtCommandBuildResult Build(const AhciReadDmaExtCommandRequest& request) noexcept;
};

static_assert(sizeof(AhciReadDmaExtCommandError) == 1U);
static_assert(sizeof(AhciReadDmaExtCommandPageWritableView) == 24U);
static_assert(alignof(AhciReadDmaExtCommandPageWritableView) == 8U);
static_assert(sizeof(AhciReadDmaExtDataBufferSpan) == 16U);
static_assert(alignof(AhciReadDmaExtDataBufferSpan) == 8U);
static_assert(sizeof(AhciReadDmaExtCommandRequest) == 80U);
static_assert(alignof(AhciReadDmaExtCommandRequest) == 8U);
static_assert(sizeof(AhciReadDmaExtCommandLayout) == 40U);
static_assert(alignof(AhciReadDmaExtCommandLayout) == 8U);
static_assert(sizeof(AhciReadDmaExtCommandBuildResult) == 48U);
static_assert(alignof(AhciReadDmaExtCommandBuildResult) == 8U);
static_assert((AhciReadDmaExtCommandBuilder::CommandListOffsetBytes %
               AhciReadDmaExtCommandBuilder::CommandListAlignmentBytes) == 0U);
static_assert((AhciReadDmaExtCommandBuilder::ReceivedFisOffsetBytes %
               AhciReadDmaExtCommandBuilder::ReceivedFisAlignmentBytes) == 0U);
static_assert((AhciReadDmaExtCommandBuilder::CommandTableOffsetBytes %
               AhciReadDmaExtCommandBuilder::CommandTableAlignmentBytes) == 0U);
static_assert((AhciReadDmaExtCommandBuilder::CommandListOffsetBytes +
               AhciReadDmaExtCommandBuilder::CommandListSizeBytes) <=
              AhciReadDmaExtCommandBuilder::ReceivedFisOffsetBytes);
static_assert((AhciReadDmaExtCommandBuilder::ReceivedFisOffsetBytes +
               AhciReadDmaExtCommandBuilder::ReceivedFisSizeBytes) <=
              AhciReadDmaExtCommandBuilder::CommandTableOffsetBytes);
static_assert((AhciReadDmaExtCommandBuilder::CommandTableOffsetBytes +
               AhciReadDmaExtCommandBuilder::CommandTableSizeBytes) <=
              AhciReadDmaExtCommandBuilder::CommandPageSizeBytes);
} // Namespace Asteria::Kernel::Device

#endif // ASTERIA_KERNEL_DEVICE_AHCI_READ_DMA_EXT_COMMAND_BUILDER_HPP_INCLUDED
