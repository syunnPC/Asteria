// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Kernel/Device/PciEcamAddress.hpp"
#include "Kernel/Device/PciTypes.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{

void TestPciEcamAddressContract(TestContext& runner) noexcept
{
    const Kernel::Device::PciEcamRegion region{
        .PhysicalStart = 0x4200'0000ULL,
        .Segment = 7U,
        .FirstBus = 32U,
        .LastBus = 63U,
        .Padding = {},
    };
    Kernel::Device::PciEcamAddressRequest request{
        .Region = region,
        .Address = Kernel::Device::PciFunctionAddress{7U, 32U, 3U, 2U},
        .RegisterOffset = 0x00FCU,
    };
    const auto firstAddress = Kernel::Device::PciEcamAddressCalculator::Calculate(request);
    ASTERIA_EXPECT(runner, "ECAM starts FirstBus at the generic region physical start", firstAddress.HasValue(),
                   firstAddress.GetValue() == 0x4201'A0FCULL);

    request.Address = Kernel::Device::PciFunctionAddress{7U, 63U, 31U, 7U};
    request.RegisterOffset = 0x0FFCU;
    const auto lastAddress = Kernel::Device::PciEcamAddressCalculator::Calculate(request);
    ASTERIA_EXPECT(runner, "the final aligned DWORD in an ECAM range is calculated exactly", lastAddress.HasValue(),
                   lastAddress.GetValue() == 0x43FF'FFFCULL);

    request.Address = Kernel::Device::PciFunctionAddress{8U, 32U, 0U, 0U};
    ASTERIA_EXPECT(runner, "an ECAM segment mismatch is rejected",
                   Kernel::Device::PciEcamAddressCalculator::Calculate(request).GetError() ==
                       Kernel::Device::PciEcamAddressError::AddressOutsideRegion);
    request.Address = Kernel::Device::PciFunctionAddress{7U, 31U, 0U, 0U};
    ASTERIA_EXPECT(runner, "a bus outside the allocation is rejected",
                   Kernel::Device::PciEcamAddressCalculator::Calculate(request).GetError() ==
                       Kernel::Device::PciEcamAddressError::AddressOutsideRegion);
    request.Address = Kernel::Device::PciFunctionAddress{7U, 32U, 32U, 0U};
    ASTERIA_EXPECT(runner, "an invalid PCI device number is rejected",
                   Kernel::Device::PciEcamAddressCalculator::Calculate(request).GetError() ==
                       Kernel::Device::PciEcamAddressError::InvalidFunctionAddress);
    request.Address = Kernel::Device::PciFunctionAddress{7U, 32U, 0U, 0U};
    request.RegisterOffset = 2U;
    ASTERIA_EXPECT(runner, "an unaligned ECAM DWORD offset is rejected",
                   Kernel::Device::PciEcamAddressCalculator::Calculate(request).GetError() ==
                       Kernel::Device::PciEcamAddressError::InvalidRegisterOffset);

    request.Region.PhysicalStart += 4096U;
    request.RegisterOffset = 0U;
    ASTERIA_EXPECT(runner, "an unaligned generic ECAM region is rejected",
                   Kernel::Device::PciEcamAddressCalculator::Calculate(request).GetError() ==
                       Kernel::Device::PciEcamAddressError::InvalidRegion);

    request.Region.PhysicalStart = 0xFFFF'FFFF'FFF0'0000ULL;
    request.Region.FirstBus = 0U;
    request.Region.LastBus = 1U;
    request.Address = Kernel::Device::PciFunctionAddress{7U, 0U, 0U, 0U};
    ASTERIA_EXPECT(runner, "an overflowing ECAM aperture is rejected before address addition",
                   Kernel::Device::PciEcamAddressCalculator::Calculate(request).GetError() ==
                       Kernel::Device::PciEcamAddressError::AddressOverflow);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"PciEcamAddressTests"};
    Asteria::Test::TestPciEcamAddressContract(runner);
    return runner.Finish();
}
