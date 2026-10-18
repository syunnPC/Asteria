// SPDX-License-Identifier: Apache-2.0

#include "Platform/Device/PhysicalMemoryMappedIo.hpp"

namespace Asteria::Platform::Device
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 DoubleWordSizeBytes{4U};
constexpr Foundation::UInt64 MaximumUInt32{0xFFFF'FFFFU};

[[nodiscard]] constexpr bool RangesOverlap(const Foundation::UInt64 leftStart, const Foundation::UInt64 leftEnd,
                                           const Foundation::UInt64 rightStart,
                                           const Foundation::UInt64 rightEnd) noexcept
{
    return (leftStart < rightEnd) && (rightStart < leftEnd);
}

[[nodiscard]] constexpr Hal::Device::MemoryMappedIoReadError
MapRetainError(const Kernel::Device::DeviceRegionError error) noexcept
{
    switch (error)
    {
    case Kernel::Device::DeviceRegionError::None:
        return Hal::Device::MemoryMappedIoReadError::None;
    case Kernel::Device::DeviceRegionError::UnsupportedWidth:
        return Hal::Device::MemoryMappedIoReadError::UnsupportedWidth;
    case Kernel::Device::DeviceRegionError::RangeExceeded:
    case Kernel::Device::DeviceRegionError::MisalignedAccess:
        return Hal::Device::MemoryMappedIoReadError::InvalidAddress;
    case Kernel::Device::DeviceRegionError::NotReady:
    case Kernel::Device::DeviceRegionError::Quiesced:
        return Hal::Device::MemoryMappedIoReadError::NotReady;
    case Kernel::Device::DeviceRegionError::InvalidConfiguration:
    case Kernel::Device::DeviceRegionError::InvalidDescriptor:
    case Kernel::Device::DeviceRegionError::AlreadyInitialized:
    case Kernel::Device::DeviceRegionError::ResourceDomainPinRejected:
    case Kernel::Device::DeviceRegionError::ResourceDomainInstanceMismatch:
    case Kernel::Device::DeviceRegionError::ResourceDomainRejected:
    case Kernel::Device::DeviceRegionError::WriteValueDenied:
    case Kernel::Device::DeviceRegionError::InvalidOperation:
    case Kernel::Device::DeviceRegionError::UnsupportedAccess:
    case Kernel::Device::DeviceRegionError::MappingNotSupported:
    case Kernel::Device::DeviceRegionError::MappingAccessDenied:
    case Kernel::Device::DeviceRegionError::MappingCacheMismatch:
    case Kernel::Device::DeviceRegionError::ReferenceExhausted:
    case Kernel::Device::DeviceRegionError::AccessReferenceUnderflow:
    case Kernel::Device::DeviceRegionError::MappingReferenceUnderflow:
    case Kernel::Device::DeviceRegionError::InUse:
    case Kernel::Device::DeviceRegionError::InvariantViolation:
        return Hal::Device::MemoryMappedIoReadError::HardwareFailure;
    }
    return Hal::Device::MemoryMappedIoReadError::HardwareFailure;
}

[[nodiscard]] constexpr Hal::Device::MemoryMappedIoWriteError
MapWriteRetainError(const Kernel::Device::DeviceRegionError error) noexcept
{
    switch (error)
    {
    case Kernel::Device::DeviceRegionError::None:
        return Hal::Device::MemoryMappedIoWriteError::None;
    case Kernel::Device::DeviceRegionError::UnsupportedWidth:
        return Hal::Device::MemoryMappedIoWriteError::UnsupportedWidth;
    case Kernel::Device::DeviceRegionError::RangeExceeded:
    case Kernel::Device::DeviceRegionError::MisalignedAccess:
        return Hal::Device::MemoryMappedIoWriteError::InvalidAddress;
    case Kernel::Device::DeviceRegionError::NotReady:
    case Kernel::Device::DeviceRegionError::Quiesced:
        return Hal::Device::MemoryMappedIoWriteError::NotReady;
    case Kernel::Device::DeviceRegionError::UnsupportedAccess:
        return Hal::Device::MemoryMappedIoWriteError::AccessDenied;
    case Kernel::Device::DeviceRegionError::InvalidConfiguration:
    case Kernel::Device::DeviceRegionError::InvalidDescriptor:
    case Kernel::Device::DeviceRegionError::AlreadyInitialized:
    case Kernel::Device::DeviceRegionError::ResourceDomainPinRejected:
    case Kernel::Device::DeviceRegionError::ResourceDomainInstanceMismatch:
    case Kernel::Device::DeviceRegionError::ResourceDomainRejected:
    case Kernel::Device::DeviceRegionError::WriteValueDenied:
    case Kernel::Device::DeviceRegionError::InvalidOperation:
    case Kernel::Device::DeviceRegionError::MappingNotSupported:
    case Kernel::Device::DeviceRegionError::MappingAccessDenied:
    case Kernel::Device::DeviceRegionError::MappingCacheMismatch:
    case Kernel::Device::DeviceRegionError::ReferenceExhausted:
    case Kernel::Device::DeviceRegionError::AccessReferenceUnderflow:
    case Kernel::Device::DeviceRegionError::MappingReferenceUnderflow:
    case Kernel::Device::DeviceRegionError::InUse:
    case Kernel::Device::DeviceRegionError::InvariantViolation:
        return Hal::Device::MemoryMappedIoWriteError::HardwareFailure;
    }
    return Hal::Device::MemoryMappedIoWriteError::HardwareFailure;
}
} // namespace

bool PhysicalMemoryMappedIo::TryGetEnd(const Foundation::UInt64 start, const Foundation::UInt64 length,
                                       Foundation::UInt64& end) noexcept
{
    if ((length == 0U) || (start > (MaximumUInt64 - length)))
    {
        return false;
    }
    end = start + length;
    return true;
}

PhysicalMemoryMappedIoInitializationError
PhysicalMemoryMappedIo::Initialize(const PhysicalMemoryMappedIoRegionView regions,
                                   const Foundation::UInt8 physicalAddressBits) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state != PhysicalMemoryMappedIoState::Uninitialized)
    {
        return PhysicalMemoryMappedIoInitializationError::AlreadyInitialized;
    }
    if ((regions.Data == nullptr) || (regions.Count == 0U) || (regions.Count > MaximumRegions))
    {
        return PhysicalMemoryMappedIoInitializationError::InvalidRegionView;
    }
    if ((physicalAddressBits < 32U) || (physicalAddressBits > m_access.GetMaximumPhysicalAddressBits()))
    {
        return PhysicalMemoryMappedIoInitializationError::InvalidPhysicalAddressWidth;
    }

    Kernel::Device::DeviceRegion* candidate[MaximumRegions]{};
    Foundation::UInt64 starts[MaximumRegions]{};
    Foundation::UInt64 ends[MaximumRegions]{};
    for (Foundation::Size index{0U}; index < regions.Count; ++index)
    {
        Kernel::Device::DeviceRegion* const region = regions.Data[index];
        if (region == nullptr)
        {
            return PhysicalMemoryMappedIoInitializationError::InvalidRegion;
        }
        const Kernel::Device::DeviceRegionSnapshot snapshot = region->GetSnapshot();
        const Kernel::Device::DeviceRegionDescriptor& descriptor = snapshot.Descriptor;
        if ((snapshot.State != Kernel::Device::DeviceRegionState::Ready) ||
            (descriptor.Kind != Kernel::Device::DeviceRegionKind::MemoryMappedIo))
        {
            return PhysicalMemoryMappedIoInitializationError::InvalidRegion;
        }
        if (!descriptor.Permissions.Contains(Kernel::Device::DeviceRegionPermission::Read) ||
            !descriptor.Widths.Contains(Kernel::Device::DeviceAccessWidth::DoubleWord) ||
            (descriptor.CachePolicy != Kernel::Memory::MemoryCachePolicy::Uncached))
        {
            return PhysicalMemoryMappedIoInitializationError::UnsupportedRegionPolicy;
        }
        if (!TryGetEnd(descriptor.BaseAddress, descriptor.LengthBytes, ends[index]))
        {
            return PhysicalMemoryMappedIoInitializationError::InvalidRegion;
        }
        if (!m_access.IsPhysicalRangeImplemented(descriptor.BaseAddress, descriptor.LengthBytes, physicalAddressBits))
        {
            return PhysicalMemoryMappedIoInitializationError::PhysicalAddressOutOfRange;
        }
        starts[index] = descriptor.BaseAddress;
        candidate[index] = region;
    }

    for (Foundation::Size left{0U}; left < regions.Count; ++left)
    {
        for (Foundation::Size right{left + 1U}; right < regions.Count; ++right)
        {
            if ((candidate[left] == candidate[right]) ||
                RangesOverlap(starts[left], ends[left], starts[right], ends[right]))
            {
                return PhysicalMemoryMappedIoInitializationError::OverlappingRegion;
            }
        }
    }

    for (Foundation::Size index{0U}; index < regions.Count; ++index)
    {
        m_regions[index] = candidate[index];
    }
    m_regionCount = regions.Count;
    m_physicalAddressBits = physicalAddressBits;
    m_state = PhysicalMemoryMappedIoState::Ready;
    return PhysicalMemoryMappedIoInitializationError::None;
}

Hal::Device::MemoryMappedIoReadError PhysicalMemoryMappedIo::Read(const Foundation::UInt64 physicalAddress,
                                                                  const Hal::Device::MemoryMappedIoWidth width,
                                                                  Foundation::UInt64& output) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state != PhysicalMemoryMappedIoState::Ready)
    {
        return Hal::Device::MemoryMappedIoReadError::NotReady;
    }
    if (width != Hal::Device::MemoryMappedIoWidth::DoubleWord)
    {
        return Hal::Device::MemoryMappedIoReadError::UnsupportedWidth;
    }
    if ((physicalAddress & (DoubleWordSizeBytes - 1U)) != 0U)
    {
        return Hal::Device::MemoryMappedIoReadError::InvalidAddress;
    }
    Foundation::UInt64 accessEnd{0U};
    if (!TryGetEnd(physicalAddress, DoubleWordSizeBytes, accessEnd) ||
        !m_access.IsPhysicalRangeImplemented(physicalAddress, DoubleWordSizeBytes, m_physicalAddressBits))
    {
        return Hal::Device::MemoryMappedIoReadError::InvalidAddress;
    }

    Kernel::Device::DeviceRegion* selected{nullptr};
    Foundation::UInt64 selectedBase{0U};
    for (Foundation::Size index{0U}; index < m_regionCount; ++index)
    {
        Kernel::Device::DeviceRegion* const candidate = m_regions[index];
        const Kernel::Device::DeviceRegionSnapshot snapshot = candidate->GetSnapshot();
        Foundation::UInt64 regionEnd{0U};
        if ((snapshot.State != Kernel::Device::DeviceRegionState::Ready) ||
            !TryGetEnd(snapshot.Descriptor.BaseAddress, snapshot.Descriptor.LengthBytes, regionEnd))
        {
            return Hal::Device::MemoryMappedIoReadError::NotReady;
        }
        if ((physicalAddress >= snapshot.Descriptor.BaseAddress) && (accessEnd <= regionEnd))
        {
            selected = candidate;
            selectedBase = snapshot.Descriptor.BaseAddress;
            break;
        }
    }
    if (selected == nullptr)
    {
        return Hal::Device::MemoryMappedIoReadError::InvalidAddress;
    }

    const Kernel::Device::DeviceRegionError retained =
        selected->RetainRegisterAccess(physicalAddress - selectedBase, Kernel::Device::DeviceAccessWidth::DoubleWord,
                                       Kernel::Device::DeviceAccessOperation::Read, 0U);
    if (retained != Kernel::Device::DeviceRegionError::None)
    {
        return MapRetainError(retained);
    }

    const Foundation::UInt64 value = m_access.ReadDoubleWord(physicalAddress);
    const Kernel::Device::DeviceRegionError released = selected->ReleaseRegisterAccess();
    if (released != Kernel::Device::DeviceRegionError::None)
    {
        return Hal::Device::MemoryMappedIoReadError::HardwareFailure;
    }
    output = value;
    return Hal::Device::MemoryMappedIoReadError::None;
}

Hal::Device::MemoryMappedIoWriteError PhysicalMemoryMappedIo::Write(const Foundation::UInt64 physicalAddress,
                                                                    const Hal::Device::MemoryMappedIoWidth width,
                                                                    const Foundation::UInt64 value) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state != PhysicalMemoryMappedIoState::Ready)
    {
        return Hal::Device::MemoryMappedIoWriteError::NotReady;
    }
    if (width != Hal::Device::MemoryMappedIoWidth::DoubleWord)
    {
        return Hal::Device::MemoryMappedIoWriteError::UnsupportedWidth;
    }
    if (value > MaximumUInt32)
    {
        return Hal::Device::MemoryMappedIoWriteError::ValueOutOfRange;
    }
    if ((physicalAddress & (DoubleWordSizeBytes - 1U)) != 0U)
    {
        return Hal::Device::MemoryMappedIoWriteError::InvalidAddress;
    }
    Foundation::UInt64 accessEnd{0U};
    if (!TryGetEnd(physicalAddress, DoubleWordSizeBytes, accessEnd) ||
        !m_access.IsPhysicalRangeImplemented(physicalAddress, DoubleWordSizeBytes, m_physicalAddressBits))
    {
        return Hal::Device::MemoryMappedIoWriteError::InvalidAddress;
    }

    Kernel::Device::DeviceRegion* selected{nullptr};
    Foundation::UInt64 selectedBase{0U};
    for (Foundation::Size index{0U}; index < m_regionCount; ++index)
    {
        Kernel::Device::DeviceRegion* const candidate = m_regions[index];
        const Kernel::Device::DeviceRegionSnapshot snapshot = candidate->GetSnapshot();
        Foundation::UInt64 regionEnd{0U};
        if ((snapshot.State != Kernel::Device::DeviceRegionState::Ready) ||
            !TryGetEnd(snapshot.Descriptor.BaseAddress, snapshot.Descriptor.LengthBytes, regionEnd))
        {
            return Hal::Device::MemoryMappedIoWriteError::NotReady;
        }
        if ((physicalAddress >= snapshot.Descriptor.BaseAddress) && (accessEnd <= regionEnd))
        {
            selected = candidate;
            selectedBase = snapshot.Descriptor.BaseAddress;
            break;
        }
    }
    if (selected == nullptr)
    {
        return Hal::Device::MemoryMappedIoWriteError::InvalidAddress;
    }

    const Kernel::Device::DeviceRegionError retained = selected->RetainRegisterAccess(
        physicalAddress - selectedBase, Kernel::Device::DeviceAccessWidth::DoubleWord,
        Kernel::Device::DeviceAccessOperation::Write, static_cast<Foundation::UInt32>(value));
    if (retained != Kernel::Device::DeviceRegionError::None)
    {
        return MapWriteRetainError(retained);
    }

    m_access.WriteDoubleWord(physicalAddress, static_cast<Foundation::UInt32>(value));
    const Kernel::Device::DeviceRegionError released = selected->ReleaseRegisterAccess();
    if (released != Kernel::Device::DeviceRegionError::None)
    {
        return Hal::Device::MemoryMappedIoWriteError::HardwareFailure;
    }
    return Hal::Device::MemoryMappedIoWriteError::None;
}

PhysicalMemoryMappedIoSnapshot PhysicalMemoryMappedIo::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return PhysicalMemoryMappedIoSnapshot{
        .RegionCount = m_regionCount,
        .PhysicalAddressBits = m_physicalAddressBits,
        .State = m_state,
        .Reserved = {},
    };
}
} // namespace Asteria::Platform::Device
