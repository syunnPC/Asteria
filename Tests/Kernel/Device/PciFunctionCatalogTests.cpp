// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <thread>

#include "Kernel/Device/PciFunctionCatalog.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt16;
using Foundation::UInt32;
using Hal::Device::IPciConfigurationSpace;
using Hal::Device::PciConfigurationReadError;
using Kernel::Device::PciBusRange;
using Kernel::Device::PciBusRangeView;
using Kernel::Device::PciFunctionAddress;
using Kernel::Device::PciFunctionCatalog;
using Kernel::Device::PciFunctionCatalogError;
using Kernel::Device::PciFunctionCatalogState;
using Kernel::Device::PciFunctionDescriptor;
using Platform::Synchronization::SpinLock;
using Platform::Synchronization::SpinLockGuard;

class FakePciConfigurationSpace final : public IPciConfigurationSpace
{
public:
    static constexpr Size MaximumEntries{PciFunctionCatalog::MaximumFunctions + 1U};

    [[nodiscard]] bool AddFunction(const PciFunctionAddress address, const UInt16 vendorId, const UInt16 deviceId,
                                   const UInt8 revisionId, const UInt8 programmingInterface, const UInt8 subclass,
                                   const UInt8 classCode, const UInt8 headerLayout, const bool multifunction) noexcept
    {
        SpinLockGuard guard{m_synchronization};
        if (!address.IsValid() || (vendorId == 0xFFFFU) || ((headerLayout & 0x80U) != 0U) ||
            (m_entryCount >= MaximumEntries))
        {
            return false;
        }
        for (Size index{0U}; index < m_entryCount; ++index)
        {
            if (m_entries[index].Address.IsEqualTo(address))
            {
                return false;
            }
        }
        m_entries[m_entryCount] = Entry{
            .Address = address,
            .Identifier = static_cast<UInt32>(vendorId) | (static_cast<UInt32>(deviceId) << 16U),
            .Class = static_cast<UInt32>(revisionId) | (static_cast<UInt32>(programmingInterface) << 8U) |
                     (static_cast<UInt32>(subclass) << 16U) | (static_cast<UInt32>(classCode) << 24U),
            .Header = static_cast<UInt32>(headerLayout | (multifunction ? 0x80U : 0U)) << 16U,
        };
        ++m_entryCount;
        return true;
    }

    void FailNextRead(const PciFunctionAddress address, const UInt16 registerOffset,
                      const PciConfigurationReadError error) noexcept
    {
        SpinLockGuard guard{m_synchronization};
        m_failureAddress = address;
        m_failureOffset = registerOffset;
        m_failureError = error;
        m_failureArmed = true;
    }

    void PauseNextRead() noexcept
    {
        m_resumePausedRead.store(false, std::memory_order_relaxed);
        m_readPaused.store(false, std::memory_order_relaxed);
        m_pauseNextRead.store(true, std::memory_order_release);
    }

    void WaitUntilReadPaused() const noexcept
    {
        while (!m_readPaused.load(std::memory_order_acquire))
        {
            m_readPaused.wait(false, std::memory_order_acquire);
        }
    }

    void ResumePausedRead() noexcept
    {
        m_resumePausedRead.store(true, std::memory_order_release);
        m_resumePausedRead.notify_all();
    }

    [[nodiscard]] PciConfigurationReadError Read32(const PciFunctionAddress address, const UInt16 registerOffset,
                                                   UInt32& output) noexcept override
    {
        if (m_pauseNextRead.exchange(false, std::memory_order_acq_rel))
        {
            m_readPaused.store(true, std::memory_order_release);
            m_readPaused.notify_all();
            while (!m_resumePausedRead.load(std::memory_order_acquire))
            {
                m_resumePausedRead.wait(false, std::memory_order_acquire);
            }
        }

        SpinLockGuard guard{m_synchronization};
        ++m_readCount;
        if (!address.IsValid() || ((registerOffset & 3U) != 0U) || (registerOffset > 0x0FFCU))
        {
            return PciConfigurationReadError::InvalidAddress;
        }
        if (m_failureArmed && m_failureAddress.IsEqualTo(address) && (m_failureOffset == registerOffset))
        {
            m_failureArmed = false;
            return m_failureError;
        }

        const Entry* entry{nullptr};
        for (Size index{0U}; index < m_entryCount; ++index)
        {
            if (m_entries[index].Address.IsEqualTo(address))
            {
                entry = &m_entries[index];
                break;
            }
        }
        if (entry == nullptr)
        {
            output = 0xFFFF'FFFFU;
            return PciConfigurationReadError::None;
        }
        switch (registerOffset)
        {
        case 0x00U:
            output = entry->Identifier;
            return PciConfigurationReadError::None;
        case 0x08U:
            output = entry->Class;
            return PciConfigurationReadError::None;
        case 0x0CU:
            output = entry->Header;
            return PciConfigurationReadError::None;
        default:
            output = 0U;
            return PciConfigurationReadError::None;
        }
    }

    [[nodiscard]] Size GetReadCount() const noexcept
    {
        SpinLockGuard guard{m_synchronization};
        return m_readCount;
    }

private:
    struct Entry
    {
        PciFunctionAddress Address{};
        UInt32 Identifier{0U};
        UInt32 Class{0U};
        UInt32 Header{0U};
    };

    mutable SpinLock m_synchronization;
    Entry m_entries[MaximumEntries]{};
    Size m_entryCount{0U};
    Size m_readCount{0U};
    PciFunctionAddress m_failureAddress{};
    UInt16 m_failureOffset{0U};
    PciConfigurationReadError m_failureError{PciConfigurationReadError::None};
    bool m_failureArmed{false};
    std::atomic<bool> m_pauseNextRead{false};
    std::atomic<bool> m_readPaused{false};
    std::atomic<bool> m_resumePausedRead{false};
};

[[nodiscard]] constexpr PciBusRange MakeRange(const UInt16 segment, const UInt8 firstBus, const UInt8 lastBus) noexcept
{
    return PciBusRange{
        .Segment = segment,
        .FirstBus = firstBus,
        .LastBus = lastBus,
        .Reserved = {},
    };
}

[[nodiscard]] bool DescriptorMatches(const PciFunctionDescriptor& descriptor, const PciFunctionAddress address,
                                     const UInt16 vendorId, const UInt16 deviceId, const UInt8 revisionId,
                                     const UInt8 programmingInterface, const UInt8 subclass, const UInt8 classCode,
                                     const UInt8 headerLayout, const bool multifunction) noexcept
{
    return descriptor.Address.IsEqualTo(address) && (descriptor.VendorId == vendorId) &&
           (descriptor.DeviceId == deviceId) && (descriptor.RevisionId == revisionId) &&
           (descriptor.ProgrammingInterface == programmingInterface) && (descriptor.Subclass == subclass) &&
           (descriptor.ClassCode == classCode) && (descriptor.HeaderLayout == headerLayout) &&
           (descriptor.Multifunction == multifunction);
}

[[nodiscard]] bool AddDenseBus(FakePciConfigurationSpace& configuration, const UInt16 segment, const UInt8 bus) noexcept
{
    Size index{0U};
    for (UInt8 device{0U}; device <= PciFunctionAddress::MaximumDevice; ++device)
    {
        for (UInt8 function{0U}; function <= PciFunctionAddress::MaximumFunction; ++function)
        {
            const UInt16 vendorId = static_cast<UInt16>(0x1000U + index);
            const UInt16 deviceId = static_cast<UInt16>(0x2000U + index);
            if (!configuration.AddFunction(PciFunctionAddress{segment, bus, device, function}, vendorId, deviceId,
                                           static_cast<UInt8>(index & 0xFFU), function, device, 0x02U, 0U,
                                           function == 0U))
            {
                return false;
            }
            ++index;
        }
    }
    return index == PciFunctionCatalog::MaximumFunctions;
}

void TestRangeValidation(TestContext& runner) noexcept
{
    FakePciConfigurationSpace configuration;
    PciFunctionCatalog catalog{configuration};
    const auto before = catalog.GetSnapshot();
    const auto notReady = catalog.GetFunction(0U);
    const auto invalidAddress = catalog.FindFunction(PciFunctionAddress{});
    const auto validBeforeReady = catalog.FindFunction(PciFunctionAddress{0U, 0U, 0U, 0U});
    ASTERIA_EXPECT(runner, "empty catalog rejects queries with deterministic precedence",
                   before.State == PciFunctionCatalogState::Empty, before.FunctionCount == 0U, !notReady.HasValue(),
                   notReady.GetError().Error == PciFunctionCatalogError::NotReady, !invalidAddress.HasValue(),
                   invalidAddress.GetError().Error == PciFunctionCatalogError::InvalidAddress,
                   !validBeforeReady.HasValue(),
                   validBeforeReady.GetError().Error == PciFunctionCatalogError::NotReady);

    PciBusRange ranges[PciFunctionCatalog::MaximumRanges + 1U]{};
    for (Size index{0U}; index < PciFunctionCatalog::MaximumRanges + 1U; ++index)
    {
        ranges[index] = MakeRange(static_cast<UInt16>(index), 0U, 0U);
    }
    const auto nullView = catalog.Discover(PciBusRangeView{
        .Data = nullptr,
        .Count = 1U,
    });
    const auto emptyView = catalog.Discover(PciBusRangeView{
        .Data = ranges,
        .Count = 0U,
    });
    const auto tooMany = catalog.Discover(PciBusRangeView{
        .Data = ranges,
        .Count = PciFunctionCatalog::MaximumRanges + 1U,
    });

    ranges[0U] = MakeRange(0U, 2U, 1U);
    const auto reversed = catalog.Discover(PciBusRangeView{
        .Data = ranges,
        .Count = 1U,
    });
    ranges[0U] = MakeRange(0U, 0U, 0U);
    ranges[0U].Reserved[2U] = 1U;
    const auto reserved = catalog.Discover(PciBusRangeView{
        .Data = ranges,
        .Count = 1U,
    });

    ranges[0U] = MakeRange(3U, 0U, 8U);
    ranges[1U] = MakeRange(3U, 8U, 10U);
    const auto overlap = catalog.Discover(PciBusRangeView{
        .Data = ranges,
        .Count = 2U,
    });
    ASTERIA_EXPECT(runner, "invalid and overlapping bus ranges fail before configuration access",
                   nullView.Error == PciFunctionCatalogError::InvalidRangeView,
                   emptyView.Error == PciFunctionCatalogError::InvalidRangeView,
                   tooMany.Error == PciFunctionCatalogError::InvalidRangeView,
                   reversed.Error == PciFunctionCatalogError::InvalidBusRange,
                   reserved.Error == PciFunctionCatalogError::InvalidBusRange,
                   overlap.Error == PciFunctionCatalogError::OverlappingBusRange,
                   catalog.GetSnapshot().State == PciFunctionCatalogState::Empty, configuration.GetReadCount() == 0U);

    FakePciConfigurationSpace adjacentConfiguration;
    PciFunctionCatalog adjacentCatalog{adjacentConfiguration};
    const PciBusRange adjacentRanges[]{
        MakeRange(7U, 1U, 1U),
        MakeRange(7U, 2U, 2U),
    };
    const auto adjacent = adjacentCatalog.Discover(PciBusRangeView{
        .Data = adjacentRanges,
        .Count = 2U,
    });
    const auto adjacentSnapshot = adjacentCatalog.GetSnapshot();
    ASTERIA_EXPECT(runner, "adjacent ranges are accepted and an empty topology becomes ready",
                   adjacent.Error == PciFunctionCatalogError::None,
                   adjacentSnapshot.State == PciFunctionCatalogState::Ready, adjacentSnapshot.RangeCount == 2U,
                   adjacentSnapshot.BusCount == 2U, adjacentSnapshot.FunctionCount == 0U,
                   adjacentSnapshot.ConfigurationReadCount == 64U);
}

void TestRangeCapacityBoundaries(TestContext& runner) noexcept
{
    FakePciConfigurationSpace configuration;
    PciBusRange ranges[PciFunctionCatalog::MaximumRanges]{};
    for (Size index{0U}; index < PciFunctionCatalog::MaximumRanges; ++index)
    {
        ranges[index] = MakeRange(static_cast<UInt16>(index), 0U, 0xFFU);
    }

    PciFunctionCatalog catalog{configuration};
    const auto discovered = catalog.Discover(PciBusRangeView{
        .Data = ranges,
        .Count = PciFunctionCatalog::MaximumRanges,
    });
    const auto snapshot = catalog.GetSnapshot();
    ASTERIA_EXPECT(runner, "all buses in eight segments are accepted without an aggregate cap",
                   discovered.Error == PciFunctionCatalogError::None, snapshot.State == PciFunctionCatalogState::Ready,
                   snapshot.RangeCount == PciFunctionCatalog::MaximumRanges,
                   snapshot.BusCount == PciFunctionCatalog::MaximumBuses, snapshot.FunctionCount == 0U,
                   snapshot.ConfigurationReadCount == 65536U);
}

void TestTwoFullSegments(TestContext& runner) noexcept
{
    FakePciConfigurationSpace configuration;
    PciFunctionCatalog catalog{configuration};
    const PciBusRange ranges[]{
        MakeRange(3U, 0U, 0xFFU),
        MakeRange(9U, 0U, 0xFFU),
    };
    const auto discovered = catalog.Discover(PciBusRangeView{
        .Data = ranges,
        .Count = 2U,
    });
    const auto snapshot = catalog.GetSnapshot();
    ASTERIA_EXPECT(runner, "two complete segment groups do not share an aggregate bus cap",
                   discovered.Error == PciFunctionCatalogError::None, snapshot.BusCount == 512U,
                   snapshot.RangeCount == 2U, snapshot.ConfigurationReadCount == 16384U);
}

void TestSparseMultifunctionDiscovery(TestContext& runner) noexcept
{
    FakePciConfigurationSpace configuration;
    const bool added =
        configuration.AddFunction(PciFunctionAddress{1U, 9U, 1U, 0U}, 0x8086U, 0x100EU, 2U, 0U, 0U, 2U, 0U, false) &&
        configuration.AddFunction(PciFunctionAddress{2U, 3U, 0U, 0U}, 0x1234U, 0x5678U, 9U, 1U, 6U, 1U, 0U, false) &&
        configuration.AddFunction(PciFunctionAddress{2U, 3U, 0U, 1U}, 0xAAAAU, 0x0001U, 0U, 0U, 0U, 0xFFU, 0U, false) &&
        configuration.AddFunction(PciFunctionAddress{2U, 3U, 5U, 0U}, 0x1111U, 0x2222U, 3U, 0U, 4U, 6U, 0U, true) &&
        configuration.AddFunction(PciFunctionAddress{2U, 3U, 5U, 1U}, 0x1111U, 0x2223U, 4U, 0x30U, 3U, 0x0CU, 0U,
                                  false) &&
        configuration.AddFunction(PciFunctionAddress{2U, 3U, 5U, 7U}, 0x1111U, 0x2229U, 5U, 1U, 8U, 1U, 0x7FU, false) &&
        configuration.AddFunction(PciFunctionAddress{2U, 4U, 31U, 0U}, 0xABCDU, 0xEF01U, 6U, 0U, 4U, 6U, 1U, false) &&
        configuration.AddFunction(PciFunctionAddress{2U, 4U, 20U, 1U}, 0xDEADU, 0xBEEFU, 0U, 0U, 0U, 0xFFU, 0U, false);
    ASTERIA_EXPECT(runner, "sparse PCI fixture installs all synthetic functions", added);

    const PciBusRange ranges[]{
        MakeRange(2U, 4U, 4U),
        MakeRange(1U, 9U, 9U),
        MakeRange(2U, 3U, 3U),
    };
    PciFunctionCatalog catalog{configuration};
    const auto discovered = catalog.Discover(PciBusRangeView{
        .Data = ranges,
        .Count = 3U,
    });
    const auto snapshot = catalog.GetSnapshot();
    ASTERIA_EXPECT(runner, "unordered ranges publish a deterministic sparse multifunction catalog",
                   discovered.Error == PciFunctionCatalogError::None, snapshot.State == PciFunctionCatalogState::Ready,
                   snapshot.RangeCount == 3U, snapshot.BusCount == 3U, snapshot.FunctionCount == 6U,
                   snapshot.ConfigurationReadCount == 115U, configuration.GetReadCount() == 115U);

    const auto first = catalog.GetFunction(0U);
    const auto endpoint = catalog.GetFunction(1U);
    const auto multifunction = catalog.GetFunction(2U);
    const auto functionOne = catalog.GetFunction(3U);
    const auto functionSeven = catalog.GetFunction(4U);
    const auto bridge = catalog.GetFunction(5U);
    ASTERIA_EXPECT(runner, "common headers decode exactly in ascending segment/BDF order", first.HasValue(),
                   DescriptorMatches(first.GetValue(), PciFunctionAddress{1U, 9U, 1U, 0U}, 0x8086U, 0x100EU, 2U, 0U, 0U,
                                     2U, 0U, false),
                   endpoint.HasValue(), endpoint.GetValue().Address.IsEqualTo(PciFunctionAddress{2U, 3U, 0U, 0U}),
                   multifunction.HasValue(), multifunction.GetValue().Multifunction, functionOne.HasValue(),
                   functionOne.GetValue().ClassCode == 0x0CU, functionOne.GetValue().ProgrammingInterface == 0x30U,
                   functionSeven.HasValue(), functionSeven.GetValue().HeaderLayout == 0x7FU, bridge.HasValue(),
                   bridge.GetValue().HeaderLayout == 1U);

    const auto found = catalog.FindFunction(PciFunctionAddress{2U, 3U, 5U, 7U});
    const auto hiddenNonMultifunction = catalog.FindFunction(PciFunctionAddress{2U, 3U, 0U, 1U});
    const auto hiddenMissingFunctionZero = catalog.FindFunction(PciFunctionAddress{2U, 4U, 20U, 1U});
    const auto outOfRange = catalog.GetFunction(6U);
    const Size readsBeforeSecondDiscovery = configuration.GetReadCount();
    const auto secondDiscovery = catalog.Discover(PciBusRangeView{
        .Data = ranges,
        .Count = 3U,
    });
    ASTERIA_EXPECT(runner, "lookup and one-shot discovery preserve PCI function-zero semantics", found.HasValue(),
                   found.GetValue().DeviceId == 0x2229U, !hiddenNonMultifunction.HasValue(),
                   hiddenNonMultifunction.GetError().Error == PciFunctionCatalogError::FunctionNotFound,
                   !hiddenMissingFunctionZero.HasValue(),
                   hiddenMissingFunctionZero.GetError().Error == PciFunctionCatalogError::FunctionNotFound,
                   !outOfRange.HasValue(), outOfRange.GetError().Error == PciFunctionCatalogError::IndexOutOfRange,
                   secondDiscovery.Error == PciFunctionCatalogError::AlreadyDiscovered,
                   configuration.GetReadCount() == readsBeforeSecondDiscovery);

    constexpr Size WorkerCount{4U};
    constexpr Size Iterations{100U};
    std::atomic<Size> failures{0U};
    std::thread workers[WorkerCount];
    for (Size worker{0U}; worker < WorkerCount; ++worker)
    {
        workers[worker] = std::thread{[&, worker]() noexcept
                                      {
                                          for (Size iteration{0U}; iteration < Iterations; ++iteration)
                                          {
                                              const Size index = (worker + iteration) % 6U;
                                              const auto byIndex = catalog.GetFunction(index);
                                              if (!byIndex.HasValue())
                                              {
                                                  static_cast<void>(failures.fetch_add(1U, std::memory_order_acq_rel));
                                                  continue;
                                              }
                                              const auto byAddress = catalog.FindFunction(byIndex.GetValue().Address);
                                              if (!byAddress.HasValue() ||
                                                  !byAddress.GetValue().Address.IsEqualTo(byIndex.GetValue().Address))
                                              {
                                                  static_cast<void>(failures.fetch_add(1U, std::memory_order_acq_rel));
                                              }
                                          }
                                      }};
    }
    for (std::thread& worker : workers)
    {
        worker.join();
    }
    ASTERIA_EXPECT(runner, "concurrent catalog queries are stable and perform no hardware access",
                   failures.load(std::memory_order_acquire) == 0U,
                   configuration.GetReadCount() == readsBeforeSecondDiscovery);
}

void TestSegmentQualifiedLookup(TestContext& runner) noexcept
{
    FakePciConfigurationSpace configuration;
    const PciFunctionAddress firstAddress{1U, 0U, 0U, 0U};
    const PciFunctionAddress secondAddress{2U, 0U, 0U, 0U};
    const bool added = configuration.AddFunction(firstAddress, 0x1111U, 0xAAAAU, 0U, 0U, 0U, 1U, 0U, false) &&
                       configuration.AddFunction(secondAddress, 0x2222U, 0xBBBBU, 0U, 0U, 0U, 2U, 0U, false);
    const PciBusRange ranges[]{
        MakeRange(2U, 0U, 0U),
        MakeRange(1U, 0U, 0U),
    };
    PciFunctionCatalog catalog{configuration};
    const auto discovered = catalog.Discover(PciBusRangeView{
        .Data = ranges,
        .Count = 2U,
    });
    const auto first = catalog.FindFunction(firstAddress);
    const auto second = catalog.FindFunction(secondAddress);
    const auto absent = catalog.FindFunction(PciFunctionAddress{3U, 0U, 0U, 0U});
    ASTERIA_EXPECT(runner, "segment remains part of identity for otherwise equal BDF values", added,
                   discovered.Error == PciFunctionCatalogError::None, catalog.GetSnapshot().FunctionCount == 2U,
                   first.HasValue(), first.GetValue().DeviceId == 0xAAAAU, second.HasValue(),
                   second.GetValue().DeviceId == 0xBBBBU, !absent.HasValue(),
                   absent.GetError().Error == PciFunctionCatalogError::FunctionNotFound);
}

void TestReadFailureRollbackAndRetry(TestContext& runner) noexcept
{
    FakePciConfigurationSpace configuration;
    const PciFunctionAddress address{0U, 0U, 0U, 0U};
    ASTERIA_EXPECT(runner, "read-failure fixture installs its function",
                   configuration.AddFunction(address, 0xCAFEU, 0xBABEU, 1U, 2U, 3U, 4U, 0U, false));
    configuration.FailNextRead(address, 0x08U, PciConfigurationReadError::HardwareFailure);
    const PciBusRange range = MakeRange(0U, 0U, 0U);
    PciFunctionCatalog catalog{configuration};
    const auto failed = catalog.Discover(PciBusRangeView{
        .Data = &range,
        .Count = 1U,
    });
    const auto afterFailure = catalog.GetSnapshot();
    ASTERIA_EXPECT(runner, "configuration read failure clears every partial catalog record",
                   failed.Error == PciFunctionCatalogError::ConfigurationReadFailed,
                   failed.Configuration == PciConfigurationReadError::HardwareFailure,
                   failed.Address.IsEqualTo(address), failed.RegisterOffset == 0x08U,
                   afterFailure.State == PciFunctionCatalogState::Empty, afterFailure.FunctionCount == 0U,
                   afterFailure.ConfigurationReadCount == 0U, !catalog.GetFunction(0U).HasValue());

    const auto retried = catalog.Discover(PciBusRangeView{
        .Data = &range,
        .Count = 1U,
    });
    const auto snapshot = catalog.GetSnapshot();
    const auto function = catalog.GetFunction(0U);
    ASTERIA_EXPECT(runner, "a read-only discovery failure can be retried from an empty state",
                   retried.Error == PciFunctionCatalogError::None, snapshot.State == PciFunctionCatalogState::Ready,
                   snapshot.FunctionCount == 1U, snapshot.ConfigurationReadCount == 34U, function.HasValue(),
                   function.GetValue().VendorId == 0xCAFEU, function.GetValue().ClassCode == 4U);

    FakePciConfigurationSpace partialConfiguration;
    const PciFunctionAddress secondAddress{0U, 0U, 1U, 0U};
    const bool partialAdded =
        partialConfiguration.AddFunction(address, 0x1000U, 0x2000U, 0U, 0U, 0U, 1U, 0U, false) &&
        partialConfiguration.AddFunction(secondAddress, 0x1001U, 0x2001U, 0U, 0U, 0U, 2U, 0U, false);
    partialConfiguration.FailNextRead(secondAddress, 0x00U, PciConfigurationReadError::HardwareFailure);
    PciFunctionCatalog partialCatalog{partialConfiguration};
    const auto partialFailure = partialCatalog.Discover(PciBusRangeView{
        .Data = &range,
        .Count = 1U,
    });
    const auto partialSnapshot = partialCatalog.GetSnapshot();
    const auto partialRetry = partialCatalog.Discover(PciBusRangeView{
        .Data = &range,
        .Count = 1U,
    });
    ASTERIA_EXPECT(runner, "identifier failure after an append clears partial records before retry", partialAdded,
                   partialFailure.Error == PciFunctionCatalogError::ConfigurationReadFailed,
                   partialFailure.Address.IsEqualTo(secondAddress), partialFailure.RegisterOffset == 0U,
                   partialSnapshot.State == PciFunctionCatalogState::Empty, partialSnapshot.FunctionCount == 0U,
                   partialSnapshot.ConfigurationReadCount == 0U, partialRetry.Error == PciFunctionCatalogError::None,
                   partialCatalog.GetSnapshot().FunctionCount == 2U);

    FakePciConfigurationSpace headerConfiguration;
    const PciFunctionAddress headerFunctionOne{0U, 0U, 0U, 1U};
    const bool headerAdded =
        headerConfiguration.AddFunction(address, 0x3000U, 0x4000U, 0U, 0U, 0U, 3U, 0U, true) &&
        headerConfiguration.AddFunction(headerFunctionOne, 0x3001U, 0x4001U, 0U, 0U, 0U, 4U, 0U, false);
    headerConfiguration.FailNextRead(headerFunctionOne, 0x0CU, PciConfigurationReadError::HardwareFailure);
    PciFunctionCatalog headerCatalog{headerConfiguration};
    const auto headerFailure = headerCatalog.Discover(PciBusRangeView{
        .Data = &range,
        .Count = 1U,
    });
    ASTERIA_EXPECT(runner, "multifunction header failure clears function zero and propagates", headerAdded,
                   headerFailure.Error == PciFunctionCatalogError::ConfigurationReadFailed,
                   headerFailure.Address.IsEqualTo(headerFunctionOne), headerFailure.RegisterOffset == 0x0CU,
                   headerCatalog.GetSnapshot().State == PciFunctionCatalogState::Empty,
                   headerCatalog.GetSnapshot().FunctionCount == 0U);

    FakePciConfigurationSpace multifunctionConfiguration;
    const PciFunctionAddress functionOne{0U, 0U, 0U, 1U};
    const bool multifunctionAdded =
        multifunctionConfiguration.AddFunction(address, 0x5000U, 0x6000U, 0U, 0U, 0U, 4U, 0U, true) &&
        multifunctionConfiguration.AddFunction(functionOne, 0x5001U, 0x6001U, 0U, 0U, 0U, 5U, 0U, false);
    multifunctionConfiguration.FailNextRead(functionOne, 0x00U, PciConfigurationReadError::HardwareFailure);
    PciFunctionCatalog multifunctionCatalog{multifunctionConfiguration};
    const auto multifunctionFailure = multifunctionCatalog.Discover(PciBusRangeView{
        .Data = &range,
        .Count = 1U,
    });
    const auto multifunctionFailureSnapshot = multifunctionCatalog.GetSnapshot();
    const auto hiddenPartialFunction = multifunctionCatalog.GetFunction(0U);
    const auto multifunctionRetry = multifunctionCatalog.Discover(PciBusRangeView{
        .Data = &range,
        .Count = 1U,
    });
    ASTERIA_EXPECT(runner, "multifunction identifier failure clears function zero before retry", multifunctionAdded,
                   multifunctionFailure.Error == PciFunctionCatalogError::ConfigurationReadFailed,
                   multifunctionFailure.Address.IsEqualTo(functionOne), multifunctionFailure.RegisterOffset == 0U,
                   multifunctionFailureSnapshot.State == PciFunctionCatalogState::Empty,
                   multifunctionFailureSnapshot.FunctionCount == 0U,
                   multifunctionFailureSnapshot.ConfigurationReadCount == 0U, !hiddenPartialFunction.HasValue(),
                   hiddenPartialFunction.GetError().Error == PciFunctionCatalogError::NotReady,
                   multifunctionRetry.Error == PciFunctionCatalogError::None,
                   multifunctionCatalog.GetSnapshot().FunctionCount == 2U);
}

void TestFunctionCapacity(TestContext& runner) noexcept
{
    FakePciConfigurationSpace configuration;
    const bool dense = AddDenseBus(configuration, 7U, 0U);
    const bool excess =
        configuration.AddFunction(PciFunctionAddress{7U, 1U, 0U, 0U}, 0x4321U, 0x8765U, 0U, 0U, 0U, 2U, 0U, false);
    ASTERIA_EXPECT(runner, "capacity fixture installs 257 distinct PCI functions", dense, excess);

    const PciBusRange exactRange = MakeRange(7U, 0U, 0U);
    PciFunctionCatalog exactCatalog{configuration};
    const auto exact = exactCatalog.Discover(PciBusRangeView{
        .Data = &exactRange,
        .Count = 1U,
    });
    ASTERIA_EXPECT(runner, "exact fixed function capacity is fully usable",
                   exact.Error == PciFunctionCatalogError::None,
                   exactCatalog.GetSnapshot().FunctionCount == PciFunctionCatalog::MaximumFunctions,
                   exactCatalog.GetSnapshot().ConfigurationReadCount == 768U,
                   exactCatalog.GetFunction(PciFunctionCatalog::MaximumFunctions - 1U).HasValue());

    const PciBusRange excessRange = MakeRange(7U, 0U, 1U);
    PciFunctionCatalog excessCatalog{configuration};
    const auto rejected = excessCatalog.Discover(PciBusRangeView{
        .Data = &excessRange,
        .Count = 1U,
    });
    const auto snapshot = excessCatalog.GetSnapshot();
    ASTERIA_EXPECT(runner, "one excess function rejects the transaction and clears partial state",
                   rejected.Error == PciFunctionCatalogError::FunctionCapacityExceeded,
                   rejected.Address.IsEqualTo(PciFunctionAddress{7U, 1U, 0U, 0U}), rejected.RegisterOffset == 0U,
                   snapshot.State == PciFunctionCatalogState::Empty, snapshot.FunctionCount == 0U,
                   snapshot.ConfigurationReadCount == 0U);
}

void TestDiscoveryQuerySerialization(TestContext& runner) noexcept
{
    FakePciConfigurationSpace configuration;
    const PciFunctionAddress address{0U, 0U, 0U, 0U};
    const bool added = configuration.AddFunction(address, 0x1234U, 0x5678U, 1U, 2U, 3U, 4U, 0U, false);
    const PciBusRange range = MakeRange(0U, 0U, 0U);
    PciFunctionCatalog catalog{configuration};
    configuration.PauseNextRead();

    std::atomic<bool> discoveryCorrect{false};
    std::thread discovery{[&]() noexcept
                          {
                              const auto result = catalog.Discover(PciBusRangeView{
                                  .Data = &range,
                                  .Count = 1U,
                              });
                              discoveryCorrect.store(result.Error == PciFunctionCatalogError::None,
                                                     std::memory_order_release);
                          }};
    configuration.WaitUntilReadPaused();

    constexpr Size QueryCount{3U};
    std::atomic<bool> started[QueryCount]{};
    std::atomic<bool> finished[QueryCount]{};
    std::atomic<bool> correct[QueryCount]{};
    std::thread getQuery{[&]() noexcept
                         {
                             started[0U].store(true, std::memory_order_release);
                             started[0U].notify_all();
                             const auto result = catalog.GetFunction(0U);
                             correct[0U].store(result.HasValue() && result.GetValue().Address.IsEqualTo(address),
                                               std::memory_order_release);
                             finished[0U].store(true, std::memory_order_release);
                         }};
    std::thread findQuery{[&]() noexcept
                          {
                              started[1U].store(true, std::memory_order_release);
                              started[1U].notify_all();
                              const auto result = catalog.FindFunction(address);
                              correct[1U].store(result.HasValue() && (result.GetValue().DeviceId == 0x5678U),
                                                std::memory_order_release);
                              finished[1U].store(true, std::memory_order_release);
                          }};
    std::thread snapshotQuery{
        [&]() noexcept
        {
            started[2U].store(true, std::memory_order_release);
            started[2U].notify_all();
            const auto snapshot = catalog.GetSnapshot();
            correct[2U].store((snapshot.State == PciFunctionCatalogState::Ready) && (snapshot.FunctionCount == 1U),
                              std::memory_order_release);
            finished[2U].store(true, std::memory_order_release);
        }};

    for (Size index{0U}; index < QueryCount; ++index)
    {
        while (!started[index].load(std::memory_order_acquire))
        {
            started[index].wait(false, std::memory_order_acquire);
        }
    }
    bool completedWhileDiscoveryPaused{false};
    for (Size attempt{0U}; attempt < 100000U; ++attempt)
    {
        for (Size index{0U}; index < QueryCount; ++index)
        {
            completedWhileDiscoveryPaused =
                completedWhileDiscoveryPaused || finished[index].load(std::memory_order_acquire);
        }
        if (completedWhileDiscoveryPaused)
        {
            break;
        }
        std::this_thread::yield();
    }

    configuration.ResumePausedRead();
    discovery.join();
    getQuery.join();
    findQuery.join();
    snapshotQuery.join();
    bool allQueriesCorrect{true};
    for (Size index{0U}; index < QueryCount; ++index)
    {
        allQueriesCorrect = allQueriesCorrect && correct[index].load(std::memory_order_acquire);
    }
    ASTERIA_EXPECT(runner, "queries wait for discovery to publish one complete catalog state", added,
                   !completedWhileDiscoveryPaused, discoveryCorrect.load(std::memory_order_acquire), allQueriesCorrect);
}
} // namespace

int RunPciFunctionCatalogTests() noexcept
{
    TestContext runner{"PciFunctionCatalogTests"};
    TestRangeValidation(runner);
    TestRangeCapacityBoundaries(runner);
    TestTwoFullSegments(runner);
    TestSparseMultifunctionDiscovery(runner);
    TestSegmentQualifiedLookup(runner);
    TestReadFailureRollbackAndRetry(runner);
    TestFunctionCapacity(runner);
    TestDiscoveryQuerySerialization(runner);
    return runner.Finish();
}
} // namespace Asteria::Test

int main()
{
    return Asteria::Test::RunPciFunctionCatalogTests();
}
