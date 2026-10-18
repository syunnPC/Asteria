// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Interrupt/Amd64LocalApic.hpp"
#include "Kernel/Scheduler/CrossProcessorWait.hpp"

#if defined(ASTERIA_TARGET_AMD64)
extern "C" void AsteriaAmd64WriteX2ApicInterruptCommand(Asteria::Foundation::UInt64 command) noexcept;
#endif

namespace Asteria::Platform::Amd64::Interrupt
{
namespace
{
#if defined(ASTERIA_TARGET_AMD64)
[[nodiscard]] Foundation::UInt64 ReadMsr(const Foundation::UInt32 index) noexcept
{
    Foundation::UInt32 low{0U};
    Foundation::UInt32 high{0U};
    __asm__ __volatile__("rdmsr" : "=a"(low), "=d"(high) : "c"(index) : "memory");
    return (static_cast<Foundation::UInt64>(high) << 32U) | low;
}
void WriteMsr(const Foundation::UInt32 index, const Foundation::UInt32 value) noexcept
{
    __asm__ __volatile__("wrmsr" : : "c"(index), "a"(value), "d"(0U) : "memory");
}
#endif
} // Namespace

bool Amd64LocalApic::Initialize(const Foundation::UInt64 mappedAddress) noexcept
{
    if (IsReady())
    {
        return false;
    }
#if defined(ASTERIA_TARGET_AMD64)
    Foundation::UInt32 eax{0U};
    Foundation::UInt32 ebx{0U};
    Foundation::UInt32 ecx{0U};
    Foundation::UInt32 edx{0U};
    __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1U), "c"(0U));
    if ((edx & (1U << 9U)) == 0U)
    {
        return false;
    }
    const Foundation::UInt64 base = ReadMsr(0x1BU);
    if ((base & 0x800U) == 0U)
    {
        return false;
    }
    if ((base & 0x400U) != 0U)
    {
        if ((ecx & (1U << 21U)) == 0U)
        {
            return false;
        }
        m_x2Apic = true;
        return true;
    }
    if ((base & 0xFFFF'FFFF'FFFF'F000ULL) != mappedAddress)
    {
        return false;
    }
#endif
    if (mappedAddress == 0U || (mappedAddress & 4095U) != 0U)
    {
        return false;
    }
    // The caller owns the page mapping; this borrow never outlives it.
    m_registers = reinterpret_cast<volatile Foundation::UInt32*>(static_cast<Foundation::UIntPointer>(mappedAddress));
    return true;
}

bool Amd64LocalApic::InitializeIdentity() noexcept
{
#if defined(ASTERIA_TARGET_AMD64)
    Foundation::UInt32 eax{0U};
    Foundation::UInt32 ebx{0U};
    Foundation::UInt32 ecx{0U};
    Foundation::UInt32 edx{0U};
    __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1U), "c"(0U));
    if ((edx & (1U << 9U)) == 0U)
    {
        return false;
    }
    return Initialize(ReadMsr(0x1BU) & 0xFFFF'FFFF'FFFF'F000ULL);
#else
    return false;
#endif
}

Foundation::UInt32 Amd64LocalApic::Read(const Foundation::UInt32 offset) const noexcept
{
#if defined(ASTERIA_TARGET_AMD64)
    if (m_x2Apic)
    {
        return static_cast<Foundation::UInt32>(ReadMsr(MsrFor(offset)));
    }
#endif
    return m_registers[offset / 4U];
}

void Amd64LocalApic::Write(const Foundation::UInt32 offset, const Foundation::UInt32 value) const noexcept
{
#if defined(ASTERIA_TARGET_AMD64)
    if (m_x2Apic)
    {
        WriteMsr(MsrFor(offset), value);
        return;
    }
#endif
    m_registers[offset / 4U] = value;
    static_cast<void>(m_registers[0x20U / 4U]);
}

Foundation::UInt32 Amd64LocalApic::GetId() const noexcept
{
    const Foundation::UInt32 id = Read(0x20U);
    return m_x2Apic ? id : id >> 24U;
}

bool Amd64LocalApic::Send(const Foundation::UInt32 destination, const Foundation::UInt32 command) const noexcept
{
    if (!IsReady() || destination == 0xFFFF'FFFFU)
    {
        return false;
    }
#if defined(ASTERIA_TARGET_AMD64)
    if (m_x2Apic)
    {
        // Shared with the shootdown notifier: MFENCE/LFENCE orders published
        // mailboxes before the single 64-bit ICR write. No busy bit exists.
        AsteriaAmd64WriteX2ApicInterruptCommand((static_cast<Foundation::UInt64>(destination) << 32U) | command);
        return true;
    }
#endif
    if (destination >= 255U)
    {
        return false;
    }
    // ICR delivery waits also service the initiator.
    for (Kernel::Scheduler::CrossProcessorWait wait; (Read(0x300U) & 0x1000U) != 0U;)
    {
        if (!wait.Poll())
        {
            return false;
        }
    }
#if defined(ASTERIA_TARGET_AMD64)
    __asm__ __volatile__("mfence" : : : "memory");
#endif
    Write(0x310U, destination << 24U);
    Write(0x300U, command);
    for (Kernel::Scheduler::CrossProcessorWait wait; (Read(0x300U) & 0x1000U) != 0U;)
    {
        if (!wait.Poll())
        {
            return false;
        }
    }
    return true;
}

bool Amd64LocalApic::TrySendNmi(const Foundation::UInt32 destination) const noexcept
{
    if (!IsReady() || destination == 0xFFFF'FFFFU)
    {
        return false;
    }
#if defined(ASTERIA_TARGET_AMD64)
    if (m_x2Apic)
    {
        AsteriaAmd64WriteX2ApicInterruptCommand((static_cast<Foundation::UInt64>(destination) << 32U) | (4U << 8U));
        return true;
    }
#endif
    if (destination >= 255U || (Read(0x300U) & 0x1000U) != 0U)
    {
        return false;
    }
    Write(0x310U, destination << 24U);
    Write(0x300U, 4U << 8U);
    return true;
}

bool Amd64LocalApic::SendSelf(const Foundation::UInt8 vector) const noexcept
{
    if (!IsReady() || vector < 16U || vector == 255U)
    {
        return false;
    }
#if defined(ASTERIA_TARGET_AMD64)
    if (m_x2Apic)
    {
        __asm__ __volatile__("mfence; lfence" : : : "memory");
        WriteMsr(0x83FU, vector);
        return true;
    }
#endif
    return Send(GetId(), vector);
}
} // Namespace Asteria::Platform::Amd64::Interrupt
