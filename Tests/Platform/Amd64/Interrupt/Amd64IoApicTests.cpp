// SPDX-License-Identifier: Apache-2.0
#include "Platform/Amd64/Interrupt/Amd64InterruptLines.hpp"
#include "Platform/Ia64/Device/Ia64PortIo.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Interrupt/MessageSignaledSource.hpp"
#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt64;
using Hal::Interrupt::InterruptSourceError;
using Platform::Amd64::Interrupt::Amd64IoApic;
using Platform::Amd64::Interrupt::Amd64IoApicLine;

constexpr UInt64 Base{0x8011'0000U};
constexpr Size Pins{24U};
constexpr UInt64 RemoteRequest{1U << 14U};
constexpr UInt64 Level{1U << 15U};
constexpr UInt64 Masked{1U << 16U};
constexpr UInt64 Writable{0xFFFF'0000'0001'A7FFULL};

/// An I/O APIC model: a level entry
/// delivers while its line is asserted, unmasked and without Remote IRR,
/// setting Remote IRR; a redirection-table write re-evaluates a level entry
/// and drops an edge entry's Remote IRR; the EOI register clears the Remote
/// IRR of every level entry of the vector written.
class IoApicModel final : public Platform::Device::IPhysicalMemoryAccess
{
public:
    [[nodiscard]] Foundation::UInt8 GetMaximumPhysicalAddressBits() const noexcept override
    {
        return 50U;
    }

    [[nodiscard]] bool IsPhysicalRangeImplemented(const UInt64 start, const UInt64 length,
                                                  const Foundation::UInt8) const noexcept override
    {
        return (start >= Base) && ((start + length) <= (Base + 0x2000U));
    }

    [[nodiscard]] UInt64 ReadDoubleWord(const UInt64 address) const noexcept override
    {
        ++Reads;
        if (address != (Base + 0x10U))
        {
            return 0U;
        }
        if (Select == 1U)
        {
            return Version;
        }
        if ((Select >= 0x10U) && (Select < (0x10U + (2U * Pins))))
        {
            const UInt64 entry = Entries[(Select - 0x10U) >> 1U];
            return ((Select - 0x10U) & 1U) ? (entry >> 32U) : (entry & 0xFFFF'FFFFU);
        }
        return 0U;
    }

    void WriteDoubleWord(const UInt64 address, const UInt32 value) const noexcept override
    {
        IoApicModel& model = const_cast<IoApicModel&>(*this);
        if (address == Base)
        {
            model.Select = value;
            return;
        }
        if (address == (Base + 0x40U))
        {
            ++model.Eois;
            for (Size pin{0U}; pin < Pins; ++pin)
            {
                if (((Entries[pin] & 0xFFU) == value) && ((Entries[pin] & Level) != 0U) &&
                    ((Entries[pin] & RemoteRequest) != 0U))
                {
                    model.Entries[pin] &= ~RemoteRequest;
                    model.Update(pin);
                }
            }
            return;
        }
        if ((address != (Base + 0x10U)) || (Select < 0x10U) || (Select >= (0x10U + (2U * Pins))))
        {
            return;
        }
        const Size pin = (Select - 0x10U) >> 1U;
        const UInt64 kept = Entries[pin] & RemoteRequest;
        UInt64 entry = Entries[pin];
        entry = ((Select - 0x10U) & 1U) ? ((entry & 0xFFFF'FFFFU) | (static_cast<UInt64>(value) << 32U))
                                        : ((entry & 0xFFFF'FFFF'0000'0000ULL) | value);
        model.Entries[pin] = (entry & Writable) | kept;
        if ((model.Entries[pin] & Level) == 0U)
        {
            model.Entries[pin] &= ~RemoteRequest;
            return;
        }
        model.Update(pin);
    }

    void SetLine(const Size pin, const bool asserted) noexcept
    {
        Asserted[pin] = asserted;
        if (asserted)
        {
            Update(pin);
        }
    }

    void Update(const Size pin) noexcept
    {
        const UInt64 entry = Entries[pin];
        if (((entry & Masked) != 0U) || ((entry & Level) == 0U) || !Asserted[pin] || ((entry & RemoteRequest) != 0U))
        {
            return;
        }
        Entries[pin] |= RemoteRequest;
        ++Delivered;
        LastVector = entry & 0xFFU;
        LastDestination = entry >> 48U;
    }

    UInt64 Version{((Pins - 1U) << 16U) | 0x20U};
    UInt32 Select{0U};
    UInt64 Entries[Pins]{};
    bool Asserted[Pins]{};
    Size Delivered{0U};
    Size Eois{0U};
    UInt64 LastVector{0U};
    UInt64 LastDestination{0U};
    mutable Size Reads{0U};
};

void TestLine(TestContext& context, const UInt64 version) noexcept
{
    IoApicModel model;
    model.Version = (23U << 16U) | version;
    Amd64IoApic apic;
    Amd64IoApicLine line;
    const bool ready = apic.Configure(model, Base, 16U) && line.Configure(apic, 17U, 0x42U, 0x12U, true, false);
    ASTERIA_EXPECT(context, "IOAPIC discovers version pins GSI base and BSP destination", ready,
                   apic.GetPinCount() == 24U, apic.Serves(16U), !apic.Serves(40U),
                   line.Claim() == InterruptSourceError::None, (model.Entries[1U] & Masked) != 0U,
                   (model.Entries[1U] & Level) != 0U, (model.Entries[1U] & (1U << 13U)) == 0U);
    model.SetLine(1U, true);
    static_cast<void>(line.Unmask());
    // A different vector's Remote IRR must survive this vector's EOI.
    model.Entries[2U] = Level | Masked | RemoteRequest | 0x43U;
    if (version < 0x20U)
    {
        // Even a stale duplicate vector must not broaden the fallback.
        model.Entries[3U] = Level | Masked | RemoteRequest | 0x42U;
    }
    const Size reads = model.Reads;
    ASTERIA_EXPECT(context, "unmask delivers an asserted level to the physical BSP", model.Delivered == 1U,
                   model.LastVector == 0x42U, model.LastDestination == 0x1200U,
                   line.MaskAndAcknowledge() == InterruptSourceError::None,
                   (model.Entries[1U] & (Masked | RemoteRequest)) == Masked, (model.Entries[2U] & RemoteRequest) != 0U,
                   model.Eois == ((version >= 0x20U) ? 2U : 0U), model.Reads - reads == ((version >= 0x20U) ? 0U : 1U),
                   (version >= 0x20U) || ((model.Entries[3U] & RemoteRequest) != 0U));
    static_cast<void>(line.Unmask());
    ASTERIA_EXPECT(context, "masked level retains the asserted request for rearm", model.Delivered == 2U);
    ASTERIA_EXPECT(context, "release leaves the line masked and no remote IRR",
                   line.Release() == InterruptSourceError::None,
                   (model.Entries[1U] & (Masked | RemoteRequest)) == Masked);
}

void TestSharedVectors(TestContext& context) noexcept
{
    IoApicModel model;
    Boot::Acpi::AcpiTopology topology{};
    topology.IoControllerCount = 1U;
    topology.IoControllers[0U] = {.PhysicalAddress = Base,
                                  .GlobalSystemInterruptBase = 0U,
                                  .ControllerId = 0U,
                                  .ControllerKind = Boot::Acpi::AcpiIoControllerKind::IoApic,
                                  .Reserved = 0U};
    topology.InterruptOverrideCount = 1U;
    topology.InterruptOverrides[0U] = {.GlobalSystemInterrupt = 2U, .Flags = 0xFU, .Bus = 0U, .Source = 1U};
    Platform::Amd64::Interrupt::Amd64DeviceVectors vectors;
    Platform::Amd64::Interrupt::Amd64InterruptLines lines;
    const bool configured = vectors.SetDestination(3U) && lines.Configure(topology, model, vectors);
    const auto reserved = lines.Reserve(2U);
    Platform::Synchronization::SpinLock lock;
    Asteria::TestSupport::TestObjectRegistry registry{lock};
    Kernel::Interrupt::MessageSignaledSource source;
    Kernel::Interrupt::Interrupt msi{registry, source};
    const auto message = vectors.Bind(msi);
    ASTERIA_EXPECT(context, "MSI and line reservations share one vector allocator", configured,
                   reserved.Source != nullptr, reserved.Vector == 0x40U, message.Vector == 0x41U,
                   lines.Reserve(2U).Source == nullptr, lines.Reserve(24U).Source == nullptr,
                   reserved.Source->Claim() == InterruptSourceError::None,
                   (model.Entries[2U] & (Level | (1U << 13U) | Masked)) == (Level | (1U << 13U) | Masked));
    static_cast<void>(reserved.Source->Release());
    lines.Release(reserved.Vector);
    ASTERIA_EXPECT(context, "released lines leave hardware masked and return shared vectors",
                   (model.Entries[2U] & Masked) != 0U, vectors.Unbind(message.Vector, msi));
    UInt64 held[16]{};
    for (Size index{0U}; index < 16U; ++index)
    {
        held[index] = vectors.Reserve();
    }
    ASTERIA_EXPECT(context, "bounded allocator refuses exhaustion", vectors.Reserve() == 0U);
    for (const UInt64 vector : held)
    {
        vectors.Release(vector);
    }
    ASTERIA_EXPECT(context, "releasing reservations permits reuse", vectors.Reserve() != 0U);
    vectors.Dispatch(0x4FU);
    ASTERIA_EXPECT(context, "reserved unpublished vectors are safe spurious deliveries", vectors.GetSpurious() == 1U);
}

void TestSparsePorts(TestContext& context) noexcept
{
    using Platform::Ia64::Device::Ia64PortIo;
    constexpr UInt64 base{0xFFFF'C000'000U};
    ASTERIA_EXPECT(context, "IA64 ports use the firmware sparse address mapping",
                   Ia64PortIo::SparseAddress(base, 0x60U) == (base + 0x18060U),
                   Ia64PortIo::SparseAddress(base, 0x64U) == (base + 0x19064U),
                   Ia64PortIo::SparseAddress(base, 0xFFFFU) == (base + 0x3FFF'FFFU));
    Ia64PortIo ports;
    ASTERIA_EXPECT(context, "SDM physical port blocks need a complete 64 MiB aligned window",
                   !ports.Configure(0U, 0x4000000U), !ports.Configure(base + 1U, 0x4000000U),
                   !ports.Configure(base, 0x3FFFFFFU), !ports.Configure(1ULL << 63U, 0x4000000U),
                   ports.Configure(base, 0x4000000U), !ports.Configure(base, 0x4000000U));
}

void TestOptionalRouting(TestContext& context) noexcept
{
    IoApicModel model;
    Boot::Acpi::AcpiTopology topology{};
    topology.IoControllerCount = 3U;
    topology.IoControllers[0U] = {.PhysicalAddress = 0U,
                                  .GlobalSystemInterruptBase = 0U,
                                  .ControllerId = 0U,
                                  .ControllerKind = Boot::Acpi::AcpiIoControllerKind::IoApic,
                                  .Reserved = 0U};
    topology.IoControllers[1U] = topology.IoControllers[0U];
    topology.IoControllers[1U].PhysicalAddress = Base + 1U;
    topology.IoControllers[2U] = topology.IoControllers[0U];
    topology.IoControllers[2U].PhysicalAddress = Base;
    topology.InterruptOverrideCount = 3U;
    topology.InterruptOverrides[0U] = {.GlobalSystemInterrupt = 1U, .Flags = 0xFU, .Bus = 1U, .Source = 1U};
    topology.InterruptOverrides[1U] = {.GlobalSystemInterrupt = 1U, .Flags = 2U, .Bus = 0U, .Source = 1U};
    topology.InterruptOverrides[2U] = {.GlobalSystemInterrupt = 1U, .Flags = 8U, .Bus = 0U, .Source = 1U};
    Platform::Amd64::Interrupt::Amd64DeviceVectors vectors;
    Platform::Amd64::Interrupt::Amd64InterruptLines lines;
    ASTERIA_EXPECT(context, "unusable controllers and malformed overrides are skipped",
                   lines.Configure(topology, model, vectors), lines.CanRoute(1U), !lines.CanRoute(24U),
                   lines.Reserve(1U).Source == nullptr, vectors.SetDestination(7U));
    const auto isa = lines.Reserve(1U);
    ASTERIA_EXPECT(context, "ISA defaults are edge high and other GSIs require policy", isa.Source != nullptr,
                   isa.Source->Claim() == InterruptSourceError::None, (model.Entries[1U] & (Level | (1U << 13U))) == 0U,
                   lines.Reserve(16U).Source == nullptr, lines.SetPolicy(16U, true, true));
    static_cast<void>(isa.Source->Release());
    lines.Release(isa.Vector);
    const auto other = lines.Reserve(16U);
    ASTERIA_EXPECT(context, "publisher policy enables a non-ISA GSI", other.Source != nullptr,
                   other.Source->Claim() == InterruptSourceError::None,
                   (model.Entries[16U] & (Level | (1U << 13U))) == (Level | (1U << 13U)));
    static_cast<void>(other.Source->Release());
    lines.Release(other.Vector);
    Platform::Amd64::Interrupt::Amd64InterruptLines absent;
    model.Version = 0xFU;
    ASTERIA_EXPECT(context, "a router without a usable version leaves lines unavailable",
                   !absent.Configure(topology, model, vectors), !absent.CanRoute(1U),
                   absent.Reserve(1U).Source == nullptr);
    Amd64IoApic apic;
    Amd64IoApicLine line;
    model.Version = (23U << 16U) | 0x20U;
    ASTERIA_EXPECT(context, "broadcast physical destinations are explicitly refused", apic.Configure(model, Base, 0U),
                   !line.Configure(apic, 1U, 0x40U, 0xFFU, false, false),
                   !line.Configure(apic, 1U, 0x40U, 0x100U, false, false),
                   !line.Configure(apic, 1U, 0x40U, 0x12345678U, false, false));
    Amd64IoApic floating;
    model.Version = 0xFFFF'FFFFU;
    ASTERIA_EXPECT(context, "a floating IOAPIC bus is unavailable", !floating.Configure(model, Base, 0U));
    model.Version = (120U << 16U) | 0x20U;
    ASTERIA_EXPECT(context, "redirection count must fit the hardware selector", !floating.Configure(model, Base, 0U));
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext context{"Amd64IoApicTests"};
    Asteria::Test::TestLine(context, 0x20U);
    Asteria::Test::TestLine(context, 0x11U);
    Asteria::Test::TestSharedVectors(context);
    Asteria::Test::TestSparsePorts(context);
    Asteria::Test::TestOptionalRouting(context);
    return context.Finish();
}
