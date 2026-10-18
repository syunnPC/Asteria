// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_INTERRUPT_AMD64_USER_ENTRY_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_INTERRUPT_AMD64_USER_ENTRY_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Platform::Amd64::Interrupt
{
inline constexpr Foundation::UInt16 KernelCodeSelector{0x08U};
inline constexpr Foundation::UInt16 KernelDataSelector{0x10U};
inline constexpr Foundation::UInt16 UserDataSelector{0x1BU};
inline constexpr Foundation::UInt16 UserCodeSelector{0x23U};
inline constexpr Foundation::UInt16 TaskStateSelector{0x28U};
inline constexpr Foundation::UInt64 SystemCallVector{256U};

/// RFLAGS of a CPL3 return (SanitizeUserFlags, IsUserReturnValid). IF and the
/// reserved bit 1 are always set. The permitted bits beyond those are the
/// flags CPL3 code can change itself with POPF, except TF: CF, PF, AF, ZF, SF,
/// DF, OF, NT, AC and ID, which a system call returns as the program left
/// them. TF (a supervised user debug trap is an Exception that terminates
/// that thread), IOPL, RF, VM, VIF, VIP and the reserved
/// bits are clear.
inline constexpr Foundation::UInt64 UserReturnRequiredFlags{0x202U};
inline constexpr Foundation::UInt64 UserReturnPermittedFlags{0x24'4ED7U};
/// Control-register policy Install applies on every CPU, the BSP after its initial activation and
/// each AP right after its startup code. CR0.MP and CR0.NE are set and CR0.EM and CR0.TS cleared,
/// so x87 and SSE instructions run and report their errors as exceptions; every context switch
/// saves and restores that state eagerly, so nothing traps on its first use. CR0.AM is cleared
/// (RFLAGS.AC, which user code may set, then enables no alignment check); the other CR0 bits keep
/// what the activation checked (PE, WP and PG set, CD and NW clear) or the startup code chose. CR4
/// is written whole, so no CPU keeps a firmware- or startup-dependent CR4 bit: PAE (IA-32e paging),
/// MCE (a machine check reaches its terminal IST handler instead of shutting the processor down),
/// OSFXSR (FXSAVE/FXRSTOR and SSE) and OSXMMEXCPT (SIMD floating-point errors raise #XM). Every
/// other CR4 bit is clear: no global pages, PCIDs or five-level paging (the root backend and the
/// initial activation), no FSGSBASE (user FS and GS bases stay zero), no SMEP or SMAP (not enabled
/// by this profile), no OSXSAVE (FXSAVE covers SSE; XSAVE is later), and no DE, TSD, PCE, UMIP,
/// VMXE, SMXE, PKE, CET, PKS, LASS or LAM.
inline constexpr Foundation::UInt64 InstalledControlRegister0Set{0x22U};
inline constexpr Foundation::UInt64 InstalledControlRegister0Clear{0x4'000CU};
inline constexpr Foundation::UInt64 InstalledControlRegister4{0x660U};

/// CPUID leaf 1 EDX bits every processor must report: FXSR, SSE and SSE2.
inline constexpr Foundation::UInt32 RequiredProcessorFeatures{(1U << 24U) | (1U << 25U) | (1U << 26U)};

/// The RFLAGS a CPL3 return restores for the user image `flags`: its
/// permitted bits, with IF and bit 1 set and every other bit clear.
[[nodiscard]] constexpr Foundation::UInt64 SanitizeUserFlags(const Foundation::UInt64 flags) noexcept
{
    return (flags & UserReturnPermittedFlags) | UserReturnRequiredFlags;
}

/// Stack image shared by SYSCALL and interrupt entry. Every field is owned
/// supervisor memory; no address supplied by user mode selects this storage.
struct Amd64TrapFrame
{
    Foundation::UInt64 R15;
    Foundation::UInt64 R14;
    Foundation::UInt64 R13;
    Foundation::UInt64 R12;
    Foundation::UInt64 R11;
    Foundation::UInt64 R10;
    Foundation::UInt64 R9;
    Foundation::UInt64 R8;
    Foundation::UInt64 Rbp;
    Foundation::UInt64 Rdi;
    Foundation::UInt64 Rsi;
    Foundation::UInt64 Rdx;
    Foundation::UInt64 Rcx;
    Foundation::UInt64 Rbx;
    Foundation::UInt64 Rax;
    Foundation::UInt64 Vector;
    Foundation::UInt64 Error;
    Foundation::UInt64 Rip;
    Foundation::UInt64 Cs;
    Foundation::UInt64 Rflags;
    Foundation::UInt64 Rsp;
    Foundation::UInt64 Ss;
};

struct alignas(16U) Amd64EntryContext
{
    Foundation::UInt64 KernelStackTop;
    Foundation::UInt64 UserStackScratch;
    Foundation::UInt64 Processor;
    Foundation::UInt64 Reserved;
};

/// Processor-owned tables. TSS fields are encoded bytewise, including its
/// unaligned 64-bit stack pointers. The I/O bitmap starts beyond the limit.
struct alignas(4096U) Amd64DescriptorTables
{
    Foundation::UInt64 Gdt[8U];
    Foundation::UInt8 Tss[104U];
    Foundation::UInt8 Gdtr[10U];
    Foundation::UInt8 Idtr[10U];
    Foundation::UInt8 Padding[3908U];
    Foundation::UInt64 Idt[512U];
};

enum class Amd64UserEntryError : Foundation::UInt8
{
    None,
    InvalidContext,
    InvalidStack,
    InvalidHandler,
    InvalidUserFrame,
};

class Amd64UserEntry final
{
public:
    Amd64UserEntry() = delete;

    /// Caller retains exclusive ownership of tables and context, and proves
    /// all four stacks are mapped supervisor RW/NX with distinct guard pages.
    [[nodiscard]] static Amd64UserEntryError
    Prepare(Amd64DescriptorTables& tables, const Amd64EntryContext& context, Foundation::UInt64 emergencyStackTop,
            Foundation::UInt64 nmiStackTop, Foundation::UInt64 machineCheckStackTop, const Foundation::UInt64* handlers,
            Foundation::Size handlerCount) noexcept;

    /// Whether IRETQ may restore `frame` to CPL3: the exact user selectors, a
    /// RIP in the lower canonical half above page zero, and RFLAGS equal to
    /// SanitizeUserFlags(RFLAGS). RSP is the program's own state and is not
    /// examined: IRETQ restores any value, and a stack the program cannot use
    /// faults in the program. Selectors and sanitized flags are kernel
    /// invariants; a rejected user RIP must fault only its thread.
    [[nodiscard]] static bool IsUserReturnValid(const Amd64TrapFrame& frame) noexcept;

    /// Makes a SYSCALL frame (the system-call vector) returnable: RFLAGS, the
    /// program's R11 image, becomes SanitizeUserFlags(RFLAGS); RSP and every
    /// other field stay unchanged. Legal user state (any flags POPF can set,
    /// any RSP) is never rejected. Returns false, changing nothing, when the
    /// frame is not a SYSCALL frame or IsUserReturnValid fails after the
    /// sanitization.
    [[nodiscard]] static bool PrepareSystemCallReturn(Amd64TrapFrame& frame) noexcept;

    /// An entry-authenticated SYSCALL with an unusable user return address.
    /// Deliver it as a supervised exception before any IRETQ or dispatch.
    [[nodiscard]] static bool IsSystemCallReturnFault(const Amd64TrapFrame& frame) noexcept;

    /// An entry-authenticated user frame with an unusable RIP, also after an interrupted fault
    /// yields behind a busy supervisor.
    [[nodiscard]] static bool IsUserReturnAddressFault(const Amd64TrapFrame& frame) noexcept;

    /// Owning CPU only, IF clear, mapped immutable code/tables/context and
    /// pinned stacks. Installs GDT, TSS, IDT, GS and the SYSCALL MSRs, and
    /// applies the control-register policy (InstalledControlRegister0Set,
    /// InstalledControlRegister0Clear, InstalledControlRegister4). Extended
    /// user state remains disabled until an extended-state owner is installed.
    static void Install(Amd64DescriptorTables& tables, Amd64EntryContext& context) noexcept;

    /// Owning CPU only, IF clear, after Install. Makes `kernelStackTop` the
    /// stack of the next CPL3 entry: both RSP0 of the TSS (interrupts and
    /// exceptions) and the SYSCALL stack top of the entry context. Each user
    /// thread owns its kernel stack, so the dispatcher selects the stack of
    /// the thread it returns to before every CPL3 return. The caller proves
    /// the stack is a mapped supervisor RW/NX stack with a guard page below
    /// it; the top is rejected, changing nothing, when it is not a canonical
    /// page boundary of at least 8 KiB.
    [[nodiscard]] static bool SelectKernelStack(Amd64DescriptorTables& tables, Amd64EntryContext& context,
                                                Foundation::UInt64 kernelStackTop) noexcept;

    /// Enters one validated CPL3 frame and abandons the current C++ call.
    [[noreturn]] static void Enter(Amd64TrapFrame& frame) noexcept;
};

extern "C" const Foundation::UInt64 AsteriaAmd64InterruptHandlers[256U];
extern "C" void AsteriaAmd64SystemCallEntry() noexcept;
extern "C" void AsteriaAmd64DispatchTrap(Amd64TrapFrame* frame) noexcept;

static_assert(sizeof(Amd64TrapFrame) == 176U);
static_assert(__builtin_offsetof(Amd64TrapFrame, R15) == 0U);
static_assert(__builtin_offsetof(Amd64TrapFrame, R14) == 8U);
static_assert(__builtin_offsetof(Amd64TrapFrame, R13) == 16U);
static_assert(__builtin_offsetof(Amd64TrapFrame, R12) == 24U);
static_assert(__builtin_offsetof(Amd64TrapFrame, R11) == 32U);
static_assert(__builtin_offsetof(Amd64TrapFrame, R10) == 40U);
static_assert(__builtin_offsetof(Amd64TrapFrame, R9) == 48U);
static_assert(__builtin_offsetof(Amd64TrapFrame, R8) == 56U);
static_assert(__builtin_offsetof(Amd64TrapFrame, Rbp) == 64U);
static_assert(__builtin_offsetof(Amd64TrapFrame, Rdi) == 72U);
static_assert(__builtin_offsetof(Amd64TrapFrame, Rsi) == 80U);
static_assert(__builtin_offsetof(Amd64TrapFrame, Rdx) == 88U);
static_assert(__builtin_offsetof(Amd64TrapFrame, Rcx) == 96U);
static_assert(__builtin_offsetof(Amd64TrapFrame, Rbx) == 104U);
static_assert(__builtin_offsetof(Amd64TrapFrame, Rax) == 112U);
static_assert(__builtin_offsetof(Amd64TrapFrame, Vector) == 120U);
static_assert(__builtin_offsetof(Amd64TrapFrame, Error) == 128U);
static_assert(__builtin_offsetof(Amd64TrapFrame, Rip) == 136U);
static_assert(__builtin_offsetof(Amd64TrapFrame, Cs) == 144U);
static_assert(__builtin_offsetof(Amd64TrapFrame, Rflags) == 152U);
static_assert(__builtin_offsetof(Amd64TrapFrame, Rsp) == 160U);
static_assert(__builtin_offsetof(Amd64TrapFrame, Ss) == 168U);
static_assert(sizeof(Amd64EntryContext) == 32U);
static_assert(sizeof(Amd64DescriptorTables) == 8192U);
static_assert(__builtin_offsetof(Amd64DescriptorTables, Idt) == 4096U);
} // Namespace Asteria::Platform::Amd64::Interrupt

#endif // ASTERIA_PLATFORM_AMD64_INTERRUPT_AMD64_USER_ENTRY_HPP_INCLUDED
