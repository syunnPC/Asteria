// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_DEVICE_PHYSICAL_MEMORY_MAPPED_IO_HPP_INCLUDED
#define ASTERIA_PLATFORM_DEVICE_PHYSICAL_MEMORY_MAPPED_IO_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Hal/Device/IMemoryMappedIo.hpp"
#include "Hal/Device/IMemoryMappedIoWriter.hpp"
#include "Kernel/Device/DeviceRegion.hpp"
#include "Platform/Device/IPhysicalMemoryAccess.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Platform::Device
{
struct PhysicalMemoryMappedIoRegionView
{
    Kernel::Device::DeviceRegion* const* Data;
    Foundation::Size Count;
};

enum class PhysicalMemoryMappedIoState : Foundation::UInt8
{
    Uninitialized,
    Ready,
};

enum class PhysicalMemoryMappedIoInitializationError : Foundation::UInt8
{
    None,
    AlreadyInitialized,
    InvalidRegionView,
    InvalidPhysicalAddressWidth,
    InvalidRegion,
    UnsupportedRegionPolicy,
    PhysicalAddressOutOfRange,
    OverlappingRegion,
};

struct PhysicalMemoryMappedIoSnapshot
{
    Foundation::Size RegionCount;
    Foundation::UInt8 PhysicalAddressBits;
    PhysicalMemoryMappedIoState State;
    Foundation::UInt8 Reserved[6];
};

/// Capability-checked uncached physical MMIO reader and writer.
///
/// The backend retains boot-lifetime DeviceRegion storage supplied by its
/// trusted platform owner. Each operation holds an exact direct-access
/// reference to the DeviceRegion that contains the whole DWORD across the
/// ISA access, which performs the physical transaction; the access states
/// its own native preconditions. The access must outlive this backend.
class PhysicalMemoryMappedIo final : public Hal::Device::IMemoryMappedIo, public Hal::Device::IMemoryMappedIoWriter
{
public:
    static constexpr Foundation::Size MaximumRegions{8U};

    explicit constexpr PhysicalMemoryMappedIo(const IPhysicalMemoryAccess& access) noexcept : m_access{access}
    {
    }
    ~PhysicalMemoryMappedIo() noexcept = default;

    PhysicalMemoryMappedIo(const PhysicalMemoryMappedIo&) = delete;
    PhysicalMemoryMappedIo& operator=(const PhysicalMemoryMappedIo&) = delete;
    PhysicalMemoryMappedIo(PhysicalMemoryMappedIo&&) = delete;
    PhysicalMemoryMappedIo& operator=(PhysicalMemoryMappedIo&&) = delete;

    [[nodiscard]] PhysicalMemoryMappedIoInitializationError Initialize(PhysicalMemoryMappedIoRegionView regions,
                                                                       Foundation::UInt8 physicalAddressBits) noexcept;

    [[nodiscard]] Hal::Device::MemoryMappedIoReadError Read(Foundation::UInt64 physicalAddress,
                                                            Hal::Device::MemoryMappedIoWidth width,
                                                            Foundation::UInt64& output) noexcept override;

    [[nodiscard]] Hal::Device::MemoryMappedIoWriteError Write(Foundation::UInt64 physicalAddress,
                                                              Hal::Device::MemoryMappedIoWidth width,
                                                              Foundation::UInt64 value) noexcept override;

    [[nodiscard]] PhysicalMemoryMappedIoSnapshot GetSnapshot() const noexcept;

private:
    [[nodiscard]] static bool TryGetEnd(Foundation::UInt64 start, Foundation::UInt64 length,
                                        Foundation::UInt64& end) noexcept;
    const IPhysicalMemoryAccess& m_access;
    mutable Platform::Synchronization::SpinLock m_synchronization;
    Kernel::Device::DeviceRegion* m_regions[MaximumRegions]{};
    Foundation::Size m_regionCount{0U};
    Foundation::UInt8 m_physicalAddressBits{0U};
    PhysicalMemoryMappedIoState m_state{PhysicalMemoryMappedIoState::Uninitialized};
};

static_assert(sizeof(PhysicalMemoryMappedIoRegionView) == 16U);
static_assert(alignof(PhysicalMemoryMappedIoRegionView) == 8U);
static_assert(sizeof(PhysicalMemoryMappedIoState) == 1U);
static_assert(sizeof(PhysicalMemoryMappedIoInitializationError) == 1U);
static_assert(sizeof(PhysicalMemoryMappedIoSnapshot) == 16U);
static_assert(alignof(PhysicalMemoryMappedIoSnapshot) == 8U);
} // namespace Asteria::Platform::Device

#endif // ASTERIA_PLATFORM_DEVICE_PHYSICAL_MEMORY_MAPPED_IO_HPP_INCLUDED
