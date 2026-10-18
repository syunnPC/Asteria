// SPDX-License-Identifier: Apache-2.0
#include "Platform/Device/FirmwareDmaReservations.hpp"

#include <memory>

#include "Support/TestContext.hpp"

namespace
{
using Asteria::Foundation::UInt64;
using Asteria::Kernel::Device::PciFunctionAddress;
using Asteria::Platform::Device::FirmwareDmaReservations;
using Asteria::Platform::Device::VtdTopologyPlan;

// Synthetic DMAR scopes reserve distinct regions for two requesters. The
// same bus/device/function on another segment is a separate requester.
constexpr UInt64 EhciBase{0x2000'0000U};
constexpr UInt64 EhciLimit{0x2000'7FFFU};
constexpr UInt64 GraphicsBase{0x4000'0000U};
constexpr UInt64 GraphicsLimit{0x401F'FFFFU};
constexpr Asteria::Foundation::UInt32 Ehci{0x0219U};
constexpr Asteria::Foundation::UInt32 Display{0x042AU};
} // namespace

int main()
{
    Asteria::TestSupport::TestContext context{"FirmwareDmaReservationsTests"};
    auto topology = std::make_unique<Asteria::Boot::Acpi::AcpiTopology>();
    auto plan = std::make_unique<VtdTopologyPlan>();
    topology->ReservedMemoryRegionCount = 2U;
    topology->ReservedMemoryRegions[0U] = {EhciBase, EhciLimit, 0U, {}};
    topology->ReservedMemoryRegions[1U] = {GraphicsBase, GraphicsLimit, 0U, {}};
    plan->Count = 4U;
    plan->Functions[0U].Address = PciFunctionAddress{0U, 2U, 3U, 1U};
    plan->Functions[0U].Regions = 1U;
    plan->Functions[1U].Address = PciFunctionAddress{0U, 4U, 5U, 2U};
    plan->Functions[1U].Regions = 2U;
    plan->Functions[2U].Address = PciFunctionAddress{0U, 6U, 7U, 0U};
    plan->Functions[3U].Address = PciFunctionAddress{1U, 2U, 3U, 1U};
    FirmwareDmaReservations dmar{};
    ASTERIA_EXPECT(context, "a DMAR's RMRRs reserve only their scoped requesters, once",
                   dmar.BuildFromDmar(*topology, *plan), !dmar.BuildFromDmar(*topology, *plan),
                   dmar.GetRangeCount() == 2U, dmar.GetRequesterCount() == 2U);
    ASTERIA_EXPECT(context, "a range overlapping the reservation at its start, end, inside or around it",
                   dmar.Overlaps(Ehci, EhciBase - 0x1000U, EhciBase),
                   dmar.Overlaps(Ehci, EhciLimit, EhciLimit + 0x1000U),
                   dmar.Overlaps(Ehci, EhciBase + 0x2000U, EhciBase + 0x2FFFU), dmar.Overlaps(Ehci, 0U, ~UInt64{0U}));
    ASTERIA_EXPECT(context, "adjacent ranges, other requesters and other ranges stay mappable",
                   !dmar.Overlaps(Ehci, EhciBase - 0x1000U, EhciBase - 1U),
                   !dmar.Overlaps(Ehci, EhciLimit + 1U, EhciLimit + 0x1000U),
                   !dmar.Overlaps(Ehci, GraphicsBase, GraphicsLimit),
                   dmar.Overlaps(Display, GraphicsBase, GraphicsBase), !dmar.Overlaps(Display, EhciBase, EhciLimit),
                   !dmar.Overlaps(0x0638U, EhciBase, EhciLimit), !dmar.Overlaps(0x0001'0219U, EhciBase, EhciLimit));

    auto ivrs = std::make_unique<Asteria::Boot::Acpi::IvrsTopology>();
    ivrs->MemoryCount = 1U;
    ivrs->Memory[0U] = {0x000A'0000U, 0x000B'FFFFU, 0U, 0xFFFFU, 0U, 6U, 0x21U};
    plan->Functions[2U].Regions = 1U;
    FirmwareDmaReservations amd{};
    ASTERIA_EXPECT(context, "an IVRS's IVMD blocks reserve their requesters the same way",
                   amd.BuildFromIvrs(*ivrs, *plan), !amd.BuildFromDmar(*topology, *plan), amd.GetRangeCount() == 1U,
                   amd.Overlaps(0x0638U, 0x000B'F000U, 0x000C'0FFFU),
                   !amd.Overlaps(0x0638U, 0x000C'0000U, 0x000C'0FFFU), amd.Overlaps(Ehci, 0x000A'0000U, 0x000A'0000U),
                   !amd.Overlaps(Display, 0x000A'0000U, 0x000A'0000U));
    return context.Finish();
}
