// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_DEVICE_IA64_VTD_RUNTIME_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_DEVICE_IA64_VTD_RUNTIME_HPP_INCLUDED

#include "Boot/BootInfo.hpp"
#include "Kernel/Device/IommuDomainPool.hpp"
#include "Kernel/Device/PciEcamConfigurationSpace.hpp"
#include "Platform/Device/VtdDomainBackend.hpp"
#include "Platform/Device/VtdDomainGroup.hpp"
#include "Platform/Device/VtdTopology.hpp"
#include "Platform/Device/VtdUnit.hpp"
#include "Platform/Ia64/Device/Ia64VtdHardware.hpp"

namespace Asteria::Platform::Device
{
class PciEcamRuntime;
}

namespace Asteria::Platform::Ia64::Device
{
/// What the IA-64 VT-d boot needs beyond the PCI runtime.
struct Ia64VtdInput
{
    const Boot::BootInfo* BootInformation;
    /// The firmware-queue policy's kernel ranges: the executing image copy
    /// (PoolStart/PoolPages) and the run-time memory reservation
    /// (StartupStart/StartupPages).
    Platform::Device::VtdFirmwareMemoryOwnership Ownership;
    /// PAL_VM_SUMMARY's implemented physical-address width.
    Foundation::UInt8 PhysicalAddressBits;
    Foundation::UInt8 Reserved[7];
};

/// How the firmware handoff before the boot's first AHCI DMA ended.
enum class Ia64VtdHandoff : Foundation::UInt8
{
    /// Every unit is taken over and untranslated: the boot's own DMA may run.
    Ready,
    /// A unit could not be taken over (or not addressed): the boot runs no
    /// DMA of its own and later grants none, as AMD64 does.
    DmaRefused,
    /// An endpoint could not be stopped: the boot must stop.
    Failed,
};

/// Intel VT-d on IA-64: the ISA-neutral topology, unit, domain backend and group behind the IA-64
/// adapters. One hub's two units at most; their root, context and domain tables are pages of this
/// object, which the boot constructs in the executing image copy (identity-mapped, write-back), so
/// every table page's address is its physical address and needs no new translation register.
/// Interrupt remapping stays off: a confined device can still write the processor interrupt block
/// until IA-64 interrupt remapping exists (a recorded weakness). RMRRs are refused: IA-64 has no
/// firmware DMA domains yet. Immovable BSP boot lifetime; HandOff runs once on the BSP before the
/// boot's storage diagnostic and Initialize once after it, both with interrupts disabled.
class Ia64VtdRuntime final
{
public:
    static constexpr Foundation::Size MaximumUnits{2U};
    static constexpr Foundation::Size UnitTablePages{1U + Platform::Device::VtdUnit::MaximumBuses};
    static constexpr Foundation::Size DomainTablePages{Kernel::Device::IommuDomainPool::Capacity *
                                                       Platform::Device::VtdDomainBackend::TablePages};
    static constexpr Foundation::Size PagesPerUnit{UnitTablePages + DomainTablePages};

    Ia64VtdRuntime(Kernel::Device::PciEcamConfigurationSpace& pci, const Ia64PhysicalMemoryAccess& access) noexcept;
    Ia64VtdRuntime(const Ia64VtdRuntime&) = delete;
    Ia64VtdRuntime& operator=(const Ia64VtdRuntime&) = delete;

    /// A DMAR that names at least one remapping unit makes VT-d the backend,
    /// whether or not its topology is then accepted (as on AMD64).
    [[nodiscard]] static bool IsDescribed(const Boot::Acpi::AcpiTopology& topology) noexcept;

    /// Before the boot's first DMA (its AHCI diagnostic), as AMD64's handoff:
    /// sweeps every published ordinary endpoint master-off except the boot's
    /// own storage controller (`bootStorage`, null for none), which keeps the
    /// bus mastering the firmware left for the diagnostic, then takes every
    /// DRHD's firmware state over (printed before any write) and leaves the
    /// unit untranslated. Allocation-free; changes no table.
    [[nodiscard]] Ia64VtdHandoff HandOff(Platform::Device::PciEcamRuntime& runtime, const Ia64VtdInput& input,
                                         const Kernel::Device::PciFunctionAddress* bootStorage) noexcept;

    /// After the diagnostic: sweeps every published ordinary endpoint
    /// master-off, takes over any unit HandOff did not, configures and enables
    /// the accepted topology. A refusal before any unit translates leaves the
    /// boot without user DMA and returns true; false (the boot stops) means an
    /// endpoint could not be stopped or a unit translates after a failure.
    [[nodiscard]] bool Initialize(Platform::Device::PciEcamRuntime& runtime, const Ia64VtdInput& input) noexcept;
    [[nodiscard]] bool IsReady() const noexcept
    {
        return m_ready;
    }
    /// One VtdDomainGroup per IommuDomain slot over the translating units.
    [[nodiscard]] Hal::Device::IIommuDomainBackend* const (
        &GetBackends() const noexcept)[Kernel::Device::IommuDomainPool::Capacity]
    {
        return m_backends;
    }
    [[nodiscard]] bool IsConfined(const Kernel::Device::PciFunctionAddress address) const noexcept
    {
        if (!m_ready)
        {
            return false;
        }
        for (Foundation::Size i{0U}; i < m_plan.Count; ++i)
        {
            const auto& route = m_plan.Functions[i];
            if (route.Address.IsEqualTo(address))
            {
                return route.Unit < MaximumUnits && m_units[route.Unit] != nullptr && m_units[route.Unit]->IsEnabled();
            }
        }
        return false;
    }
    /// Polls every live unit and publishes the unit facts and the last fault.
    void Record() noexcept;
    [[nodiscard]] const Platform::Device::VtdHandoffRecord* GetHandoffRecords() const noexcept
    {
        return m_handoffRecords;
    }

private:
    [[nodiscard]] Platform::Device::VtdTablePage Page(Foundation::Size unit, Foundation::Size page) noexcept;
    void Prepare(const Ia64VtdInput& input) noexcept;
    [[nodiscard]] bool Sweep(Platform::Device::PciEcamRuntime& runtime,
                             const Kernel::Device::PciFunctionAddress* exempt, bool count) noexcept;
    [[nodiscard]] bool RegisterSetIsValid(Foundation::Size unit) const noexcept;
    void Bind(Foundation::Size unit, const Ia64VtdInput& input) noexcept;
    void TakeOverAll(const Ia64VtdInput& input) noexcept;
    [[nodiscard]] bool ConfigureUnit(Foundation::Size unit) noexcept;
    [[nodiscard]] bool Refuse(bool enabledAny) noexcept;
    void BuildBackends() noexcept;

    Kernel::Device::PciEcamConfigurationSpace& m_pci;
    const Ia64PhysicalMemoryAccess& m_access;
    const Boot::Acpi::AcpiTopology* m_topology{nullptr};
    Ia64VtdRegisters m_registers[MaximumUnits];
    Ia64VtdCacheMaintenance m_cache[MaximumUnits]{};
    alignas(Platform::Device::VtdUnit)
        Foundation::UInt8 m_unitStorage[MaximumUnits][sizeof(Platform::Device::VtdUnit)]{};
    Platform::Device::VtdUnit* m_units[MaximumUnits]{};
    alignas(Platform::Device::VtdDomainBackend)
        Foundation::UInt8 m_backendStorage[Kernel::Device::IommuDomainPool::Capacity][MaximumUnits]
                                          [sizeof(Platform::Device::VtdDomainBackend)]{};
    alignas(Platform::Device::VtdDomainGroup) Foundation::UInt8
        m_groupStorage[Kernel::Device::IommuDomainPool::Capacity][sizeof(Platform::Device::VtdDomainGroup)]{};
    Hal::Device::IIommuDomainBackend* m_backends[Kernel::Device::IommuDomainPool::Capacity]{};
    Platform::Device::VtdTopologyPlan m_plan{};
    Kernel::Device::PciFunctionAddress m_functions[Platform::Device::VtdUnit::MaximumRequesters]{};
    Foundation::UInt16 m_requesters[Platform::Device::VtdUnit::MaximumRequesters]{};
    Platform::Device::VtdTopologyError m_error{Platform::Device::VtdTopologyError::NoDmar};
    Foundation::UInt8 m_addressBits{0U};
    Foundation::Size m_unitCount{0U};
    Platform::Device::VtdHandoffRecord m_handoffRecords[Boot::Acpi::MaximumAcpiIommuHandoffs]{};
    bool m_ready{false};
    alignas(4096) Foundation::UInt64 m_pages[MaximumUnits][PagesPerUnit][512U]{};
};
} // Namespace Asteria::Platform::Ia64::Device

#endif // ASTERIA_PLATFORM_IA64_DEVICE_IA64_VTD_RUNTIME_HPP_INCLUDED
