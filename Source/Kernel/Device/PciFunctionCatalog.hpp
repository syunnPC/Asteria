// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_DEVICE_PCI_FUNCTION_CATALOG_HPP_INCLUDED
#define ASTERIA_KERNEL_DEVICE_PCI_FUNCTION_CATALOG_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Hal/Device/IPciConfigurationSpace.hpp"
#include "Kernel/Device/PciTypes.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Device
{
enum class PciFunctionCatalogState : Foundation::UInt8
{
    Empty,
    Ready,
};

enum class PciFunctionCatalogError : Foundation::UInt8
{
    None,
    InvalidRangeView,
    InvalidBusRange,
    OverlappingBusRange,
    FunctionCapacityExceeded,
    ConfigurationReadFailed,
    AlreadyDiscovered,
    NotReady,
    InvalidAddress,
    FunctionNotFound,
    IndexOutOfRange,
};

struct PciFunctionCatalogFailure
{
    PciFunctionAddress Address;
    Foundation::UInt16 RegisterOffset;
    Hal::Device::PciConfigurationReadError Configuration;
    PciFunctionCatalogError Error;
    Foundation::UInt8 Reserved[6];
};

struct PciFunctionCatalogSnapshot
{
    Foundation::Size FunctionCount;
    Foundation::Size BusCount;
    Foundation::Size RangeCount;
    Foundation::UInt64 ConfigurationReadCount;
    PciFunctionCatalogState State;
    Foundation::UInt8 Reserved[7];
};

/// Discovers the common header of every PCI function in validated bus ranges.
///
/// The platform supplies authoritative segment/bus ranges and a normalized
/// configuration-cycle implementation. Discovery never writes configuration
/// space, sizes BARs, allocates resources, or changes bus numbering. Function
/// zero is probed first for each device; functions 1-7 are read only when its
/// multifunction bit is set. Results are published in ascending BDF order.
/// Every failure clears partial records so the same instance can be retried.
/// Operations may spin and are forbidden from ISRs and same-CPU reentry.
class PciFunctionCatalog final
{
public:
    static constexpr Foundation::Size MaximumRanges{8U};
    static constexpr Foundation::Size MaximumBuses{MaximumRanges * 256U};
    static constexpr Foundation::Size MaximumFunctions{256U};

    explicit constexpr PciFunctionCatalog(Hal::Device::IPciConfigurationSpace& configuration) noexcept
        : m_configuration{configuration}
    {
    }

    /// Preconditions:
    /// - configuration outlives this object and every operation;
    /// - no concurrent operation remains.
    ~PciFunctionCatalog() noexcept = default;

    PciFunctionCatalog(const PciFunctionCatalog&) = delete;
    PciFunctionCatalog& operator=(const PciFunctionCatalog&) = delete;
    PciFunctionCatalog(PciFunctionCatalog&&) = delete;
    PciFunctionCatalog& operator=(PciFunctionCatalog&&) = delete;

    /// Performs one read-only, bounded discovery transaction. Ranges may be in
    /// any order but must be nonoverlapping within each segment. On failure the
    /// catalog remains Empty and contains no externally observable record.
    /// The caller keeps the range storage and PCI topology stable until this
    /// call returns; hotplug and concurrent configuration changes are outside
    /// this bootstrap catalog's contract.
    [[nodiscard]] PciFunctionCatalogFailure Discover(PciBusRangeView ranges) noexcept;

    [[nodiscard]] Foundation::Result<PciFunctionDescriptor, PciFunctionCatalogFailure>
    GetFunction(Foundation::Size index) const noexcept;

    [[nodiscard]] Foundation::Result<PciFunctionDescriptor, PciFunctionCatalogFailure>
    FindFunction(PciFunctionAddress address) const noexcept;

    [[nodiscard]] PciFunctionCatalogSnapshot GetSnapshot() const noexcept;

private:
    [[nodiscard]] static PciFunctionCatalogFailure MakeFailure(
        PciFunctionCatalogError error, PciFunctionAddress address = {}, Foundation::UInt16 registerOffset = 0U,
        Hal::Device::PciConfigurationReadError configuration = Hal::Device::PciConfigurationReadError::None) noexcept;
    [[nodiscard]] static bool RangeReservedIsZero(const PciBusRange& range) noexcept;
    [[nodiscard]] static bool RangesOverlap(const PciBusRange& left, const PciBusRange& right) noexcept;
    [[nodiscard]] static bool RangePrecedes(const PciBusRange& left, const PciBusRange& right) noexcept;
    [[nodiscard]] static PciFunctionCatalogFailure ValidateAndSortRanges(PciBusRangeView input,
                                                                         PciBusRange (&sorted)[MaximumRanges],
                                                                         Foundation::Size& busCount) noexcept;
    [[nodiscard]] PciFunctionCatalogFailure Read32Locked(PciFunctionAddress address, Foundation::UInt16 registerOffset,
                                                         Foundation::UInt32& output) noexcept;
    [[nodiscard]] PciFunctionCatalogFailure
    AppendFunctionLocked(PciFunctionAddress address, Foundation::UInt32 identifier, bool& multifunction) noexcept;
    void ResetStorageLocked() noexcept;

    Hal::Device::IPciConfigurationSpace& m_configuration;
    mutable Platform::Synchronization::SpinLock m_synchronization;
    PciFunctionDescriptor m_functions[MaximumFunctions]{};
    Foundation::Size m_functionCount{0U};
    Foundation::Size m_busCount{0U};
    Foundation::Size m_rangeCount{0U};
    Foundation::UInt64 m_configurationReadCount{0U};
    PciFunctionCatalogState m_state{PciFunctionCatalogState::Empty};
};

static_assert(sizeof(PciFunctionCatalogState) == 1U);
static_assert(sizeof(PciFunctionCatalogError) == 1U);
static_assert(sizeof(PciFunctionCatalogFailure) == 16U);
static_assert(alignof(PciFunctionCatalogFailure) == 2U);
static_assert(sizeof(PciFunctionCatalogSnapshot) == 40U);
static_assert(alignof(PciFunctionCatalogSnapshot) == 8U);
} // namespace Asteria::Kernel::Device

#endif // ASTERIA_KERNEL_DEVICE_PCI_FUNCTION_CATALOG_HPP_INCLUDED
