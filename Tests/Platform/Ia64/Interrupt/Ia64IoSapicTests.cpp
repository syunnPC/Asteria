// SPDX-License-Identifier: Apache-2.0
#include "Platform/Ia64/Interrupt/Ia64InterruptLines.hpp"
#include "Platform/Ia64/Interrupt/Ia64IoSapic.hpp"

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
using Platform::Ia64::Interrupt::Ia64IoSapic;
using Platform::Ia64::Interrupt::Ia64IoSapicLine;
using Polarity = Platform::Ia64::Interrupt::Ia64LinePolarity;
using Trigger = Platform::Ia64::Interrupt::Ia64LineTrigger;

constexpr UInt64 Base{0x8011'0000U};
constexpr Size Pins{24U};
constexpr UInt64 RemoteRequest{1U << 14U};
constexpr UInt64 Level{1U << 15U};
constexpr UInt64 Masked{1U << 16U};
constexpr UInt64 Writable{0xFFFF'0000'0001'A7FFULL};

/// Register/interrupt interleaving model for the public IoSapic contract: a level entry
/// delivers while its line is asserted, unmasked and without Remote IRR,
/// setting Remote IRR; an edge entry delivers on an asserting transition
/// while unmasked and drops one while masked; a redirection-table write
/// re-evaluates a level entry and drops an edge entry's Remote IRR; the EOI
/// register clears the Remote IRR of every level entry of the vector written.
class IoSapicModel final : public Platform::Device::IPhysicalMemoryAccess
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
        IoSapicModel& model = const_cast<IoSapicModel&>(*this);
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
        const bool rising = asserted && !Asserted[pin];
        Asserted[pin] = asserted;
        const UInt64 entry = Entries[pin];
        if (rising && ((entry & Level) == 0U) && ((entry & Masked) == 0U))
        {
            ++Delivered;
            LastVector = entry & 0xFFU;
            LastDestination = entry >> 48U;
            return;
        }
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

    UInt64 Version{((Pins - 1U) << 16U) | 0x11U};
    UInt32 Select{0U};
    UInt64 Entries[Pins]{};
    bool Asserted[Pins]{};
    Size Delivered{0U};
    Size Eois{0U};
    UInt64 LastVector{0U};
    UInt64 LastDestination{0U};
    mutable Size Reads{0U};
};

void TestConfiguration(TestContext& runner) noexcept
{
    IoSapicModel model;
    Ia64IoSapic sapic;
    const bool configured = sapic.Configure(model, Base, 0U);
    const bool again = sapic.Configure(model, Base, 0U);
    IoSapicModel old;
    old.Version = (23U << 16U) | 0x10U;
    Ia64IoSapic ancient;
    const bool refused = ancient.Configure(old, Base, 0U);
    Ia64IoSapic elsewhere;
    const bool outside = elsewhere.Configure(model, 0x9000'0000U, 0U);
    ASTERIA_EXPECT(runner, "a controller reports 24 pins and an EOI register; older ones and other addresses refuse",
                   configured, !again, sapic.GetPinCount() == 24U, sapic.Serves(16U), !sapic.Serves(24U), !refused,
                   !outside);
}

// The line's states as a driver's Interrupt drives them: delivered once
// while asserted, kept from delivering again until unmasked, delivered again
// on unmask while still asserted, never once the device is acknowledged.
void TestLevelLine(TestContext& runner) noexcept
{
    IoSapicModel model;
    Ia64IoSapic sapic;
    Ia64IoSapicLine line;
    // A Remote IRR an earlier owner left set.
    model.Entries[16U] = 0x45U | Level | RemoteRequest | Masked;
    const bool configured = sapic.Configure(model, Base, 0U) &&
                            line.Configure(sapic, 16U, 0x41U, 0U, 0U, Polarity::ActiveLow, Trigger::Level);
    const bool unconfigured = !line.Configure(sapic, 30U, 0x41U, 0U, 0U, Polarity::ActiveLow, Trigger::Level);
    const InterruptSourceError claimed = line.Claim();
    const InterruptSourceError twice = line.Claim();
    const UInt64 fresh = model.Entries[16U];
    model.SetLine(16U, true);
    const Size whileMasked = model.Delivered;
    const InterruptSourceError unmasked = line.Unmask();
    const Size delivered = model.Delivered;
    const InterruptSourceError acknowledged = line.MaskAndAcknowledge();
    const UInt64 afterAcknowledge = model.Entries[16U];
    // Rearmed while the device still asserts its line: delivered again.
    const InterruptSourceError rearmed = line.Unmask();
    const Size again = model.Delivered;
    static_cast<void>(line.MaskAndAcknowledge());
    // The driver acknowledges its device, then rearms: nothing more.
    model.SetLine(16U, false);
    static_cast<void>(line.Unmask());
    const Size quiet = model.Delivered;
    const InterruptSourceError masked = line.Mask();
    const InterruptSourceError released = line.Release();
    const InterruptSourceError unclaimed = line.Unmask();
    ASTERIA_EXPECT(runner, "a claimed line is masked, level, active low, without an earlier owner's Remote IRR",
                   configured, unconfigured, claimed == InterruptSourceError::None,
                   twice == InterruptSourceError::AlreadyClaimed, (fresh & 0xFFU) == 0x41U, (fresh & Masked) != 0U,
                   (fresh & Level) != 0U, (fresh & (1U << 13U)) != 0U, (fresh & RemoteRequest) == 0U,
                   whileMasked == 0U);
    ASTERIA_EXPECT(runner, "an asserted line is delivered once per unmask, and never once the device is quiet",
                   unmasked == InterruptSourceError::None, delivered == 1U, model.LastVector == 0x41U,
                   acknowledged == InterruptSourceError::None, (afterAcknowledge & Masked) != 0U,
                   (afterAcknowledge & RemoteRequest) == 0U, rearmed == InterruptSourceError::None, again == 2U,
                   quiet == 2U, masked == InterruptSourceError::None, released == InterruptSourceError::None,
                   unclaimed == InterruptSourceError::NotClaimed, (model.Entries[16U] & Masked) != 0U);
}

// An ISA line programmed edge-triggered: an asserting transition while masked is dropped, so an
// unmask while the device still asserts delivers nothing (the driver drains its device after
// re-arming); the next transition delivers once, and the EOI of a masked edge entry changes nothing
// but the mask.
void TestEdgeLine(TestContext& runner) noexcept
{
    IoSapicModel model;
    Ia64IoSapic sapic;
    Ia64IoSapicLine line;
    const bool configured = sapic.Configure(model, Base, 0U) &&
                            line.Configure(sapic, 1U, 0x43U, 0U, 0U, Polarity::ActiveHigh, Trigger::Edge) &&
                            (line.Claim() == InterruptSourceError::None);
    const UInt64 fresh = model.Entries[1U];
    model.SetLine(1U, true);
    const InterruptSourceError unmasked = line.Unmask();
    const Size held = model.Delivered;
    model.SetLine(1U, false);
    model.SetLine(1U, true);
    const Size edge = model.Delivered;
    const InterruptSourceError acknowledged = line.MaskAndAcknowledge();
    const UInt64 afterAcknowledge = model.Entries[1U];
    model.SetLine(1U, false);
    model.SetLine(1U, true);
    const Size whileMasked = model.Delivered;
    ASTERIA_EXPECT(runner, "an edge line is masked, edge, active high when claimed", configured,
                   (fresh & 0xFFU) == 0x43U, (fresh & Masked) != 0U, (fresh & Level) == 0U,
                   (fresh & (1U << 13U)) == 0U);
    ASTERIA_EXPECT(runner, "an edge line delivers only an unmasked asserting transition",
                   unmasked == InterruptSourceError::None, held == 0U, edge == 1U, model.LastVector == 0x43U,
                   acknowledged == InterruptSourceError::None, afterAcknowledge == (fresh | Masked), whileMasked == 1U);
}

// The destination is the processor's local SAPIC id and eid.
void TestDestination(TestContext& runner) noexcept
{
    IoSapicModel model;
    Ia64IoSapic sapic;
    Ia64IoSapicLine line;
    const bool configured = sapic.Configure(model, Base, 16U) &&
                            line.Configure(sapic, 17U, 0x42U, 0x12U, 0x34U, Polarity::ActiveLow, Trigger::Level) &&
                            (line.Claim() == InterruptSourceError::None);
    model.SetLine(1U, true);
    static_cast<void>(line.Unmask());
    ASTERIA_EXPECT(runner, "a line of a controller based at GSI 16 delivers to its id and eid", configured,
                   model.Delivered == 1U, model.LastVector == 0x42U, model.LastDestination == 0x1234U);
}
// One Interrupt per line, vectors from 0x40 in rotation, published once the Interrupt exists; a
// vector with no Interrupt is spurious and still leaves its line masked without Remote IRR.
void TestLines(TestContext& runner) noexcept
{
    IoSapicModel model;
    Ia64IoSapic sapic;
    Platform::Ia64::Interrupt::Ia64InterruptLines lines;
    const bool unconfigured = lines.Reserve(16U).Source == nullptr;
    const bool configured =
        sapic.Configure(model, Base, 0U) && lines.Configure(sapic, 0U, 0U) && !lines.Configure(sapic, 0U, 0U);
    const auto first = lines.Reserve(16U);
    const auto shared = lines.Reserve(16U);
    const auto unrouted = lines.Reserve(40U);
    const auto other = lines.Reserve(17U);
    // A delivery before any Interrupt is published: spurious, silenced.
    const bool claimed = (first.Source != nullptr) && (first.Source->Claim() == InterruptSourceError::None);
    model.SetLine(16U, true);
    static_cast<void>(first.Source->Unmask());
    lines.Dispatch(first.Vector);
    const UInt64 silenced = model.Entries[16U];
    lines.Release(first.Vector);
    const auto again = lines.Reserve(16U);
    ASTERIA_EXPECT(runner, "lines take vectors in rotation, one Interrupt per line; an unbound vector is silenced",
                   unconfigured, configured, first.Source != nullptr, first.Vector == 0x40U, shared.Source == nullptr,
                   unrouted.Source == nullptr, other.Vector == 0x41U, claimed, model.Delivered == 1U,
                   lines.GetSpurious() == 1U, lines.GetDelivered() == 0U, (silenced & Masked) != 0U,
                   (silenced & RemoteRequest) == 0U, again.Vector == 0x42U);
}

// A recovery reserves the same line again under the next vector; a late
// delivery of the released vector, one the processor latched before the
// release, must not touch the pin the new owner armed.
void TestLateVectorAfterRelease(TestContext& runner) noexcept
{
    IoSapicModel model;
    Ia64IoSapic sapic;
    Platform::Ia64::Interrupt::Ia64InterruptLines lines;
    const bool configured = sapic.Configure(model, Base, 0U) && lines.Configure(sapic, 0U, 0U);
    const auto first = lines.Reserve(16U);
    const bool claimed = (first.Source != nullptr) && (first.Source->Claim() == InterruptSourceError::None) &&
                         (first.Source->Release() == InterruptSourceError::None);
    lines.Release(first.Vector);
    const auto second = lines.Reserve(16U);
    const bool armed = (second.Source != nullptr) && (second.Source->Claim() == InterruptSourceError::None) &&
                       (second.Source->Unmask() == InterruptSourceError::None);
    lines.Dispatch(first.Vector);
    const UInt64 entry = model.Entries[16U];
    ASTERIA_EXPECT(runner, "a late delivery of a released vector leaves the line its new owner armed", configured,
                   claimed, armed, first.Vector == 0x40U, second.Vector == 0x41U, (entry & 0xFFU) == 0x41U,
                   (entry & Masked) == 0U, lines.GetSpurious() == 1U, lines.GetDelivered() == 0U);
}
void TestIsaPolicies(TestContext& runner) noexcept
{
    IoSapicModel model;
    Ia64IoSapic sapic;
    Platform::Ia64::Interrupt::Ia64InterruptLines lines;
    const bool configured = sapic.Configure(model, Base, 0U) && lines.Configure(sapic, 0x12U, 0x34U);
    const bool policies = lines.SetIsaPolicy(1U, Polarity::ActiveHigh, Trigger::Edge) &&
                          lines.SetIsaPolicy(12U, Polarity::ActiveLow, Trigger::Level) &&
                          !lines.SetIsaPolicy(1U, Polarity::ActiveLow, Trigger::Level) &&
                          !lines.SetIsaPolicy(40U, Polarity::ActiveHigh, Trigger::Edge);
    const auto keyboard = lines.Reserve(1U);
    const auto mouse = lines.Reserve(12U);
    const auto pci = lines.Reserve(16U);
    const bool claimed = keyboard.Source != nullptr && mouse.Source != nullptr && pci.Source != nullptr &&
                         keyboard.Source->Claim() == InterruptSourceError::None &&
                         mouse.Source->Claim() == InterruptSourceError::None &&
                         pci.Source->Claim() == InterruptSourceError::None;
    ASTERIA_EXPECT(runner, "ISA polarity and trigger change only their own lines; PCI stays low/level", configured,
                   policies, claimed, (model.Entries[1U] & (1U << 13U)) == 0U, (model.Entries[12U] & (1U << 13U)) != 0U,
                   (model.Entries[16U] & (1U << 13U)) != 0U, (model.Entries[1U] & Level) == 0U,
                   (model.Entries[12U] & Level) != 0U, (model.Entries[16U] & Level) != 0U);
}

void TestAdditionalController(TestContext& runner) noexcept
{
    IoSapicModel firstModel;
    IoSapicModel secondModel;
    Ia64IoSapic first;
    Ia64IoSapic second;
    Platform::Ia64::Interrupt::Ia64InterruptLines lines;
    const bool configured = first.Configure(firstModel, Base, 0U) && second.Configure(secondModel, Base, 32U) &&
                            lines.Configure(first, 0U, 0U) && lines.AddController(second) &&
                            !lines.AddController(first);
    const auto low = lines.Reserve(1U);
    const auto high = lines.Reserve(44U);
    const bool claimed = low.Source != nullptr && high.Source != nullptr &&
                         low.Source->Claim() == InterruptSourceError::None &&
                         high.Source->Claim() == InterruptSourceError::None;
    ASTERIA_EXPECT(runner, "MADT GSIs select their controller, including a second I/O SAPIC", configured, claimed,
                   lines.CanRoute(44U), !lines.CanRoute(24U), !lines.CanRoute(56U),
                   (firstModel.Entries[1U] & 255U) == low.Vector, (secondModel.Entries[12U] & 255U) == high.Vector);
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64IoSapicTests"};
    Asteria::Test::TestConfiguration(runner);
    Asteria::Test::TestLevelLine(runner);
    Asteria::Test::TestEdgeLine(runner);
    Asteria::Test::TestDestination(runner);
    Asteria::Test::TestLines(runner);
    Asteria::Test::TestLateVectorAfterRelease(runner);
    Asteria::Test::TestIsaPolicies(runner);
    Asteria::Test::TestAdditionalController(runner);
    return runner.Finish();
}
