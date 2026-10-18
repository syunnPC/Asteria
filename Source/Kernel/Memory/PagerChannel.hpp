// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_PAGER_CHANNEL_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_PAGER_CHANNEL_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Hal/Memory/ITlbShootdownTransport.hpp"
#include "Kernel/Ipc/Endpoint.hpp"
#include "Kernel/Ipc/Notification.hpp"
#include "Kernel/Memory/FaultResolutionDispatcher.hpp"
#include "Kernel/Memory/FaultChannelTransition.hpp"
#include "Kernel/Memory/FaultTokenFactory.hpp"
#include "Kernel/Memory/IPagerResolution.hpp"
#include "Kernel/Memory/IPagerShootdownProgress.hpp"
#include "Kernel/Memory/PageFaultIngress.hpp"
#include "Kernel/Memory/PagerFaultDispatcher.hpp"
#include "Kernel/Memory/TlbShootdownDispatcher.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"

namespace Asteria::Kernel::Memory
{
enum class PagerChannelError : Foundation::UInt8
{
    None,
    InvalidConfiguration,
    AlreadyConfigured,
    NotConfigured,
    Busy,
    IngressRejected,
    Faulted,
};

struct PagerChannelConfiguration
{
    /// The trusted kernel space that receives each new FaultToken capability
    /// before delivery moves it to the pager. It shares the pager Endpoint's
    /// registry and lock.
    Object::CapabilitySpace* TokenSpace;
    /// Signalled after every delivery; the pager waits on it in its WaitSet.
    Ipc::Notification* Readiness;
    /// Optional; without it a resolution that leaves remote invalidation
    /// pending faults the channel.
    IPagerShootdownProgress* ShootdownProgress;
    Object::ResourceDomainId ChargeOwner;
    Object::ResourceDomainId Sender;
    Object::RevocationDomainId RevocationDomain;
    Object::ObjectId ChannelIdentity{};
};

/// What a trap path knows about one user fault of the running thread.
struct PagerFaultSource
{
    Scheduler::ThreadId Thread;
    AddressSpaceId AddressSpace;
    Scheduler::SchedulingContextId SchedulingContext;
    FaultRecord Fault;
    /// The faulting domain's supervision tag, which the delivery carries; zero for a boot domain.
    Foundation::UInt64 SupervisionTag;
    // Registry slot and generation, assigned by the kernel.
    Foundation::UInt64 ProtectionDomainIdentity{0U};
};

struct PagerChannelSnapshot
{
    PageFaultIngressSnapshot Ingress;
    Foundation::UInt64 Deliveries;
    Foundation::UInt64 Resolutions;
    /// Of the resolutions, those that terminated the faulting thread.
    Foundation::UInt64 Terminations;
    Foundation::UInt64 Busy;
    FaultResolutionDispatcherError Resolution;
    PagerChannelState State;
    PagerChannelError Error;
    Foundation::UInt8 Reserved[5];
};

/// Joins one external pager to the faults of the address spaces it serves: one fault at a time,
/// from capture to the faulting thread's wake. PrepareFault correlates the running faulting thread.
/// The trap path then completes the run as Blocked through ProcessorKernelRunLoop:
/// FinishFaultAndDispatch with this channel as the ingress; CommitBlocked creates the FaultToken,
/// delivers the pager message and signals the readiness Notification. Resolve is the capability
/// layer of the pager's FaultResolve call: it proves from the caller's own space that the caller
/// holds the delivered token (Invoke) and the faulting AddressSpace, MemoryObject and Frame (each
/// Map), lets FaultResolutionDispatcher map and wake the thread, then retires the token and returns
/// to Idle. A Terminate proves only the token and completes the pager transaction by exiting the
/// thread with Abi::FaultTerminatedExitCode; the thread's binding and SchedulingContext stay as
/// they are until teardown. The channel owns no lock. CAS claims serialize transaction fields
/// across processors; losers return without waiting. Calls run with local interrupts disabled,
/// outside an ISR and without same-CPU reentry; the composed objects lock themselves in the
/// documented order.
class PagerChannel final : public IPagerResolution
{
public:
    /// Preconditions: every referenced object shares one registry, outlives
    /// this channel and stays immovable; the pager Endpoint's receiver space
    /// is the pager's capability space.
    PagerChannel(Object::ObjectRegistry& registry, Object::RootCapabilityIssuer& issuer, Ipc::Endpoint& pagerEndpoint,
                 Scheduler::ProcessorSchedulerWakeFacade& wakeFacade, TlbShootdownDispatcher& shootdown,
                 Hal::Memory::ITlbShootdownTransport& transport) noexcept;
    ~PagerChannel() noexcept = default;

    PagerChannel(const PagerChannel&) = delete;
    PagerChannel& operator=(const PagerChannel&) = delete;
    PagerChannel(PagerChannel&&) = delete;
    PagerChannel& operator=(PagerChannel&&) = delete;

    [[nodiscard]] PagerChannelError Configure(const PagerChannelConfiguration& configuration) noexcept;

    /// Correlates the running faulting thread. Busy while an earlier fault
    /// awaits its resolution; every failure leaves the thread untouched.
    [[nodiscard]] PagerChannelError PrepareFault(const PagerFaultSource& source) noexcept;

    /// ProcessorKernelRunLoop::FinishFaultAndDispatch's ingress hook: called once the prepared
    /// thread is Blocked, before another context runs.
    void CommitBlocked(Scheduler::ThreadBlockToken blockToken) noexcept;

    /// Collects retired tokens, including while Idle, and retries a delivery
    /// that token-pool or Endpoint pressure deferred.
    void Retry() noexcept;

    /// True only while Idle with every retired token physically reclaimed.
    /// A storage owner must check this before destroying the channel.
    [[nodiscard]] bool IsReclaimable() noexcept;

    [[nodiscard]] PagerResolutionResult Resolve(Object::CapabilitySpace& caller,
                                                const PagerResolveRequest& request) noexcept override;

    [[nodiscard]] PagerChannelSnapshot GetSnapshot() const noexcept;

    /// The resolver's failure behind the last rejected or failed resolution,
    /// kept for diagnostics until the next one.
    [[nodiscard]] FaultResolutionDispatcherFailure GetLastResolutionFailure() const noexcept
    {
        const auto state = m_transition.State();
        if (!m_transition.Claim(state, state))
        {
            return {};
        }
        const auto result = m_lastFailure;
        m_transition.Publish(state);
        return result;
    }

private:
    [[nodiscard]] PagerResolutionResult ResolveClaimed(Object::CapabilitySpace& caller,
                                                       const PagerResolveRequest& request) noexcept;
    // The resolution is split so each frame stays within the target stack
    // budget: the token and space leases, then the backing leases, then the
    // resolver, whose request is built in m_request.
    [[nodiscard]] PagerResolutionResult ResolveLeased(Object::CapabilitySpace& caller,
                                                      const PagerResolveRequest& request,
                                                      Object::ObjectId pendingToken) noexcept;
    [[nodiscard]] PagerResolutionResult ResolveWithBacking(Object::CapabilitySpace& caller,
                                                           const PagerResolveRequest& request) noexcept;
    [[nodiscard]] PagerResolutionResult RunResolver() noexcept;
    /// Claims the pending token the caller holds and exits its thread; the
    /// token lease ends before the caller retires the token.
    [[nodiscard]] PagerResolutionResult TerminateLeased(Object::CapabilitySpace& caller,
                                                        const PagerResolveRequest& request,
                                                        Object::ObjectId pendingToken) noexcept;
    /// Retires the resolved token and ends the fault: the channel is Idle.
    [[nodiscard]] PagerResolutionResult FinishResolution(Object::ObjectId token) noexcept;
    [[nodiscard]] bool BeginResolver(FaultResolutionDispatcherState& state, PagerResolutionError& rejection) noexcept;
    [[nodiscard]] bool ContinueResolver(FaultResolutionDispatcherState& state) noexcept;
    [[nodiscard]] bool RetireToken(Object::ObjectId token) noexcept;
    [[nodiscard]] bool CollectRetiredTokens() noexcept;
    [[nodiscard]] bool ClaimPending() noexcept;
    void RecordBusy() noexcept;
    void SignalIfDelivered() noexcept;
    [[nodiscard]] PagerResolutionResult Fail(PagerResolutionError error) noexcept;

    Object::ObjectRegistry& m_registry;
    Object::RootCapabilityIssuer& m_issuer;
    TlbShootdownDispatcher& m_shootdown;
    Hal::Memory::ITlbShootdownTransport& m_transport;
    FaultTokenFactory m_factory;
    Object::ObjectId m_retiredTokens[FaultTokenFactory::MaximumTokens]{};
    PagerFaultDispatcher m_dispatcher;
    PageFaultIngress m_ingress;
    FaultResolutionDispatcher m_resolver;
    FaultResolutionMapRequest m_request{};
    FaultResolutionDispatcherFailure m_lastFailure{};
    PagerChannelConfiguration m_configuration{};
    Foundation::UInt64 m_transaction{0U};
    Foundation::UInt64 m_deliveries{0U};
    Foundation::UInt64 m_resolutions{0U};
    Foundation::UInt64 m_terminations{0U};
    mutable Platform::Synchronization::AtomicUInt64 m_busy{};
    FaultResolutionDispatcherError m_resolutionError{FaultResolutionDispatcherError::None};
    FaultChannelTransition m_transition{};
    PagerChannelError m_error{PagerChannelError::None};
    bool m_signalled{false};
    bool m_terminal{false};
};
static_assert(FaultTokenFactory::MaximumTokens * sizeof(Object::ObjectId) <= 512U);
} // Namespace Asteria::Kernel::Memory

#endif // ASTERIA_KERNEL_MEMORY_PAGER_CHANNEL_HPP_INCLUDED
