// SPDX-License-Identifier: Apache-2.0
#include "Acpi/Aml/Library.hpp"

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
void Numbers(TestContext& context, Aml::Library& library) noexcept
{
    struct Example
    {
        const char* Path;
        Aml::UInt64 Value;
    };
    constexpr Example examples[]{
        {"SIZE", 3U},
        {"TYPE", 4U},
        {"DREF", 19U},
        {"SETS", 0x1234U},
        {"MISS", 0U},
        {"SHFL", 96U},
        {"SHFR", 8U},
        {"SH64", 0U},
        {"SUBT", ~Aml::UInt64{1U}},
        {"BAND", 0x50U},
        {"BNAN", ~Aml::UInt64{0x50U}},
        {"BORS", 0xF5U},
        {"BNOR", ~Aml::UInt64{0xF5U}},
        {"BNOT", ~Aml::UInt64{0x55U}},
        {"FSLB", 5U},
        {"FSRB", 5U},
        {"MODU", 2U},
        {"DIVD", 5U},
        {"DECR", 4U},
        {"CMPS", ~Aml::UInt64{0U}},
        {"CMPB", ~Aml::UInt64{0U}},
        {"NEQU", ~Aml::UInt64{0U}},
        {"LGEQ", ~Aml::UInt64{0U}},
        {"LLEQ", ~Aml::UInt64{0U}},
        {"LOGA", ~Aml::UInt64{0U}},
        {"FAMS", 0x9ABDBDE2ULL},
        {"SYNL", 1U},
        {"GWRD", 0x3412U},
    };
    for (const auto& example : examples)
    {
        const auto result{library.Evaluate(library.Find(example.Path))};
        ASTERIA_EXPECT(context, example.Path, result.Known(),
                       library.GetObject(result.Value).Kind == Aml::ObjectKind::Integer,
                       library.GetObject(result.Value).Integer == example.Value);
    }
    const Aml::Id argument{library.Integer(41U)};
    const auto result{library.Evaluate(library.Find("ARGW"), &argument, 1U)};
    ASTERIA_EXPECT(context, "argument slots writable", result.Known(), library.GetObject(result.Value).Integer == 42U);
    const auto debug{library.Evaluate(library.Find("DBUG"))};
    ASTERIA_EXPECT(context, "Debug writes are benign", debug.Known(), library.EffectCount() != 0U);
    for (const char* const name : {"BCD0", "BCD1", "CRES", "LODA", "LDTB"})
    {
        const auto unknown{library.Evaluate(library.Find(name))};
        ASTERIA_EXPECT(context, name, unknown.Error == Aml::Reason::Unsupported || unknown.Error == Aml::Reason::Type);
    }
}
void Data(TestContext& context, Aml::Library& library) noexcept
{
    struct TextCase
    {
        const char* Method;
        const char* Expected;
    };
    constexpr TextCase textCases[]{{"CPYO", "changed"}, {"HXBF", "0x00,0xAB,0xFF"}, {"DCBF", "0,171,255"}};
    for (const auto& example : textCases)
    {
        const auto result{library.Evaluate(library.Find(example.Method))};
        const auto data{library.Data(result.Value)};
        ASTERIA_EXPECT(context, example.Method, result.Known(), data.Length == std::strlen(example.Expected),
                       std::memcmp(data.Data, example.Expected, data.Length) == 0);
    }
    const auto failed{library.Evaluate(library.Find("FAIL"))};
    const auto unchanged{library.Evaluate(library.Find("LINT"))};
    ASTERIA_EXPECT(context, "Unknown rolls back model stores", failed.Error == Aml::Reason::RegionAccess,
                   unchanged.Known(), library.GetObject(unchanged.Value).Integer == 0x1234U);
    for (Aml::Size repeat{0U}; repeat < 2U; ++repeat)
    {
        const auto named{library.Evaluate(library.Find("WNAM"))};
        const auto data{library.Data(named.Value)};
        ASTERIA_EXPECT(context, "method Name lifetime", named.Known(), data.Length == 3U, data.Data[1U] == 7U);
    }
    const auto wide{library.Evaluate(library.Find("WIDE"))};
    const auto wideBytes{library.Data(wide.Value)};
    ASTERIA_EXPECT(context, "wide buffer field", wide.Known(), wideBytes.Length == 9U, wideBytes.Data[0U] == 1U,
                   wideBytes.Data[8U] == 9U);
    const auto cat{library.Evaluate(library.Find("BCAT"))};
    const auto bytes{library.Data(cat.Value)};
    ASTERIA_EXPECT(context, "buffer concatenate", cat.Known(), bytes.Length == 4U, bytes.Data[0U] == 1U,
                   bytes.Data[3U] == 4U);
    const auto mid{library.Evaluate(library.Find("BMID"))};
    const auto part{library.Data(mid.Value)};
    ASTERIA_EXPECT(context, "buffer Mid", mid.Known(), part.Length == 2U, part.Data[0U] == 2U, part.Data[1U] == 3U);
    const Aml::Id argument{library.Integer(2U)};
    const auto package{library.Evaluate(library.Find("VPCK"), &argument, 1U)};
    ASTERIA_EXPECT(context, "variable package", package.Known(), library.GetObject(package.Value).Count == 2U,
                   library.GetObject(library.Element(package.Value, 1U)).Integer == 2U);
}
void IntegerWidth(TestContext& context, Aml::Library& library) noexcept
{
    // Method BUFF returns ToBuffer(Ones, NullName). ACPI integer width also
    // determines this conversion's buffer length, not only its numeric value.
    auto table{Fixture::Table({0x14U, 0x0AU, 'B', 'U', 'F', 'F', 0U, 0xA4U, 0x96U, 0xFFU, 0U})};
    for (const Aml::UInt8 revision : {Aml::UInt8{1U}, Aml::UInt8{2U}})
    {
        library.Reset();
        table[8U] = revision;
        Fixture::Checksum(table);
        if (!ASTERIA_EXPECT(context, "integer width fixture", library.Load({table.data(), table.size()}),
                            library.FinishLoading()))
        {
            return;
        }
        const auto result{library.Evaluate(library.Find("BUFF"))};
        const auto bytes{library.Data(result.Value)};
        ASTERIA_EXPECT(context, "ToBuffer integer width", result.Known(), bytes.Length == (revision == 1U ? 4U : 8U));
        for (Aml::Size index{0U}; index < bytes.Length; ++index)
        {
            ASTERIA_EXPECT(context, "Ones byte conversion", bytes.Data[index] == 0xFFU);
        }
    }
}
} // namespace
int main()
{
    TestContext context{"AML opcodes"};
    const auto arena{std::make_unique<Aml::Arena>()};
    Aml::Library library{arena->Get()};
    std::vector<Aml::UInt8> table;
    if (!ASTERIA_EXPECT(context, "additional grammar fixture", Fixture::Fixture("More", table),
                        library.Load({table.data(), table.size()}), library.FinishLoading()))
    {
        return context.Finish();
    }
    Numbers(context, library);
    Data(context, library);
    IntegerWidth(context, library);
    return context.Finish();
}
