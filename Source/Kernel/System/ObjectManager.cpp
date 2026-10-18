// SPDX-License-Identifier: Apache-2.0

#include "Kernel/System/ObjectManager.hpp"
#include "Platform/Synchronization/ProcessorContext.hpp"

#include "Abi/SystemCallAbi.hpp"

namespace Asteria::Kernel::System
{
namespace
{
[[nodiscard]] constexpr ObjectManagementResult
MakeResult(const ObjectManagementError error, const Object::CapabilityError capability = Object::CapabilityError::Ok,
           const Object::CapabilityHandle created = {}, const Foundation::UInt64 value = 0U,
           const Foundation::UInt64 value1 = 0U) noexcept
{
    return ObjectManagementResult{
        .Error = error,
        .Capability = capability,
        .Reserved = {0U, 0U, 0U, 0U, 0U, 0U},
        .Created = created,
        .Value = value,
        .Value1 = value1,
    };
}

[[nodiscard]] constexpr ObjectManagementResult MapPoolFailure(const Object::ObjectPoolFailure& failure) noexcept
{
    switch (failure.Error)
    {
    case Object::ObjectPoolError::InvalidRequest:
        return MakeResult(ObjectManagementError::InvalidRequest);
    case Object::ObjectPoolError::CapabilityRejected:
    case Object::ObjectPoolError::MintRejected:
        return MakeResult(ObjectManagementError::CapabilityRejected, failure.Capability);
    case Object::ObjectPoolError::NotOwned:
        return MakeResult(ObjectManagementError::NotOwned);
    case Object::ObjectPoolError::Busy:
        return MakeResult(ObjectManagementError::Busy);
    case Object::ObjectPoolError::ResourceDomainRejected:
    case Object::ObjectPoolError::ChargeRejected:
    case Object::ObjectPoolError::CapacityExceeded:
    case Object::ObjectPoolError::RegistrationRejected:
        return MakeResult(ObjectManagementError::ResourceExhausted);
    case Object::ObjectPoolError::None:
    case Object::ObjectPoolError::InvalidConfiguration:
    case Object::ObjectPoolError::DestroyRejected:
    case Object::ObjectPoolError::InvariantViolation:
    case Object::ObjectPoolError::Faulted:
        break;
    }
    return MakeResult(ObjectManagementError::Faulted);
}
} // Namespace

ObjectManagementResult ObjectManager::Create(Object::CapabilitySpace& caller,
                                             const ObjectCreateRequest& request) noexcept
{
    const auto processor = Platform::Synchronization::CurrentProcessor();
    if (request.Type == Object::ObjectType::Interrupt && processor.IsValid() && processor.GetValue() != 0U)
    {
        return MakeResult(ObjectManagementError::NotConfigured);
    }
    if ((request.Type != Object::ObjectType::Endpoint) && (request.Type != Object::ObjectType::Frame) &&
        (request.Type != Object::ObjectType::MemoryObject) && (request.Type != Object::ObjectType::Thread) &&
        (request.Type != Object::ObjectType::CapabilitySpace) && (request.Type != Object::ObjectType::AddressSpace) &&
        (request.Type != Object::ObjectType::ProtectionDomain) && (request.Type != Object::ObjectType::WaitSet) &&
        (request.Type != Object::ObjectType::ResourceDomain) && (request.Type != Object::ObjectType::IommuDomain) &&
        (request.Type != Object::ObjectType::Interrupt) && (request.Type != Object::ObjectType::FaultChannel))
    {
        return MakeResult(ObjectManagementError::UnsupportedType);
    }
    if (!Collect())
    {
        return MakeResult(ObjectManagementError::Faulted);
    }
    const CreationResult created = CreatePooled(caller, request);
    if (!created.HasValue())
    {
        // Refusals leave both object and charge absent.
        if (request.Type == Object::ObjectType::ResourceDomain &&
            request.DomainFlags == Abi::ObjectCreateDomainFlagCode &&
            created.GetError().Capability == Object::CapabilityError::RightsDenied)
        {
            __atomic_fetch_add(&m_codeFlagRefusals, 1U, __ATOMIC_RELAXED);
        }
        if (created.GetError().Resource == Resource::ResourceDomainError::ObjectLimitExceeded)
        {
            constexpr Object::ObjectType types[Abi::LimitedObjectCount]{Object::ObjectType::ProtectionDomain,
                                                                        Object::ObjectType::AddressSpace,
                                                                        Object::ObjectType::CapabilitySpace,
                                                                        Object::ObjectType::Endpoint,
                                                                        Object::ObjectType::WaitSet,
                                                                        Object::ObjectType::FaultChannel,
                                                                        Object::ObjectType::Frame,
                                                                        Object::ObjectType::MemoryObject};
            for (Foundation::Size i{0U}; i < Abi::LimitedObjectCount; ++i)
            {
                if (request.Type == types[i])
                {
                    __atomic_fetch_add(&m_typeLimitRefusals[i], 1U, __ATOMIC_RELAXED);
                }
            }
        }
        // A kernel without a usable IOMMU or message vectors has no
        // IommuDomain or Interrupt to give.
        return (((request.Type == Object::ObjectType::IommuDomain) ||
                 (request.Type == Object::ObjectType::Interrupt)) &&
                (created.GetError().Error == Object::ObjectPoolError::InvalidConfiguration))
                   ? MakeResult(ObjectManagementError::NotConfigured)
                   : MapPoolFailure(created.GetError());
    }
    return MakeResult(ObjectManagementError::None, Object::CapabilityError::Ok, created.GetValue());
}

// Each type's creation runs in its own frame, so Create stays within the
// stack budget as types are added.
ObjectManager::CreationResult ObjectManager::CreatePooled(Object::CapabilitySpace& caller,
                                                          const ObjectCreateRequest& request) noexcept
{
    switch (request.Type)
    {
    case Object::ObjectType::FaultChannel:
        return m_faultChannels.Create(caller, request.Authority, m_spaces.FindIdentity(caller));
    case Object::ObjectType::Endpoint:
        return CreateEndpoint(caller, request);
    case Object::ObjectType::Thread:
        return CreateThread(caller, request);
    case Object::ObjectType::WaitSet:
        return CreateWaitSet(caller, request);
    case Object::ObjectType::CapabilitySpace:
        return CreateSpace(caller, request);
    case Object::ObjectType::AddressSpace:
        return CreateAddressSpace(caller, request);
    case Object::ObjectType::ProtectionDomain:
        return CreateDomain(caller, request);
    case Object::ObjectType::Frame:
        return CreateFrame(caller, request);
    case Object::ObjectType::ResourceDomain:
        return CreateResourceDomain(caller, request);
    case Object::ObjectType::IommuDomain:
        return CreateIommuDomain(caller, request);
    case Object::ObjectType::Interrupt:
        return CreateInterrupt(caller, request);
    default:
        return CreateMemoryObject(caller, request);
    }
}

// An Interrupt for the messages of a function the caller is the assignee of, charged to the
// authorizing domain.
ObjectManager::CreationResult ObjectManager::CreateInterrupt(Object::CapabilitySpace& caller,
                                                             const ObjectCreateRequest& request) noexcept
{
    // Interrupt routing/programming is BSP-only.
    const auto processor = Platform::Synchronization::CurrentProcessor();
    if (processor.IsValid() && processor.GetValue() != 0U)
    {
        return CreationResult::Failure(Object::MakeObjectPoolFailure(Object::ObjectPoolError::InvalidConfiguration));
    }
    const auto interrupt = m_interrupts.Create(Interrupt::InterruptCreateRequest{
        .Space = &caller,
        .Authority = request.Authority,
        .Function = request.InterruptFunction,
        .Caller = request.Caller,
        .Source = request.InterruptSource,
    });
    return interrupt.HasValue() ? CreationResult::Success(interrupt.GetValue().Capability)
                                : CreationResult::Failure(interrupt.GetError());
}

// A domain of the platform's IOMMU charged to the authorizing domain. On identity backends only the
// assignee of a trusted function gets one; every other driver sees what it sees without an IOMMU.
ObjectManager::CreationResult ObjectManager::CreateIommuDomain(Object::CapabilitySpace& caller,
                                                               const ObjectCreateRequest& request) noexcept
{
    if ((m_iommuAdmission == IommuAdmission::TrustedAssignees) && !m_deviceControl.HasTrustedAssignment(request.Caller))
    {
        return CreationResult::Failure(Object::MakeObjectPoolFailure(Object::ObjectPoolError::InvalidConfiguration));
    }
    const auto domain = m_iommuDomains.Create(Device::IommuDomainCreateRequest{
        .Space = &caller,
        .Authority = request.Authority,
        .InputAddressBits = request.IommuInputAddressBits,
        .Reserved = {},
    });
    return domain.HasValue() ? CreationResult::Success(domain.GetValue().Capability)
                             : CreationResult::Failure(domain.GetError());
}

// A child of the authorizing domain.
ObjectManager::CreationResult ObjectManager::CreateResourceDomain(Object::CapabilitySpace& caller,
                                                                  const ObjectCreateRequest& request) noexcept
{
    const auto domain = m_resourceDomains.Create(Resource::ResourceDomainCreateRequest{
        .Space = &caller,
        .Authority = request.Authority,
        .Limits = request.DomainLimits,
        .Flags = request.DomainFlags,
        .ExplicitObjectLimits = request.ExplicitObjectLimits,
    });
    return domain.HasValue() ? CreationResult::Success(domain.GetValue().Capability)
                             : CreationResult::Failure(domain.GetError());
}

// A WaitSet charged to the authorizing domain.
ObjectManager::CreationResult ObjectManager::CreateWaitSet(Object::CapabilitySpace& caller,
                                                           const ObjectCreateRequest& request) noexcept
{
    const auto waitSet = m_waitSets.Create(Ipc::WaitSetCreateRequest{
        .Space = &caller,
        .Authority = request.Authority,
    });
    return waitSet.HasValue() ? CreationResult::Success(waitSet.GetValue().Capability)
                              : CreationResult::Failure(waitSet.GetError());
}

ObjectManager::CreationResult ObjectManager::CreateEndpoint(Object::CapabilitySpace& caller,
                                                            const ObjectCreateRequest& request) noexcept
{
    // A receiver other than the caller's space is a created CapabilitySpace the caller manages. The
    // Endpoint pins a created receiver, the caller's own space included, so the space outlives it.
    Object::CapabilitySpace* receiver{nullptr};
    Object::ObjectId receiverObject = m_spaces.FindIdentity(caller);
    if (receiverObject.IsValid())
    {
        receiver = &caller;
    }
    if (request.EndpointReceiver.IsValid())
    {
        const auto space = caller.Lookup(request.EndpointReceiver, Object::ObjectType::CapabilitySpace,
                                         Object::CapabilityOperation::Manage);
        if (!space.HasValue())
        {
            return CreationResult::Failure(
                Object::MakeObjectPoolFailure(Object::ObjectPoolError::CapabilityRejected, space.GetError()));
        }
        receiver = &static_cast<Object::CapabilitySpaceObject&>(space.GetValue().GetObject()).GetSpace();
        receiverObject = space.GetValue().GetDescriptor().Object;
    }
    const auto endpoint = m_endpoints.Create(Ipc::EndpointCreateRequest{
        .Space = &caller,
        .Authority = request.Authority,
        .Configuration = request.Endpoint,
        .Receiver = receiver,
        .ReceiverObject = receiverObject,
        .SealedSelector = request.EndpointSealedSelector,
    });
    return endpoint.HasValue() ? CreationResult::Success(endpoint.GetValue().Capability)
                               : CreationResult::Failure(endpoint.GetError());
}

ObjectManager::CreationResult ObjectManager::CreateThread(Object::CapabilitySpace& caller,
                                                          const ObjectCreateRequest& request) noexcept
{
    // Another domain than the caller's is a ProtectionDomain the caller manages; the platform finds
    // its record.
    Scheduler::UserThreadStart start = request.Thread;
    if (request.ThreadDomain.IsValid())
    {
        const auto domain = caller.Lookup(request.ThreadDomain, Object::ObjectType::ProtectionDomain,
                                          Object::CapabilityOperation::Manage);
        if (!domain.HasValue())
        {
            return CreationResult::Failure(
                Object::MakeObjectPoolFailure(Object::ObjectPoolError::CapabilityRejected, domain.GetError()));
        }
        start.Domain = &static_cast<const Protection::ProtectionDomain&>(domain.GetValue().GetObject());
    }
    const auto thread = m_threads.Create(Scheduler::ThreadCreateRequest{
        .Space = &caller,
        .Authority = request.Authority,
        .Start = start,
    });
    return thread.HasValue() ? CreationResult::Success(thread.GetValue().Capability)
                             : CreationResult::Failure(thread.GetError());
}

ObjectManager::CreationResult ObjectManager::CreateSpace(Object::CapabilitySpace& caller,
                                                         const ObjectCreateRequest& request) noexcept
{
    const auto space = m_spaces.Create(Object::CapabilitySpaceCreateRequest{
        .Space = &caller,
        .Authority = request.Authority,
    });
    return space.HasValue() ? CreationResult::Success(space.GetValue().Capability)
                            : CreationResult::Failure(space.GetError());
}

ObjectManager::CreationResult ObjectManager::CreateAddressSpace(Object::CapabilitySpace& caller,
                                                                const ObjectCreateRequest& request) noexcept
{
    const auto space = m_addressSpaces.Create(Memory::AddressSpaceCreateRequest{
        .Space = &caller,
        .Authority = request.Authority,
        .LargePageTableArena = request.LargePageTableArena,
    });
    return space.HasValue() ? CreationResult::Success(space.GetValue().Capability)
                            : CreationResult::Failure(space.GetError());
}

ObjectManager::CreationResult ObjectManager::CreateDomain(Object::CapabilitySpace& caller,
                                                          const ObjectCreateRequest& request) noexcept
{
    if (!request.DomainChannel.IsValid() && m_domains.IsCreated(request.Caller))
    {
        return CreationResult::Failure(Object::MakeObjectPoolFailure(Object::ObjectPoolError::InvalidRequest));
    }
    const auto domain = m_domains.Create(Protection::ProtectionDomainCreateRequest{
        .Space = &caller,
        .Authority = request.Authority,
        .AddressSpace = request.DomainAddressSpace,
        .Capabilities = request.DomainCapabilities,
        .SupervisionTag = request.SupervisionTag,
        .Channel = request.DomainChannel,
        .DiagnosticComponentKind = request.DiagnosticComponentKind,
    });
    return domain.HasValue() ? CreationResult::Success(domain.GetValue().Capability)
                             : CreationResult::Failure(domain.GetError());
}

ObjectManager::CreationResult ObjectManager::CreateFrame(Object::CapabilitySpace& caller,
                                                         const ObjectCreateRequest& request) noexcept
{
    const auto frame = m_frames.Create(Memory::FrameCreateRequest{
        .Space = &caller,
        .Authority = request.Authority,
        .Pages = request.Pages,
        .Region = request.FrameRegion,
        .RegionOffsetPages = request.FrameRegionOffsetPages,
        .AlignmentPages = request.FrameAlignmentPages,
        .AddressLimitBits = request.FrameAddressLimitBits,
    });
    return frame.HasValue() ? CreationResult::Success(frame.GetValue().Capability)
                            : CreationResult::Failure(frame.GetError());
}

ObjectManager::CreationResult ObjectManager::CreateMemoryObject(Object::CapabilitySpace& caller,
                                                                const ObjectCreateRequest& request) noexcept
{
    // Only a code producer may admit Execute. The pool reauthorizes this same immutable capability
    // at mint time.
    if (request.MaximumPermissions.Contains(Memory::PagePermission::Execute))
    {
        const auto authority =
            caller.Lookup(request.Authority, Object::ObjectType::ResourceDomain, Object::CapabilityOperation::Manage);
        if (!authority.HasValue())
        {
            return CreationResult::Failure(
                Object::MakeObjectPoolFailure(Object::ObjectPoolError::CapabilityRejected, authority.GetError()));
        }
        if (!authority.GetValue().GetDescriptor().Rights.ContainsAll(
                Object::CapabilityRights{Object::CapabilityRight::Code}))
        {
            return CreationResult::Failure(Object::MakeObjectPoolFailure(Object::ObjectPoolError::CapabilityRejected,
                                                                         Object::CapabilityError::RightsDenied));
        }
    }
    const auto memory = m_memoryObjects.Create(Memory::MemoryObjectCreateRequest{
        .Space = &caller,
        .Authority = request.Authority,
        .Pages = request.Pages,
        .MaximumPermissions = request.MaximumPermissions,
        .CachePolicy = request.MemoryCachePolicy,
    });
    return memory.HasValue() ? CreationResult::Success(memory.GetValue().Capability)
                             : CreationResult::Failure(memory.GetError());
}

ObjectManagementResult ObjectManager::Destroy(Object::CapabilitySpace& caller, const Object::CapabilityHandle object,
                                              const Object::ObjectType type) noexcept
{
    if ((type != Object::ObjectType::Endpoint) && (type != Object::ObjectType::Frame) &&
        (type != Object::ObjectType::MemoryObject) && (type != Object::ObjectType::Thread) &&
        (type != Object::ObjectType::CapabilitySpace) && (type != Object::ObjectType::AddressSpace) &&
        (type != Object::ObjectType::ProtectionDomain) && (type != Object::ObjectType::WaitSet) &&
        (type != Object::ObjectType::ResourceDomain) && (type != Object::ObjectType::IommuDomain) &&
        (type != Object::ObjectType::Interrupt) && (type != Object::ObjectType::FaultChannel))
    {
        return MakeResult(ObjectManagementError::UnsupportedType);
    }
    if (!Collect())
    {
        return MakeResult(ObjectManagementError::Faulted);
    }
    if (type == Object::ObjectType::ResourceDomain)
    {
        return DestroyResourceDomain(caller, object);
    }
    if (type == Object::ObjectType::Thread)
    {
        // The exited thread's native slot is released by the idle loop, so
        // its storage returns at a later collection.
        const auto reclaimed = m_threads.Destroy(caller, object);
        if (!reclaimed.HasValue())
        {
            return MapPoolFailure(reclaimed.GetError());
        }
        return Collect()
                   ? MakeResult(ObjectManagementError::None, Object::CapabilityError::Ok, {}, reclaimed.GetValue())
                   : MakeResult(ObjectManagementError::Faulted);
    }
    const ObjectManagementResult destroyed = DestroyPooled(caller, object, type);
    if (destroyed.Error != ObjectManagementError::None)
    {
        return destroyed;
    }
    // The pool's lease on the object ended when Destroy returned, so an
    // object nothing else references is reclaimed before the call returns.
    return Collect() ? MakeResult(ObjectManagementError::None) : MakeResult(ObjectManagementError::Faulted);
}

ObjectManagementResult ObjectManager::DestroyPooled(Object::CapabilitySpace& caller,
                                                    const Object::CapabilityHandle object,
                                                    const Object::ObjectType type) noexcept
{
    const auto pooled = [](const Object::ObjectPoolFailure& destroyed) noexcept
    {
        return (destroyed.Error == Object::ObjectPoolError::None) ? MakeResult(ObjectManagementError::None)
                                                                  : MapPoolFailure(destroyed);
    };
    switch (type)
    {
    case Object::ObjectType::AddressSpace:
        return pooled(m_addressSpaces.Destroy(caller, object));
    case Object::ObjectType::CapabilitySpace:
        return pooled(m_spaces.Destroy(caller, object));
    case Object::ObjectType::FaultChannel:
        return pooled(m_faultChannels.Destroy(caller, object));
    case Object::ObjectType::ProtectionDomain:
        return pooled(m_domains.Destroy(caller, object));
    case Object::ObjectType::WaitSet:
    {
        const auto waitSet = m_waitSets.Destroy(caller, object);
        return waitSet.HasValue() ? MakeResult(ObjectManagementError::None) : MapPoolFailure(waitSet.GetError());
    }
    case Object::ObjectType::IommuDomain:
    {
        // The device control stops the DMA of every function attached to it first. A domain whose
        // IOMMU cannot quiesce it stays live and reports so.
        const auto domain = m_iommuDomains.Destroy(caller, object, m_deviceControl);
        if (!domain.HasValue() && (domain.GetError().Error == Object::ObjectPoolError::DestroyRejected))
        {
            return MakeResult(ObjectManagementError::NotConfigured);
        }
        return domain.HasValue() ? MakeResult(ObjectManagementError::None) : MapPoolFailure(domain.GetError());
    }
    case Object::ObjectType::Interrupt:
    {
        // Its messages go off first.
        const auto interrupt = m_interrupts.Destroy(caller, object);
        if (!interrupt.HasValue() && (interrupt.GetError().Error == Object::ObjectPoolError::DestroyRejected))
        {
            return MakeResult(ObjectManagementError::NotConfigured);
        }
        return interrupt.HasValue() ? MakeResult(ObjectManagementError::None) : MapPoolFailure(interrupt.GetError());
    }
    default:
        break;
    }
    const auto destroyed = (type == Object::ObjectType::Endpoint)
                               ? m_endpoints.Destroy(caller, object)
                               : ((type == Object::ObjectType::Frame) ? m_frames.Destroy(caller, object)
                                                                      : m_memoryObjects.Destroy(caller, object));
    return destroyed.HasValue() ? MakeResult(ObjectManagementError::None) : MapPoolFailure(destroyed.GetError());
}

namespace
{
/// The child a drain destroys and its descendants, which the pool that
/// created them knows.
class DrainedSubtree final : public Resource::IResourceDomainSubtree
{
public:
    constexpr DrainedSubtree(const Resource::ResourceDomainPool& pool, const Object::ObjectId root) noexcept
        : m_pool{pool}, m_root{root}
    {
    }

    [[nodiscard]] bool Contains(const Resource::ResourceDomain* const domain) const noexcept override
    {
        return m_pool.IsInSubtree(domain, m_root);
    }

private:
    const Resource::ResourceDomainPool& m_pool;
    const Object::ObjectId m_root;
};

/// The dirty runs a drain scrubs at most once its Frames are gone: about
/// what a component's Frames dirty, so a drained domain's pages come back
/// clean before the call returns (its phases no longer scrub one run each).
constexpr Foundation::Size DrainScrubRuns{8U};

/// Folds one pool's collection into reclaimed; false if the pool faulted.
template <typename Collected>
[[nodiscard]] bool FoldCollected(const Collected& collected, Foundation::UInt64& reclaimed) noexcept
{
    if (!collected.HasValue())
    {
        return false;
    }
    reclaimed += collected.GetValue();
    return true;
}

/// Folds one pool's drain into busy; false if the pool faulted.
[[nodiscard]] bool FoldDrain(const Object::ObjectPoolFailure& drained, bool& busy) noexcept
{
    busy = busy || (drained.Error == Object::ObjectPoolError::Busy);
    return (drained.Error == Object::ObjectPoolError::None) || (drained.Error == Object::ObjectPoolError::Busy);
}
} // Namespace

// The pools drain in Collect order, each collected before the next drains,
// so a reclaimed thread's pin on its ProtectionDomain is gone when that pool
// retires it. The domains are destroyed last: while something is Busy the
// capability still names the domain, and the caller repeats the call.
ObjectManagementResult ObjectManager::DestroyResourceDomain(Object::CapabilitySpace& caller,
                                                            const Object::CapabilityHandle object) noexcept
{
    const auto owned = m_resourceDomains.FindOwned(caller, object);
    if (!owned.HasValue())
    {
        return MapPoolFailure(owned.GetError());
    }
    const DrainedSubtree subtree{m_resourceDomains, owned.GetValue()};
    if (!m_threads.HaveChargedExited(subtree))
    {
        return MakeResult(ObjectManagementError::Busy);
    }
    // From here on nothing new may be charged to the subtree, while this call or a retry destroys
    // what is.
    const auto draining = m_resourceDomains.BeginDrain(owned.GetValue());
    if (draining.Error != Resource::ResourceDomainPoolError::None)
    {
        return MapPoolFailure(draining);
    }
    // The device pre-pass, whatever the caller does: a dying driver's Interrupts go with their
    // messages off, its functions stop bus mastering, its IommuDomains are quiesced and destroyed,
    // and its functions lose their assignment, before any of its memory can be reused. A
    // configuration write a retry may yet make (a message or bus-master disable, so a release while
    // messages stay on) makes the drain Busy before anything of the subtree's memory goes; an
    // invariant failure faults it.
    bool busy{false};
    if (!FoldDrain(m_interrupts.DestroyCharged(subtree), busy))
    {
        return MakeResult(ObjectManagementError::Faulted);
    }
    const Device::PciFunctionControlError disabled = m_deviceControl.DisableCharged(subtree);
    if (disabled != Device::PciFunctionControlError::None)
    {
        return MakeResult((disabled == Device::PciFunctionControlError::ConfigurationRejected)
                              ? ObjectManagementError::Busy
                              : ObjectManagementError::Faulted);
    }
    if (!FoldDrain(m_iommuDomains.DestroyCharged(subtree, m_deviceControl), busy))
    {
        return MakeResult(ObjectManagementError::Faulted);
    }
    const Device::PciFunctionControlError released = m_deviceControl.ReleaseCharged(subtree);
    if (released != Device::PciFunctionControlError::None)
    {
        return MakeResult(((released == Device::PciFunctionControlError::ConfigurationRejected) ||
                           (released == Device::PciFunctionControlError::MessagesInUse))
                              ? ObjectManagementError::Busy
                              : ObjectManagementError::Faulted);
    }
    // Each phase needs the earlier phases' objects gone, not clean pages:
    // the call scrubs once its Frames are gone.
    const bool drained = FoldDrain(m_threads.DestroyCharged(subtree), busy) && CollectPools() &&
                         FoldDrain(m_waitSets.DestroyCharged(subtree), busy) && CollectPools() &&
                         FoldDrain(m_endpoints.DestroyCharged(subtree), busy) && CollectPools() &&
                         FoldDrain(m_domains.DestroyCharged(subtree), busy) && CollectPools() &&
                         FoldDrain(m_faultChannels.DestroyCharged(subtree), busy) && CollectPools() &&
                         FoldDrain(m_spaces.DestroyCharged(subtree), busy) && CollectPools() &&
                         FoldDrain(m_addressSpaces.DestroyCharged(subtree), busy) && CollectPools() &&
                         FoldDrain(m_frames.DestroyCharged(subtree), busy) && CollectPools() &&
                         FoldDrain(m_memoryObjects.DestroyCharged(subtree), busy) && CollectPools();
    if (!drained)
    {
        return MakeResult(ObjectManagementError::Faulted);
    }
    if (busy)
    {
        return ScrubRuns(DrainScrubRuns) ? MakeResult(ObjectManagementError::Busy)
                                         : MakeResult(ObjectManagementError::Faulted);
    }
    // A concurrent drain of the same domain may have destroyed it meanwhile
    // (NotOwned); the root is named by its identity, never by its storage.
    const Object::ObjectPoolFailure destroyed = m_resourceDomains.DestroySubtree(owned.GetValue());
    if (destroyed.Error != Object::ObjectPoolError::None)
    {
        return MapPoolFailure(destroyed);
    }
    return (CollectPools() && ScrubRuns(DrainScrubRuns)) ? MakeResult(ObjectManagementError::None)
                                                         : MakeResult(ObjectManagementError::Faulted);
}

namespace
{
[[nodiscard]] constexpr ObjectManagementResult MapMapperResult(const Memory::MemoryMapperResult& mapped) noexcept
{
    switch (mapped.Error)
    {
    case Memory::MemoryMapperError::None:
        return MakeResult(ObjectManagementError::None);
    case Memory::MemoryMapperError::NotConfigured:
        return MakeResult(ObjectManagementError::NotConfigured);
    case Memory::MemoryMapperError::CapabilityRejected:
        return MakeResult(ObjectManagementError::CapabilityRejected, mapped.Capability);
    case Memory::MemoryMapperError::InvalidRequest:
        return MakeResult(ObjectManagementError::InvalidRequest);
    case Memory::MemoryMapperError::CapacityExceeded:
        return MakeResult(ObjectManagementError::ResourceExhausted);
    case Memory::MemoryMapperError::Busy:
        return MakeResult(ObjectManagementError::Busy);
    case Memory::MemoryMapperError::Faulted:
        break;
    }
    return MakeResult(ObjectManagementError::Faulted);
}
} // Namespace

ObjectManagementResult ObjectManager::Map(Object::CapabilitySpace& caller,
                                          const Memory::MemoryMapRequest& request) noexcept
{
    return MapMapperResult(LocalMapper().Map(caller, request));
}

ObjectManagementResult ObjectManager::Unmap(Object::CapabilitySpace& caller, const Object::CapabilityHandle space,
                                            const Memory::VirtualAddress virtualStart,
                                            const Memory::PageCount pages) noexcept
{
    const ObjectManagementResult result = MapMapperResult(LocalMapper().Unmap(caller, space, virtualStart, pages));
    if ((result.Error == ObjectManagementError::None) && !Collect())
    {
        return MakeResult(ObjectManagementError::Faulted);
    }
    return result;
}

// Each pool's reclamation releases the pins the next one waits for: a thread's on its domain, a
// domain's on its spaces, an Endpoint's on the space it receives in, a space's on its mappings'
// Frames and MemoryObjects, so one pass reclaims a whole dead domain.
ObjectManagementResult ObjectManager::Terminate(Object::CapabilitySpace& caller,
                                                const Object::CapabilityHandle thread) noexcept
{
    if (!Collect())
    {
        return MakeResult(ObjectManagementError::Faulted);
    }
    const Object::ObjectPoolFailure terminated = m_threads.Terminate(caller, thread, Abi::KilledExitCode);
    switch (terminated.Error)
    {
    case Object::ObjectPoolError::None:
        return MakeResult(ObjectManagementError::None);
    case Object::ObjectPoolError::InvalidConfiguration:
        return MakeResult(ObjectManagementError::NotConfigured);
    default:
        return MapPoolFailure(terminated);
    }
}

// An Endpoint's readiness, or an Interrupt's Notification.
ObjectManagementResult ObjectManager::FindBindReadiness(Object::CapabilitySpace& caller,
                                                        const Object::CapabilityHandle source,
                                                        Ipc::NotificationId& identity, Ipc::Notification*& readiness,
                                                        Object::ObjectPin& pin) noexcept
{
    const auto queue = caller.Lookup(source, Object::ObjectType::Endpoint, Object::CapabilityOperation::Read);
    if (queue.HasValue())
    {
        // The lookup checked the type, and only Endpoint instances report it.
        const Ipc::Endpoint& bound = static_cast<const Ipc::Endpoint&>(queue.GetValue().GetObject());
        identity = bound.GetReadinessId();
        readiness = bound.GetReadiness();
        if (readiness == nullptr || m_registry.PinObject(identity.GetObjectId(), Object::ObjectType::Notification,
                                                         pin) != Object::ObjectPinError::Ok)
        {
            return MakeResult(ObjectManagementError::InvalidRequest);
        }
    }
    else if ((queue.GetError() == Object::CapabilityError::TypeMismatch) ||
             (queue.GetError() == Object::CapabilityError::RightsDenied))
    {
        // A lookup checks rights before the type, so an Interrupt's
        // capability, without Read, fails the Endpoint lookup either way.
        const auto channel = caller.Lookup(source, Object::ObjectType::FaultChannel, Object::CapabilityOperation::Read);
        if (channel.HasValue())
        {
            auto& bound = static_cast<Memory::FaultChannel&>(channel.GetValue().GetObject());
            identity = bound.GetReadinessId();
            readiness = &bound.GetReadiness();
            if (m_registry.PinObject(identity.GetObjectId(), Object::ObjectType::Notification, pin) !=
                Object::ObjectPinError::Ok)
            {
                return MakeResult(ObjectManagementError::CapabilityRejected, Object::CapabilityError::ObjectNotFound);
            }
            return MakeResult(ObjectManagementError::None);
        }
        const auto interrupt = m_interrupts.FindReadiness(caller, source);
        if (!interrupt.HasValue())
        {
            if (interrupt.GetError().Error != Object::ObjectPoolError::CapabilityRejected)
            {
                return MakeResult(ObjectManagementError::InvalidRequest);
            }
            // Something other than an Interrupt keeps the Endpoint lookup's
            // answer.
            return MakeResult(ObjectManagementError::CapabilityRejected,
                              (interrupt.GetError().Capability == Object::CapabilityError::TypeMismatch)
                                  ? queue.GetError()
                                  : interrupt.GetError().Capability);
        }
        identity = interrupt.GetValue().Identity;
        readiness = interrupt.GetValue().Notification;
    }
    else
    {
        return MakeResult(ObjectManagementError::CapabilityRejected, queue.GetError());
    }
    return (readiness == nullptr) ? MakeResult(ObjectManagementError::InvalidRequest)
                                  : MakeResult(ObjectManagementError::None);
}

// Any WaitSet, a boot one included, takes the readiness of a pooled Endpoint. Both leases live
// across the registration, which takes the WaitSet's lock and then the registry's, so no lock is
// held here.
ObjectManagementResult ObjectManager::BindWaitSet(Object::CapabilitySpace& caller,
                                                  const Object::CapabilityHandle waitSet,
                                                  const Object::CapabilityHandle endpoint,
                                                  const Foundation::UInt64 tag) noexcept
{
    if (tag == 0U)
    {
        return MakeResult(ObjectManagementError::InvalidRequest);
    }
    const auto set = caller.Lookup(waitSet, Object::ObjectType::WaitSet, Object::CapabilityOperation::Manage);
    if (!set.HasValue())
    {
        return MakeResult(ObjectManagementError::CapabilityRejected, set.GetError());
    }
    Ipc::NotificationId readinessId{};
    Ipc::Notification* readiness{nullptr};
    Object::ObjectPin sourcePin;
    const ObjectManagementResult found = FindBindReadiness(caller, endpoint, readinessId, readiness, sourcePin);
    if (found.Error != ObjectManagementError::None)
    {
        return found;
    }
    // The lookup checked the type, and only WaitSet instances report it.
    const auto registered =
        static_cast<Ipc::WaitSet&>(set.GetValue().GetObject()).RegisterNotification(readinessId, *readiness, tag);
    if (registered.HasValue())
    {
        __atomic_fetch_add(&m_binds, 1U, __ATOMIC_RELAXED);
        return MakeResult(ObjectManagementError::None);
    }
    switch (registered.GetError().Error)
    {
    case Ipc::WaitSetError::SetFull:
        return MakeResult(ObjectManagementError::ResourceExhausted);
    case Ipc::WaitSetError::ClaimActive:
        return MakeResult(ObjectManagementError::Busy);
    default:
        return MakeResult(ObjectManagementError::InvalidRequest);
    }
}

namespace
{
[[nodiscard]] constexpr ObjectManagementResult MapDeviceControl(const Device::PciFunctionControlError error) noexcept
{
    switch (error)
    {
    case Device::PciFunctionControlError::None:
        return MakeResult(ObjectManagementError::None);
    case Device::PciFunctionControlError::NotConfigured:
    case Device::PciFunctionControlError::IommuRequired:
        return MakeResult(ObjectManagementError::NotConfigured);
    case Device::PciFunctionControlError::NotAssigned:
    case Device::PciFunctionControlError::AssignmentRejected:
        return MakeResult(ObjectManagementError::InvalidRequest);
    case Device::PciFunctionControlError::NotAssignee:
    case Device::PciFunctionControlError::Revoked:
        return MakeResult(ObjectManagementError::NotOwned);
    case Device::PciFunctionControlError::CapacityExceeded:
        return MakeResult(ObjectManagementError::ResourceExhausted);
    case Device::PciFunctionControlError::AttachmentConflict:
    case Device::PciFunctionControlError::MessagesUnavailable:
        return MakeResult(ObjectManagementError::InvalidRequest);
    case Device::PciFunctionControlError::MessagesInUse:
        // Its Interrupt must be destroyed first.
        return MakeResult(ObjectManagementError::Busy);
    case Device::PciFunctionControlError::Quarantined:
        // Its Device Manager must confirm the device quiet first.
        return MakeResult(ObjectManagementError::Busy);
    case Device::PciFunctionControlError::DmaPending:
        // Detach must retain the quiet association.
        return MakeResult(ObjectManagementError::Busy);
    case Device::PciFunctionControlError::BridgeReserved:
        return MakeResult(ObjectManagementError::BridgeReserved);
    case Device::PciFunctionControlError::DisplayReserved:
        return MakeResult(ObjectManagementError::DisplayReserved);
    case Device::PciFunctionControlError::ReservedMemory:
        return MakeResult(ObjectManagementError::ReservedMemory);
    case Device::PciFunctionControlError::QuarantineMismatch:
        return MakeResult(ObjectManagementError::InvalidRequest);
    case Device::PciFunctionControlError::IommuRejected:
        // The domain refused; an ambiguous failure may remain attached
        // conservatively until the trusted drain retries it.
        return MakeResult(ObjectManagementError::NotConfigured);
    case Device::PciFunctionControlError::AccessDenied:
        return MakeResult(ObjectManagementError::InvalidRequest);
    case Device::PciFunctionControlError::ConfigurationRejected:
        break;
    }
    return MakeResult(ObjectManagementError::Faulted);
}
} // Namespace

// The Device Manager (Manage) assigns, releases, revokes and, after a drain, confirms the device
// quiet (DeviceQuiesced naming the drained assignment's sequence in the first word of domain); the
// assignee (Invoke) toggles its own bus mastering. Disabling with Manage is the revocation.
ObjectManagementResult ObjectManager::ControlDevice(Object::CapabilitySpace& caller,
                                                    const Protection::ProtectionDomainId callerDomain,
                                                    const Object::CapabilityHandle function,
                                                    const Foundation::UInt64 operation,
                                                    const Object::CapabilityHandle domain) noexcept
{
    if ((operation < Abi::DeviceAssign) || (operation > Abi::DeviceReboot) ||
        ((operation > Abi::DeviceWriteLegacySupport) && (operation < Abi::DevicePowerQuery)))
    {
        return MakeResult(ObjectManagementError::InvalidRequest);
    }
    if (operation >= Abi::DevicePowerQuery && operation <= Abi::DeviceReboot)
    {
        const auto managed =
            caller.Lookup(function, Object::ObjectType::ResourceDomain, Object::CapabilityOperation::Manage);
        if (!managed.HasValue())
        {
            return MakeResult(ObjectManagementError::CapabilityRejected, managed.GetError());
        }
        if (m_power == nullptr)
        {
            return MakeResult(ObjectManagementError::NotConfigured);
        }
        if (managed.GetValue().GetDescriptor().Object.GetValue() != m_powerManager.GetValue())
        {
            return MakeResult(ObjectManagementError::NotOwned);
        }
        if (operation == Abi::DevicePowerQuery)
        {
            return MakeResult(ObjectManagementError::None, Object::CapabilityError::Ok, {}, m_power->Available());
        }
        return MakeResult(m_power->Transition(operation == Abi::DeviceReboot) ? ObjectManagementError::None
                                                                              : ObjectManagementError::NotConfigured);
    }
    const bool manage =
        (operation == Abi::DeviceAssign) || (operation == Abi::DeviceRelease) || (operation == Abi::DeviceQuiesced);
    const bool disable = operation == Abi::DeviceDisableBusMaster;
    if (manage || disable)
    {
        const auto managed =
            caller.Lookup(function, Object::ObjectType::PciFunction, Object::CapabilityOperation::Manage);
        if (managed.HasValue())
        {
            // The lookup checked the type, and only PciFunction instances
            // report it.
            Device::PciFunction& target = static_cast<Device::PciFunction&>(managed.GetValue().GetObject());
            switch (operation)
            {
            case Abi::DeviceAssign:
                return AssignDevice(caller, target, domain);
            case Abi::DeviceRelease:
                return MapDeviceControl(m_deviceControl.Release(target));
            case Abi::DeviceQuiesced:
                return MapDeviceControl(m_deviceControl.ConfirmQuiesced(target, domain.GetSelector()));
            default:
                return MapDeviceControl(m_deviceControl.DisableBusMaster(target, callerDomain, true));
            }
        }
        if (manage)
        {
            return MakeResult(ObjectManagementError::CapabilityRejected, managed.GetError());
        }
    }
    if (operation == Abi::DeviceReadConfiguration)
    {
        // The Device Manager (Manage) reads a function no driver holds, as its proof that the
        // device is quiet needs; an assigned function is read by its assignee, with Invoke, below.
        const auto managed =
            caller.Lookup(function, Object::ObjectType::PciFunction, Object::CapabilityOperation::Manage);
        if (managed.HasValue())
        {
            // The lookup checked the type, and only PciFunction instances
            // report it. The dispatcher checked the offset's range.
            const Device::PciFunction& released =
                static_cast<const Device::PciFunction&>(managed.GetValue().GetObject());
            Foundation::UInt32 value{0U};
            const Device::PciFunctionControlError read = m_deviceControl.ReadReleasedConfiguration(
                released, static_cast<Foundation::UInt16>(domain.GetSelector()), value);
            if (read == Device::PciFunctionControlError::None)
            {
                return MakeResult(ObjectManagementError::None, Object::CapabilityError::Ok, {}, value);
            }
            if (read != Device::PciFunctionControlError::NotAssignee)
            {
                return MapDeviceControl(read);
            }
        }
    }
    const auto invoked = caller.Lookup(function, Object::ObjectType::PciFunction, Object::CapabilityOperation::Invoke);
    if (!invoked.HasValue())
    {
        return MakeResult(ObjectManagementError::CapabilityRejected, invoked.GetError());
    }
    const Device::PciFunction& target = static_cast<const Device::PciFunction&>(invoked.GetValue().GetObject());
    if ((operation == Abi::DeviceReadConfiguration) || (operation == Abi::DeviceWriteLegacySupport))
    {
        // The dispatcher checked the offset's and value's ranges.
        Foundation::UInt32 value{0U};
        const ObjectManagementResult accessed = MapDeviceControl(m_deviceControl.AccessLegacySupport(
            target, callerDomain, static_cast<Foundation::UInt16>(domain.GetSelector()),
            operation == Abi::DeviceWriteLegacySupport, static_cast<Foundation::UInt32>(domain.GetAuthenticator()),
            value));
        return (accessed.Error == ObjectManagementError::None)
                   ? MakeResult(ObjectManagementError::None, Object::CapabilityError::Ok, {}, value)
                   : accessed;
    }
    return disable ? MapDeviceControl(m_deviceControl.DisableBusMaster(target, callerDomain, false))
                   : MapDeviceControl(m_deviceControl.EnableBusMaster(target, callerDomain));
}

ObjectManagementResult ObjectManager::AssignDevice(Object::CapabilitySpace& caller, Device::PciFunction& function,
                                                   const Object::CapabilityHandle domain) noexcept
{
    const auto driver =
        caller.Lookup(domain, Object::ObjectType::ProtectionDomain, Object::CapabilityOperation::Manage);
    if (!driver.HasValue())
    {
        return MakeResult(ObjectManagementError::CapabilityRejected, driver.GetError());
    }
    // The lookup checked the type, and only ProtectionDomain instances report
    // it. Value 0 is the assignment's sequence, which a quarantine after its
    // drain names.
    const ObjectManagementResult assigned = MapDeviceControl(m_deviceControl.Assign(
        function, Protection::ProtectionDomainId{m_registry, driver.GetValue().GetDescriptor().Object},
        static_cast<Protection::ProtectionDomain&>(driver.GetValue().GetObject())));
    return (assigned.Error == ObjectManagementError::None)
               ? MakeResult(ObjectManagementError::None, Object::CapabilityError::Ok, {},
                            function.GetSnapshot().LastAssignmentSequence)
               : assigned;
}

namespace
{
[[nodiscard]] ObjectManagementResult MapIommuDomain(const Device::IommuDomainFailure& failure) noexcept
{
    switch (failure.Error)
    {
    case Device::IommuDomainError::None:
        return MakeResult(ObjectManagementError::None);
    case Device::IommuDomainError::InvalidMappingRequest:
    case Device::IommuDomainError::InvalidMapping:
    case Device::IommuDomainError::RamFrameRequired:
    case Device::IommuDomainError::FrameRangeExceeded:
    case Device::IommuDomainError::DmaRangeExceeded:
    case Device::IommuDomainError::DmaRangeOverlap:
    case Device::IommuDomainError::FramePolicyRejected:
    case Device::IommuDomainError::FrameNotReady:
        return MakeResult(ObjectManagementError::InvalidRequest);
    case Device::IommuDomainError::MappingCapacityExceeded:
        return MakeResult(ObjectManagementError::ResourceExhausted);
    case Device::IommuDomainError::ReservedDmaRange:
        // A firmware DMA reservation of an attached requester (point 4 of
        // Documentation/Architecture/IommuDomain.md).
        return MakeResult(ObjectManagementError::ReservedMemory);
    case Device::IommuDomainError::BackendRejected:
        // An ordinary refusal leaves nothing behind; any other faults the
        // domain, which then only quiesces.
        return MakeResult((failure.Backend == Hal::Device::IommuBackendError::CapacityExceeded)
                              ? ObjectManagementError::ResourceExhausted
                              : ((failure.Backend == Hal::Device::IommuBackendError::UnsupportedConfiguration)
                                     ? ObjectManagementError::InvalidRequest
                                     : ObjectManagementError::NotConfigured));
    default:
        return MakeResult(ObjectManagementError::NotConfigured);
    }
}
} // Namespace

// The assignee attaches its function and maps its own Frames; the mappings are what the device can
// reach.
ObjectManagementResult ObjectManager::ControlIommu(Object::CapabilitySpace& caller,
                                                   const Protection::ProtectionDomainId callerDomain,
                                                   const IommuControlRequest& request) noexcept
{
    const bool manage = (request.Operation == Abi::IommuAttach) || (request.Operation == Abi::IommuDetach);
    const Object::CapabilityOperation needed =
        manage ? Object::CapabilityOperation::Manage
               : ((request.Operation == Abi::IommuFaults) ? Object::CapabilityOperation::Inspect
                                                          : Object::CapabilityOperation::Map);
    const auto lease = caller.Lookup(request.Domain, Object::ObjectType::IommuDomain, needed);
    if (!lease.HasValue())
    {
        return MakeResult(ObjectManagementError::CapabilityRejected, lease.GetError());
    }
    // The lookup checked the type, and only IommuDomain instances report it.
    Device::IommuDomain& domain = static_cast<Device::IommuDomain&>(lease.GetValue().GetObject());
    switch (request.Operation)
    {
    case Abi::IommuAttach:
    case Abi::IommuDetach:
        return AttachIommu(caller, callerDomain, request, domain);
    case Abi::IommuMap:
        return MapIommu(caller, request, domain);
    case Abi::IommuUnmap:
        return MapIommuDomain(domain.UnmapAt(Device::DmaAddress{request.DeviceAddress}));
    case Abi::IommuFaults:
    {
        const Hal::Device::IommuBackendFaults faults = domain.GetFaults();
        return MakeResult(ObjectManagementError::None, Object::CapabilityError::Ok, {},
                          (faults.Count & 0xFFFF'FFFFU) | (static_cast<Foundation::UInt64>(faults.LastSource) << 32U) |
                              (static_cast<Foundation::UInt64>(faults.LastReason) << 48U),
                          faults.LastAddress);
    }
    default:
        return MakeResult(ObjectManagementError::InvalidRequest);
    }
}

ObjectManagementResult ObjectManager::AttachIommu(Object::CapabilitySpace& caller,
                                                  const Protection::ProtectionDomainId callerDomain,
                                                  const IommuControlRequest& request,
                                                  Device::IommuDomain& domain) noexcept
{
    const auto function =
        caller.Lookup(request.Function, Object::ObjectType::PciFunction, Object::CapabilityOperation::Invoke);
    if (!function.HasValue())
    {
        return MakeResult(ObjectManagementError::CapabilityRejected, function.GetError());
    }
    // The lookup checked the type, and only PciFunction instances report it.
    Device::PciFunction& target = static_cast<Device::PciFunction&>(function.GetValue().GetObject());
    return MapDeviceControl((request.Operation == Abi::IommuAttach)
                                ? m_deviceControl.Attach(target, callerDomain, domain)
                                : m_deviceControl.Detach(target, callerDomain, domain));
}

ObjectManagementResult ObjectManager::MapIommu(Object::CapabilitySpace& caller, const IommuControlRequest& request,
                                               Device::IommuDomain& domain) noexcept
{
    const auto frame = caller.Lookup(request.Frame, Object::ObjectType::Frame, Object::CapabilityOperation::Map);
    if (!frame.HasValue())
    {
        return MakeResult(ObjectManagementError::CapabilityRejected, frame.GetError());
    }
    Device::DmaPermissions permissions{};
    permissions = ((request.Permissions & Abi::IommuMapDeviceRead) != 0U)
                      ? permissions.With(Device::DmaPermission::Read)
                      : permissions;
    permissions = ((request.Permissions & Abi::IommuMapDeviceWrite) != 0U)
                      ? permissions.With(Device::DmaPermission::Write)
                      : permissions;
    // The lookup checked the type, and only Frame instances report it.
    const auto mapped = domain.Map(Device::IommuMappingRequest{
        .FrameStorage = &static_cast<Memory::Frame&>(frame.GetValue().GetObject()),
        .FrameIdentity = Memory::FrameId{m_registry, frame.GetValue().GetDescriptor().Object},
        .DmaStart = Device::DmaAddress{request.DeviceAddress},
        .FrameOffsetPages = Memory::PageCount{request.FramePageOffset},
        .Pages = Memory::PageCount{request.Pages},
        .Permissions = permissions,
        .Reserved = {},
    });
    // Value 0 is the device address the driver programs: the requested one behind a translating
    // IOMMU, the physical one on the trusted identity backend.
    return mapped.HasValue() ? MakeResult(ObjectManagementError::None, Object::CapabilityError::Ok, {},
                                          mapped.GetValue().DmaStart.GetValue())
                             : MapIommuDomain(mapped.GetError());
}

Abi::SystemCallResponse ObjectManager::AccessPorts(Object::CapabilitySpace& caller,
                                                   const Abi::SystemCallRequest& request) noexcept
{
    using Abi::SystemCallError;
    const auto respond = [](const SystemCallError error, const Foundation::UInt64 value = 0U) noexcept
    {
        return Abi::SystemCallResponse{.Error = error, .Value0 = value, .Value1 = 0U};
    };
    const auto processor = Platform::Synchronization::CurrentProcessor();
    if (processor.IsValid() && processor.GetValue() != 0U)
    {
        return respond(SystemCallError::InvalidConfiguration);
    }
    const Foundation::UInt64 width = request.Arguments[3U];
    const Foundation::UInt64 operation = request.Arguments[4U];
    const Foundation::UInt64 value = request.Arguments[5U];
    if (((width != 1U) && (width != 2U) && (width != 4U)) || (operation > 1U) || ((operation == 0U) && (value != 0U)) ||
        (value > 0xFFFF'FFFFU) || ((width < 4U) && (value >= (Foundation::UInt64{1U} << (width * 8U)))))
    {
        return respond(SystemCallError::InvalidArguments);
    }
    const auto lease = caller.Lookup(
        Object::CapabilityHandle{request.Arguments[0U], request.Arguments[1U]}, Object::ObjectType::DeviceRegion,
        (operation == 0U) ? Object::CapabilityOperation::Read : Object::CapabilityOperation::Write);
    if (!lease.HasValue())
    {
        // Preserve the public errors that capability Lookup can return.
        switch (lease.GetError())
        {
        case Object::CapabilityError::InvalidArgument:
            return respond(SystemCallError::InvalidArguments);
        case Object::CapabilityError::InvalidConfiguration:
            return respond(SystemCallError::InvalidConfiguration);
        case Object::CapabilityError::ResourceExhausted:
            return respond(SystemCallError::ResourceExhausted);
        case Object::CapabilityError::RightsDenied:
            return respond(SystemCallError::RightsDenied);
        case Object::CapabilityError::TypeMismatch:
            return respond(SystemCallError::TypeMismatch);
        case Object::CapabilityError::Revoked:
            return respond(SystemCallError::Revoked);
        case Object::CapabilityError::Expired:
            return respond(SystemCallError::Expired);
        case Object::CapabilityError::ObjectNotFound:
            return respond(SystemCallError::ObjectNotFound);
        default:
            return respond(SystemCallError::InvalidHandle);
        }
    }
    auto& region = static_cast<Device::DeviceRegion&>(lease.GetValue().GetObject());
    const Device::DeviceRegionDescriptor descriptor = region.GetSnapshot().Descriptor;
    if (descriptor.Kind != Device::DeviceRegionKind::PortIo)
    {
        return respond(SystemCallError::TypeMismatch);
    }
    if (m_ports == nullptr)
    {
        return respond(SystemCallError::InvalidConfiguration);
    }
    const auto retained = region.RetainRegisterAccess(
        request.Arguments[2U], static_cast<Device::DeviceAccessWidth>(width),
        (operation == 0U) ? Device::DeviceAccessOperation::Read : Device::DeviceAccessOperation::Write,
        static_cast<Foundation::UInt32>(value));
    if (retained != Device::DeviceRegionError::None)
    {
        return respond((retained == Device::DeviceRegionError::WriteValueDenied)
                           ? SystemCallError::WriteValueDenied
                           : ((retained == Device::DeviceRegionError::UnsupportedAccess)
                                  ? SystemCallError::RightsDenied
                                  : SystemCallError::InvalidArguments));
    }
    const auto port = static_cast<Foundation::UInt16>(descriptor.BaseAddress + request.Arguments[2U]);
    Foundation::UInt32 result{0U};
    if (operation == 0U)
    {
        result = m_ports->Read(port, static_cast<Foundation::UInt8>(width));
    }
    else
    {
        m_ports->Write(port, static_cast<Foundation::UInt8>(width), static_cast<Foundation::UInt32>(value));
    }
    if (region.ReleaseRegisterAccess() != Device::DeviceRegionError::None)
    {
        return respond(SystemCallError::InvalidConfiguration);
    }
    return respond(SystemCallError::None, result);
}

// The holder of an Interrupt (Manage) acknowledges its events and arms or masks it.
ObjectManagementResult ObjectManager::ControlInterrupt(Object::CapabilitySpace& caller,
                                                       const Object::CapabilityHandle interrupt,
                                                       const Foundation::UInt64 operation) noexcept
{
    const auto processor = Platform::Synchronization::CurrentProcessor();
    if (processor.IsValid() && processor.GetValue() != 0U)
    {
        return MakeResult(ObjectManagementError::NotConfigured);
    }
    const auto controlled = m_interrupts.Control(caller, interrupt, operation);
    if (controlled.HasValue())
    {
        return MakeResult(ObjectManagementError::None, Object::CapabilityError::Ok, {}, controlled.GetValue());
    }
    // An unconfigured pool has no Interrupt the lookup could find; a faulted
    // Interrupt is reported, not fatal.
    return (controlled.GetError().Error == Object::ObjectPoolError::InvalidRequest)
               ? MakeResult(ObjectManagementError::InvalidRequest)
               : MapPoolFailure(controlled.GetError());
}

bool ObjectManager::Collect() noexcept
{
    return CollectPools() && ScrubOneRun();
}

bool ObjectManager::CollectPools() noexcept
{
    // A WaitSet's destruction drops its registrations, which pin destroyed Endpoints' readiness
    // Notifications, so WaitSets go before Endpoints. ResourceDomains go last: everything charged
    // to one pins it.
    Foundation::UInt64 reclaimed{0U};
    const bool collected =
        FoldCollected(m_threads.Collect(), reclaimed) && FoldCollected(m_iommuDomains.Collect(), reclaimed) &&
        FoldCollected(m_waitSets.Collect(), reclaimed) && FoldCollected(m_interrupts.Collect(), reclaimed) &&
        FoldCollected(m_endpoints.Collect(), reclaimed) && FoldCollected(m_domains.Collect(), reclaimed) &&
        FoldCollected(m_faultChannels.Collect(), reclaimed) && FoldCollected(m_spaces.Collect(), reclaimed) &&
        FoldCollected(m_addressSpaces.Collect(), reclaimed) && FoldCollected(m_frames.Collect(), reclaimed) &&
        FoldCollected(m_memoryObjects.Collect(), reclaimed) && FoldCollected(m_resourceDomains.Collect(), reclaimed);
    __atomic_fetch_add(&m_collection.Collections, 1U, __ATOMIC_RELAXED);
    __atomic_fetch_add(&m_collection.Reclaimed, reclaimed, __ATOMIC_RELAXED);
    return collected;
}

ObjectCollectionSnapshot ObjectManager::GetCollectionSnapshot() const noexcept
{
    return {__atomic_load_n(&m_collection.Collections, __ATOMIC_ACQUIRE),
            __atomic_load_n(&m_collection.Reclaimed, __ATOMIC_ACQUIRE),
            __atomic_load_n(&m_collection.ScrubRuns, __ATOMIC_ACQUIRE),
            __atomic_load_n(&m_collection.ScrubbedPages, __ATOMIC_ACQUIRE)};
}

bool ObjectManager::ScrubRuns(const Foundation::Size runs) noexcept
{
    for (Foundation::Size run{0U}; run < runs; ++run)
    {
        const Foundation::UInt64 dirty = m_framePool.GetSnapshot().DirtyPages;
        if (dirty == 0U)
        {
            break;
        }
        if (!ScrubOneRun())
        {
            return false;
        }
        // No scrubber, or the scrub deferred: the pages wait for a later call.
        if (m_framePool.GetSnapshot().DirtyPages >= dirty)
        {
            break;
        }
    }
    return true;
}

bool ObjectManager::ScrubOneRun() noexcept
{
    if (m_scrubber == nullptr)
    {
        return true;
    }
    // Bound each collection scrub visit to 16 pages.
    const auto claimed = m_framePool.BeginScrub(Memory::PhysicalPageAllocator::MaximumScrubPages);
    if (!claimed.HasValue())
    {
        // Nothing dirty, or no frame pool.
        return (claimed.GetError() == Memory::FramePoolError::Exhausted) ||
               (claimed.GetError() == Memory::FramePoolError::NotReady);
    }
    const Memory::FramePoolRun run = claimed.GetValue();
    switch (m_scrubber->Zero(run.PhysicalStart, run.Pages))
    {
    case Memory::FrameScrubError::None:
        __atomic_fetch_add(&m_collection.ScrubRuns, 1U, __ATOMIC_RELAXED);
        __atomic_fetch_add(&m_collection.ScrubbedPages, run.Pages, __ATOMIC_RELAXED);
        return m_framePool.EndScrub(run.PhysicalStart, run.Pages) == Memory::FramePoolError::None;
    case Memory::FrameScrubError::Deferred:
        return m_framePool.AbortScrub(run.PhysicalStart, run.Pages) == Memory::FramePoolError::None;
    case Memory::FrameScrubError::Faulted:
        break;
    }
    // A run the scrubber may have half written stays claimed: never reused.
    return false;
}

Ipc::EndpointPoolSnapshot ObjectManager::GetEndpointSnapshot() const noexcept
{
    return m_endpoints.GetSnapshot();
}

Memory::FrameObjectPoolSnapshot ObjectManager::GetFrameSnapshot() const noexcept
{
    return m_frames.GetSnapshot();
}

Memory::MemoryObjectPoolSnapshot ObjectManager::GetMemoryObjectSnapshot() const noexcept
{
    return m_memoryObjects.GetSnapshot();
}

Memory::FramePoolSnapshot ObjectManager::GetFramePoolSnapshot() const noexcept
{
    return m_framePool.GetSnapshot();
}
} // Namespace Asteria::Kernel::System
