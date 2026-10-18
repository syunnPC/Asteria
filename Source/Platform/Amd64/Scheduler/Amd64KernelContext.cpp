// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Scheduler/Amd64KernelContext.hpp"

extern "C" void AsteriaAmd64StartKernelContext() noexcept;

extern "C" void
AsteriaAmd64SwitchKernelContext(Asteria::Platform::Amd64::Scheduler::Amd64KernelRegisterState* current,
                                const Asteria::Platform::Amd64::Scheduler::Amd64KernelRegisterState* next) noexcept;

// The target entry points are in Amd64KernelContext.S.
#if !defined(ASTERIA_TARGET_AMD64)
extern "C" void AsteriaAmd64StartKernelContext() noexcept
{
}
#endif

namespace Asteria::Platform::Amd64::Scheduler
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 LowerCanonicalExclusiveEnd{0x0000'8000'0000'0000ULL};
constexpr Foundation::UInt64 UpperCanonicalStart{0xFFFF'8000'0000'0000ULL};

[[nodiscard]] constexpr bool IsPageAligned(const Foundation::UInt64 value) noexcept
{
    return (value & (Amd64KernelStackPageSizeBytes - 1U)) == 0U;
}

[[nodiscard]] constexpr bool IsCanonicalAddress(const Foundation::UInt64 address) noexcept
{
    return (address < LowerCanonicalExclusiveEnd) || (address >= UpperCanonicalStart);
}

[[nodiscard]] constexpr bool TryGetExclusiveEnd(const Foundation::UInt64 start, const Foundation::UInt64 size,
                                                Foundation::UInt64& end) noexcept
{
    if ((size == 0U) || (start > (MaximumUInt64 - size)))
    {
        return false;
    }
    end = start + size;
    return true;
}

[[nodiscard]] constexpr bool RangesOverlap(const Foundation::UInt64 firstStart, const Foundation::UInt64 firstEnd,
                                           const Foundation::UInt64 secondStart,
                                           const Foundation::UInt64 secondEnd) noexcept
{
    return (firstStart < secondEnd) && (secondStart < firstEnd);
}
} // namespace

Amd64KernelContextError Amd64KernelContext::Prepare(const WritableAmd64KernelStackView& stack,
                                                    const Amd64KernelContextEntry entry, void* const argument) noexcept
{
    if (m_state != Amd64KernelContextState::Fresh)
    {
        return Amd64KernelContextError::AlreadyPrepared;
    }
    if (entry == nullptr)
    {
        return Amd64KernelContextError::InvalidEntry;
    }

    const Foundation::UInt64 entryAddress =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(entry));
    if (!IsCanonicalAddress(entryAddress))
    {
        return Amd64KernelContextError::InvalidEntry;
    }

    const Foundation::UInt64 dataStart =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(stack.Data));
    Foundation::UInt64 dataEnd{0U};
    Foundation::UInt64 virtualEnd{0U};
    if ((stack.Data == nullptr) || (stack.SizeBytes < MinimumAmd64KernelStackSizeBytes) ||
        (stack.SizeBytes > MaximumAmd64KernelStackSizeBytes) || !IsPageAligned(dataStart) ||
        !IsPageAligned(stack.VirtualStart) || !IsPageAligned(stack.SizeBytes) ||
        !TryGetExclusiveEnd(dataStart, stack.SizeBytes, dataEnd) ||
        !TryGetExclusiveEnd(stack.VirtualStart, stack.SizeBytes, virtualEnd) ||
        !IsCanonicalAddress(stack.VirtualStart) || !IsCanonicalAddress(virtualEnd - 1U))
    {
        return Amd64KernelContextError::InvalidStack;
    }

    const Foundation::UInt64 contextStart =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(this));
    Foundation::UInt64 contextEnd{0U};
    if (!TryGetExclusiveEnd(contextStart, sizeof(*this), contextEnd) ||
        RangesOverlap(dataStart, dataEnd, contextStart, contextEnd) ||
        RangesOverlap(stack.VirtualStart, virtualEnd, contextStart, contextEnd))
    {
        return Amd64KernelContextError::InvalidStack;
    }

    const Foundation::UInt64 argumentAddress =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(argument));
    const Foundation::UInt64 startAddress =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(&AsteriaAmd64StartKernelContext));

    m_registers = Amd64KernelRegisterState{
        .Rbx = 0U,
        .Rbp = 0U,
        .R12 = entryAddress,
        .R13 = argumentAddress,
        .R14 = 0U,
        .R15 = 0U,
        .Rsp = virtualEnd,
        .Rip = startAddress,
    };
    m_stackVirtualStart = stack.VirtualStart;
    m_stackSizeBytes = stack.SizeBytes;
    m_entryAddress = entryAddress;
    m_argumentAddress = argumentAddress;
    m_state = Amd64KernelContextState::Prepared;
    return Amd64KernelContextError::None;
}

Amd64KernelContextError Amd64KernelContext::SwitchTo(Amd64KernelContext& next) noexcept
{
    if (&next == this)
    {
        return Amd64KernelContextError::SameContext;
    }
    if (next.m_state != Amd64KernelContextState::Prepared)
    {
        return Amd64KernelContextError::ContextNotPrepared;
    }

    m_state = Amd64KernelContextState::Prepared;
    // The kernel touches no extended state between here and the switch, so
    // next's state is live when next resumes.
    if (m_extendedState != nullptr)
    {
        Amd64SaveExtendedState(*m_extendedState);
    }
    if (next.m_extendedState != nullptr)
    {
        Amd64RestoreExtendedState(*next.m_extendedState);
    }
    AsteriaAmd64SwitchKernelContext(&m_registers, &next.m_registers);
    return Amd64KernelContextError::None;
}

void Amd64KernelContext::AttachExtendedState(Amd64ExtendedStateArea& area) noexcept
{
    InitializeAmd64ExtendedState(area);
    m_extendedState = &area;
}

void InitializeAmd64ExtendedState(Amd64ExtendedStateArea& area) noexcept
{
    for (Foundation::UInt8& byte : area.Bytes)
    {
        byte = 0U;
    }
    area.Bytes[Amd64ExtendedStateControlWordOffset] = static_cast<Foundation::UInt8>(Amd64InitialFpuControlWord);
    area.Bytes[Amd64ExtendedStateControlWordOffset + 1U] =
        static_cast<Foundation::UInt8>(Amd64InitialFpuControlWord >> 8U);
    for (Foundation::Size index{0U}; index < 4U; ++index)
    {
        area.Bytes[Amd64ExtendedStateMxcsrOffset + index] =
            static_cast<Foundation::UInt8>(Amd64InitialMxcsr >> (8U * index));
    }
}

[[gnu::noinline]] void Amd64SaveExtendedState(Amd64ExtendedStateArea& area) noexcept
{
    __asm__ __volatile__("fxsave64 %0" : "=m"(area) : : "memory");
}

[[gnu::noinline]] void Amd64RestoreExtendedState(const Amd64ExtendedStateArea& area) noexcept
{
    __asm__ __volatile__("fxrstor64 %0" : : "m"(area) : "memory");
}
Amd64KernelContextError Amd64KernelContext::ConfigureExternalFrameIdleContinuation(
    const WritableAmd64KernelStackView& stack, const Amd64KernelContextEntry entry, void* const argument) noexcept
{
    if (m_externalConfigured)
    {
        return Amd64KernelContextError::AlreadyPrepared;
    }
    Amd64KernelContext prepared;
    const auto result = prepared.Prepare(stack, entry, argument);
    if (result != Amd64KernelContextError::None)
    {
        return result;
    }
    const auto storage = reinterpret_cast<Foundation::UIntPointer>(this);
    const auto data = reinterpret_cast<Foundation::UIntPointer>(stack.Data);
    if (RangesOverlap(data, data + stack.SizeBytes, storage, storage + sizeof(*this)) ||
        RangesOverlap(stack.VirtualStart, stack.VirtualStart + stack.SizeBytes, storage, storage + sizeof(*this)) ||
        (m_stackSizeBytes != 0U && RangesOverlap(stack.VirtualStart, stack.VirtualStart + stack.SizeBytes,
                                                 m_stackVirtualStart, m_stackVirtualStart + m_stackSizeBytes)))
    {
        return Amd64KernelContextError::InvalidStack;
    }
    m_externalRegisters = prepared.m_registers;
    m_externalStackStart = stack.VirtualStart;
    m_externalStackSize = stack.SizeBytes;
    m_externalConfigured = true;
    return Amd64KernelContextError::None;
}

bool Amd64KernelContext::SaveExternalFrameIdleContinuation(const Foundation::UInt64 generation) noexcept
{
    if (!m_externalConfigured || !IsPrepared() || generation == 0U || generation <= m_externalGeneration.LoadAcquire())
    {
        return false;
    }
    m_registers = m_externalRegisters;
    m_stackVirtualStart = m_externalStackStart;
    m_stackSizeBytes = m_externalStackSize;
    m_entryAddress = m_externalRegisters.R12;
    m_argumentAddress = m_externalRegisters.R13;
    m_externalGeneration.StoreRelease(generation);
    return true;
}
} // namespace Asteria::Platform::Amd64::Scheduler
