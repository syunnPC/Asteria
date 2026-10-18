// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Kernel/Device/PciEndpointBarDecoder.hpp"

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
using Foundation::UInt64;
using Hal::Device::PciConfigurationReadError;
using Kernel::Device::PciEndpointBar;
using Kernel::Device::PciEndpointBarDecodeError;
using Kernel::Device::PciEndpointBarDecodeRequest;
using Kernel::Device::PciEndpointBarDecoder;
using Kernel::Device::PciEndpointBarExtentState;
using Kernel::Device::PciEndpointBarKind;
using Kernel::Device::PciFunctionAddress;

constexpr UInt64 MaximumUInt64{~UInt64{0U}};

class FakePciConfigurationSpace final : public Hal::Device::IPciConfigurationSpace
{
public:
    [[nodiscard]] PciConfigurationReadError Read32(const PciFunctionAddress address, const UInt16 registerOffset,
                                                   UInt32& output) noexcept override
    {
        if (m_readCount < MaximumRecordedReads)
        {
            m_addresses[m_readCount] = address;
            m_offsets[m_readCount] = registerOffset;
        }
        ++m_readCount;
        if ((registerOffset < FirstBarOffset) || (registerOffset > LastBarOffset) || ((registerOffset & 3U) != 0U))
        {
            return PciConfigurationReadError::InvalidAddress;
        }
        const Size index = static_cast<Size>((registerOffset - FirstBarOffset) / 4U);
        if (m_errors[index] != PciConfigurationReadError::None)
        {
            return m_errors[index];
        }
        output = m_values[index];
        return PciConfigurationReadError::None;
    }

    void SetBar(const UInt8 index, const UInt32 value,
                const PciConfigurationReadError error = PciConfigurationReadError::None) noexcept
    {
        m_values[index] = value;
        m_errors[index] = error;
    }

    [[nodiscard]] Size GetReadCount() const noexcept
    {
        return m_readCount;
    }

    [[nodiscard]] UInt16 GetOffset(const Size index) const noexcept
    {
        return m_offsets[index];
    }

    [[nodiscard]] PciFunctionAddress GetAddress(const Size index) const noexcept
    {
        return m_addresses[index];
    }

private:
    static constexpr Size MaximumRecordedReads{16U};
    static constexpr UInt16 FirstBarOffset{0x10U};
    static constexpr UInt16 LastBarOffset{0x24U};

    UInt32 m_values[6U]{};
    PciConfigurationReadError m_errors[6U]{};
    PciFunctionAddress m_addresses[MaximumRecordedReads]{};
    UInt16 m_offsets[MaximumRecordedReads]{};
    Size m_readCount{0U};
};

[[nodiscard]] constexpr PciEndpointBarDecodeRequest
MakeRequest(const UInt8 barIndex, const UInt64 maximumPhysicalAddress = MaximumUInt64) noexcept
{
    return PciEndpointBarDecodeRequest{
        .MaximumPhysicalAddress = maximumPhysicalAddress,
        .ExtentBytes = 0U,
        .Address = PciFunctionAddress{7U, 0x41U, 9U, 2U},
        .ExtentState = PciEndpointBarExtentState::Unknown,
        .BarIndex = barIndex,
    };
}

[[nodiscard]] constexpr PciEndpointBar MakeSentinel() noexcept
{
    return PciEndpointBar{
        .BaseAddress = 0xAA55'AA55'AA55'AA55ULL,
        .ExtentBytes = 0x1122'3344'5566'7788ULL,
        .Kind = PciEndpointBarKind::Memory64,
        .ExtentState = PciEndpointBarExtentState::Authoritative,
        .BarIndex = 4U,
        .RegisterCount = 2U,
        .Prefetchable = true,
        .Reserved = {1U, 2U, 3U},
    };
}

[[nodiscard]] constexpr bool IsSentinel(const PciEndpointBar& bar) noexcept
{
    return (bar.BaseAddress == 0xAA55'AA55'AA55'AA55ULL) && (bar.ExtentBytes == 0x1122'3344'5566'7788ULL) &&
           (bar.Kind == PciEndpointBarKind::Memory64) &&
           (bar.ExtentState == PciEndpointBarExtentState::Authoritative) && (bar.BarIndex == 4U) &&
           (bar.RegisterCount == 2U) && bar.Prefetchable && (bar.Reserved[0U] == 1U) && (bar.Reserved[1U] == 2U) &&
           (bar.Reserved[2U] == 3U);
}

[[nodiscard]] constexpr bool FailureReservedIsZero(const Kernel::Device::PciEndpointBarDecodeFailure& failure) noexcept
{
    return (failure.Reserved[0U] == 0U) && (failure.Reserved[1U] == 0U) && (failure.Reserved[2U] == 0U) &&
           (failure.Reserved[3U] == 0U) && (failure.Reserved[4U] == 0U) && (failure.Reserved[5U] == 0U);
}

void TestIoAndMemory32Decoding(TestContext& runner) noexcept
{
    FakePciConfigurationSpace configuration;
    configuration.SetBar(5U, 0xFEBF'0008U);
    configuration.SetBar(2U, 0x0000'C001U);
    PciEndpointBarDecoder decoder{configuration};
    PciEndpointBar bar{};

    const auto memory = decoder.Decode(MakeRequest(5U), bar);
    ASTERIA_EXPECT(runner, "a prefetchable 32-bit BAR is decoded without inventing extent",
                   memory.Error == PciEndpointBarDecodeError::None, bar.Kind == PciEndpointBarKind::Memory32,
                   bar.BaseAddress == 0xFEBF'0000ULL, bar.ExtentState == PciEndpointBarExtentState::Unknown,
                   bar.ExtentBytes == 0U, bar.RegisterCount == 1U, bar.Prefetchable);

    const auto io = decoder.Decode(MakeRequest(2U), bar);
    ASTERIA_EXPECT(runner, "an I/O BAR masks attributes and each one-register decode reads once",
                   io.Error == PciEndpointBarDecodeError::None, bar.Kind == PciEndpointBarKind::Io,
                   bar.BaseAddress == 0xC000U, bar.RegisterCount == 1U, !bar.Prefetchable,
                   configuration.GetReadCount() == 2U, configuration.GetOffset(0U) == 0x24U,
                   configuration.GetOffset(1U) == 0x18U,
                   configuration.GetAddress(0U).IsEqualTo(MakeRequest(5U).Address));
}

void TestMalformedAndUnavailableBars(TestContext& runner) noexcept
{
    FakePciConfigurationSpace configuration;
    configuration.SetBar(0U, 0U);
    configuration.SetBar(1U, 1U);
    configuration.SetBar(2U, 0x1003U);
    configuration.SetBar(3U, 0x1002U);
    configuration.SetBar(4U, 0x1006U);
    PciEndpointBarDecoder decoder{configuration};

    PciEndpointBar zeroOutput = MakeSentinel();
    const Size readsBeforeZero = configuration.GetReadCount();
    const auto zero = decoder.Decode(MakeRequest(0U), zeroOutput);
    ASTERIA_EXPECT(runner, "an absent BAR preserves every output field after one read",
                   zero.Error == PciEndpointBarDecodeError::NoAssignedAddress,
                   zero.ConfigurationRead == PciConfigurationReadError::None, FailureReservedIsZero(zero),
                   configuration.GetReadCount() == (readsBeforeZero + 1U), IsSentinel(zeroOutput));

    PciEndpointBar ioUnassignedOutput = MakeSentinel();
    const Size readsBeforeIoUnassigned = configuration.GetReadCount();
    const auto ioUnassigned = decoder.Decode(MakeRequest(1U), ioUnassignedOutput);
    ASTERIA_EXPECT(runner, "an unassigned I/O BAR preserves every output field after one read",
                   ioUnassigned.Error == PciEndpointBarDecodeError::NoAssignedAddress,
                   ioUnassigned.ConfigurationRead == PciConfigurationReadError::None,
                   FailureReservedIsZero(ioUnassigned), configuration.GetReadCount() == (readsBeforeIoUnassigned + 1U),
                   IsSentinel(ioUnassignedOutput));

    PciEndpointBar ioReservedOutput = MakeSentinel();
    const Size readsBeforeIoReserved = configuration.GetReadCount();
    const auto ioReserved = decoder.Decode(MakeRequest(2U), ioReservedOutput);
    ASTERIA_EXPECT(runner, "a reserved I/O BAR preserves every output field after one read",
                   ioReserved.Error == PciEndpointBarDecodeError::ReservedEncoding,
                   ioReserved.ConfigurationRead == PciConfigurationReadError::None, FailureReservedIsZero(ioReserved),
                   configuration.GetReadCount() == (readsBeforeIoReserved + 1U), IsSentinel(ioReservedOutput));

    PciEndpointBar memoryBelowMegabyteOutput = MakeSentinel();
    const Size readsBeforeMemoryBelowMegabyte = configuration.GetReadCount();
    const auto memoryBelowMegabyte = decoder.Decode(MakeRequest(3U), memoryBelowMegabyteOutput);
    ASTERIA_EXPECT(runner, "a below-1-MiB BAR preserves every output field after one read",
                   memoryBelowMegabyte.Error == PciEndpointBarDecodeError::ReservedEncoding,
                   memoryBelowMegabyte.ConfigurationRead == PciConfigurationReadError::None,
                   FailureReservedIsZero(memoryBelowMegabyte),
                   configuration.GetReadCount() == (readsBeforeMemoryBelowMegabyte + 1U),
                   IsSentinel(memoryBelowMegabyteOutput));

    PciEndpointBar memoryReservedOutput = MakeSentinel();
    const Size readsBeforeMemoryReserved = configuration.GetReadCount();
    const auto memoryReserved = decoder.Decode(MakeRequest(4U), memoryReservedOutput);
    ASTERIA_EXPECT(runner, "a reserved memory BAR preserves every output field after one read",
                   memoryReserved.Error == PciEndpointBarDecodeError::ReservedEncoding,
                   memoryReserved.ConfigurationRead == PciConfigurationReadError::None,
                   FailureReservedIsZero(memoryReserved),
                   configuration.GetReadCount() == (readsBeforeMemoryReserved + 1U), IsSentinel(memoryReservedOutput));
}

void TestRequestValidationAndReadFailure(TestContext& runner) noexcept
{
    FakePciConfigurationSpace configuration;
    configuration.SetBar(1U, 0U, PciConfigurationReadError::HardwareFailure);
    PciEndpointBarDecoder decoder{configuration};
    PciEndpointBar output = MakeSentinel();

    PciEndpointBarDecodeRequest invalidFunction = MakeRequest(0U);
    invalidFunction.Address = PciFunctionAddress{};
    PciEndpointBarDecodeRequest invalidExtent = MakeRequest(0U);
    invalidExtent.ExtentBytes = 0x1000U;

    const Size readsBeforeFunction = configuration.GetReadCount();
    const auto function = decoder.Decode(invalidFunction, output);
    ASTERIA_EXPECT(runner, "an invalid function preserves every output field without a read",
                   function.Error == PciEndpointBarDecodeError::InvalidFunctionAddress,
                   function.ConfigurationRead == PciConfigurationReadError::None, FailureReservedIsZero(function),
                   configuration.GetReadCount() == readsBeforeFunction, IsSentinel(output));

    output = MakeSentinel();
    const Size readsBeforeIndex = configuration.GetReadCount();
    const auto index = decoder.Decode(MakeRequest(6U), output);
    ASTERIA_EXPECT(runner, "an invalid BAR index preserves every output field without a read",
                   index.Error == PciEndpointBarDecodeError::InvalidBarIndex,
                   index.ConfigurationRead == PciConfigurationReadError::None, FailureReservedIsZero(index),
                   configuration.GetReadCount() == readsBeforeIndex, IsSentinel(output));

    output = MakeSentinel();
    const Size readsBeforeExtent = configuration.GetReadCount();
    const auto extent = decoder.Decode(invalidExtent, output);
    ASTERIA_EXPECT(runner, "inconsistent extent metadata preserves every output field without a read",
                   extent.Error == PciEndpointBarDecodeError::InvalidExtent,
                   extent.ConfigurationRead == PciConfigurationReadError::None, FailureReservedIsZero(extent),
                   configuration.GetReadCount() == readsBeforeExtent, IsSentinel(output));

    output = MakeSentinel();
    const Size readsBeforeRead = configuration.GetReadCount();
    const auto read = decoder.Decode(MakeRequest(1U), output);
    ASTERIA_EXPECT(runner, "a lower-register failure preserves every output field after one read",
                   read.Error == PciEndpointBarDecodeError::ConfigurationReadFailed,
                   read.ConfigurationRead == PciConfigurationReadError::HardwareFailure, FailureReservedIsZero(read),
                   configuration.GetReadCount() == (readsBeforeRead + 1U), IsSentinel(output));
}

void TestMemory64Pairing(TestContext& runner) noexcept
{
    FakePciConfigurationSpace configuration;
    configuration.SetBar(3U, 0xABC0'000CU);
    configuration.SetBar(4U, 0x0000'0012U);
    configuration.SetBar(5U, 0x0000'0004U);
    configuration.SetBar(1U, 0x0000'0004U);
    configuration.SetBar(2U, 0U, PciConfigurationReadError::Unsupported);
    PciEndpointBarDecoder decoder{configuration};
    PciEndpointBar output{};

    const auto decoded = decoder.Decode(MakeRequest(3U), output);
    ASTERIA_EXPECT(runner, "a 64-bit BAR consumes its immediately following upper register",
                   decoded.Error == PciEndpointBarDecodeError::None, output.Kind == PciEndpointBarKind::Memory64,
                   output.BaseAddress == 0x0000'0012'ABC0'0000ULL, output.RegisterCount == 2U, output.Prefetchable,
                   configuration.GetOffset(0U) == 0x1CU, configuration.GetOffset(1U) == 0x20U);

    output = MakeSentinel();
    const Size readsBeforeMissing = configuration.GetReadCount();
    const auto missing = decoder.Decode(MakeRequest(5U), output);
    ASTERIA_EXPECT(runner, "BAR5 cannot start a pair and preserves every output field",
                   missing.Error == PciEndpointBarDecodeError::MissingUpperBar,
                   missing.ConfigurationRead == PciConfigurationReadError::None, FailureReservedIsZero(missing),
                   configuration.GetReadCount() == (readsBeforeMissing + 1U), configuration.GetOffset(2U) == 0x24U,
                   IsSentinel(output));

    output = MakeSentinel();
    const Size readsBeforeUpperFailure = configuration.GetReadCount();
    const auto upperFailure = decoder.Decode(MakeRequest(1U), output);
    ASTERIA_EXPECT(runner, "an upper-register failure preserves every output field after two reads",
                   upperFailure.Error == PciEndpointBarDecodeError::ConfigurationReadFailed,
                   upperFailure.ConfigurationRead == PciConfigurationReadError::Unsupported,
                   FailureReservedIsZero(upperFailure), configuration.GetReadCount() == (readsBeforeUpperFailure + 2U),
                   configuration.GetOffset(3U) == 0x14U, configuration.GetOffset(4U) == 0x18U, IsSentinel(output));
}

void TestExtentAndAddressBoundaries(TestContext& runner) noexcept
{
    FakePciConfigurationSpace configuration;
    configuration.SetBar(0U, 0x0000'0004U);
    configuration.SetBar(1U, 0x0000'0001U);
    PciEndpointBarDecoder decoder{configuration};
    PciEndpointBar output{};

    PciEndpointBarDecodeRequest exact = MakeRequest(0U, 0x0000'0001'0000'0FFFULL);
    exact.ExtentState = PciEndpointBarExtentState::Authoritative;
    exact.ExtentBytes = 0x1000U;
    const auto exactLimit = decoder.Decode(exact, output);
    ASTERIA_EXPECT(runner, "an authoritative extent may end exactly at the physical ceiling",
                   exactLimit.Error == PciEndpointBarDecodeError::None,
                   exactLimit.ConfigurationRead == PciConfigurationReadError::None, FailureReservedIsZero(exactLimit),
                   output.BaseAddress == 0x0000'0001'0000'0000ULL,
                   output.ExtentState == PciEndpointBarExtentState::Authoritative, output.ExtentBytes == 0x1000U,
                   output.Kind == PciEndpointBarKind::Memory64, output.BarIndex == 0U, output.RegisterCount == 2U,
                   !output.Prefetchable, output.Reserved[0U] == 0U, output.Reserved[1U] == 0U,
                   output.Reserved[2U] == 0U);

    PciEndpointBarDecodeRequest beyond = exact;
    beyond.MaximumPhysicalAddress = 0x0000'0001'0000'0FFEULL;
    output = MakeSentinel();
    const Size readsBeforeBeyond = configuration.GetReadCount();
    const auto beyondLimit = decoder.Decode(beyond, output);
    ASTERIA_EXPECT(runner, "an extent above the physical ceiling preserves every output field",
                   beyondLimit.Error == PciEndpointBarDecodeError::PhysicalAddressLimitExceeded,
                   beyondLimit.ConfigurationRead == PciConfigurationReadError::None, FailureReservedIsZero(beyondLimit),
                   configuration.GetReadCount() == (readsBeforeBeyond + 2U), IsSentinel(output));

    PciEndpointBarDecodeRequest baseAbove = exact;
    baseAbove.MaximumPhysicalAddress = 0x0000'0000'FFFF'FFFFULL;
    output = MakeSentinel();
    const Size readsBeforeBaseAbove = configuration.GetReadCount();
    const auto baseAboveLimit = decoder.Decode(baseAbove, output);
    ASTERIA_EXPECT(runner, "a base above the physical ceiling is rejected without subtraction underflow",
                   baseAboveLimit.Error == PciEndpointBarDecodeError::PhysicalAddressLimitExceeded,
                   baseAboveLimit.ConfigurationRead == PciConfigurationReadError::None,
                   FailureReservedIsZero(baseAboveLimit), configuration.GetReadCount() == (readsBeforeBaseAbove + 2U),
                   IsSentinel(output));

    const PciEndpointBarDecodeRequest unknownAbove = MakeRequest(0U, 0x0000'0000'FFFF'FFFFULL);
    output = MakeSentinel();
    const Size readsBeforeUnknownAbove = configuration.GetReadCount();
    const auto unknownAboveLimit = decoder.Decode(unknownAbove, output);
    ASTERIA_EXPECT(runner, "an unknown extent still enforces the decoded base physical ceiling",
                   unknownAboveLimit.Error == PciEndpointBarDecodeError::PhysicalAddressLimitExceeded,
                   unknownAboveLimit.ConfigurationRead == PciConfigurationReadError::None,
                   FailureReservedIsZero(unknownAboveLimit),
                   configuration.GetReadCount() == (readsBeforeUnknownAbove + 2U), IsSentinel(output));

    PciEndpointBarDecodeRequest invalid = MakeRequest(0U);
    invalid.ExtentState = PciEndpointBarExtentState::Authoritative;
    invalid.ExtentBytes = 24U;
    output = MakeSentinel();
    const Size readsBeforeInvalid = configuration.GetReadCount();
    const auto invalidExtent = decoder.Decode(invalid, output);
    ASTERIA_EXPECT(runner, "a non-power-of-two extent preserves every output field without a read",
                   invalidExtent.Error == PciEndpointBarDecodeError::InvalidExtent,
                   invalidExtent.ConfigurationRead == PciConfigurationReadError::None,
                   FailureReservedIsZero(invalidExtent), configuration.GetReadCount() == readsBeforeInvalid,
                   IsSentinel(output));
}

void TestMisalignedAuthoritativeBase(TestContext& runner) noexcept
{
    FakePciConfigurationSpace configuration;
    configuration.SetBar(0U, 0x0000'3000U);
    PciEndpointBarDecoder decoder{configuration};
    PciEndpointBarDecodeRequest request = MakeRequest(0U);
    request.ExtentState = PciEndpointBarExtentState::Authoritative;
    request.ExtentBytes = 0x2000U;
    PciEndpointBar output = MakeSentinel();

    const Size readsBefore = configuration.GetReadCount();
    const auto result = decoder.Decode(request, output);
    ASTERIA_EXPECT(runner, "an isolated misaligned base fails after one read and preserves output",
                   result.Error == PciEndpointBarDecodeError::InvalidExtent,
                   result.ConfigurationRead == PciConfigurationReadError::None, FailureReservedIsZero(result),
                   configuration.GetReadCount() == (readsBefore + 1U), IsSentinel(output));
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"PciEndpointBarDecoderTests"};
    Asteria::Test::TestIoAndMemory32Decoding(runner);
    Asteria::Test::TestMalformedAndUnavailableBars(runner);
    Asteria::Test::TestRequestValidationAndReadFailure(runner);
    Asteria::Test::TestMemory64Pairing(runner);
    Asteria::Test::TestExtentAndAddressBoundaries(runner);
    Asteria::Test::TestMisalignedAuthoritativeBase(runner);
    return runner.Finish();
}
