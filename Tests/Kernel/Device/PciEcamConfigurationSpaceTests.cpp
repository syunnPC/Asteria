// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Kernel/Device/PciEcamConfigurationSpace.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;
using Hal::Device::MemoryMappedIoReadError;
using Hal::Device::MemoryMappedIoWidth;
using Hal::Device::MemoryMappedIoWriteError;
using Hal::Device::PciConfigurationReadError;
using Hal::Device::PciConfigurationWriteError;
using Kernel::Device::PciEcamConfigurationSpace;
using Kernel::Device::PciEcamConfigurationSpaceInitializationError;
using Kernel::Device::PciEcamConfigurationSpaceState;
using Kernel::Device::PciEcamRegion;
using Kernel::Device::PciEcamRegionView;
using Kernel::Device::PciFunctionAddress;

class FakeMemoryMappedIo final : public Hal::Device::IMemoryMappedIo
{
public:
    [[nodiscard]] MemoryMappedIoReadError Read(const UInt64 physicalAddress, const MemoryMappedIoWidth width,
                                               UInt64& output) noexcept override
    {
        ++m_readCount;
        m_lastAddress = physicalAddress;
        m_lastWidth = width;
        if (m_error != MemoryMappedIoReadError::None)
        {
            return m_error;
        }
        output = m_value;
        return MemoryMappedIoReadError::None;
    }

    void SetResult(const MemoryMappedIoReadError error, const UInt64 value = 0U) noexcept
    {
        m_error = error;
        m_value = value;
    }

    [[nodiscard]] UInt64 GetLastAddress() const noexcept
    {
        return m_lastAddress;
    }

    [[nodiscard]] MemoryMappedIoWidth GetLastWidth() const noexcept
    {
        return m_lastWidth;
    }

    [[nodiscard]] Size GetReadCount() const noexcept
    {
        return m_readCount;
    }

private:
    MemoryMappedIoReadError m_error{MemoryMappedIoReadError::None};
    UInt64 m_value{0U};
    UInt64 m_lastAddress{0U};
    Size m_readCount{0U};
    MemoryMappedIoWidth m_lastWidth{MemoryMappedIoWidth::Byte};
};

// Records the writes it is given.
class FakeMemoryMappedIoWriter final : public Hal::Device::IMemoryMappedIoWriter
{
public:
    [[nodiscard]] MemoryMappedIoWriteError Write(const UInt64 physicalAddress, const MemoryMappedIoWidth width,
                                                 const UInt64 value) noexcept override
    {
        ++Writes;
        LastAddress = physicalAddress;
        LastWidth = width;
        LastValue = value;
        return Error;
    }

    MemoryMappedIoWriteError Error{MemoryMappedIoWriteError::None};
    Size Writes{0U};
    UInt64 LastAddress{0U};
    UInt64 LastValue{0U};
    MemoryMappedIoWidth LastWidth{MemoryMappedIoWidth::Byte};
};

[[nodiscard]] constexpr PciEcamRegion MakeRegion(const UInt64 physicalStart, const UInt16 segment,
                                                 const Foundation::UInt8 firstBus,
                                                 const Foundation::UInt8 lastBus) noexcept
{
    return PciEcamRegion{
        .PhysicalStart = physicalStart,
        .Segment = segment,
        .FirstBus = firstBus,
        .LastBus = lastBus,
        .Padding = {},
    };
}

void TestUnavailableAndValidation(TestContext& runner) noexcept
{
    FakeMemoryMappedIo memoryMappedIo;
    PciEcamConfigurationSpace uninitialized{memoryMappedIo};
    UInt32 output{0xA5A5'A5A5U};
    ASTERIA_EXPECT(runner, "an uninitialized reader fails without publishing output",
                   uninitialized.Read32(PciFunctionAddress{0U, 0U, 0U, 0U}, 0U, output) ==
                       PciConfigurationReadError::NotReady,
                   output == 0xA5A5'A5A5U);

    PciEcamConfigurationSpace unavailable{memoryMappedIo};
    const auto initialized = unavailable.Initialize(PciEcamRegionView{
        .Data = nullptr,
        .Count = 0U,
    });
    const auto unsupported = unavailable.Read32(PciFunctionAddress{0U, 0U, 0U, 0U}, 0U, output);
    const auto invalid = unavailable.Read32(PciFunctionAddress{0U, 0U, 32U, 0U}, 0U, output);
    ASTERIA_EXPECT(runner, "empty optional topology is unavailable and still validates requests",
                   initialized == PciEcamConfigurationSpaceInitializationError::None,
                   unavailable.GetSnapshot().State == PciEcamConfigurationSpaceState::Unavailable,
                   unsupported == PciConfigurationReadError::Unsupported,
                   invalid == PciConfigurationReadError::InvalidAddress, output == 0xA5A5'A5A5U,
                   memoryMappedIo.GetReadCount() == 0U);
}

void TestNormalizationAndLookup(TestContext& runner) noexcept
{
    FakeMemoryMappedIo memoryMappedIo;
    memoryMappedIo.SetResult(MemoryMappedIoReadError::None, 0x1234'8086U);
    PciEcamConfigurationSpace configuration{memoryMappedIo};
    const PciEcamRegion regions[]{
        MakeRegion(0x9000'0000ULL, 9U, 64U, 95U),
        MakeRegion(0x5000'0000ULL, 3U, 32U, 47U),
    };
    const auto initialized = configuration.Initialize(PciEcamRegionView{
        .Data = regions,
        .Count = 2U,
    });
    UInt32 output{0U};
    const auto read = configuration.Read32(PciFunctionAddress{9U, 65U, 2U, 3U}, 0x00FCU, output);
    ASTERIA_EXPECT(runner, "segment lookup uses a FirstBus-relative generic physical start",
                   initialized == PciEcamConfigurationSpaceInitializationError::None,
                   configuration.GetSnapshot().RegionCount == 2U, read == PciConfigurationReadError::None,
                   output == 0x1234'8086U, memoryMappedIo.GetLastAddress() == 0x9011'30FCULL,
                   memoryMappedIo.GetLastWidth() == MemoryMappedIoWidth::DoubleWord);

    const Size priorReads = memoryMappedIo.GetReadCount();
    output = 0xCAFE'BABEU;
    const auto wrongSegment = configuration.Read32(PciFunctionAddress{8U, 65U, 0U, 0U}, 0U, output);
    const auto wrongBus = configuration.Read32(PciFunctionAddress{9U, 63U, 0U, 0U}, 0U, output);
    const auto unaligned = configuration.Read32(PciFunctionAddress{9U, 64U, 0U, 0U}, 2U, output);
    const auto beyond = configuration.Read32(PciFunctionAddress{9U, 64U, 0U, 0U}, 0x1000U, output);
    ASTERIA_EXPECT(
        runner, "segment, bus, alignment, and register bounds fail before MMIO",
        wrongSegment == PciConfigurationReadError::Unsupported, wrongBus == PciConfigurationReadError::Unsupported,
        unaligned == PciConfigurationReadError::InvalidAddress, beyond == PciConfigurationReadError::InvalidAddress,
        output == 0xCAFE'BABEU, memoryMappedIo.GetReadCount() == priorReads);
}

void TestInitializationFailureAtomicity(TestContext& runner) noexcept
{
    FakeMemoryMappedIo memoryMappedIo;
    PciEcamConfigurationSpace configuration{memoryMappedIo};
    PciEcamRegion bad[]{MakeRegion(0xFFFF'FFFF'FFF0'0000ULL, 0U, 0U, 1U)};
    const auto overflow = configuration.Initialize(PciEcamRegionView{
        .Data = bad,
        .Count = 1U,
    });
    bad[0U] = MakeRegion(0x4000'1000ULL, 0U, 0U, 0U);
    const auto alignment = configuration.Initialize(PciEcamRegionView{
        .Data = bad,
        .Count = 1U,
    });
    PciEcamRegion overlap[]{
        MakeRegion(0x4000'0000ULL, 1U, 0U, 3U),
        MakeRegion(0x5000'0000ULL, 1U, 3U, 4U),
    };
    const auto busOverlap = configuration.Initialize(PciEcamRegionView{
        .Data = overlap,
        .Count = 2U,
    });
    overlap[1U] = MakeRegion(0x4020'0000ULL, 2U, 8U, 9U);
    const auto physicalOverlap = configuration.Initialize(PciEcamRegionView{
        .Data = overlap,
        .Count = 2U,
    });
    const PciEcamRegion good[]{MakeRegion(0x6000'0000ULL, 4U, 8U, 8U)};
    const auto retry = configuration.Initialize(PciEcamRegionView{
        .Data = good,
        .Count = 1U,
    });
    ASTERIA_EXPECT(runner, "failed initialization is atomic and permits a corrected retry",
                   overflow == PciEcamConfigurationSpaceInitializationError::InvalidRegion,
                   alignment == PciEcamConfigurationSpaceInitializationError::InvalidRegion,
                   busOverlap == PciEcamConfigurationSpaceInitializationError::OverlappingBusRange,
                   physicalOverlap == PciEcamConfigurationSpaceInitializationError::OverlappingPhysicalRange,
                   retry == PciEcamConfigurationSpaceInitializationError::None,
                   configuration.GetSnapshot().State == PciEcamConfigurationSpaceState::Ready);
}

void TestMemoryMappedIoFailures(TestContext& runner) noexcept
{
    FakeMemoryMappedIo memoryMappedIo;
    PciEcamConfigurationSpace configuration{memoryMappedIo};
    const PciEcamRegion region = MakeRegion(0x7000'0000ULL, 2U, 10U, 10U);
    const auto initialized = configuration.Initialize(PciEcamRegionView{
        .Data = &region,
        .Count = 1U,
    });
    UInt32 output{0x7654'3210U};
    memoryMappedIo.SetResult(MemoryMappedIoReadError::NotReady);
    const auto notReady = configuration.Read32(PciFunctionAddress{2U, 10U, 0U, 0U}, 0U, output);
    memoryMappedIo.SetResult(MemoryMappedIoReadError::HardwareFailure);
    const auto hardware = configuration.Read32(PciFunctionAddress{2U, 10U, 0U, 0U}, 4U, output);
    memoryMappedIo.SetResult(MemoryMappedIoReadError::None, 1ULL << 32U);
    const auto invalidBackendValue = configuration.Read32(PciFunctionAddress{2U, 10U, 0U, 0U}, 8U, output);
    ASTERIA_EXPECT(runner, "MMIO failures and invalid backend results leave output unchanged",
                   initialized == PciEcamConfigurationSpaceInitializationError::None,
                   notReady == PciConfigurationReadError::NotReady,
                   hardware == PciConfigurationReadError::HardwareFailure,
                   invalidBackendValue == PciConfigurationReadError::HardwareFailure, output == 0x7654'3210U);
}
// Writes select the window as reads do and store one DWORD; without a
// writer, or outside the windows, nothing is written.
void TestWrites(TestContext& runner) noexcept
{
    FakeMemoryMappedIo memoryMappedIo;
    FakeMemoryMappedIoWriter writer;
    PciEcamConfigurationSpace configuration{memoryMappedIo, writer};
    PciEcamConfigurationSpace readOnly{memoryMappedIo};
    const PciEcamRegion regions[]{MakeRegion(0x9000'0000ULL, 9U, 64U, 95U)};
    const auto uninitialized = configuration.Write32(PciFunctionAddress{9U, 65U, 2U, 3U}, 4U, 0x0006U);
    const bool initialized = (configuration.Initialize(PciEcamRegionView{.Data = regions, .Count = 1U}) ==
                              PciEcamConfigurationSpaceInitializationError::None) &&
                             (readOnly.Initialize(PciEcamRegionView{.Data = regions, .Count = 1U}) ==
                              PciEcamConfigurationSpaceInitializationError::None);
    const auto written = configuration.Write32(PciFunctionAddress{9U, 65U, 2U, 3U}, 0x00FCU, 0xDEAD'BEEFU);
    const Size afterWrite = writer.Writes;
    const UInt64 firstAddress = writer.LastAddress;
    const UInt64 firstValue = writer.LastValue;
    const MemoryMappedIoWidth firstWidth = writer.LastWidth;
    const auto wrongBus = configuration.Write32(PciFunctionAddress{9U, 63U, 0U, 0U}, 4U, 0U);
    const auto unaligned = configuration.Write32(PciFunctionAddress{9U, 64U, 0U, 0U}, 6U, 0U);
    const auto withoutWriter = readOnly.Write32(PciFunctionAddress{9U, 65U, 2U, 3U}, 4U, 0U);
    writer.Error = MemoryMappedIoWriteError::AccessDenied;
    const auto refused = configuration.Write32(PciFunctionAddress{9U, 65U, 2U, 3U}, 4U, 0U);
    ASTERIA_EXPECT(runner, "a configuration write stores one DWORD in the selected window",
                   uninitialized == PciConfigurationWriteError::NotReady, initialized,
                   written == PciConfigurationWriteError::None, afterWrite == 1U, firstAddress == 0x9011'30FCULL,
                   firstWidth == MemoryMappedIoWidth::DoubleWord, firstValue == 0xDEAD'BEEFU,
                   wrongBus == PciConfigurationWriteError::Unsupported,
                   unaligned == PciConfigurationWriteError::InvalidAddress,
                   withoutWriter == PciConfigurationWriteError::Unsupported,
                   refused == PciConfigurationWriteError::HardwareFailure, writer.Writes == 2U,
                   memoryMappedIo.GetReadCount() == 0U);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"PciEcamConfigurationSpaceTests"};
    Asteria::Test::TestUnavailableAndValidation(runner);
    Asteria::Test::TestNormalizationAndLookup(runner);
    Asteria::Test::TestInitializationFailureAtomicity(runner);
    Asteria::Test::TestMemoryMappedIoFailures(runner);
    Asteria::Test::TestWrites(runner);
    return runner.Finish();
}
