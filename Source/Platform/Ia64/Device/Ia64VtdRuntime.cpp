// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Device/Ia64VtdRuntime.hpp"

#if defined(ASTERIA_TARGET_IA64) || defined(ASTERIA_TARGET_AMD64)
void* operator new(Asteria::Foundation::Size, void*) noexcept;
#else
#include <new>
#endif

#include "Kernel/Device/PciFunctionControl.hpp"
#include "Kernel/Diagnostics/StopTextWriter.hpp"
#include "Platform/Device/PciEcamRuntime.hpp"
#include "Platform/Firmware/Diagnostic.hpp"
#include "Platform/Ia64/Diagnostics/Ia64EvidenceRecord.hpp"

namespace Asteria::Platform::Ia64::Device
{
namespace
{
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt16;
using Foundation::UInt64;
using Platform::Device::VtdDomainBackend;
using Platform::Device::VtdDomainGroup;
using Platform::Device::VtdTablePage;
using Platform::Device::VtdTopologyError;
using Platform::Device::VtdUnit;
using Platform::Device::VtdUnitError;

constexpr UInt64 TranslationEnableStatus{UInt64{1U} << 31U};
constexpr UInt64 UnitKeptOff{1U};
constexpr UInt64 UnitTranslating{2U};
constexpr UInt64 UnitFailed{3U};

/// Boot facts on the diagnostic channel (the IA-64 boot console).
class DiagnosticWriter final : public Kernel::Diagnostics::StopTextWriter<DiagnosticWriter>
{
public:
    void Put(const UInt8 byte) noexcept
    {
        Platform::Firmware::WriteDiagnosticBytes(&byte, 1U);
    }
};

void ReportUnit(const char* const event, const Size unit, const UInt64 base, const UInt64 state) noexcept
{
    DiagnosticWriter writer;
    writer.Text("ASTERIA:IA64-VTD:");
    writer.Text(event);
    writer.Text(":");
    writer.Decimal(unit);
    writer.Text(":BASE:");
    writer.Hex(base);
    writer.Text(":STATE:");
    writer.Decimal(state);
    writer.Text("\r\n");
}

[[nodiscard]] Diagnostics::Ia64VtdUnitEvidence& UnitRecord(const Size unit) noexcept
{
    return Diagnostics::GetIa64EvidenceRecord().IommuBackend.Units[unit];
}
} // Namespace

Ia64VtdRuntime::Ia64VtdRuntime(Kernel::Device::PciEcamConfigurationSpace& pci,
                               const Ia64PhysicalMemoryAccess& access) noexcept
    : m_pci{pci}, m_access{access}, m_registers{Ia64VtdRegisters{access}, Ia64VtdRegisters{access}}
{
}

bool Ia64VtdRuntime::IsDescribed(const Boot::Acpi::AcpiTopology& topology) noexcept
{
    return (topology.DmarHostAddressWidth != 0U) && (topology.DmaRemappingUnitCount != 0U);
}

VtdTablePage Ia64VtdRuntime::Page(const Size unit, const Size page) noexcept
{
    // The image copy is identity-mapped: a page's address is its physical
    // address, below the PAL width the boot already checked for the image.
    return VtdTablePage{m_pages[unit][page],
                        static_cast<UInt64>(reinterpret_cast<Foundation::UIntPointer>(m_pages[unit][page]))};
}

bool Ia64VtdRuntime::RegisterSetIsValid(const Size unit) const noexcept
{
    // VT-d revision 1.3 section 8.3 (DRHD): Size[3:0] is 2^N 4 KiB pages and
    // the base is aligned to the set; the set lies below the address limit.
    const auto& drhd = m_topology->DmaRemappingUnits[unit];
    const UInt64 bytes = Platform::Device::VtdRegisterBytes(drhd);
    if ((m_addressBits < 32U) || (m_addressBits > 63U) || (bytes == 0U) || (drhd.RegisterBaseAddress == 0U) ||
        ((drhd.RegisterBaseAddress & (bytes - 1U)) != 0U))
    {
        return false;
    }
    const UInt64 limit = UInt64{1U} << m_addressBits;
    return (bytes <= limit) && (drhd.RegisterBaseAddress <= (limit - bytes));
}

void Ia64VtdRuntime::Bind(const Size unit, const Ia64VtdInput& input) noexcept
{
    const UInt64 base = m_topology->DmaRemappingUnits[unit].RegisterBaseAddress;
    UnitRecord(unit).RegisterBase = base;
    m_registers[unit].Bind(base, input.PhysicalAddressBits);
    m_registers[unit].SetFirmwareMemoryPolicy(*input.BootInformation, input.Ownership);
    if (m_units[unit] == nullptr)
    {
        m_units[unit] = new (m_unitStorage[unit]) VtdUnit{m_registers[unit], m_cache[unit]};
    }
}

bool Ia64VtdRuntime::Sweep(Platform::Device::PciEcamRuntime& runtime,
                           const Kernel::Device::PciFunctionAddress* const exempt, const bool count) noexcept
{
    // Every published ordinary endpoint stops mastering before any unit's
    // firmware state changes. Bridges keep forwarding and displays keep
    // scanning out; neither is ever assignable (VtdFunctionPolicy).
    auto& backend = Diagnostics::GetIa64EvidenceRecord().IommuBackend;
    Platform::Device::PciEcamRuntimeSummary summary{};
    if (runtime.GetSummary(summary) != Platform::Device::PciEcamFunctionQueryError::None)
    {
        return false;
    }
    for (Size index{0U}; index < summary.PublishedFunctionCount; ++index)
    {
        Platform::Device::PciFunctionPublication publication{};
        if (runtime.GetPublishedFunction(index, publication) != Platform::Device::PciEcamFunctionQueryError::None)
        {
            return false;
        }
        const auto& function = publication.Descriptor;
        if ((function.HeaderLayout == 1U) || (function.ClassCode == 3U) ||
            ((exempt != nullptr) && function.Address.IsEqualTo(*exempt)))
        {
            continue;
        }
        if (!Kernel::Device::SetPciBusMastering(m_pci, m_pci, function.Address, false))
        {
            return false;
        }
        backend.SweptFunctions += count ? 1U : 0U;
    }
    Diagnostics::GetIa64EvidenceRecord().Iommu.SweptFunctions = backend.SweptFunctions;
    return true;
}

void Ia64VtdRuntime::Prepare(const Ia64VtdInput& input) noexcept
{
    auto& backend = Diagnostics::GetIa64EvidenceRecord().IommuBackend;
    m_topology = &input.BootInformation->Acpi;
    for (Size index{0U}; index < m_topology->IommuHandoffCount && index < Boot::Acpi::MaximumAcpiIommuHandoffs; ++index)
    {
        m_handoffRecords[index].RegisterBase = m_topology->IommuHandoffs[index].RegisterBase;
        m_handoffRecords[index].Kind = m_topology->IommuHandoffs[index].Kind;
    }
    const auto& topology = *m_topology;
    backend.Backend = 1U;
    backend.UnitCount = topology.DmaRemappingUnitCount;
    // The limit is the smaller of the DMAR host address width (stored as HAW + 1) and the PAL
    // physical address size, never a fixed 2^48.
    m_addressBits = (topology.DmarHostAddressWidth < input.PhysicalAddressBits) ? topology.DmarHostAddressWidth
                                                                                : input.PhysicalAddressBits;
    backend.HostAddressBits = m_addressBits;
    m_unitCount = (topology.DmaRemappingUnitCount < MaximumUnits) ? topology.DmaRemappingUnitCount : MaximumUnits;
    backend.TablePhysical = Page(0U, 0U).PhysicalAddress;
    backend.TablePages = MaximumUnits * PagesPerUnit;
}

Ia64VtdHandoff Ia64VtdRuntime::HandOff(Platform::Device::PciEcamRuntime& runtime, const Ia64VtdInput& input,
                                       const Kernel::Device::PciFunctionAddress* const bootStorage) noexcept
{
    Prepare(input);
    Platform::Firmware::WriteDiagnosticAscii("ASTERIA:IA64-IOMMU:VT-D\r\n");
    if (!Sweep(runtime, bootStorage, false))
    {
        return Ia64VtdHandoff::Failed;
    }
    // A unit this boot cannot address or does not hold storage for may still
    // translate through the firmware's tables: no boot DMA may then start.
    bool ready = m_topology->DmaRemappingUnitCount <= MaximumUnits;
    for (Size unit{0U}; unit < m_unitCount; ++unit)
    {
        if (!RegisterSetIsValid(unit))
        {
            ready = false;
            continue;
        }
        Bind(unit, input);
        const VtdUnitError prepared = m_units[unit]->PrepareDisabled();
        for (Size index{0U}; index < m_topology->IommuHandoffCount && index < Boot::Acpi::MaximumAcpiIommuHandoffs;
             ++index)
        {
            if (m_handoffRecords[index].RegisterBase == m_topology->DmaRemappingUnits[unit].RegisterBaseAddress)
            {
                m_units[unit]->CopyHandoffRecord(m_handoffRecords[index]);
            }
        }
        auto& record = UnitRecord(unit);
        record.ConfigureError = static_cast<UInt64>(prepared);
        record.State = (prepared == VtdUnitError::None) ? UnitKeptOff : UnitFailed;
        ready = ready && (prepared == VtdUnitError::None);
    }
    // Storage for translating units is limited to two, but scratch takeover/records cover every
    // other addressable inventory entry. No scratch owner or its firmware record is reused
    // prematurely.
    Ia64VtdRegisters scratchRegisters{m_access};
    scratchRegisters.SetFirmwareMemoryPolicy(*input.BootInformation, input.Ownership);
    alignas(Platform::Device::VtdUnit) static UInt8 scratchStorage[sizeof(Platform::Device::VtdUnit)]{};
    for (Size index{0U}; index < m_topology->IommuHandoffCount; ++index)
    {
        auto& record = m_handoffRecords[index];
        if (record.Kind != 1U || record.Outcome != Platform::Device::FirmwareHandoffOutcome::NotAddressed)
        {
            continue;
        }
        bool addressed{false};
        for (Size unit{0U}; unit < m_topology->DmaRemappingUnitCount && unit < Boot::Acpi::MaximumAcpiDmaRemappingUnits;
             ++unit)
        {
            if (m_topology->DmaRemappingUnits[unit].RegisterBaseAddress != record.RegisterBase ||
                !RegisterSetIsValid(unit))
            {
                continue;
            }
            scratchRegisters.Bind(record.RegisterBase, input.PhysicalAddressBits);
            auto* const scratch = new (scratchStorage) Platform::Device::VtdUnit{scratchRegisters, m_cache[0U]};
            const auto result = scratch->PrepareDisabled();
            scratch->CopyHandoffRecord(record);
            addressed = true;
            ready = ready && result == VtdUnitError::None;
            break;
        }
        if (!addressed)
        {
            ready = false;
        }
    }
    Diagnostics::PublishIa64Evidence(Diagnostics::GetIa64EvidenceRecord().IommuBackend.Published);
    return ready ? Ia64VtdHandoff::Ready : Ia64VtdHandoff::DmaRefused;
}

void Ia64VtdRuntime::TakeOverAll(const Ia64VtdInput& input) noexcept
{
    // A refused topology still has its firmware state taken over (printed,
    // translation, queued invalidation and interrupt remapping off) on every
    // unit whose register set is valid, as AMD64's handoff does: nothing is
    // left translating through tables this kernel does not own.
    for (Size unit{0U}; unit < m_unitCount; ++unit)
    {
        if (!RegisterSetIsValid(unit))
        {
            continue;
        }
        Bind(unit, input);
        const VtdUnitError prepared = m_units[unit]->PrepareDisabled();
        for (Size index{0U}; index < m_topology->IommuHandoffCount && index < Boot::Acpi::MaximumAcpiIommuHandoffs;
             ++index)
        {
            if (m_handoffRecords[index].RegisterBase == m_topology->DmaRemappingUnits[unit].RegisterBaseAddress)
            {
                m_units[unit]->CopyHandoffRecord(m_handoffRecords[index]);
            }
        }
        auto& record = UnitRecord(unit);
        record.ConfigureError = static_cast<UInt64>(prepared);
        record.State = (prepared == VtdUnitError::None) ? UnitKeptOff : UnitFailed;
    }
}

bool Ia64VtdRuntime::ConfigureUnit(const Size unit) noexcept
{
    auto& record = UnitRecord(unit);
    const UInt64 base = record.RegisterBase;
    if (m_plan.KeepDisabled[unit])
    {
        const VtdUnitError prepared = m_units[unit]->PrepareDisabled();
        for (Size index{0U}; index < m_topology->IommuHandoffCount && index < Boot::Acpi::MaximumAcpiIommuHandoffs;
             ++index)
        {
            if (m_handoffRecords[index].RegisterBase == m_topology->DmaRemappingUnits[unit].RegisterBaseAddress)
            {
                m_units[unit]->CopyHandoffRecord(m_handoffRecords[index]);
            }
        }
        record.ConfigureError = static_cast<UInt64>(prepared);
        record.DisabledReason = m_plan.NoFunctions[unit] ? 4U : 1U;
        record.State = (prepared == VtdUnitError::None) ? UnitKeptOff : UnitFailed;
        ReportUnit(m_plan.NoFunctions[unit] ? "UNIT-EMPTY-OFF" : "DISPLAY-OFF", unit, base, record.State);
        return prepared == VtdUnitError::None;
    }
    Size count{0U};
    for (Size index{0U}; index < m_plan.Count; ++index)
    {
        if (m_plan.Functions[index].Unit == unit)
        {
            m_requesters[count++] = m_plan.Functions[index].Address.GetRequesterId();
        }
    }
    VtdTablePage contexts[VtdUnit::MaximumBuses]{};
    for (Size bus{0U}; bus < VtdUnit::MaximumBuses; ++bus)
    {
        contexts[bus] = Page(unit, 1U + bus);
    }
    if (m_plan.DisplayConflict[unit])
    {
        ReportUnit("SHARED-DISPLAY", unit, base, 0U);
    }
    const VtdUnitError configured = m_units[unit]->Configure(
        Page(unit, 0U), contexts, m_requesters, count,
        static_cast<Size>(Platform::Device::VtdRegisterBytes(m_topology->DmaRemappingUnits[unit])));
    for (Size index{0U}; index < m_topology->IommuHandoffCount && index < Boot::Acpi::MaximumAcpiIommuHandoffs; ++index)
    {
        if (m_handoffRecords[index].Kind == 1U && m_handoffRecords[index].RegisterBase == base)
        {
            m_units[unit]->CopyHandoffRecord(m_handoffRecords[index]);
        }
    }
    record.ConfigureError = static_cast<UInt64>(configured);
    if (configured != VtdUnitError::None)
    {
        record.State = UnitFailed;
        return false;
    }
    return true;
}

bool Ia64VtdRuntime::Refuse(const bool enabledAny) noexcept
{
    auto& backend = Diagnostics::GetIa64EvidenceRecord().IommuBackend;
    if (m_error == VtdTopologyError::None)
    {
        m_error = VtdTopologyError::Hardware;
    }
    backend.SelectionError = static_cast<UInt64>(m_error);
    for (Size unit{0U}; unit < m_unitCount; ++unit)
    {
        if (UnitRecord(unit).State != UnitTranslating)
        {
            UnitRecord(unit).DisabledReason = 3U;
        }
    }
    Record();
    DiagnosticWriter writer;
    writer.Text("ASTERIA:IA64-VTD:REFUSED:PRE-ENABLE:");
    writer.Decimal(enabledAny ? 0U : 1U);
    writer.Text(":TOPOLOGY:");
    writer.Decimal(static_cast<UInt64>(m_error));
    writer.Text("\r\n");
    return !enabledAny;
}

void Ia64VtdRuntime::BuildBackends() noexcept
{
    // Domain identifiers 1-4 are the IommuDomain slots on every unit; each
    // unit walks its own eight table pages per slot (VtdDomainGroup).
    for (Size slot{0U}; slot < Kernel::Device::IommuDomainPool::Capacity; ++slot)
    {
        VtdDomainBackend* backends[VtdDomainGroup::MaximumUnits]{};
        VtdUnit* units[VtdDomainGroup::MaximumUnits]{};
        Size count{0U};
        for (Size unit{0U}; unit < m_unitCount; ++unit)
        {
            if ((m_units[unit] == nullptr) || !m_units[unit]->IsEnabled())
            {
                continue;
            }
            VtdTablePage pages[VtdDomainBackend::TablePages]{};
            for (Size index{0U}; index < VtdDomainBackend::TablePages; ++index)
            {
                pages[index] = Page(unit, UnitTablePages + (slot * VtdDomainBackend::TablePages) + index);
            }
            backends[count] = new (m_backendStorage[slot][unit])
                VtdDomainBackend{*m_units[unit], static_cast<UInt16>(slot + 1U), pages};
            units[count++] = m_units[unit];
        }
        m_backends[slot] = new (m_groupStorage[slot]) VtdDomainGroup{backends, units, count};
    }
}

bool Ia64VtdRuntime::Initialize(Platform::Device::PciEcamRuntime& runtime, const Ia64VtdInput& input) noexcept
{
    auto& backend = Diagnostics::GetIa64EvidenceRecord().IommuBackend;
    Prepare(input);
    const auto& topology = *m_topology;

    Platform::Device::PciEcamRuntimeSummary summary{};
    Size count{0U};
    VtdTopologyError error{VtdTopologyError::None};
    if (runtime.GetSummary(summary) != Platform::Device::PciEcamFunctionQueryError::None)
    {
        error = VtdTopologyError::Hardware;
    }
    else if ((summary.DiscoveredFunctionCount != summary.PublishedFunctionCount) ||
             (summary.PublishedFunctionCount > VtdUnit::MaximumRequesters))
    {
        // An unpublished function could neither be swept nor routed.
        error = VtdTopologyError::FunctionCapacity;
    }
    else if (topology.DmaRemappingUnitCount > MaximumUnits)
    {
        error = VtdTopologyError::UnitCount;
    }
    else if ((topology.ReservedMemoryRegionCount != 0U) || (topology.MalformedRmrrCount != 0U))
    {
        error = VtdTopologyError::ReservedMemory;
    }
    else
    {
        // The plan names every published function; ResolveVtdTopology also
        // selects (validates) the DRHDs with the IA-64 address limit.
        for (; count < summary.PublishedFunctionCount; ++count)
        {
            Platform::Device::PciFunctionPublication publication{};
            if (runtime.GetPublishedFunction(count, publication) != Platform::Device::PciEcamFunctionQueryError::None)
            {
                error = VtdTopologyError::Hardware;
                break;
            }
            m_functions[count] = publication.Descriptor.Address;
        }
        if (error == VtdTopologyError::None)
        {
            error = Platform::Device::ResolveVtdTopology(topology, m_pci, m_functions, count, m_plan, m_addressBits);
        }
    }
    m_error = error;
    backend.SelectionError = static_cast<UInt64>(error);
    if (!Sweep(runtime, nullptr, true))
    {
        Diagnostics::PublishIa64Evidence(backend.Published);
        return false;
    }
    if (error != VtdTopologyError::None)
    {
        TakeOverAll(input);
        return Refuse(false);
    }
    // Configure every unit (each Configure takes the firmware state over
    // first) before enabling any; a refusal leaves every unit untranslated.
    bool configured{true};
    for (Size unit{0U}; unit < m_unitCount; ++unit)
    {
        Bind(unit, input);
        const bool ready = ConfigureUnit(unit);
        configured = configured && ready;
    }
    if (!configured)
    {
        return Refuse(false);
    }
    bool enabledAny{false};
    for (Size unit{0U}; unit < m_unitCount; ++unit)
    {
        if (m_plan.KeepDisabled[unit])
        {
            continue;
        }
        auto& record = UnitRecord(unit);
        const VtdUnitError enabled = m_units[unit]->Enable();
        record.EnableError = static_cast<UInt64>(enabled);
        record.State = (enabled == VtdUnitError::None) ? UnitTranslating : UnitFailed;
        ReportUnit("UNIT", unit, record.RegisterBase, record.State);
        if (enabled != VtdUnitError::None)
        {
            // A timed-out enable whose TES is already set translates anyway.
            const bool translating = (m_units[unit]->GetSnapshot().Status & TranslationEnableStatus) != 0U;
            return Refuse(enabledAny || translating);
        }
        enabledAny = true;
    }
    BuildBackends();
    m_ready = enabledAny;
    Record();
    return true;
}

void Ia64VtdRuntime::Record() noexcept
{
    auto& evidence = Diagnostics::GetIa64EvidenceRecord().Iommu;
    auto& backend = Diagnostics::GetIa64EvidenceRecord().IommuBackend;
    evidence.Ready = m_ready ? 1U : 0U;
    evidence.Faults = 0U;
    for (Size unit{0U}; unit < m_unitCount; ++unit)
    {
        if (m_units[unit] == nullptr)
        {
            continue;
        }
        if (!m_units[unit]->GetSnapshot().Failed)
        {
            m_units[unit]->PollFaults();
        }
        const auto snapshot = m_units[unit]->GetSnapshot();
        auto& record = UnitRecord(unit);
        record.Version = snapshot.Version;
        record.Capability = snapshot.Capability;
        record.ExtendedCapability = snapshot.ExtendedCapability;
        record.HandoffStatus = snapshot.HandoffStatus;
        record.Status = snapshot.Status;
        record.Levels = snapshot.Levels;
        record.ServedFunctions = snapshot.Served;
        record.Faults = snapshot.Faults;
        record.Overflows = snapshot.Overflows;
        record.Unattributed = snapshot.Unattributed;
        record.TableFlushes = m_cache[unit].GetTableFlushes();
        evidence.Faults += snapshot.Faults;
        if (snapshot.Faults != 0U)
        {
            evidence.LastFaultSource = snapshot.LastFault.Source;
            evidence.LastFaultReason = snapshot.LastFault.Reason;
            evidence.LastFaultAddress = snapshot.LastFault.Address;
            backend.LastFaultRead = snapshot.LastFault.Read ? 1U : 0U;
        }
    }
    Diagnostics::PublishIa64Evidence(backend.Published);
    Diagnostics::PublishIa64Evidence(evidence.Published);
}
} // Namespace Asteria::Platform::Ia64::Device
