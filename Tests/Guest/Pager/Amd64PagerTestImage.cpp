// SPDX-License-Identifier: Apache-2.0
#include "User/Pager/PagerProgram.hpp"
#include "Guest/Pager/TestRootService.hpp"
#include "User/Platform/Amd64/System/Amd64SystemCallTransport.hpp"

extern "C" void AsteriaAmd64PagerThreadEntry() noexcept;
extern "C" void AsteriaAmd64PagerFaultingThreadEntry() noexcept;
extern "C" void AsteriaAmd64PagerExceptionThreadEntry() noexcept;
extern "C" void AsteriaAmd64PagerSpinningThreadEntry() noexcept;
extern "C" void AsteriaAmd64PagerWaitingThreadEntry() noexcept;
extern "C" bool AsteriaAmd64PagerThreadStateIntact() noexcept;
extern "C" Asteria::Foundation::UInt64 AsteriaAmd64PagerThreadClock() noexcept;

namespace
{
// The self-test threads' user stack, in the pager image; the second thread starts after the first
// was reclaimed.
alignas(16) constinit Asteria::Foundation::UInt8 g_threadStack[8192U]{};
// The thread waits to be preempted for at most 256 of the kernel's 40M-tick
// time slices (Amd64UserScheduling.hpp), in TSC ticks.
constexpr Asteria::Foundation::UInt64 ThreadSpinLimitTicks{256ULL * 40'000'000ULL};
// The component's first incarnation spins three of those slices before it crashes, so at least one
// tick ends its run.
constexpr Asteria::Foundation::UInt64 ComponentSpinTicks{3ULL * 40'000'000ULL};
} // Namespace

// The kernel starts the self-test thread here with its control block's
// address in RDI (Amd64PagerImage.S); ThreadExit returns only if refused.
extern "C" void AsteriaAmd64PagerThreadMain(const Asteria::Foundation::UInt64 controlAddress) noexcept
{
    const auto transport = Asteria::User::Platform::Amd64::System::GetAmd64SystemCallTransport();
    const Asteria::Foundation::UInt64 code =
        Asteria::User::Pager::RunPagerThread(transport,
                                             *reinterpret_cast<Asteria::User::Pager::PagerThreadControl*>(
                                                 static_cast<Asteria::Foundation::UIntPointer>(controlAddress)),
                                             AsteriaAmd64PagerThreadStateIntact, AsteriaAmd64PagerThreadClock);
    const Asteria::User::System::SystemCallClient client{transport};
    for (;;)
    {
        static_cast<void>(client.ExitThread(code));
    }
}

// The waiting self-test thread, with its control block's address in RDI.
extern "C" void AsteriaAmd64PagerWaitingThreadMain(const Asteria::Foundation::UInt64 controlAddress) noexcept
{
    const auto transport = Asteria::User::Platform::Amd64::System::GetAmd64SystemCallTransport();
    const Asteria::Foundation::UInt64 code = Asteria::User::Pager::RunWaitingThread(
        transport, *reinterpret_cast<Asteria::User::Pager::PagerWaitingThreadControl*>(
                       static_cast<Asteria::Foundation::UIntPointer>(controlAddress)));
    const Asteria::User::System::SystemCallClient client{transport};
    for (;;)
    {
        static_cast<void>(client.ExitThread(code));
    }
}

// The kernel writes the start block at the top of the pager's initial stack
// and passes its address in RDI (Abi/PagerBootstrap.hpp).
extern "C" void AsteriaAmd64PagerImageMain(const Asteria::Foundation::UInt64 bootstrapAddress) noexcept
{
    const auto& words = *reinterpret_cast<const Asteria::Foundation::UInt64(*)[Asteria::Abi::PagerBootstrapWords]>(
        static_cast<Asteria::Foundation::UIntPointer>(bootstrapAddress));
    static constinit Asteria::User::Pager::PagerThreadLaunch launch{};
    launch =
        Asteria::User::Pager::PagerThreadLaunch{
            .Entry = reinterpret_cast<Asteria::Foundation::UIntPointer>(&AsteriaAmd64PagerThreadEntry),
            .StackTop = reinterpret_cast<Asteria::Foundation::UIntPointer>(g_threadStack + sizeof(g_threadStack)),
            .SpinLimit = ThreadSpinLimitTicks,
            .FaultEntry = reinterpret_cast<Asteria::Foundation::UIntPointer>(&AsteriaAmd64PagerFaultingThreadEntry),
            .ExceptionEntry =
                reinterpret_cast<Asteria::Foundation::UIntPointer>(&AsteriaAmd64PagerExceptionThreadEntry),
            .SpinningEntry = reinterpret_cast<Asteria::Foundation::UIntPointer>(&AsteriaAmd64PagerSpinningThreadEntry),
            .WaitingEntry = reinterpret_cast<Asteria::Foundation::UIntPointer>(&AsteriaAmd64PagerWaitingThreadEntry),
            .ComponentImages = nullptr,
            .ComponentImagesBytes = 0U,
            .ComponentSpinTicks = ComponentSpinTicks,
            // Canonical lower-half addresses above page zero, a code entry, a
            // stack below the start block, and coherent instruction fetch.
            .Component =
                {
                    .Image = Asteria::User::Pager::MakeComponentRules(Asteria::User::Loader::ExecutableMachine::Amd64,
                                                                      0x1000U, 0x0000'8000'0000'0000ULL, false,
                                                                      Asteria::User::Pager::Amd64ComponentSegmentPages),
                    .StackStart = Asteria::Abi::ComponentStackStart,
                    .StackPages = Asteria::Abi::ComponentStackPages,
                    .FlushCode = nullptr,
                    // The driver task alone waits for five of edu's transfers, each
                    // Timed at 100 ms of host time; the watchdog leaves more than ten times that,
                    // since a loaded host delays the guest without stopping its clock: at four
                    // times, a full suite of emulators running side by side let it kill the driver
                    // once in some forty runs (as IA-64's, 7dc562a).
                    .WatchdogMicroseconds = 6'000'000U,
                    .ForeignImageAddress = Asteria::Abi::ComponentForeignImageAddress,
                    .WindowStart = Asteria::Abi::ComponentWindowStart,
                    .WindowPages = Asteria::Abi::ComponentWindowPages,
                    .Clock = AsteriaAmd64PagerThreadClock,
                },
            // The root service's component exercises the domain types.
            .DomainCycles = 0U,
            .ServiceDemo = Asteria::User::Pager::RunServiceDemo,
            .LoaderDemo = Asteria::User::Pager::RunLoaderDemo,
            .DriverDemo = Asteria::User::Pager::RunDriverDemo,
            .BlockDemo = Asteria::User::Pager::RunBlockDemo,
            .FsDemo = Asteria::User::Pager::RunFsDemo,
            .NetDemo = Asteria::User::Pager::RunNetDemo,
            .PortsDemo = Asteria::User::Pager::RunPortsDemo,
            .InputDemo = Asteria::User::Pager::RunInputDemo,
    };
    Asteria::User::Pager::RunPager(Asteria::User::Platform::Amd64::System::GetAmd64SystemCallTransport(), words,
                                   &launch, Asteria::User::Pager::RunTestRootService);
}
