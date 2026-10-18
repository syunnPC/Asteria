// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Device/PciEcamConfigurationSpace.hpp"

namespace Asteria::Kernel::Device
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 PciBusConfigurationSizeBytes{Foundation::UInt64{1U} << 20U};

[[nodiscard]] constexpr Hal::Device::PciConfigurationReadError
MapReadError(const Hal::Device::MemoryMappedIoReadError error) noexcept
{
    switch (error)
    {
    case Hal::Device::MemoryMappedIoReadError::None:
        return Hal::Device::PciConfigurationReadError::None;
    case Hal::Device::MemoryMappedIoReadError::NotReady:
        return Hal::Device::PciConfigurationReadError::NotReady;
    case Hal::Device::MemoryMappedIoReadError::InvalidAddress:
    case Hal::Device::MemoryMappedIoReadError::UnsupportedWidth:
    case Hal::Device::MemoryMappedIoReadError::HardwareFailure:
        return Hal::Device::PciConfigurationReadError::HardwareFailure;
    }
    return Hal::Device::PciConfigurationReadError::HardwareFailure;
}

[[nodiscard]] constexpr Hal::Device::PciConfigurationWriteError
MapWriteError(const Hal::Device::MemoryMappedIoWriteError error) noexcept
{
    switch (error)
    {
    case Hal::Device::MemoryMappedIoWriteError::None:
        return Hal::Device::PciConfigurationWriteError::None;
    case Hal::Device::MemoryMappedIoWriteError::NotReady:
        return Hal::Device::PciConfigurationWriteError::NotReady;
    case Hal::Device::MemoryMappedIoWriteError::InvalidAddress:
    case Hal::Device::MemoryMappedIoWriteError::UnsupportedWidth:
    case Hal::Device::MemoryMappedIoWriteError::ValueOutOfRange:
    case Hal::Device::MemoryMappedIoWriteError::AccessDenied:
    case Hal::Device::MemoryMappedIoWriteError::HardwareFailure:
        return Hal::Device::PciConfigurationWriteError::HardwareFailure;
    }
    return Hal::Device::PciConfigurationWriteError::HardwareFailure;
}
} // namespace

bool PciEcamConfigurationSpace::PaddingIsZero(const PciEcamRegion& region) noexcept
{
    for (const Foundation::UInt8 value : region.Padding)
    {
        if (value != 0U)
        {
            return false;
        }
    }
    return true;
}

bool PciEcamConfigurationSpace::RegionPrecedes(const PciEcamRegion& left, const PciEcamRegion& right) noexcept
{
    return (left.Segment < right.Segment) ||
           ((left.Segment == right.Segment) &&
            ((left.FirstBus < right.FirstBus) ||
             ((left.FirstBus == right.FirstBus) &&
              ((left.LastBus < right.LastBus) ||
               ((left.LastBus == right.LastBus) && (left.PhysicalStart < right.PhysicalStart))))));
}

bool PciEcamConfigurationSpace::BusRangesOverlap(const PciEcamRegion& left, const PciEcamRegion& right) noexcept
{
    return (left.Segment == right.Segment) && (left.FirstBus <= right.LastBus) && (right.FirstBus <= left.LastBus);
}

bool PciEcamConfigurationSpace::TryGetPhysicalEnd(const PciEcamRegion& region, Foundation::UInt64& end) noexcept
{
    if (!PaddingIsZero(region) || ((region.PhysicalStart & (PciBusConfigurationSizeBytes - 1U)) != 0U) ||
        (region.FirstBus > region.LastBus))
    {
        return false;
    }
    const Foundation::UInt64 busCount =
        static_cast<Foundation::UInt64>(region.LastBus) - static_cast<Foundation::UInt64>(region.FirstBus) + 1U;
    const Foundation::UInt64 lengthBytes = busCount * PciBusConfigurationSizeBytes;
    if (region.PhysicalStart > (MaximumUInt64 - lengthBytes))
    {
        return false;
    }
    end = region.PhysicalStart + lengthBytes;
    return true;
}

bool PciEcamConfigurationSpace::RegionsOverlap(const PciEcamRegion& left, const PciEcamRegion& right) noexcept
{
    Foundation::UInt64 leftEnd{0U};
    Foundation::UInt64 rightEnd{0U};
    if (!TryGetPhysicalEnd(left, leftEnd) || !TryGetPhysicalEnd(right, rightEnd))
    {
        return true;
    }
    return (left.PhysicalStart < rightEnd) && (right.PhysicalStart < leftEnd);
}

PciEcamConfigurationSpaceInitializationError
PciEcamConfigurationSpace::Initialize(const PciEcamRegionView regions) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state != PciEcamConfigurationSpaceState::Uninitialized)
    {
        return PciEcamConfigurationSpaceInitializationError::AlreadyInitialized;
    }
    if ((regions.Count > MaximumRegions) || ((regions.Count != 0U) && (regions.Data == nullptr)))
    {
        return PciEcamConfigurationSpaceInitializationError::InvalidRegionView;
    }
    if (regions.Count == 0U)
    {
        m_state = PciEcamConfigurationSpaceState::Unavailable;
        return PciEcamConfigurationSpaceInitializationError::None;
    }

    PciEcamRegion sorted[MaximumRegions]{};
    for (Foundation::Size index{0U}; index < regions.Count; ++index)
    {
        Foundation::UInt64 end{0U};
        if (!TryGetPhysicalEnd(regions.Data[index], end))
        {
            return PciEcamConfigurationSpaceInitializationError::InvalidRegion;
        }
        sorted[index] = regions.Data[index];
    }
    for (Foundation::Size index{1U}; index < regions.Count; ++index)
    {
        const PciEcamRegion value = sorted[index];
        Foundation::Size destination{index};
        while ((destination != 0U) && RegionPrecedes(value, sorted[destination - 1U]))
        {
            sorted[destination] = sorted[destination - 1U];
            --destination;
        }
        sorted[destination] = value;
    }
    for (Foundation::Size left{0U}; left < regions.Count; ++left)
    {
        for (Foundation::Size right{left + 1U}; right < regions.Count; ++right)
        {
            if (BusRangesOverlap(sorted[left], sorted[right]))
            {
                return PciEcamConfigurationSpaceInitializationError::OverlappingBusRange;
            }
            if (RegionsOverlap(sorted[left], sorted[right]))
            {
                return PciEcamConfigurationSpaceInitializationError::OverlappingPhysicalRange;
            }
        }
    }

    for (Foundation::Size index{0U}; index < regions.Count; ++index)
    {
        m_regions[index] = sorted[index];
    }
    m_regionCount = regions.Count;
    m_state = PciEcamConfigurationSpaceState::Ready;
    return PciEcamConfigurationSpaceInitializationError::None;
}

PciEcamConfigurationSpace::Selection
PciEcamConfigurationSpace::SelectLocked(const PciFunctionAddress address, const Foundation::UInt16 registerOffset,
                                        Foundation::UInt64& physicalAddress) const noexcept
{
    if (m_state == PciEcamConfigurationSpaceState::Uninitialized)
    {
        return Selection::NotReady;
    }
    if (!address.IsValid() || (registerOffset > 0x0FFCU) || ((registerOffset & 3U) != 0U))
    {
        return Selection::InvalidAddress;
    }
    if (m_state == PciEcamConfigurationSpaceState::Unavailable)
    {
        return Selection::Unsupported;
    }

    const PciEcamRegion* selected{nullptr};
    for (Foundation::Size index{0U}; index < m_regionCount; ++index)
    {
        const PciEcamRegion& candidate = m_regions[index];
        if ((candidate.Segment == address.GetSegment()) && (candidate.FirstBus <= address.GetBus()) &&
            (address.GetBus() <= candidate.LastBus))
        {
            selected = &candidate;
            break;
        }
    }
    if (selected == nullptr)
    {
        return Selection::Unsupported;
    }

    const auto calculated = PciEcamAddressCalculator::Calculate(PciEcamAddressRequest{
        .Region = *selected,
        .Address = address,
        .RegisterOffset = registerOffset,
    });
    if (!calculated.HasValue())
    {
        return Selection::HardwareFailure;
    }
    physicalAddress = calculated.GetValue();
    return Selection::Ready;
}

Hal::Device::PciConfigurationReadError PciEcamConfigurationSpace::Read32(const PciFunctionAddress address,
                                                                         const Foundation::UInt16 registerOffset,
                                                                         Foundation::UInt32& output) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    Foundation::UInt64 physicalAddress{0U};
    switch (SelectLocked(address, registerOffset, physicalAddress))
    {
    case Selection::Ready:
        break;
    case Selection::NotReady:
        return Hal::Device::PciConfigurationReadError::NotReady;
    case Selection::InvalidAddress:
        return Hal::Device::PciConfigurationReadError::InvalidAddress;
    case Selection::Unsupported:
        return Hal::Device::PciConfigurationReadError::Unsupported;
    case Selection::HardwareFailure:
        return Hal::Device::PciConfigurationReadError::HardwareFailure;
    }

    Foundation::UInt64 value{0U};
    const Hal::Device::MemoryMappedIoReadError readError =
        m_memoryMappedIo.Read(physicalAddress, Hal::Device::MemoryMappedIoWidth::DoubleWord, value);
    if (readError != Hal::Device::MemoryMappedIoReadError::None)
    {
        return MapReadError(readError);
    }
    if ((value >> 32U) != 0U)
    {
        return Hal::Device::PciConfigurationReadError::HardwareFailure;
    }
    output = static_cast<Foundation::UInt32>(value);
    return Hal::Device::PciConfigurationReadError::None;
}

Hal::Device::PciConfigurationWriteError PciEcamConfigurationSpace::Write32(const PciFunctionAddress address,
                                                                           const Foundation::UInt16 registerOffset,
                                                                           const Foundation::UInt32 value) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    Foundation::UInt64 physicalAddress{0U};
    switch (SelectLocked(address, registerOffset, physicalAddress))
    {
    case Selection::Ready:
        break;
    case Selection::NotReady:
        return Hal::Device::PciConfigurationWriteError::NotReady;
    case Selection::InvalidAddress:
        return Hal::Device::PciConfigurationWriteError::InvalidAddress;
    case Selection::Unsupported:
        return Hal::Device::PciConfigurationWriteError::Unsupported;
    case Selection::HardwareFailure:
        return Hal::Device::PciConfigurationWriteError::HardwareFailure;
    }
    if (m_writer == nullptr)
    {
        return Hal::Device::PciConfigurationWriteError::Unsupported;
    }
    return MapWriteError(m_writer->Write(physicalAddress, Hal::Device::MemoryMappedIoWidth::DoubleWord, value));
}

PciEcamConfigurationSpaceSnapshot PciEcamConfigurationSpace::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return PciEcamConfigurationSpaceSnapshot{
        .RegionCount = m_regionCount,
        .State = m_state,
        .Reserved = {},
    };
}
} // namespace Asteria::Kernel::Device
