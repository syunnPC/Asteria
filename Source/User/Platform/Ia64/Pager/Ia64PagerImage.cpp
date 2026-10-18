// SPDX-License-Identifier: Apache-2.0

#include "User/Pager/PagerProgram.hpp"
#include "User/Pager/RootService.hpp"
#include "User/Platform/Ia64/System/Ia64SystemCallTransport.hpp"

extern "C" void AsteriaIa64PagerThreadEntry() noexcept;
extern "C" void AsteriaIa64PagerFaultingThreadEntry() noexcept;
extern "C" void AsteriaIa64PagerSpinningThreadEntry() noexcept;
extern "C" void AsteriaIa64PagerExceptionThreadEntry() noexcept;
extern "C" void AsteriaIa64PagerWaitingThreadEntry() noexcept;
extern "C" Asteria::Foundation::UInt64 AsteriaIa64PagerThreadClock() noexcept;
extern "C" bool AsteriaIa64PagerThreadStateIntact() noexcept;
extern "C" void AsteriaIa64PagerFlushCode(Asteria::Foundation::UInt64 address,
                                          Asteria::Foundation::UInt64 bytes) noexcept;

namespace
{
// The self-test threads' user region in the pager image: the kernel takes its lower 16 KiB as the
// RSE backing store and its upper 16 KiB as the memory stack; the second thread starts after the
// first was reclaimed.
alignas(4096) constinit Asteria::Foundation::UInt8 g_threadRegion[32768U]{};
// The first thread waits to be preempted for at most 256 of the kernel's
// 20M-tick time slices (Ia64UserPreemption.hpp), in ITC ticks, as AMD64's
// waits 256 of its slices.
constexpr Asteria::Foundation::UInt64 ThreadSpinLimitTicks{256ULL * 20'000'000ULL};
// A crashing component spins 200M ITC ticks (0.5 s, ten time slices) and
// then crashes, so ticks end many of its runs; a reporting one computes as
// long before it reports. It shares the BSP round-robin with the CPU-bound
// initial image. The watchdog leaves many times that, since the ITC follows
// the host's clock and a loaded host delays the guest's slices without
// stopping its time. The margin accommodates delayed guest execution.
constexpr Asteria::Foundation::UInt64 ComponentSpinTicks{200'000'000ULL};
constexpr Asteria::Foundation::UInt64 ComponentWatchdogMicroseconds{10'000'000ULL};

} // Namespace

// The kernel starts the first self-test thread here with its control
// block's address in r4 (Ia64PagerImage.S); ThreadExit returns only if
// refused.
extern "C" void AsteriaIa64PagerThreadMain(const Asteria::Foundation::UInt64 controlAddress) noexcept
{
    const auto transport = Asteria::User::Platform::Ia64::System::GetIa64SystemCallTransport();
    const Asteria::Foundation::UInt64 code =
        Asteria::User::Pager::RunPagerThread(transport,
                                             *reinterpret_cast<Asteria::User::Pager::PagerThreadControl*>(
                                                 static_cast<Asteria::Foundation::UIntPointer>(controlAddress)),
                                             AsteriaIa64PagerThreadStateIntact, AsteriaIa64PagerThreadClock);
    const Asteria::User::System::SystemCallClient client{transport};
    for (;;)
    {
        static_cast<void>(client.ExitThread(code));
    }
}

// The waiting self-test thread, with its control block's address in r4 (Ia64PagerImage.S).
extern "C" void AsteriaIa64PagerWaitingThreadMain(const Asteria::Foundation::UInt64 controlAddress) noexcept
{
    const auto transport = Asteria::User::Platform::Ia64::System::GetIa64SystemCallTransport();
    const Asteria::Foundation::UInt64 code = Asteria::User::Pager::RunWaitingThread(
        transport, *reinterpret_cast<Asteria::User::Pager::PagerWaitingThreadControl*>(
                       static_cast<Asteria::Foundation::UIntPointer>(controlAddress)));
    const Asteria::User::System::SystemCallClient client{transport};
    for (;;)
    {
        static_cast<void>(client.ExitThread(code));
    }
}

// The kernel writes the start block below the pager's initial memory stack
// pointer and passes its address in r4 (Abi/PagerBootstrap.hpp); the entry
// stub in Ia64PagerImage.S forwards it.
extern "C" void AsteriaIa64PagerImageMain(const Asteria::Foundation::UInt64 bootstrapAddress) noexcept
{
    const auto& words = *reinterpret_cast<const Asteria::Foundation::UInt64(*)[Asteria::Abi::PagerBootstrapWords]>(
        static_cast<Asteria::Foundation::UIntPointer>(bootstrapAddress));
    // A function's address is its descriptor {ip, gp}, which the kernel reads
    // for the launch.
    static constinit Asteria::User::Pager::PagerThreadLaunch launch{};
    launch = Asteria::User::Pager::PagerThreadLaunch{
        .Entry = reinterpret_cast<Asteria::Foundation::UIntPointer>(&AsteriaIa64PagerThreadEntry),
        .StackTop = reinterpret_cast<Asteria::Foundation::UIntPointer>(g_threadRegion + sizeof(g_threadRegion)),
        .SpinLimit = ThreadSpinLimitTicks,
        .FaultEntry = reinterpret_cast<Asteria::Foundation::UIntPointer>(&AsteriaIa64PagerFaultingThreadEntry),
        .ExceptionEntry = reinterpret_cast<Asteria::Foundation::UIntPointer>(&AsteriaIa64PagerExceptionThreadEntry),
        .SpinningEntry = reinterpret_cast<Asteria::Foundation::UIntPointer>(&AsteriaIa64PagerSpinningThreadEntry),
        .WaitingEntry = reinterpret_cast<Asteria::Foundation::UIntPointer>(&AsteriaIa64PagerWaitingThreadEntry),
        .ComponentImages = nullptr,
        .ComponentImagesBytes = 0U,
        .ComponentSpinTicks = ComponentSpinTicks,
        // An entry descriptor, and copied code flushed to instruction fetch.
        .Component =
            {
                .Image = Asteria::User::Pager::MakeComponentRules(
                    Asteria::User::Loader::ExecutableMachine::Ia64, Asteria::Abi::Ia64ComponentUserStart,
                    Asteria::Abi::Ia64ComponentImageLimit, true, Asteria::User::Pager::Ia64ComponentSegmentPages),
                .StackStart = Asteria::Abi::Ia64ComponentStackStart,
                .StackPages = Asteria::Abi::Ia64ComponentStackPages,
                .FlushCode = AsteriaIa64PagerFlushCode,
                .WatchdogMicroseconds = ComponentWatchdogMicroseconds,
                // The guest bytes sit clear of the image, stack and account window.
                .ForeignImageAddress = Asteria::Abi::Ia64ComponentForeignImageAddress,
                .WindowStart = Asteria::Abi::Ia64ComponentWindowStart,
                .WindowPages = Asteria::Abi::Ia64ComponentWindowPages,
                .ScanoutAddress = Asteria::Abi::Ia64ComponentScanoutAddress,
                .Clock = AsteriaIa64PagerThreadClock,
            },
        // The root service's component exercises the domain types.
        .DomainCycles = 0U,
        .ServiceDemo = nullptr,
        .LoaderDemo = nullptr,
        // The driver's recovery with factorials: the device may not master the bus without an
        // IOMMU.
        .DriverDemo = nullptr,
        // IA-64 keeps its boot AHCI diagnostic.
        .BlockDemo = nullptr,
        .FsDemo = nullptr,
        .NetDemo = nullptr,
        .InputDemo = nullptr,
    };
    Asteria::User::Pager::RunPager(Asteria::User::Platform::Ia64::System::GetIa64SystemCallTransport(), words, &launch,
                                   Asteria::User::Pager::RunRootService);
}
