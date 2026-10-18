// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_SYSTEM_OBJECT_MANAGER_HPP_INCLUDED
#define ASTERIA_KERNEL_SYSTEM_OBJECT_MANAGER_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Kernel/Memory/FaultChannelPool.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Ipc/EndpointPool.hpp"
#include "Kernel/Ipc/WaitSetPool.hpp"
#include "Kernel/Memory/AddressSpacePool.hpp"
#include "Kernel/Memory/FrameObjectPool.hpp"
#include "Kernel/Memory/FramePool.hpp"
#include "Kernel/Memory/IFrameScrubber.hpp"
#include "Kernel/Memory/MemoryMapper.hpp"
#include "Kernel/Memory/MemoryObjectPool.hpp"
#include "Kernel/Object/CapabilitySpacePool.hpp"
#include "Kernel/Protection/ProtectionDomainPool.hpp"
#include "Kernel/Device/IommuDomainPool.hpp"
#include "Hal/IPowerControl.hpp"
#include "Kernel/Device/PciFunctionControl.hpp"
#include "Kernel/Interrupt/InterruptPool.hpp"
#include "Hal/Device/IPortIo.hpp"
#include "Kernel/Resource/ResourceDomainPool.hpp"
#include "Kernel/Scheduler/ThreadPool.hpp"
#include "Kernel/System/IObjectManagement.hpp"

namespace Asteria::Kernel::System
{
/// Who may create an IommuDomain once the pool has its backends: every caller
/// behind a translating IOMMU; only the assignee of a function granted
/// trusted DMA on the trusted-driver profile's identity backends.
enum class IommuAdmission : Foundation::UInt8
{
    Every,
    TrustedAssignees,
};

/// Owns the pools of objects user code creates through ObjectCreate and destroys through
/// ObjectDestroy: Endpoints, Frames drawn from the frame pool, MemoryObjects, Threads started
/// through the platform's native slots, CapabilitySpaces once the boot gives their keys
/// (ConfigureSpaces), AddressSpaces once it gives their provider (ConfigureAddressSpaces) and
/// ProtectionDomains once it gives the records of created domains (ConfigureDomains). Until its
/// owner hands it a frame pool (InitializeFramePool), Frame creation is exhausted; until it hands
/// it the shootdown collaborators (ConfigureMapping), MemoryMap is not configured; until it hands
/// it a scrubber (ConfigureScrubber), a destroyed Frame's pages stay dirty. Each call first
/// collects the pools' quiescing objects that are no longer referenced, and ObjectDestroy collects
/// again once the object is quiescing, so a destroyed object nothing else references returns its
/// storage and charge before the call returns; one still referenced returns at a later management
/// call or when the owner calls Collect. What the collections did: the pool passes, the slots they
/// reclaimed, and the dirty frame-pool runs zeroed and their pages.
struct ObjectCollectionSnapshot
{
    Foundation::UInt64 Collections;
    Foundation::UInt64 Reclaimed;
    Foundation::UInt64 ScrubRuns;
    Foundation::UInt64 ScrubbedPages;
};

class ObjectManager final : public IObjectManagement
{
public:
    /// Preconditions:
    /// - issuer is bound to objectRegistry, and both outlive this manager;
    /// - callers prevent same-CPU reentry through the shared registry lock.
    constexpr ObjectManager(Object::ObjectRegistry& objectRegistry, Object::RootCapabilityIssuer& issuer) noexcept
        : m_registry{objectRegistry}, m_endpoints{objectRegistry, issuer}, m_waitSets{objectRegistry, issuer},
          m_frames{objectRegistry, issuer, m_framePool}, m_memoryObjects{objectRegistry, issuer},
          m_threads{objectRegistry, issuer}, m_spaces{objectRegistry, issuer}, m_addressSpaces{objectRegistry, issuer},
          m_domains{objectRegistry, issuer}, m_faultChannels{objectRegistry, issuer},
          m_resourceDomains{objectRegistry, issuer}, m_iommuDomains{objectRegistry, issuer},
          m_interrupts{objectRegistry, issuer},
          m_mappers{Memory::MemoryMapper{objectRegistry, &m_frames}, Memory::MemoryMapper{objectRegistry, &m_frames},
                    Memory::MemoryMapper{objectRegistry, &m_frames}, Memory::MemoryMapper{objectRegistry, &m_frames}}
    {
    }

    /// Gives the Frame and MemoryObject pools their slots: frameSlab and memoryObjectSlab, each its
    /// pool's SlabBytes aligned to its SlabAlignment, in the metadata the platform placed; once
    /// only. Until then both creations are exhausted.
    [[nodiscard]] bool AttachSlabs(void* frameSlab, void* memoryObjectSlab) noexcept
    {
        return m_frames.AttachSlab(frameSlab) && m_memoryObjects.AttachSlab(memoryObjectSlab);
    }

    /// Gives the Frame pool its pages: pages zeroed pages from physicalStart.
    [[nodiscard]] Memory::FramePoolError InitializeFramePool(Memory::PhysicalAddress physicalStart,
                                                             Foundation::UInt64 pages) noexcept
    {
        return m_framePool.Initialize(physicalStart, pages);
    }

    /// Boot imports after all DMA handoff outcomes.
    [[nodiscard]] Memory::PhysicalPageAllocator& GetPhysicalPageAllocator() noexcept
    {
        return m_framePool.GetAllocator();
    }

    /// Gives the thread pool the platform's native slots; once only. Until then Thread creation is
    /// exhausted.
    [[nodiscard]] bool ConfigureThreads(Scheduler::IUserThreadBackend& backend) noexcept
    {
        return m_threads.Configure(backend);
    }

    /// Gives the CapabilitySpace pool the collaborators created spaces share with the boot spaces
    /// and its slots' keys; once only.
    [[nodiscard]] bool
    ConfigureSpaces(Object::RevocationRegistry& revocationRegistry, Time::IMonotonicClock& clock,
                    const Object::CapabilityValidationKey (&keys)[Object::CapabilitySpacePool::Capacity]) noexcept
    {
        return m_spaces.Configure(revocationRegistry, clock, keys);
    }

    /// Gives the AddressSpace pool the platform's prepared roots; once only.
    [[nodiscard]] bool ConfigureAddressSpaces(Memory::IAddressSpaceProvider& provider) noexcept
    {
        return m_addressSpaces.Configure(provider);
    }

    /// Gives the ProtectionDomain pool the platform's records of created domains; once only.
    [[nodiscard]] bool ConfigureDomains(Protection::ICreatedDomainRecords& records) noexcept
    {
        return m_domains.Configure(records);
    }

    /// Lets DeviceControl write the Command register of assigned PCI functions through reader and
    /// writer; once only.
    [[nodiscard]] bool ConfigureDeviceControl(Hal::Device::IPciConfigurationSpace& reader,
                                              Hal::Device::IPciConfigurationWriter& writer) noexcept
    {
        return m_deviceControl.Configure(reader, writer);
    }

    /// The platform's observer of the parked Frame pins confirmations release, or null.
    void SetParkedPinObserver(Device::IParkedPinObserver* const observer) noexcept
    {
        m_deviceControl.SetParkedPinObserver(observer);
    }

    /// Gives the IommuDomain pool the platform's slots and backends; once only. Until then
    /// ObjectCreate of an IommuDomain is not configured. With TrustedAssignees (the trusted-driver
    /// profile's identity backends, Documentation/Architecture/IommuDomain.md ) ObjectCreate stays
    /// not configured for every caller but the assignee of a function granted trusted DMA, as it is
    /// without an IOMMU. reservations are the firmware DMA ranges every IommuDomain enforces
    /// (Device::IDmaReservations), or null where the platform has none.
    [[nodiscard]] bool
    ConfigureIommu(Device::IommuDomainSlot (&slots)[Device::IommuDomainPool::Capacity],
                   Hal::Device::IIommuDomainBackend* const (&backends)[Device::IommuDomainPool::Capacity],
                   const IommuAdmission admission = IommuAdmission::Every,
                   const Device::IDmaReservations* const reservations = nullptr) noexcept
    {
        if (!m_iommuDomains.Configure(slots, backends, reservations))
        {
            return false;
        }
        m_iommuAdmission = admission;
        return true;
    }

    void ConfigurePower(Hal::IPowerControl& power, Object::ObjectId manager) noexcept
    {
        m_power = &power;
        m_powerManager = manager;
    }

    void ConfigurePorts(Hal::Device::IPortIo& ports) noexcept
    {
        m_ports = &ports;
    }
    [[nodiscard]] Abi::SystemCallResponse AccessPorts(Object::CapabilitySpace& caller,
                                                      const Abi::SystemCallRequest& request) noexcept override;

    /// Gives the Interrupt pool the platform's message vectors; once only, after
    /// ConfigureDeviceControl. Until then ObjectCreate of an Interrupt is not configured.
    [[nodiscard]] bool ConfigureInterrupts(Hal::Interrupt::IMessageVectors& vectors) noexcept
    {
        return m_interrupts.Configure(vectors, m_deviceControl);
    }

    /// Also available with ConfigureInterrupts; on a platform whose drivers have no MSI: an
    /// Interrupt takes its function's INTx line through lines; once only, after
    /// ConfigureDeviceControl.
    [[nodiscard]] bool ConfigureInterruptLines(Hal::Interrupt::IInterruptLines& lines) noexcept
    {
        return m_interrupts.ConfigureLines(lines, m_deviceControl);
    }

    /// Lets user code create child ResourceDomains in the tree of ledger, with identities from
    /// firstIdentity on; until then type 6 is not configured.
    [[nodiscard]] bool ConfigureResourceDomains(Resource::ResourceDomainLedger& ledger,
                                                const Object::ResourceDomainId firstIdentity) noexcept
    {
        return m_resourceDomains.Configure(ledger, firstIdentity);
    }

    /// Gives Collect the platform's scrubber, through which it zeroes dirty frame-pool runs and
    /// frees them; once only.
    [[nodiscard]] bool ConfigureScrubber(Memory::IFrameScrubber& scrubber) noexcept
    {
        if (m_scrubber != nullptr)
        {
            return false;
        }
        if (!m_frames.ConfigureScrubber(scrubber))
        {
            return false;
        }
        m_scrubber = &scrubber;
        return true;
    }

    void EnableCreationZeroProbe() noexcept
    {
        m_frames.EnableCreationZeroProbe();
    }

    /// Gives MemoryMap the processor's shootdown dispatcher, its transport and a progress wait;
    /// once only. Preconditions: as Memory::MemoryMapper's.
    [[nodiscard]] bool ConfigureMapping(Memory::TlbShootdownDispatcher& shootdown,
                                        Hal::Memory::ITlbShootdownTransport& transport,
                                        Memory::IPagerShootdownProgress& progress) noexcept
    {
        for (auto& mapper : m_mappers)
        {
            if (!mapper.Configure(shootdown, transport, progress))
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool ConfigureFaultChannels(Object::RevocationRegistry& revocations, Time::IMonotonicClock& clock,
                                              const Object::CapabilityValidationKey* keys,
                                              Scheduler::ProcessorSchedulerWakeFacade& wake,
                                              Memory::TlbShootdownDispatcher& shootdown,
                                              Hal::Memory::ITlbShootdownTransport& transport,
                                              Memory::IPagerShootdownProgress& progress) noexcept
    {
        return m_faultChannels.Configure(revocations, clock, keys, wake, shootdown, transport, progress);
    }
    [[nodiscard]] Memory::FaultChannelMetrics GetFaultChannelMetrics(Foundation::Size index) const noexcept
    {
        return m_faultChannels.GetMetrics(index);
    }
    [[nodiscard]] Foundation::UInt64 GetTagRefusals() const noexcept
    {
        return m_domains.GetTagRefusals();
    }
    [[nodiscard]] Foundation::UInt64 GetCodeFlagRefusals() const noexcept
    {
        return __atomic_load_n(&m_codeFlagRefusals, __ATOMIC_ACQUIRE);
    }
    [[nodiscard]] Foundation::UInt64 GetTypeLimitRefusals(Foundation::Size index) const noexcept
    {
        return index < Abi::LimitedObjectCount ? __atomic_load_n(&m_typeLimitRefusals[index], __ATOMIC_ACQUIRE) : 0U;
    }
    [[nodiscard]] bool ProgressFaultChannels() noexcept
    {
        return m_faultChannels.Progress();
    }
    [[nodiscard]] Memory::FaultChannelPoolSnapshot GetFaultChannelSnapshot() const noexcept
    {
        return m_faultChannels.GetSnapshot();
    }
    ObjectManager(const ObjectManager&) = delete;
    ObjectManager& operator=(const ObjectManager&) = delete;
    ObjectManager(ObjectManager&&) = delete;
    ObjectManager& operator=(ObjectManager&&) = delete;

    [[nodiscard]] ObjectManagementResult Create(Object::CapabilitySpace& caller,
                                                const ObjectCreateRequest& request) noexcept override;

    [[nodiscard]] ObjectManagementResult Destroy(Object::CapabilitySpace& caller, Object::CapabilityHandle object,
                                                 Object::ObjectType type) noexcept override;

    [[nodiscard]] ObjectManagementResult Map(Object::CapabilitySpace& caller,
                                             const Memory::MemoryMapRequest& request) noexcept override;

    /// A successful unmap collects, so a Frame or MemoryObject destroyed
    /// while it was mapped returns before the call does.
    [[nodiscard]] ObjectManagementResult Unmap(Object::CapabilitySpace& caller, Object::CapabilityHandle space,
                                               Memory::VirtualAddress virtualStart,
                                               Memory::PageCount pages) noexcept override;

    [[nodiscard]] ObjectManagementResult Terminate(Object::CapabilitySpace& caller,
                                                   Object::CapabilityHandle thread) noexcept override;
    [[nodiscard]] ObjectManagementResult BindWaitSet(Object::CapabilitySpace& caller, Object::CapabilityHandle waitSet,
                                                     Object::CapabilityHandle endpoint,
                                                     Foundation::UInt64 tag) noexcept override;
    [[nodiscard]] ObjectManagementResult ControlDevice(Object::CapabilitySpace& caller,
                                                       Protection::ProtectionDomainId callerDomain,
                                                       Object::CapabilityHandle function, Foundation::UInt64 operation,
                                                       Object::CapabilityHandle domain) noexcept override;

    [[nodiscard]] ObjectManagementResult ControlIommu(Object::CapabilitySpace& caller,
                                                      Protection::ProtectionDomainId callerDomain,
                                                      const IommuControlRequest& request) noexcept override;
    [[nodiscard]] ObjectManagementResult ControlInterrupt(Object::CapabilitySpace& caller,
                                                          Object::CapabilityHandle interrupt,
                                                          Foundation::UInt64 operation) noexcept override;

    /// Reclaims every unreferenced quiescing object, then zeroes and frees at
    /// most one released dirty run of MaximumScrubPages through the scrubber
    /// (a deferred scrub leaves it dirty); false if a pool or the scrubber
    /// faulted.
    [[nodiscard]] bool Collect() noexcept;
    [[nodiscard]] ObjectCollectionSnapshot GetCollectionSnapshot() const noexcept;

    [[nodiscard]] Ipc::EndpointPoolSnapshot GetEndpointSnapshot() const noexcept;
    [[nodiscard]] Ipc::WaitSetPoolSnapshot GetWaitSetSnapshot() const noexcept
    {
        return m_waitSets.GetSnapshot();
    }
    /// The Endpoints WaitSetBind has bound.
    // One bounded scrub in the caller's idle root.
    [[nodiscard]] bool ScrubIdle() noexcept
    {
        return ScrubRuns(1U);
    }

    [[nodiscard]] Foundation::UInt64 GetBinds() const noexcept
    {
        return __atomic_load_n(&m_binds, __ATOMIC_ACQUIRE);
    }
    [[nodiscard]] Memory::FrameObjectPoolSnapshot GetFrameSnapshot() const noexcept;
    [[nodiscard]] Memory::MemoryObjectPoolSnapshot GetMemoryObjectSnapshot() const noexcept;
    [[nodiscard]] Memory::FramePoolSnapshot GetFramePoolSnapshot() const noexcept;
    [[nodiscard]] Foundation::UInt64 GetAllocatablePages() const noexcept
    {
        return m_framePool.GetAllocator().GetSnapshot().AllocatablePages;
    }
    [[nodiscard]] Memory::PhysicalPageAllocatorSnapshot GetAllocatorSnapshot() const noexcept
    {
        return m_framePool.GetAllocator().GetSnapshot();
    }
    [[nodiscard]] Scheduler::ThreadPoolSnapshot GetThreadSnapshot() const noexcept
    {
        return m_threads.GetSnapshot();
    }
    [[nodiscard]] Memory::MemoryMapperSnapshot GetMapperSnapshot() const noexcept
    {
        auto snapshot = LocalMapper().GetSnapshot();
        snapshot.Mappings = 0U;
        snapshot.Unmappings = 0U;
        for (const auto& mapper : m_mappers)
        {
            snapshot.Mappings += mapper.GetMappings();
            snapshot.Unmappings += mapper.GetUnmappings();
        }
        return snapshot;
    }
    [[nodiscard]] Object::CapabilitySpacePoolSnapshot GetSpaceSnapshot() const noexcept
    {
        return m_spaces.GetSnapshot();
    }
    [[nodiscard]] Memory::AddressSpacePoolSnapshot GetAddressSpaceSnapshot() const noexcept
    {
        return m_addressSpaces.GetSnapshot();
    }
    [[nodiscard]] Protection::ProtectionDomainPoolSnapshot GetDomainSnapshot() const noexcept
    {
        return m_domains.GetSnapshot();
    }
    [[nodiscard]] Resource::ResourceDomainPoolSnapshot GetResourceDomainSnapshot() const noexcept
    {
        return m_resourceDomains.GetSnapshot();
    }
    [[nodiscard]] Device::PciFunctionControlSnapshot GetDeviceControlSnapshot() const noexcept
    {
        return m_deviceControl.GetSnapshot();
    }
    [[nodiscard]] Device::IommuDomainPoolSnapshot GetIommuDomainSnapshot() const noexcept
    {
        return m_iommuDomains.GetSnapshot();
    }
    [[nodiscard]] Interrupt::InterruptPoolSnapshot GetInterruptSnapshot() const noexcept
    {
        return m_interrupts.GetSnapshot();
    }

private:
    using CreationResult = Foundation::Result<Object::CapabilityHandle, Object::ObjectPoolFailure>;

    [[nodiscard]] CreationResult CreatePooled(Object::CapabilitySpace& caller,
                                              const ObjectCreateRequest& request) noexcept;
    [[nodiscard, gnu::noinline]] CreationResult CreateEndpoint(Object::CapabilitySpace& caller,
                                                               const ObjectCreateRequest& request) noexcept;
    [[nodiscard, gnu::noinline]] CreationResult CreateThread(Object::CapabilitySpace& caller,
                                                             const ObjectCreateRequest& request) noexcept;
    [[nodiscard, gnu::noinline]] CreationResult CreateWaitSet(Object::CapabilitySpace& caller,
                                                              const ObjectCreateRequest& request) noexcept;
    [[nodiscard, gnu::noinline]] CreationResult CreateSpace(Object::CapabilitySpace& caller,
                                                            const ObjectCreateRequest& request) noexcept;
    [[nodiscard, gnu::noinline]] CreationResult CreateAddressSpace(Object::CapabilitySpace& caller,
                                                                   const ObjectCreateRequest& request) noexcept;
    [[nodiscard, gnu::noinline]] CreationResult CreateDomain(Object::CapabilitySpace& caller,
                                                             const ObjectCreateRequest& request) noexcept;
    [[nodiscard, gnu::noinline]] CreationResult CreateResourceDomain(Object::CapabilitySpace& caller,
                                                                     const ObjectCreateRequest& request) noexcept;
    /// ObjectDestroy of the types that have no exit code (all but Thread).
    [[nodiscard, gnu::noinline]] ObjectManagementResult
    DestroyPooled(Object::CapabilitySpace& caller, Object::CapabilityHandle object, Object::ObjectType type) noexcept;
    /// ObjectDestroy of a ResourceDomain: drains the child object names and its descendants.
    [[nodiscard, gnu::noinline]] ObjectManagementResult DestroyResourceDomain(Object::CapabilitySpace& caller,
                                                                              Object::CapabilityHandle object) noexcept;
    /// WaitSetBind's source: an Endpoint's readiness (Read) or an
    /// Interrupt's Notification (Wait).
    [[nodiscard, gnu::noinline]] ObjectManagementResult
    FindBindReadiness(Object::CapabilitySpace& caller, Object::CapabilityHandle source, Ipc::NotificationId& identity,
                      Ipc::Notification*& readiness, Object::ObjectPin& pin) noexcept;
    [[nodiscard, gnu::noinline]] CreationResult CreateIommuDomain(Object::CapabilitySpace& caller,
                                                                  const ObjectCreateRequest& request) noexcept;
    [[nodiscard, gnu::noinline]] CreationResult CreateInterrupt(Object::CapabilitySpace& caller,
                                                                const ObjectCreateRequest& request) noexcept;
    /// IommuAttach and IommuDetach of the function request names.
    [[nodiscard, gnu::noinline]] ObjectManagementResult AttachIommu(Object::CapabilitySpace& caller,
                                                                    Protection::ProtectionDomainId callerDomain,
                                                                    const IommuControlRequest& request,
                                                                    Device::IommuDomain& domain) noexcept;
    /// IommuMap of the Frame request names.
    [[nodiscard, gnu::noinline]] ObjectManagementResult
    MapIommu(Object::CapabilitySpace& caller, const IommuControlRequest& request, Device::IommuDomain& domain) noexcept;
    /// DeviceAssign: the domain (Manage) domain names gets the function.
    [[nodiscard, gnu::noinline]] ObjectManagementResult AssignDevice(Object::CapabilitySpace& caller,
                                                                     Device::PciFunction& function,
                                                                     Object::CapabilityHandle domain) noexcept;
    [[nodiscard, gnu::noinline]] CreationResult CreateFrame(Object::CapabilitySpace& caller,
                                                            const ObjectCreateRequest& request) noexcept;
    [[nodiscard, gnu::noinline]] CreationResult CreateMemoryObject(Object::CapabilitySpace& caller,
                                                                   const ObjectCreateRequest& request) noexcept;
    /// Zeroes and frees one dirty run, if there is one and a scrubber.
    [[nodiscard]] bool ScrubOneRun() noexcept;
    /// Zeroes and frees dirty runs until none is left, at most runs of them.
    [[nodiscard, gnu::noinline]] bool ScrubRuns(Foundation::Size runs) noexcept;
    /// Collect without the scrub: what a drain does between its phases,
    /// which need the earlier phases' objects gone, not clean pages.
    [[nodiscard]] bool CollectPools() noexcept;

    Foundation::UInt64 m_codeFlagRefusals{0U};
    Foundation::UInt64 m_typeLimitRefusals[Abi::LimitedObjectCount]{};
    Object::ObjectRegistry& m_registry;
    Memory::FramePool m_framePool;
    Ipc::EndpointPool m_endpoints;
    Ipc::WaitSetPool m_waitSets;
    Memory::FrameObjectPool m_frames;
    Memory::MemoryObjectPool m_memoryObjects;
    Scheduler::ThreadPool m_threads;
    Object::CapabilitySpacePool m_spaces;
    Memory::AddressSpacePool m_addressSpaces;
    Protection::ProtectionDomainPool m_domains;
    Memory::FaultChannelPool m_faultChannels;
    Resource::ResourceDomainPool m_resourceDomains;
    Device::IommuDomainPool m_iommuDomains;
    IommuAdmission m_iommuAdmission{IommuAdmission::Every};
    Interrupt::InterruptPool m_interrupts;
    Hal::Device::IPortIo* m_ports{nullptr};
    Device::PciFunctionControl m_deviceControl;
    Hal::IPowerControl* m_power{nullptr};
    Object::ObjectId m_powerManager{};
    // The wait primitive cannot switch this continuation; other CPUs
    // may map simultaneously using their own preparation/pins.
    Memory::MemoryMapper m_mappers[Scheduler::MaximumProcessorCount];
    [[nodiscard]] Memory::MemoryMapper& LocalMapper() noexcept
    {
        return m_mappers[ProcessorIndex()];
    }
    [[nodiscard]] const Memory::MemoryMapper& LocalMapper() const noexcept
    {
        return m_mappers[ProcessorIndex()];
    }
    [[nodiscard]] static Foundation::Size ProcessorIndex() noexcept
    {
        const auto processor = Platform::Synchronization::CurrentProcessor();
        return processor.IsValid() ? processor.GetValue() : 0U;
    }
    static_assert(Scheduler::MaximumProcessorCount == 4U);
    Memory::IFrameScrubber* m_scrubber{nullptr};
    Foundation::UInt64 m_binds{0U};
    ObjectCollectionSnapshot m_collection{};
};
} // Namespace Asteria::Kernel::System

#endif // ASTERIA_KERNEL_SYSTEM_OBJECT_MANAGER_HPP_INCLUDED
