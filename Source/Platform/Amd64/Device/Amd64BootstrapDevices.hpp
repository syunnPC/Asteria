// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_DEVICE_AMD64_BOOTSTRAP_DEVICES_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_DEVICE_AMD64_BOOTSTRAP_DEVICES_HPP_INCLUDED

#include "Abi/PagerBootstrap.hpp"
#include "Platform/Device/PciEcamRuntime.hpp"
#include "Platform/Amd64/Time/Amd64ApicTimer.hpp"
#include "Platform/Device/VtdTopology.hpp"

namespace Asteria::Platform::Amd64::Device
{
struct Amd64BootstrapDeviceInput
{
    const ::Asteria::Boot::BootInfo* BootInformation;
    Platform::Firmware::AhciPciResource Ahci;
    Kernel::Object::ObjectRegistry* Registry;
    Synchronization::SpinLock* RegistryLock;
    Kernel::Object::RootCapabilityIssuer* Issuer;
    Kernel::Resource::ResourceDomainObjectId ResourceIdentity;
    Kernel::Resource::ResourceDomain* Resources;
    Time::Amd64ApicTimer* Clock;
    Foundation::UInt64 RuntimePool;
    Foundation::UInt8 PhysicalAddressBits;
    /// The assignable device's captured BAR (a zero length for none), and where its published
    /// function and region are written.
    Platform::Firmware::AhciPciResource Device;
    Platform::Device::AssignableDevice* Assignable;
    /// Where the AHCI controller's function and ABAR region are written once the boot's own
    /// commands ran, disk or none; left untouched without a Ready controller.
    Platform::Device::AssignableDevice* AhciAssignable;
    /// The NIC's captured register BAR (a zero length for none) and where its published function
    /// and region are written.
    Platform::Firmware::AhciPciResource Nic;
    Platform::Device::AssignableDevice* NicAssignable;
    /// Four retained xHCI publications, in PCI catalog order; caller storage.
    Platform::Device::AssignableDevice* XhciAssignable;
    /// Four retained USB 1.1/2.0 host controller publications
    /// (Platform/Device/UsbHostPublication.hpp); caller storage.
    Platform::Device::AssignableDevice (*UsbHostAssignable)[Abi::PagerUsbHostControllers];
    /// Where the scanout's DeviceRegion is written once the boot's display (BootInfo::Display) is
    /// published; left untouched without one, or when the display overlaps RAM.
    Kernel::Object::ObjectId* ScanoutRegion;
    /// Where whether the AHCI controller holds no disk but the emulator's (no disk, or Kernel:
    /// Device::IsEmulatorDisk) is written once the boot's IDENTIFY ran; left untouched without a
    /// Ready controller.
    bool* AhciDiskWritable;
    /// Resolved before the bootstrap ECAM aperture closes. Caller-owned
    /// persistent plan and selection diagnostic, both required together.
    Platform::Device::VtdTopologyPlan* VtdPlan;
    Platform::Device::VtdTopologyError* VtdError;
    /// Sweep and stop all retained firmware units before IDENTIFY/READ DMA,
    /// including a refused topology or failed catalog preparation.
    void* HandoffContext;
    bool (*Handoff)(void*, Kernel::Device::PciEcamConfigurationSpace&) noexcept;
    /// Set by Handoff on a recoverable firmware-unit refusal. Withhold all
    /// kernel/user DMA devices and IDENTIFY; preserve PCI and scanout evidence.
    const bool* DmaRefused{nullptr};
};

/// One-shot BSP operation after firmware exit and before AP startup. Uses the
/// common capability/device/PCI/AHCI implementations with an AMD64 UC aperture.
/// Three reserved coherent WB RAM pages at pool offsets 225..227 remain owned
/// permanently, including every failure requiring a live DMA allocation.
/// Unrecoverable setup/command failures enter the terminal processor stop path.
[[nodiscard]] bool RunAmd64BootstrapDevices(const Amd64BootstrapDeviceInput& input) noexcept;

[[nodiscard]] bool ReserveAmd64FirmwareDma(Kernel::Device::PciFunctionAddress address) noexcept;

/// After RunAmd64BootstrapDevices: the addresses of the functions the ECAM
/// scan published, in catalog order. Copies at most capacity and returns the
/// complete count so callers must explicitly refuse truncation.
[[nodiscard]] Foundation::Size GetAmd64PciFunctions(Kernel::Device::PciFunctionAddress* output,
                                                    Foundation::Size capacity) noexcept;
} // Namespace Asteria::Platform::Amd64::Device
#endif
