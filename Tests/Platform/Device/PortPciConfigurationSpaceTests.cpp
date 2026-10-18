// SPDX-License-Identifier: Apache-2.0
#include "Platform/Device/PortPciConfigurationSpace.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt16;
using Foundation::UInt32;
using Hal::Device::PciConfigurationReadError;
using Hal::Device::PciConfigurationWriteError;
using Kernel::Device::PciFunctionAddress;
using Platform::Device::PortPciConfigurationSpace;

// Records every port access in order and answers reads of the data port.
class FakePorts final : public Hal::Device::IPortIo32
{
public:
    [[nodiscard]] UInt32 Read32(const UInt16 port) noexcept override
    {
        Record(port, 0U, false);
        return Answer;
    }

    void Write32(const UInt16 port, const UInt32 value) noexcept override
    {
        Record(port, value, true);
    }

    void Record(const UInt16 port, const UInt32 value, const bool write) noexcept
    {
        if (Count < 8U)
        {
            Ports[Count] = port;
            Values[Count] = value;
            Writes[Count] = write;
        }
        ++Count;
    }

    UInt32 Answer{0x11E8'1234U};
    UInt16 Ports[8U]{};
    UInt32 Values[8U]{};
    bool Writes[8U]{};
    Size Count{0U};
};

// The kernel's run-time configuration access on AMD64 selects a register through 0xCF8 and moves
// Its doubleword through 0xCFC, for segment 0 and the first 256 bytes only.
void TestMechanismOne(TestContext& runner) noexcept
{
    FakePorts ports;
    PortPciConfigurationSpace space{ports};
    const PciFunctionAddress edu{0U, 0U, 2U, 0U};
    UInt32 identifier{0U};
    const auto read = space.Read32(edu, 0x00U, identifier);
    const auto written = space.Write32(edu, 0x04U, 0x0000'0506U);
    ASTERIA_EXPECT(runner, "a read and a write each select the register, then move its doubleword",
                   read == PciConfigurationReadError::None, identifier == 0x11E8'1234U,
                   written == PciConfigurationWriteError::None, ports.Count == 4U, ports.Ports[0U] == 0x0CF8U,
                   ports.Values[0U] == 0x8000'1000U, ports.Writes[0U], ports.Ports[1U] == 0x0CFCU, !ports.Writes[1U],
                   ports.Ports[2U] == 0x0CF8U, ports.Values[2U] == 0x8000'1004U, ports.Ports[3U] == 0x0CFCU,
                   ports.Values[3U] == 0x0000'0506U, ports.Writes[3U],
                   PortPciConfigurationSpace::Encode(PciFunctionAddress{0U, 0xABU, 31U, 7U}, 0xFCU) == 0x80AB'FFFCU);

    UInt32 untouched{0x5A5AU};
    const auto extended = space.Read32(edu, 0x100U, untouched);
    const auto misaligned = space.Write32(edu, 0x06U, 0U);
    const auto segment = space.Write32(PciFunctionAddress{1U, 0U, 2U, 0U}, 0x04U, 0U);
    ASTERIA_EXPECT(runner, "extended space, other segments and misaligned registers touch no port",
                   extended == PciConfigurationReadError::Unsupported, untouched == 0x5A5AU,
                   misaligned == PciConfigurationWriteError::InvalidAddress,
                   segment == PciConfigurationWriteError::Unsupported, ports.Count == 4U);
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"PortPciConfigurationSpaceTests"};
    Asteria::Test::TestMechanismOne(runner);
    return runner.Finish();
}
