// SPDX-License-Identifier: Apache-2.0
#include "Kernel/Device/AhciEmulatorDisk.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Kernel::Device::AhciIdentifyDeviceInformation;
using Kernel::Device::IsEmulatorDisk;

// A device whose IDENTIFY model, as the parser trims it, is model.
[[nodiscard]] AhciIdentifyDeviceInformation WithModel(const char* const model) noexcept
{
    AhciIdentifyDeviceInformation device{};
    Foundation::UInt8 length{0U};
    while ((model[length] != '\0') && (length < 40U))
    {
        device.ModelNumber[length] = model[length];
        ++length;
    }
    device.ModelNumberLength = length;
    return device;
}

void TestModels(TestContext& runner) noexcept
{
    ASTERIA_EXPECT(runner, "QEMU's default disk and the runner's disk are the emulator's",
                   IsEmulatorDisk(WithModel("QEMU HARDDISK")), IsEmulatorDisk(WithModel("ASTERIA AHCI DISK")));
    AhciIdentifyDeviceInformation longer = WithModel("QEMU HARDDISK");
    longer.ModelNumberLength = 12U;
    ASTERIA_EXPECT(runner, "a real drive, a prefix, an extension or an empty model is not",
                   !IsEmulatorDisk(WithModel("Samsung SSD 860 EVO 500GB")), !IsEmulatorDisk(WithModel("QEMU DVD-ROM")),
                   !IsEmulatorDisk(WithModel("QEMU HARDDISK 2")), !IsEmulatorDisk(WithModel("ASTERIA AHCI DIS")),
                   !IsEmulatorDisk(WithModel("qemu harddisk")), !IsEmulatorDisk(WithModel("")),
                   !IsEmulatorDisk(longer));
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"AhciEmulatorDiskTests"};
    Asteria::Test::TestModels(runner);
    return runner.Finish();
}
