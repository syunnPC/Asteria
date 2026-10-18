// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Scheduler/Ia64KernelContext.hpp"

#include "Platform/Synchronization/AtomicUInt64.hpp"

extern "C" void AsteriaIa64StartKernelContext() noexcept;

extern "C" Asteria::Foundation::UInt64 AsteriaIa64GetStartKernelContextAddress() noexcept;

extern "C" void
AsteriaIa64SwitchKernelContext(Asteria::Platform::Ia64::Scheduler::Ia64KernelRegisterState* current,
                               const Asteria::Platform::Ia64::Scheduler::Ia64KernelRegisterState* next) noexcept;

extern "C" void
AsteriaIa64SaveHighFloatingPartition(Asteria::Platform::Ia64::Scheduler::Ia64HighFloatingArea* area) noexcept;

extern "C" void
AsteriaIa64RestoreHighFloatingPartition(const Asteria::Platform::Ia64::Scheduler::Ia64HighFloatingArea* area) noexcept;

// The target entry points are in Ia64KernelContext.S.
#if !defined(__ia64__)
extern "C" void AsteriaIa64StartKernelContext() noexcept
{
}

extern "C" Asteria::Foundation::UInt64 AsteriaIa64GetStartKernelContextAddress() noexcept
{
    return static_cast<Asteria::Foundation::UInt64>(
        reinterpret_cast<Asteria::Foundation::UIntPointer>(&AsteriaIa64StartKernelContext));
}
#endif

namespace Asteria::Platform::Ia64::Scheduler
{
Ia64SynchronousContinuationSwitchError SwitchIa64SynchronousContinuation(Ia64KernelRegisterState& current,
                                                                         const Ia64KernelRegisterState& next) noexcept
{
    if (&current == &next)
    {
        return Ia64SynchronousContinuationSwitchError::SameRegisterState;
    }

    AsteriaIa64SwitchKernelContext(&current, &next);
    return Ia64SynchronousContinuationSwitchError::None;
}

namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt8 MinimumImplementedVirtualAddressBit{50U};
constexpr Foundation::UInt8 MaximumImplementedVirtualAddressBit{60U};

struct Ia64FunctionDescriptor
{
    Foundation::UInt64 EntryPoint;
    Foundation::UInt64 GlobalPointer;
};

static_assert(sizeof(Ia64FunctionDescriptor) == 16U);
static_assert(alignof(Ia64FunctionDescriptor) == 8U);

[[nodiscard]] constexpr bool IsFunctionDescriptorAligned(const Foundation::UInt64 address) noexcept
{
    return (address & (alignof(Ia64FunctionDescriptor) - 1U)) == 0U;
}

static_assert(IsFunctionDescriptorAligned(8U));
static_assert(!IsFunctionDescriptorAligned(4U));

struct ValidatedStorage
{
    Foundation::UInt64 DataStart;
    Foundation::UInt64 DataEnd;
    Foundation::UInt64 VirtualStart;
    Foundation::UInt64 VirtualEnd;
};

[[nodiscard]] constexpr bool IsPageAligned(const Foundation::UInt64 value) noexcept
{
    return (value & (Ia64KernelStoragePageSizeBytes - 1U)) == 0U;
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

[[nodiscard]] constexpr bool IsVirtualAddressImplemented(const Foundation::UInt64 address,
                                                         const Foundation::UInt8 implementedMostSignificantBit) noexcept
{
    if (implementedMostSignificantBit == MaximumImplementedVirtualAddressBit)
    {
        return true;
    }
    const Foundation::UInt8 unimplementedBitCount =
        static_cast<Foundation::UInt8>(MaximumImplementedVirtualAddressBit - implementedMostSignificantBit);
    const Foundation::UInt64 mask = (Foundation::UInt64{1U} << unimplementedBitCount) - 1U;
    const Foundation::UInt64 unimplementedBits = (address >> (implementedMostSignificantBit + 1U)) & mask;
    const Foundation::UInt64 signExtension =
        ((address & (Foundation::UInt64{1U} << implementedMostSignificantBit)) != 0U) ? mask : 0U;
    return unimplementedBits == signExtension;
}

[[nodiscard]] constexpr bool IsImplementedRange(const Foundation::UInt64 start, const Foundation::UInt64 end,
                                                const Foundation::UInt8 implementedMostSignificantBit) noexcept
{
    return (start < end) && ((start >> 61U) == ((end - 1U) >> 61U)) &&
           IsVirtualAddressImplemented(start, implementedMostSignificantBit) &&
           IsVirtualAddressImplemented(end - 1U, implementedMostSignificantBit);
}

[[nodiscard]] constexpr bool RangesOverlap(const Foundation::UInt64 firstStart, const Foundation::UInt64 firstEnd,
                                           const Foundation::UInt64 secondStart,
                                           const Foundation::UInt64 secondEnd) noexcept
{
    return (firstStart < secondEnd) && (secondStart < firstEnd);
}

[[nodiscard]] bool ValidateStorage(const WritableIa64KernelStorageView& storage, const Foundation::UInt64 minimumSize,
                                   const Foundation::UInt64 maximumSize,
                                   const Foundation::UInt8 implementedMostSignificantBit,
                                   ValidatedStorage& validated) noexcept
{
    if ((storage.Data == nullptr) || (storage.SizeBytes < minimumSize) || (storage.SizeBytes > maximumSize) ||
        !IsPageAligned(storage.SizeBytes))
    {
        return false;
    }

    const Foundation::UInt64 dataStart =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(storage.Data));
    Foundation::UInt64 dataEnd{0U};
    Foundation::UInt64 virtualEnd{0U};
    if (!IsPageAligned(dataStart) || !IsPageAligned(storage.VirtualStart) ||
        !TryGetExclusiveEnd(dataStart, storage.SizeBytes, dataEnd) ||
        !TryGetExclusiveEnd(storage.VirtualStart, storage.SizeBytes, virtualEnd) ||
        !IsImplementedRange(dataStart, dataEnd, implementedMostSignificantBit) ||
        !IsImplementedRange(storage.VirtualStart, virtualEnd, implementedMostSignificantBit))
    {
        return false;
    }

    validated = ValidatedStorage{
        .DataStart = dataStart,
        .DataEnd = dataEnd,
        .VirtualStart = storage.VirtualStart,
        .VirtualEnd = virtualEnd,
    };
    return true;
}

[[nodiscard]] bool ResolveFunctionDescriptor(const Foundation::UInt64 descriptorAddress,
                                             const Foundation::UInt8 implementedMostSignificantBit,
                                             Foundation::UInt64& entryPoint, Foundation::UInt64& globalPointer) noexcept
{
    if (descriptorAddress == 0U)
    {
        return false;
    }

#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
    Foundation::UInt64 descriptorEnd{0U};
    if (!IsFunctionDescriptorAligned(descriptorAddress) ||
        !TryGetExclusiveEnd(descriptorAddress, sizeof(Ia64FunctionDescriptor), descriptorEnd) ||
        !IsImplementedRange(descriptorAddress, descriptorEnd, implementedMostSignificantBit))
    {
        return false;
    }

    const auto* const descriptor = reinterpret_cast<const Ia64FunctionDescriptor*>(descriptorAddress);
    entryPoint = descriptor->EntryPoint;
    globalPointer = descriptor->GlobalPointer;
#else
    entryPoint = descriptorAddress;
    globalPointer = 0U;
#endif

    return (entryPoint != 0U)
#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
           && ((entryPoint & 15U) == 0U)
#endif
           && IsVirtualAddressImplemented(entryPoint, implementedMostSignificantBit) &&
           ((globalPointer == 0U) || IsVirtualAddressImplemented(globalPointer, implementedMostSignificantBit));
}

/// A cooperative switch may leave or resume a thread whose page-fault frame
/// is Inactive, or Retained: the user-fault bridge blocks the thread with
/// its fault retained and resumes it there. Capturing and ResumeCommitted
/// belong to the entry assembly, which never switches, and any other value
/// is not a frame state.
[[nodiscard]] bool PageFaultFrameBlocksSwitch(const Interrupt::Ia64PageFaultFrame& frame) noexcept
{
    switch (__atomic_load_n(&frame.State, __ATOMIC_ACQUIRE))
    {
    case Interrupt::Ia64PageFaultFrameState::Inactive:
    case Interrupt::Ia64PageFaultFrameState::Retained:
        return false;
    case Interrupt::Ia64PageFaultFrameState::Capturing:
    case Interrupt::Ia64PageFaultFrameState::ResumeCommitted:
        return true;
    }
    return true;
}

} // namespace

Ia64PageFaultFrameBindingError
Ia64KernelContext::BindPageFaultFrame(Interrupt::Ia64PageFaultFrame* const pageFaultFrame) noexcept
{
    if (m_pageFaultFrame != &m_embeddedPageFaultFrame)
    {
        return Ia64PageFaultFrameBindingError::AlreadyBound;
    }
    if (m_state != Ia64KernelContextState::Fresh)
    {
        return Ia64PageFaultFrameBindingError::ContextAlreadyPrepared;
    }
    if (pageFaultFrame == nullptr)
    {
        return Ia64PageFaultFrameBindingError::InvalidFrame;
    }

    const Foundation::UInt64 frameStart =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(pageFaultFrame));
    if ((frameStart & (alignof(Interrupt::Ia64PageFaultFrame) - 1U)) != 0U)
    {
        return Ia64PageFaultFrameBindingError::InvalidFrameAlignment;
    }
    Foundation::UInt64 frameEnd{0U};
    if (!TryGetExclusiveEnd(frameStart, sizeof(Interrupt::Ia64PageFaultFrame), frameEnd))
    {
        return Ia64PageFaultFrameBindingError::InvalidFrame;
    }

    const Foundation::UInt64 contextStart =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(this));
    Foundation::UInt64 contextEnd{0U};
    if (!TryGetExclusiveEnd(contextStart, sizeof(*this), contextEnd) ||
        RangesOverlap(contextStart, contextEnd, frameStart, frameEnd))
    {
        return Ia64PageFaultFrameBindingError::FrameAliasesContext;
    }
    if (Interrupt::Ia64PageFaultFrameIsOwned(m_embeddedPageFaultFrame))
    {
        return Ia64PageFaultFrameBindingError::EmbeddedFrameOwned;
    }
    if (!Interrupt::Ia64PageFaultFrameIsPristine(m_embeddedPageFaultFrame))
    {
        return Ia64PageFaultFrameBindingError::EmbeddedFrameNotPristine;
    }
    if (Interrupt::Ia64PageFaultFrameIsOwned(*pageFaultFrame))
    {
        return Ia64PageFaultFrameBindingError::ExternalFrameOwned;
    }
    if (!Interrupt::Ia64PageFaultFrameIsPristine(*pageFaultFrame))
    {
        return Ia64PageFaultFrameBindingError::ExternalFrameNotPristine;
    }

    m_pageFaultFrame = pageFaultFrame;
    return Ia64PageFaultFrameBindingError::None;
}

Ia64KernelContextError Ia64KernelContext::Prepare(const Foundation::UInt8 implementedVirtualAddressMostSignificantBit,
                                                  const WritableIa64KernelStorageView& memoryStack,
                                                  const WritableIa64KernelStorageView& rseBackingStore,
                                                  const Ia64KernelContextEntry entry, void* const argument) noexcept
{
    if (m_state != Ia64KernelContextState::Fresh)
    {
        return Ia64KernelContextError::AlreadyPrepared;
    }
    if ((implementedVirtualAddressMostSignificantBit < MinimumImplementedVirtualAddressBit) ||
        (implementedVirtualAddressMostSignificantBit > MaximumImplementedVirtualAddressBit))
    {
        return Ia64KernelContextError::InvalidAddressProfile;
    }

    ValidatedStorage validatedMemoryStack{};
    if (!ValidateStorage(memoryStack, MinimumIa64KernelMemoryStackSizeBytes, MaximumIa64KernelMemoryStackSizeBytes,
                         implementedVirtualAddressMostSignificantBit, validatedMemoryStack))
    {
        return Ia64KernelContextError::InvalidMemoryStack;
    }

    ValidatedStorage validatedRseBackingStore{};
    if (!ValidateStorage(rseBackingStore, MinimumIa64KernelRseBackingStoreSizeBytes,
                         MaximumIa64KernelRseBackingStoreSizeBytes, implementedVirtualAddressMostSignificantBit,
                         validatedRseBackingStore))
    {
        return Ia64KernelContextError::InvalidRseBackingStore;
    }

    if (RangesOverlap(validatedMemoryStack.DataStart, validatedMemoryStack.DataEnd, validatedRseBackingStore.DataStart,
                      validatedRseBackingStore.DataEnd) ||
        RangesOverlap(validatedMemoryStack.VirtualStart, validatedMemoryStack.VirtualEnd,
                      validatedRseBackingStore.VirtualStart, validatedRseBackingStore.VirtualEnd) ||
        RangesOverlap(validatedMemoryStack.DataStart, validatedMemoryStack.DataEnd,
                      validatedRseBackingStore.VirtualStart, validatedRseBackingStore.VirtualEnd) ||
        RangesOverlap(validatedMemoryStack.VirtualStart, validatedMemoryStack.VirtualEnd,
                      validatedRseBackingStore.DataStart, validatedRseBackingStore.DataEnd))
    {
        return Ia64KernelContextError::StorageRangesOverlap;
    }

    const Foundation::UInt64 contextStart =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(this));
    Foundation::UInt64 contextEnd{0U};
    if (!TryGetExclusiveEnd(contextStart, sizeof(*this), contextEnd) ||
        RangesOverlap(contextStart, contextEnd, validatedMemoryStack.DataStart, validatedMemoryStack.DataEnd) ||
        RangesOverlap(contextStart, contextEnd, validatedMemoryStack.VirtualStart, validatedMemoryStack.VirtualEnd) ||
        RangesOverlap(contextStart, contextEnd, validatedRseBackingStore.DataStart, validatedRseBackingStore.DataEnd) ||
        RangesOverlap(contextStart, contextEnd, validatedRseBackingStore.VirtualStart,
                      validatedRseBackingStore.VirtualEnd))
    {
        return Ia64KernelContextError::StorageRangesOverlap;
    }

    const Foundation::UInt64 entryDescriptorAddress =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(entry));
    Foundation::UInt64 entryPoint{0U};
    Foundation::UInt64 entryGlobalPointer{0U};
    if ((entry == nullptr) ||
        !ResolveFunctionDescriptor(entryDescriptorAddress, implementedVirtualAddressMostSignificantBit, entryPoint,
                                   entryGlobalPointer))
    {
        return Ia64KernelContextError::InvalidEntry;
    }

    const Foundation::UInt64 startEntryPoint = AsteriaIa64GetStartKernelContextAddress();
    if ((startEntryPoint == 0U)
#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
        || ((startEntryPoint & 15U) != 0U)
#endif
        || !IsVirtualAddressImplemented(startEntryPoint, implementedVirtualAddressMostSignificantBit))
    {
        return Ia64KernelContextError::InvalidEntry;
    }

    const Foundation::UInt64 argumentAddress =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(argument));
    // All fallible validation is complete. Publish directly into the retained
    // image so Prepare does not place the 496-byte register file on the
    // bounded kernel stack.
    m_registers = {};
    m_registers.R4 = entryPoint;
    m_registers.R5 = entryGlobalPointer;
    m_registers.R6 = argumentAddress;
    m_registers.GlobalPointer = 0U;
    m_registers.StackPointer = validatedMemoryStack.VirtualEnd - Ia64KernelInitialMemoryStackScratchBytes;
    m_registers.InstructionPointer = startEntryPoint;
    m_registers.RseBackingStorePointer = validatedRseBackingStore.VirtualStart;
    m_registers.FloatingPointStatus = Ia64KernelDefaultFloatingPointStatus;

    m_memoryStackVirtualStart = validatedMemoryStack.VirtualStart;
    m_memoryStackSizeBytes = memoryStack.SizeBytes;
    m_rseBackingStoreVirtualStart = validatedRseBackingStore.VirtualStart;
    m_rseBackingStoreSizeBytes = rseBackingStore.SizeBytes;
    m_entryDescriptorAddress = entryDescriptorAddress;
    m_entryPointAddress = entryPoint;
    m_argumentAddress = argumentAddress;
    m_implementedVirtualAddressMostSignificantBit = implementedVirtualAddressMostSignificantBit;
    m_state = Ia64KernelContextState::Prepared;
    return Ia64KernelContextError::None;
}

Ia64KernelContextError Ia64KernelContext::SwitchTo(Ia64KernelContext& next) noexcept
{
    if (&next == this)
    {
        return Ia64KernelContextError::SameContext;
    }
    if (Interrupt::Ia64ExternalPreemptionFrameIsOwned(m_externalPreemptionFrame) ||
        Interrupt::Ia64ExternalPreemptionFrameIsOwned(next.m_externalPreemptionFrame))
    {
        return Ia64KernelContextError::ExternalPreemptionFrameOwned;
    }
    if (PageFaultFrameBlocksSwitch(*m_pageFaultFrame) || PageFaultFrameBlocksSwitch(*next.m_pageFaultFrame))
    {
        return Ia64KernelContextError::PageFaultFrameOwned;
    }
    if (next.m_state != Ia64KernelContextState::Prepared)
    {
        return Ia64KernelContextError::ContextNotPrepared;
    }

    m_state = Ia64KernelContextState::Prepared;
#if defined(ASTERIA_TARGET_IA64)
    if (m_highFloating != nullptr)
    {
        AsteriaIa64SaveHighFloatingPartition(m_highFloating);
    }
    if (next.m_highFloating != nullptr)
    {
        AsteriaIa64RestoreHighFloatingPartition(next.m_highFloating);
    }
#endif
    static_cast<void>(SwitchIa64SynchronousContinuation(m_registers, next.m_registers));
    return Ia64KernelContextError::None;
}

bool Ia64KernelContext::AttachHighFloatingArea(Ia64HighFloatingArea& area) noexcept
{
    if ((m_highFloating != nullptr) || (m_state != Ia64KernelContextState::Fresh))
    {
        return false;
    }
    m_highFloating = &area;
    return true;
}

Ia64ExternalFrameIdleContinuationError Ia64KernelContext::ConfigureExternalFrameIdleContinuation(
    const Foundation::UInt8 implementedVirtualAddressMostSignificantBit,
    const WritableIa64KernelStorageView& memoryStack, const WritableIa64KernelStorageView& rseBackingStore,
    const Ia64KernelContextEntry entry, void* const argument) noexcept
{
    if (m_externalIdleConfigured)
    {
        return Ia64ExternalFrameIdleContinuationError::AlreadyConfigured;
    }
    if ((implementedVirtualAddressMostSignificantBit < MinimumImplementedVirtualAddressBit) ||
        (implementedVirtualAddressMostSignificantBit > MaximumImplementedVirtualAddressBit))
    {
        return Ia64ExternalFrameIdleContinuationError::InvalidAddressProfile;
    }

    const Foundation::UInt64 descriptorAddress =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(entry));
    Foundation::UInt64 entryPoint{0U};
    Foundation::UInt64 globalPointer{0U};
    if ((entry == nullptr) || !ResolveFunctionDescriptor(descriptorAddress, implementedVirtualAddressMostSignificantBit,
                                                         entryPoint, globalPointer))
    {
        return Ia64ExternalFrameIdleContinuationError::InvalidEntry;
    }

    return ConfigureExternalFrameIdleContinuation(implementedVirtualAddressMostSignificantBit, memoryStack,
                                                  rseBackingStore,
                                                  Ia64KernelContextResolvedEntry{
                                                      .DescriptorAddress = descriptorAddress,
                                                      .EntryPointAddress = entryPoint,
                                                      .GlobalPointer = globalPointer,
                                                  },
                                                  argument);
}

Ia64ExternalFrameIdleContinuationError Ia64KernelContext::ConfigureExternalFrameIdleContinuation(
    const Foundation::UInt8 implementedVirtualAddressMostSignificantBit,
    const WritableIa64KernelStorageView& memoryStack, const WritableIa64KernelStorageView& rseBackingStore,
    const Ia64KernelContextResolvedEntry& entry, void* const argument) noexcept
{
    if (m_externalIdleConfigured)
    {
        return Ia64ExternalFrameIdleContinuationError::AlreadyConfigured;
    }
    if ((implementedVirtualAddressMostSignificantBit < MinimumImplementedVirtualAddressBit) ||
        (implementedVirtualAddressMostSignificantBit > MaximumImplementedVirtualAddressBit))
    {
        return Ia64ExternalFrameIdleContinuationError::InvalidAddressProfile;
    }

    ValidatedStorage validatedMemoryStack{};
    if (!ValidateStorage(memoryStack, MinimumIa64KernelMemoryStackSizeBytes, MaximumIa64KernelMemoryStackSizeBytes,
                         implementedVirtualAddressMostSignificantBit, validatedMemoryStack))
    {
        return Ia64ExternalFrameIdleContinuationError::InvalidMemoryStack;
    }
    ValidatedStorage validatedRseBackingStore{};
    if (!ValidateStorage(rseBackingStore, MinimumIa64KernelRseBackingStoreSizeBytes,
                         MaximumIa64KernelRseBackingStoreSizeBytes, implementedVirtualAddressMostSignificantBit,
                         validatedRseBackingStore))
    {
        return Ia64ExternalFrameIdleContinuationError::InvalidRseBackingStore;
    }
    if (RangesOverlap(validatedMemoryStack.DataStart, validatedMemoryStack.DataEnd, validatedRseBackingStore.DataStart,
                      validatedRseBackingStore.DataEnd) ||
        RangesOverlap(validatedMemoryStack.VirtualStart, validatedMemoryStack.VirtualEnd,
                      validatedRseBackingStore.VirtualStart, validatedRseBackingStore.VirtualEnd) ||
        RangesOverlap(validatedMemoryStack.DataStart, validatedMemoryStack.DataEnd,
                      validatedRseBackingStore.VirtualStart, validatedRseBackingStore.VirtualEnd) ||
        RangesOverlap(validatedMemoryStack.VirtualStart, validatedMemoryStack.VirtualEnd,
                      validatedRseBackingStore.DataStart, validatedRseBackingStore.DataEnd))
    {
        return Ia64ExternalFrameIdleContinuationError::StorageRangesOverlap;
    }
    const Foundation::UInt64 contextStart =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(this));
    Foundation::UInt64 contextEnd{0U};
    if (!TryGetExclusiveEnd(contextStart, sizeof(*this), contextEnd) ||
        RangesOverlap(contextStart, contextEnd, validatedMemoryStack.DataStart, validatedMemoryStack.DataEnd) ||
        RangesOverlap(contextStart, contextEnd, validatedMemoryStack.VirtualStart, validatedMemoryStack.VirtualEnd) ||
        RangesOverlap(contextStart, contextEnd, validatedRseBackingStore.DataStart, validatedRseBackingStore.DataEnd) ||
        RangesOverlap(contextStart, contextEnd, validatedRseBackingStore.VirtualStart,
                      validatedRseBackingStore.VirtualEnd))
    {
        return Ia64ExternalFrameIdleContinuationError::StorageRangesOverlap;
    }

    Foundation::UInt64 descriptorEnd{0U};
    if ((entry.DescriptorAddress == 0U) ||
        !TryGetExclusiveEnd(entry.DescriptorAddress, sizeof(Ia64FunctionDescriptor), descriptorEnd) ||
        !IsImplementedRange(entry.DescriptorAddress, descriptorEnd, implementedVirtualAddressMostSignificantBit) ||
        (entry.EntryPointAddress == 0U)
#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
        || !IsFunctionDescriptorAligned(entry.DescriptorAddress) || ((entry.EntryPointAddress & 15U) != 0U)
#endif
        || !IsVirtualAddressImplemented(entry.EntryPointAddress, implementedVirtualAddressMostSignificantBit) ||
        ((entry.GlobalPointer != 0U) &&
         !IsVirtualAddressImplemented(entry.GlobalPointer, implementedVirtualAddressMostSignificantBit)))
    {
        return Ia64ExternalFrameIdleContinuationError::InvalidEntry;
    }
    const Foundation::UInt64 startEntryPoint = AsteriaIa64GetStartKernelContextAddress();
    if ((startEntryPoint == 0U)
#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
        || ((startEntryPoint & 15U) != 0U)
#endif
        || !IsVirtualAddressImplemented(startEntryPoint, implementedVirtualAddressMostSignificantBit))
    {
        return Ia64ExternalFrameIdleContinuationError::InvalidEntry;
    }

    m_externalIdleMemoryStackVirtualStart = validatedMemoryStack.VirtualStart;
    m_externalIdleMemoryStackSizeBytes = memoryStack.SizeBytes;
    m_externalIdleRseBackingStoreVirtualStart = validatedRseBackingStore.VirtualStart;
    m_externalIdleRseBackingStoreSizeBytes = rseBackingStore.SizeBytes;
    m_externalIdleEntryDescriptorAddress = entry.DescriptorAddress;
    m_externalIdleEntryPointAddress = entry.EntryPointAddress;
    m_externalIdleEntryGlobalPointer = entry.GlobalPointer;
    m_externalIdleArgumentAddress =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(argument));
    m_externalIdleConfigured = true;
    return Ia64ExternalFrameIdleContinuationError::None;
}

bool Ia64KernelContext::SaveExternalFrameIdleContinuation(const Foundation::UInt64 generation) noexcept
{
    const Foundation::UInt64 publishedGeneration = GetExternalFrameIdleContinuationGeneration();
    if (!m_externalIdleConfigured || (m_state != Ia64KernelContextState::Prepared) || (generation == 0U) ||
        (generation <= publishedGeneration) ||
        Interrupt::Ia64ExternalPreemptionFrameIsOwned(m_externalPreemptionFrame) ||
        Interrupt::Ia64PageFaultFrameIsOwned(*m_pageFaultFrame))
    {
        return false;
    }

    m_registers = {};
    m_registers.R4 = m_externalIdleEntryPointAddress;
    m_registers.R5 = m_externalIdleEntryGlobalPointer;
    m_registers.R6 = m_externalIdleArgumentAddress;
    m_registers.StackPointer = m_externalIdleMemoryStackVirtualStart + m_externalIdleMemoryStackSizeBytes -
                               Ia64KernelInitialMemoryStackScratchBytes;
    m_registers.InstructionPointer = AsteriaIa64GetStartKernelContextAddress();
    m_registers.RseBackingStorePointer = m_externalIdleRseBackingStoreVirtualStart;
    m_registers.FloatingPointStatus = Ia64KernelDefaultFloatingPointStatus;
    Platform::Synchronization::AtomicUInt64Reference publication{m_externalIdlePublishedGeneration};
    publication.StoreRelease(generation);
    return true;
}

Foundation::UInt64 Ia64KernelContext::GetExternalFrameIdleContinuationGeneration() const noexcept
{
    return Platform::Synchronization::AtomicUInt64Reference::LoadAcquire(m_externalIdlePublishedGeneration);
}
} // namespace Asteria::Platform::Ia64::Scheduler
