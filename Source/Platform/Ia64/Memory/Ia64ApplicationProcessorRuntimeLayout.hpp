// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_MEMORY_IA64_APPLICATION_PROCESSOR_RUNTIME_LAYOUT_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_MEMORY_IA64_APPLICATION_PROCESSOR_RUNTIME_LAYOUT_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Memory/BootstrapPageAllocator.hpp"
#include "Kernel/Scheduler/SchedulerTypes.hpp"
#include "Platform/Ia64/Memory/ApplicationProcessorExecutionStoragePreparer.hpp"

namespace Asteria::Platform::Ia64::Memory
{
constexpr Foundation::UInt64 Ia64ApplicationProcessorRuntimeRegionOneBase{0x2000'0000'0400'0000ULL};
constexpr Foundation::UInt64 Ia64ApplicationProcessorRuntimeProcessorStride{0x2'0000U};
constexpr Foundation::UInt64 Ia64ApplicationProcessorRuntimeIdleAliasOffset{0U};
constexpr Foundation::UInt64 Ia64ApplicationProcessorRuntimeIdleAliasSizeBytes{0x1'0000U};
constexpr Foundation::UInt64 Ia64ApplicationProcessorRuntimeFirstGuardOffset{0x1'0000U};
constexpr Foundation::UInt64 Ia64ApplicationProcessorRuntimeFirstGuardSizeBytes{0x4000U};
constexpr Foundation::UInt64 Ia64ApplicationProcessorRuntimeWorkerAliasOffset{0x1'4000U};
constexpr Foundation::UInt64 Ia64ApplicationProcessorRuntimeWorkerAliasSizeBytes{0x8000U};
constexpr Foundation::UInt64 Ia64ApplicationProcessorRuntimeSecondGuardOffset{0x1'C000U};
constexpr Foundation::UInt64 Ia64ApplicationProcessorRuntimeSecondGuardSizeBytes{0x4000U};
constexpr Foundation::UInt64 Ia64ApplicationProcessorRuntimeIdleStackSizeBytes{0x8000U};
constexpr Foundation::UInt64 Ia64ApplicationProcessorRuntimeIdleRseSizeBytes{0x8000U};
constexpr Foundation::UInt64 Ia64ApplicationProcessorRuntimeWorkerStackSizeBytes{0x4000U};
constexpr Foundation::UInt64 Ia64ApplicationProcessorRuntimeWorkerRseSizeBytes{0x4000U};
constexpr Foundation::UInt64 Ia64ApplicationProcessorRuntimeInitialStackScratchSizeBytes{16U};
constexpr Foundation::UInt64 Ia64ApplicationProcessorRuntimeWorkerPagesPerProcessor{8U};
constexpr Foundation::UInt64 Ia64ApplicationProcessorRuntimeWorkerPoolAlignmentPages{4U};
constexpr Foundation::UInt8 Ia64ApplicationProcessorRuntimeEncoderPhysicalAddressBits{50U};
constexpr Foundation::UInt8 Ia64ApplicationProcessorRuntimeMinimumVirtualAddressMostSignificantBit{50U};
constexpr Foundation::Size Ia64ApplicationProcessorRuntimeMaximumProcessorCount{
    static_cast<Foundation::Size>(Kernel::Scheduler::MaximumProcessorCount - 1U)};

struct Ia64ApplicationProcessorRuntimeLayoutRequest
{
    ApplicationProcessorExecutionStorageDescriptorView IdleStorage;
    Kernel::Memory::PhysicalPageRange WorkerPool;
    Foundation::UInt8 ProcessorCount;
    Foundation::UInt8 EnabledProcessorMask;
    Foundation::UInt8 CommonPhysicalAddressBits;
    Foundation::UInt8 CommonImplementedVirtualAddressMostSignificantBit;
};

/// These three bases derive every alias, guard, physical half, SP, and
/// BSPSTORE value without retaining duplicate ranges.
struct Ia64ApplicationProcessorRuntimeProcessorLayout
{
    Foundation::UInt64 SlotVirtualBase;
    Foundation::UInt64 IdlePhysicalBase;
    Foundation::UInt64 WorkerPhysicalBase;
    Kernel::Scheduler::CpuId Processor;
    Foundation::UInt8 Reserved[7];
};

struct Ia64ApplicationProcessorRuntimeLayoutPlan
{
    Ia64ApplicationProcessorRuntimeProcessorLayout Processors[Ia64ApplicationProcessorRuntimeMaximumProcessorCount];
    Foundation::UInt8 ProcessorCount;
    Foundation::UInt8 EnabledProcessorMask;
    Foundation::UInt8 LayoutCount;
    Foundation::UInt8 Reserved[5];
};

enum class Ia64ApplicationProcessorRuntimeLayoutError : Foundation::UInt32
{
    None,
    InvalidProcessorTopology,
    InvalidCapabilities,
    InvalidIdleStorage,
    InvalidWorkerPool,
    PhysicalAddressOutOfRange,
    PhysicalRangeOverlap,
};

namespace Ia64ApplicationProcessorRuntimeLayoutDetail
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 PageSizeBytes{Kernel::Memory::PhysicalPageSizeBytes};
constexpr Foundation::UInt64 PageOffsetMask{PageSizeBytes - 1U};
constexpr Foundation::UInt64 FirmwareMemoryWriteBackAttribute{Foundation::UInt64{1U} << 3U};
constexpr Foundation::UInt64 FirmwareMemoryRuntimeAttribute{Foundation::UInt64{1U} << 63U};
constexpr Foundation::UInt32 ConventionalMemoryType{7U};

struct ByteRange
{
    Foundation::UInt64 Start;
    Foundation::UInt64 SizeBytes;
};

[[nodiscard]] constexpr bool TryGetBytes(const Kernel::Memory::PhysicalPageRange& range, ByteRange& bytes) noexcept
{
    if (((range.PhysicalStart & PageOffsetMask) != 0U) || (range.PageCount > (MaximumUInt64 / PageSizeBytes)))
    {
        return false;
    }
    const Foundation::UInt64 sizeBytes = range.PageCount * PageSizeBytes;
    if (range.PhysicalStart > (MaximumUInt64 - sizeBytes))
    {
        return false;
    }
    bytes = ByteRange{range.PhysicalStart, sizeBytes};
    return true;
}

[[nodiscard]] constexpr bool Overlaps(const ByteRange left, const ByteRange right) noexcept
{
    return (left.Start < (right.Start + right.SizeBytes)) && (right.Start < (left.Start + left.SizeBytes));
}

[[nodiscard]] constexpr bool IsEncodable(const ByteRange range, const Foundation::UInt8 physicalAddressBits) noexcept
{
    const Foundation::UInt8 bits = physicalAddressBits < Ia64ApplicationProcessorRuntimeEncoderPhysicalAddressBits
                                       ? physicalAddressBits
                                       : Ia64ApplicationProcessorRuntimeEncoderPhysicalAddressBits;
    return (range.SizeBytes != 0U) && (range.Start <= (MaximumUInt64 - range.SizeBytes)) &&
           ((range.Start + range.SizeBytes) <= (Foundation::UInt64{1U} << bits));
}

[[nodiscard]] constexpr bool IsRegionOneMsb53Address(const Foundation::UInt64 address) noexcept
{
    constexpr Foundation::UInt64 canonicalExtensionMask{0x1FE0'0000'0000'0000ULL};
    const Foundation::UInt64 extension = address & canonicalExtensionMask;
    return ((address >> 61U) == 1U) && ((extension == 0U) || (extension == canonicalExtensionMask));
}

[[nodiscard]] constexpr bool
ReservedBytesAreZero(const ApplicationProcessorExecutionStorageDescriptor& descriptor) noexcept
{
    for (Foundation::Size index{0U}; index < sizeof(descriptor.Reserved); ++index)
    {
        if (descriptor.Reserved[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool IsIdleDescriptorExact(const ApplicationProcessorExecutionStorageDescriptor& descriptor,
                                                   const Foundation::Size index) noexcept
{
    return (descriptor.AllocationPhysicalStart != 0U) &&
           ((descriptor.AllocationPhysicalStart &
             ((ApplicationProcessorExecutionStorageAlignmentPages * PageSizeBytes) - 1U)) == 0U) &&
           (descriptor.AllocationPhysicalStart <= (MaximumUInt64 - ApplicationProcessorExecutionStorageSizeBytes)) &&
           (descriptor.AllocationSizeBytes == ApplicationProcessorExecutionStorageSizeBytes) &&
           (descriptor.MemoryStackPhysicalStart == descriptor.AllocationPhysicalStart) &&
           (descriptor.MemoryStackSizeBytes == Ia64ApplicationProcessorRuntimeIdleStackSizeBytes) &&
           (descriptor.InitialStackPointer == descriptor.AllocationPhysicalStart +
                                                  Ia64ApplicationProcessorRuntimeIdleStackSizeBytes -
                                                  Ia64ApplicationProcessorRuntimeInitialStackScratchSizeBytes) &&
           (descriptor.RseBackingStorePhysicalStart ==
            descriptor.AllocationPhysicalStart + Ia64ApplicationProcessorRuntimeIdleStackSizeBytes) &&
           (descriptor.RseBackingStoreSizeBytes == Ia64ApplicationProcessorRuntimeIdleRseSizeBytes) &&
           descriptor.Processor.IsValid() &&
           (descriptor.Processor.GetValue() == static_cast<Foundation::UInt8>(index + 1U)) &&
           ReservedBytesAreZero(descriptor);
}
} // namespace Ia64ApplicationProcessorRuntimeLayoutDetail

/// Pure AP geometry validation and planning. Every check completes before the
/// single output assignment, so failure leaves output unchanged.
class Ia64ApplicationProcessorRuntimeLayoutPlanner final
{
public:
    [[nodiscard]] static constexpr Foundation::UInt64
    GetIdleMemoryStackVirtualStart(const Ia64ApplicationProcessorRuntimeProcessorLayout& layout) noexcept
    {
        return layout.SlotVirtualBase + Ia64ApplicationProcessorRuntimeIdleAliasOffset;
    }

    [[nodiscard]] static constexpr Foundation::UInt64
    GetIdleRseBackingStoreVirtualStart(const Ia64ApplicationProcessorRuntimeProcessorLayout& layout) noexcept
    {
        return GetIdleMemoryStackVirtualStart(layout) + Ia64ApplicationProcessorRuntimeIdleStackSizeBytes;
    }

    [[nodiscard]] static constexpr Foundation::UInt64
    GetIdleRseBackingStorePhysicalStart(const Ia64ApplicationProcessorRuntimeProcessorLayout& layout) noexcept
    {
        return layout.IdlePhysicalBase + Ia64ApplicationProcessorRuntimeIdleStackSizeBytes;
    }

    [[nodiscard]] static constexpr Foundation::UInt64
    GetIdleInitialStackPointer(const Ia64ApplicationProcessorRuntimeProcessorLayout& layout) noexcept
    {
        return GetIdleRseBackingStoreVirtualStart(layout) - Ia64ApplicationProcessorRuntimeInitialStackScratchSizeBytes;
    }

    [[nodiscard]] static constexpr Foundation::UInt64
    GetIdleInitialBspstore(const Ia64ApplicationProcessorRuntimeProcessorLayout& layout) noexcept
    {
        return GetIdleRseBackingStoreVirtualStart(layout);
    }

    [[nodiscard]] static constexpr Foundation::UInt64
    GetFirstGuardVirtualStart(const Ia64ApplicationProcessorRuntimeProcessorLayout& layout) noexcept
    {
        return layout.SlotVirtualBase + Ia64ApplicationProcessorRuntimeFirstGuardOffset;
    }

    [[nodiscard]] static constexpr Foundation::UInt64
    GetWorkerAliasVirtualStart(const Ia64ApplicationProcessorRuntimeProcessorLayout& layout) noexcept
    {
        return layout.SlotVirtualBase + Ia64ApplicationProcessorRuntimeWorkerAliasOffset;
    }

    [[nodiscard]] static constexpr Foundation::UInt64
    GetWorkerRseBackingStoreVirtualStart(const Ia64ApplicationProcessorRuntimeProcessorLayout& layout) noexcept
    {
        return GetWorkerAliasVirtualStart(layout) + Ia64ApplicationProcessorRuntimeWorkerStackSizeBytes;
    }

    [[nodiscard]] static constexpr Foundation::UInt64
    GetWorkerRseBackingStorePhysicalStart(const Ia64ApplicationProcessorRuntimeProcessorLayout& layout) noexcept
    {
        return layout.WorkerPhysicalBase + Ia64ApplicationProcessorRuntimeWorkerStackSizeBytes;
    }

    [[nodiscard]] static constexpr Foundation::UInt64
    GetWorkerInitialStackPointer(const Ia64ApplicationProcessorRuntimeProcessorLayout& layout) noexcept
    {
        return GetWorkerRseBackingStoreVirtualStart(layout) -
               Ia64ApplicationProcessorRuntimeInitialStackScratchSizeBytes;
    }

    [[nodiscard]] static constexpr Foundation::UInt64
    GetWorkerInitialBspstore(const Ia64ApplicationProcessorRuntimeProcessorLayout& layout) noexcept
    {
        return GetWorkerRseBackingStoreVirtualStart(layout);
    }

    [[nodiscard]] static constexpr Foundation::UInt64
    GetSecondGuardVirtualStart(const Ia64ApplicationProcessorRuntimeProcessorLayout& layout) noexcept
    {
        return layout.SlotVirtualBase + Ia64ApplicationProcessorRuntimeSecondGuardOffset;
    }

    [[nodiscard]] static constexpr Ia64ApplicationProcessorRuntimeLayoutError
    Validate(const Ia64ApplicationProcessorRuntimeLayoutRequest& request) noexcept
    {
        using Ia64ApplicationProcessorRuntimeLayoutDetail::ByteRange;
        using Ia64ApplicationProcessorRuntimeLayoutDetail::ConventionalMemoryType;
        using Ia64ApplicationProcessorRuntimeLayoutDetail::FirmwareMemoryRuntimeAttribute;
        using Ia64ApplicationProcessorRuntimeLayoutDetail::FirmwareMemoryWriteBackAttribute;
        using Ia64ApplicationProcessorRuntimeLayoutDetail::IsEncodable;
        using Ia64ApplicationProcessorRuntimeLayoutDetail::IsIdleDescriptorExact;
        using Ia64ApplicationProcessorRuntimeLayoutDetail::Overlaps;
        using Ia64ApplicationProcessorRuntimeLayoutDetail::PageSizeBytes;
        using Ia64ApplicationProcessorRuntimeLayoutDetail::TryGetBytes;

        if ((request.ProcessorCount == 0U) || (request.ProcessorCount > Kernel::Scheduler::MaximumProcessorCount))
        {
            return Ia64ApplicationProcessorRuntimeLayoutError::InvalidProcessorTopology;
        }
        const Foundation::UInt8 expectedMask =
            static_cast<Foundation::UInt8>((Foundation::UInt32{1U} << request.ProcessorCount) - 1U);
        const Foundation::Size applicationProcessorCount = static_cast<Foundation::Size>(request.ProcessorCount - 1U);
        if ((request.EnabledProcessorMask != expectedMask) ||
            (request.IdleStorage.Count != applicationProcessorCount) ||
            ((applicationProcessorCount == 0U) != (request.IdleStorage.Data == nullptr)))
        {
            return Ia64ApplicationProcessorRuntimeLayoutError::InvalidProcessorTopology;
        }
        if ((request.CommonPhysicalAddressBits < 32U) || (request.CommonPhysicalAddressBits > 63U) ||
            (request.CommonImplementedVirtualAddressMostSignificantBit <
             Ia64ApplicationProcessorRuntimeMinimumVirtualAddressMostSignificantBit) ||
            (request.CommonImplementedVirtualAddressMostSignificantBit > 60U))
        {
            return Ia64ApplicationProcessorRuntimeLayoutError::InvalidCapabilities;
        }
        ByteRange workerBytes{};
        if (applicationProcessorCount == 0U)
        {
            if ((request.WorkerPool.PhysicalStart != 0U) || (request.WorkerPool.PageCount != 0U) ||
                (request.WorkerPool.Attributes != 0U) || (request.WorkerPool.FirmwareType != 0U) ||
                (request.WorkerPool.Reserved != 0U))
            {
                return Ia64ApplicationProcessorRuntimeLayoutError::InvalidWorkerPool;
            }
        }
        else
        {
            const Foundation::UInt64 expectedWorkerPages =
                applicationProcessorCount * Ia64ApplicationProcessorRuntimeWorkerPagesPerProcessor;
            const Foundation::UInt64 alignmentBytes =
                Ia64ApplicationProcessorRuntimeWorkerPoolAlignmentPages * PageSizeBytes;
            if ((request.WorkerPool.PhysicalStart == 0U) ||
                ((request.WorkerPool.PhysicalStart & (alignmentBytes - 1U)) != 0U) ||
                (request.WorkerPool.PageCount != expectedWorkerPages) ||
                ((request.WorkerPool.Attributes & FirmwareMemoryWriteBackAttribute) == 0U) ||
                ((request.WorkerPool.Attributes & FirmwareMemoryRuntimeAttribute) != 0U) ||
                (request.WorkerPool.FirmwareType != ConventionalMemoryType) || (request.WorkerPool.Reserved != 0U) ||
                !TryGetBytes(request.WorkerPool, workerBytes))
            {
                return Ia64ApplicationProcessorRuntimeLayoutError::InvalidWorkerPool;
            }
            if (!IsEncodable(workerBytes, request.CommonPhysicalAddressBits))
            {
                return Ia64ApplicationProcessorRuntimeLayoutError::PhysicalAddressOutOfRange;
            }
        }

        for (Foundation::Size index{0U}; index < applicationProcessorCount; ++index)
        {
            const ApplicationProcessorExecutionStorageDescriptor& descriptor = request.IdleStorage.Data[index];
            if (!IsIdleDescriptorExact(descriptor, index))
            {
                return Ia64ApplicationProcessorRuntimeLayoutError::InvalidIdleStorage;
            }
            const ByteRange idleBytes{descriptor.AllocationPhysicalStart, descriptor.AllocationSizeBytes};
            if (!IsEncodable(idleBytes, request.CommonPhysicalAddressBits))
            {
                return Ia64ApplicationProcessorRuntimeLayoutError::PhysicalAddressOutOfRange;
            }
            if (Overlaps(idleBytes, workerBytes))
            {
                return Ia64ApplicationProcessorRuntimeLayoutError::PhysicalRangeOverlap;
            }
            for (Foundation::Size prior{0U}; prior < index; ++prior)
            {
                const ByteRange priorBytes{request.IdleStorage.Data[prior].AllocationPhysicalStart,
                                           request.IdleStorage.Data[prior].AllocationSizeBytes};
                if (Overlaps(idleBytes, priorBytes))
                {
                    return Ia64ApplicationProcessorRuntimeLayoutError::PhysicalRangeOverlap;
                }
            }
        }
        return Ia64ApplicationProcessorRuntimeLayoutError::None;
    }

    [[nodiscard]] static constexpr Ia64ApplicationProcessorRuntimeLayoutError
    Plan(const Ia64ApplicationProcessorRuntimeLayoutRequest& request,
         Ia64ApplicationProcessorRuntimeLayoutPlan& output) noexcept
    {
        const Ia64ApplicationProcessorRuntimeLayoutError error = Validate(request);
        if (error != Ia64ApplicationProcessorRuntimeLayoutError::None)
        {
            return error;
        }

        Ia64ApplicationProcessorRuntimeLayoutPlan planned{};
        planned.ProcessorCount = request.ProcessorCount;
        planned.EnabledProcessorMask = request.EnabledProcessorMask;
        planned.LayoutCount = static_cast<Foundation::UInt8>(request.IdleStorage.Count);
        for (Foundation::Size index{0U}; index < request.IdleStorage.Count; ++index)
        {
            planned.Processors[index] = Ia64ApplicationProcessorRuntimeProcessorLayout{
                .SlotVirtualBase = Ia64ApplicationProcessorRuntimeRegionOneBase +
                                   (index * Ia64ApplicationProcessorRuntimeProcessorStride),
                .IdlePhysicalBase = request.IdleStorage.Data[index].AllocationPhysicalStart,
                .WorkerPhysicalBase =
                    request.WorkerPool.PhysicalStart + (index * Ia64ApplicationProcessorRuntimeWorkerPagesPerProcessor *
                                                        Kernel::Memory::PhysicalPageSizeBytes),
                .Processor = Kernel::Scheduler::CpuId{static_cast<Foundation::UInt8>(index + 1U)},
                .Reserved = {},
            };
        }
        output = planned;
        return Ia64ApplicationProcessorRuntimeLayoutError::None;
    }

private:
    Ia64ApplicationProcessorRuntimeLayoutPlanner() = delete;
};

static_assert(Ia64ApplicationProcessorRuntimeMaximumProcessorCount == 3U);
static_assert(Ia64ApplicationProcessorRuntimeRegionOneBase == 0x2000'0000'0400'0000ULL);
static_assert(Ia64ApplicationProcessorRuntimeProcessorStride == 0x2'0000U);
static_assert(Ia64ApplicationProcessorRuntimeIdleAliasOffset == 0U);
static_assert(Ia64ApplicationProcessorRuntimeIdleAliasSizeBytes == 0x1'0000U);
static_assert(Ia64ApplicationProcessorRuntimeFirstGuardOffset == 0x1'0000U);
static_assert(Ia64ApplicationProcessorRuntimeWorkerAliasOffset == 0x1'4000U);
static_assert(Ia64ApplicationProcessorRuntimeSecondGuardOffset == 0x1'C000U);
static_assert(Ia64ApplicationProcessorRuntimeIdleStackSizeBytes + Ia64ApplicationProcessorRuntimeIdleRseSizeBytes ==
              Ia64ApplicationProcessorRuntimeIdleAliasSizeBytes);
static_assert(Ia64ApplicationProcessorRuntimeIdleAliasOffset + Ia64ApplicationProcessorRuntimeIdleAliasSizeBytes ==
              Ia64ApplicationProcessorRuntimeFirstGuardOffset);
static_assert(Ia64ApplicationProcessorRuntimeFirstGuardOffset + Ia64ApplicationProcessorRuntimeFirstGuardSizeBytes ==
              Ia64ApplicationProcessorRuntimeWorkerAliasOffset);
static_assert(Ia64ApplicationProcessorRuntimeWorkerStackSizeBytes + Ia64ApplicationProcessorRuntimeWorkerRseSizeBytes ==
              Ia64ApplicationProcessorRuntimeWorkerAliasSizeBytes);
static_assert(Ia64ApplicationProcessorRuntimeWorkerAliasOffset + Ia64ApplicationProcessorRuntimeWorkerAliasSizeBytes ==
              Ia64ApplicationProcessorRuntimeSecondGuardOffset);
static_assert(Ia64ApplicationProcessorRuntimeSecondGuardOffset + Ia64ApplicationProcessorRuntimeSecondGuardSizeBytes ==
              Ia64ApplicationProcessorRuntimeProcessorStride);
static_assert(Ia64ApplicationProcessorRuntimeIdleStackSizeBytes == 32U * 1024U);
static_assert(Ia64ApplicationProcessorRuntimeIdleRseSizeBytes == 32U * 1024U);
static_assert(Ia64ApplicationProcessorRuntimeWorkerStackSizeBytes == 16U * 1024U);
static_assert(Ia64ApplicationProcessorRuntimeWorkerRseSizeBytes == 16U * 1024U);
static_assert(Ia64ApplicationProcessorRuntimeWorkerPagesPerProcessor == 8U);
static_assert(Ia64ApplicationProcessorRuntimeWorkerPoolAlignmentPages == 4U);
static_assert(
    Ia64ApplicationProcessorRuntimeLayoutDetail::IsRegionOneMsb53Address(Ia64ApplicationProcessorRuntimeRegionOneBase));
static_assert(Ia64ApplicationProcessorRuntimeLayoutDetail::IsRegionOneMsb53Address(
    Ia64ApplicationProcessorRuntimeRegionOneBase +
    (Ia64ApplicationProcessorRuntimeMaximumProcessorCount * Ia64ApplicationProcessorRuntimeProcessorStride) - 1U));
static_assert(Ia64ApplicationProcessorRuntimeLayoutDetail::IsRegionOneMsb53Address(0x3FFF'FFFF'FFFF'FFFFULL));
static_assert(!Ia64ApplicationProcessorRuntimeLayoutDetail::IsRegionOneMsb53Address(0x2020'0000'0000'0000ULL));
static_assert(sizeof(Ia64ApplicationProcessorRuntimeLayoutRequest) == 56U);
static_assert(sizeof(Ia64ApplicationProcessorRuntimeProcessorLayout) == 32U);
static_assert(sizeof(Ia64ApplicationProcessorRuntimeLayoutPlan) == 104U);
static_assert(sizeof(Ia64ApplicationProcessorRuntimeLayoutError) == 4U);
} // namespace Asteria::Platform::Ia64::Memory

#endif // ASTERIA_PLATFORM_IA64_MEMORY_IA64_APPLICATION_PROCESSOR_RUNTIME_LAYOUT_HPP_INCLUDED
