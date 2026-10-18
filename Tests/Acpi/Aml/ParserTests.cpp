// SPDX-License-Identifier: Apache-2.0
#include "Acpi/Aml/Library.hpp"

#include <memory>
#include <vector>

#include "Acpi/Aml/Fixture.hpp"
#include "Support/TestContext.hpp"

namespace
{
namespace Aml = Asteria::Acpi::Aml;
namespace Fixture = Asteria::TestSupport::Aml;
using Asteria::TestSupport::TestContext;
void Thousands(TestContext& context, Aml::Library& library, Aml::Arena& arena) noexcept
{
    std::vector<Aml::UInt8> body;
    constexpr Aml::UInt8 hex[]{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
    for (Aml::UInt32 i{0U}; i < 10000U; ++i)
    {
        // Four-segment-independent NameSegs, all authored here.
        body.insert(body.end(), {0x08U, 'A', hex[(i >> 8U) & 15U], hex[(i >> 4U) & 15U], hex[i & 15U], 0x0CU});
        // Use 26 distinct initial letters to avoid three-digit name rollover.
        body[body.size() - 5U] = static_cast<Aml::UInt8>('A' + i / 4096U);
        for (Aml::Size byte{0U}; byte < 4U; ++byte)
        {
            body.push_back(static_cast<Aml::UInt8>((i >> (byte * 8U)) & 0xFFU));
        }
    }
    const auto table{Fixture::Table(body)};
    library.Reset();
    ASTERIA_EXPECT(context, "laptop-sized namespace", library.Load({table.data(), table.size()}),
                   library.FinishLoading(), library.Used().Nodes == 10010U);
    const auto last{library.Evaluate(library.Find("C70F"))};
    ASTERIA_EXPECT(context, "last large-namespace name", last.Known(), library.GetObject(last.Value).Integer == 9999U);
    arena.Get().NodeLimit = 64U;
    library.Reset();
    static_cast<void>(library.Load({table.data(), table.size()}));
    static_cast<void>(library.FinishLoading());
    ASTERIA_EXPECT(context, "capacity retains prefix", library.Used().Nodes == 64U, library.Find("A000") != Aml::NoId,
                   library.Find("C70F") == Aml::NoId, library.DiagnosticCount() != 0U);
    arena.Get().NodeLimit = Aml::MaximumNodes;
}
void Names(TestContext& context, Aml::Library& library) noexcept
{
    // NullName following RootChar opens the namespace root. DualNamePrefix
    // creates a forward scope; its target is defined after it.
    const auto table{Fixture::Table({
        0x10U, 0x0AU, '\\', 0U,    0x08U, 'R',   'O',   'O',   'T', 0x0AU, 0x17U, 0x5BU, 0x82U, 0x05U, 'D',
        'E',   'V',   '0',  0x10U, 0x14U, '\\',  0x2EU, 'D',   'E', 'V',   '0',   'S',   'U',   'B',   '0',
        0x08U, 'N',   'U',  'M',   '0',   'R',   'O',   'O',   'T', 0x10U, 0x1AU, '\\',  0x2FU, 3U,    'D',
        'E',   'V',   '0',  'S',   'U',   'B',   '0',   'D',   'E', 'E',   'P',   0x08U, 'V',   'A',   'L',
        '0',   '^',   'N',  'U',   'M',   '0',   0x10U, 0x13U, 'D', 'E',   'V',   '0',   0x5BU, 0x82U, 0x0CU,
        'S',   'U',   'B',  '0',   0x5BU, 0x82U, 0x05U, 'D',   'E', 'E',   'P',
    })};
    library.Reset();
    const bool loaded{library.Load({table.data(), table.size()})};
    static_cast<void>(library.FinishLoading());
    ASTERIA_EXPECT(context, "names parsed", loaded);
    const auto result{library.Evaluate(library.Find("\\DEV0.SUB0.DEEP.VAL0"))};
    ASTERIA_EXPECT(context, "single segment ancestor search and explicit parent", result.Known(),
                   library.GetObject(result.Value).Integer == 23U);
}
void BadInputs(TestContext& context, Aml::Library& library) noexcept
{
    const auto table{Fixture::Table({0x08U, 'N', 'U', 'M', '0', 0x0EU, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U})};
    for (Aml::Size length{0U}; length < table.size(); ++length)
    {
        library.Reset();
        ASTERIA_EXPECT(context, "all table truncations", !library.Load({table.data(), length}));
    }
    auto corrupt{table};
    corrupt.back() ^= 1U;
    library.Reset();
    ASTERIA_EXPECT(context, "bad table checksum", !library.Load({corrupt.data(), corrupt.size()}),
                   library.DiagnosticCount() == 1U, library.GetDiagnostic(0U)->Code == Aml::Reason::Checksum);
    const std::vector<std::vector<Aml::UInt8>> malformed{
        {0x02U},
        {0x08U, '0', 'B', 'A', 'D', 1U},
        {0x10U, 0x00U},
        {0x10U, 0xFFU},
        {0x10U, 0x40U, 0x01U},
        {0x08U, 0x2FU, 0U, 1U},
        {0x08U, 'N', 'A', 'M', 'E', 0x0DU, 'x'},
        {0x5BU, 0xFFU},
        {0x08U, 'N', 'A', 'M', 'E', 0x0CU, 1U},
    };
    for (const auto& body : malformed)
    {
        const auto input{Fixture::Table(body)};
        library.Reset();
        ASTERIA_EXPECT(context, "malformed shape terminates", !library.Load({input.data(), input.size()}),
                       library.DiagnosticCount() > 0U, library.Used().LoadOperations <= Aml::MaximumLoadOperations);
    }
    const auto cycle{Fixture::Table({0x08U, 'S', 'E', 'L', 'F', 'S', 'E', 'L', 'F'})};
    library.Reset();
    ASTERIA_EXPECT(context, "self reference parses", library.Load({cycle.data(), cycle.size()}),
                   library.FinishLoading());
    const auto result{library.Evaluate(library.Find("SELF"))};
    ASTERIA_EXPECT(context, "self reference is bounded Unknown", result.Error == Aml::Reason::Cycle);
}
} // namespace
int main()
{
    TestContext context{"AML parser"};
    const auto arena{std::make_unique<Aml::Arena>()};
    Aml::Library library{arena->Get()};
    Thousands(context, library, *arena);
    Names(context, library);
    BadInputs(context, library);
    return context.Finish();
}
