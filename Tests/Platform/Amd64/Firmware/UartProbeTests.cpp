// SPDX-License-Identifier: Apache-2.0
#include "Platform/Amd64/Firmware/UartProbe.hpp"

#include <cstdio>

#include "Support/Random.hpp"
#include "Support/TestContext.hpp"

namespace
{
using Asteria::Foundation::UInt8;
namespace Firmware = Asteria::Platform::Amd64::Firmware;
// PC16550D sections 8.1/8.3/8.6-8.10. Model register ownership and the
// four independent loopback wires, without reproducing the probe sequence.
class Model final : public Firmware::IUartRegisters
{
public:
    [[nodiscard]] UInt8 Read(const UInt8 offset) noexcept override
    {
        if (Absent)
        {
            return 0xFFU;
        }
        if (offset == 2U)
        {
            return BadIir ? 0x31U : static_cast<UInt8>(Ier == 0U ? 1U : 0U);
        }
        if (offset == 6U)
        {
            const UInt8 wiring[4U]{0x20U, 0x10U, 0x40U, 0x80U};
            UInt8 status{0U};
            for (unsigned int output{0U}; output < 4U; ++output)
            {
                if ((Mcr & (1U << output)) != 0U)
                {
                    status = static_cast<UInt8>(status | wiring[output]);
                }
            }
            return static_cast<UInt8>(status ^ BadWire);
        }
        if (offset == 7U)
        {
            return static_cast<UInt8>(Scratch & ~StuckLow);
        }
        if (offset == 3U)
        {
            return Lcr;
        }
        if (offset == 4U)
        {
            return Mcr;
        }
        if (offset == 1U)
        {
            return (Lcr & 0x80U) != 0U ? DivisorHigh : Ier;
        }
        return 0U;
    }
    void Write(const UInt8 offset, const UInt8 value) noexcept override
    {
        if (Absent)
        {
            return;
        }
        if (offset == 7U)
        {
            Scratch = value;
        }
        else if (offset == 3U)
        {
            Lcr = value;
        }
        else if (offset == 4U)
        {
            Mcr = value;
        }
        else if (offset == 1U)
        {
            if ((Lcr & 0x80U) != 0U)
            {
                DivisorHigh = value;
            }
            else
            {
                Ier = value;
            }
        }
        else if (offset == 0U && (Lcr & 0x80U) != 0U)
        {
            DivisorLow = value;
        }
        else if (offset == 2U)
        {
            Fcr = value;
        }
        else
        {
            Valid = false;
        }
    }
    UInt8 Scratch{0U};
    UInt8 Lcr{0U};
    UInt8 Mcr{0U};
    UInt8 Ier{0U};
    UInt8 DivisorLow{0U};
    UInt8 DivisorHigh{0U};
    UInt8 Fcr{0U};
    UInt8 StuckLow{0U};
    UInt8 BadWire{0U};
    bool BadIir{false};
    bool Absent{false};
    bool Valid{true};
};
} // namespace

int main()
{
    Asteria::TestSupport::TestContext context{"UartProbe"};
    const auto seeds = Asteria::TestSupport::GetSeedRange(510U, 8U);
    for (Asteria::Foundation::Size seedAt{0U}; seedAt < seeds.Count; ++seedAt)
    {
        Asteria::TestSupport::Random random{seeds.First + seedAt};
        for (unsigned int step{0U}; step < 256U; ++step)
        {
            Model model;
            model.Scratch = static_cast<UInt8>(random.Below(256U));
            model.Lcr = static_cast<UInt8>(random.Below(256U));
            model.Mcr = static_cast<UInt8>(random.Below(16U));
            model.Ier = static_cast<UInt8>(random.Below(16U));
            const unsigned int fault = step % 15U;
            model.Absent = fault == 1U;
            model.StuckLow = (fault >= 2U && fault < 10U) ? static_cast<UInt8>(1U << (fault - 2U)) : UInt8{0U};
            model.BadWire = (fault >= 10U && fault < 14U) ? static_cast<UInt8>(1U << (fault - 6U)) : UInt8{0U};
            model.BadIir = fault == 14U;
            model.Scratch = static_cast<UInt8>(model.Scratch & ~model.StuckLow);
            const Model before = model;
            const bool detected = Firmware::ProbeUart(model);
            const bool controls = detected ? (model.Lcr == 3U && model.Mcr == 3U && model.Ier == 0U &&
                                              model.DivisorLow == 1U && model.DivisorHigh == 0U && model.Fcr == 7U)
                                           : (model.Lcr == before.Lcr && model.Mcr == before.Mcr &&
                                              model.Ier == before.Ier && model.Fcr == 0U);
            if (!ASTERIA_EXPECT(context, "register/fault model and control restoration", detected == (fault == 0U),
                                model.Valid, model.Scratch == before.Scratch, controls))
            {
                static_cast<void>(std::fprintf(stderr, "seed=%llu step=%u probe fault=%u\n",
                                               static_cast<unsigned long long>(seeds.First + seedAt), step, fault));
                return context.Finish();
            }
        }
    }
    return context.Finish();
}
