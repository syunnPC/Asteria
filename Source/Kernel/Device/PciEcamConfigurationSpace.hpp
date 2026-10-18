// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_DEVICE_PCI_ECAM_CONFIGURATION_SPACE_HPP_INCLUDED
#define ASTERIA_KERNEL_DEVICE_PCI_ECAM_CONFIGURATION_SPACE_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Hal/Device/IMemoryMappedIo.hpp"
#include "Hal/Device/IMemoryMappedIoWriter.hpp"
#include "Hal/Device/IPciConfigurationSpace.hpp"
#include "Hal/Device/IPciConfigurationWriter.hpp"
#include "Kernel/Device/PciEcamAddress.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Device
{
struct PciEcamRegionView
{
    const PciEcamRegion* Data;
    Foundation::Size Count;
};

enum class PciEcamConfigurationSpaceState : Foundation::UInt8
{
    Uninitialized,
    Unavailable,
    Ready,
};

enum class PciEcamConfigurationSpaceInitializationError : Foundation::UInt8
{
    None,
    AlreadyInitialized,
    InvalidRegionView,
    InvalidRegion,
    OverlappingBusRange,
    OverlappingPhysicalRange,
};

struct PciEcamConfigurationSpaceSnapshot
{
    Foundation::Size RegionCount;
    PciEcamConfigurationSpaceState State;
    Foundation::UInt8 Reserved[7];
};

/// Checked target-neutral PCI Express ECAM configuration reader and, given an MMIO writer, writer
/// (the run-time configuration writes of Documentation/Architecture/IommuDomain.md on a platform
/// without configuration ports). Initialize copies at most eight normalized regions. An empty view
/// is a successful unavailable configuration for platforms without MCFG/ECAM. Read32 and Write32
/// select the exact segment/bus window and delegate one ordered DWORD access to the injected MMIO
/// backend; Read32 publishes output only on success, and Write32 without a writer is Unsupported.
class PciEcamConfigurationSpace final : public Hal::Device::IPciConfigurationSpace,
                                        public Hal::Device::IPciConfigurationWriter
{
public:
    static constexpr Foundation::Size MaximumRegions{8U};

    explicit constexpr PciEcamConfigurationSpace(Hal::Device::IMemoryMappedIo& memoryMappedIo) noexcept
        : m_memoryMappedIo{memoryMappedIo}
    {
    }

    constexpr PciEcamConfigurationSpace(Hal::Device::IMemoryMappedIo& memoryMappedIo,
                                        Hal::Device::IMemoryMappedIoWriter& writer) noexcept
        : m_memoryMappedIo{memoryMappedIo}, m_writer{&writer}
    {
    }

    ~PciEcamConfigurationSpace() noexcept = default;

    PciEcamConfigurationSpace(const PciEcamConfigurationSpace&) = delete;
    PciEcamConfigurationSpace& operator=(const PciEcamConfigurationSpace&) = delete;
    PciEcamConfigurationSpace(PciEcamConfigurationSpace&&) = delete;
    PciEcamConfigurationSpace& operator=(PciEcamConfigurationSpace&&) = delete;

    [[nodiscard]] PciEcamConfigurationSpaceInitializationError Initialize(PciEcamRegionView regions) noexcept;

    [[nodiscard]] Hal::Device::PciConfigurationReadError
    Read32(PciFunctionAddress address, Foundation::UInt16 registerOffset, Foundation::UInt32& output) noexcept override;

    [[nodiscard]] Hal::Device::PciConfigurationWriteError
    Write32(PciFunctionAddress address, Foundation::UInt16 registerOffset, Foundation::UInt32 value) noexcept override;

    [[nodiscard]] PciEcamConfigurationSpaceSnapshot GetSnapshot() const noexcept;

    /// Boot-only immutable window inventory, once Initialize returned.
    [[nodiscard]] PciEcamRegionView GetRegions() const noexcept
    {
        return {m_regions, m_regionCount};
    }

private:
    /// Why a configuration access cannot be made; Ready gives its physical
    /// address.
    enum class Selection : Foundation::UInt8
    {
        Ready,
        NotReady,
        InvalidAddress,
        Unsupported,
        HardwareFailure,
    };

    [[nodiscard]] Selection SelectLocked(PciFunctionAddress address, Foundation::UInt16 registerOffset,
                                         Foundation::UInt64& physicalAddress) const noexcept;
    [[nodiscard]] static bool RegionPrecedes(const PciEcamRegion& left, const PciEcamRegion& right) noexcept;
    [[nodiscard]] static bool BusRangesOverlap(const PciEcamRegion& left, const PciEcamRegion& right) noexcept;
    [[nodiscard]] static bool TryGetPhysicalEnd(const PciEcamRegion& region, Foundation::UInt64& end) noexcept;
    [[nodiscard]] static bool RegionsOverlap(const PciEcamRegion& left, const PciEcamRegion& right) noexcept;
    [[nodiscard]] static bool PaddingIsZero(const PciEcamRegion& region) noexcept;

    Hal::Device::IMemoryMappedIo& m_memoryMappedIo;
    Hal::Device::IMemoryMappedIoWriter* m_writer{nullptr};
    mutable Platform::Synchronization::SpinLock m_synchronization;
    PciEcamRegion m_regions[MaximumRegions]{};
    Foundation::Size m_regionCount{0U};
    PciEcamConfigurationSpaceState m_state{PciEcamConfigurationSpaceState::Uninitialized};
};

static_assert(sizeof(PciEcamRegionView) == 16U);
static_assert(alignof(PciEcamRegionView) == 8U);
static_assert(sizeof(PciEcamConfigurationSpaceState) == 1U);
static_assert(sizeof(PciEcamConfigurationSpaceInitializationError) == 1U);
static_assert(sizeof(PciEcamConfigurationSpaceSnapshot) == 16U);
static_assert(alignof(PciEcamConfigurationSpaceSnapshot) == 8U);
} // Namespace Asteria::Kernel::Device

#endif // ASTERIA_KERNEL_DEVICE_PCI_ECAM_CONFIGURATION_SPACE_HPP_INCLUDED
