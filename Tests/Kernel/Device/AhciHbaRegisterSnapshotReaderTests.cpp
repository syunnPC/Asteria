// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Kernel/Device/AhciHbaRegisterSnapshotReader.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt64;
using Hal::Device::MemoryMappedIoReadError;
using Hal::Device::MemoryMappedIoWidth;
using Kernel::Device::AhciHbaGlobalRegister;
using Kernel::Device::AhciHbaRegisterRange;
using Kernel::Device::AhciHbaRegisterSnapshot;
using Kernel::Device::AhciHbaRegisterSnapshotReader;
using Kernel::Device::AhciHbaSnapshotError;

class FakeMemoryMappedIo final : public Hal::Device::IMemoryMappedIo
{
public:
    [[nodiscard]] MemoryMappedIoReadError Read(const UInt64 physicalAddress, const MemoryMappedIoWidth width,
                                               UInt64& output) noexcept override
    {
        const Size index = m_readCount;
        ++m_readCount;
        if (index >= MaximumReads)
        {
            return MemoryMappedIoReadError::HardwareFailure;
        }
        m_addresses[index] = physicalAddress;
        m_widths[index] = width;
        if ((index == m_failureIndex) && (m_failure != MemoryMappedIoReadError::None))
        {
            return m_failure;
        }
        output = m_values[index];
        return MemoryMappedIoReadError::None;
    }

    void SetValue(const Size index, const UInt64 value) noexcept
    {
        m_values[index] = value;
    }

    void SetFailure(const Size index, const MemoryMappedIoReadError failure) noexcept
    {
        m_failureIndex = index;
        m_failure = failure;
    }

    [[nodiscard]] Size GetReadCount() const noexcept
    {
        return m_readCount;
    }

    [[nodiscard]] UInt64 GetAddress(const Size index) const noexcept
    {
        return m_addresses[index];
    }

    [[nodiscard]] MemoryMappedIoWidth GetWidth(const Size index) const noexcept
    {
        return m_widths[index];
    }

private:
    static constexpr Size MaximumReads{8U};

    UInt64 m_values[MaximumReads]{};
    UInt64 m_addresses[MaximumReads]{};
    MemoryMappedIoWidth m_widths[MaximumReads]{};
    Size m_readCount{0U};
    Size m_failureIndex{MaximumReads};
    MemoryMappedIoReadError m_failure{MemoryMappedIoReadError::None};
};

[[nodiscard]] constexpr AhciHbaRegisterSnapshot MakeSentinel() noexcept
{
    return AhciHbaRegisterSnapshot{
        .Capabilities = 0x1111'1111U,
        .GlobalHostControl = 0x2222'2222U,
        .InterruptStatus = 0x3333'3333U,
        .PortsImplemented = 0x4444'4444U,
        .Version = 0x5555'5555U,
        .DecodedCapabilities =
            {
                .PortCount = 6U,
                .CommandSlotCount = 7U,
                .InterfaceSpeedSupport = 8U,
                .Supports64BitAddressing = true,
                .SupportsNativeCommandQueuing = true,
                .SupportsPortMultiplier = true,
                .SupportsStaggeredSpinUp = true,
                .SupportsAggressiveLinkPowerManagement = true,
            },
        .DecodedControl =
            {
                .AhciModeEnabled = true,
                .InterruptsEnabled = true,
                .ResetRequested = true,
                .Reserved = {1U, 2U, 3U, 4U, 5U},
            },
        .DecodedVersion =
            {
                .Major = 9U,
                .Minor = 0x0A0BU,
            },
    };
}

[[nodiscard]] constexpr bool IsSentinel(const AhciHbaRegisterSnapshot& snapshot) noexcept
{
    return (snapshot.Capabilities == 0x1111'1111U) && (snapshot.GlobalHostControl == 0x2222'2222U) &&
           (snapshot.InterruptStatus == 0x3333'3333U) && (snapshot.PortsImplemented == 0x4444'4444U) &&
           (snapshot.Version == 0x5555'5555U) && (snapshot.DecodedCapabilities.PortCount == 6U) &&
           (snapshot.DecodedCapabilities.CommandSlotCount == 7U) &&
           (snapshot.DecodedCapabilities.InterfaceSpeedSupport == 8U) &&
           snapshot.DecodedCapabilities.Supports64BitAddressing &&
           snapshot.DecodedCapabilities.SupportsNativeCommandQueuing &&
           snapshot.DecodedCapabilities.SupportsPortMultiplier &&
           snapshot.DecodedCapabilities.SupportsStaggeredSpinUp &&
           snapshot.DecodedCapabilities.SupportsAggressiveLinkPowerManagement &&
           snapshot.DecodedControl.AhciModeEnabled && snapshot.DecodedControl.InterruptsEnabled &&
           snapshot.DecodedControl.ResetRequested && (snapshot.DecodedControl.Reserved[0U] == 1U) &&
           (snapshot.DecodedControl.Reserved[1U] == 2U) && (snapshot.DecodedControl.Reserved[2U] == 3U) &&
           (snapshot.DecodedControl.Reserved[3U] == 4U) && (snapshot.DecodedControl.Reserved[4U] == 5U) &&
           (snapshot.DecodedVersion.Major == 9U) && (snapshot.DecodedVersion.Minor == 0x0A0BU);
}

[[nodiscard]] constexpr bool FailureReservedIsZero(const Kernel::Device::AhciHbaSnapshotFailure& failure) noexcept
{
    return (failure.Reserved[0U] == 0U) && (failure.Reserved[1U] == 0U) && (failure.Reserved[2U] == 0U) &&
           (failure.Reserved[3U] == 0U) && (failure.Reserved[4U] == 0U);
}

void SetSuccessfulValues(FakeMemoryMappedIo& memoryMappedIo) noexcept
{
    constexpr UInt64 capabilities =
        3U | (31U << 8U) | (3U << 20U) | (1U << 17U) | (1U << 26U) | (1U << 27U) | (1U << 30U) | (1ULL << 31U);
    memoryMappedIo.SetValue(0U, capabilities);
    memoryMappedIo.SetValue(1U, (1ULL << 31U) | (1U << 1U));
    memoryMappedIo.SetValue(2U, 0x0000'0005U);
    memoryMappedIo.SetValue(3U, 0x0000'000BU);
    memoryMappedIo.SetValue(4U, 0x0001'0301U);
}

void TestSuccessfulSnapshotAndReadSequence(TestContext& runner) noexcept
{
    constexpr UInt64 baseAddress{0x0000'0000'FEBF'0000ULL};
    FakeMemoryMappedIo memoryMappedIo;
    SetSuccessfulValues(memoryMappedIo);
    AhciHbaRegisterSnapshotReader reader{memoryMappedIo};
    AhciHbaRegisterSnapshot snapshot{};
    const auto result = reader.Read(
        AhciHbaRegisterRange{
            .BaseAddress = baseAddress,
            .LengthBytes = 0x14U,
        },
        snapshot);

    bool sequenceMatches = memoryMappedIo.GetReadCount() == 5U;
    for (Size index{0U}; index < 5U; ++index)
    {
        sequenceMatches = sequenceMatches &&
                          (memoryMappedIo.GetAddress(index) == (baseAddress + (static_cast<UInt64>(index) * 4U))) &&
                          (memoryMappedIo.GetWidth(index) == MemoryMappedIoWidth::DoubleWord);
    }
    ASTERIA_EXPECT(runner, "CAP, GHC, IS, PI, and VS are read once in architectural order",
                   result.Error == AhciHbaSnapshotError::None, sequenceMatches, snapshot.Capabilities == 0xCC32'1F03U,
                   snapshot.GlobalHostControl == 0x8000'0002U, snapshot.InterruptStatus == 5U,
                   snapshot.PortsImplemented == 0xBU, snapshot.Version == 0x0001'0301U);
    ASTERIA_EXPECT(
        runner, "the snapshot decodes stable baseline capability and control fields",
        snapshot.DecodedCapabilities.PortCount == 4U, snapshot.DecodedCapabilities.CommandSlotCount == 32U,
        snapshot.DecodedCapabilities.InterfaceSpeedSupport == 3U, snapshot.DecodedCapabilities.Supports64BitAddressing,
        snapshot.DecodedCapabilities.SupportsNativeCommandQueuing, snapshot.DecodedCapabilities.SupportsPortMultiplier,
        snapshot.DecodedCapabilities.SupportsStaggeredSpinUp,
        snapshot.DecodedCapabilities.SupportsAggressiveLinkPowerManagement, snapshot.DecodedControl.AhciModeEnabled,
        snapshot.DecodedControl.InterruptsEnabled, !snapshot.DecodedControl.ResetRequested,
        snapshot.DecodedVersion.Major == 1U, snapshot.DecodedVersion.Minor == 0x0301U);
}

void TestVersionFieldDiscriminator(TestContext& runner) noexcept
{
    FakeMemoryMappedIo memoryMappedIo;
    SetSuccessfulValues(memoryMappedIo);
    memoryMappedIo.SetValue(4U, 0x0003'0102U);
    AhciHbaRegisterSnapshotReader reader{memoryMappedIo};
    AhciHbaRegisterSnapshot snapshot{};
    const auto result = reader.Read(
        AhciHbaRegisterRange{
            .BaseAddress = 0x0000'0000'FEBF'0000ULL,
            .LengthBytes = 0x14U,
        },
        snapshot);

    ASTERIA_EXPECT(runner, "VS keeps its complete 16-bit BCD major and minor fields",
                   result.Error == AhciHbaSnapshotError::None, snapshot.Version == 0x0003'0102U,
                   snapshot.DecodedVersion.Major == 0x0003U, snapshot.DecodedVersion.Minor == 0x0102U,
                   memoryMappedIo.GetReadCount() == 5U);
}

void TestRangeValidation(TestContext& runner) noexcept
{
    FakeMemoryMappedIo memoryMappedIo;
    AhciHbaRegisterSnapshotReader reader{memoryMappedIo};

    AhciHbaRegisterSnapshot zeroBaseOutput = MakeSentinel();
    const Size readsBeforeZeroBase = memoryMappedIo.GetReadCount();
    const auto zeroBase = reader.Read(AhciHbaRegisterRange{.BaseAddress = 0U, .LengthBytes = 0x14U}, zeroBaseOutput);
    ASTERIA_EXPECT(runner, "a zero base fails before MMIO and preserves every output field",
                   zeroBase.Error == AhciHbaSnapshotError::InvalidRange,
                   zeroBase.MemoryMappedIoRead == MemoryMappedIoReadError::None,
                   zeroBase.Register == AhciHbaGlobalRegister::None, FailureReservedIsZero(zeroBase),
                   memoryMappedIo.GetReadCount() == readsBeforeZeroBase, IsSentinel(zeroBaseOutput));

    AhciHbaRegisterSnapshot misalignedOutput = MakeSentinel();
    const Size readsBeforeMisaligned = memoryMappedIo.GetReadCount();
    const auto misaligned =
        reader.Read(AhciHbaRegisterRange{.BaseAddress = 0x1002U, .LengthBytes = 0x14U}, misalignedOutput);
    ASTERIA_EXPECT(runner, "a misaligned range fails before MMIO and preserves every output field",
                   misaligned.Error == AhciHbaSnapshotError::InvalidRange,
                   misaligned.MemoryMappedIoRead == MemoryMappedIoReadError::None,
                   misaligned.Register == AhciHbaGlobalRegister::None, FailureReservedIsZero(misaligned),
                   memoryMappedIo.GetReadCount() == readsBeforeMisaligned, IsSentinel(misalignedOutput));

    AhciHbaRegisterSnapshot shortRangeOutput = MakeSentinel();
    const Size readsBeforeShortRange = memoryMappedIo.GetReadCount();
    const auto shortRange =
        reader.Read(AhciHbaRegisterRange{.BaseAddress = 0x1000U, .LengthBytes = 0x13U}, shortRangeOutput);
    ASTERIA_EXPECT(runner, "a short range fails before MMIO and preserves every output field",
                   shortRange.Error == AhciHbaSnapshotError::InvalidRange,
                   shortRange.MemoryMappedIoRead == MemoryMappedIoReadError::None,
                   shortRange.Register == AhciHbaGlobalRegister::None, FailureReservedIsZero(shortRange),
                   memoryMappedIo.GetReadCount() == readsBeforeShortRange, IsSentinel(shortRangeOutput));

    AhciHbaRegisterSnapshot overflowOutput = MakeSentinel();
    const Size readsBeforeOverflow = memoryMappedIo.GetReadCount();
    const auto overflow = reader.Read(
        AhciHbaRegisterRange{
            .BaseAddress = 0xFFFF'FFFF'FFFF'FFE0ULL,
            .LengthBytes = 0x20U,
        },
        overflowOutput);
    ASTERIA_EXPECT(runner, "an overflowing range fails before MMIO and preserves every output field",
                   overflow.Error == AhciHbaSnapshotError::InvalidRange,
                   overflow.MemoryMappedIoRead == MemoryMappedIoReadError::None,
                   overflow.Register == AhciHbaGlobalRegister::None, FailureReservedIsZero(overflow),
                   memoryMappedIo.GetReadCount() == readsBeforeOverflow, IsSentinel(overflowOutput));

    FakeMemoryMappedIo upperBoundaryMemoryMappedIo;
    AhciHbaRegisterSnapshotReader upperBoundaryReader{upperBoundaryMemoryMappedIo};
    AhciHbaRegisterSnapshot upperBoundaryOutput{};
    const auto upperBoundary = upperBoundaryReader.Read(
        AhciHbaRegisterRange{
            .BaseAddress = 0xFFFF'FFFF'FFFF'FFE0ULL,
            .LengthBytes = 0x1FU,
        },
        upperBoundaryOutput);
    ASTERIA_EXPECT(runner, "a half-open range whose representable exclusive end is UInt64 max is accepted",
                   upperBoundary.Error == AhciHbaSnapshotError::None, upperBoundaryMemoryMappedIo.GetReadCount() == 5U);
}

void TestMemoryMappedIoFailureAtomicity(TestContext& runner) noexcept
{
    constexpr AhciHbaGlobalRegister registers[5U]{
        AhciHbaGlobalRegister::Capabilities,    AhciHbaGlobalRegister::GlobalHostControl,
        AhciHbaGlobalRegister::InterruptStatus, AhciHbaGlobalRegister::PortsImplemented,
        AhciHbaGlobalRegister::Version,
    };
    for (Size failureIndex{0U}; failureIndex < 5U; ++failureIndex)
    {
        FakeMemoryMappedIo memoryMappedIo;
        SetSuccessfulValues(memoryMappedIo);
        memoryMappedIo.SetFailure(failureIndex, MemoryMappedIoReadError::HardwareFailure);
        AhciHbaRegisterSnapshotReader reader{memoryMappedIo};
        AhciHbaRegisterSnapshot output = MakeSentinel();
        const Size readsBefore = memoryMappedIo.GetReadCount();
        const auto failure = reader.Read(
            AhciHbaRegisterRange{
                .BaseAddress = 0x8000'0000U,
                .LengthBytes = 0x1000U,
            },
            output);
        ASTERIA_EXPECT(runner, "each MMIO failure stops at its register and preserves every output field",
                       failure.Error == AhciHbaSnapshotError::MemoryMappedIoReadFailed,
                       failure.MemoryMappedIoRead == MemoryMappedIoReadError::HardwareFailure,
                       failure.Register == registers[failureIndex], FailureReservedIsZero(failure),
                       memoryMappedIo.GetReadCount() == (readsBefore + failureIndex + 1U), IsSentinel(output));
    }
}

void TestInvalidBackendValue(TestContext& runner) noexcept
{
    constexpr AhciHbaGlobalRegister registers[5U]{
        AhciHbaGlobalRegister::Capabilities,    AhciHbaGlobalRegister::GlobalHostControl,
        AhciHbaGlobalRegister::InterruptStatus, AhciHbaGlobalRegister::PortsImplemented,
        AhciHbaGlobalRegister::Version,
    };
    for (Size failureIndex{0U}; failureIndex < 5U; ++failureIndex)
    {
        FakeMemoryMappedIo memoryMappedIo;
        SetSuccessfulValues(memoryMappedIo);
        memoryMappedIo.SetValue(failureIndex, 1ULL << 32U);
        AhciHbaRegisterSnapshotReader reader{memoryMappedIo};
        AhciHbaRegisterSnapshot output = MakeSentinel();
        const Size readsBefore = memoryMappedIo.GetReadCount();
        const auto failure = reader.Read(
            AhciHbaRegisterRange{
                .BaseAddress = 0x9000'0000U,
                .LengthBytes = 0x14U,
            },
            output);
        ASTERIA_EXPECT(runner, "each over-wide register value preserves every output field",
                       failure.Error == AhciHbaSnapshotError::InvalidRegisterValue,
                       failure.MemoryMappedIoRead == MemoryMappedIoReadError::None,
                       failure.Register == registers[failureIndex], FailureReservedIsZero(failure),
                       memoryMappedIo.GetReadCount() == (readsBefore + failureIndex + 1U), IsSentinel(output));
    }
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"AhciHbaRegisterSnapshotReaderTests"};
    Asteria::Test::TestSuccessfulSnapshotAndReadSequence(runner);
    Asteria::Test::TestVersionFieldDiscriminator(runner);
    Asteria::Test::TestRangeValidation(runner);
    Asteria::Test::TestMemoryMappedIoFailureAtomicity(runner);
    Asteria::Test::TestInvalidBackendValue(runner);
    return runner.Finish();
}
