// SPDX-License-Identifier: Apache-2.0
#include "Acpi/Aml/Facts.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "Acpi/Aml/Fixture.hpp"
#include "Support/TestContext.hpp"

namespace
{
namespace Aml = Asteria::Acpi::Aml;
namespace Fixture = Asteria::TestSupport::Aml;
using Asteria::TestSupport::TestContext;
void Number(TestContext& context, Aml::Library& library, const char* const path, const Aml::UInt64 expected,
            const Aml::Id* const args = nullptr, const Aml::Size count = 0U) noexcept
{
    const auto result{library.Evaluate(library.Find(path), args, count)};
    ASTERIA_EXPECT(context, path, result.Known(), library.GetObject(result.Value).Kind == Aml::ObjectKind::Integer,
                   library.GetObject(result.Value).Integer == expected);
}
void Text(TestContext& context, Aml::Library& library, const char* const path, const char* const expected) noexcept
{
    const auto result{library.Evaluate(library.Find(path))};
    const auto data{library.Data(result.Value)};
    ASTERIA_EXPECT(context, path, result.Known(), library.GetObject(result.Value).Kind == Aml::ObjectKind::String,
                   data.Length == std::strlen(expected), std::memcmp(data.Data, expected, data.Length) == 0);
}
void Operators(TestContext& context, Aml::Library& library) noexcept
{
    Number(context, library, "CALL", 53U);
    Number(context, library, "ALIA", 23U);
    Number(context, library, "\\_SB.PCI0.KBD0.LOOK", 23U);
    const Aml::Id argument{library.Integer(8U)};
    Number(context, library, "LOOP", 8U, &argument, 1U);
    Number(context, library, "IDXS", 0xA5U);
    Number(context, library, "BFLD", 0x1FFU);
    Number(context, library, "PKGS", 9U);
    Number(context, library, "REFS", 23U);
    Number(context, library, "CNDR", 23U);
    Number(context, library, "INTS", 0x1234U);
    Number(context, library, "SZOF", 2U);
    Number(context, library, "OTYP", 10U);
    Number(context, library, "MTCH", 1U);
    Text(context, library, "CONV", "there");
    Text(context, library, "HEXS", "ABCDEF");
    Text(context, library, "DECS", "12345");
    Text(context, library, "STRS", "AB");
    Number(context, library, "OSIS", 1U);
    ASTERIA_EXPECT(context, "every OSI query retained", library.OsiQueryCount() == 3U,
                   library.OsiQuery(0U).Length == 12U, library.OsiQuery(1U).Length == 5U);
    Number(context, library, "_REV", 2U);
    Text(context, library, "_OS", "Microsoft Windows NT");
    const auto buffer{library.Evaluate(library.Find("BUFS"))};
    const auto bytes{library.Data(buffer.Value)};
    ASTERIA_EXPECT(context, "integer to little-endian buffer", buffer.Known(), bytes.Length == 8U,
                   bytes.Data[0U] == 0x78U, bytes.Data[3U] == 0x12U, bytes.Data[7U] == 0U);
    Number(context, library, "EFFT", ~Aml::UInt64{0U});
    ASTERIA_EXPECT(context, "effects recorded", library.EffectCount() == 8U, library.GetEffect(0U)->Opcode == 0x5B23U);
    for (const char* const path : {"FLDR", "FLDW"})
    {
        const auto result{library.Evaluate(library.Find(path))};
        ASTERIA_EXPECT(context, "field access is Unknown", result.Error == Aml::Reason::RegionAccess);
    }
    for (const char* const path : {"UNSP", "FATL"})
    {
        const auto result{library.Evaluate(library.Find(path))};
        ASTERIA_EXPECT(context, "unsupported is Unknown", result.Error == Aml::Reason::Unsupported);
    }
    const auto spin{library.Evaluate(library.Find("SPIN"), nullptr, 0U, 100U)};
    ASTERIA_EXPECT(context, "loop budget", spin.Error == Aml::Reason::Budget, spin.Operations == 100U);
    const auto recur{library.Evaluate(library.Find("RECU"))};
    ASTERIA_EXPECT(context, "iterative call bound", recur.Error == Aml::Reason::CallDepth,
                   library.Used().CallDepth == Aml::MaximumCallDepth);
    const Aml::Id zero{library.Integer(0U)};
    const auto divide{library.Evaluate(library.Find("DIV0"), &zero, 1U)};
    ASTERIA_EXPECT(context, "divide by zero", divide.Error == Aml::Reason::DivideByZero);
}
void Model(TestContext& context, Aml::Library& library) noexcept
{
    Aml::UInt64 seed{0xA57E21AULL};
    const char* const replay{std::getenv("ASTERIA_TEST_SEED")};
    if (replay != nullptr)
    {
        seed = std::strtoull(replay, nullptr, 0);
    }
    const Aml::UInt64 initial{seed};
    const Aml::Id calc{library.Find("CALC")};
    const Aml::Id pic{library.Find("_PIC")};
    const Aml::Id mode{library.Find("MODE")};
    for (Aml::Size step{0U}; step < 1000U; ++step)
    {
        // Unsigned wrap is deliberate in both the seeded generator and the
        // independent AML integer arithmetic reference model.
        seed = seed * 6364136223846793005ULL + 1U;
        const Aml::UInt64 a{seed};
        seed = seed * 6364136223846793005ULL + 1U;
        const Aml::UInt64 b{seed};
        const Aml::Id arguments[]{library.Integer(a), library.Integer(b)};
        const auto result{library.Evaluate(calc, arguments, 2U)};
        const Aml::UInt64 expected{((a + b) * 3U) ^ 5U};
        const auto stored{library.Evaluate(pic, arguments, 1U)};
        const auto snapshot{library.Evaluate(mode)};
        if (!result.Known() || library.GetObject(result.Value).Integer != expected || !stored.Known() ||
            !snapshot.Known() || library.GetObject(snapshot.Value).Integer != a)
        {
            static_cast<void>(std::fprintf(stderr, "seed=%llu step=%zu op=CALC/PIC\n", initial, step));
            ASTERIA_FAIL(context, "stateful method model");
            return;
        }
    }
    ASTERIA_EXPECT(context, "model resource counts bounded", library.Used().Objects < Aml::MaximumObjects);
}
void Devices(TestContext& context, Aml::Library& library) noexcept
{
    const auto output{std::make_unique<Aml::DeviceFacts>()};
    Aml::Facts facts{library};
    ASTERIA_EXPECT(context, "PIC selects APIC", facts.Prepare() == Aml::Reason::None);
    facts.ReadDevice(library.Find("\\_SB.PCI0"), *output);
    ASTERIA_EXPECT(context, "PCI root and APIC routes", output->PciRoot, output->PrtError == Aml::Reason::None,
                   output->PrtCount == 2U, output->Prt[0U].Direct, output->Prt[0U].Index == 17U,
                   output->Prt[1U].SourceNode == library.Find("\\_SB.PCI0.LNK0"));
    facts.ReadDevice(library.Find("\\_SB.PCI0.KBD0"), *output);
    ASTERIA_EXPECT(context, "keyboard facts", output->Keyboard, output->Sta == Aml::Presence::Present,
                   output->Cid.Count == 2U, output->Crs.Error == Aml::Reason::None, output->Crs.Count == 4U,
                   output->Crs.Items[0U].Minimum == 0x60U, output->Crs.Items[1U].Minimum == 0x64U,
                   output->Crs.Items[2U].Interrupt == 2U);
    facts.ReadDevice(library.Find("\\_SB.PCI0.MOU0"), *output);
    ASTERIA_EXPECT(context, "mouse facts", output->Mouse, output->Crs.Error == Aml::Reason::None,
                   output->Crs.Items[2U].Interrupt == 0x1000U);
    facts.ReadDevice(library.Find("\\_SB.PCI0.OFF0.CHLD"), *output);
    ASTERIA_EXPECT(context, "absent ancestor applies", output->Sta == Aml::Presence::Absent);
    facts.ReadDevice(library.Find("\\_SB.PCI0.UNKN"), *output);
    ASTERIA_EXPECT(context, "Unknown cannot become present", output->Sta == Aml::Presence::Unknown,
                   output->StaError == Aml::Reason::RegionAccess, output->Crs.Error == Aml::Reason::RegionAccess);
    const auto sleep{facts.Sleep()};
    ASTERIA_EXPECT(context, "S5 package", sleep.Error == Aml::Reason::None, sleep.TypeA == 5U, sleep.TypeB == 6U);
    Aml::Facts bounded{library, 0U};
    facts.ReadDevice(library.Find("\\_SB.PCI0.KBD0"), *output);
    ASTERIA_EXPECT(context, "facts budget reports Unknown", bounded.Prepare() == Aml::Reason::Budget);
}
void Forward(TestContext& context, Aml::Library& library) noexcept
{
    std::vector<Aml::UInt8> first;
    std::vector<Aml::UInt8> second;
    if (!ASTERIA_EXPECT(context, "forward fixtures", Fixture::Fixture("Forward", first),
                        Fixture::Fixture("Later", second)))
    {
        return;
    }
    library.Reset();
    ASTERIA_EXPECT(context, "table order and forward scope", library.Load({first.data(), first.size()}),
                   library.Load({second.data(), second.size()}), library.FinishLoading());
    Number(context, library, "\\LATE.CALL", 43U);
    Number(context, library, "DUPE", 11U);
    bool conflict{false};
    for (Aml::Size i{0U}; i < library.DiagnosticCount(); ++i)
    {
        conflict = conflict || library.GetDiagnostic(i)->Code == Aml::Reason::Conflict;
    }
    ASTERIA_EXPECT(context, "later definition diagnosed", conflict);
}
void ModuleConditions(TestContext& context, Aml::Library& library) noexcept
{
    std::vector<Aml::UInt8> table;
    ASTERIA_EXPECT(context, "module fixture", Fixture::Fixture("Conditional", table));
    library.Reset();
    ASTERIA_EXPECT(context, "module conditional loading", library.Load({table.data(), table.size()}),
                   library.FinishLoading());
    Number(context, library, "CHOS", 1U);
    Number(context, library, "STEP", 3U);
    ASTERIA_EXPECT(context, "only selected declarations resolve", library.Find("GOOD") != Aml::NoId,
                   library.Find("BAD0") == Aml::NoId, library.Find("SKIP") == Aml::NoId, library.OsiQueryCount() == 1U);
    Aml::Facts facts{library};
    const auto output{std::make_unique<Aml::DeviceFacts>()};
    const Aml::Id good{facts.NextDevice()};
    facts.ReadDevice(good, *output);
    ASTERIA_EXPECT(context, "inactive branches cannot publish devices", output->Keyboard,
                   output->Sta == Aml::Presence::Present, facts.NextDevice(good) == Aml::NoId);
    ASTERIA_EXPECT(context, "unknown predicate fixture", Fixture::Fixture("ConditionalUnknown", table));
    library.Reset();
    ASTERIA_EXPECT(context, "unknown module load", library.Load({table.data(), table.size()}),
                   !library.FinishLoading());
    Aml::Facts unknown{library};
    const Aml::Id node{unknown.NextDevice()};
    unknown.ReadDevice(node, *output);
    ASTERIA_EXPECT(context, "Unknown module predicate propagates", node != Aml::NoId,
                   output->Sta == Aml::Presence::Unknown, output->StaError == Aml::Reason::RegionAccess,
                   output->Hid.Error == Aml::Reason::RegionAccess);
}

} // namespace
int main()
{
    TestContext context{"AML library"};
    const auto arena{std::make_unique<Aml::Arena>()};
    Aml::Library library{arena->Get()};
    std::vector<Aml::UInt8> table;
    if (!ASTERIA_EXPECT(context, "core fixture", Fixture::Fixture("Core", table),
                        library.Load({table.data(), table.size()}), library.FinishLoading()))
    {
        return context.Finish();
    }
    ASTERIA_EXPECT(context, "serialized method flags", (library.GetNode(library.Find("CALC"))->Flags & 8U) != 0U);
    const auto* field{library.GetNode(library.Find("FLD1"))};
    ASTERIA_EXPECT(context, "field units retain bit geometry", field != nullptr, field->Kind == Aml::NodeKind::Field,
                   field->BitOffset == 16U, field->BitLength == 16U, field->Body.Length > 4U);
    Operators(context, library);
    Devices(context, library);
    Model(context, library);
    Forward(context, library);
    ModuleConditions(context, library);
    return context.Finish();
}
