// SPDX-License-Identifier: Apache-2.0
#include "Kernel/Device/PciMessageSignaled.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt16;
using Foundation::UInt32;
using Kernel::Device::DisablePciMessages;
using Kernel::Device::EnablePciMessages;
using Kernel::Device::FindPciMessageCapability;
using Kernel::Device::PciFunctionAddress;
using Kernel::Device::PciMessageCapability;

/// One function's configuration space: dwords, the low half of every
/// capability header read-only, and every write logged in order.
class FakeSpace final : public Hal::Device::IPciConfigurationSpace, public Hal::Device::IPciConfigurationWriter
{
public:
    [[nodiscard]] Hal::Device::PciConfigurationReadError Read32(PciFunctionAddress, const UInt16 offset,
                                                                UInt32& output) noexcept override
    {
        output = Dwords[offset / 4U];
        return Hal::Device::PciConfigurationReadError::None;
    }

    [[nodiscard]] Hal::Device::PciConfigurationWriteError Write32(PciFunctionAddress, const UInt16 offset,
                                                                  const UInt32 value) noexcept override
    {
        const bool header = (offset == 0x40U) || (offset == 0x50U);
        Dwords[offset / 4U] = header ? ((Dwords[offset / 4U] & 0xFFFFU) | (value & 0xFFFF'0000U)) : value;
        if (Logged < 16U)
        {
            Offsets[Logged] = offset;
            ++Logged;
        }
        return Hal::Device::PciConfigurationWriteError::None;
    }

    UInt32 Dwords[64U]{};
    UInt16 Offsets[16U]{};
    Size Logged{0U};
};

// A power-management capability at 0x40, then edu's 64-bit MSI at 0x50
// With a multiple-message field the kernel clears.
void Prepare(FakeSpace& space, const UInt32 control) noexcept
{
    space.Dwords[0x04U / 4U] = 0x0010'0006U;
    space.Dwords[0x34U / 4U] = 0x40U;
    space.Dwords[0x40U / 4U] = 0x0003'5001U;
    space.Dwords[0x50U / 4U] = (control << 16U) | 0x0005U;
}

// The MSI capability is found through the list, programmed with INTx disabled before the enable
// bit, and disabled with INTx left off.
void TestMessages(TestContext& runner) noexcept
{
    FakeSpace space;
    Prepare(space, 0x00A0U);
    const PciFunctionAddress function{0U, 0U, 2U, 0U};
    PciMessageCapability capability{};
    const bool found = FindPciMessageCapability(space, function, capability);
    const bool enabled = EnablePciMessages(space, space, function, capability, 0xFEE0'1000U, 0x0041U);
    const Size enableWrites = space.Logged;
    const UInt16 lastBeforeDisable = space.Offsets[enableWrites - 1U];
    const UInt16 commandWrite = space.Offsets[enableWrites - 2U];
    const UInt32 control = space.Dwords[0x50U / 4U] >> 16U;
    const bool disabled = DisablePciMessages(space, space, function, capability);
    ASTERIA_EXPECT(runner, "a 64-bit MSI is programmed, INTx disabled before the enable bit, and disabled again", found,
                   capability.Offset == 0x50U, capability.Wide, !capability.PerVectorMask, enabled,
                   space.Dwords[0x54U / 4U] == 0xFEE0'1000U, space.Dwords[0x58U / 4U] == 0U,
                   space.Dwords[0x5CU / 4U] == 0x0041U, (control & 0x0071U) == 0x0001U, commandWrite == 0x04U,
                   lastBeforeDisable == 0x50U, (space.Dwords[0x04U / 4U] & 0x0400U) != 0U, disabled,
                   ((space.Dwords[0x50U / 4U] >> 16U) & 1U) == 0U, (space.Dwords[0x04U / 4U] & 0x0400U) != 0U,
                   (space.Dwords[0x50U / 4U] & 0xFFFFU) == 0x0005U);

    FakeSpace narrow;
    Prepare(narrow, 0x0100U);
    PciMessageCapability masked{};
    const bool narrowFound = FindPciMessageCapability(narrow, function, masked);
    narrow.Dwords[0x5CU / 4U] = 1U;
    const bool narrowEnabled = EnablePciMessages(narrow, narrow, function, masked, 0xFEE0'0000U, 0x0042U);
    ASTERIA_EXPECT(runner, "a 32-bit MSI with per-vector masking takes its data at +8 and its mask cleared",
                   narrowFound, !masked.Wide, masked.PerVectorMask, narrowEnabled, narrow.Dwords[0x58U / 4U] == 0x0042U,
                   narrow.Dwords[0x5CU / 4U] == 0U);

    FakeSpace none;
    Prepare(none, 0x0080U);
    none.Dwords[0x04U / 4U] = 0x0000'0006U;
    FakeSpace cycle;
    Prepare(cycle, 0x0080U);
    cycle.Dwords[0x50U / 4U] = 0x0080'4009U;
    cycle.Dwords[0x40U / 4U] = 0x0003'5001U;
    PciMessageCapability ignored{};
    ASTERIA_EXPECT(runner, "a function without a capability list or MSI has none, and a cyclic list ends",
                   !FindPciMessageCapability(none, function, ignored),
                   !FindPciMessageCapability(cycle, function, ignored));
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"PciMessageSignaledTests"};
    Asteria::Test::TestMessages(runner);
    return runner.Finish();
}
