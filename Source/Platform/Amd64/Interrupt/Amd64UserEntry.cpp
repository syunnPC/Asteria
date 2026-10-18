// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Interrupt/Amd64UserEntry.hpp"

#include "Platform/Amd64/Memory/Amd64ControlRegister4.hpp"
#include "Platform/Amd64/Memory/UserAddressPolicy.hpp"

#include "Runtime/Memory.hpp"
#include "Platform/Synchronization/ProcessorContext.hpp"

namespace Asteria::Platform::Amd64::Interrupt
{
namespace
{
#if defined(ASTERIA_TARGET_AMD64)
Amd64DescriptorTables* g_processorTables[Kernel::Scheduler::MaximumProcessorCount]{};
Amd64EntryContext* g_processorEntries[Kernel::Scheduler::MaximumProcessorCount]{};
#endif
[[nodiscard]] bool IsCanonical(const Foundation::UInt64 value) noexcept
{
    return (value < Memory::UserAddressEnd) || (value >= 0xFFFF'8000'0000'0000ULL);
}

/// Selectors come from the entry path; RIP comes from user execution and must
/// occupy the shared user range before IRETQ can safely restore it.
[[nodiscard]] bool IsUserContextValid(const Amd64TrapFrame& frame) noexcept
{
    return (frame.Cs == UserCodeSelector) && (frame.Ss == UserDataSelector) && Memory::IsUserAddress(frame.Rip);
}

[[nodiscard]] Foundation::UInt64 AddressOf(const void* const value) noexcept
{
    return static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(value));
}

void Encode(Foundation::UInt8* const output, Foundation::UInt64 value, const Foundation::Size count) noexcept
{
    for (Foundation::Size index{0U}; index < count; ++index)
    {
        output[index] = static_cast<Foundation::UInt8>(value & 0xFFU);
        value >>= 8U;
    }
}

[[nodiscard]] bool Overlaps(const Foundation::UInt64 first, const Foundation::Size firstSize,
                            const Foundation::UInt64 second, const Foundation::Size secondSize) noexcept
{
    return first <= second ? second - first < firstSize : first - second < secondSize;
}

#if defined(ASTERIA_TARGET_AMD64)
void WriteMsr(const Foundation::UInt32 number, const Foundation::UInt64 value) noexcept
{
    __asm__ __volatile__("wrmsr"
                         :
                         : "c"(number), "a"(static_cast<Foundation::UInt32>(value)),
                           "d"(static_cast<Foundation::UInt32>(value >> 32U))
                         : "memory");
}

[[nodiscard]] Foundation::UInt64 ReadMsr(const Foundation::UInt32 number) noexcept
{
    Foundation::UInt32 low{0U};
    Foundation::UInt32 high{0U};
    __asm__ __volatile__("rdmsr" : "=a"(low), "=d"(high) : "c"(number));
    return (static_cast<Foundation::UInt64>(high) << 32U) | low;
}
#endif
} // namespace

Amd64UserEntryError Amd64UserEntry::Prepare(Amd64DescriptorTables& tables, const Amd64EntryContext& context,
                                            const Foundation::UInt64 emergencyStackTop,
                                            const Foundation::UInt64 nmiStackTop,
                                            const Foundation::UInt64 machineCheckStackTop,
                                            const Foundation::UInt64* const handlers,
                                            const Foundation::Size handlerCount) noexcept
{
    if ((context.Processor >= 4U) || (context.Reserved != 0U) || (context.UserStackScratch != 0U) ||
        Overlaps(AddressOf(&tables), sizeof(tables), AddressOf(&context), sizeof(context)))
    {
        return Amd64UserEntryError::InvalidContext;
    }
    const Foundation::UInt64 stacks[4U]{context.KernelStackTop, emergencyStackTop, nmiStackTop, machineCheckStackTop};
    for (Foundation::Size index{0U}; index < 4U; ++index)
    {
        if ((stacks[index] < 8192U) || ((stacks[index] & 0xFFFU) != 0U) || !IsCanonical(stacks[index]) ||
            !IsCanonical(stacks[index] - 8192U))
        {
            return Amd64UserEntryError::InvalidStack;
        }
        for (Foundation::Size previous{0U}; previous < index; ++previous)
        {
            if (Overlaps(stacks[index] - 8192U, 8192U, stacks[previous] - 8192U, 8192U))
            {
                return Amd64UserEntryError::InvalidStack;
            }
        }
    }
    if ((handlers == nullptr) || (handlerCount != 256U) ||
        Overlaps(AddressOf(&tables), sizeof(tables), AddressOf(handlers), handlerCount * sizeof(*handlers)))
    {
        return Amd64UserEntryError::InvalidHandler;
    }
    for (Foundation::Size index{0U}; index < handlerCount; ++index)
    {
        if ((handlers[index] == 0U) || !IsCanonical(handlers[index]))
        {
            return Amd64UserEntryError::InvalidHandler;
        }
    }
    static_cast<void>(memset(&tables, 0, sizeof(tables)));
    tables.Gdt[1U] = 0x00AF'9A00'0000'FFFFULL;
    tables.Gdt[2U] = 0x00CF'9200'0000'FFFFULL;
    tables.Gdt[3U] = 0x00CF'F200'0000'FFFFULL;
    tables.Gdt[4U] = 0x00AF'FA00'0000'FFFFULL;
    const Foundation::UInt64 tss = AddressOf(tables.Tss);
    tables.Gdt[5U] =
        103U | ((tss & 0xFFFFFFU) << 16U) | (Foundation::UInt64{0x89U} << 40U) | ((tss & 0xFF00'0000U) << 32U);
    tables.Gdt[6U] = tss >> 32U;
    Encode(tables.Tss + 4U, context.KernelStackTop, 8U);
    Encode(tables.Tss + 36U, emergencyStackTop, 8U);
    Encode(tables.Tss + 44U, nmiStackTop, 8U);
    Encode(tables.Tss + 52U, machineCheckStackTop, 8U);
    Encode(tables.Tss + 102U, sizeof(tables.Tss), 2U);
    Encode(tables.Gdtr, sizeof(tables.Gdt) - 1U, 2U);
    Encode(tables.Gdtr + 2U, AddressOf(tables.Gdt), 8U);
    Encode(tables.Idtr, sizeof(tables.Idt) - 1U, 2U);
    Encode(tables.Idtr + 2U, AddressOf(tables.Idt), 8U);
    for (Foundation::Size index{0U}; index < handlerCount; ++index)
    {
        const Foundation::UInt64 handler = handlers[index];
        // DF, NMI and MC use separate IST1/2/3 stacks. A winner may return
        // from a nested NMI; MC must never overwrite its live NMI stack.
        const Foundation::UInt64 ist = index == 8U ? 1U : (index == 2U ? 2U : (index == 18U ? 3U : 0U));
        tables.Idt[index * 2U] = (handler & 0xFFFFU) | (Foundation::UInt64{KernelCodeSelector} << 16U) | (ist << 32U) |
                                 (Foundation::UInt64{0x8EU} << 40U) | ((handler & 0xFFFF'0000U) << 32U);
        tables.Idt[index * 2U + 1U] = handler >> 32U;
    }
    return Amd64UserEntryError::None;
}

bool Amd64UserEntry::SelectKernelStack(Amd64DescriptorTables& tables, Amd64EntryContext& context,
                                       const Foundation::UInt64 kernelStackTop) noexcept
{
    if ((kernelStackTop < 8192U) || ((kernelStackTop & 0xFFFU) != 0U) || !IsCanonical(kernelStackTop) ||
        !IsCanonical(kernelStackTop - 8192U))
    {
        return false;
    }
    // Launch/resume changes only this CPU's pair.
    Amd64DescriptorTables* selectedTables = &tables;
    Amd64EntryContext* selectedContext = &context;
#if defined(ASTERIA_TARGET_AMD64)
    const auto processor = Platform::Synchronization::CurrentProcessor();
    if (processor.IsValid() && g_processorTables[processor.GetValue()] != nullptr)
    {
        selectedTables = g_processorTables[processor.GetValue()];
        selectedContext = g_processorEntries[processor.GetValue()];
    }
#endif
    Encode(selectedTables->Tss + 4U, kernelStackTop, 8U);
    selectedContext->KernelStackTop = kernelStackTop;
    return true;
}

bool Amd64UserEntry::IsUserReturnValid(const Amd64TrapFrame& frame) noexcept
{
    return IsUserContextValid(frame) && (frame.Rflags == SanitizeUserFlags(frame.Rflags));
}

bool Amd64UserEntry::PrepareSystemCallReturn(Amd64TrapFrame& frame) noexcept
{
    if ((frame.Vector != SystemCallVector) || !IsUserContextValid(frame))
    {
        return false;
    }
    frame.Rflags = SanitizeUserFlags(frame.Rflags);
    return true;
}

bool Amd64UserEntry::IsSystemCallReturnFault(const Amd64TrapFrame& frame) noexcept
{
    // SYSCALL at UserAddressEnd - 2 saves the noncanonical next instruction
    // in RCX (Intel SDM 325462-093US, Vol. 2B, SYSCALL, Operation). Selectors
    // and vector remain kernel invariants; this RIP is the user's fault.
    return frame.Vector == SystemCallVector && IsUserReturnAddressFault(frame);
}

bool Amd64UserEntry::IsUserReturnAddressFault(const Amd64TrapFrame& frame) noexcept
{
    return frame.Cs == UserCodeSelector && frame.Ss == UserDataSelector && !Memory::IsUserAddress(frame.Rip);
}

#if defined(ASTERIA_TARGET_AMD64)
extern "C" void AsteriaAmd64LoadDescriptors(const Foundation::UInt8* gdtr, const Foundation::UInt8* idtr) noexcept;
extern "C" [[noreturn]] void AsteriaAmd64RestoreUserFrame(Amd64TrapFrame* frame) noexcept;

void Amd64UserEntry::Install(Amd64DescriptorTables& tables, Amd64EntryContext& context) noexcept
{
    if (context.Processor >= Kernel::Scheduler::MaximumProcessorCount)
    {
        __builtin_trap();
    }
    g_processorTables[context.Processor] = &tables;
    g_processorEntries[context.Processor] = &context;
    AsteriaAmd64LoadDescriptors(tables.Gdtr, tables.Idtr);
    Foundation::UInt64 control{0U};
    __asm__ __volatile__("mov %%cr0,%0" : "=r"(control));
    control = (control | InstalledControlRegister0Set) & ~InstalledControlRegister0Clear;
    __asm__ __volatile__("mov %0,%%cr0" : : "r"(control) : "memory");
    // The whole register, not a change of some bits: every CPU ends with the
    // same CR4 whatever firmware or the startup code left. PAE stays set, so
    // the write is legal in IA-32e mode.
    control = InstalledControlRegister4;
    __asm__ __volatile__("and %1,%0; mov %0,%%cr4"
                         : "+r"(control)
                         : "i"(~Memory::DisabledTranslationFeatures)
                         : "memory", "cc");
    WriteMsr(0xC000'0100U, 0U);
    WriteMsr(0xC000'0101U, AddressOf(&context));
    WriteMsr(0xC000'0102U, 0U);
    WriteMsr(0xC000'0081U, Foundation::UInt64{KernelCodeSelector} << 32U);
    WriteMsr(0xC000'0082U,
             static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(&AsteriaAmd64SystemCallEntry)));
    WriteMsr(0xC000'0084U, 0x0004'7700U); // IF/TF/DF/IOPL/NT/AC.
    WriteMsr(0xC000'0080U, ReadMsr(0xC000'0080U) | 1U);
}

[[noreturn]] void Amd64UserEntry::Enter(Amd64TrapFrame& frame) noexcept
{
    if (IsUserReturnValid(frame))
    {
        AsteriaAmd64RestoreUserFrame(&frame);
    }
    for (;;)
    {
        __asm__ __volatile__("cli; hlt" : : : "memory");
    }
}
#else
void Amd64UserEntry::Install(Amd64DescriptorTables&, Amd64EntryContext&) noexcept
{
}
[[noreturn]] void Amd64UserEntry::Enter(Amd64TrapFrame&) noexcept
{
    __builtin_trap();
}
#endif
} // Namespace Asteria::Platform::Amd64::Interrupt
