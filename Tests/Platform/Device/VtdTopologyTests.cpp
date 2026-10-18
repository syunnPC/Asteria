// SPDX-License-Identifier: Apache-2.0
#include "Platform/Device/VtdTopology.hpp"

#include "Boot/Acpi/AcpiTableParser.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Boot::Acpi::AcpiDmaDeviceScope;
using Boot::Acpi::AcpiTopology;
using Foundation::Size;
using Foundation::UInt16;
using Kernel::Device::PciFunctionAddress;
using Platform::Device::SelectVtdRequesters;
using Platform::Device::SelectVtdUnit;
using Platform::Device::VtdTopologyError;

// The AMD64 adapter's register-set and RMRR limit.
constexpr Foundation::UInt8 TestAddressBits{48U};

// QEMU's q35 DMAR: one unit at 0xFED90000 without INCLUDE_PCI_ALL, an
// IOAPIC scope and one endpoint scope per function on bus 0.
[[nodiscard]] AcpiTopology MakeQemuTopology() noexcept
{
    AcpiTopology topology{};
    topology.DmarHostAddressWidth = 48U;
    topology.DmaRemappingUnitCount = 1U;
    topology.DmaRemappingUnits[0U] = {
        .RegisterBaseAddress = 0xFED9'0000U, .Segment = 0U, .Flags = 0U, .Size = 0U, .Reserved = 0U};
    topology.DmaDeviceScopes[0U] = AcpiDmaDeviceScope{.Unit = 0U,
                                                      .Type = 3U,
                                                      .StartBus = 0xFFU,
                                                      .PathEntries = 1U,
                                                      .Device = 0U,
                                                      .Function = 0U,
                                                      .Reserved = 0U,
                                                      .Path = {},
                                                      .Padding = 0U};
    topology.DmaDeviceScopes[1U] = AcpiDmaDeviceScope{.Unit = 0U,
                                                      .Type = 1U,
                                                      .StartBus = 0U,
                                                      .PathEntries = 1U,
                                                      .Device = 0U,
                                                      .Function = 0U,
                                                      .Reserved = 0U,
                                                      .Path = {},
                                                      .Padding = 0U};
    topology.DmaDeviceScopes[2U] = AcpiDmaDeviceScope{.Unit = 0U,
                                                      .Type = 1U,
                                                      .StartBus = 0U,
                                                      .PathEntries = 1U,
                                                      .Device = 2U,
                                                      .Function = 0U,
                                                      .Reserved = 0U,
                                                      .Path = {},
                                                      .Padding = 0U};
    topology.DmaDeviceScopes[3U] = AcpiDmaDeviceScope{.Unit = 0U,
                                                      .Type = 2U,
                                                      .StartBus = 0U,
                                                      .PathEntries = 1U,
                                                      .Device = 0x1CU,
                                                      .Function = 0U,
                                                      .Reserved = 0U,
                                                      .Path = {},
                                                      .Padding = 0U};
    topology.DmaDeviceScopes[4U] = AcpiDmaDeviceScope{.Unit = 0U,
                                                      .Type = 1U,
                                                      .StartBus = 0U,
                                                      .PathEntries = 2U,
                                                      .Device = 0x1CU,
                                                      .Function = 0U,
                                                      .Reserved = 0U,
                                                      .Path = {},
                                                      .Padding = 0U};
    topology.DmaDeviceScopeCount = 5U;
    return topology;
}

// One segment-0 unit with known flags and no RMRR is used; anything else leaves the machine without
// remapping.
void TestUnitSelection(TestContext& runner) noexcept
{
    const AcpiTopology qemu = MakeQemuTopology();
    AcpiTopology absent{};
    AcpiTopology two = qemu;
    two.DmaRemappingUnitCount = 5U;
    AcpiTopology segment = qemu;
    segment.DmaRemappingUnits[0U].Segment = 1U;
    AcpiTopology flagged = qemu;
    flagged.DmaRemappingUnits[0U].Flags = 2U;
    AcpiTopology reserved = qemu;
    reserved.ReservedMemoryRegionCount = 1U;
    AcpiTopology all = qemu;
    all.DmaRemappingUnits[0U].Flags = Boot::Acpi::AcpiDmaRemappingIncludeAllFlag;
    ASTERIA_EXPECT(runner, "only a lone segment-0 unit with known flags and no reserved memory is used",
                   SelectVtdUnit(qemu, TestAddressBits).Error == VtdTopologyError::None,
                   SelectVtdUnit(qemu, TestAddressBits).RegisterBase == 0xFED9'0000U,
                   SelectVtdUnit(all, TestAddressBits).Error == VtdTopologyError::None,
                   SelectVtdUnit(absent, TestAddressBits).Error == VtdTopologyError::NoDmar,
                   SelectVtdUnit(two, TestAddressBits).Error == VtdTopologyError::UnitCount,
                   SelectVtdUnit(segment, TestAddressBits).Error == VtdTopologyError::Segment,
                   SelectVtdUnit(flagged, TestAddressBits).Error == VtdTopologyError::Flags,
                   SelectVtdUnit(reserved, TestAddressBits).Error == VtdTopologyError::ReservedMemory,
                   SelectVtdUnit(two, TestAddressBits).RegisterBase == 0U);
}

void TestRegisterRanges(TestContext& runner) noexcept
{
    AcpiTopology topology{};
    topology.DmarHostAddressWidth = 48U;
    topology.DmaRemappingUnitCount = 3U;
    for (Size unit{0U}; unit < 3U; ++unit)
    {
        topology.DmaRemappingUnits[unit] = {.RegisterBaseAddress = 0xFC80'0000U + (unit * 65536U),
                                            .Segment = 0U,
                                            .Flags = static_cast<Foundation::UInt8>(unit == 2U ? 1U : 0U),
                                            .Size = 4U,
                                            .Reserved = 0U};
    }
    ASTERIA_EXPECT(runner, "three adjacent 64 KiB sets are accepted",
                   SelectVtdUnit(topology, TestAddressBits).Error == VtdTopologyError::None);
    topology.DmaRemappingUnits[0U].Size = 5U;
    ASTERIA_EXPECT(runner, "distinct bases still overlap when their sets overlap",
                   SelectVtdUnit(topology, TestAddressBits).Error == VtdTopologyError::RegisterRange);
    topology.DmaRemappingUnitCount = 1U;
    auto& unit = topology.DmaRemappingUnits[0U];
    unit.Size = 4U;
    unit.RegisterBaseAddress += 4096U;
    ASTERIA_EXPECT(runner, "alignment follows set size",
                   SelectVtdUnit(topology, TestAddressBits).Error == VtdTopologyError::RegisterRange);
    unit.RegisterBaseAddress = (Foundation::UInt64{1U} << 48U) - 65536U;
    ASTERIA_EXPECT(runner, "last in-range set accepted",
                   SelectVtdUnit(topology, TestAddressBits).Error == VtdTopologyError::None);
    unit.RegisterBaseAddress += 65536U;
    ASTERIA_EXPECT(runner, "out-of-range set rejected",
                   SelectVtdUnit(topology, TestAddressBits).Error == VtdTopologyError::RegisterRange);
    unit.RegisterBaseAddress = 0xFC80'0000U;
    unit.Size = 0x14U;
    ASTERIA_EXPECT(runner, "reserved size bits rejected",
                   SelectVtdUnit(topology, TestAddressBits).Error == VtdTopologyError::RegisterRange);
}

// A function is served when INCLUDE_PCI_ALL covers its segment or a one-hop
// endpoint scope names it; a bridge or a longer path serves nothing.
void TestRequesters(TestContext& runner) noexcept
{
    const PciFunctionAddress functions[]{PciFunctionAddress{0U, 0U, 0U, 0U},    PciFunctionAddress{0U, 0U, 2U, 0U},
                                         PciFunctionAddress{0U, 0U, 0x1CU, 0U}, PciFunctionAddress{0U, 1U, 0U, 0U},
                                         PciFunctionAddress{0U, 0U, 0x1FU, 2U}, PciFunctionAddress{1U, 0U, 2U, 0U}};
    constexpr Size count{sizeof(functions) / sizeof(functions[0U])};
    UInt16 served[8U]{};
    const AcpiTopology qemu = MakeQemuTopology();
    const Size named = SelectVtdRequesters(qemu, functions, count, served, 8U, TestAddressBits);
    const UInt16 first = served[0U];
    const UInt16 second = served[1U];
    AcpiTopology all = qemu;
    all.DmaRemappingUnits[0U].Flags = Boot::Acpi::AcpiDmaRemappingIncludeAllFlag;
    const Size everything = SelectVtdRequesters(all, functions, count, served, 8U, TestAddressBits);
    const Size capped = SelectVtdRequesters(all, functions, count, served, 2U, TestAddressBits);
    AcpiTopology absent{};
    ASTERIA_EXPECT(runner, "the unit serves the functions its scopes name, or all of segment 0", named == 2U,
                   first == 0x0000U, second == 0x0010U, everything == 5U, served[0U] == 0x0000U, capped == 2U,
                   SelectVtdRequesters(absent, functions, count, served, 8U, TestAddressBits) == 0U,
                   SelectVtdRequesters(qemu, nullptr, count, served, 8U, TestAddressBits) == 0U);
}
class Ecam final : public Hal::Device::IPciConfigurationSpace
{
public:
    [[nodiscard]] Hal::Device::PciConfigurationReadError Read32(const PciFunctionAddress address,
                                                                const Foundation::UInt16 offset,
                                                                Foundation::UInt32& output) noexcept override
    {
        if (Fail)
        {
            return Hal::Device::PciConfigurationReadError::HardwareFailure;
        }
        if ((offset == 0U) && address.IsEqualTo(Absent))
        {
            output = 0xFFFF'FFFFU;
            return Hal::Device::PciConfigurationReadError::None;
        }
        const bool bridge = (address.GetBus() == 0U) && (address.GetDevice() == 0x1CU);
        output = (offset == 0U) ? 0x1234'8086U : 0U;
        if (offset == 8U)
        {
            output = bridge ? 0x0604'0000U : ((address.GetDevice() == 2U) ? 0x0300'0000U : 0x0C03'0000U);
        }
        if (offset == 0x34U)
        {
            output = bridge && !Conventional ? 0x40U : 0U;
        }
        if (offset == 0x40U)
        {
            output = 0x0040'0010U;
        }
        if (offset == 0x0CU)
        {
            output = bridge ? 0x0001'0000U : 0U;
        }
        if (offset == 0x18U)
        {
            output = BusNumbers;
        }
        return Hal::Device::PciConfigurationReadError::None;
    }
    PciFunctionAddress Absent{};
    bool Fail{false};
    bool Conventional{false};
    Foundation::UInt32 BusNumbers{0x0002'0100U};
};

[[nodiscard]] AcpiDmaDeviceScope Endpoint(const Foundation::UInt8 owner, const Foundation::UInt8 device) noexcept
{
    return {.Unit = owner,
            .Type = 1U,
            .StartBus = 0U,
            .PathEntries = 1U,
            .Device = device,
            .Function = 0U,
            .Reserved = 0U,
            .Path = {},
            .Padding = 0U};
}

void TestSharedBridgeRequester(TestContext& runner) noexcept
{
    Ecam ecam;
    AcpiTopology topology{};
    topology.DmarHostAddressWidth = 48U;
    topology.DmaRemappingUnitCount = 1U;
    topology.DmaRemappingUnits[0U].RegisterBaseAddress = 0xFED90000U;
    topology.DmaRemappingUnits[0U].Flags = 1U;
    constexpr PciFunctionAddress functions[]{{0U, 0U, 0x1CU, 0U}, {0U, 1U, 1U, 0U}};
    Platform::Device::VtdTopologyPlan plan{};
    ASTERIA_EXPECT(runner, "a PCIe root port retains independent downstream requester IDs",
                   Platform::Device::ResolveVtdTopology(topology, ecam, functions, 2U, plan, TestAddressBits) ==
                       VtdTopologyError::None,
                   plan.Functions[1U].Policy == Platform::Device::VtdFunctionPolicy::Assignable);
    ecam.Conventional = true;
    ASTERIA_EXPECT(runner, "conventional downstream DMA never gets independent assignment without an alias",
                   Platform::Device::ResolveVtdTopology(topology, ecam, functions, 2U, plan, TestAddressBits) ==
                       VtdTopologyError::None,
                   plan.Functions[1U].Policy == Platform::Device::VtdFunctionPolicy::Unserved,
                   !Platform::Device::ShouldSweepVtdFunction(plan.Functions[1U], plan),
                   Platform::Device::ResolveVtdTopology(topology, ecam, functions,
                                                        Platform::Device::VtdUnit::MaximumRequesters + 1U, plan,
                                                        TestAddressBits) == VtdTopologyError::FunctionCapacity);
}

void TestSandyBridgeRouting(TestContext& runner) noexcept
{
    using Platform::Device::ResolveVtdTopology;
    using Platform::Device::VtdFunctionPolicy;
    using Platform::Device::VtdTopologyPlan;
    AcpiTopology topology{};
    topology.DmarHostAddressWidth = 39U;
    topology.DmaRemappingUnitCount = 2U;
    topology.DmaRemappingUnits[0U] = {
        .RegisterBaseAddress = 0xFED9'0000U, .Segment = 0U, .Flags = 0U, .Size = 0U, .Reserved = 0U};
    topology.DmaRemappingUnits[1U] = {
        .RegisterBaseAddress = 0xFED9'1000U, .Segment = 0U, .Flags = 1U, .Size = 0U, .Reserved = 0U};
    topology.DmaDeviceScopeCount = 1U;
    topology.DmaDeviceScopes[0U] = Endpoint(0U, 2U);
    topology.ReservedMemoryRegionCount = 2U;
    topology.ReservedMemoryRegions[0U] = {.Base = 0x7D00'0000U, .Limit = 0x811F'FFFFU, .Segment = 0U, .Reserved = {}};
    topology.ReservedMemoryRegions[1U] = {.Base = 0x7C00'0000U, .Limit = 0x7C00'1FFFU, .Segment = 0U, .Reserved = {}};
    topology.ReservedMemoryScopeCount = 3U;
    topology.ReservedMemoryScopes[0U] = Endpoint(0U, 2U);
    topology.ReservedMemoryScopes[1U] = Endpoint(1U, 0x1AU);
    topology.ReservedMemoryScopes[2U] = Endpoint(1U, 0x1DU);
    const PciFunctionAddress functions[]{PciFunctionAddress{0U, 0U, 2U, 0U},    PciFunctionAddress{0U, 0U, 0x1AU, 0U},
                                         PciFunctionAddress{0U, 0U, 0x1DU, 0U}, PciFunctionAddress{0U, 0U, 0x1FU, 2U},
                                         PciFunctionAddress{0U, 0U, 0x1CU, 0U}, PciFunctionAddress{0U, 0U, 0x1CU, 1U},
                                         PciFunctionAddress{0U, 1U, 0U, 0U}};
    Ecam ecam;
    VtdTopologyPlan plan{};
    ASTERIA_EXPECT(runner, "Sandy Bridge graphics is off, USB remains reserved, and SATA routes to include-all",
                   ResolveVtdTopology(topology, ecam, functions, 7U, plan, TestAddressBits) == VtdTopologyError::None,
                   plan.KeepDisabled[0U], !plan.KeepDisabled[1U], plan.Functions[0U].Unit == 0U,
                   plan.Functions[0U].Policy == VtdFunctionPolicy::Display, plan.Functions[0U].Regions == 1U,
                   plan.Functions[1U].Unit == 1U, plan.Functions[1U].Policy == VtdFunctionPolicy::ReservedMemory,
                   plan.Functions[1U].Regions == 2U, plan.Functions[2U].Regions == 2U, plan.Functions[3U].Unit == 1U,
                   plan.Functions[3U].Policy == VtdFunctionPolicy::Assignable, plan.Functions[3U].Regions == 0U);
    using Platform::Device::ShouldSweepVtdFunction;
    ASTERIA_EXPECT(runner, "root ports remain kernel owned; only ordinary endpoints lose bus mastering",
                   plan.Functions[4U].Policy == VtdFunctionPolicy::Bridge,
                   plan.Functions[5U].Policy == VtdFunctionPolicy::Bridge,
                   !ShouldSweepVtdFunction(plan.Functions[0U], plan), !ShouldSweepVtdFunction(plan.Functions[1U], plan),
                   !ShouldSweepVtdFunction(plan.Functions[2U], plan), ShouldSweepVtdFunction(plan.Functions[3U], plan),
                   !ShouldSweepVtdFunction(plan.Functions[4U], plan), !ShouldSweepVtdFunction(plan.Functions[5U], plan),
                   ShouldSweepVtdFunction(plan.Functions[6U], plan));
    // A shared display unit keeps graphics reserved and untouched while ordinary endpoints translate.
    topology.DmaDeviceScopeCount = 0U;
    ASTERIA_EXPECT(runner, "a shared display unit reserves graphics identity memory without sweeping scanout",
                   ResolveVtdTopology(topology, ecam, functions, 7U, plan, TestAddressBits) == VtdTopologyError::None,
                   !plan.KeepDisabled[1U], plan.DisplayConflict[1U], !ShouldSweepVtdFunction(plan.Functions[0U], plan),
                   plan.Functions[0U].Regions == 1U, ShouldSweepVtdFunction(plan.Functions[3U], plan));
    topology.DmaDeviceScopeCount = 1U;
    topology.DmaDeviceScopes[0U] = Endpoint(0U, 2U);
    topology.DmaDeviceScopes[1U] = Endpoint(0U, 4U);
    topology.DmaDeviceScopeCount = 2U;
    topology.ReservedMemoryScopes[3U] = Endpoint(1U, 4U);
    topology.ReservedMemoryScopeCount = 4U;
    ecam.Absent = PciFunctionAddress{0U, 0U, 4U, 0U};
    ASTERIA_EXPECT(runner, "absent final hops in DRHD and RMRR scopes are counted without losing isolation",
                   ResolveVtdTopology(topology, ecam, functions, 7U, plan, TestAddressBits) == VtdTopologyError::None,
                   plan.IgnoredScopes == 2U, plan.Functions[1U].Regions == 2U, !plan.KeepDisabled[1U]);
    ecam.Absent = {};
    topology.DmaDeviceScopeCount = 1U;
    topology.ReservedMemoryScopeCount = 3U;
    ecam.Absent = functions[0U];
    ASTERIA_EXPECT(runner, "an absent graphics function leaves its empty DRHD off while include-all still translates",
                   ResolveVtdTopology(topology, ecam, functions + 1U, 6U, plan, TestAddressBits) ==
                       VtdTopologyError::None,
                   plan.IgnoredScopes == 2U, plan.NoFunctions[0U], plan.KeepDisabled[0U], !plan.KeepDisabled[1U],
                   plan.Functions[0U].Regions == 2U);
    ecam.Absent = {};
    topology.DmaRemappingUnits[1U].Segment = 1U;
    ASTERIA_EXPECT(runner, "RMRR authorities remain reserved even when a DRHD refuses the plan",
                   ResolveVtdTopology(topology, ecam, functions, 7U, plan, TestAddressBits) ==
                       VtdTopologyError::Segment,
                   plan.Count == 7U, plan.Functions[0U].Regions == 1U, plan.Functions[1U].Regions == 2U,
                   plan.Functions[2U].Regions == 2U, plan.Functions[3U].Regions == 0U);
    topology.DmaRemappingUnits[1U].Segment = 0U;
    topology.DmaDeviceScopeCount = 2U;
    topology.DmaDeviceScopes[1U] = Endpoint(1U, 2U);
    ASTERIA_EXPECT(runner, "overlapping explicit scopes never fall through to an arbitrary unit",
                   ResolveVtdTopology(topology, ecam, functions, 7U, plan, TestAddressBits) ==
                       VtdTopologyError::ScopeOverlap);
    topology.DmaDeviceScopeCount = 1U;
    topology.DmaRemappingUnits[0U].Flags = 1U;
    ASTERIA_EXPECT(runner, "two include-all units refuse the topology",
                   SelectVtdUnit(topology, TestAddressBits).Error == VtdTopologyError::IncludeAllOverlap);
    topology.DmaRemappingUnits[0U].Flags = 0U;
    topology.ReservedMemoryRegionCount = 9U;
    ASTERIA_EXPECT(runner, "RMRR truncation is an explicit refusal",
                   SelectVtdUnit(topology, TestAddressBits).Error == VtdTopologyError::ReservedMemory);
    topology.ReservedMemoryRegionCount = 2U;
    topology.DmaDeviceScopeCount = 33U;
    ASTERIA_EXPECT(runner, "scope truncation is an explicit refusal",
                   SelectVtdUnit(topology, TestAddressBits).Error == VtdTopologyError::ScopeCapacity);
}

void TestPaths(TestContext& runner) noexcept
{
    using Platform::Device::ResolveVtdTopology;
    using Platform::Device::VtdTopologyPlan;
    AcpiTopology topology{};
    topology.DmarHostAddressWidth = 39U;
    topology.DmaRemappingUnitCount = 2U;
    topology.DmaRemappingUnits[0U] = {
        .RegisterBaseAddress = 0xFED9'0000U, .Segment = 0U, .Flags = 0U, .Size = 0U, .Reserved = 0U};
    topology.DmaRemappingUnits[1U] = {
        .RegisterBaseAddress = 0xFED9'1000U, .Segment = 0U, .Flags = 1U, .Size = 0U, .Reserved = 0U};
    topology.DmaDeviceScopeCount = 1U;
    topology.DmaDeviceScopes[0U] = Endpoint(0U, 0x1CU);
    topology.DmaDeviceScopes[0U].PathEntries = 2U;
    topology.DmaDeviceScopes[0U].Path[0U] = 3U;
    const PciFunctionAddress functions[]{PciFunctionAddress{0U, 1U, 3U, 0U}, PciFunctionAddress{0U, 2U, 4U, 0U},
                                         PciFunctionAddress{0U, 3U, 4U, 0U}, PciFunctionAddress{0U, 0U, 0x1CU, 0U}};
    Ecam ecam;
    VtdTopologyPlan plan{};
    ASTERIA_EXPECT(runner, "an endpoint path walks the bridge secondary bus rather than assuming start bus",
                   ResolveVtdTopology(topology, ecam, functions, 4U, plan, TestAddressBits) == VtdTopologyError::None,
                   plan.Functions[0U].Unit == 0U, plan.Functions[1U].Unit == 1U);
    topology.DmaDeviceScopes[0U].Type = 2U;
    topology.DmaDeviceScopes[0U].PathEntries = 1U;
    ASTERIA_EXPECT(runner, "a bridge scope covers its subtree and bridge, but not the next bus",
                   ResolveVtdTopology(topology, ecam, functions, 4U, plan, TestAddressBits) == VtdTopologyError::None,
                   plan.Functions[0U].Unit == 0U, plan.Functions[1U].Unit == 0U, plan.Functions[2U].Unit == 1U,
                   plan.Functions[3U].Unit == 0U);
    topology.DmaDeviceScopeCount = 2U;
    topology.DmaDeviceScopes[1U] = Endpoint(1U, 3U);
    topology.DmaDeviceScopes[1U].StartBus = 1U;
    ASTERIA_EXPECT(runner, "an endpoint overlapping a bridge subtree is refused",
                   ResolveVtdTopology(topology, ecam, functions, 4U, plan, TestAddressBits) ==
                       VtdTopologyError::ScopeOverlap);
    topology.DmaDeviceScopeCount = 1U;
    topology.DmaDeviceScopes[0U].PathEntries = 9U;
    ASTERIA_EXPECT(runner, "overlong paths cannot use a truncated prefix",
                   ResolveVtdTopology(topology, ecam, functions, 4U, plan, TestAddressBits) ==
                       VtdTopologyError::ScopePath);
    topology.DmaDeviceScopes[0U].PathEntries = 1U;
    topology.DmaDeviceScopes[0U].PathEntries = 2U;
    ecam.Absent = PciFunctionAddress{0U, 0U, 0x1CU, 0U};
    ASTERIA_EXPECT(runner, "an absent intermediate bridge cannot be skipped as a stale endpoint",
                   ResolveVtdTopology(topology, ecam, functions, 4U, plan, TestAddressBits) ==
                       VtdTopologyError::ScopePath);
    ecam.Absent = {};
    topology.DmaDeviceScopes[0U].PathEntries = 1U;
    ecam.BusNumbers = 0x0002'0000U;
    ASTERIA_EXPECT(runner, "a bridge with no secondary bus cannot serve the start bus",
                   ResolveVtdTopology(topology, ecam, functions, 4U, plan, TestAddressBits) ==
                       VtdTopologyError::ScopePath);
    ecam.BusNumbers = 0x0002'0101U;
    ASTERIA_EXPECT(runner, "a bridge whose primary bus disagrees with the path is rejected",
                   ResolveVtdTopology(topology, ecam, functions, 4U, plan, TestAddressBits) ==
                       VtdTopologyError::ScopePath);
    ecam.BusNumbers = 0x0002'0100U;
    ecam.Fail = true;
    ASTERIA_EXPECT(runner, "ECAM failure refuses routing before touching VT-d",
                   ResolveVtdTopology(topology, ecam, functions, 4U, plan, TestAddressBits) ==
                       VtdTopologyError::ScopePath);
}
void TestFirmwareMemory(TestContext& runner) noexcept
{
    using Platform::Device::VtdFirmwareRangeIsSafe;
    using Boot::PhysicalMemoryKind;
    static Boot::BootInfo info{};
    constexpr Foundation::UInt64 base{0x7D00'0000U};
    constexpr Foundation::UInt64 limit{0x811F'FFFFU}; // 66 MiB, including an unreported gap.
    info.MemoryRegionCount = 2U;
    info.MemoryRegions[0U] = {.PhysicalStart = base,
                              .VirtualStart = 0U,
                              .PageCount = 2U,
                              .Attributes = 0U,
                              .FirmwareType = 0U,
                              .Kind = PhysicalMemoryKind::Reserved};
    info.MemoryRegions[1U] = {.PhysicalStart = limit - 4095U,
                              .VirtualStart = 0U,
                              .PageCount = 1U,
                              .Attributes = 0U,
                              .FirmwareType = 11U,
                              .Kind = PhysicalMemoryKind::MemoryMappedIo};
    ASTERIA_EXPECT(runner, "graphics stolen memory may cross an absent UEFI map range",
                   VtdFirmwareRangeIsSafe(info, base, limit));
    constexpr PhysicalMemoryKind allocatable[]{PhysicalMemoryKind::Loader,       PhysicalMemoryKind::BootServices,
                                               PhysicalMemoryKind::Conventional, PhysicalMemoryKind::AcpiReclaimable,
                                               PhysicalMemoryKind::Persistent,   PhysicalMemoryKind::Unaccepted};
    for (const auto kind : allocatable)
    {
        info.MemoryRegions[1U].Kind = kind;
        ASTERIA_EXPECT(runner, "every allocatable RAM category refuses an overlapping RMRR",
                       !VtdFirmwareRangeIsSafe(info, base, limit));
    }
    info.MemoryRegions[1U].Kind = PhysicalMemoryKind::Conventional;
    info.MemoryRegions[1U].PhysicalStart = limit + 1U;
    ASTERIA_EXPECT(runner, "an adjacent RAM range does not overlap the inclusive RMRR limit",
                   VtdFirmwareRangeIsSafe(info, base, limit));
}

void TestFirmwareQueueOwnership(TestContext& runner) noexcept
{
    using Boot::PhysicalMemoryKind;
    using Platform::Device::VtdFirmwareMemoryOwnership;
    using Platform::Device::VtdFirmwareQueueRangeIsWritable;
    static Boot::BootInfo info;
    constexpr PhysicalMemoryKind kinds[]{
        PhysicalMemoryKind::Reserved,        PhysicalMemoryKind::Loader,          PhysicalMemoryKind::BootServices,
        PhysicalMemoryKind::RuntimeServices, PhysicalMemoryKind::Conventional,    PhysicalMemoryKind::Unusable,
        PhysicalMemoryKind::AcpiReclaimable, PhysicalMemoryKind::AcpiNonVolatile, PhysicalMemoryKind::MemoryMappedIo,
        PhysicalMemoryKind::PalCode,         PhysicalMemoryKind::Persistent,      PhysicalMemoryKind::Unaccepted};
    for (const auto kind : kinds)
    {
        info = {};
        info.MemoryRegionCount = 1U;
        info.MemoryRegions[0U].PhysicalStart = 0x4000U;
        info.MemoryRegions[0U].PageCount = 2U;
        info.MemoryRegions[0U].Kind = kind;
        const bool firmware = kind == PhysicalMemoryKind::Reserved || kind == PhysicalMemoryKind::BootServices ||
                              kind == PhysicalMemoryKind::RuntimeServices ||
                              kind == PhysicalMemoryKind::AcpiNonVolatile;
        ASTERIA_EXPECT(runner, "UEFI ownership allows only complete firmware-owned queue memory",
                       VtdFirmwareQueueRangeIsWritable(info, 0x4000U, 0x5FFFU, {}) == firmware,
                       !VtdFirmwareQueueRangeIsWritable(info, 0x4000U, 0x6000U, {}));
    }
    info.MemoryRegions[0U].Kind = PhysicalMemoryKind::BootServices;
    for (Size reserved{0U}; reserved < 4U; ++reserved)
    {
        VtdFirmwareMemoryOwnership ownership{};
        info.Files = {};
        info.Inventory = {};
        if (reserved == 0U)
        {
            ownership.PoolStart = 0x5000U;
            ownership.PoolPages = 1U;
        }
        if (reserved == 1U)
        {
            ownership.StartupStart = 0x5000U;
            ownership.StartupPages = 1U;
        }
        if (reserved == 2U)
        {
            info.Files.PhysicalStart = 0x5000U;
            info.Files.PageCount = 1U;
        }
        if (reserved == 3U)
        {
            info.Inventory.PhysicalStart = 0x5000U;
            info.Inventory.Pages = 1U;
        }
        ASTERIA_EXPECT(runner, "kernel pool, startup, files/modules and inventory exclude even allowed map kinds",
                       !VtdFirmwareQueueRangeIsWritable(info, 0x4000U, 0x5FFFU, ownership),
                       VtdFirmwareQueueRangeIsWritable(info, 0x4000U, 0x4FFFU, ownership));
    }
    info.Files = {};
    info.Inventory = {};
    info.MemoryRegionCount = 2U;
    info.MemoryRegions[0U].PhysicalStart = 0x5000U;
    info.MemoryRegions[0U].PageCount = 1U;
    info.MemoryRegions[1U] = info.MemoryRegions[0U];
    info.MemoryRegions[1U].PhysicalStart = 0x4000U;
    ASTERIA_EXPECT(runner, "unsorted adjacent firmware descriptors can cover a whole queue",
                   VtdFirmwareQueueRangeIsWritable(info, 0x4000U, 0x5FFFU, {}));
    info.MemoryRegions[0U].PhysicalStart = 0x6000U;
    ASTERIA_EXPECT(runner, "a memory-map gap refuses DMA queue writes",
                   !VtdFirmwareQueueRangeIsWritable(info, 0x4000U, 0x6FFFU, {}));
    info.MemoryRegions[0U].PhysicalStart = 0x4000U;
    info.MemoryRegions[0U].Kind = PhysicalMemoryKind::Loader;
    ASTERIA_EXPECT(runner, "an overlapping LoaderData descriptor refuses even if another descriptor permits it",
                   !VtdFirmwareQueueRangeIsWritable(info, 0x4000U, 0x4FFFU, {}));
    info.MemoryRegions[0U].PhysicalStart = 0xFFFF'FFFF'FFFF'F000ULL;
    info.MemoryRegions[0U].PageCount = 2U;
    ASTERIA_EXPECT(runner, "map and owned-range arithmetic is checked before accepting any write",
                   !VtdFirmwareQueueRangeIsWritable(info, 0x4000U, 0x4FFFU, {}));
    info.MemoryRegionCount = 1U;
    info.MemoryRegions[0U] = info.MemoryRegions[1U];
    ASTERIA_EXPECT(runner, "invalid bounds and unaligned/overflowed kernel ownership refuse writes",
                   !VtdFirmwareQueueRangeIsWritable(info, 5U, 4U, {}),
                   !VtdFirmwareQueueRangeIsWritable(info, 0U, ~Foundation::UInt64{0U}, {}),
                   !VtdFirmwareQueueRangeIsWritable(info, 0x4000U, 0x4FFFU, {0x5001U, 1U, 0U, 0U}),
                   !VtdFirmwareQueueRangeIsWritable(info, 0x4000U, 0x4FFFU, {0xFFFF'FFFF'FFFF'F000ULL, 2U, 0U, 0U}));
}

// An authored DMAR shaped like an Intel 7500 hub's (Documentation/Architecture/IommuDomain.md;
// Datasheet section 21.14.1: one register page at VTBAR and one at VTBAR + 1000h): host address
// width field 50, flags 0, two segment-0 DRHDs of one page each, the first naming the bridge
// 00:07.0 with a sub-hierarchy scope, the last INCLUDE_PCI_ALL, and no RMRR.
constexpr Foundation::UInt64 Ioh7500Base{0xFED9'0000U};

[[nodiscard]] AcpiTopology MakeIoh7500Topology(const Foundation::UInt64 base) noexcept
{
    AcpiTopology topology{};
    topology.DmarHostAddressWidth = 51U;
    topology.DmaRemappingUnitCount = 2U;
    topology.DmaRemappingUnits[0U] = {
        .RegisterBaseAddress = base + 0x1000U, .Segment = 0U, .Flags = 0U, .Size = 0U, .Reserved = 0U};
    topology.DmaRemappingUnits[1U] = {.RegisterBaseAddress = base,
                                      .Segment = 0U,
                                      .Flags = Boot::Acpi::AcpiDmaRemappingIncludeAllFlag,
                                      .Size = 0U,
                                      .Reserved = 0U};
    topology.DmaDeviceScopeCount = 1U;
    topology.DmaDeviceScopes[0U] = {.Unit = 0U,
                                    .Type = Boot::Acpi::AcpiDmaScopeBridge,
                                    .StartBus = 0U,
                                    .PathEntries = 1U,
                                    .Device = 7U,
                                    .Function = 0U,
                                    .Reserved = 0U,
                                    .Path = {},
                                    .Padding = 0U};
    return topology;
}

void Store(Foundation::UInt8* const bytes, const Foundation::UInt64 value, const Size width) noexcept
{
    for (Size index{0U}; index < width; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> (8U * index));
    }
}

void Sign(Foundation::UInt8* const bytes, const char* const signature, const Size length) noexcept
{
    for (Size index{0U}; index < length; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(signature[index]);
    }
}

void Checksum(Foundation::UInt8* const bytes, const Size length, const Size at) noexcept
{
    bytes[at] = 0U;
    Foundation::UInt8 sum{0U};
    for (Size index{0U}; index < length; ++index)
    {
        sum = static_cast<Foundation::UInt8>(sum + bytes[index]);
    }
    bytes[at] = static_cast<Foundation::UInt8>(0U - sum);
}

void SealTable(Foundation::UInt8* const table, const char* const signature, const Size length) noexcept
{
    Sign(table, signature, 4U);
    Store(table + 4U, length, 4U);
    Checksum(table, length, 9U);
}

/// The same DMAR as ACPI bytes, parsed by the boot's own parser for IA-64:
/// An RSDP (revision 2), an XSDT, a MADT with one local SAPIC and one I/O SAPIC, and the DMAR, in
/// one page the memory map calls ACPI reclaimable.
[[nodiscard]] Boot::Acpi::AcpiTableParseError ParseIoh7500Dmar(AcpiTopology& output) noexcept
{
    constexpr Size xsdt{64U};
    constexpr Size madt{128U};
    constexpr Size dmar{256U};
    alignas(4096) static Foundation::UInt8 page[4096U];
    static Boot::BootInfo info;
    for (Foundation::UInt8& byte : page)
    {
        byte = 0U;
    }
    const auto address = static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(page));
    info = {};
    info.Signature = Boot::BootInfoSignature;
    info.Version = Boot::BootInfoVersion;
    info.SizeBytes = sizeof(Boot::BootInfo);
    info.MemoryRegionCount = 1U;
    info.FirmwareDescriptorVersion = 1U;
    info.FirmwareDescriptorSize = 40U;
    info.FirmwareMemoryMapSizeBytes = 40U;
    info.MemoryRegions[0U] = {.PhysicalStart = address,
                              .VirtualStart = 0U,
                              .PageCount = 1U,
                              .Attributes = 0U,
                              .FirmwareType = 9U,
                              .Kind = Boot::PhysicalMemoryKind::AcpiReclaimable};

    Foundation::UInt8* const table = page + dmar;
    table[8U] = 1U;
    table[36U] = 50U;
    Foundation::UInt8* const first = table + 48U;
    Store(first + 2U, 24U, 2U);
    Store(first + 8U, Ioh7500Base + 0x1000U, 8U);
    first[16U] = Boot::Acpi::AcpiDmaScopeBridge;
    first[17U] = 8U;
    first[22U] = 7U;
    Foundation::UInt8* const last = first + 24U;
    Store(last + 2U, 16U, 2U);
    last[4U] = Boot::Acpi::AcpiDmaRemappingIncludeAllFlag;
    Store(last + 8U, Ioh7500Base, 8U);
    SealTable(table, "DMAR", 88U);

    Foundation::UInt8* const interrupts = page + madt;
    interrupts[8U] = 3U;
    Store(interrupts + 36U, 0xFEE0'0000U, 4U);
    Foundation::UInt8* const sapic = interrupts + 44U;
    sapic[0U] = 7U;
    sapic[1U] = 16U;
    Store(sapic + 8U, 1U, 4U);
    Foundation::UInt8* const ioSapic = sapic + 16U;
    ioSapic[0U] = 6U;
    ioSapic[1U] = 16U;
    Store(ioSapic + 8U, 0xFEC0'0000U, 8U);
    SealTable(interrupts, "APIC", 76U);

    Foundation::UInt8* const root = page + xsdt;
    root[8U] = 1U;
    Store(root + 36U, address + madt, 8U);
    Store(root + 44U, address + dmar, 8U);
    SealTable(root, "XSDT", 52U);

    Sign(page, "RSD PTR ", 8U);
    page[15U] = 2U;
    Store(page + 20U, 36U, 4U);
    Store(page + 24U, address + xsdt, 8U);
    Checksum(page, 20U, 8U);
    Checksum(page, 36U, 32U);
    return Boot::Acpi::AcpiTableParser::Parse(info, page, Boot::Acpi::AcpiTargetArchitecture::Ia64, output);
}

/// Bus 0 of the authored machine: a host bridge, a display, an endpoint
/// (vendor 1234, device 11e8) and a PCI-to-PCI bridge without a PCI Express
/// capability whose bus numbers the firmware left zero. Nothing else answers.
class Ioh7500Ecam final : public Hal::Device::IPciConfigurationSpace
{
public:
    [[nodiscard]] Hal::Device::PciConfigurationReadError Read32(const PciFunctionAddress address,
                                                                const Foundation::UInt16 offset,
                                                                Foundation::UInt32& output) noexcept override
    {
        const Foundation::UInt8 device = address.GetDevice();
        const bool present = (address.GetBus() == 0U) && (address.GetFunction() == 0U) &&
                             ((device == 0U) || (device == 2U) || (device == 6U) || (device == 7U));
        output = present ? 0U : 0xFFFF'FFFFU;
        if (!present)
        {
            return Hal::Device::PciConfigurationReadError::None;
        }
        if (offset == 0U)
        {
            constexpr Foundation::UInt32 identities[]{0x3407'8086U, 0U, 0x1111'1234U, 0U,
                                                      0U,           0U, 0x11E8'1234U, 0x340E'8086U};
            output = identities[device];
        }
        else if (offset == 8U)
        {
            constexpr Foundation::UInt32 classes[]{0x0600'0000U, 0U, 0x0300'0000U, 0U,
                                                   0U,           0U, 0x00FF'0000U, 0x0604'0000U};
            output = classes[device];
        }
        else if ((offset == 0x0CU) && (device == 7U))
        {
            output = 0x0001'0000U;
        }
        return Hal::Device::PciConfigurationReadError::None;
    }
};

constexpr PciFunctionAddress Ioh7500Functions[]{PciFunctionAddress{0U, 0U, 0U, 0U}, PciFunctionAddress{0U, 0U, 2U, 0U},
                                                PciFunctionAddress{0U, 0U, 6U, 0U}, PciFunctionAddress{0U, 0U, 7U, 0U}};

// The IA-64 limit: the DMAR's 51-bit host address width and a 50-bit PAL
// physical address size.
constexpr Foundation::UInt8 Ioh7500AddressBits{50U};

// The bridge the firmware gave no bus range is served alone by the first
// unit; everything else falls to INCLUDE_PCI_ALL, where the display shares
// its unit with ordinary endpoints.
void ExpectIoh7500Routing(TestContext& runner, const AcpiTopology& topology, const char* const description) noexcept
{
    using Platform::Device::ShouldSweepVtdFunction;
    using Platform::Device::VtdFunctionPolicy;
    Ioh7500Ecam ecam;
    Platform::Device::VtdTopologyPlan plan{};
    ASTERIA_EXPECT(runner, description,
                   Platform::Device::ResolveVtdTopology(topology, ecam, Ioh7500Functions, 4U, plan,
                                                        Ioh7500AddressBits) == VtdTopologyError::None,
                   plan.Count == 4U, plan.IgnoredScopes == 0U, plan.Functions[3U].Unit == 0U,
                   plan.Functions[3U].Policy == VtdFunctionPolicy::Bridge, plan.Functions[2U].Unit == 1U,
                   plan.Functions[2U].Policy == VtdFunctionPolicy::Assignable, plan.Functions[1U].Unit == 1U,
                   plan.Functions[1U].Policy == VtdFunctionPolicy::Display, plan.Functions[0U].Unit == 1U,
                   plan.DisplayConflict[1U], !plan.DisplayConflict[0U], !plan.NoFunctions[0U], !plan.NoFunctions[1U],
                   !plan.KeepDisabled[0U], !plan.KeepDisabled[1U], ShouldSweepVtdFunction(plan.Functions[2U], plan),
                   !ShouldSweepVtdFunction(plan.Functions[1U], plan),
                   !ShouldSweepVtdFunction(plan.Functions[3U], plan));
}

void TestIoh7500Topology(TestContext& runner) noexcept
{
    using Platform::Device::ResolveVtdTopology;
    using Platform::Device::VtdTopologyPlan;
    ExpectIoh7500Routing(
        runner, MakeIoh7500Topology(Ioh7500Base),
        "a 7500-shaped DMAR routes its unranged bridge to the first unit and the rest to INCLUDE_PCI_ALL");
    AcpiTopology parsed{};
    const auto parseError = ParseIoh7500Dmar(parsed);
    ASTERIA_EXPECT(runner, "the parser keeps HAW + 1, both DRHDs in order and the bridge scope",
                   parseError == Boot::Acpi::AcpiTableParseError::None, parsed.DmarHostAddressWidth == 51U,
                   parsed.DmarFlags == 0U, parsed.DmaRemappingUnitCount == 2U,
                   parsed.DmaRemappingUnits[0U].RegisterBaseAddress == (Ioh7500Base + 0x1000U),
                   parsed.DmaRemappingUnits[0U].Flags == 0U, parsed.DmaRemappingUnits[0U].Size == 0U,
                   parsed.DmaRemappingUnits[1U].RegisterBaseAddress == Ioh7500Base,
                   parsed.DmaRemappingUnits[1U].Flags == Boot::Acpi::AcpiDmaRemappingIncludeAllFlag,
                   parsed.DmaDeviceScopeCount == 1U, parsed.DmaDeviceScopes[0U].Unit == 0U,
                   parsed.DmaDeviceScopes[0U].Type == Boot::Acpi::AcpiDmaScopeBridge,
                   parsed.DmaDeviceScopes[0U].StartBus == 0U, parsed.DmaDeviceScopes[0U].PathEntries == 1U,
                   parsed.DmaDeviceScopes[0U].Device == 7U, parsed.DmaDeviceScopes[0U].Function == 0U,
                   parsed.ReservedMemoryRegionCount == 0U, parsed.ReservedMemoryScopeCount == 0U);
    ExpectIoh7500Routing(runner, parsed, "the parsed 7500-shaped DMAR routes like the authored topology");

    // Register pages above 2^48: Itanium's 50 physical address bits reach
    // them, AMD64's 48 do not.
    const AcpiTopology high = MakeIoh7500Topology(0x1'0000'FED9'0000ULL);
    ExpectIoh7500Routing(runner, high, "register sets above 2^48 are routed within a 50-bit address limit");
    Ioh7500Ecam ecam;
    VtdTopologyPlan plan{};
    const AcpiTopology low = MakeIoh7500Topology(Ioh7500Base);
    ASTERIA_EXPECT(runner, "the address limit is the adapter's, from 32 to 63 bits, never a fixed 2^48",
                   SelectVtdUnit(high, Ioh7500AddressBits).RegisterBase == 0x1'0000'FED9'1000ULL,
                   SelectVtdUnit(high, 48U).Error == VtdTopologyError::RegisterRange,
                   ResolveVtdTopology(high, ecam, Ioh7500Functions, 4U, plan, 48U) == VtdTopologyError::RegisterRange,
                   SelectVtdUnit(low, 32U).Error == VtdTopologyError::None,
                   SelectVtdUnit(low, 63U).Error == VtdTopologyError::None,
                   SelectVtdUnit(low, 31U).Error == VtdTopologyError::RegisterRange,
                   SelectVtdUnit(low, 64U).Error == VtdTopologyError::RegisterRange);

    // A path cannot descend through the bridge without a bus range.
    AcpiTopology through = low;
    through.DmaDeviceScopes[0U].Type = Boot::Acpi::AcpiDmaScopeEndpoint;
    through.DmaDeviceScopes[0U].PathEntries = 2U;
    ASTERIA_EXPECT(runner, "a bridge without a bus range ends a scope path; one through it is refused",
                   ResolveVtdTopology(through, ecam, Ioh7500Functions, 4U, plan, Ioh7500AddressBits) ==
                       VtdTopologyError::ScopePath);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"VtdTopologyTests"};
    Asteria::Test::TestUnitSelection(runner);
    Asteria::Test::TestRegisterRanges(runner);
    Asteria::Test::TestRequesters(runner);
    Asteria::Test::TestSandyBridgeRouting(runner);
    Asteria::Test::TestSharedBridgeRequester(runner);
    Asteria::Test::TestPaths(runner);
    Asteria::Test::TestFirmwareMemory(runner);
    Asteria::Test::TestFirmwareQueueOwnership(runner);
    Asteria::Test::TestIoh7500Topology(runner);
    return runner.Finish();
}
