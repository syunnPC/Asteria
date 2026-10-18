// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_SCHEDULER_IA64_KERNEL_CONTEXT_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_SCHEDULER_IA64_KERNEL_CONTEXT_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Platform/Ia64/Interrupt/Ia64ExternalPreemptionFrame.hpp"
#include "Platform/Ia64/Interrupt/Ia64PageFaultEntryContract.hpp"

namespace Asteria::Platform::Ia64::Scheduler
{
constexpr Foundation::UInt64 Ia64KernelStoragePageSizeBytes{4096U};
constexpr Foundation::UInt64 MinimumIa64KernelMemoryStackSizeBytes{16U * 1024U};
constexpr Foundation::UInt64 MaximumIa64KernelMemoryStackSizeBytes{64U * 1024U};
constexpr Foundation::UInt64 MinimumIa64KernelRseBackingStoreSizeBytes{16U * 1024U};
constexpr Foundation::UInt64 MaximumIa64KernelRseBackingStoreSizeBytes{64U * 1024U};
constexpr Foundation::UInt64 Ia64KernelInitialMemoryStackScratchBytes{16U};
constexpr Foundation::UInt64 Ia64KernelDefaultFloatingPointStatus{0x0009'804C'0270'033FULL};

enum class Ia64KernelContextState : Foundation::UInt8
{
    Fresh,
    Prepared,
};

enum class Ia64KernelContextError : Foundation::UInt8
{
    None,
    AlreadyPrepared,
    InvalidAddressProfile,
    InvalidMemoryStack,
    InvalidRseBackingStore,
    StorageRangesOverlap,
    InvalidEntry,
    ContextNotPrepared,
    SameContext,
    ExternalPreemptionFrameOwned,
    PageFaultFrameOwned,
};

enum class Ia64PageFaultFrameBindingError : Foundation::UInt8
{
    None,
    AlreadyBound,
    ContextAlreadyPrepared,
    InvalidFrame,
    InvalidFrameAlignment,
    FrameAliasesContext,
    EmbeddedFrameOwned,
    EmbeddedFrameNotPristine,
    ExternalFrameOwned,
    ExternalFrameNotPristine,
};

enum class Ia64SynchronousContinuationSwitchError : Foundation::UInt8
{
    None,
    SameRegisterState,
};

enum class Ia64ExternalFrameIdleContinuationError : Foundation::UInt8
{
    None,
    AlreadyConfigured,
    InvalidAddressProfile,
    InvalidMemoryStack,
    InvalidRseBackingStore,
    StorageRangesOverlap,
    InvalidEntry,
};

using Ia64KernelContextEntry = void (*)(void*) noexcept;

/// Exact P64 descriptor words retained by a fresh idle landing. The explicit
/// form lets architecture fixtures prove nonzero gp transport without asking a
/// hosted compiler to emulate IA-64 function-pointer representation.
struct Ia64KernelContextResolvedEntry
{
    Foundation::UInt64 DescriptorAddress;
    Foundation::UInt64 EntryPointAddress;
    Foundation::UInt64 GlobalPointer;
};

struct WritableIa64KernelStorageView
{
    Foundation::UInt8* Data;
    Foundation::UInt64 SizeBytes;
    Foundation::UInt64 VirtualStart;
};

struct alignas(16) Ia64FloatingRegisterState
{
    Foundation::UInt64 Low;
    Foundation::UInt64 High;
};

/// Synchronous IA-64 calling-convention state. FloatingRegisters contains
/// f2-f5 followed by f16-f31. Scratch register and translation state belongs
/// to the interrupt/user-return and address-space owners rather than this
/// cooperative call-boundary primitive.
struct alignas(16) Ia64KernelRegisterState
{
    Foundation::UInt64 R4;
    Foundation::UInt64 R5;
    Foundation::UInt64 R6;
    Foundation::UInt64 R7;
    Foundation::UInt64 GlobalPointer;
    Foundation::UInt64 StackPointer;
    Foundation::UInt64 ThreadPointer;
    Foundation::UInt64 InstructionPointer;
    Foundation::UInt64 PreviousFunctionState;
    Foundation::UInt64 RseConfiguration;
    Foundation::UInt64 RseBackingStorePointer;
    Foundation::UInt64 RseNatCollection;
    Foundation::UInt64 UserNatCollection;
    Foundation::UInt64 PreservedGeneralNatCollection;
    Foundation::UInt64 LoopCounter;
    Foundation::UInt64 FloatingPointStatus;
    Foundation::UInt64 Predicates;
    Foundation::UInt64 B1;
    Foundation::UInt64 B2;
    Foundation::UInt64 B3;
    Foundation::UInt64 B4;
    Foundation::UInt64 B5;
    Ia64FloatingRegisterState FloatingRegisters[20];
};

/// One thread's high floating-point partition f32-f127, which user threads own and the kernel never
/// Uses, in spill format.
struct alignas(16) Ia64HighFloatingArea
{
    Ia64FloatingRegisterState Registers[96];
};

/// Saves the current synchronous P64 call boundary into current and restores next. On target, a
/// successful invocation returns only after another switch later restores current. Hosted tests
/// provide the raw adapter and may return immediately so higher-level continuation state machines
/// can exercise their unavailable-transition path. Preconditions: - CPL0, PSR.ic is one, PSR.i is
/// zero, and same-CPU re-entry is excluded; - current describes the executing call boundary and
/// next is suspended; - both distinct register images remain alive and mapped throughout use; - the
/// caller already selected translations for next's code and storage.
[[nodiscard]] Ia64SynchronousContinuationSwitchError
SwitchIa64SynchronousContinuation(Ia64KernelRegisterState& current, const Ia64KernelRegisterState& next) noexcept;

struct Ia64KernelContextSnapshot
{
    Foundation::UInt64 MemoryStackVirtualStart;
    Foundation::UInt64 MemoryStackSizeBytes;
    Foundation::UInt64 RseBackingStoreVirtualStart;
    Foundation::UInt64 RseBackingStoreSizeBytes;
    Foundation::UInt64 EntryDescriptorAddress;
    Foundation::UInt64 EntryPointAddress;
    Foundation::UInt64 ArgumentAddress;
    Ia64KernelContextState State;
    Foundation::UInt8 ImplementedVirtualAddressMostSignificantBit;
    Foundation::UInt8 Reserved[6];
};

struct Ia64ExternalFrameIdleContinuationSnapshot
{
    Foundation::UInt64 MemoryStackVirtualStart;
    Foundation::UInt64 MemoryStackSizeBytes;
    Foundation::UInt64 RseBackingStoreVirtualStart;
    Foundation::UInt64 RseBackingStoreSizeBytes;
    Foundation::UInt64 EntryDescriptorAddress;
    Foundation::UInt64 EntryPointAddress;
    Foundation::UInt64 EntryGlobalPointer;
    Foundation::UInt64 ArgumentAddress;
    Foundation::UInt64 PublishedGeneration;
    Foundation::UInt8 Configured;
    Foundation::UInt8 Reserved[7U];
};

/// Owns one cooperative CPL0 IA-64 execution context on an already selected translation regime. The
/// memory stack and RSE backing store are independent owned ranges that remain mapped for the
/// complete context lifetime. SwitchTo is a synchronous call boundary. It preserves the IA-64 P64
/// callee-saved general, floating-point, predicate, branch, application, NaT, and RSE state. The
/// separately owned ExternalPreemptionFrame is a complete persistent asynchronous image; the
/// embedded or permanently bound PageFaultFrame is the corresponding complete synchronous PL3 fault
/// image. Neither is interpreted as cooperative state. Region/translation registers, unrelated
/// user-return policy, debug state, and performance state are owned by platform runtime code.
class Ia64KernelContext final
{
public:
    constexpr Ia64KernelContext() noexcept = default;

    ~Ia64KernelContext() noexcept = default;

    Ia64KernelContext(const Ia64KernelContext&) = delete;
    Ia64KernelContext& operator=(const Ia64KernelContext&) = delete;
    Ia64KernelContext(Ia64KernelContext&&) = delete;
    Ia64KernelContext& operator=(Ia64KernelContext&&) = delete;

    /// Selects one externally owned page-fault frame before this context can enter or become
    /// Prepared. Failure changes neither frame nor the selection; success is permanent and copies
    /// no frame state. Preconditions before invocation: - any non-null, correctly aligned
    /// pageFaultFrame points to an accessible Ia64PageFaultFrame object. Preconditions after
    /// success: - the external owner keeps pageFaultFrame alive, stable, and mapped for this
    /// context's complete lifetime; - the external owner serializes binding, capture, resume,
    /// switching, and retirement access to this context and pageFaultFrame.
    [[nodiscard]] Ia64PageFaultFrameBindingError
    BindPageFaultFrame(Interrupt::Ia64PageFaultFrame* pageFaultFrame) noexcept;

    /// Prepares a fresh context to call entry(argument). Entry must not return. Failure changes no
    /// context or storage byte. Preconditions: - both storage views are exclusively owned,
    /// writable, mapped in the supplied IA-64 virtual-address profile, and remain stable until
    /// context retirement; - entry's function descriptor, code, global data, and argument remain
    /// valid for the context lifetime.
    [[nodiscard]] Ia64KernelContextError Prepare(Foundation::UInt8 implementedVirtualAddressMostSignificantBit,
                                                 const WritableIa64KernelStorageView& memoryStack,
                                                 const WritableIa64KernelStorageView& rseBackingStore,
                                                 Ia64KernelContextEntry entry, void* argument) noexcept;

    /// Saves this call frame and resumes next. On success this invocation returns only after
    /// another switch later resumes this exact context. It fails with PageFaultFrameOwned while
    /// either page-fault frame is in a state the entry assembly owns (Capturing or
    /// ResumeCommitted); a frame Retained by a thread blocked in the user-fault bridge does not
    /// block. Preconditions: - CPL0, PSR.ic is one, PSR.i is zero, and no same-CPU re-entry; - this
    /// is the context currently executing this call; next is suspended and is not active on another
    /// processor; - both objects and their mutually distinct storage remain alive; - the caller
    /// already selected translations for next's code and storage.
    [[nodiscard]] Ia64KernelContextError SwitchTo(Ia64KernelContext& next) noexcept;

    /// Gives this context a thread's f32-f127 area: SwitchTo spills the high
    /// partition into it when this context switches away and fills it from
    /// there when this context resumes, so a thread blocked in the kernel
    /// keeps its values while another runs. A context without an area (idle,
    /// kernel workers) leaves the registers alone. Once, on a Fresh context;
    /// false otherwise. The area is not the page-fault frame's, whose capture
    /// requires the frame pristine.
    [[nodiscard]] bool AttachHighFloatingArea(Ia64HighFloatingArea& area) noexcept;

    /// Retains immutable storage and a no-return landing entry used only when
    /// common scheduling selects an architecture frame. This does not alter
    /// the cooperative image or make the context Prepared. The coordinator
    /// stack/RBS are one processor-owned corridor, not per-thread storage.
    [[nodiscard]] Ia64ExternalFrameIdleContinuationError ConfigureExternalFrameIdleContinuation(
        Foundation::UInt8 implementedVirtualAddressMostSignificantBit, const WritableIa64KernelStorageView& memoryStack,
        const WritableIa64KernelStorageView& rseBackingStore, Ia64KernelContextEntry entry, void* argument) noexcept;

    /// Configures the same landing from already resolved P64 descriptor
    /// words. The caller must retain the descriptor/code/global-data lifetime;
    /// this method revalidates every numeric address before publication.
    [[nodiscard]] Ia64ExternalFrameIdleContinuationError
    ConfigureExternalFrameIdleContinuation(Foundation::UInt8 implementedVirtualAddressMostSignificantBit,
                                           const WritableIa64KernelStorageView& memoryStack,
                                           const WritableIa64KernelStorageView& rseBackingStore,
                                           const Ia64KernelContextResolvedEntry& entry, void* argument) noexcept;

    /// Exact persistent target frame owned by this context. Native entry and
    /// restore code may access it only with interrupts disabled under the
    /// external-frame lifecycle. It contains no coordinator stack or RBS.
    [[nodiscard]] constexpr Interrupt::Ia64ExternalPreemptionFrame& GetExternalPreemptionFrame() noexcept
    {
        return m_externalPreemptionFrame;
    }

    [[nodiscard]] constexpr const Interrupt::Ia64ExternalPreemptionFrame& GetExternalPreemptionFrame() const noexcept
    {
        return m_externalPreemptionFrame;
    }

    /// Exact persistent synchronous page-fault image selected for this
    /// context. This is the private embedded frame by default or the exact
    /// permanently bound external frame. Native entry may write it only under
    /// the page-fault capture lifecycle. The common scheduler sees only its
    /// published opaque token; it never imports this architecture-specific
    /// register representation.
    [[nodiscard]] constexpr Interrupt::Ia64PageFaultFrame& GetPageFaultFrame() noexcept
    {
        return *m_pageFaultFrame;
    }

    [[nodiscard]] constexpr const Interrupt::Ia64PageFaultFrame& GetPageFaultFrame() const noexcept
    {
        return *m_pageFaultFrame;
    }

    /// Target hook required by retained external-frame resume. Success
    /// replaces the suspended cooperative image with a fresh zero-frame
    /// trampoline into the configured no-return idle-dispatch landing. The
    /// target-owned generation is release-published only after the complete
    /// register image is installed. Restoring it cannot re-enter the caller's
    /// Refresh/Commit path.
    [[nodiscard]] bool SaveExternalFrameIdleContinuation(Foundation::UInt64 generation) noexcept;

    [[nodiscard]] Foundation::UInt64 GetExternalFrameIdleContinuationGeneration() const noexcept;

    [[nodiscard]] Ia64ExternalFrameIdleContinuationSnapshot GetExternalFrameIdleContinuationSnapshot() const noexcept
    {
        return Ia64ExternalFrameIdleContinuationSnapshot{
            .MemoryStackVirtualStart = m_externalIdleMemoryStackVirtualStart,
            .MemoryStackSizeBytes = m_externalIdleMemoryStackSizeBytes,
            .RseBackingStoreVirtualStart = m_externalIdleRseBackingStoreVirtualStart,
            .RseBackingStoreSizeBytes = m_externalIdleRseBackingStoreSizeBytes,
            .EntryDescriptorAddress = m_externalIdleEntryDescriptorAddress,
            .EntryPointAddress = m_externalIdleEntryPointAddress,
            .EntryGlobalPointer = m_externalIdleEntryGlobalPointer,
            .ArgumentAddress = m_externalIdleArgumentAddress,
            .PublishedGeneration = GetExternalFrameIdleContinuationGeneration(),
            .Configured = static_cast<Foundation::UInt8>(m_externalIdleConfigured ? 1U : 0U),
            .Reserved = {},
        };
    }

    [[nodiscard]] constexpr bool IsPrepared() const noexcept
    {
        return m_state == Ia64KernelContextState::Prepared;
    }

    [[nodiscard]] constexpr Ia64KernelContextSnapshot GetSnapshot() const noexcept
    {
        return Ia64KernelContextSnapshot{
            .MemoryStackVirtualStart = m_memoryStackVirtualStart,
            .MemoryStackSizeBytes = m_memoryStackSizeBytes,
            .RseBackingStoreVirtualStart = m_rseBackingStoreVirtualStart,
            .RseBackingStoreSizeBytes = m_rseBackingStoreSizeBytes,
            .EntryDescriptorAddress = m_entryDescriptorAddress,
            .EntryPointAddress = m_entryPointAddress,
            .ArgumentAddress = m_argumentAddress,
            .State = m_state,
            .ImplementedVirtualAddressMostSignificantBit = m_implementedVirtualAddressMostSignificantBit,
            .Reserved = {},
        };
    }

private:
    Interrupt::Ia64ExternalPreemptionFrame m_externalPreemptionFrame{};
    Interrupt::Ia64PageFaultFrame m_embeddedPageFaultFrame{};
    Ia64KernelRegisterState m_registers{};
    Foundation::UInt64 m_memoryStackVirtualStart{0U};
    Foundation::UInt64 m_memoryStackSizeBytes{0U};
    Foundation::UInt64 m_rseBackingStoreVirtualStart{0U};
    Foundation::UInt64 m_rseBackingStoreSizeBytes{0U};
    Foundation::UInt64 m_entryDescriptorAddress{0U};
    Foundation::UInt64 m_entryPointAddress{0U};
    Foundation::UInt64 m_argumentAddress{0U};
    Foundation::UInt64 m_externalIdleMemoryStackVirtualStart{0U};
    Foundation::UInt64 m_externalIdleMemoryStackSizeBytes{0U};
    Foundation::UInt64 m_externalIdleRseBackingStoreVirtualStart{0U};
    Foundation::UInt64 m_externalIdleRseBackingStoreSizeBytes{0U};
    Foundation::UInt64 m_externalIdleEntryDescriptorAddress{0U};
    Foundation::UInt64 m_externalIdleEntryPointAddress{0U};
    Foundation::UInt64 m_externalIdleEntryGlobalPointer{0U};
    Foundation::UInt64 m_externalIdleArgumentAddress{0U};
    volatile Foundation::UInt64 m_externalIdlePublishedGeneration{0U};
    Ia64KernelContextState m_state{Ia64KernelContextState::Fresh};
    Foundation::UInt8 m_implementedVirtualAddressMostSignificantBit{0U};
    bool m_externalIdleConfigured{false};
    [[maybe_unused]] Foundation::UInt8 m_reserved[5]{};
    Interrupt::Ia64PageFaultFrame* m_pageFaultFrame{&m_embeddedPageFaultFrame};
    Ia64HighFloatingArea* m_highFloating{nullptr};
};

static_assert(sizeof(Ia64KernelContextState) == 1U);
static_assert(sizeof(Ia64KernelContextError) == 1U);
static_assert(sizeof(Ia64PageFaultFrameBindingError) == 1U);
static_assert(sizeof(Ia64SynchronousContinuationSwitchError) == 1U);
static_assert(sizeof(Ia64ExternalFrameIdleContinuationError) == 1U);
static_assert(sizeof(Ia64KernelContextResolvedEntry) == 24U);
static_assert(alignof(Ia64KernelContextResolvedEntry) == 8U);
static_assert(sizeof(WritableIa64KernelStorageView) == 24U);
static_assert(alignof(WritableIa64KernelStorageView) == 8U);
static_assert(sizeof(Ia64FloatingRegisterState) == 16U);
static_assert(alignof(Ia64FloatingRegisterState) == 16U);
static_assert(sizeof(Ia64KernelRegisterState) == 496U);
static_assert(alignof(Ia64KernelRegisterState) == 16U);
static_assert(__builtin_offsetof(Ia64KernelRegisterState, R4) == 0U);
static_assert(__builtin_offsetof(Ia64KernelRegisterState, R5) == 8U);
static_assert(__builtin_offsetof(Ia64KernelRegisterState, R6) == 16U);
static_assert(__builtin_offsetof(Ia64KernelRegisterState, R7) == 24U);
static_assert(__builtin_offsetof(Ia64KernelRegisterState, GlobalPointer) == 32U);
static_assert(__builtin_offsetof(Ia64KernelRegisterState, StackPointer) == 40U);
static_assert(__builtin_offsetof(Ia64KernelRegisterState, ThreadPointer) == 48U);
static_assert(__builtin_offsetof(Ia64KernelRegisterState, InstructionPointer) == 56U);
static_assert(__builtin_offsetof(Ia64KernelRegisterState, PreviousFunctionState) == 64U);
static_assert(__builtin_offsetof(Ia64KernelRegisterState, RseConfiguration) == 72U);
static_assert(__builtin_offsetof(Ia64KernelRegisterState, RseBackingStorePointer) == 80U);
static_assert(__builtin_offsetof(Ia64KernelRegisterState, RseNatCollection) == 88U);
static_assert(__builtin_offsetof(Ia64KernelRegisterState, UserNatCollection) == 96U);
static_assert(__builtin_offsetof(Ia64KernelRegisterState, PreservedGeneralNatCollection) == 104U);
static_assert(__builtin_offsetof(Ia64KernelRegisterState, LoopCounter) == 112U);
static_assert(__builtin_offsetof(Ia64KernelRegisterState, FloatingPointStatus) == 120U);
static_assert(__builtin_offsetof(Ia64KernelRegisterState, Predicates) == 128U);
static_assert(__builtin_offsetof(Ia64KernelRegisterState, B1) == 136U);
static_assert(__builtin_offsetof(Ia64KernelRegisterState, B2) == 144U);
static_assert(__builtin_offsetof(Ia64KernelRegisterState, B3) == 152U);
static_assert(__builtin_offsetof(Ia64KernelRegisterState, B4) == 160U);
static_assert(__builtin_offsetof(Ia64KernelRegisterState, B5) == 168U);
static_assert(__builtin_offsetof(Ia64KernelRegisterState, FloatingRegisters) == 176U);
static_assert(sizeof(Ia64KernelContextSnapshot) == 64U);
static_assert(alignof(Ia64KernelContextSnapshot) == 8U);
static_assert(sizeof(Ia64ExternalFrameIdleContinuationSnapshot) == 80U);
static_assert(alignof(Ia64ExternalFrameIdleContinuationSnapshot) == 8U);
static_assert(__builtin_offsetof(Ia64ExternalFrameIdleContinuationSnapshot, EntryDescriptorAddress) == 32U);
static_assert(__builtin_offsetof(Ia64ExternalFrameIdleContinuationSnapshot, EntryPointAddress) == 40U);
static_assert(__builtin_offsetof(Ia64ExternalFrameIdleContinuationSnapshot, EntryGlobalPointer) == 48U);
static_assert(__builtin_offsetof(Ia64ExternalFrameIdleContinuationSnapshot, PublishedGeneration) == 64U);


static_assert(sizeof(Ia64KernelContext) == 6080U);
static_assert(sizeof(Ia64HighFloatingArea) == 1536U);
static_assert(alignof(Ia64KernelContext) == 64U);
} // Namespace Asteria::Platform::Ia64::Scheduler

#endif // ASTERIA_PLATFORM_IA64_SCHEDULER_IA64_KERNEL_CONTEXT_HPP_INCLUDED
