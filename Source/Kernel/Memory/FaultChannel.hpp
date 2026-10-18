// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_FAULT_CHANNEL_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_FAULT_CHANNEL_HPP_INCLUDED
#include "Kernel/Memory/PagerChannel.hpp"
#include "Kernel/Object/CapabilityKeyDerivation.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
namespace Asteria::Kernel::Memory
{
// The private Endpoint has no send capability. Native supervisor adapters only supply the ISA's
// block/switch operation.
class FaultChannel final : public Object::IKernelObject
{
public:
    FaultChannel(Object::ObjectRegistry& registry, Object::RootCapabilityIssuer& issuer,
                 Object::RevocationRegistry& revocations, Time::IMonotonicClock& clock,
                 Object::CapabilitySpace& receiver, Resource::ResourceDomain& domain,
                 const Object::CapabilityValidationKey& key, Scheduler::ProcessorSchedulerWakeFacade& wake,
                 TlbShootdownDispatcher& shootdown, Hal::Memory::ITlbShootdownTransport& transport) noexcept
        : m_domain{domain}, m_tokens{registry, revocations, clock, key.Word0, key.Word1},
          m_endpoint{receiver, domain.GetIdentifier(), Ipc::EndpointConfiguration::Default()},
          m_channel{registry, issuer, m_endpoint, wake, shootdown, transport}
    {
        m_endpoint.AttachReceiveAuthority(*this);
    }
    [[nodiscard]] Object::ObjectType GetObjectType() const noexcept override
    {
        return Object::ObjectType::FaultChannel;
    }
    [[nodiscard]] Object::ResourceDomainId GetChargeOwner() const noexcept override
    {
        return m_domain.GetIdentifier();
    }
    [[nodiscard]] Resource::ResourceDomain& GetDomain() const noexcept
    {
        return m_domain;
    }
    [[nodiscard]] PagerChannel& GetChannel() noexcept
    {
        return m_channel;
    }
    [[nodiscard]] Ipc::Endpoint& GetEndpoint() noexcept
    {
        return m_endpoint;
    }
    [[nodiscard]] Ipc::Notification& GetReadiness() noexcept
    {
        return m_readiness;
    }
    [[nodiscard]] Ipc::NotificationId GetReadinessId() const noexcept
    {
        return m_readinessId;
    }
    [[nodiscard]] bool Initialize(Object::ObjectRegistry& registry, Object::ObjectId identity,
                                  Object::ObjectId readiness, Object::RevocationDomainId root,
                                  IPagerShootdownProgress& progress) noexcept
    {
        m_readinessId = Ipc::NotificationId{registry, readiness};
        m_endpoint.AttachReadiness(m_readiness, m_readinessId);
        return m_tokens.IsReady() && m_endpoint.IsReady() &&
               m_channel.Configure(PagerChannelConfiguration{&m_tokens, &m_readiness, &progress, GetChargeOwner(),
                                                             GetChargeOwner(), root, identity}) ==
                   PagerChannelError::None;
    }
    // A close claim cannot race a new domain binding. Registry pins retain
    // storage; this counter additionally refuses destruction of live domains.
    [[nodiscard]] bool BindDomain() noexcept
    {
        Foundation::UInt64 count = m_domains.LoadAcquire();
        for (Foundation::Size attempt{0U}; attempt < Abi::ObjectPoolCapacities[0U] + 1U; ++attempt)
        {
            if (count >= Closing)
            {
                return false;
            }
            if (m_domains.CompareExchangeStrongAcquireRelease(count, count + 1U))
            {
                return true;
            }
        }
        return false;
    }
    void ReleaseDomain() noexcept
    {
        Foundation::UInt64 count = m_domains.LoadAcquire();
        for (Foundation::Size attempt{0U}; attempt <= Abi::ObjectPoolCapacities[0U]; ++attempt)
        {
            if (count == 0U || count >= Closing)
            {
                __builtin_trap();
            }
            if (m_domains.CompareExchangeStrongAcquireRelease(count, count - 1U))
            {
                return;
            }
        }
        __builtin_trap();
    }
    [[nodiscard]] bool BeginDestroy() noexcept
    {
        if (!m_channel.IsReclaimable())
        {
            return false;
        }
        Foundation::UInt64 expected{0U};
        return m_domains.CompareExchangeStrongAcquireRelease(expected, Closing);
    }

private:
    static constexpr Foundation::UInt64 Closing{Foundation::UInt64{1U} << 63U};
    Resource::ResourceDomain& m_domain;
    Object::CapabilitySpace m_tokens;
    Ipc::Endpoint m_endpoint;
    Ipc::Notification m_readiness;
    Ipc::NotificationId m_readinessId{};
    PagerChannel m_channel;
    Platform::Synchronization::AtomicUInt64 m_domains{};
};
} // Namespace Asteria::Kernel::Memory
#endif
