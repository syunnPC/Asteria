// SPDX-License-Identifier: Apache-2.0
#include "Acpi/Aml/Facts.hpp"

#include <memory>
#include <vector>

#include "Acpi/Aml/Fixture.hpp"
#include "Support/TestContext.hpp"

namespace
{
namespace Aml = Asteria::Acpi::Aml;
namespace Fixture = Asteria::TestSupport::Aml;
using Asteria::TestSupport::TestContext;

void Run(TestContext& context, Aml::Library& library) noexcept
{
    std::vector<Aml::UInt8> table;
    ASTERIA_EXPECT(context, "local shadow fixture", Fixture::Fixture("LocalShadow", table));
    library.Reset();
    ASTERIA_EXPECT(context, "local data shadows ancestor method grammar", library.Load({table.data(), table.size()}),
                   library.FinishLoading());
    for (Aml::Size call{0U}; call < 2U; ++call)
    {
        const auto package{library.Evaluate(library.Find("PACK"))};
        ASTERIA_EXPECT(context, "local package lifetime and value", package.Known(),
                       library.GetObject(package.Value).Kind == Aml::ObjectKind::Package,
                       library.GetObject(package.Value).Count == 3U,
                       library.GetObject(library.Element(package.Value, 2U)).Integer == 6U);
        const auto byte{library.Evaluate(library.Find("BYTE"))};
        ASTERIA_EXPECT(context, "local buffer field shadows ancestor method", byte.Known(),
                       library.GetObject(byte.Value).Integer == 0x72U);
    }
    ASTERIA_EXPECT(context, "Debug fixture", Fixture::Fixture("DebugOutput", table));
    library.Reset();
    std::vector<Aml::UInt8> external;
    ASTERIA_EXPECT(context, "late External fixture", Fixture::Fixture("ExternalAfter", external));
    ASTERIA_EXPECT(context, "Debug module output does not prevent loading", library.Load({table.data(), table.size()}),
                   library.Load({external.data(), external.size()}), library.FinishLoading(),
                   library.EffectCount() == 2U);
    const auto done{library.Evaluate(library.Find("DONE"))};
    const auto out{library.Evaluate(library.Find("OUTP"))};
    ASTERIA_EXPECT(context, "Debug consumes package and reports object type", done.Known(),
                   library.GetObject(done.Value).Integer == 3U, out.Known(),
                   library.GetObject(out.Value).Integer == 16U, library.GetEffect(2U)->Opcode == 0x5B31U,
                   library.GetObject(library.GetEffect(2U)->Value).Kind == Aml::ObjectKind::Package);
    const auto again{library.Evaluate(library.Find("AGAN"))};
    ASTERIA_EXPECT(context, "later External preserves original method body", again.Known(),
                   library.GetObject(again.Value).Integer == 16U);
    const auto timer{library.Evaluate(library.Find("TIME"))};
    ASTERIA_EXPECT(context, "clock is unknown when its value matters", timer.Error == Aml::Reason::Unsupported);
    ASTERIA_EXPECT(context, "recovery fixture", Fixture::Fixture("ModuleRecovery", table));
    library.Reset();
    ASTERIA_EXPECT(context, "hardware-dependent module is partial", library.Load({table.data(), table.size()}),
                   !library.FinishLoading());
    const auto keep{library.Evaluate(library.Find("KEEP"))};
    const auto late{library.Evaluate(library.Find("LATE"))};
    const auto temp{library.Evaluate(library.Find("TEMP"))};
    ASTERIA_EXPECT(context, "failed statement rolls back stores and loading continues", keep.Known(),
                   library.GetObject(keep.Value).Integer == 9U, late.Known(),
                   library.GetObject(late.Value).Integer == 5U, temp.Known(),
                   library.GetObject(temp.Value).Integer == 1U);
    Aml::Facts facts{library};
    const auto device{std::make_unique<Aml::DeviceFacts>()};
    Aml::Size unknown{0U};
    for (Aml::Id node{facts.NextDevice()}; node != Aml::NoId; node = facts.NextDevice(node))
    {
        facts.ReadDevice(node, *device);
        ASTERIA_EXPECT(context, "both unknown predicate alternatives stay unknown",
                       device->Sta == Aml::Presence::Unknown, device->Hid.Error == Aml::Reason::RegionAccess);
        ++unknown;
    }
    ASTERIA_EXPECT(context, "both alternatives retained", unknown == 2U);
}
} // namespace

int main()
{
    TestContext context{"Firmware AML constructs"};
    const auto arena{std::make_unique<Aml::Arena>()};
    Aml::Library library{arena->Get()};
    Run(context, library);
    return context.Finish();
}
