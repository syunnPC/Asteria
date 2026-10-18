// SPDX-License-Identifier: Apache-2.0
#include "Platform/Amd64/Boot/Amd64ProcessorTopology.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace
{
using Asteria::Boot::Acpi::AcpiProcessorControllerKind;
using Asteria::Boot::Acpi::AcpiTopology;
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt32;
using Asteria::Platform::Amd64::Bootstrap::Amd64ProcessorSelection;
using Asteria::Platform::Amd64::Bootstrap::Amd64ProcessorTopologyError;
using Asteria::Platform::Amd64::Bootstrap::CountEnabledAmd64Processors;
using Asteria::Platform::Amd64::Bootstrap::SelectAmd64Processors;

// MADT processor flags as the boot parser retains them (exactly one set).
constexpr UInt32 Enabled{1U};
constexpr UInt32 OnlineCapable{2U};

struct Record
{
    UInt32 ApicId;
    UInt32 Flags;
};

AcpiTopology g_topology{};

/// A topology of Local APIC records in firmware order.
void SetRecords(const Record* const records, const Size count) noexcept
{
    g_topology = AcpiTopology{};
    g_topology.ProcessorCount = static_cast<UInt32>(count);
    for (Size index{0U}; index < count; ++index)
    {
        auto& processor = g_topology.Processors[index];
        processor.AcpiUid = static_cast<UInt32>(index);
        processor.HardwareId = records[index].ApicId;
        processor.Flags = records[index].Flags;
        processor.ControllerKind = AcpiProcessorControllerKind::LocalApic;
    }
}

template <Size Count> void SetRecords(const Record (&records)[Count]) noexcept
{
    SetRecords(records, Count);
}

[[nodiscard]] bool Selects(const UInt32 bsp, const UInt32* const expected, const Size count) noexcept
{
    Amd64ProcessorSelection selection{};
    if ((SelectAmd64Processors(g_topology, bsp, selection) != Amd64ProcessorTopologyError::None) ||
        (selection.Count != count) || (CountEnabledAmd64Processors(g_topology) != count))
    {
        return false;
    }
    for (Size index{0U}; index < count; ++index)
    {
        if (selection.HardwareIds[index] != expected[index])
        {
            return false;
        }
    }
    return true;
}

/// The error for `bsp`, and whether the output selection stayed untouched.
[[nodiscard]] bool Rejects(const UInt32 bsp, const Amd64ProcessorTopologyError expected) noexcept
{
    Amd64ProcessorSelection selection{{0xA5U, 0xA6U, 0xA7U, 0xA8U}, 9U};
    return (SelectAmd64Processors(g_topology, bsp, selection) == expected) && (selection.Count == 9U) &&
           (selection.HardwareIds[0U] == 0xA5U) && (selection.HardwareIds[3U] == 0xA8U);
}
} // namespace

int main()
{
    TestContext runner{"Amd64ProcessorTopologyTests"};

    SetRecords({{0U, Enabled}, {1U, Enabled}, {2U, Enabled}, {3U, Enabled}});
    constexpr UInt32 four[]{0U, 1U, 2U, 3U};
    ASTERIA_EXPECT(runner, "four enabled processors, BSP first", Selects(0U, four, 4U));

    SetRecords({{1U, Enabled}, {0U, Enabled}, {2U, Enabled}});
    constexpr UInt32 bspMoved[]{0U, 1U, 2U};
    ASTERIA_EXPECT(runner, "the BSP becomes CPU 0 wherever the MADT lists it", Selects(0U, bspMoved, 3U));

    // An online-capable processor is not present at boot: it is validated and
    // omitted, and the others keep their firmware order.
    SetRecords({{0U, Enabled}, {1U, OnlineCapable}, {2U, Enabled}, {3U, OnlineCapable}});
    constexpr UInt32 present[]{0U, 2U};
    ASTERIA_EXPECT(runner, "online-capable processors are omitted", Selects(0U, present, 2U));
    SetRecords({{5U, OnlineCapable}, {4U, Enabled}});
    constexpr UInt32 alone[]{4U};
    ASTERIA_EXPECT(runner, "an online-capable record before the BSP is omitted", Selects(4U, alone, 1U));

    SetRecords({{0U, OnlineCapable}, {1U, Enabled}});
    ASTERIA_EXPECT(runner, "a BSP that is only online-capable is rejected",
                   Rejects(0U, Amd64ProcessorTopologyError::BspNotEnabled));
    SetRecords({{1U, Enabled}, {2U, Enabled}});
    ASTERIA_EXPECT(runner, "a BSP the MADT does not list is rejected",
                   Rejects(0U, Amd64ProcessorTopologyError::BspNotEnabled));
    SetRecords({{0U, Enabled}, {1U, Enabled}, {1U, Enabled}});
    ASTERIA_EXPECT(runner, "two enabled records of one APIC ID are rejected",
                   Rejects(0U, Amd64ProcessorTopologyError::DuplicateProcessor));
    SetRecords({{0U, Enabled}, {0U, Enabled}});
    ASTERIA_EXPECT(runner, "two BSP records are rejected",
                   Rejects(0U, Amd64ProcessorTopologyError::DuplicateProcessor));
    SetRecords({{0U, Enabled}, {256U, Enabled}});
    ASTERIA_EXPECT(runner, "an enabled APIC ID xAPIC mode cannot address is rejected",
                   Rejects(0U, Amd64ProcessorTopologyError::UnaddressableProcessor));
    SetRecords({{0U, Enabled}, {300U, OnlineCapable}});
    constexpr UInt32 bspOnly[]{0U};
    ASTERIA_EXPECT(runner, "an omitted record needs no xAPIC ID", Selects(0U, bspOnly, 1U));
    SetRecords({{0U, Enabled}, {255U, Enabled}});
    ASTERIA_EXPECT(runner, "xAPIC broadcast ID 255 is refused",
                   Rejects(0U, Amd64ProcessorTopologyError::UnaddressableProcessor));
    SetRecords({{7U, Enabled}, {254U, Enabled}, {0U, Enabled}});
    constexpr UInt32 edges[]{7U, 254U, 0U};
    ASTERIA_EXPECT(runner, "APIC IDs 254 and 0 are ordinary application processors", Selects(7U, edges, 3U));
    SetRecords({{1U, Enabled}, {2U, Enabled}, {3U, Enabled}, {4U, Enabled}});
    ASTERIA_EXPECT(runner, "four enabled processors besides the BSP do not fit",
                   Rejects(0U, Amd64ProcessorTopologyError::TooManyProcessors));
    // Records past ProcessorCount are not part of the topology.
    SetRecords({{0U, Enabled}, {1U, Enabled}, {2U, Enabled}, {3U, Enabled}});
    g_topology.ProcessorCount = 2U;
    constexpr UInt32 counted[]{0U, 1U};
    ASTERIA_EXPECT(runner, "stale records past the count are ignored", Selects(0U, counted, 2U));

    // Every record, omitted ones included, must be a well-formed xAPIC or
    // x2APIC record with exactly one known flag.
    constexpr UInt32 badFlags[]{0U, 3U, 4U};
    for (const UInt32 flags : badFlags)
    {
        SetRecords({{0U, Enabled}, {1U, flags}});
        ASTERIA_EXPECT(runner, "flags other than exactly Enabled or OnlineCapable are rejected",
                       Rejects(0U, Amd64ProcessorTopologyError::InvalidRecord));
    }
    SetRecords({{0U, Enabled}, {1U, OnlineCapable}});
    g_topology.Processors[1U].Reserved = 1U;
    ASTERIA_EXPECT(runner, "nonzero reserved bits are rejected",
                   Rejects(0U, Amd64ProcessorTopologyError::InvalidRecord));
    SetRecords({{0U, Enabled}, {1U, OnlineCapable}});
    g_topology.Processors[1U].HardwareEid = 1U;
    ASTERIA_EXPECT(runner, "an extended ID is rejected", Rejects(0U, Amd64ProcessorTopologyError::InvalidRecord));
    SetRecords({{0U, Enabled}, {1U, Enabled}});
    g_topology.Processors[1U].ControllerKind = AcpiProcessorControllerKind::LocalSapic;
    ASTERIA_EXPECT(runner, "a Local SAPIC record is rejected", Rejects(0U, Amd64ProcessorTopologyError::InvalidRecord));
    SetRecords({{0U, Enabled}, {1U, Enabled}});
    g_topology.Processors[1U].ControllerKind = AcpiProcessorControllerKind::LocalX2Apic;
    constexpr UInt32 mixed[]{0U, 1U};
    ASTERIA_EXPECT(runner, "an x2APIC record of an xAPIC ID is admitted", Selects(0U, mixed, 2U));
    SetRecords({{0U, Enabled}});
    g_topology.ProcessorCount = 5U;
    ASTERIA_EXPECT(runner, "more records than the topology holds are rejected",
                   Rejects(0U, Amd64ProcessorTopologyError::InvalidRecord));
    SetRecords({{0x1234U, Enabled}, {0U, Enabled}, {0xABCDEFU, Enabled}, {0x100U, Enabled}});
    Amd64ProcessorSelection wide{};
    ASTERIA_EXPECT(runner, "x2APIC IDs remain full width and BSP becomes logical zero",
                   SelectAmd64Processors(g_topology, 0xABCDEFU, wide, 0xFFFF'FFFEU) ==
                       Amd64ProcessorTopologyError::None,
                   wide.Count == 4U, wide.HardwareIds[0U] == 0xABCDEFU, wide.HardwareIds[1U] == 0x1234U,
                   wide.HardwareIds[2U] == 0U, wide.HardwareIds[3U] == 0x100U);
    SetRecords({{0U, Enabled}, {0xFFFF'FFFFU, Enabled}});
    ASTERIA_EXPECT(runner, "x2APIC broadcast ID cannot name a processor",
                   SelectAmd64Processors(g_topology, 0U, wide, 0xFFFF'FFFFU) ==
                       Amd64ProcessorTopologyError::UnaddressableProcessor);
    return runner.Finish();
}
