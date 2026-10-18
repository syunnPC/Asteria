// SPDX-License-Identifier: Apache-2.0
#include "Platform/Ia64/System/Ia64PagerRuntime.hpp"
#include "Support/TestContext.hpp"

int main()
{
    namespace Abi = Asteria::Abi;
    namespace Foundation = Asteria::Foundation;
    namespace Kernel = Asteria::Kernel;
    namespace Platform = Asteria::Platform;
    namespace TestSupport = Asteria::TestSupport;
    TestSupport::TestContext test{"Ia64PagerDevicesTests"};
    static Platform::Ia64::System::Ia64PagerRuntimeInput input{};
    ASTERIA_EXPECT(test, "empty table has no assignable devices", !input.HasPublishedDevices());
    for (Foundation::Size slot{0U}; slot < Abi::PagerUsbHostControllers; ++slot)
    {
        input = {};
        input.UsbHostFunctions[slot] = Kernel::Object::ObjectId{20U};
        ASTERIA_EXPECT(test, "an incomplete USB publication grants no device authority", !input.HasPublishedDevices());
        input.UsbHostRegions[slot] = Kernel::Object::ObjectId{30U};
        ASTERIA_EXPECT(test, "every USB-only slot enables management and publishes the exact pair",
                       input.HasPublishedDevices(),
                       input.PublishedDevice(Abi::PagerDeviceUsbHost + slot).Function.GetValue() == 20U,
                       input.PublishedDevice(Abi::PagerDeviceUsbHost + slot).Region.GetValue() == 30U,
                       !input.PublishedDevice(Abi::PagerDeviceAhci).IsPublished(),
                       !input.PublishedDevice(Abi::PagerBootstrapDevices).IsPublished());
    }
    input = {};
    input.DeviceFunction = Kernel::Object::ObjectId{1U};
    input.DeviceRegion = Kernel::Object::ObjectId{2U};
    ASTERIA_EXPECT(test, "legacy EDU slot retains its published pair", input.HasPublishedDevices(),
                   input.PublishedDevice(Abi::PagerDeviceEdu).Region.GetValue() == 2U);
    input = {};
    input.AhciFunction = Kernel::Object::ObjectId{3U};
    input.AhciRegion = Kernel::Object::ObjectId{4U};
    ASTERIA_EXPECT(test, "legacy AHCI slot retains its published pair", input.HasPublishedDevices(),
                   input.PublishedDevice(Abi::PagerDeviceAhci).Region.GetValue() == 4U);
    input = {};
    input.NicFunction = Kernel::Object::ObjectId{5U};
    input.NicRegion = Kernel::Object::ObjectId{6U};
    ASTERIA_EXPECT(test, "legacy NIC slot retains its published pair", input.HasPublishedDevices(),
                   input.PublishedDevice(Abi::PagerDeviceNic).Region.GetValue() == 6U);
    return test.Finish();
}
