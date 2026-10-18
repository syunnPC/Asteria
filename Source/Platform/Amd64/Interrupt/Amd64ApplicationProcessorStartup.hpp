// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_INTERRUPT_AMD64_APPLICATION_PROCESSOR_STARTUP_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_INTERRUPT_AMD64_APPLICATION_PROCESSOR_STARTUP_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Platform/Amd64/Time/Amd64ApicTimer.hpp"

namespace Asteria::Platform::Amd64::Interrupt
{
struct Amd64StartupMailbox
{
    Foundation::UInt32 Root;
    Foundation::UInt32 Reserved;
    Foundation::UInt32 LongEntry;
    Foundation::UInt16 Selector;
    Foundation::UInt16 Reserved16;
    Foundation::UInt64 StackTop;
    Foundation::UInt64 Context;
    Foundation::UInt64 Processor;
    Foundation::UInt64 Entry;
    Foundation::UInt64 Reserved64[2U];
    Foundation::UInt64 Gdt[3U];
    Foundation::UInt16 GdtLimit;
    Foundation::UInt8 GdtBase[4U];
    Foundation::UInt16 Padding;
};

/// A serialized INIT/SIPI transport. The caller owns two WB LoaderData pages
/// below 1 MiB, maps the first RX and the second RW/NX in every runnable root,
/// and never rewrites the code page after publishing the first startup IPI.
class Amd64ApplicationProcessorStartup final
{
public:
    [[nodiscard]] static bool PrepareCode(Foundation::UInt8* storage, Foundation::UInt64 physical,
                                          const Foundation::UInt8* code, Foundation::Size codeBytes) noexcept;
    [[nodiscard]] static bool PrepareMailbox(Amd64StartupMailbox& mailbox, Foundation::UInt64 physical,
                                             Foundation::UInt64 root, Foundation::Size longOffset,
                                             Foundation::UInt64 stackTop, Foundation::UInt64 context,
                                             Foundation::UInt64 processor, Foundation::UInt64 entry) noexcept;
    [[nodiscard]] static bool Start(Time::Amd64ApicTimer& apic, Foundation::UInt32 hardwareId,
                                    Foundation::UInt64 physical) noexcept;
    /// Calibrated TSC delay, valid on every CPU after BSP calibration.
    [[nodiscard]] static bool DelayMicroseconds(Foundation::UInt32 microseconds) noexcept;
};

extern "C" const Foundation::UInt8 AsteriaAmd64StartupCodeStart[];
extern "C" const Foundation::UInt8 AsteriaAmd64StartupLongEntry[];
extern "C" const Foundation::UInt8 AsteriaAmd64StartupCodeEnd[];
static_assert(sizeof(Amd64StartupMailbox) == 96U);
static_assert(__builtin_offsetof(Amd64StartupMailbox, StackTop) == 16U);
static_assert(__builtin_offsetof(Amd64StartupMailbox, Gdt) == 64U);
static_assert(__builtin_offsetof(Amd64StartupMailbox, GdtLimit) == 88U);
} // namespace Asteria::Platform::Amd64::Interrupt
#endif
