// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_TLB_SHOOTDOWN_DISPATCHER_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_TLB_SHOOTDOWN_DISPATCHER_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Platform/Synchronization/ProcessorContext.hpp"
#include "Hal/Memory/ILocalTlbInvalidator.hpp"
#include "Hal/Memory/ITlbShootdownTransport.hpp"
#include "Kernel/Memory/AddressSpace.hpp"

namespace Asteria::Kernel::Memory
{
class TlbShootdownWork final
{
public:
    constexpr TlbShootdownWork() noexcept = default;
    constexpr TlbShootdownWork(const TlbShootdownWork&) noexcept = default;
    constexpr TlbShootdownWork& operator=(const TlbShootdownWork&) noexcept = default;

    [[nodiscard]] constexpr AddressSpace* GetOwner() const noexcept
    {
        return m_owner;
    }

    [[nodiscard]] constexpr AddressSpaceMappingToken GetToken() const noexcept
    {
        return m_token;
    }

    [[nodiscard]] constexpr VirtualAddress GetInvalidationStart() const noexcept
    {
        return m_invalidationStart;
    }

    [[nodiscard]] constexpr PageCount GetInvalidationPages() const noexcept
    {
        return m_invalidationPages;
    }

    [[nodiscard]] constexpr Scheduler::CpuId GetTargetProcessor() const noexcept
    {
        return m_targetProcessor;
    }

private:
    friend class TlbShootdownDispatcher;

    constexpr TlbShootdownWork(AddressSpace& owner, const AddressSpaceMappingToken token,
                               const VirtualAddress invalidationStart, const PageCount invalidationPages,
                               const Scheduler::CpuId targetProcessor) noexcept
        : m_owner{&owner}, m_token{token}, m_invalidationStart{invalidationStart},
          m_invalidationPages{invalidationPages}, m_targetProcessor{targetProcessor}
    {
    }

    AddressSpace* m_owner{nullptr};
    AddressSpaceMappingToken m_token{};
    VirtualAddress m_invalidationStart{};
    PageCount m_invalidationPages{};
    Scheduler::CpuId m_targetProcessor{};
    [[maybe_unused]] Foundation::UInt8 m_reserved[7]{};
};

class TlbShootdownBatch final
{
public:
    constexpr TlbShootdownBatch() noexcept = default;
    constexpr TlbShootdownBatch(const TlbShootdownBatch&) noexcept = default;
    constexpr TlbShootdownBatch& operator=(const TlbShootdownBatch&) noexcept = default;

    [[nodiscard]] constexpr Foundation::UInt8 GetCount() const noexcept
    {
        return m_count;
    }

    /// Preconditions: index is less than GetCount.
    [[nodiscard]] constexpr const TlbShootdownWork& GetWork(const Foundation::Size index) const noexcept
    {
        return m_work[index];
    }

private:
    friend class TlbShootdownDispatcher;

    TlbShootdownWork m_work[Scheduler::MaximumProcessorCount]{};
    Foundation::UInt8 m_count{0U};
    [[maybe_unused]] Foundation::UInt8 m_reserved[7]{};
};

enum class TlbShootdownError : Foundation::UInt8
{
    None,
    InvalidRequest,
    LocalInvalidationRejected,
    TransportRejected,
    AcknowledgementRejected,
};

struct TlbShootdownReceipt
{
    AddressSpaceMappingToken Token;
    Foundation::ProcessorMask QueuedProcessorMask;
    Foundation::ProcessorMask AcknowledgedProcessorMask;
    Scheduler::CpuId Processor;
    bool RejectionObserved;
    Foundation::UInt8 Reserved[4];
};

struct TlbShootdownFailure
{
    TlbShootdownError Error;
    Hal::Memory::LocalTlbInvalidationError LocalError;
    Hal::Memory::TlbShootdownTransportError TransportError;
    AddressSpaceError AddressSpaceErrorCode;
    Scheduler::CpuId Processor;
    Foundation::UInt8 Reserved[3];
};

/// Connects one AddressSpace shootdown generation to local invalidation and a bounded remote-work
/// transport without retaining transaction state. One instance is permanently bound to one CPU and
/// that CPU's local invalidator before publication. It is invoked only on that CPU with migration
/// and re-entry disabled. Dispatch runs in serialized thread context. HandleRemote is ISR-safe when
/// the bound local invalidator is ISR-safe: it takes no lock, allocates no memory, performs one
/// bounded invalidation, and updates only the AddressSpace's lock-free shootdown word. A remote
/// failure publishes an atomic rejection marker; the initiating thread observes it through
/// CompleteMapping and performs the terminal state transition. The serialized caller invokes
/// Dispatch exactly once for each opaque request. Every queued or already received TlbShootdownWork
/// is non-owning. The caller keeps its AddressSpace and token-owning transaction alive until every
/// remote HandleRemote call has returned. Removing a work item from transport storage is not
/// completion and does not release that lifetime obligation.
class TlbShootdownDispatcher final
{
public:
    explicit constexpr TlbShootdownDispatcher(const Scheduler::CpuId processor,
                                              Hal::Memory::ILocalTlbInvalidator& localInvalidator) noexcept
        : m_localInvalidator{&localInvalidator}, m_processor{processor}
    {
    }

    ~TlbShootdownDispatcher() noexcept = default;

    TlbShootdownDispatcher(const TlbShootdownDispatcher&) = delete;
    TlbShootdownDispatcher& operator=(const TlbShootdownDispatcher&) = delete;
    TlbShootdownDispatcher(TlbShootdownDispatcher&&) = delete;
    TlbShootdownDispatcher& operator=(TlbShootdownDispatcher&&) = delete;

    [[nodiscard]] constexpr Scheduler::CpuId GetProcessor() const noexcept
    {
        return m_currentProcessor ? Platform::Synchronization::CurrentProcessor() : m_processor;
    }

    // Mapper/channel facades dispatch on the caller.
    void UseCurrentProcessor() noexcept
    {
        m_currentProcessor = true;
    }
    [[nodiscard]] Foundation::Result<TlbShootdownReceipt, TlbShootdownFailure>
    Dispatch(AddressSpace& addressSpace, const AddressSpaceShootdownRequest& request,
             Hal::Memory::ITlbShootdownTransport& transport) noexcept;

    [[nodiscard]] Foundation::Result<TlbShootdownReceipt, TlbShootdownFailure>
    HandleRemote(const TlbShootdownWork& work) noexcept;

private:
    Hal::Memory::ILocalTlbInvalidator* m_localInvalidator;
    Scheduler::CpuId m_processor;
    bool m_currentProcessor{false};
    [[maybe_unused]] Foundation::UInt8 m_reserved[7]{};
};

static_assert(sizeof(TlbShootdownWork) == 48U);
static_assert(alignof(TlbShootdownWork) == 8U);
static_assert(sizeof(TlbShootdownBatch) == 200U);
static_assert(alignof(TlbShootdownBatch) == 8U);
static_assert(sizeof(TlbShootdownError) == 1U);
static_assert(sizeof(TlbShootdownReceipt) == 40U);
static_assert(alignof(TlbShootdownReceipt) == 8U);
static_assert(sizeof(TlbShootdownFailure) == 8U);
static_assert(alignof(TlbShootdownFailure) == 1U);
static_assert(sizeof(TlbShootdownDispatcher) == 24U);
static_assert(alignof(TlbShootdownDispatcher) == 8U);
static_assert(sizeof(Foundation::Result<TlbShootdownReceipt, TlbShootdownFailure>) == 56U);
static_assert(alignof(Foundation::Result<TlbShootdownReceipt, TlbShootdownFailure>) == 8U);
} // Namespace Asteria::Kernel::Memory

#endif // ASTERIA_KERNEL_MEMORY_TLB_SHOOTDOWN_DISPATCHER_HPP_INCLUDED
