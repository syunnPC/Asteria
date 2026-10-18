// SPDX-License-Identifier: Apache-2.0
#include "Kernel/Device/FirmwareBusMasterGate.hpp"
#include "Support/TestContext.hpp"
#include <initializer_list>

namespace
{
namespace Foundation = Asteria::Foundation;
namespace Hal = Asteria::Hal;
namespace Kernel = Asteria::Kernel;
namespace TestSupport = Asteria::TestSupport;
using Asteria::Kernel::Device::CheckFirmwareBusMasters;
using Asteria::Kernel::Device::FirmwareMasterOutcome;
using Asteria::Kernel::Device::IFirmwareMasterProof;
using Asteria::Kernel::Device::PciEcamRegion;
using Asteria::Kernel::Device::PciEcamRegionView;
using Asteria::Kernel::Device::PciFunctionAddress;
using Foundation::UInt32;
using Foundation::UInt16;
using Hal::Device::PciConfigurationReadError;
using Hal::Device::PciConfigurationWriteError;
class Configuration final : public Hal::Device::IPciConfigurationSpace,
                            public Hal::Device::IPciConfigurationWriter,
                            public IFirmwareMasterProof
{
public:
    [[nodiscard]] PciConfigurationReadError Read32(const PciFunctionAddress address, const UInt16 offset,
                                                   UInt32& value) noexcept override
    {
        ++Reads;
        if (Unreadable)
        {
            return PciConfigurationReadError::HardwareFailure;
        }
        if (address.GetRequesterId() >= 6U)
        {
            value = ~UInt32{0U};
            return PciConfigurationReadError::None;
        }
        const auto index = address.GetRequesterId();
        value = offset == 0U   ? 0x12348086U
                : offset == 4U ? Commands[index]
                : offset == 8U ? (index == 2U   ? 0x0C032000U
                                  : index == 3U ? 0x03000000U
                                                : 0x02000000U)
                : index == 4U  ? 0x00010000U
                               : 0U;
        return PciConfigurationReadError::None;
    }
    [[nodiscard]] PciConfigurationWriteError Write32(const PciFunctionAddress address, const UInt16 offset,
                                                     const UInt32 value) noexcept override
    {
        ++Writes;
        GoodWrites = GoodWrites && offset == 4U && (value >> 16U) == 0U;
        if (Refuse)
        {
            return PciConfigurationWriteError::HardwareFailure;
        }
        if (!Ignore)
        {
            Commands[address.GetRequesterId()] = value;
        }
        return PciConfigurationWriteError::None;
    }
    [[nodiscard]] bool IsConfined(const PciFunctionAddress address) const noexcept override
    {
        return address.GetRequesterId() == 0U;
    }
    [[nodiscard]] bool IsHaltedEhci(const PciFunctionAddress address) const noexcept override
    {
        return address.GetRequesterId() == 2U;
    }
    void Report(const PciFunctionAddress address, const UInt32, const FirmwareMasterOutcome outcome) noexcept override
    {
        if (address.GetRequesterId() < 6U)
        {
            Outcomes[address.GetRequesterId()] = outcome;
        }
    }
    UInt32 Commands[6U]{0xF9000006U, 6U, 6U, 6U, 6U, 2U};
    FirmwareMasterOutcome Outcomes[6U]{};
    Foundation::Size Reads{0U};
    Foundation::Size Writes{0U};
    bool Unreadable{false};
    bool Refuse{false};
    bool Ignore{false};
    bool GoodWrites{true};
};
} // namespace
int main()
{
    TestSupport::TestContext test{"FirmwareBusMasterGate"};
    const PciEcamRegion window{0x80000000U, 0U, 0U, 0U, {}};
    const PciEcamRegionView view{&window, 1U};
    Configuration pci;
    const auto blocked = CheckFirmwareBusMasters(pci, pci, view, true, pci);
    ASTERIA_EXPECT(
        test, "full scan classifies confined, cleared, halted and unconfined display masters; bridges forward",
        !blocked.AllowsBootServices(), blocked.Complete, blocked.Checked == 6U, blocked.Mastering == 4U,
        blocked.Unconfined == 1U, pci.Reads >= 256U, pci.Writes == 1U, pci.GoodWrites,
        pci.Outcomes[0U] == FirmwareMasterOutcome::Confined, pci.Outcomes[1U] == FirmwareMasterOutcome::Cleared,
        pci.Outcomes[2U] == FirmwareMasterOutcome::Halted, pci.Outcomes[3U] == FirmwareMasterOutcome::Unconfined,
        (pci.Commands[4U] & 4U) != 0U);
    pci.Commands[3U] = 2U;
    ASTERIA_CHECK(test, CheckFirmwareBusMasters(pci, pci, view, true, pci).AllowsBootServices());
    Configuration unreadable;
    unreadable.Unreadable = true;
    ASTERIA_EXPECT(test, "unreadable, absent or incomplete configuration cannot authorize BootServices reuse",
                   !CheckFirmwareBusMasters(unreadable, unreadable, view, true, unreadable).AllowsBootServices(),
                   !CheckFirmwareBusMasters(pci, pci, {}, true, pci).AllowsBootServices(),
                   !CheckFirmwareBusMasters(pci, pci, view, false, pci).AllowsBootServices());
    for (const bool ignored : {false, true})
    {
        Configuration failed;
        failed.Commands[3U] = 2U;
        failed.Refuse = !ignored;
        failed.Ignore = ignored;
        ASTERIA_CHECK(test, !CheckFirmwareBusMasters(failed, failed, view, true, failed).AllowsBootServices());
    }
    return test.Finish();
}
