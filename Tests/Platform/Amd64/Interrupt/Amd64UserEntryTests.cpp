// SPDX-License-Identifier: Apache-2.0
#include "Platform/Amd64/Interrupt/Amd64UserEntry.hpp"

#include <cstdio>
#include <cstring>

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace
{
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt64;
using Asteria::Platform::Amd64::Interrupt::Amd64DescriptorTables;
using Asteria::Platform::Amd64::Interrupt::Amd64EntryContext;
using Asteria::Platform::Amd64::Interrupt::Amd64TrapFrame;
using Asteria::Platform::Amd64::Interrupt::Amd64UserEntry;
using Asteria::Platform::Amd64::Interrupt::Amd64UserEntryError;
Amd64DescriptorTables g_tables{};
UInt64 g_handlers[256U]{};

constexpr UInt64 UserRip{0x4000'0D4FU};

// RFLAGS bits (Intel SDM vol. 1, 3.4.3) a SYSCALL frame carries back as the
// program left them: CF, PF, AF, ZF, SF, DF, OF, NT, AC and ID, the bits CPL3
// code changes with POPF except TF. IF and the reserved bit 1 are always set.
constexpr Size PreservedFlagBits[]{0U, 2U, 4U, 6U, 7U, 10U, 11U, 14U, 18U, 21U};
constexpr Size ForcedFlagBits[]{1U, 9U};

[[nodiscard]] bool Contains(const Size* const bits, const Size count, const Size bit) noexcept
{
    for (Size index{0U}; index < count; ++index)
    {
        if (bits[index] == bit)
        {
            return true;
        }
    }
    return false;
}

/// The frame the SYSCALL stub builds for a program at UserRip with `rsp` and
/// `rflags` (R11 holds the same RFLAGS image, RCX the return address).
[[nodiscard]] Amd64TrapFrame SystemCallFrame(const UInt64 rsp, const UInt64 rflags) noexcept
{
    Amd64TrapFrame frame{};
    frame.Vector = 256U;
    frame.Cs = 0x23U;
    frame.Ss = 0x1BU;
    frame.Rip = UserRip;
    frame.Rcx = UserRip;
    frame.Rsp = rsp;
    frame.Rflags = rflags;
    frame.R11 = rflags;
    frame.Rax = 2U;
    frame.Rdi = 0x1234U;
    frame.R15 = 0xF15U;
    return frame;
}

/// Every field of `after` except RFLAGS equals `before`.
[[nodiscard]] bool OnlyFlagsDiffer(const Amd64TrapFrame& before, const Amd64TrapFrame& after) noexcept
{
    Amd64TrapFrame expected{before};
    expected.Rflags = after.Rflags;
    return std::memcmp(&expected, &after, sizeof(after)) == 0;
}

[[nodiscard]] bool Unchanged(const Amd64TrapFrame& before, const Amd64TrapFrame& after) noexcept
{
    return std::memcmp(&before, &after, sizeof(after)) == 0;
}
} // namespace

int main()
{
    TestContext runner{"Amd64UserEntryTests"};
    for (auto& handler : g_handlers)
    {
        handler = 0xFFFF'8000'1234'5678ULL;
    }
    Amd64EntryContext context{0x100000U, 0U, 0U, 0U};
    ASTERIA_EXPECT(runner, "valid entry tables",
                   Amd64UserEntry::Prepare(g_tables, context, 0x110000U, 0x120000U, 0x130000U, g_handlers, 256U) ==
                       Amd64UserEntryError::None);
    ASTERIA_EXPECT(runner, "CPL3 data/code selectors", g_tables.Gdt[3U] == 0x00CF'F200'0000'FFFFULL,
                   g_tables.Gdt[4U] == 0x00AF'FA00'0000'FFFFULL);
    ASTERIA_EXPECT(runner, "available 64-bit TSS descriptor", ((g_tables.Gdt[5U] >> 40U) & 0xFFU) == 0x89U);
    ASTERIA_EXPECT(runner, "RSP0 and denied I/O bitmap", g_tables.Tss[6U] == 0x10U, g_tables.Tss[102U] == 104U);
    bool gatesValid{true};
    for (Asteria::Foundation::Size i{0U}; i < 256U; ++i)
    {
        const UInt64 low = g_tables.Idt[i * 2U];
        const UInt64 address = (low & 0xFFFFU) | ((low >> 32U) & 0xFFFF'0000U) | (g_tables.Idt[i * 2U + 1U] << 32U);
        const UInt64 ist = i == 8U ? 1U : (i == 2U ? 2U : (i == 18U ? 3U : 0U));
        gatesValid =
            gatesValid && address == g_handlers[i] && ((low >> 40U) & 0xFFU) == 0x8EU && ((low >> 32U) & 7U) == ist;
    }
    ASTERIA_EXPECT(runner, "all gates retain exact handler, DPL0 and IST policy", gatesValid);
    const UInt64 before = g_tables.Idt[0U];
    g_handlers[255U] = 0x0000'8000'0000'0000ULL;
    ASTERIA_EXPECT(runner, "noncanonical handler rejected",
                   Amd64UserEntry::Prepare(g_tables, context, 0x110000U, 0x120000U, 0x130000U, g_handlers, 256U) ==
                       Amd64UserEntryError::InvalidHandler);
    ASTERIA_EXPECT(runner, "handler failure leaves tables unchanged", g_tables.Idt[0U] == before);
    g_handlers[255U] = g_handlers[0U];
    ASTERIA_EXPECT(runner, "overlapping emergency stack rejected",
                   Amd64UserEntry::Prepare(g_tables, context, 0x101000U, 0x120000U, 0x130000U, g_handlers, 256U) ==
                       Amd64UserEntryError::InvalidStack);
    context.Processor = 4U;
    ASTERIA_EXPECT(runner, "invalid processor rejected",
                   Amd64UserEntry::Prepare(g_tables, context, 0x110000U, 0x120000U, 0x130000U, g_handlers, 256U) ==
                       Amd64UserEntryError::InvalidContext);
    // Every RFLAGS bit a program can hold at SYSCALL returns as the ABI
    // states: preserved, forced set, or cleared. No flag value is rejected.
    for (Size bit{0U}; bit < 64U; ++bit)
    {
        const UInt64 flag{UInt64{1U} << bit};
        const bool preserved = Contains(PreservedFlagBits, sizeof(PreservedFlagBits) / sizeof(Size), bit);
        const UInt64 expected = (preserved ? flag : 0U) | 0x202U;
        const Amd64TrapFrame original = SystemCallFrame(0x5000'4F00U, flag);
        Amd64TrapFrame frame{original};
        ASTERIA_EXPECT(runner, "SYSCALL frame flag bit is preserved or cleared, IF and bit 1 set",
                       Amd64UserEntry::PrepareSystemCallReturn(frame), frame.Rflags == expected,
                       Asteria::Platform::Amd64::Interrupt::SanitizeUserFlags(flag) == expected,
                       OnlyFlagsDiffer(original, frame), Amd64UserEntry::IsUserReturnValid(frame));
        const bool forced = Contains(ForcedFlagBits, sizeof(ForcedFlagBits) / sizeof(Size), bit);
        Amd64TrapFrame launch = SystemCallFrame(0x5000'5000U, 0x202U | flag);
        ASTERIA_EXPECT(runner, "a kernel-built return with an unsanitized flag is rejected",
                       Amd64UserEntry::IsUserReturnValid(launch) == (preserved || forced));
    }
    Amd64TrapFrame all = SystemCallFrame(0x5000'4F00U, ~UInt64{0U});
    ASTERIA_EXPECT(runner, "all flag bits reduce to the permitted set", Amd64UserEntry::PrepareSystemCallReturn(all),
                   all.Rflags == 0x24'4ED7U);

    // RSP is the program's own state: any value, zero and noncanonical ones
    // included, returns unchanged.
    constexpr UInt64 stacks[]{0U,         8U, 4095U, 0x7FFF'FFFF'FFF8ULL, 0x8000'0000'0000ULL, 0xFFFF'8000'0000'0000ULL,
                              ~UInt64{0U}};
    for (const UInt64 stack : stacks)
    {
        const Amd64TrapFrame original = SystemCallFrame(stack, 0x24'0246U);
        Amd64TrapFrame frame{original};
        ASTERIA_EXPECT(runner, "any user stack pointer is a valid return",
                       Amd64UserEntry::PrepareSystemCallReturn(frame), Unchanged(original, frame),
                       Amd64UserEntry::IsUserReturnValid(frame));
    }

    // RIP may be anywhere in the lower canonical half above page zero.
    constexpr UInt64 userRips[]{0x1000U, 0x7FFF'FFFF'FFFFULL};
    for (const UInt64 rip : userRips)
    {
        Amd64TrapFrame frame = SystemCallFrame(0x5000'4F00U, 0x202U);
        frame.Rip = rip;
        ASTERIA_EXPECT(runner, "a RIP at either end of the user half is a valid return",
                       Amd64UserEntry::PrepareSystemCallReturn(frame), Amd64UserEntry::IsUserReturnValid(frame));
    }

    // Fields the entry stub or the address space fixes are still checked;
    // a rejected frame is left unchanged.
    struct Corruption
    {
        const char* Description;
        UInt64 Amd64TrapFrame::* Field;
        UInt64 Value;
    };
    constexpr Corruption corruptions[]{
        {"kernel code selector", &Amd64TrapFrame::Cs, 0x08U},
        {"kernel data selector", &Amd64TrapFrame::Ss, 0x10U},
        {"RIP in page zero", &Amd64TrapFrame::Rip, 0xFFFU},
        {"noncanonical RIP", &Amd64TrapFrame::Rip, 0x8000'0000'0000ULL},
        {"kernel RIP", &Amd64TrapFrame::Rip, 0xFFFF'8000'0000'1000ULL},
        {"interrupt vector", &Amd64TrapFrame::Vector, 0xE8U},
    };
    for (const Corruption& corruption : corruptions)
    {
        Amd64TrapFrame original = SystemCallFrame(0U, 0x4'0102U);
        original.*corruption.Field = corruption.Value;
        Amd64TrapFrame frame{original};
        const bool rejected = !Amd64UserEntry::PrepareSystemCallReturn(frame);
        if (!ASTERIA_EXPECT(runner, "a wrong kernel-produced field is rejected unchanged", rejected,
                            Unchanged(original, frame)))
        {
            std::fprintf(stderr, "  corruption: %s\n", corruption.Description);
        }
    }

    // The final two canonical bytes can hold SYSCALL; its next RIP is outside
    // the permitted range. This is supervised before dispatch or IRETQ.
    constexpr UInt64 badReturns[]{0xFFFU, 0x8000'0000'0000ULL, 0xFFFF'8000'0000'1000ULL};
    for (const UInt64 rip : badReturns)
    {
        auto frame = SystemCallFrame(0U, 0x202U);
        frame.Rip = rip;
        ASTERIA_EXPECT(runner,
                       "an unusable SYSCALL return is a user fault; kernel-produced selectors remain invariants",
                       Amd64UserEntry::IsSystemCallReturnFault(frame), !Amd64UserEntry::PrepareSystemCallReturn(frame));
        frame.Cs = 0x08U;
        ASTERIA_EXPECT(runner, "a corrupted kernel selector cannot be disguised as a user return fault",
                       !Amd64UserEntry::IsSystemCallReturnFault(frame));
        frame.Cs = Asteria::Platform::Amd64::Interrupt::UserCodeSelector;
        frame.Vector = 14U;
        ASTERIA_EXPECT(runner, "interrupt frames are not classified as SYSCALL return faults",
                       !Amd64UserEntry::IsSystemCallReturnFault(frame),
                       Amd64UserEntry::IsUserReturnAddressFault(frame));
        frame.Ss = 0x10U;
        ASTERIA_EXPECT(runner, "an interrupt's corrupted selector remains a kernel invariant",
                       !Amd64UserEntry::IsUserReturnAddressFault(frame));
    }

    // Each user thread's kernel stack becomes both RSP0 and the SYSCALL stack
    // top before its CPL3 return; an unusable top changes neither.
    Amd64EntryContext selected{0x100000U, 0U, 0U, 0U};
    const bool switched = Amd64UserEntry::SelectKernelStack(g_tables, selected, 0x0123'4000U);
    UInt64 rsp0{0U};
    for (Size index{0U}; index < 8U; ++index)
    {
        rsp0 |= static_cast<UInt64>(g_tables.Tss[4U + index]) << (8U * index);
    }
    ASTERIA_EXPECT(runner, "a selected kernel stack is both RSP0 and the SYSCALL stack top", switched,
                   rsp0 == 0x0123'4000U, selected.KernelStackTop == 0x0123'4000U);
    const bool unaligned = Amd64UserEntry::SelectKernelStack(g_tables, selected, 0x0123'4008U);
    const bool low = Amd64UserEntry::SelectKernelStack(g_tables, selected, 0x1000U);
    const bool noncanonical = Amd64UserEntry::SelectKernelStack(g_tables, selected, 0x0000'8000'0000'1000ULL);
    ASTERIA_EXPECT(runner, "an unaligned, low or noncanonical stack top changes nothing", !unaligned, !low,
                   !noncanonical, selected.KernelStackTop == 0x0123'4000U, g_tables.Tss[5U] == 0x40U);
    return runner.Finish();
}
