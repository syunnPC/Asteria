// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_DEVICE_FIRMWARE_BUS_MASTER_GATE_HPP_INCLUDED
#define ASTERIA_KERNEL_DEVICE_FIRMWARE_BUS_MASTER_GATE_HPP_INCLUDED

#include "Hal/Device/IPciConfigurationSpace.hpp"
#include "Hal/Device/IPciConfigurationWriter.hpp"
#include "Kernel/Device/PciEcamConfigurationSpace.hpp"

namespace Asteria::Kernel::Device
{
enum class FirmwareMasterOutcome : Foundation::UInt8
{
    Confined,
    Cleared,
    Halted,
    Unconfined,
    Unreadable
};
struct FirmwareBusMasterAdmission
{
    Foundation::UInt64 Checked{0U};
    Foundation::UInt64 Mastering{0U};
    Foundation::UInt64 Unconfined{0U};
    bool Complete{false};
    [[nodiscard]] constexpr bool AllowsBootServices() const noexcept
    {
        return Complete && Unconfined == 0U;
    }
};

/// Confinement must mean a taken-over translating unit serves this requester with no BootServices
/// mappings. Trusted identity domains do not qualify. EHCI halt proof uses
/// USBCMD.RS=0/USBSTS.HCHalted=1 (EHCI 1.0 sections 2.3.1-2); a display retained for scanout is not
/// cleared.
class IFirmwareMasterProof
{
public:
    [[nodiscard]] virtual bool IsConfined(PciFunctionAddress address) const noexcept = 0;
    [[nodiscard]] virtual bool IsHaltedEhci(PciFunctionAddress address) const noexcept = 0;
    /// A live kernel-owned remapper may need its own untranslated queues.
    /// Preserving it denies BootServices admission until confinement is proven.
    [[nodiscard]] virtual bool KeepUnconfined(PciFunctionAddress) const noexcept
    {
        return false;
    }
    virtual void Report(PciFunctionAddress address, Foundation::UInt32 classCode,
                        FirmwareMasterOutcome outcome) noexcept = 0;

protected:
    constexpr IFirmwareMasterProof() noexcept = default;
    ~IFirmwareMasterProof() noexcept = default;
};

/// Bounded full configuration-space scan, independent of the published
/// catalog and previous sweeps. The caller supplies the complete windows;
/// no ECAM or a partial window inventory never proves admission (hp-i2000).
/// PCI Local Bus Specification rev. 3.0 section 6.2.2, Command.BME: write
/// only the command half of the dword, never echo W1C Status bits.
[[nodiscard]] inline FirmwareBusMasterAdmission
CheckFirmwareBusMasters(Hal::Device::IPciConfigurationSpace& pci, Hal::Device::IPciConfigurationWriter& writer,
                        const PciEcamRegionView windows, const bool complete, IFirmwareMasterProof& proof) noexcept
{
    using Foundation::UInt32;
    using Hal::Device::PciConfigurationReadError;
    FirmwareBusMasterAdmission result{};
    if (!complete || windows.Data == nullptr || windows.Count == 0U ||
        windows.Count > PciEcamConfigurationSpace::MaximumRegions)
    {
        return result;
    }
    result.Complete = true;
    for (Foundation::Size region{0U}; region < windows.Count; ++region)
    {
        const auto& window = windows.Data[region];
        if (window.FirstBus > window.LastBus)
        {
            result.Complete = false;
            continue;
        }
        for (Foundation::UInt16 bus{window.FirstBus}; bus <= window.LastBus; ++bus)
        {
            for (Foundation::UInt8 device{0U}; device < 32U; ++device)
            {
                for (Foundation::UInt8 function{0U}; function < 8U; ++function)
                {
                    const PciFunctionAddress address{window.Segment, static_cast<Foundation::UInt8>(bus), device,
                                                     function};
                    UInt32 identity{0U};
                    UInt32 command{0U};
                    UInt32 header{0U};
                    UInt32 kind{0U};
                    if (pci.Read32(address, 0U, identity) != PciConfigurationReadError::None)
                    {
                        result.Complete = false;
                        proof.Report(address, 0U, FirmwareMasterOutcome::Unreadable);
                        continue;
                    }
                    if ((identity & 0xFFFFU) == 0xFFFFU)
                    {
                        continue;
                    }
                    ++result.Checked;
                    if (pci.Read32(address, 4U, command) != PciConfigurationReadError::None || command == ~UInt32{0U} ||
                        pci.Read32(address, 0xCU, header) != PciConfigurationReadError::None || header == ~UInt32{0U} ||
                        pci.Read32(address, 8U, kind) != PciConfigurationReadError::None || kind == ~UInt32{0U})
                    {
                        result.Complete = false;
                        proof.Report(address, 0U, FirmwareMasterOutcome::Unreadable);
                        continue;
                    }
                    if ((command & 4U) == 0U || ((header >> 16U) & 0x7FU) == 1U)
                    {
                        continue;
                    }
                    ++result.Mastering;
                    const UInt32 classCode = kind >> 8U;
                    FirmwareMasterOutcome outcome{FirmwareMasterOutcome::Unconfined};
                    if (proof.IsConfined(address))
                    {
                        outcome = FirmwareMasterOutcome::Confined;
                    }
                    else if (classCode == 0x0C0320U && proof.IsHaltedEhci(address))
                    {
                        outcome = FirmwareMasterOutcome::Halted;
                    }
                    else if ((classCode >> 16U) != 3U && !proof.KeepUnconfined(address))
                    {
                        UInt32 verified{0U};
                        if (writer.Write32(address, 4U, (command & 0xFFFFU) & ~UInt32{4U}) ==
                                Hal::Device::PciConfigurationWriteError::None &&
                            pci.Read32(address, 4U, verified) == PciConfigurationReadError::None &&
                            verified != ~UInt32{0U} && (verified & 4U) == 0U)
                        {
                            outcome = FirmwareMasterOutcome::Cleared;
                        }
                    }
                    result.Unconfined += outcome == FirmwareMasterOutcome::Unconfined ? 1U : 0U;
                    proof.Report(address, classCode, outcome);
                }
            }
        }
    }
    return result;
}
} // Namespace Asteria::Kernel::Device
#endif
