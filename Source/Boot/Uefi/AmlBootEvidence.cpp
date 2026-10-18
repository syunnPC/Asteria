// SPDX-License-Identifier: Apache-2.0

#include "Boot/Uefi/AmlBootReport.hpp"

#include "Boot/Acpi/I8042Evidence.hpp"

namespace Asteria::Boot::Uefi
{
namespace
{
constinit AmlBootEvidence g_evidence{};
}
AmlBootEvidence& GetAmlBootEvidence() noexcept
{
    return g_evidence;
}

void ResetAmlBootEvidence() noexcept
{
    g_evidence = {};
}

bool AmlBootEvidenceAttempted() noexcept
{
    return g_evidence.Attempted;
}

void ApplyAmlBootEvidence(Acpi::AcpiTopology& topology, Acpi::PowerRegisters* const power) noexcept
{
    const auto& bound = g_evidence.SleepBinding;
    if (power != nullptr && g_evidence.SleepKnown && topology.Dsdt.ChecksumValid != 0U &&
        topology.Dsdt.PhysicalAddress == bound.PhysicalAddress && topology.Dsdt.Length == bound.Length &&
        topology.Dsdt.ChecksumValid == bound.ChecksumValid)
    {
        power->SleepKnown = 1U;
        power->TypeA = g_evidence.TypeA;
        power->TypeB = g_evidence.TypeB;
    }
    if (g_evidence.MaskFadt)
    {
        topology.IapcBootArchitecture &= static_cast<Foundation::UInt16>(~Foundation::UInt16{2U});
    }
    if (g_evidence.Address != 0U)
    {
        Acpi::I8042Evidence::Record(topology, g_evidence.Flags, g_evidence.Identifier, g_evidence.Checksum,
                                    g_evidence.Address, g_evidence.Length);
    }
}

}
