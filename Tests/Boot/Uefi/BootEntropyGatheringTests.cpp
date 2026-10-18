// SPDX-License-Identifier: Apache-2.0
// Gathering the boot entropy from firmware and processor sources before
// ExitBootServices (BootEntropyGathering): which firmware services are
// called, which sources end up credited, and the exact samples the seed
// extracts. The processor sources are link-time stand-ins.

#include <cstring>

#include "Boot/Uefi/BootEntropyGathering.hpp"
#include "Boot/Uefi/UefiTypes.hpp"
#include "Platform/Firmware/BootEntropySources.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Boot::BootEntropy;
using Boot::BootEntropySource;
using Boot::GetBootEntropySourceMask;
using Boot::Entropy::BootEntropyError;
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::UInt8;

constexpr UInt32 FirmwareRng{GetBootEntropySourceMask(BootEntropySource::FirmwareRng)};
constexpr UInt32 ProcessorRng{GetBootEntropySourceMask(BootEntropySource::ProcessorRng)};
constexpr UInt32 Jitter{GetBootEntropySourceMask(BootEntropySource::CycleCounterJitter)};
constexpr UInt32 FirmwareState{GetBootEntropySourceMask(BootEntropySource::FirmwareState)};

enum class CounterMode : UInt8
{
    Varied,
    Stuck,
};

enum class RngMode : UInt8
{
    Absent,
    Healthy,
    Failing,
    Zeros,
    // LocateProtocol fails but still writes an interface pointer.
    LocateFails,
};

struct Firmware
{
    Boot::Uefi::SystemTable System;
    Boot::Uefi::BootServices Services;
    Boot::Uefi::RuntimeServices Runtime;
    Boot::Uefi::RngProtocol Protocol;
    RngMode RngBehavior;
    bool ProcessorRandom;
    CounterMode Counter;
    UInt64 CounterValue;
    UInt64 CounterReadings;
    UInt64 ProcessorReadings;
    Size LocateCalls;
    Size GetRngCalls;
    Size GetTimeCalls;
    Size MonotonicCalls;
    bool GetRngArgumentsExact;
};

constinit Firmware g_firmware{};

Boot::Uefi::Status LocateProtocol(Boot::Uefi::Guid* const protocol, void* const registration,
                                  void** const interface) noexcept
{
    ++g_firmware.LocateCalls;
    constexpr UInt8 RngGuidTail[8U]{0x86U, 0x2EU, 0xC0U, 0x1CU, 0xDCU, 0x29U, 0x1FU, 0x44U};
    const bool isRng = (protocol->Data1 == 0x3152'BCA5U) && (protocol->Data2 == 0xEADEU) &&
                       (protocol->Data3 == 0x433DU) && (std::memcmp(protocol->Data4, RngGuidTail, 8U) == 0);
    if (!isRng || (registration != nullptr) || (g_firmware.RngBehavior == RngMode::Absent))
    {
        *interface = nullptr;
        return Boot::Uefi::NotFound;
    }
    *interface = &g_firmware.Protocol;
    return g_firmware.RngBehavior == RngMode::LocateFails ? Boot::Uefi::DeviceError : Boot::Uefi::Success;
}

Boot::Uefi::Status GetRng(Boot::Uefi::RngProtocol* const self, Boot::Uefi::Guid* const algorithm, const Size sizeBytes,
                          UInt8* const bytes) noexcept
{
    ++g_firmware.GetRngCalls;
    g_firmware.GetRngArgumentsExact = g_firmware.GetRngArgumentsExact && (self == &g_firmware.Protocol) &&
                                      (algorithm == nullptr) && (sizeBytes == Boot::Uefi::FirmwareRngBlockBytes);
    if (g_firmware.RngBehavior == RngMode::Failing)
    {
        return Boot::Uefi::DeviceError;
    }
    for (Size index{0U}; index < sizeBytes; ++index)
    {
        bytes[index] = g_firmware.RngBehavior == RngMode::Zeros
                           ? UInt8{0U}
                           : static_cast<UInt8>((g_firmware.GetRngCalls * 97U) + (index * 13U));
    }
    return Boot::Uefi::Success;
}

Boot::Uefi::Status GetTime(Boot::Uefi::Time* const time, void* const capabilities) noexcept
{
    ++g_firmware.GetTimeCalls;
    *time = Boot::Uefi::Time{2026U, 9U, 30U, 12U, 34U, 56U, 0U, 789U, 0, 0U, 0U};
    return capabilities == nullptr ? Boot::Uefi::Success : Boot::Uefi::InvalidParameter;
}

Boot::Uefi::Status GetNextMonotonicCount(UInt64* const count) noexcept
{
    ++g_firmware.MonotonicCalls;
    *count = 0x0000'0007'0000'0001ULL;
    return Boot::Uefi::Success;
}

// Firmware with every service; each test withdraws what it does not want.
void SetUp(const RngMode rng, const bool processorRandom, const CounterMode counter) noexcept
{
    g_firmware = Firmware{};
    g_firmware.Services.LocateProtocolFunction = LocateProtocol;
    g_firmware.Services.GetNextMonotonicCountFunction = GetNextMonotonicCount;
    g_firmware.Runtime.Header =
        Boot::Uefi::TableHeader{Boot::Uefi::RuntimeServicesSignature, 0x0002'0000U,
                                static_cast<UInt32>(sizeof(Boot::Uefi::RuntimeServices)), 0U, 0U};
    g_firmware.Runtime.GetTimeFunction = GetTime;
    g_firmware.Protocol.GetRng = GetRng;
    g_firmware.System.BootServicesTable = &g_firmware.Services;
    g_firmware.System.RuntimeServices = &g_firmware.Runtime;
    g_firmware.RngBehavior = rng;
    g_firmware.ProcessorRandom = processorRandom;
    g_firmware.Counter = counter;
    g_firmware.GetRngArgumentsExact = true;
}

[[nodiscard]] Foundation::Result<BootEntropy, BootEntropyError> Gather() noexcept
{
    return Boot::Uefi::GatherBootEntropy(g_firmware.System);
}

[[nodiscard]] UInt32 SourcesOf(const Foundation::Result<BootEntropy, BootEntropyError>& result) noexcept
{
    return result.HasValue() ? result.GetValue().Sources : 0U;
}

void TestEverySourceIsGathered(TestContext& runner) noexcept
{
    SetUp(RngMode::Healthy, true, CounterMode::Varied);
    const auto result = Gather();
    ASTERIA_EXPECT(runner, "a firmware RNG, a processor generator and a running counter are all credited",
                   SourcesOf(result) == (FirmwareRng | ProcessorRng | Jitter | FirmwareState),
                   g_firmware.LocateCalls == 1U, g_firmware.GetRngCalls == Boot::Uefi::FirmwareRngBlockCount,
                   g_firmware.GetRngArgumentsExact,
                   g_firmware.ProcessorReadings == Boot::Uefi::ProcessorRandomValueCount,
                   g_firmware.CounterReadings == Boot::Uefi::CycleCounterReadingCount, g_firmware.GetTimeCalls == 1U,
                   g_firmware.MonotonicCalls == 1U);
}

void TestSeedExtraction(TestContext& runner) noexcept
{
    // HKDF-Extract of the framed samples in gathering order (two RNG blocks,
    // eight processor values, the time of day and the monotonic count, then
    // 2,049 counter readings), computed by an independent implementation.
    constexpr UInt8 Expected[Boot::BootEntropySeedBytes]{
        0x38U, 0x45U, 0x23U, 0x6EU, 0x75U, 0x03U, 0x36U, 0x85U, 0xAAU, 0xFEU, 0x77U, 0xB5U, 0xBDU, 0x70U, 0xAEU, 0x84U,
        0xA0U, 0xCFU, 0x0AU, 0xAAU, 0x7DU, 0x0CU, 0xE2U, 0x9EU, 0x89U, 0x6AU, 0x4BU, 0x86U, 0xABU, 0x43U, 0x87U, 0xF3U,
    };
    SetUp(RngMode::Healthy, true, CounterMode::Varied);
    const auto result = Gather();
    ASTERIA_EXPECT(runner, "the seed extracts exactly the gathered samples, framed in gathering order",
                   result.HasValue(), std::memcmp(result.GetValue().Seed, Expected, sizeof(Expected)) == 0);
}

void TestMissingOrBrokenSources(TestContext& runner) noexcept
{
    SetUp(RngMode::Absent, false, CounterMode::Varied);
    ASTERIA_EXPECT(runner, "without generators the jitter source alone qualifies the seed",
                   SourcesOf(Gather()) == (Jitter | FirmwareState), g_firmware.GetRngCalls == 0U);

    SetUp(RngMode::Failing, false, CounterMode::Varied);
    ASTERIA_EXPECT(runner, "a failing GetRNG credits nothing", SourcesOf(Gather()) == (Jitter | FirmwareState));

    SetUp(RngMode::LocateFails, false, CounterMode::Varied);
    ASTERIA_EXPECT(runner, "an interface returned with a failing LocateProtocol is not used",
                   SourcesOf(Gather()) == (Jitter | FirmwareState), g_firmware.GetRngCalls == 0U);

    SetUp(RngMode::Zeros, false, CounterMode::Varied);
    ASTERIA_EXPECT(runner, "an RNG that returns zeros fails its health test",
                   SourcesOf(Gather()) == (Jitter | FirmwareState));

    SetUp(RngMode::Healthy, false, CounterMode::Stuck);
    ASTERIA_EXPECT(runner, "a stopped counter is not credited, the firmware RNG still is",
                   SourcesOf(Gather()) == (FirmwareRng | FirmwareState));

    SetUp(RngMode::Zeros, false, CounterMode::Stuck);
    const auto nothing = Gather();
    ASTERIA_EXPECT(runner, "without any credited source no seed is published", !nothing.HasValue(),
                   nothing.GetError() == BootEntropyError::NoHealthySource);

    SetUp(RngMode::Absent, false, CounterMode::Varied);
    g_firmware.Services.LocateProtocolFunction = nullptr;
    g_firmware.Services.GetNextMonotonicCountFunction = nullptr;
    g_firmware.Runtime.Header.HeaderSize =
        static_cast<UInt32>(__builtin_offsetof(Boot::Uefi::RuntimeServices, GetTimeFunction));
    ASTERIA_EXPECT(runner, "absent services and a runtime table too short for GetTime are not called",
                   SourcesOf(Gather()) == Jitter, g_firmware.GetTimeCalls == 0U);

    SetUp(RngMode::Absent, false, CounterMode::Varied);
    g_firmware.Runtime.Header.Signature ^= 1U;
    g_firmware.System.BootServicesTable = nullptr;
    ASTERIA_EXPECT(runner, "a runtime table with a foreign signature and a missing boot table are not used",
                   SourcesOf(Gather()) == Jitter, g_firmware.GetTimeCalls == 0U, g_firmware.LocateCalls == 0U);
}

} // namespace
} // namespace Asteria::Test

namespace Asteria::Platform::Firmware
{
Foundation::UInt64 ReadCycleCounter() noexcept
{
    auto& firmware = Test::g_firmware;
    ++firmware.CounterReadings;
    if (firmware.Counter == Test::CounterMode::Varied)
    {
        firmware.CounterValue += 100U + ((firmware.CounterReadings * 7919U) % 997U);
    }
    return firmware.CounterValue;
}

bool TryReadProcessorRandom(Foundation::UInt64& value) noexcept
{
    auto& firmware = Test::g_firmware;
    if (!firmware.ProcessorRandom)
    {
        value = 0U;
        return false;
    }
    ++firmware.ProcessorReadings;
    value = 0x9E37'79B9'7F4A'7C15ULL * firmware.ProcessorReadings;
    return true;
}
} // namespace Asteria::Platform::Firmware

int main()
{
    TestContext runner{"BootEntropyGatheringTests"};
    Asteria::Test::TestEverySourceIsGathered(runner);
    Asteria::Test::TestSeedExtraction(runner);
    Asteria::Test::TestMissingOrBrokenSources(runner);
    return runner.Finish();
}
