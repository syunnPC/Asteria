// SPDX-License-Identifier: Apache-2.0
#include "Platform/Amd64/Boot/I8042Gate.hpp"

#include <initializer_list>

#include "Boot/Acpi/I8042Evidence.hpp"
#include "Kernel/Boot/I8042Publication.hpp"
#include "Support/TestContext.hpp"

namespace F = Asteria::Foundation;
namespace B = Asteria::Platform::Amd64::Bootstrap;
namespace A = Asteria::Boot::Acpi;
namespace K = Asteria::Boot::Bootstrap;
namespace
{
class Ports final : public Asteria::Hal::Device::IPortIo
{
public:
    F::UInt32 Status{0U};
    F::Size Reads{0U};
    F::Size Writes{0U};
    bool Valid{true};
    F::UInt32 Read(F::UInt16 port, F::UInt8 width) noexcept override
    {
        ++Reads;
        Valid = Valid && port == 0x64U && width == 1U;
        return Status;
    }
    void Write(F::UInt16, F::UInt8, F::UInt32) noexcept override
    {
        ++Writes;
    }
};
} // namespace
int main()
{
    Asteria::TestSupport::TestContext context{"I8042 publication gates and DSDT binding"};
    for (F::UInt32 flags{0U}; flags < 16U; ++flags)
    {
        for (const bool fadt : {false, true})
        {
            for (const bool matches : {false, true})
            {
                for (const F::UInt32 status : {0U, 0x20U, 0xFEU, 0xFFU})
                {
                    Ports ports{};
                    ports.Status = status;
                    const bool evidence = fadt || (matches && (flags & 2U) != 0U);
                    const auto result = B::GateI8042(fadt ? 2U : 0U, flags, matches, ports);
                    B::I8042PublicationReason reason{B::I8042PublicationReason::Published};
                    if (!evidence)
                    {
                        reason = flags != 0U && !matches ? B::I8042PublicationReason::DsdtMismatch
                                                         : B::I8042PublicationReason::NoEvidence;
                    }
                    else if (status == 0xFFU)
                    {
                        reason = B::I8042PublicationReason::FloatingPort;
                    }
                    ASTERIA_EXPECT(context, "FADT and AML both require one nonfloating status read",
                                   result.Reason == reason, ports.Reads == (evidence ? 1U : 0U), ports.Writes == 0U,
                                   ports.Valid, (result.Declared != 0U) == (evidence && status != 0xFFU));
                    // The signaling bits never reach the published declared word,
                    // and route only a publication from namespace evidence alone.
                    const auto signaled = B::GateI8042(fadt ? 2U : 0U, flags | 0x3F0U, matches, ports);
                    const bool aml = !fadt && signaled.Reason == B::I8042PublicationReason::Published;
                    ASTERIA_EXPECT(context, "IRQ signaling evidence is boot-only and AML-publication-only",
                                   signaled.Declared == result.Declared, (signaled.Declared & ~F::UInt64{15U}) == 0U,
                                   signaled.AmlInterrupts == (aml ? 0x3F0U : 0U), result.AmlInterrupts == 0U);
                }
            }
        }
    }
    static A::AcpiTopology topology{};
    constexpr F::UInt64 address{0xFEDC'BA98'7654'3000ULL};
    constexpr F::UInt32 length{4096U};
    ASTERIA_EXPECT(context, "old zero-reserved boot has no evidence",
                   !A::I8042Evidence::Matches(topology, address, length, 47U));
    A::I8042Evidence::Record(topology, 0xFFFF'FC0EU, 0x030BU, 47U, address, length);
    ASTERIA_EXPECT(context, "pointer-free bound evidence drops bits beyond the evidence mask",
                   A::I8042Evidence::Matches(topology, address, length, 47U), topology.Reserved == 14U,
                   !A::I8042Evidence::Matches(topology, address + 4096U, length, 47U),
                   !A::I8042Evidence::Matches(topology, address, length + 1U, 47U),
                   !A::I8042Evidence::Matches(topology, address, length, 48U));
    A::I8042Evidence::Record(topology, 0x3F2U, 0x030BU, 47U, address, length);
    ASTERIA_EXPECT(context, "IRQ signaling bits round-trip through the topology",
                   A::I8042Evidence::Matches(topology, address, length, 47U), topology.Reserved == 0x3F2U);
    topology.Reserved = 0x402U;
    ASTERIA_EXPECT(context, "an evidence bit beyond the signaling bits is refused",
                   !A::I8042Evidence::Matches(topology, address, length, 47U));
    topology.Reserved = 0x3F2U;
    topology.TrailingReserved = 0x030CU;
    ASTERIA_EXPECT(context, "unknown reason ID refused", !A::I8042Evidence::Matches(topology, address, length, 47U));
    topology.InterruptOverrideCount = 1U;
    auto& entry = topology.InterruptOverrides[0U];
    entry.Bus = 0U;
    entry.Source = 12U;
    entry.GlobalSystemInterrupt = 43U;
    // AML evidence that disagrees with every override: mouse level/high,
    // keyboard edge/low (Abi/I8042.hpp signaling bits).
    constexpr F::UInt32 aml{0x10U | 0x40U | 0x80U | 0x100U};
    for (F::UInt16 flags{0U}; flags < 32U; ++flags)
    {
        entry.Flags = flags;
        const bool valid = (flags < 16U) && ((flags & 3U) != 2U) && (((flags >> 2U) & 3U) != 2U);
        for (const F::UInt32 interrupts : {0U, aml})
        {
            const auto route = K::ResolveI8042Route(topology, 12U, interrupts);
            const auto keyboard = K::ResolveI8042Route(topology, 1U, interrupts);
            const bool mouseAml = !valid && interrupts != 0U;
            ASTERIA_EXPECT(
                context, "a valid ISA override decides GSI, polarity and trigger, conforming meaning ISA edge/high",
                route.Gsi == (valid ? 43U : 12U), route.ActiveLow == (valid && (flags & 3U) == 3U),
                route.Level == (valid ? ((flags >> 2U) & 3U) == 3U : mouseAml),
                route.Source == (valid ? K::I8042RouteSource::SourceOverride
                                       : (mouseAml ? K::I8042RouteSource::Aml : K::I8042RouteSource::IsaDefault)),
                keyboard.Gsi == 1U, keyboard.ActiveLow == (interrupts != 0U), !keyboard.Level,
                keyboard.Source == (interrupts != 0U ? K::I8042RouteSource::Aml : K::I8042RouteSource::IsaDefault));
        }
    }
    topology.InterruptOverrideCount = 0U;
    const auto level = K::ResolveI8042Route(topology, 1U, 0x10U | 0x20U | 0x40U);
    const auto mouseLow = K::ResolveI8042Route(topology, 12U, 0x80U | 0x200U);
    const auto undeclared = K::ResolveI8042Route(topology, 1U, 0x20U | 0x40U | 0x80U | 0x100U | 0x200U);
    const auto other = K::ResolveI8042Route(topology, 4U, 0x3F0U);
    ASTERIA_EXPECT(context, "AML signaling applies per IRQ and only when declared; ISA default is edge/high",
                   level.Level, level.ActiveLow, level.Source == K::I8042RouteSource::Aml, !mouseLow.Level,
                   mouseLow.ActiveLow, mouseLow.Gsi == 12U, !undeclared.Level, !undeclared.ActiveLow,
                   undeclared.Source == K::I8042RouteSource::IsaDefault, !other.Level, !other.ActiveLow,
                   other.Gsi == 4U, other.Source == K::I8042RouteSource::IsaDefault);
    topology.InterruptOverrideCount = 1U;
    entry.Flags = 15U;
    entry.Bus = 1U;
    ASTERIA_EXPECT(context, "non-ISA source override cannot redirect the mouse",
                   K::ResolveI8042Route(topology, 12U, 0U).Gsi == 12U);
    topology.InterruptOverrideCount = 0xFFFF'FFFFU;
    ASTERIA_EXPECT(context, "an override count beyond the topology's capacity reads no further entries",
                   K::ResolveI8042Route(topology, 12U, 0U).Gsi == 12U);
    return context.Finish();
}
