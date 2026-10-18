// SPDX-License-Identifier: Apache-2.0
#include "Platform/Amd64/Time/Amd64ApicTimer.hpp"
#include "Platform/Amd64/Diagnostics/Amd64StopStage.hpp"

#include "Support/TestContext.hpp"

int main()
{
    Asteria::TestSupport::TestContext context{"Amd64ApicInterruptPolicyTests"};
    using Asteria::Foundation::UInt32;
    using Asteria::Platform::Amd64::Time::Amd64ApicTimer;
    using Asteria::Platform::Amd64::Time::ApicErrorVector;
    using Asteria::Platform::Amd64::Time::ApicLvtMaskBit;
    for (UInt32 maximum{3U}; maximum <= 6U; ++maximum)
    {
        alignas(4096U) UInt32 registers[1024U]{};
        for (auto& value : registers)
        {
            value = 0x12345678U;
        }
        registers[0x30U / 4U] = maximum << 16U;
        Asteria::Platform::Amd64::Interrupt::Amd64LocalApic apic;
        ASTERIA_EXPECT(context, "MMIO adapter initialized",
                       apic.Initialize(static_cast<Asteria::Foundation::UInt64>(
                           reinterpret_cast<Asteria::Foundation::UIntPointer>(registers))));
        Amd64ApicTimer::MaskUnusedInterrupts(apic);
        constexpr UInt32 offsets[]{0x330U, 0x340U, 0x2F0U, 0x350U, 0x360U, 0x370U};
        constexpr UInt32 minimum[]{5U, 4U, 6U, 0U, 0U, 0U};
        for (Asteria::Foundation::Size entry{0U}; entry < 6U; ++entry)
        {
            ASTERIA_EXPECT(context, "implemented LVTs masked, unsupported registers untouched",
                           registers[offsets[entry] / 4U] ==
                               (maximum >= minimum[entry] ? ApicLvtMaskBit | ApicErrorVector : 0x12345678U));
        }
        ASTERIA_EXPECT(context, "timer policy is independent", registers[0x320U / 4U] == 0x12345678U);
    }
    using Asteria::Platform::Amd64::Diagnostics::Amd64StageNameFor;
    ASTERIA_EXPECT(context, "stage name tables used for P1", Amd64StageNameFor(false, 10U)[0U] == 'P',
                   Amd64StageNameFor(true, 1U)[0U] == 'E', Amd64StageNameFor(false, 999U)[0U] == 'U');
    return context.Finish();
}
