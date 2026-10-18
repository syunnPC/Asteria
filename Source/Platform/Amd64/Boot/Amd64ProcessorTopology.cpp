// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Boot/Amd64ProcessorTopology.hpp"

namespace Asteria::Platform::Amd64::Bootstrap
{
namespace
{
using ::Asteria::Boot::Acpi::AcpiProcessorControllerKind;
using ::Asteria::Boot::Acpi::AcpiProcessorTopology;
using ::Asteria::Boot::Acpi::AcpiTopology;
using Foundation::Size;
using Foundation::UInt32;

// MADT Local APIC / Local x2APIC flags (ACPI 6.5, 5.2.12.2): exactly one of
// these is set in every record the boot parser retains.
constexpr UInt32 ProcessorEnabledFlag{1U};
constexpr UInt32 ProcessorOnlineCapableFlag{1U << 1U};

[[nodiscard]] bool IsRecordValid(const AcpiProcessorTopology& record) noexcept
{
    return (record.Reserved == 0U) && (record.HardwareEid == 0U) &&
           ((record.ControllerKind == AcpiProcessorControllerKind::LocalApic) ||
            (record.ControllerKind == AcpiProcessorControllerKind::LocalX2Apic)) &&
           ((record.Flags == ProcessorEnabledFlag) || (record.Flags == ProcessorOnlineCapableFlag));
}

[[nodiscard]] Size RecordCount(const AcpiTopology& topology) noexcept
{
    return topology.ProcessorCount < ::Asteria::Boot::Acpi::MaximumAcpiProcessors
               ? topology.ProcessorCount
               : ::Asteria::Boot::Acpi::MaximumAcpiProcessors;
}
} // namespace

Size CountEnabledAmd64Processors(const AcpiTopology& topology) noexcept
{
    Size enabled{0U};
    for (Size index{0U}; index < RecordCount(topology); ++index)
    {
        if (topology.Processors[index].Flags == ProcessorEnabledFlag)
        {
            ++enabled;
        }
    }
    return enabled;
}

Amd64ProcessorTopologyError SelectAmd64Processors(const AcpiTopology& topology, const UInt32 bspApicId,
                                                  Amd64ProcessorSelection& selection,
                                                  const UInt32 maximumApicId) noexcept
{
    if (topology.ProcessorCount > ::Asteria::Boot::Acpi::MaximumAcpiProcessors)
    {
        return Amd64ProcessorTopologyError::InvalidRecord;
    }
    Amd64ProcessorSelection selected{{bspApicId}, 1U};
    bool foundBsp{false};
    for (Size index{0U}; index < topology.ProcessorCount; ++index)
    {
        const AcpiProcessorTopology& record = topology.Processors[index];
        if (!IsRecordValid(record))
        {
            return Amd64ProcessorTopologyError::InvalidRecord;
        }
        if (record.Flags != ProcessorEnabledFlag)
        {
            continue;
        }
        if (record.HardwareId > maximumApicId || record.HardwareId == 0xFFFF'FFFFU)
        {
            return Amd64ProcessorTopologyError::UnaddressableProcessor;
        }
        if (record.HardwareId == bspApicId)
        {
            if (foundBsp)
            {
                return Amd64ProcessorTopologyError::DuplicateProcessor;
            }
            foundBsp = true;
            continue;
        }
        for (Size previous{1U}; previous < selected.Count; ++previous)
        {
            if (selected.HardwareIds[previous] == record.HardwareId)
            {
                return Amd64ProcessorTopologyError::DuplicateProcessor;
            }
        }
        if (selected.Count >= Kernel::Scheduler::MaximumProcessorCount)
        {
            return Amd64ProcessorTopologyError::TooManyProcessors;
        }
        selected.HardwareIds[selected.Count] = record.HardwareId;
        ++selected.Count;
    }
    if (!foundBsp)
    {
        return Amd64ProcessorTopologyError::BspNotEnabled;
    }
    selection = selected;
    return Amd64ProcessorTopologyError::None;
}
} // namespace Asteria::Platform::Amd64::Bootstrap
