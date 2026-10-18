// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_DIAGNOSTICS_STOP_SCREEN_LAYOUT_HPP_INCLUDED
#define ASTERIA_KERNEL_DIAGNOSTICS_STOP_SCREEN_LAYOUT_HPP_INCLUDED

#include "Kernel/Diagnostics/StopRecord.hpp"
#include "Kernel/Diagnostics/Watchdog.hpp"

#if defined(ASTERIA_TARGET_AMD64) || defined(ASTERIA_TARGET_IA64)
extern "C" const Asteria::Foundation::UInt8 AsteriaKernelTextStart[];
extern "C" const Asteria::Foundation::UInt8 AsteriaKernelTextEnd[];
#endif

namespace Asteria::Kernel::Diagnostics
{
[[nodiscard]] constexpr const char* StopStateName(const Foundation::UInt64 state) noexcept
{
    switch (state)
    {
    case 0U:
        return "running";
    case 1U:
        return "stopping";
    case 2U:
        return "frozen";
    case 3U:
        return "unconfirmed";
    default:
        return "unknown";
    }
}
template <class TScreen, class TRecord, class TStageName>
void StopScreenHeading(TScreen& screen, const TRecord& record, const Foundation::UInt64 cpu,
                       TStageName stageName) noexcept
{
    using Foundation::UInt32;
    screen.Text("Asteria stopped\nCode ");
    screen.Code(record.Stop.Code);
    screen.Text(" ");
    screen.Text(Kernel::Diagnostics::StopName(static_cast<StopCode>(record.Stop.Code)));
    screen.Text("\nP1 ");
    screen.Hex(record.Stop.Parameters[0U]);
    if (record.Stop.Code == static_cast<UInt32>(StopCode::BootStage) ||
        record.Stop.Code == static_cast<UInt32>(StopCode::ApplicationProcessorStage))
    {
        screen.Text(" ");
        screen.Text(stageName(record.Stop.Code == static_cast<UInt32>(StopCode::ApplicationProcessorStage),
                              record.Stop.Parameters[0U]));
    }
    screen.Text("  P2 ");
    screen.Hex(record.Stop.Parameters[1U]);
    screen.Text("\nP3 ");
    screen.Hex(record.Stop.Parameters[2U]);
    screen.Text("  P4 ");
    screen.Hex(record.Stop.Parameters[3U]);
    screen.Text("\nCPU ");
    screen.Decimal(cpu);
}
template <class TScreen, class TEvidence>
void StopScreenFooter(TScreen& screen, const TEvidence& evidence, const Foundation::Size processors,
                      const char* clockName, const char* buildIdentity,
                      const WatchdogEvidence* watchdog = nullptr) noexcept
{
    using Foundation::Size;
    using Foundation::UInt64;
    for (Size processor{0U}; processor < processors; ++processor)
    {
        screen.Text("\nCPU ");
        screen.Decimal(processor);
        const UInt64 bit = UInt64{1U} << processor;
        const UInt64 state = evidence.DrawStates[processor];
        screen.Text(" ");
        screen.Text(StopStateName(state));
        if ((evidence.OnlineMask & bit) == 0U)
        {
            screen.Text(" not online");
        }
        else
        {
            screen.Text((evidence.ConfirmedMask & bit) != 0U ? " confirmed" : " unconfirmed");
        }
        if (watchdog != nullptr && (evidence.OnlineMask & bit) != 0U)
        {
            const auto& sample = watchdog->Processors[processor];
            if ((watchdog->StalledMask & bit) != 0U)
            {
                screen.Text(" stalled");
            }
            screen.Text(" age ");
            screen.Decimal(sample.AgeMilliseconds);
            screen.Text(" ms ");
            screen.Text(Abi::WatchdogComponentName(sample.Component));
            screen.Text(sample.Captured != 0U ? "\n  captured " : "\n  last heartbeat ");
            screen.Hex(sample.Location);
#if defined(ASTERIA_TARGET_AMD64) || defined(ASTERIA_TARGET_IA64)
#if defined(__ia64__)
            // KernelElf.md: section bounds are image-relative IMM64 fixups,
            // not GOT slots with IA-64 short-data section flags.
            UInt64 start{0U};
            UInt64 end{0U};
            __asm__ volatile("movl %0 = AsteriaKernelTextStart" : "=r"(start));
            __asm__ volatile("movl %0 = AsteriaKernelTextEnd" : "=r"(end));
#else
            const auto start = reinterpret_cast<Foundation::UIntPointer>(AsteriaKernelTextStart);
            const auto end = reinterpret_cast<Foundation::UIntPointer>(AsteriaKernelTextEnd);
#endif
            if (sample.Location >= start && sample.Location < end)
            {
                screen.Text(" KERNEL.ELF .text+");
                screen.Hex(sample.Location - start);
            }
#endif
        }
    }
    screen.Text("\nOnline ");
    screen.Hex(evidence.OnlineMask);
    screen.Text("\nUptime ");
    screen.Text(clockName);
    screen.Text(" ticks ");
    screen.Hex(evidence.UptimeTicks);
    screen.Text("\nDMA not quiesced\nBuild ");
    screen.Text(buildIdentity);
}
} // namespace Asteria::Kernel::Diagnostics

#endif // ASTERIA_KERNEL_DIAGNOSTICS_STOP_SCREEN_LAYOUT_HPP_INCLUDED
