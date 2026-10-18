// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_SCHEDULER_AMD64_KERNEL_CONTEXT_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_SCHEDULER_AMD64_KERNEL_CONTEXT_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"

namespace Asteria::Platform::Amd64::Scheduler
{
constexpr Foundation::UInt64 Amd64KernelStackPageSizeBytes{4096U};
constexpr Foundation::UInt64 MinimumAmd64KernelStackSizeBytes{Amd64KernelStackPageSizeBytes};
constexpr Foundation::UInt64 MaximumAmd64KernelStackSizeBytes{64U * 1024U};

enum class Amd64KernelContextState : Foundation::UInt8
{
    Fresh,
    Prepared,
};

enum class Amd64KernelContextError : Foundation::UInt8
{
    None,
    AlreadyPrepared,
    InvalidStack,
    InvalidEntry,
    ContextNotPrepared,
    SameContext,
};

using Amd64KernelContextEntry = void (*)(void*) noexcept;

struct WritableAmd64KernelStackView
{
    Foundation::UInt8* Data;
    Foundation::UInt64 SizeBytes;
    Foundation::UInt64 VirtualStart;
};

/// Register subset preserved by the AMD64 System V function-call ABI.
/// Caller-saved registers are already dead at the SwitchTo call boundary.
struct Amd64KernelRegisterState
{
    Foundation::UInt64 Rbx;
    Foundation::UInt64 Rbp;
    Foundation::UInt64 R12;
    Foundation::UInt64 R13;
    Foundation::UInt64 R14;
    Foundation::UInt64 R15;
    Foundation::UInt64 Rsp;
    Foundation::UInt64 Rip;
};

/// The FXSAVE image of one thread's x87, MMX and SSE state.
struct alignas(16) Amd64ExtendedStateArea
{
    Foundation::UInt8 Bytes[512];
};

/// Byte offsets of the FXSAVE fields a new thread's state sets.
constexpr Foundation::Size Amd64ExtendedStateControlWordOffset{0U};
constexpr Foundation::Size Amd64ExtendedStateMxcsrOffset{24U};
/// X87: every exception masked, extended precision, round to nearest.
constexpr Foundation::UInt16 Amd64InitialFpuControlWord{0x037FU};
/// SSE: every exception masked, round to nearest, no flush to zero.
constexpr Foundation::UInt32 Amd64InitialMxcsr{0x1F80U};

/// Fills area with the state a new thread starts with: the initial x87
/// control word and MXCSR, an empty x87 tag word, every register zero.
void InitializeAmd64ExtendedState(Amd64ExtendedStateArea& area) noexcept;

/// FXSAVE64 into area and FXRSTOR64 from it: the only kernel instructions
/// that touch extended state (Verify Amd64KernelGeneralRegistersOnly).
void Amd64SaveExtendedState(Amd64ExtendedStateArea& area) noexcept;
void Amd64RestoreExtendedState(const Amd64ExtendedStateArea& area) noexcept;

struct Amd64KernelContextSnapshot
{
    Foundation::UInt64 StackVirtualStart;
    Foundation::UInt64 StackSizeBytes;
    Foundation::UInt64 EntryAddress;
    Foundation::UInt64 ArgumentAddress;
    Amd64KernelContextState State;
    Foundation::UInt8 Reserved[7];
};

/// Owns one cooperative CPL0 execution context on an already selected root.
/// This primitive switches the ABI-preserved general-register subset and the kernel stack, and, for
/// a context with an extended-state area, the thread's x87, MMX and SSE state: SwitchTo saves the
/// current context's state into its area and loads next's from its area before it switches. A
/// context without an area leaves the registers as they are, which is safe because the kernel never
/// uses them. The caller disables local interrupts, serializes both contexts to one processor, and
/// activates the required address space before SwitchTo. Debug, user-return, and address-space
/// state belong to other owners.
class Amd64KernelContext final
{
public:
    constexpr Amd64KernelContext() noexcept = default;

    ~Amd64KernelContext() noexcept = default;

    Amd64KernelContext(const Amd64KernelContext&) = delete;
    Amd64KernelContext& operator=(const Amd64KernelContext&) = delete;
    Amd64KernelContext(Amd64KernelContext&&) = delete;
    Amd64KernelContext& operator=(Amd64KernelContext&&) = delete;

    /// Prepares a fresh context to call entry(argument) on stack. Entry must not return. Failure
    /// changes no context or stack byte. Preconditions: - stack describes an exclusively owned,
    /// writable, mapped kernel stack whose mapping remains valid until this context is retired. -
    /// entry and argument remain valid for the context lifetime.
    [[nodiscard]] Amd64KernelContextError Prepare(const WritableAmd64KernelStackView& stack,
                                                  Amd64KernelContextEntry entry, void* argument) noexcept;

    /// Saves this call frame and resumes next. On success this invocation returns only after
    /// another switch later resumes this exact context. Preconditions: - CPL0, local interrupts
    /// disabled, and no same-CPU re-entry. - this is the context currently executing this call;
    /// next is suspended and is not active on any processor. - Both context objects and their
    /// distinct stacks are alive and exclusively owned. - The caller has already selected a root
    /// mapping next's code and stack.
    [[nodiscard]] Amd64KernelContextError SwitchTo(Amd64KernelContext& next) noexcept;

    /// Gives this context the extended-state area of the thread it runs and
    /// fills the area with a new thread's state. Before the first switch to
    /// this context; area outlives it.
    void AttachExtendedState(Amd64ExtendedStateArea& area) noexcept;

    /// Configures a fresh idle landing before external capture is armed. Its
    /// stack must be owned, mapped, and disjoint from the active worker and
    /// coordinator stacks. The old suspended idle continuation is abandoned
    /// when SaveExternalFrameIdleContinuation publishes a new generation.
    [[nodiscard]] Amd64KernelContextError
    ConfigureExternalFrameIdleContinuation(const WritableAmd64KernelStackView& stack, Amd64KernelContextEntry entry,
                                           void* argument) noexcept;
    [[nodiscard]] bool SaveExternalFrameIdleContinuation(Foundation::UInt64 generation) noexcept;
    [[nodiscard]] Foundation::UInt64 GetExternalFrameIdleContinuationGeneration() const noexcept
    {
        return m_externalGeneration.LoadAcquire();
    }

    [[nodiscard]] constexpr bool IsPrepared() const noexcept
    {
        return m_state == Amd64KernelContextState::Prepared;
    }

    [[nodiscard]] constexpr Amd64KernelContextSnapshot GetSnapshot() const noexcept
    {
        return Amd64KernelContextSnapshot{
            .StackVirtualStart = m_stackVirtualStart,
            .StackSizeBytes = m_stackSizeBytes,
            .EntryAddress = m_entryAddress,
            .ArgumentAddress = m_argumentAddress,
            .State = m_state,
            .Reserved = {},
        };
    }

private:
    Amd64KernelRegisterState m_registers{};
    Foundation::UInt64 m_stackVirtualStart{0U};
    Foundation::UInt64 m_stackSizeBytes{0U};
    Foundation::UInt64 m_entryAddress{0U};
    Foundation::UInt64 m_argumentAddress{0U};
    Amd64KernelContextState m_state{Amd64KernelContextState::Fresh};
    [[maybe_unused]] Foundation::UInt8 m_reserved[7]{};
    Amd64KernelRegisterState m_externalRegisters{};
    Foundation::UInt64 m_externalStackStart{0U};
    Foundation::UInt64 m_externalStackSize{0U};
    Synchronization::AtomicUInt64 m_externalGeneration;
    bool m_externalConfigured{false};
    [[maybe_unused]] Foundation::UInt8 m_externalReserved[7U]{};
    Amd64ExtendedStateArea* m_extendedState{nullptr};
};

static_assert(sizeof(Amd64KernelContextState) == 1U);
static_assert(sizeof(Amd64KernelContextError) == 1U);
static_assert(sizeof(WritableAmd64KernelStackView) == 24U);
static_assert(alignof(WritableAmd64KernelStackView) == 8U);
static_assert(sizeof(Amd64KernelRegisterState) == 64U);
static_assert(alignof(Amd64KernelRegisterState) == 8U);
static_assert(__builtin_offsetof(Amd64KernelRegisterState, Rbx) == 0U);
static_assert(__builtin_offsetof(Amd64KernelRegisterState, Rbp) == 8U);
static_assert(__builtin_offsetof(Amd64KernelRegisterState, R12) == 16U);
static_assert(__builtin_offsetof(Amd64KernelRegisterState, R13) == 24U);
static_assert(__builtin_offsetof(Amd64KernelRegisterState, R14) == 32U);
static_assert(__builtin_offsetof(Amd64KernelRegisterState, R15) == 40U);
static_assert(__builtin_offsetof(Amd64KernelRegisterState, Rsp) == 48U);
static_assert(__builtin_offsetof(Amd64KernelRegisterState, Rip) == 56U);
static_assert(sizeof(Amd64KernelContextSnapshot) == 40U);
static_assert(alignof(Amd64KernelContextSnapshot) == 8U);
static_assert(sizeof(Amd64ExtendedStateArea) == 512U);
static_assert(alignof(Amd64ExtendedStateArea) == 16U);
static_assert(sizeof(Amd64KernelContext) == 208U);
static_assert(alignof(Amd64KernelContext) == 8U);
} // Namespace Asteria::Platform::Amd64::Scheduler

#endif // ASTERIA_PLATFORM_AMD64_SCHEDULER_AMD64_KERNEL_CONTEXT_HPP_INCLUDED
