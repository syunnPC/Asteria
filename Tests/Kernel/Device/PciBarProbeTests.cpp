// SPDX-License-Identifier: Apache-2.0
#include "Kernel/Device/PciBarProbe.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt16;
using Foundation::UInt32;
using Kernel::Device::PciBarProbeError;
using Kernel::Device::PciFunctionAddress;

// One function's Command register and BAR 0, which decodes Size bytes: a
// write of all ones reads back as the size mask while memory decode is off.
class FakeFunction final : public Hal::Device::IPciConfigurationSpace, public Hal::Device::IPciConfigurationWriter
{
public:
    [[nodiscard]] Hal::Device::PciConfigurationReadError Read32(PciFunctionAddress, const UInt16 offset,
                                                                UInt32& output) noexcept override
    {
        output = (offset == 0x04U) ? Command : ((offset == 0x10U) ? Bar : ((offset == 0x14U) ? High : 0U));
        return Hal::Device::PciConfigurationReadError::None;
    }

    [[nodiscard]] Hal::Device::PciConfigurationWriteError Write32(PciFunctionAddress, const UInt16 offset,
                                                                  const UInt32 value) noexcept override
    {
        ++Writes;
        if (Refuse != 0U && Writes == Refuse)
        {
            return Hal::Device::PciConfigurationWriteError::HardwareFailure;
        }
        if (offset == 0x04U)
        {
            DecodedWhileSizing = DecodedWhileSizing || (((value & 2U) != 0U) && (Bar == ~UInt32{0U}));
            Command = (Command & 0xFFFF'0000U) | (value & 0xFFFFU);
        }
        else if (offset == 0x10U)
        {
            DecodedWhileSizing = DecodedWhileSizing || (((Command & 2U) != 0U) && (value == ~UInt32{0U}));
            Bar = (value == ~UInt32{0U}) ? (~(Size - 1U) | (Bar & 0xFU)) : value;
        }
        else if (offset == 0x14U)
        {
            High = (value == ~UInt32{0U}) ? ~UInt32{0U} : value;
        }
        return Hal::Device::PciConfigurationWriteError::None;
    }

    UInt32 High{0U};
    UInt32 Command{0x0010'0006U};
    UInt32 Bar{0xC120'0000U};
    UInt32 Size{0x0010'0000U};
    Foundation::Size Writes{0U};
    Foundation::Size Refuse{0U};
    bool DecodedWhileSizing{false};
};

// Edu's 1 MiB BAR at 0xC1200000 sizes with memory decode off and comes back with the Command
// register as it was.
void TestProbe(TestContext& runner) noexcept
{
    FakeFunction function;
    const auto probed = Kernel::Device::ProbePciMemoryBar32(function, function, PciFunctionAddress{0U, 0U, 0U, 0U}, 0U);
    ASTERIA_EXPECT(runner, "a 32-bit memory BAR sizes and is restored with the Command register",
                   probed.HasValue() && (probed.GetValue().BaseAddress == 0xC120'0000U),
                   probed.HasValue() && (probed.GetValue().LengthBytes == 0x0010'0000U), function.Bar == 0xC120'0000U,
                   function.Command == 0x0010'0006U, !function.DecodedWhileSizing, function.Writes == 4U);

    FakeFunction io;
    io.Bar = 0x0000'C001U;
    FakeFunction wide;
    wide.Bar = 0xC120'0004U;
    FakeFunction empty;
    empty.Bar = 0U;
    FakeFunction refused;
    refused.Refuse = 2U;
    const auto ioProbe = Kernel::Device::ProbePciMemoryBar32(io, io, PciFunctionAddress{0U, 0U, 0U, 0U}, 0U);
    const auto wideProbe = Kernel::Device::ProbePciMemoryBar32(wide, wide, PciFunctionAddress{0U, 0U, 0U, 0U}, 0U);
    const auto emptyProbe = Kernel::Device::ProbePciMemoryBar32(empty, empty, PciFunctionAddress{0U, 0U, 0U, 0U}, 0U);
    const auto refusedProbe =
        Kernel::Device::ProbePciMemoryBar32(refused, refused, PciFunctionAddress{0U, 0U, 0U, 0U}, 0U);
    const auto beyond = Kernel::Device::ProbePciMemoryBar32(function, function, PciFunctionAddress{0U, 0U, 0U, 0U}, 6U);
    ASTERIA_EXPECT(runner, "I/O, 64-bit and unassigned BARs are refused untouched; a failed write still restores",
                   !ioProbe.HasValue() && (ioProbe.GetError() == PciBarProbeError::NotMemory32), io.Writes == 0U,
                   !wideProbe.HasValue() && (wideProbe.GetError() == PciBarProbeError::NotMemory32),
                   !emptyProbe.HasValue() && (emptyProbe.GetError() == PciBarProbeError::Unassigned),
                   empty.Writes == 0U,
                   !refusedProbe.HasValue() && (refusedProbe.GetError() == PciBarProbeError::ConfigurationWriteFailed),
                   refused.Bar == 0xC120'0000U, refused.Command == 0x0010'0006U,
                   !beyond.HasValue() && (beyond.GetError() == PciBarProbeError::InvalidBarIndex));
}
void TestWideProbe(TestContext& runner) noexcept
{
    FakeFunction function;
    function.Command = 2U;
    function.Bar = 0xC120'0004U;
    function.High = 1U;
    const PciFunctionAddress address{};
    const auto wide = Kernel::Device::ProbePciMemoryBar(function, function, address, 0U);
    ASTERIA_EXPECT(runner, "64-bit BAR restores both halves and memory decode", wide.HasValue(),
                   wide.HasValue() && wide.GetValue().BaseAddress == 0x1'C120'0000U,
                   wide.HasValue() && wide.GetValue().LengthBytes == 0x100000U, function.Bar == 0xC120'0004U,
                   function.High == 1U, function.Command == 2U, !function.DecodedWhileSizing);
    function.Command = 6U;
    const auto running = Kernel::Device::ProbePciMemoryBar(function, function, address, 0U);
    ASTERIA_EXPECT(runner, "wide sizing refuses a bus-mastering function", !running.HasValue());
    function.Command = 2U;
    function.Refuse = function.Writes + 3U;
    const auto rejected = Kernel::Device::ProbePciMemoryBar(function, function, address, 0U);
    ASTERIA_EXPECT(runner, "failed upper probe restores both BAR halves and command", !rejected.HasValue(),
                   function.Bar == 0xC120'0004U, function.High == 1U, function.Command == 2U);
}

} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"PciBarProbeTests"};
    Asteria::Test::TestProbe(runner);
    Asteria::Test::TestWideProbe(runner);
    return runner.Finish();
}
