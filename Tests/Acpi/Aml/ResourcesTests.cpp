// SPDX-License-Identifier: Apache-2.0
#include "Acpi/Aml/Resources.hpp"

#include <memory>
#include <vector>

#include "Acpi/Aml/Fixture.hpp"
#include "Support/TestContext.hpp"

namespace
{
namespace Aml = Asteria::Acpi::Aml;
namespace Fixture = Asteria::TestSupport::Aml;
using Asteria::TestSupport::TestContext;
void FixtureResources(TestContext& context, Aml::Library& library, Aml::Resources& resources) noexcept
{
    std::vector<Aml::UInt8> table;
    if (!ASTERIA_EXPECT(context, "resource fixture", Fixture::Fixture("Resources", table),
                        library.Load({table.data(), table.size()}), library.FinishLoading()))
    {
        return;
    }
    const auto result{library.Evaluate(library.Find("RES0"))};
    ASTERIA_EXPECT(context, "resource decoding", result.Known(),
                   Aml::ResourceDecoder::Decode(library.Data(result.Value), resources), resources.Count == 13U);
    constexpr Aml::ResourceKind expected[]{Aml::ResourceKind::Irq,          Aml::ResourceKind::Dma,
                                           Aml::ResourceKind::ExtendedIrq,  Aml::ResourceKind::Memory24,
                                           Aml::ResourceKind::Memory32,     Aml::ResourceKind::FixedMemory32,
                                           Aml::ResourceKind::WordAddress,  Aml::ResourceKind::DWordAddress,
                                           Aml::ResourceKind::QWordAddress, Aml::ResourceKind::ExtendedAddress,
                                           Aml::ResourceKind::Vendor,       Aml::ResourceKind::Vendor,
                                           Aml::ResourceKind::End};
    for (Aml::Size i{0U}; i < resources.Count; ++i)
    {
        ASTERIA_EXPECT(context, "descriptor kind", resources.Items[i].Kind == expected[i]);
    }
    ASTERIA_EXPECT(context, "extended IRQ source", resources.Items[2U].Interrupt == 20U,
                   resources.Items[2U].Length == 2U, resources.Items[2U].Source.Length == 9U);
    ASTERIA_EXPECT(context, "memory and address widths", resources.Items[3U].Minimum == 0x100000U,
                   resources.Items[4U].Minimum == 0x100000U, resources.Items[5U].Length == 0x1000U,
                   resources.Items[8U].Minimum == 0x100000000ULL, resources.Items[9U].Minimum == 0x200000000ULL);
}
void Boundaries(TestContext& context, Aml::Resources& resources) noexcept
{
    constexpr Aml::UInt8 shortIrq[]{0x22U, 0x02U, 0x00U, 0x79U, 0U};
    ASTERIA_EXPECT(context, "IRQ no flags defaults",
                   Aml::ResourceDecoder::Decode({shortIrq, sizeof(shortIrq)}, resources),
                   resources.Items[0U].Flags == 1U, resources.Items[0U].Interrupt == 2U);
    for (Aml::Size length{0U}; length < sizeof(shortIrq); ++length)
    {
        ASTERIA_EXPECT(context, "truncated resource fails",
                       !Aml::ResourceDecoder::Decode({shortIrq, length}, resources));
    }
    constexpr Aml::UInt8 checked[]{0x22U, 2U, 0U, 0x79U, 0x63U};
    ASTERIA_EXPECT(context, "resource checksum", Aml::ResourceDecoder::Decode({checked, sizeof(checked)}, resources));
    constexpr Aml::UInt8 badChecksum[]{0x22U, 2U, 0U, 0x79U, 0x64U};
    ASTERIA_EXPECT(context, "bad resource checksum",
                   !Aml::ResourceDecoder::Decode({badChecksum, sizeof(badChecksum)}, resources),
                   resources.Error == Aml::Reason::Checksum);
    std::vector<Aml::UInt8> connections;
    for (const Aml::UInt8 tag : {Aml::UInt8{0x8CU}, Aml::UInt8{0x8DU}, Aml::UInt8{0x8FU}, Aml::UInt8{0x90U},
                                 Aml::UInt8{0x91U}, Aml::UInt8{0x92U}})
    {
        connections.push_back(tag);
        connections.push_back(20U);
        connections.push_back(0U);
        connections.insert(connections.end(), 20U, 0U);
    }
    for (Aml::UInt8 type{1U}; type <= 3U; ++type)
    {
        connections.insert(connections.end(), {0x8EU, 9U, 0U, 2U, 0U, type, 0U, 0U, 0U, 1U, 0U, 0U});
    }
    connections.insert(connections.end(), {0x79U, 0U});
    ASTERIA_EXPECT(
        context, "connections recognized and skipped",
        Aml::ResourceDecoder::Decode({connections.data(), connections.size()}, resources), resources.Count == 10U,
        resources.Items[0U].Kind == Aml::ResourceKind::Gpio, resources.Items[6U].Kind == Aml::ResourceKind::I2c,
        resources.Items[7U].Kind == Aml::ResourceKind::Spi, resources.Items[8U].Kind == Aml::ResourceKind::Uart);
}
} // namespace
int main()
{
    TestContext context{"AML resources"};
    const auto arena{std::make_unique<Aml::Arena>()};
    const auto resources{std::make_unique<Aml::Resources>()};
    Aml::Library library{arena->Get()};
    FixtureResources(context, library, *resources);
    Boundaries(context, *resources);
    return context.Finish();
}
