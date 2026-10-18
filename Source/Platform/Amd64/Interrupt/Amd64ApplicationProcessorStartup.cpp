// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Interrupt/Amd64ApplicationProcessorStartup.hpp"

#include "Runtime/Memory.hpp"
#include "Platform/Amd64/Time/Amd64TscClock.hpp"

namespace Asteria::Platform::Amd64::Interrupt
{
namespace
{
[[nodiscard]] bool ValidPhysical(const Foundation::UInt64 physical) noexcept
{
    return physical >= 4096U && physical <= 0xFE000U && (physical & 4095U) == 0U;
}
[[nodiscard]] bool Canonical(const Foundation::UInt64 address) noexcept
{
    return (address != 0U) && ((address < (Foundation::UInt64{1U} << 47U)) || (address >= 0xFFFF'8000'0000'0000ULL));
}

} // namespace

bool Amd64ApplicationProcessorStartup::PrepareCode(Foundation::UInt8* const storage, const Foundation::UInt64 physical,
                                                   const Foundation::UInt8* const code,
                                                   const Foundation::Size codeBytes) noexcept
{
    const auto output = reinterpret_cast<Foundation::UIntPointer>(storage);
    const auto source = reinterpret_cast<Foundation::UIntPointer>(code);
    if (storage == nullptr || code == nullptr || !ValidPhysical(physical) || (output & 4095U) != 0U ||
        codeBytes == 0U || codeBytes > 4096U || output > ~Foundation::UIntPointer{0U} - 4096U ||
        source > ~Foundation::UIntPointer{0U} - codeBytes || (output < source + codeBytes && source < output + 4096U))
    {
        return false;
    }
    static_cast<void>(memset(storage, 0, 4096U));
    static_cast<void>(memcpy(storage, code, codeBytes));
    return true;
}

bool Amd64ApplicationProcessorStartup::PrepareMailbox(Amd64StartupMailbox& mailbox, const Foundation::UInt64 physical,
                                                      const Foundation::UInt64 root, const Foundation::Size longOffset,
                                                      const Foundation::UInt64 stackTop,
                                                      const Foundation::UInt64 context,
                                                      const Foundation::UInt64 processor,
                                                      const Foundation::UInt64 entry) noexcept
{
    if (!ValidPhysical(physical) || root == 0U || root > 0xFFFF'F000U || (root & 4095U) != 0U || longOffset == 0U ||
        longOffset >= 4096U || processor == 0U || processor >= 4U || !Canonical(stackTop) || stackTop < 16384U ||
        (stackTop & 4095U) != 0U || !Canonical(stackTop - 16384U) || !Canonical(context) || !Canonical(entry))
    {
        return false;
    }
    mailbox = {};
    mailbox.Root = static_cast<Foundation::UInt32>(root);
    mailbox.LongEntry = static_cast<Foundation::UInt32>(physical + longOffset);
    mailbox.Selector = 8U;
    mailbox.StackTop = stackTop;
    mailbox.Context = context;
    mailbox.Processor = processor;
    mailbox.Entry = entry;
    mailbox.Gdt[1U] = 0x00AF'9A00'0000'FFFFULL;
    mailbox.Gdt[2U] = 0x00CF'9200'0000'FFFFULL;
    mailbox.GdtLimit = 23U;
    const Foundation::UInt64 gdt = physical + 4096U + 64U;
    for (Foundation::Size byte{0U}; byte < 4U; ++byte)
    {
        mailbox.GdtBase[byte] = static_cast<Foundation::UInt8>(gdt >> (byte * 8U));
    }
    return true;
}

bool Amd64ApplicationProcessorStartup::DelayMicroseconds(const Foundation::UInt32 microseconds) noexcept
{
    return Time::DelayTscMicroseconds(microseconds);
}

bool Amd64ApplicationProcessorStartup::Start(Time::Amd64ApicTimer& apic, const Foundation::UInt32 hardwareId,
                                             const Foundation::UInt64 physical) noexcept
{
    if (!ValidPhysical(physical))
    {
        return false;
    }
#if defined(ASTERIA_TARGET_AMD64)
    return apic.GetApic().Send(hardwareId, 0xC500U) && DelayMicroseconds(10'000U) &&
           apic.GetApic().Send(hardwareId, 0x8500U) && DelayMicroseconds(200U) &&
           apic.GetApic().Send(hardwareId, 0x600U | static_cast<Foundation::UInt32>(physical >> 12U)) &&
           DelayMicroseconds(200U) &&
           apic.GetApic().Send(hardwareId, 0x600U | static_cast<Foundation::UInt32>(physical >> 12U));
#else
    static_cast<void>(apic);
    static_cast<void>(hardwareId);
    return false;
#endif
}
} // namespace Asteria::Platform::Amd64::Interrupt
