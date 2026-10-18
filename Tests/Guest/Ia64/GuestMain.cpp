// SPDX-License-Identifier: Apache-2.0
// IA-64 entry of the guest test program. The image's entry stub
// (AsteriaIa64MinimalUserImageEntry) calls this function with the bootstrap
// handles the kernel placed in r4/r5 (Endpoint) and r6/r7
// (DiagnosticChannel), and idles when it returns. The image receives no
// WaitSet yet, so the WaitSet points are skipped. Every system call goes
// through AsteriaGuestIa64CheckedSystemCallTransport
// (CheckedSystemCallTransport.S), which checks the global pointer the kernel
// returns with, except the one AsteriaGuestIa64ReadStackedRegistersAfter-
// SystemCall (StackedRegisterProbe.S) makes to inspect the stacked registers.

#include "Abi/PagerBootstrap.hpp"
#include "Guest/GuestTests.hpp"
#include "Platform/Ia64/Ia64UserVirtualLayout.hpp"

extern "C"
{
void AsteriaGuestIa64CheckedSystemCallTransport(void* context, const Asteria::Abi::SystemCallRequest* request,
                                                Asteria::Abi::SystemCallResponse* response) noexcept;

extern volatile const Asteria::Foundation::UInt64 AsteriaGuestIa64CheckedSystemCallCount;
extern volatile const Asteria::Foundation::UInt64 AsteriaGuestIa64GlobalPointerDifference;

Asteria::Foundation::UInt64
AsteriaGuestIa64ReadStackedRegistersAfterSystemCall(Asteria::Foundation::UInt64 number,
                                                    Asteria::Foundation::UInt64 argument0,
                                                    Asteria::Foundation::UInt64 argument1) noexcept;

void AsteriaGuestIa64LoadExtendedState() noexcept;
bool AsteriaGuestIa64ExtendedStateIntact() noexcept;
}

namespace
{
using Asteria::Foundation::UInt64;
namespace Layout = Asteria::Platform::Ia64;

// The memory stack grows down from its end; the register backing store
// grows up from its start (Ia64UserVirtualLayout.hpp).
constexpr UInt64 StackStart{Layout::Ia64InitialUserMemoryStackVirtualStart};
constexpr UInt64 StackEnd{StackStart + Layout::Ia64InitialUserMemoryStackSizeBytes};
constexpr UInt64 BackingStoreStart{Layout::Ia64InitialUserRseBackingStoreVirtualStart};
constexpr UInt64 BackingStoreEnd{BackingStoreStart + Layout::Ia64InitialUserRseBackingStoreSizeBytes};

// sp (r12) and ar.bsp in the first instructions of the C++ entry.
constinit UInt64 g_entryStackPointer{0U};
constinit UInt64 g_entryBackingStorePointer{0U};
// The bootstrap Endpoint, which the stacked-register point queries.
constinit Asteria::User::System::CapabilityHandle g_endpoint{};

[[nodiscard]] volatile UInt64* LazyWord(const UInt64 page, const UInt64 word) noexcept
{
    return reinterpret_cast<volatile UInt64*>(static_cast<Asteria::Foundation::UIntPointer>(
        Layout::Ia64InitialUserLazyVirtualStart + (page * Layout::Ia64UserPageSizeBytes) + (word * sizeof(UInt64))));
}

/// Touches the lazily backed region, which nothing maps yet: each first
/// access misses, the boot pager maps the page from its Frame, and the access
/// completes when the kernel resumes the image at the same instruction.
void CheckLazyRegion(Asteria::Guest::Tap& tap) noexcept
{
    const UInt64 first = *LazyWord(1U, 0U);
    const UInt64 firstIndex = *LazyWord(1U, 1U);
    if (!tap.Check((first == Asteria::Abi::LazyPagePattern) && (firstIndex == 1U),
                   "pager.read: a first read of a lazy page returns the pager's frame"))
    {
        tap.Detail("pattern", Asteria::Abi::LazyPagePattern, first);
        tap.Detail("page", 1U, firstIndex);
    }
    constexpr UInt64 Written{0x5752'4954'5445'4E21ULL};
    *LazyWord(0U, 2U) = Written;
    const UInt64 pattern = *LazyWord(0U, 0U);
    if (!tap.Check((*LazyWord(0U, 2U) == Written) && (pattern == Asteria::Abi::LazyPagePattern) &&
                       (*LazyWord(0U, 1U) == 0U),
                   "pager.write: a first write to a lazy page lands in the pager's frame"))
    {
        tap.Detail("written", Written, *LazyWord(0U, 2U));
        tap.Detail("pattern", Asteria::Abi::LazyPagePattern, pattern);
    }
    static_cast<void>(tap.Equal("pager.resident: a resolved page stays mapped", 1U, *LazyWord(1U, 1U)));
}

void CheckUserState(Asteria::Guest::Tap& tap) noexcept
{
    const UInt64 calls{AsteriaGuestIa64CheckedSystemCallCount};
    const UInt64 difference{AsteriaGuestIa64GlobalPointerDifference};
    if (!tap.Check((calls != 0U) && (difference == 0U), "abi.gp-preserved: r1 equals __gp after every system call"))
    {
        tap.Detail("checked calls, at least", 1U, calls);
        tap.Detail("r1 xor __gp", 0U, difference);
    }
    const bool stack{(g_entryStackPointer > StackStart) && (g_entryStackPointer <= StackEnd)};
    const bool backingStore{(g_entryBackingStorePointer >= BackingStoreStart) &&
                            (g_entryBackingStorePointer < BackingStoreEnd)};
    if (!tap.Check(stack && backingStore,
                   "abi.stack-bounds: the entry runs on the initial memory stack and register backing store"))
    {
        tap.Detail("stack start", StackStart, g_entryStackPointer);
        tap.Detail("stack end", StackEnd, g_entryStackPointer);
        tap.Detail("backing store start", BackingStoreStart, g_entryBackingStorePointer);
        tap.Detail("backing store end", BackingStoreEnd, g_entryBackingStorePointer);
    }
    // A Query runs the kernel's capability lookup, whose frames fill the
    // physical stacked registers above the caller's with kernel values.
    const UInt64 grown{AsteriaGuestIa64ReadStackedRegistersAfterSystemCall(
        static_cast<UInt64>(Asteria::Abi::SystemCallNumber::CapabilityQuery), g_endpoint.Selector,
        g_endpoint.Authenticator)};
    if (!tap.Check(grown == 0U, "abi.stacked-registers: registers a frame grows into after a system call hold zero"))
    {
        tap.Detail("OR of r38-r127 (all-ones: a NaT)", 0U, grown);
    }
    CheckLazyRegion(tap);
    // Last: the pattern the entry loaded outlived every test above, their
    // faults and preemptions while the pager's thread ran its own.
    static_cast<void>(
        tap.Check(AsteriaGuestIa64ExtendedStateIntact(),
                  "abi.extended-state: f32-f127 keep the program's values across preemption, faults and system calls"));
}
} // namespace

extern "C" UInt64 AsteriaIa64MinimalUserImageMain(const UInt64 endpointSelector, const UInt64 endpointAuthenticator,
                                                  const UInt64 diagnosticSelector,
                                                  const UInt64 diagnosticAuthenticator) noexcept
{
    AsteriaGuestIa64LoadExtendedState();
    UInt64 stackPointer{0U};
    UInt64 backingStorePointer{0U};
    __asm__ volatile("mov %0 = r12\n\tmov %1 = ar.bsp\n\t;;" : "=r"(stackPointer), "=r"(backingStorePointer));
    g_entryStackPointer = stackPointer;
    g_entryBackingStorePointer = backingStorePointer;
    g_endpoint = {endpointSelector, endpointAuthenticator};
    const Asteria::Guest::Bootstrap bootstrap{
        .Endpoint = {endpointSelector, endpointAuthenticator},
        .WaitSet = {0U, 0U},
        .Diagnostic = {diagnosticSelector, diagnosticAuthenticator},
        .HasWaitSet = false,
    };
    const Asteria::User::System::SystemCallTransport transport{
        .Function = AsteriaGuestIa64CheckedSystemCallTransport,
        .Context = nullptr,
    };
    return Asteria::Guest::RunGuestTests(bootstrap, transport, CheckUserState);
}
