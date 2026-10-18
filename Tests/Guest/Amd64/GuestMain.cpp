// SPDX-License-Identifier: Apache-2.0
// AMD64 entry of the guest test program. The image's entry stub
// (AsteriaAmd64MinimalUserImageEntry) calls this function with the bootstrap
// handles the kernel placed in RDI/RSI (Endpoint), RDX/RCX (WaitSet) and
// R8/R9 (DiagnosticChannel), and idles when it returns.

#include "Abi/PagerBootstrap.hpp"
#include "Guest/GuestTests.hpp"
#include "Platform/Amd64/Executable/Amd64InitialUserImage.hpp"
#include "User/Platform/Amd64/System/Amd64SystemCallTransport.hpp"

extern "C" void AsteriaGuestAmd64LoadExtendedState() noexcept;
extern "C" bool AsteriaGuestAmd64ExtendedStateIntact() noexcept;

namespace
{
using Asteria::Foundation::UInt64;

// User selectors and the user RFLAGS contract (Amd64UserEntry.hpp).
constexpr UInt64 UserCodeSelector{0x23U};
constexpr UInt64 UserDataSelector{0x1BU};
constexpr UInt64 InterruptFlag{1U << 9U};
constexpr UInt64 IoPrivilegeLevelMask{3U << 12U};
// RFLAGS bits a CPL3 program may set with POPF and a system call returns as
// the program left them (Amd64UserEntry.hpp, UserReturnPermittedFlags).
constexpr UInt64 AlignmentCheckFlag{1U << 18U};
constexpr UInt64 IdentificationFlag{1U << 21U};

// The stack the kernel maps for the initial image (Amd64InitialUserImage.hpp).
constexpr UInt64 StackStart{Asteria::Platform::Amd64::Executable::InitialUserStackStart};
constexpr UInt64 StackEnd{StackStart + (Asteria::Platform::Amd64::Executable::InitialUserStackPages *
                                        Asteria::Kernel::Memory::MappingPageSizeBytes)};

// RSP in the first instructions of the C++ entry.
constinit UInt64 g_entryStackPointer{0U};
// The bootstrap Endpoint, which the SYSCALL-state checks query.
constinit Asteria::User::System::CapabilityHandle g_endpoint{};

/// What one raw Query observed: the response registers, RFLAGS just before
/// SYSCALL and right after it, and RSP right after it.
struct RawQuery
{
    Asteria::Abi::SystemCallResponse Response;
    UInt64 FlagsBefore;
    UInt64 FlagsAfter;
    UInt64 StackPointerAfter;
};

/// Queries `handle` through SYSCALL while RFLAGS.AC and RFLAGS.ID are set and
/// RSP is zero: both are legal user state that only the program owns. Nothing
/// between the two RFLAGS samples and SYSCALL changes a flag, so FlagsBefore
/// is the RFLAGS image SYSCALL saves. RSP stays zero until the call returned;
/// the original stack pointer lives in a register the kernel preserves, and
/// AC and ID are cleared again before the function returns.
[[nodiscard]] RawQuery QueryWithUserState(const Asteria::User::System::CapabilityHandle handle) noexcept
{
    UInt64 error{static_cast<UInt64>(Asteria::Abi::SystemCallNumber::CapabilityQuery)};
    UInt64 value0{0U};
    UInt64 value1{0U};
    UInt64 before{0U};
    UInt64 after{0U};
    UInt64 stackAfter{0U};
    UInt64 saved{0U};
    __asm__ volatile("pushfq\n\t"
                     "orq %[set], (%%rsp)\n\t"
                     "popfq\n\t"
                     "pushfq\n\t"
                     "popq %[before]\n\t"
                     "movl $0, %%r8d\n\t"
                     "movl $0, %%r9d\n\t"
                     "movl $0, %%r10d\n\t"
                     "movq %%rsp, %[saved]\n\t"
                     "movq $0, %%rsp\n\t"
                     "syscall\n\t"
                     "movq %%rsp, %[stackAfter]\n\t"
                     "movq %[saved], %%rsp\n\t"
                     "pushfq\n\t"
                     "popq %[after]\n\t"
                     "movq %%r8, %[value1]\n\t"
                     "pushfq\n\t"
                     "andq %[clear], (%%rsp)\n\t"
                     "popfq"
                     : "+a"(error), "+d"(value0), [value1] "=&r"(value1), [before] "=&r"(before), [after] "=&r"(after),
                       [stackAfter] "=&r"(stackAfter), [saved] "=&r"(saved)
                     : "D"(handle.Selector),
                       "S"(handle.Authenticator), [set] "n"(AlignmentCheckFlag | IdentificationFlag),
                       [clear] "n"(~(AlignmentCheckFlag | IdentificationFlag))
                     : "rcx", "r8", "r9", "r10", "r11", "cc", "memory");
    return {{static_cast<Asteria::Abi::SystemCallError>(error), value0, value1}, before, after, stackAfter};
}

void CheckSystemCallUserState(Asteria::Guest::Tap& tap) noexcept
{
    const Asteria::User::System::SystemCallClient client{
        Asteria::User::Platform::Amd64::System::GetAmd64SystemCallTransport()};
    const Asteria::Abi::SystemCallResponse expected = client.QueryCapability(g_endpoint);
    const RawQuery raw = QueryWithUserState(g_endpoint);
    if (!tap.Check((raw.Response.Error == expected.Error) && (raw.Response.Value0 == expected.Value0) &&
                       (raw.Response.Value1 == expected.Value1),
                   "abi.syscall-user-state: SYSCALL with AC and ID set and RSP zero answers like a plain call"))
    {
        tap.Detail("error", static_cast<UInt64>(expected.Error), static_cast<UInt64>(raw.Response.Error));
        tap.Detail("value0", expected.Value0, raw.Response.Value0);
        tap.Detail("value1", expected.Value1, raw.Response.Value1);
    }
    static_cast<void>(
        tap.Equal("abi.syscall-flags: the call returns RFLAGS as the program set them, AC and ID included",
                  raw.FlagsBefore, raw.FlagsAfter));
    static_cast<void>(tap.Check((raw.FlagsBefore & (AlignmentCheckFlag | IdentificationFlag)) ==
                                    (AlignmentCheckFlag | IdentificationFlag),
                                "abi.syscall-flags-set: AC and ID were set at SYSCALL"));
    static_cast<void>(
        tap.Equal("abi.syscall-stack: the call returns RSP unchanged, even zero", 0U, raw.StackPointerAfter));
}

[[nodiscard]] volatile UInt64* LazyWord(const UInt64 page, const UInt64 word) noexcept
{
    return reinterpret_cast<volatile UInt64*>(static_cast<Asteria::Foundation::UIntPointer>(
        Asteria::Platform::Amd64::Executable::InitialUserLazyStart +
        (page * Asteria::Kernel::Memory::MappingPageSizeBytes) + (word * sizeof(UInt64))));
}

/// Touches the lazily backed region, which nothing maps yet: each first
/// access faults, the boot pager maps the page from its Frame, and the access
/// completes when the kernel resumes the image.
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
    UInt64 codeSelector{0U};
    UInt64 stackSelector{0U};
    UInt64 flags{0U};
    __asm__ volatile("mov %%cs, %0\n\tmov %%ss, %1\n\tpushfq\n\tpopq %2"
                     : "=r"(codeSelector), "=r"(stackSelector), "=r"(flags));
    static_cast<void>(tap.Equal("abi.code-selector: user code runs in CPL 3 with the user code selector",
                                UserCodeSelector, codeSelector));
    static_cast<void>(tap.Equal("abi.stack-selector: the user data selector", UserDataSelector, stackSelector));
    static_cast<void>(tap.Check((flags & InterruptFlag) != 0U, "abi.interrupts: user code runs with IF set"));
    static_cast<void>(tap.Equal("abi.io-privilege: IOPL is 0", 0U, flags & IoPrivilegeLevelMask));
    if (!tap.Check((g_entryStackPointer > StackStart) && (g_entryStackPointer <= StackEnd),
                   "abi.stack-bounds: the entry runs on the initial user stack"))
    {
        tap.Detail("stack start", StackStart, g_entryStackPointer);
        tap.Detail("stack end", StackEnd, g_entryStackPointer);
    }
    CheckSystemCallUserState(tap);
    CheckLazyRegion(tap);
    // Last: the pattern the entry loaded outlived every test above, its
    // waits, faults and preemptions while the spinner ran its own.
    static_cast<void>(tap.Check(
        AsteriaGuestAmd64ExtendedStateIntact(),
        "abi.extended-state: xmm0-xmm15 and MXCSR keep the program's values across preemption, faults, system "
        "calls and waits"));
}
} // namespace

extern "C" UInt64 AsteriaAmd64MinimalUserImageMain(const UInt64 endpointSelector, const UInt64 endpointAuthenticator,
                                                   const UInt64 waitSetSelector, const UInt64 waitSetAuthenticator,
                                                   const UInt64 diagnosticSelector,
                                                   const UInt64 diagnosticAuthenticator) noexcept
{
    AsteriaGuestAmd64LoadExtendedState();
    UInt64 stackPointer{0U};
    __asm__ volatile("mov %%rsp, %0" : "=r"(stackPointer));
    g_entryStackPointer = stackPointer;
    g_endpoint = {endpointSelector, endpointAuthenticator};
    const Asteria::Guest::Bootstrap bootstrap{
        .Endpoint = {endpointSelector, endpointAuthenticator},
        .WaitSet = {waitSetSelector, waitSetAuthenticator},
        .Diagnostic = {diagnosticSelector, diagnosticAuthenticator},
        .HasWaitSet = true,
    };
    return Asteria::Guest::RunGuestTests(
        bootstrap, Asteria::User::Platform::Amd64::System::GetAmd64SystemCallTransport(), CheckUserState);
}
