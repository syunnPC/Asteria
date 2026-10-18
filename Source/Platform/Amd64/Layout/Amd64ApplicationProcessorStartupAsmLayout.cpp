// SPDX-License-Identifier: Apache-2.0

// Amd64StartupMailbox offsets for Interrupt/Amd64ApplicationProcessorStartup.S
// (see Platform/AsmLayout.hpp).

#include "Platform/AsmLayout.hpp"
#include "Platform/Amd64/Interrupt/Amd64ApplicationProcessorStartup.hpp"

namespace
{
using Asteria::Platform::Amd64::Interrupt::Amd64StartupMailbox;
} // namespace

extern "C" void AsteriaAmd64ApplicationProcessorStartupAsmLayout() noexcept
{
    ASTERIA_ASM_LAYOUT(ASTERIA_AMD64_STARTUP_MAILBOX_ROOT, __builtin_offsetof(Amd64StartupMailbox, Root));
    ASTERIA_ASM_LAYOUT(ASTERIA_AMD64_STARTUP_MAILBOX_LONG_ENTRY, __builtin_offsetof(Amd64StartupMailbox, LongEntry));
    ASTERIA_ASM_LAYOUT(ASTERIA_AMD64_STARTUP_MAILBOX_STACK_TOP, __builtin_offsetof(Amd64StartupMailbox, StackTop));
    ASTERIA_ASM_LAYOUT(ASTERIA_AMD64_STARTUP_MAILBOX_CONTEXT, __builtin_offsetof(Amd64StartupMailbox, Context));
    ASTERIA_ASM_LAYOUT(ASTERIA_AMD64_STARTUP_MAILBOX_PROCESSOR, __builtin_offsetof(Amd64StartupMailbox, Processor));
    ASTERIA_ASM_LAYOUT(ASTERIA_AMD64_STARTUP_MAILBOX_ENTRY, __builtin_offsetof(Amd64StartupMailbox, Entry));
    ASTERIA_ASM_LAYOUT(ASTERIA_AMD64_STARTUP_MAILBOX_GDT_LIMIT, __builtin_offsetof(Amd64StartupMailbox, GdtLimit));
}
