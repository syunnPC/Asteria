// SPDX-License-Identifier: Apache-2.0
#include "Acpi/Aml/Ps2Evidence.hpp"

#include <memory>
#include <vector>

#include "Abi/I8042.hpp"
#include "Acpi/Aml/Fixture.hpp"
#include "Acpi/Aml/Report.hpp"
#include "Support/TestContext.hpp"

namespace
{
class Discard final : public Asteria::Acpi::Aml::IReportSink
{
public:
    [[nodiscard]] bool Write(const char*, Asteria::Acpi::Aml::Size) noexcept override
    {
        return true;
    }
};
} // namespace

namespace Aml = Asteria::Acpi::Aml;
int main()
{
    Asteria::TestSupport::TestContext context{"PS/2 namespace evidence"};
    std::vector<Aml::UInt8> table{};
    ASTERIA_EXPECT(context, "authored ASL fixture", Asteria::TestSupport::Aml::Fixture("Ps2Evidence", table));
    const auto arena = std::make_unique<Aml::Arena>();
    const auto device = std::make_unique<Aml::DeviceFacts>();
    Aml::Library library{arena->Get()};
    ASTERIA_EXPECT(context, "load declarations", library.Load({table.data(), table.size()}), library.FinishLoading());
    Aml::Facts facts{library};
    struct Expected
    {
        const char* Path;
        Aml::Ps2Condition Condition;
        bool Unknown;
    };
    const Expected cases[]{
        {"\\_SB.I300", Aml::Ps2Condition::Qualified, false},
        {"\\_SB.I301", Aml::Ps2Condition::Qualified, false},
        {"\\_SB.I302", Aml::Ps2Condition::Qualified, false},
        {"\\_SB.I303", Aml::Ps2Condition::Qualified, false},
        {"\\_SB.I304", Aml::Ps2Condition::Qualified, false},
        {"\\_SB.I305", Aml::Ps2Condition::Qualified, false},
        {"\\_SB.I306", Aml::Ps2Condition::Qualified, false},
        {"\\_SB.I307", Aml::Ps2Condition::Qualified, false},
        {"\\_SB.I308", Aml::Ps2Condition::Qualified, false},
        {"\\_SB.I309", Aml::Ps2Condition::Qualified, false},
        {"\\_SB.I30A", Aml::Ps2Condition::Qualified, false},
        {"\\_SB.I30B", Aml::Ps2Condition::Qualified, false},
        {"\\_SB.I320", Aml::Ps2Condition::Qualified, false},
        {"\\_SB.CID0", Aml::Ps2Condition::Qualified, false},
        {"\\_SB.BAD0", Aml::Ps2Condition::Identifier, false},
        {"\\_SB.OFF0", Aml::Ps2Condition::Status, false},
        {"\\_SB.DIS0", Aml::Ps2Condition::Status, false},
        {"\\_SB.UNKN", Aml::Ps2Condition::Qualified, true},
        {"\\_SB.MISS", Aml::Ps2Condition::Ports, false},
        {"\\_SB.LEN0", Aml::Ps2Condition::Ports, false},
        {"\\_SB.RANG", Aml::Ps2Condition::Ports, false},
        {"\\_SB.PORT", Aml::Ps2Condition::Ports, false},
        {"\\_SB.MEM0", Aml::Ps2Condition::ResourceKind, false},
        {"\\_SB.DMA0", Aml::Ps2Condition::ResourceKind, false},
        {"\\_SB.IRQ0", Aml::Ps2Condition::Interrupt, false},
        {"\\_SB.IRQF", Aml::Ps2Condition::Qualified, false},
        {"\\_SB.EXT0", Aml::Ps2Condition::Qualified, false},
        {"\\_SB.EXT1", Aml::Ps2Condition::Interrupt, false},
        {"\\_SB.VEND", Aml::Ps2Condition::Qualified, false},
        {"\\_SB.FIO0", Aml::Ps2Condition::Qualified, false},
        {"\\_SB.MOU0", Aml::Ps2Condition::Qualified, false},
        {"\\_SB.MOU1", Aml::Ps2Condition::Qualified, false},
        {"\\_SB.MOU2", Aml::Ps2Condition::Interrupt, false},
        {"\\_SB.MOU3", Aml::Ps2Condition::Ports, false},
        {"\\_SB.MOU4", Aml::Ps2Condition::Identifier, false},
        {"\\_SB.AOFF.KEY0", Aml::Ps2Condition::Status, false},
        {"\\_SB.ADIS.KEY0", Aml::Ps2Condition::Status, false},
        {"\\_SB.AUNK.KEY0", Aml::Ps2Condition::Qualified, true},
        {"\\_SB.CRSU", Aml::Ps2Condition::ResourcesUnknown, false},
        {"\\_SB.CRSM", Aml::Ps2Condition::ResourcesUnknown, false},
        {"\\_SB.CRST", Aml::Ps2Condition::ResourcesUnknown, false},
    };
    for (const auto& expected : cases)
    {
        const auto node = library.Find(expected.Path);
        facts.ReadDevice(node, *device);
        const auto declaration = Aml::Ps2Evidence::Derive(library, *device);
        ASTERIA_EXPECT(context, expected.Path, node != Aml::NoId, declaration.Candidate,
                       declaration.Condition == expected.Condition, declaration.StaUnknown == expected.Unknown);
    }
    struct Signaling
    {
        const char* Path;
        bool Level;
        bool ActiveLow;
    };
    // ACPI 6.5 6.4.2.1: a two-byte IRQ descriptor is edge/high; _HE and _LL
    // otherwise. 6.4.3.6: Extended Interrupt mode bit 1, polarity bit 2. The
    // first descriptor naming the required IRQ decides.
    const Signaling signaling[]{
        {"\\_SB.I303", false, false}, {"\\_SB.VEND", false, false}, {"\\_SB.IRQF", true, true},
        {"\\_SB.EDLO", false, true},  {"\\_SB.FRST", true, false},  {"\\_SB.EXT0", true, true},
        {"\\_SB.EXHI", true, false},  {"\\_SB.MOU1", false, false},
    };
    for (const auto& expected : signaling)
    {
        facts.ReadDevice(library.Find(expected.Path), *device);
        const auto declaration = Aml::Ps2Evidence::Derive(library, *device);
        ASTERIA_EXPECT(context, expected.Path, declaration.Condition == Aml::Ps2Condition::Qualified,
                       declaration.InterruptLevel == expected.Level,
                       declaration.InterruptActiveLow == expected.ActiveLow);
    }
    facts.ReadDevice(library.Find("\\_SB.BOTH"), *device);
    const auto keyboard = Aml::Ps2Evidence::Derive(library, *device, Aml::Ps2DeviceKind::Keyboard);
    const auto mouse = Aml::Ps2Evidence::Derive(library, *device, Aml::Ps2DeviceKind::Mouse);
    ASTERIA_EXPECT(context, "HID/CID families are independent", keyboard.Identifier == 0x0303U,
                   mouse.Identifier == 0x0F03U, keyboard.Condition == Aml::Ps2Condition::Qualified,
                   mouse.Condition == Aml::Ps2Condition::Qualified);
    {
        std::vector<Aml::UInt8> signalingTable{};
        const auto signalingArena = std::make_unique<Aml::Arena>();
        const auto storage = std::make_unique<Aml::ReportStorage>();
        Aml::Library signalingLibrary{signalingArena->Get()};
        ASTERIA_EXPECT(context, "signaling fixture", Asteria::TestSupport::Aml::Fixture("Ps2Signaling", signalingTable),
                       signalingLibrary.Load({signalingTable.data(), signalingTable.size()}),
                       signalingLibrary.FinishLoading());
        Aml::Facts signalingFacts{signalingLibrary};
        Discard sink{};
        ASTERIA_EXPECT(context, "signaling report",
                       Aml::Report::Print(signalingLibrary, signalingFacts, *storage, sink));
        namespace A = Asteria::Abi;
        constexpr Asteria::Foundation::UInt64 expected{A::I8042KeyboardAml | A::I8042MouseAml |
                                                       A::I8042KeyboardIrqDeclared | A::I8042KeyboardIrqLevel |
                                                       A::I8042MouseIrqDeclared | A::I8042MouseIrqActiveLow};
        ASTERIA_EXPECT(context, "the first qualified keyboard and mouse carry the IRQ 1/12 signaling evidence",
                       storage->Summary.I8042Flags == expected, storage->Summary.I8042KeyboardId == 0x0303U);
    }
    for (Aml::Size index{0U}; index <= Aml::MaximumDiagnostics; ++index)
    {
        library.Diagnose(Aml::Reason::RegionAccess);
    }
    library.Diagnose(Aml::Reason::Capacity);
    ASTERIA_EXPECT(context, "arena failure survives a full diagnostic log", library.CapacityFailed());
    library.Reset();
    ASTERIA_EXPECT(context, "fresh arena attempt clears failure", !library.CapacityFailed());
    return context.Finish();
}
