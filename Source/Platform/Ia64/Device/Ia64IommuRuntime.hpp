// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_DEVICE_IA64_IOMMU_RUNTIME_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_DEVICE_IA64_IOMMU_RUNTIME_HPP_INCLUDED

#include "Kernel/Device/PciEcamConfigurationSpace.hpp"
#include "Kernel/Device/IommuDomainPool.hpp"
#include "Platform/Device/TrustedDmaRuntime.hpp"
#include "Platform/Device/VirtioIommuDomainBackend.hpp"
#include "Platform/Ia64/Device/Ia64PhysicalMemoryAccess.hpp"
#include "Platform/Ia64/Device/Ia64VtdRuntime.hpp"

namespace Asteria::Kernel::System
{
class ObjectManager;
}
namespace Asteria::Platform::Device
{
class PciEcamRuntime;
}

namespace Asteria::Platform::Ia64::Device
{
/// Bootstrap-owned modern virtio BAR apertures. Transactions use the existing
/// uncached physical corridor, so these kernel-only apertures consume no TRs.
class Ia64VirtioMmio final : public Hal::Device::IMemoryMappedIo, public Hal::Device::IMemoryMappedIoWriter
{
public:
    explicit Ia64VirtioMmio(const Ia64PhysicalMemoryAccess& access) noexcept : m_access{access}
    {
    }
    /// Boot only with decode, bus mastering and INTx disabled, before any
    /// consumer uses these BARs. Failure retains the owner until fail-stop.
    [[nodiscard]] bool Discover(Hal::Device::IPciConfigurationSpace& pci, Hal::Device::IPciConfigurationWriter& writer,
                                Kernel::Device::PciFunctionAddress function, Foundation::UInt8 physicalBits) noexcept;
    [[nodiscard]] const Platform::Device::VirtioPciBar (&GetBars() const noexcept)[6U]
    {
        return m_bars;
    }
    [[nodiscard]] Hal::Device::MemoryMappedIoReadError Read(Foundation::UInt64 address,
                                                            Hal::Device::MemoryMappedIoWidth width,
                                                            Foundation::UInt64& output) noexcept override;
    [[nodiscard]] Hal::Device::MemoryMappedIoWriteError Write(Foundation::UInt64 address,
                                                              Hal::Device::MemoryMappedIoWidth width,
                                                              Foundation::UInt64 value) noexcept override;

private:
    [[nodiscard]] bool Contains(Foundation::UInt64 address, Foundation::UInt64 bytes) const noexcept;
    const Ia64PhysicalMemoryAccess& m_access;
    Platform::Device::VirtioPciBar m_bars[6U]{};
};

/// Immovable BSP boot lifetime. Constructed only after transfer to the aligned image copy; storage
/// resides in its reserved, identity-mapped WB writable section. Its coherent queue pages and VT-d
/// table pages never enter the Frame allocator and remain pinned even on initialization failure.
/// Lifecycle and callers are serialized on the BSP with interrupts disabled. The assignable
/// devices, edu, the AHCI controller and the NIC, are granted DMA authority through it.
/// Backend choice (DMA isolation, as AMD64 chooses VT-d before AMD-Vi): Intel VT-d when the DMAR
/// names a remapping unit, else the virtio-iommu function 1af4:1057 on segment 0, bus 0, else none.
/// A VT-d topology or handoff refusal leaves the boot without user DMA; it never falls back to
/// another backend.
class Ia64IommuRuntime final
{
public:
    Ia64IommuRuntime(Kernel::Device::PciEcamConfigurationSpace& pci, const Ia64PhysicalMemoryAccess& access) noexcept;
    /// Before the boot's first DMA: VT-d's firmware handoff where the DMAR names a remapping unit
    /// (Ia64VtdRuntime::HandOff); nothing to do for virtio-iommu, whose boot bypass the firmware's
    /// DMA needs until Initialize.
    [[nodiscard]] Ia64VtdHandoff HandOff(Platform::Device::PciEcamRuntime& runtime, const Ia64VtdInput& input,
                                         const Kernel::Device::PciFunctionAddress* bootStorage) noexcept;
    [[nodiscard]] bool Initialize(Platform::Device::PciEcamRuntime& runtime, const Ia64VtdInput& input) noexcept;
    /// Working when Initialize configured VT-d or virtio-iommu domains, Refused when the boot found
    /// one of them and could not use it, Absent otherwise.
    [[nodiscard]] Kernel::Device::IommuAvailability GetAvailability() const noexcept;
    /// After Initialize: the BOOT.CFG trust allowlist for the published DMA functions (Platform:
    /// Device::TrustedDmaRuntime); without a usable IOMMU and with a trusted function, Configure
    /// gives the pool identity backends.
    void ApplyTrustedDma(Kernel::Object::ObjectRegistry& registry, const Boot::BootInfo& information,
                         const Platform::Device::TrustedDmaCandidate* candidates, Foundation::Size count) noexcept;
    [[nodiscard]] bool Configure(Kernel::System::ObjectManager& objects) noexcept;
    void Record(const Kernel::System::ObjectManager& objects) noexcept;
    [[nodiscard]] const Platform::Device::VtdHandoffRecord* GetHandoffRecords() const noexcept
    {
        return m_vtd.GetHandoffRecords();
    }
    [[nodiscard]] bool IsConfined(Kernel::Device::PciFunctionAddress address) const noexcept
    {
        return m_vtdSelected && m_vtd.IsConfined(address);
    }
    [[nodiscard]] bool IsQueueFunction(Kernel::Device::PciFunctionAddress address) const noexcept
    {
        return m_present && m_queueFunction.IsEqualTo(address);
    }

private:
    [[nodiscard]] bool InitializeVirtio(Platform::Device::PciEcamRuntime& runtime,
                                        Foundation::UInt8 physicalBits) noexcept;
    [[nodiscard]] bool Start(Kernel::Device::PciFunctionAddress function, Foundation::UInt8 physicalBits) noexcept;
    [[nodiscard]] Platform::Device::VirtioDmaPage Page(Foundation::Size index) noexcept;
    Kernel::Device::PciEcamConfigurationSpace& m_pci;
    Ia64VirtioMmio m_mmio;
    Platform::Device::VirtioPciTransport m_transport;
    Platform::Device::VirtioIommuDevice m_device;
    alignas(Platform::Device::VirtioIommuDomainBackend)
        Foundation::UInt8 m_backendStorage[Kernel::Device::IommuDomainPool::Capacity]
                                          [sizeof(Platform::Device::VirtioIommuDomainBackend)]{};
    Hal::Device::IIommuDomainBackend* m_backends[Kernel::Device::IommuDomainPool::Capacity]{};
    Kernel::Device::IommuDomainSlot m_slots[Kernel::Device::IommuDomainPool::Capacity]{};
    alignas(4096) Foundation::UInt8 m_pages[8U][4096U]{};
    Kernel::Device::PciFunctionAddress m_queueFunction{};
    bool m_present{false};
    bool m_vtdSelected{false};
    Ia64VtdRuntime m_vtd;
    Platform::Device::TrustedDmaRuntime m_trusted{};
};
} // Namespace Asteria::Platform::Ia64::Device
#endif // ASTERIA_PLATFORM_IA64_DEVICE_IA64_IOMMU_RUNTIME_HPP_INCLUDED
