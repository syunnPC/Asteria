// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <thread>

#include "Hal/Memory/ITlbShootdownTransport.hpp"
#include "Hal/Memory/ITlbShootdownNotifier.hpp"
#include "Kernel/Device/DeviceRegion.hpp"
#include "Kernel/Device/IommuDomain.hpp"
#include "Kernel/Memory/AddressSpace.hpp"
#include "Kernel/Memory/ProcessorAddressSpaceController.hpp"
#include "Kernel/Memory/DirectUserMemoryAccess.hpp"
#include "Kernel/Memory/RecordUserMemoryAccess.hpp"
#include "Kernel/Memory/TlbShootdownDrain.hpp"
#include "Kernel/Memory/TlbShootdownDispatcher.hpp"
#include "Kernel/Memory/TlbShootdownMailboxTransport.hpp"
#include "Kernel/Scheduler/CrossProcessorWait.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"

#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
using Hal::Device::IIommuDevice;
using Hal::Device::IIommuDomainBackend;
using Hal::Device::IommuBackendError;
using Hal::Device::IommuBackendMapRequest;
using Hal::Device::IommuBackendOwnershipError;
using Hal::Memory::AddressSpacePageTableError;
using Hal::Memory::AddressSpacePageTableActivationError;
using Hal::Memory::AddressSpacePageTableMapResult;
using Hal::Memory::AddressSpacePageTableOwnershipError;
using Hal::Memory::IAddressSpacePageTable;
using Hal::Memory::ILocalTlbInvalidator;
using Hal::Memory::ITlbShootdownNotifier;
using Hal::Memory::ITlbShootdownTransport;
using Hal::Memory::LocalTlbInvalidationError;
using Hal::Memory::TlbShootdownTransportError;
using Kernel::Device::DeviceAccessWidth;
using Kernel::Device::DeviceAccessWidths;
using Kernel::Device::DeviceRegion;
using Kernel::Device::DeviceRegionDescriptor;
using Kernel::Device::DeviceRegionError;
using Kernel::Device::DeviceRegionId;
using Kernel::Device::DeviceRegionKind;
using Kernel::Device::DeviceRegionPermission;
using Kernel::Device::DeviceRegionPermissions;
using Kernel::Device::DmaAddress;
using Kernel::Device::DmaPermission;
using Kernel::Device::DmaPermissions;
using Kernel::Device::IommuDomain;
using Kernel::Device::IommuDomainConfiguration;
using Kernel::Device::IommuDomainError;
using Kernel::Device::IommuDomainFailure;
using Kernel::Device::IommuMappingRequest;
using Kernel::Memory::AddressSpace;
using Kernel::Memory::AddressSpaceError;
using Kernel::Memory::AddressSpaceId;
using Kernel::Memory::AddressSpaceMappingRecord;
using Kernel::Memory::AddressSpaceState;
using Kernel::Memory::FaultAccess;
using Kernel::Memory::FaultCause;
using Kernel::Memory::FaultFrameMappingError;
using Kernel::Memory::FaultFrameMappingPreparation;
using Kernel::Memory::FaultFrameMappingRequest;
using Kernel::Memory::FaultPrivilege;
using Kernel::Memory::FaultRecord;
using Kernel::Memory::FaultResolutionClaimToken;
using Kernel::Memory::FaultResolutionKind;
using Kernel::Memory::FaultToken;
using Kernel::Memory::FaultTokenError;
using Kernel::Memory::BootFrame;
using Kernel::Memory::Frame;
using Kernel::Memory::FrameDescriptor;
using Kernel::Memory::FrameError;
using Kernel::Memory::FrameId;
using Kernel::Memory::FrameKind;
using Kernel::Memory::InitialMapping;
using Kernel::Memory::InitialMappingOwner;
using Kernel::Memory::InitialMappingPlan;
using Kernel::Memory::InitialMappingPlanError;
using Kernel::Memory::MappingPermission;
using Kernel::Memory::MappingPermissions;
using Kernel::Memory::MemoryCachePolicy;
using Kernel::Memory::MemoryCommitment;
using Kernel::Memory::MemoryObject;
using Kernel::Memory::MemoryObjectDescriptor;
using Kernel::Memory::MemoryObjectError;
using Kernel::Memory::MemoryObjectId;
using Kernel::Memory::PageCount;
using Kernel::Memory::PagePermission;
using Kernel::Memory::PagePermissions;
using Kernel::Memory::PhysicalAddress;
using Kernel::Memory::PinnedFrameMappingError;
using Kernel::Memory::PinnedFrameMappingPreparation;
using Kernel::Memory::PinnedFrameMappingRequest;
using Kernel::Memory::TlbShootdownBatch;
using Kernel::Memory::TlbShootdownDrain;
using Kernel::Memory::TlbShootdownDispatcher;
using Kernel::Memory::TlbShootdownError;
using Kernel::Memory::TlbShootdownMailboxSlotCount;
using Kernel::Memory::TlbShootdownMailboxTransport;
using Kernel::Memory::TlbShootdownReceiveError;
using Kernel::Memory::TlbShootdownWork;
using Kernel::Memory::VirtualAddress;
using Kernel::Object::IKernelObject;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectRegistryError;
using Kernel::Object::ObjectType;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Resource::ResourceDomain;
using Kernel::Resource::ResourceDomainError;
using Kernel::Resource::ResourceDomainLedger;
using Kernel::Resource::ResourceDomainLimits;
using Kernel::Resource::ResourceDomainObjectId;
using Kernel::Scheduler::CpuId;
using Kernel::Scheduler::ThreadId;
using Platform::Synchronization::SpinLock;

class TestKernelObject final : public IKernelObject
{
public:
    constexpr TestKernelObject(const ObjectType type, const ResourceDomainId chargeOwner) noexcept
        : m_type{type}, m_chargeOwner{chargeOwner}
    {
    }

    [[nodiscard]] ObjectType GetObjectType() const noexcept override
    {
        return m_type;
    }

    [[nodiscard]] ResourceDomainId GetChargeOwner() const noexcept override
    {
        return m_chargeOwner;
    }

private:
    ObjectType m_type;
    ResourceDomainId m_chargeOwner;
};

enum class PageTableResultMode : Foundation::UInt8
{
    Exact,
    WrongStart,
    WrongPages,
    Reserved,
};

class TestPageTable final : public IAddressSpacePageTable
{
public:
    [[nodiscard]] bool IsReady() const noexcept override
    {
        return Ready;
    }

    [[nodiscard]] AddressSpacePageTableOwnershipError Claim(const AddressSpace& owner,
                                                            const InitialMappingPlan& seedPlan) noexcept override
    {
        ++ClaimCalls;
        if (!Ready)
        {
            return AddressSpacePageTableOwnershipError::NotReady;
        }
        if (Owner != nullptr)
        {
            return AddressSpacePageTableOwnershipError::AlreadyOwned;
        }
        Owner = &owner;
        SeedPlan = &seedPlan;
        return AddressSpacePageTableOwnershipError::None;
    }

    [[nodiscard]] AddressSpacePageTableOwnershipError Release(const AddressSpace& owner) noexcept override
    {
        ++ReleaseCalls;
        if (Owner != &owner)
        {
            return AddressSpacePageTableOwnershipError::InvalidOwner;
        }
        Owner = nullptr;
        return AddressSpacePageTableOwnershipError::None;
    }

    [[nodiscard]] AddressSpacePageTableActivationError Activate(const AddressSpace& owner,
                                                                const CpuId processor) noexcept override
    {
        ++ActivateCalls;
        LastActivationOwner = &owner;
        LastActivationProcessor = processor;
        if (Owner != &owner)
        {
            return AddressSpacePageTableActivationError::InvalidOwner;
        }
        return NextActivationError;
    }

    [[nodiscard]] AddressSpacePageTableError Unmap(const AddressSpaceMappingRecord& mapping,
                                                   AddressSpacePageTableMapResult& output) noexcept override
    {
        ++UnmapCalls;
        LastUnmapping = mapping;
        if (NextUnmapError != AddressSpacePageTableError::None)
        {
            return NextUnmapError;
        }
        output = AddressSpacePageTableMapResult{
            .InvalidationStart = mapping.VirtualStart,
            .InvalidationPages = mapping.Pages,
            .Reserved = 0U,
        };
        --InstalledMappings;
        return AddressSpacePageTableError::None;
    }

    [[nodiscard]] AddressSpacePageTableError Map(const AddressSpaceMappingRecord& mapping,
                                                 AddressSpacePageTableMapResult& output) noexcept override
    {
        ++MapCalls;
        LastMapping = mapping;
        if (NextError != AddressSpacePageTableError::None)
        {
            if (NextError == AddressSpacePageTableError::CommittedStateUnknown || CountErrorAsInstalled)
            {
                ++InstalledMappings;
            }
            return NextError;
        }

        AddressSpacePageTableMapResult result{
            .InvalidationStart = mapping.VirtualStart,
            .InvalidationPages = mapping.Pages,
            .Reserved = 0U,
        };
        switch (Mode)
        {
        case PageTableResultMode::Exact:
            break;
        case PageTableResultMode::WrongStart:
            result.InvalidationStart = VirtualAddress{mapping.VirtualStart.GetValue() + 0x1000U};
            break;
        case PageTableResultMode::WrongPages:
            result.InvalidationPages = PageCount{mapping.Pages.GetValue() + 1U};
            break;
        case PageTableResultMode::Reserved:
            result.Reserved = 1U;
            break;
        }
        output = result;
        ++InstalledMappings;
        return AddressSpacePageTableError::None;
    }

    [[nodiscard]] bool DetachAcknowledgesShootdown(bool) const noexcept override
    {
        return AcknowledgeOnDetach;
    }
    bool AcknowledgeOnDetach{false};
    bool Ready{true};
    AddressSpacePageTableError NextError{AddressSpacePageTableError::None};
    AddressSpacePageTableActivationError NextActivationError{AddressSpacePageTableActivationError::None};
    bool CountErrorAsInstalled{false};
    PageTableResultMode Mode{PageTableResultMode::Exact};
    Foundation::Size MapCalls{0U};
    Foundation::Size UnmapCalls{0U};
    AddressSpacePageTableError NextUnmapError{AddressSpacePageTableError::None};
    AddressSpaceMappingRecord LastUnmapping{};
    Foundation::Size InstalledMappings{0U};
    Foundation::Size ClaimCalls{0U};
    Foundation::Size ReleaseCalls{0U};
    Foundation::Size ActivateCalls{0U};
    const AddressSpace* Owner{nullptr};
    const InitialMappingPlan* SeedPlan{nullptr};
    const AddressSpace* LastActivationOwner{nullptr};
    CpuId LastActivationProcessor{};
    AddressSpaceMappingRecord LastMapping{};
};

class TestIommuBackend final : public IIommuDomainBackend
{
public:
    [[nodiscard]] bool IsReady() const noexcept override
    {
        return true;
    }

    [[nodiscard]] Hal::Device::IommuBackendFaults GetFaults() noexcept override
    {
        return {};
    }

    [[nodiscard]] IommuBackendOwnershipError Claim(const IommuDomain& owner, Foundation::UInt8) noexcept override
    {
        if (Owner != nullptr)
        {
            return IommuBackendOwnershipError::AlreadyOwned;
        }
        Owner = &owner;
        return IommuBackendOwnershipError::None;
    }

    [[nodiscard]] IommuBackendOwnershipError Release(const IommuDomain& owner) noexcept override
    {
        if (Owner != &owner)
        {
            return IommuBackendOwnershipError::InvalidOwner;
        }
        Owner = nullptr;
        LiveMappings = 0U;
        return IommuBackendOwnershipError::None;
    }

    [[nodiscard]] Kernel::Device::DmaAddress
    ChooseDeviceAddress(Kernel::Device::DmaAddress requested, Kernel::Memory::PhysicalAddress) const noexcept override
    {
        return requested;
    }

    [[nodiscard]] IommuBackendError Map(const IommuBackendMapRequest&) noexcept override
    {
        ++MapCalls;
        if (NextMap != IommuBackendError::None)
        {
            const IommuBackendError error = NextMap;
            NextMap = IommuBackendError::None;
            return error;
        }
        ++LiveMappings;
        return IommuBackendError::None;
    }

    [[nodiscard]] IommuBackendError Unmap(const IommuBackendMapRequest&) noexcept override
    {
        ++UnmapCalls;
        if (LiveMappings == 0U)
        {
            return IommuBackendError::InvalidRequest;
        }
        --LiveMappings;
        return IommuBackendError::None;
    }

    [[nodiscard]] IommuBackendError Attach(IIommuDevice&) noexcept override
    {
        return IommuBackendError::UnsupportedConfiguration;
    }

    [[nodiscard]] IommuBackendError Detach(IIommuDevice&) noexcept override
    {
        return IommuBackendError::UnsupportedConfiguration;
    }

    const IommuDomain* Owner{nullptr};
    IommuBackendError NextMap{IommuBackendError::None};
    Foundation::Size MapCalls{0U};
    Foundation::Size UnmapCalls{0U};
    Foundation::Size LiveMappings{0U};
};

class TestLocalTlbInvalidator final : public ILocalTlbInvalidator
{
public:
    [[nodiscard]] LocalTlbInvalidationError Invalidate(const VirtualAddress start,
                                                       const PageCount pages) noexcept override
    {
        const Foundation::Size callIndex = Calls;
        ++Calls;
        LastStart = start;
        LastPages = pages;
        if (callIndex < ScriptedErrorCount)
        {
            return ScriptedErrors[callIndex];
        }
        return NextError;
    }

    LocalTlbInvalidationError NextError{LocalTlbInvalidationError::None};
    LocalTlbInvalidationError ScriptedErrors[8]{};
    Foundation::Size ScriptedErrorCount{0U};
    Foundation::Size Calls{0U};
    VirtualAddress LastStart{};
    PageCount LastPages{};
};

class TestTlbShootdownTransport final : public ITlbShootdownTransport
{
public:
    [[nodiscard]] TlbShootdownTransportError Publish(const TlbShootdownBatch& batch) noexcept override
    {
        ++PublishCalls;
        if (NextError != TlbShootdownTransportError::None)
        {
            return NextError;
        }
        Captured = batch;
        HasBatch = true;
        return TlbShootdownTransportError::None;
    }

    TlbShootdownTransportError NextError{TlbShootdownTransportError::None};
    Foundation::Size PublishCalls{0U};
    TlbShootdownBatch Captured{};
    bool HasBatch{false};
};

class TestTlbShootdownNotifier final : public ITlbShootdownNotifier
{
public:
    void Notify(const Foundation::ProcessorMask targetProcessorMask) noexcept override
    {
        LastMask.store(targetProcessorMask, std::memory_order_release);
        static_cast<void>(ObservedMask.fetch_or(targetProcessorMask, std::memory_order_acq_rel));
        static_cast<void>(Calls.fetch_add(1U, std::memory_order_acq_rel));
    }

    std::atomic<Foundation::Size> Calls{0U};
    std::atomic<Foundation::ProcessorMask> LastMask{0U};
    std::atomic<Foundation::ProcessorMask> ObservedMask{0U};
};

[[nodiscard]] constexpr MappingPermissions ReadOnly() noexcept
{
    return MappingPermissions{MappingPermission::Read};
}

[[nodiscard]] constexpr MappingPermissions ReadWrite() noexcept
{
    return ReadOnly().With(MappingPermission::Write);
}

[[nodiscard]] constexpr MappingPermissions ReadExecute() noexcept
{
    return ReadOnly().With(MappingPermission::Execute);
}

[[nodiscard]] constexpr PagePermissions ReadWriteUser() noexcept
{
    return PagePermissions{PagePermission::Read}.With(PagePermission::Write).With(PagePermission::User);
}

[[nodiscard]] constexpr PagePermissions ReadExecuteUser() noexcept
{
    return PagePermissions{PagePermission::Read}.With(PagePermission::Execute).With(PagePermission::User);
}

[[nodiscard]] bool ActivateProcessor(AddressSpace& space, const CpuId processor) noexcept
{
    return space.ActivateProcessor(processor).HasValue();
}

[[nodiscard]] constexpr InitialMapping MakeSeedMapping(const Foundation::UInt64 virtualStart,
                                                       const Foundation::UInt64 physicalStart,
                                                       const Foundation::UInt64 pages,
                                                       const MappingPermissions permissions,
                                                       const InitialMappingOwner owner) noexcept
{
    return InitialMapping{
        .VirtualStart = VirtualAddress{virtualStart},
        .PhysicalStart = PhysicalAddress{physicalStart},
        .Pages = PageCount{pages},
        .Permissions = permissions,
        .CachePolicy = MemoryCachePolicy::WriteBack,
        .Owner = owner,
        .Reserved = 0U,
    };
}

[[nodiscard]] bool BuildSeedPlan(InitialMappingPlan& plan) noexcept
{
    constexpr Foundation::UInt64 base{0xFFFF'8000'0000'0000ULL};
    return (plan.AddMapping(MakeSeedMapping(base + 0x1000U, 0x0010'0000U, 2U, ReadExecute(),
                                            InitialMappingOwner::NucleusCode)) == InitialMappingPlanError::None) &&
           (plan.AddMapping(MakeSeedMapping(base + 0x4000U, 0x0010'4000U, 4U, ReadWrite(),
                                            InitialMappingOwner::NucleusStack)) == InitialMappingPlanError::None) &&
           (plan.AddMapping(MakeSeedMapping(base + 0x8000U, 0x0010'8000U, 16U, ReadWrite(),
                                            InitialMappingOwner::PageTable)) == InitialMappingPlanError::None) &&
           (plan.Seal() == InitialMappingPlanError::None);
}

[[nodiscard]] constexpr FrameDescriptor
MakeFrameDescriptor(const Foundation::UInt64 physicalStart = 0x0200'0000U,
                    const MemoryCachePolicy cachePolicy = MemoryCachePolicy::WriteBack,
                    const FrameKind kind = FrameKind::Ram) noexcept
{
    return FrameDescriptor{
        .PhysicalStart = PhysicalAddress{physicalStart},
        .Pages = PageCount{32U},
        .CachePolicy = cachePolicy,
        .Kind = kind,
        .Reserved = {},
    };
}

[[nodiscard]] constexpr FaultRecord MakeFault(const Foundation::UInt64 virtualStart, const FaultAccess access) noexcept
{
    return FaultRecord{
        .VirtualAddress = virtualStart + 0x123U,
        .InstructionPointer = 0x0080'0000U,
        .ArchitectureSyndrome = 0xA55AU,
        .Access = access,
        .Cause = FaultCause::MissingMapping,
        .Privilege = FaultPrivilege::User,
        .Reserved = {},
    };
}

[[nodiscard]] constexpr FaultFrameMappingRequest MakeRequest(const Foundation::UInt64 virtualStart,
                                                             const Foundation::UInt64 frameOffset,
                                                             const PagePermissions permissions) noexcept
{
    return FaultFrameMappingRequest{
        .VirtualStart = VirtualAddress{virtualStart},
        .Pages = PageCount{2U},
        .MemoryObjectOffsetPages = frameOffset,
        .FrameOffsetPages = frameOffset,
        .Permissions = permissions,
        .Reserved = {},
    };
}

class RegistryFixture final
{
public:
    constexpr RegistryFixture() noexcept : Registry{Synchronization}, Issuer{Registry}
    {
    }

    SpinLock Synchronization;
    Asteria::TestSupport::TestObjectRegistry Registry;
    RootCapabilityIssuer Issuer;
};

[[nodiscard]] constexpr ResourceDomainLimits MakeMemoryDomainLimits() noexcept
{
    return ResourceDomainLimits{
        .CommittedMemoryPages = 1024U,
        .ResidentMemoryPages = 1024U,
        .KernelMetadataBytes = 1U << 20U,
        .CapabilityHandles = 64U,
        .IpcQueueBytes = 0U,
        .Threads = 0U,
        .ChildDomains = 0U,
    };
}

[[nodiscard]] constexpr MemoryObjectDescriptor
MakeMemoryDescriptor(const MemoryCachePolicy cachePolicy = MemoryCachePolicy::WriteBack,
                     const MemoryCommitment commitment = MemoryCommitment::BestEffort) noexcept
{
    return MemoryObjectDescriptor{
        .Pages = PageCount{32U},
        .CachePolicy = cachePolicy,
        .MaximumPermissions = PagePermissions{PagePermission::Read}
                                  .With(PagePermission::Write)
                                  .With(PagePermission::Execute)
                                  .With(PagePermission::User),
        .Commitment = commitment,
        .Reserved = {},
    };
}

class MemoryPolicyFixture final
{
private:
    RegistryFixture& m_registry;
    TestContext& m_runner;

public:
    MemoryPolicyFixture(RegistryFixture& registry, TestContext& runner,
                        const MemoryObjectDescriptor& descriptor = MakeMemoryDescriptor()) noexcept
        : m_registry{registry}, m_runner{runner}, Domain{Ledger}
    {
        const auto domainInitialization = Domain.InitializeRoot(ResourceDomainId{13U}, MakeMemoryDomainLimits());
        if (domainInitialization.Error != ResourceDomainError::None)
        {
            return;
        }
        const auto domainRegistration = m_registry.Issuer.CreateObject(Domain);
        if (!domainRegistration.HasValue())
        {
            return;
        }
        DomainObject = domainRegistration.GetValue();
        const auto memoryInitialization = Object.Initialize(
            m_registry.Registry, ResourceDomainObjectId{m_registry.Registry, DomainObject}, Domain, descriptor);
        if (memoryInitialization.Error != MemoryObjectError::None)
        {
            return;
        }
        const auto memoryRegistration = m_registry.Issuer.CreateObject(Object);
        if (!memoryRegistration.HasValue())
        {
            return;
        }
        ObjectIdValue = memoryRegistration.GetValue();
        Identity = MemoryObjectId{m_registry.Registry, ObjectIdValue};
    }

    ~MemoryPolicyFixture() noexcept
    {
        if (ObjectIdValue.IsValid())
        {
            const auto quiesced = Object.Quiesce();
            const auto destroyed = m_registry.Issuer.DestroyObject(ObjectIdValue);
            const auto collected = m_registry.Issuer.CollectDestroyedObject();
            ASTERIA_EXPECT(m_runner, "fixture retires its logical MemoryObject after mappings",
                           quiesced.Error == MemoryObjectError::None, destroyed == ObjectRegistryError::Ok,
                           collected.HasValue(), collected.GetValue().Instance == &Object);
        }
        if (DomainObject.IsValid())
        {
            const auto quiesced = Domain.Quiesce();
            const auto destroyed = m_registry.Issuer.DestroyObject(DomainObject);
            const auto collected = m_registry.Issuer.CollectDestroyedObject();
            ASTERIA_EXPECT(m_runner, "fixture retires its MemoryObject resource domain",
                           quiesced == ResourceDomainError::None, destroyed == ObjectRegistryError::Ok,
                           collected.HasValue(), collected.GetValue().Instance == &Domain);
        }
    }

    MemoryPolicyFixture(const MemoryPolicyFixture&) = delete;
    MemoryPolicyFixture& operator=(const MemoryPolicyFixture&) = delete;

    [[nodiscard]] bool IsReady() const noexcept
    {
        return Identity.IsValid();
    }

    ResourceDomainLedger Ledger;
    ResourceDomain Domain;
    MemoryObject Object;
    ObjectId DomainObject{};
    ObjectId ObjectIdValue{};
    MemoryObjectId Identity{};
};

class AddressSpaceFixture final
{
    TestContext& m_runner;

public:
    explicit AddressSpaceFixture(TestContext& runner, const bool device = false,
                                 const MemoryCommitment commitment = MemoryCommitment::BestEffort,
                                 const bool readOnlyBootstrap = false) noexcept
        : m_runner{runner},
          MemoryPolicy{
              Registry, runner,
              MakeMemoryDescriptor(device ? MemoryCachePolicy::Uncached : MemoryCachePolicy::WriteBack, commitment)},
          IsDevice{device}
    {
        const bool planReady = BuildSeedPlan(SeedPlan);
        ASTERIA_EXPECT(m_runner, "fixture seals a verified bootstrap mapping plan", planReady);
        if (!planReady)
        {
            return;
        }
        ASTERIA_EXPECT(m_runner, "fixture initializes an AddressSpace from its verified seed",
                       Space.Initialize(ResourceDomainId{10U}, PageTable, SeedPlan) == AddressSpaceError::None);
        const auto addressResult = Registry.Issuer.CreateObject(Space);
        ASTERIA_EXPECT(m_runner, "fixture registers the initialized AddressSpace", addressResult.HasValue());
        if (!addressResult.HasValue())
        {
            return;
        }
        AddressObject = addressResult.GetValue();

        ASTERIA_EXPECT(m_runner, "fixture binds the exact registered AddressSpace identity",
                       Space.BindIdentity(Registry.Registry, AddressSpaceId{Registry.Registry, AddressObject}).Error ==
                           AddressSpaceError::None);

        const auto threadResult = Registry.Issuer.CreateObject(Thread);
        ASTERIA_EXPECT(m_runner, "fixture registers one faulting Thread", threadResult.HasValue());
        if (threadResult.HasValue())
        {
            ThreadObject = threadResult.GetValue();
        }

        ASTERIA_EXPECT(m_runner, "fixture initializes one physical Frame",
                       (readOnlyBootstrap
                            ? FrameStorage.InitializeReadOnly(ResourceDomainId{12U}, MakeFrameDescriptor(0x0010'4000U))
                            : FrameStorage.Initialize(
                                  ResourceDomainId{12U},
                                  MakeFrameDescriptor(
                                      0x0200'0000U, device ? MemoryCachePolicy::Uncached : MemoryCachePolicy::WriteBack,
                                      device ? FrameKind::Device : FrameKind::Ram))) == FrameError::None);
        const auto frameResult = Registry.Issuer.CreateObject(FrameStorage);
        ASTERIA_EXPECT(m_runner, "fixture registers its RAM Frame", frameResult.HasValue());
        if (frameResult.HasValue())
        {
            FrameObject = frameResult.GetValue();
        }

        if (device)
        {
            const DeviceRegionDescriptor descriptor{
                .BaseAddress = 0x0200'0000U,
                .LengthBytes = 32U * 4096U,
                .Kind = DeviceRegionKind::MemoryMappedIo,
                .Permissions =
                    DeviceRegionPermissions{DeviceRegionPermission::Read}.With(DeviceRegionPermission::Write),
                .Widths = DeviceAccessWidths{DeviceAccessWidth::Byte}
                              .With(DeviceAccessWidth::Word)
                              .With(DeviceAccessWidth::DoubleWord)
                              .With(DeviceAccessWidth::QuadWord),
                .CachePolicy = MemoryCachePolicy::Uncached,
                .Reserved = {},
            };
            ASTERIA_EXPECT(
                m_runner, "fixture initializes the device Frame's exact MMIO authority",
                DeviceRegionStorage
                        .Initialize(Registry.Registry,
                                    MemoryPolicy.Identity.IsValid()
                                        ? ResourceDomainObjectId{Registry.Registry, MemoryPolicy.DomainObject}
                                        : ResourceDomainObjectId{},
                                    MemoryPolicy.Domain, descriptor)
                        .Error == DeviceRegionError::None);
            const auto deviceRegionResult = Registry.Issuer.CreateObject(DeviceRegionStorage);
            ASTERIA_EXPECT(m_runner, "fixture registers its MMIO DeviceRegion", deviceRegionResult.HasValue());
            if (deviceRegionResult.HasValue())
            {
                DeviceRegionObject = deviceRegionResult.GetValue();
            }
        }
    }

    ~AddressSpaceFixture() noexcept
    {
        for (Foundation::UInt8 cpu{0U}; cpu < Kernel::Scheduler::MaximumProcessorCount; ++cpu)
        {
            const AddressSpaceState state = Space.GetSnapshot().State;
            if ((state == AddressSpaceState::Ready) || (state == AddressSpaceState::Faulted))
            {
                static_cast<void>(Space.DetachProcessor(CpuId{cpu}));
            }
        }
        const AddressSpaceState state = Space.GetSnapshot().State;
        if ((state != AddressSpaceState::Uninitialized) && (state != AddressSpaceState::Quiesced) &&
            (state != AddressSpaceState::ShootdownPending))
        {
            ASTERIA_EXPECT(m_runner, "fixture quiesces the AddressSpace before collection",
                           Space.Quiesce() == AddressSpaceError::None);
        }

        if (DeviceRegionObject.IsValid())
        {
            ASTERIA_EXPECT(m_runner, "fixture quiesces DeviceRegion after AddressSpace release",
                           DeviceRegionStorage.Quiesce().Error == DeviceRegionError::None);
            DestroyAndCollect(DeviceRegionObject, &DeviceRegionStorage);
        }
        DestroyAndCollect(FrameObject, &FrameStorage);
        DestroyAndCollect(ThreadObject, &Thread);
        DestroyAndCollect(AddressObject, &Space);
    }

    AddressSpaceFixture(const AddressSpaceFixture&) = delete;
    AddressSpaceFixture& operator=(const AddressSpaceFixture&) = delete;
    AddressSpaceFixture(AddressSpaceFixture&&) = delete;
    AddressSpaceFixture& operator=(AddressSpaceFixture&&) = delete;

    [[nodiscard]] bool IsReady() const noexcept
    {
        return AddressObject.IsValid() && ThreadObject.IsValid() && FrameObject.IsValid() &&
               (!IsDevice || DeviceRegionObject.IsValid()) && MemoryPolicy.IsReady() &&
               (Space.GetSnapshot().State == AddressSpaceState::Ready);
    }

    [[nodiscard]] AddressSpaceId GetAddressSpaceId() const noexcept
    {
        return AddressSpaceId{Registry.Registry, AddressObject};
    }

    [[nodiscard]] ThreadId GetThreadId() const noexcept
    {
        return ThreadId{Registry.Registry, ThreadObject};
    }

    [[nodiscard]] FrameId GetFrameId() const noexcept
    {
        return FrameId{Registry.Registry, FrameObject};
    }

    [[nodiscard]] MemoryObjectId GetMemoryObjectId() const noexcept
    {
        return MemoryPolicy.Identity;
    }

    [[nodiscard]] DeviceRegionId GetDeviceRegionId() const noexcept
    {
        return IsDevice ? DeviceRegionId{Registry.Registry, DeviceRegionObject} : DeviceRegionId{};
    }

    RegistryFixture Registry;
    MemoryPolicyFixture MemoryPolicy;
    TestPageTable PageTable;
    InitialMappingPlan SeedPlan;
    AddressSpace Space;
    TestKernelObject Thread{ObjectType::Thread, ResourceDomainId{11U}};
    DeviceRegion DeviceRegionStorage;
    BootFrame FrameStorage;
    ObjectId AddressObject{};
    ObjectId ThreadObject{};
    ObjectId DeviceRegionObject{};
    ObjectId FrameObject{};
    bool IsDevice{false};

private:
    void DestroyAndCollect(const ObjectId object, IKernelObject* const expected) noexcept
    {
        if (!object.IsValid())
        {
            return;
        }
        const ObjectRegistryError destroyError = Registry.Issuer.DestroyObject(object);
        const auto collected = Registry.Issuer.CollectDestroyedObject();
        ASTERIA_EXPECT(m_runner, "fixture collects registered storage after every pin releases",
                       destroyError == ObjectRegistryError::Ok, collected.HasValue(),
                       collected.GetValue().Object.GetValue() == object.GetValue(),
                       collected.GetValue().Instance == expected);
    }
};

class PreparedFault final
{
    TestContext& m_runner;

public:
    PreparedFault(AddressSpaceFixture& fixture, TestContext& runner, const Foundation::UInt64 virtualStart,
                  const Foundation::UInt64 frameOffset, const PagePermissions permissions,
                  const FaultAccess access) noexcept
        : m_runner{runner}
    {
        const FaultTokenError initializeError =
            Token.Initialize(fixture.Registry.Registry, ResourceDomainId{20U}, fixture.GetThreadId(),
                             fixture.GetAddressSpaceId(), MakeFault(virtualStart, access));
        const auto claim = Token.ClaimResolution();
        ASTERIA_EXPECT(m_runner, "fault fixture publishes and claims one mapping fault",
                       initializeError == FaultTokenError::None, claim.HasValue());
        if (!claim.HasValue())
        {
            return;
        }
        Claim = claim.GetValue().Token;
        const FaultFrameMappingError prepareError = Preparation.Prepare(
            fixture.Registry.Registry, Token, Claim, fixture.GetMemoryObjectId(), fixture.MemoryPolicy.Object,
            fixture.GetDeviceRegionId(), fixture.IsDevice ? &fixture.DeviceRegionStorage : nullptr,
            fixture.GetFrameId(), fixture.FrameStorage, MakeRequest(virtualStart, frameOffset, permissions));
        ASTERIA_EXPECT(m_runner, "fault fixture prepares an authorized generation-pinned mapping",
                       prepareError == FaultFrameMappingError::None);
        Prepared = prepareError == FaultFrameMappingError::None;
    }

    ~PreparedFault() noexcept
    {
        if (Claim.IsValid() && !Resolved)
        {
            static_cast<void>(Token.CancelResolution(Claim));
        }
    }

    void Commit() noexcept
    {
        if (!Claim.IsValid())
        {
            return;
        }
        ASTERIA_EXPECT(m_runner, "completed AddressSpace mapping commits the exact fault claim",
                       Token.CommitResolution(Claim, FaultResolutionKind::Map) == FaultTokenError::None);
        Resolved = true;
    }

    FaultToken Token;
    FaultFrameMappingPreparation Preparation;
    FaultResolutionClaimToken Claim{};
    bool Prepared{false};
    bool Resolved{false};
};

[[nodiscard]] constexpr DmaPermissions ReadWriteDma() noexcept
{
    return DmaPermissions{DmaPermission::Read}.With(DmaPermission::Write);
}

[[nodiscard]] IommuMappingRequest MakeDmaRequest(AddressSpaceFixture& fixture, const Foundation::UInt64 dmaStart,
                                                 const Foundation::UInt64 frameOffset,
                                                 const Foundation::UInt64 pages) noexcept
{
    return IommuMappingRequest{
        .FrameStorage = &fixture.FrameStorage,
        .FrameIdentity = fixture.GetFrameId(),
        .DmaStart = DmaAddress{dmaStart},
        .FrameOffsetPages = PageCount{frameOffset},
        .Pages = PageCount{pages},
        .Permissions = ReadWriteDma(),
        .Reserved = {},
    };
}

[[nodiscard]] bool IsDmaWriteExecuteConflict(const IommuDomainFailure& failure) noexcept
{
    return (failure.Error == IommuDomainError::FramePolicyRejected) &&
           (failure.FramePolicy == Kernel::Memory::FrameMappingPolicyError::WriteExecuteConflict);
}

[[nodiscard]] bool RecordIsExpectedRuntimeMapping(const AddressSpaceMappingRecord& record,
                                                  const MemoryObjectId memoryObject, const FrameId frame,
                                                  const Foundation::UInt64 virtualStart,
                                                  const Foundation::UInt64 physicalStart,
                                                  const PagePermissions permissions) noexcept
{
    return record.MemoryObject.IsEqualTo(memoryObject) && !record.DeviceRegion.IsValid() &&
           record.Frame.IsEqualTo(frame) && (record.VirtualStart.GetValue() == virtualStart) &&
           (record.PhysicalStart.GetValue() == physicalStart) && (record.Pages.GetValue() == 2U) &&
           (record.MemoryObjectOffsetPages == ((physicalStart - 0x0200'0000U) / 4096U)) &&
           (record.Permissions.GetBits() == permissions.GetBits()) &&
           (record.CachePolicy == MemoryCachePolicy::WriteBack) && (record.Kind == FrameKind::Ram) &&
           (record.Commitment == MemoryCommitment::BestEffort) && record.RuntimeMapping && (record.Reserved[0] == 0U) &&
           (record.Reserved[1] == 0U) && (record.Reserved[2] == 0U);
}

void TestInitializationAndIdentity(TestContext& runner) noexcept
{
    AddressSpace space;
    TestPageTable pageTable;
    InitialMappingPlan seed;
    ASTERIA_EXPECT(runner, "initialization test builds its seed plan", BuildSeedPlan(seed));

    InitialMappingPlan unsealed;
    ASTERIA_EXPECT(runner, "unsealed seed rejection leaves the AddressSpace inert",
                   space.Initialize(ResourceDomainId{1U}, pageTable, unsealed) == AddressSpaceError::InvalidMappingPlan,
                   space.GetSnapshot().State == AddressSpaceState::Uninitialized,
                   space.GetChargeOwner().GetValue() == 0U);
    pageTable.Ready = false;
    ASTERIA_EXPECT(runner, "unready page-table backend is rejected without publication",
                   space.Initialize(ResourceDomainId{1U}, pageTable, seed) ==
                       AddressSpaceError::PageTableOwnershipRejected,
                   space.GetSnapshot().State == AddressSpaceState::Uninitialized);
    pageTable.Ready = true;
    ASTERIA_EXPECT(runner, "valid initialization imports three seed mappings exactly once",
                   space.Initialize(ResourceDomainId{}, pageTable, seed) == AddressSpaceError::InvalidConfiguration,
                   space.Initialize(ResourceDomainId{1U}, pageTable, seed) == AddressSpaceError::None,
                   space.GetSnapshot().State == AddressSpaceState::Unbound, space.GetSnapshot().MappingCount == 3U,
                   space.GetChargeOwner().GetValue() == 1U);
    AddressSpace competingOwner;
    ASTERIA_EXPECT(runner, "one native root cannot be claimed by two AddressSpace owners",
                   competingOwner.Initialize(ResourceDomainId{2U}, pageTable, seed) ==
                       AddressSpaceError::PageTableOwnershipRejected,
                   competingOwner.GetSnapshot().State == AddressSpaceState::Uninitialized);

    RegistryFixture registry;
    RegistryFixture foreignRegistry;
    const auto registered = registry.Issuer.CreateObject(space);
    ASTERIA_EXPECT(runner, "initialized AddressSpace registers", registered.HasValue());
    if (!registered.HasValue())
    {
        return;
    }
    const ObjectId object = registered.GetValue();
    ASTERIA_EXPECT(runner, "identity failures preserve the unbound AddressSpace",
                   space.BindIdentity(registry.Registry, AddressSpaceId{}).Error == AddressSpaceError::InvalidIdentity,
                   space.BindIdentity(registry.Registry, AddressSpaceId{foreignRegistry.Registry, object}).Error ==
                       AddressSpaceError::RegistryMismatch,
                   space.GetSnapshot().State == AddressSpaceState::Unbound);
    ASTERIA_EXPECT(runner, "exact registry identity publishes Ready once",
                   space.BindIdentity(registry.Registry, AddressSpaceId{registry.Registry, object}).Error ==
                       AddressSpaceError::None,
                   space.GetSnapshot().State == AddressSpaceState::Ready,
                   space.GetSnapshot().Identity.IsEqualTo(AddressSpaceId{registry.Registry, object}),
                   space.BindIdentity(registry.Registry, AddressSpaceId{registry.Registry, object}).Error ==
                       AddressSpaceError::AlreadyBound);
    const auto invalidActivation = space.ActivateProcessor(CpuId{4U});
    const auto completedActivation = space.ActivateProcessor(CpuId{0U});
    ASTERIA_EXPECT(
        runner, "integrated activation publishes residency only after backend commit", !invalidActivation.HasValue(),
        invalidActivation.GetError().Error == AddressSpaceError::InvalidProcessor,
        !invalidActivation.GetError().ProcessorMayBeResident, completedActivation.HasValue(),
        completedActivation.GetValue().Processor.GetValue() == 0U, completedActivation.GetValue().Sequence == 1U,
        pageTable.ActivateCalls == 1U, pageTable.LastActivationOwner == &space,
        pageTable.LastActivationProcessor.GetValue() == 0U, space.GetSnapshot().ResidentProcessorMask == 0x01U,
        space.Quiesce() == AddressSpaceError::ProcessorsStillResident,
        space.DetachProcessor(CpuId{0U}) == AddressSpaceError::None, space.Quiesce() == AddressSpaceError::None,
        space.GetSnapshot().State == AddressSpaceState::Quiesced);

    const ObjectRegistryError destroyed = registry.Issuer.DestroyObject(object);
    const auto collected = registry.Issuer.CollectDestroyedObject();
    ASTERIA_EXPECT(runner, "quiesced AddressSpace storage is collectable", destroyed == ObjectRegistryError::Ok,
                   collected.HasValue(), collected.GetValue().Instance == &space);
}

void TestActivationFailureClassification(TestContext& runner) noexcept
{
    {
        AddressSpaceFixture fixture{runner};
        if (!fixture.IsReady())
        {
            return;
        }

        fixture.PageTable.NextActivationError = AddressSpacePageTableActivationError::RejectedBeforeCommit;
        const auto rejected = fixture.Space.ActivateProcessor(CpuId{1U});
        const auto rejectedSnapshot = fixture.Space.GetSnapshot();
        ASTERIA_EXPECT(runner, "pre-commit activation rejection preserves prior-root authority", !rejected.HasValue(),
                       rejected.GetError().Error == AddressSpaceError::PageTableActivationRejected,
                       rejected.GetError().PageTable == AddressSpacePageTableActivationError::RejectedBeforeCommit,
                       !rejected.GetError().ProcessorMayBeResident, rejectedSnapshot.State == AddressSpaceState::Ready,
                       rejectedSnapshot.ResidentProcessorMask == 0U, !rejectedSnapshot.ActivatingProcessor.IsValid());

        fixture.PageTable.NextActivationError = AddressSpacePageTableActivationError::None;
        const auto completed = fixture.Space.ActivateProcessor(CpuId{1U});
        const auto duplicate = fixture.Space.ActivateProcessor(CpuId{1U});
        ASTERIA_EXPECT(runner, "activation sequences consume rejected attempts and exact residency rejects duplicates",
                       completed.HasValue(), completed.GetValue().Sequence == 2U, !duplicate.HasValue(),
                       duplicate.GetError().Error == AddressSpaceError::ProcessorAlreadyResident,
                       !duplicate.GetError().ProcessorMayBeResident, fixture.PageTable.ActivateCalls == 2U,
                       fixture.Space.GetSnapshot().ResidentProcessorMask == 0x02U);
    }

    {
        AddressSpaceFixture fixture{runner};
        if (!fixture.IsReady())
        {
            return;
        }

        fixture.PageTable.NextActivationError = AddressSpacePageTableActivationError::CommittedStateUnknown;
        const auto unknown = fixture.Space.ActivateProcessor(CpuId{2U});
        const auto snapshot = fixture.Space.GetSnapshot();
        ASTERIA_EXPECT(runner, "ambiguous activation commit faults and retains conservative residency",
                       !unknown.HasValue(),
                       unknown.GetError().Error == AddressSpaceError::PageTableActivationCommittedStateUnknown,
                       unknown.GetError().PageTable == AddressSpacePageTableActivationError::CommittedStateUnknown,
                       unknown.GetError().ProcessorMayBeResident, snapshot.State == AddressSpaceState::Faulted,
                       snapshot.ResidentProcessorMask == 0x04U, !snapshot.ActivatingProcessor.IsValid());
    }
}

void TestProcessorTransitionReservations(TestContext& runner) noexcept
{
    {
        AddressSpaceFixture fixture{runner};
        if (!fixture.IsReady())
        {
            return;
        }

        const auto held = fixture.Space.ReserveProcessorTransition(CpuId{0U});
        if (!held.HasValue())
        {
            ASTERIA_EXPECT(runner, "quiescence fixture reserves its empty root", false);
            return;
        }

        const AddressSpaceError quiesce = fixture.Space.Quiesce();
        const AddressSpaceError staleDetach = fixture.Space.DetachReservedProcessor(held.GetValue());
        const AddressSpaceError staleRelease = fixture.Space.ReleaseProcessorTransition(held.GetValue());
        const auto snapshot = fixture.Space.GetSnapshot();
        ASTERIA_EXPECT(runner, "zero-resident quiescence releases the backend and retires its reservation",
                       quiesce == AddressSpaceError::None, fixture.PageTable.Owner == nullptr,
                       snapshot.State == AddressSpaceState::Quiesced, snapshot.ResidentProcessorMask == 0U,
                       snapshot.TransitionSequence == 0U, !snapshot.TransitionProcessor.IsValid(),
                       staleDetach == AddressSpaceError::InvalidProcessorTransitionReceipt,
                       staleRelease == AddressSpaceError::InvalidProcessorTransitionReceipt);
    }

    {
        AddressSpaceFixture fixture{runner};
        if (!fixture.IsReady())
        {
            return;
        }

        constexpr CpuId processor{2U};
        const auto activated = fixture.Space.ActivateProcessor(processor);
        const auto held = fixture.Space.ReserveProcessorTransition(processor);
        if (!activated.HasValue() || !held.HasValue())
        {
            ASTERIA_EXPECT(runner, "resident quiescence fixture reserves its active root", false);
            return;
        }

        const AddressSpaceError blocked = fixture.Space.Quiesce();
        const auto blockedSnapshot = fixture.Space.GetSnapshot();
        const AddressSpaceError detach = fixture.Space.DetachReservedProcessor(held.GetValue());
        const AddressSpaceError release = fixture.Space.ReleaseProcessorTransition(held.GetValue());
        const AddressSpaceError completed = fixture.Space.Quiesce();
        ASTERIA_EXPECT(
            runner, "resident reserved roots reject quiescence until exact detach and release",
            blocked == AddressSpaceError::ProcessorTransitionPending, blockedSnapshot.State == AddressSpaceState::Ready,
            blockedSnapshot.ResidentProcessorMask == 0x04U,
            blockedSnapshot.TransitionSequence == held.GetValue().GetSequence(), detach == AddressSpaceError::None,
            release == AddressSpaceError::None, completed == AddressSpaceError::None);
    }

    {
        AddressSpaceFixture fixture{runner};
        if (!fixture.IsReady())
        {
            return;
        }

        PreparedFault mapping{fixture, runner, 0x0030'0000U, 0U, ReadWriteUser(), FaultAccess::Write};
        if (!mapping.Prepared)
        {
            return;
        }

        const auto transition = fixture.Space.ReserveProcessorTransition(CpuId{0U});
        if (!transition.HasValue())
        {
            ASTERIA_EXPECT(runner, "processor transition fixture reserves its root", false);
            return;
        }

        const auto competing = fixture.Space.ReserveProcessorTransition(CpuId{0U});
        const Foundation::Size mapCallsBefore = fixture.PageTable.MapCalls;
        const auto blockedMapping = fixture.Space.BeginFaultMapping(mapping.Preparation);
        const auto blockedActivation = fixture.Space.ActivateProcessor(CpuId{0U});
        const AddressSpaceError blockedDetach = fixture.Space.DetachProcessor(CpuId{0U});
        const auto reservedSnapshot = fixture.Space.GetSnapshot();
        ASTERIA_EXPECT(runner, "one exact reservation atomically excludes mapping and bare residency changes",
                       !competing.HasValue(), competing.GetError() == AddressSpaceError::ProcessorTransitionPending,
                       !blockedMapping.HasValue(),
                       blockedMapping.GetError().Error == AddressSpaceError::ProcessorTransitionPending,
                       fixture.PageTable.MapCalls == mapCallsBefore, !blockedActivation.HasValue(),
                       blockedActivation.GetError().Error == AddressSpaceError::ProcessorTransitionPending,
                       blockedDetach == AddressSpaceError::ProcessorTransitionPending,
                       reservedSnapshot.TransitionSequence == transition.GetValue().GetSequence(),
                       reservedSnapshot.TransitionProcessor.IsValid(),
                       reservedSnapshot.TransitionProcessor.GetValue() == 0U);

        const auto activated = fixture.Space.ActivateReservedProcessor(transition.GetValue());
        const AddressSpaceError invalidDetach =
            fixture.Space.DetachReservedProcessor(Kernel::Memory::AddressSpaceTransitionReceipt{});
        const AddressSpaceError detached = fixture.Space.DetachReservedProcessor(transition.GetValue());
        const AddressSpaceError released = fixture.Space.ReleaseProcessorTransition(transition.GetValue());
        const AddressSpaceError staleRelease = fixture.Space.ReleaseProcessorTransition(transition.GetValue());
        ASTERIA_EXPECT(runner, "only the current sequence receipt activates, detaches, and releases its CPU",
                       activated.HasValue(), invalidDetach == AddressSpaceError::InvalidProcessorTransitionReceipt,
                       detached == AddressSpaceError::None, released == AddressSpaceError::None,
                       staleRelease == AddressSpaceError::InvalidProcessorTransitionReceipt,
                       fixture.Space.GetSnapshot().TransitionSequence == 0U,
                       !fixture.Space.GetSnapshot().TransitionProcessor.IsValid());

        const auto begin = fixture.Space.BeginFaultMapping(mapping.Preparation);
        const auto complete =
            begin.HasValue()
                ? fixture.Space.CompleteFaultMapping(begin.GetValue().GetToken())
                : decltype(fixture.Space.CompleteFaultMapping(Kernel::Memory::AddressSpaceMappingToken{}))::Failure({});
        ASTERIA_EXPECT(runner, "successful transition release permits the blocked mapping to retry", begin.HasValue(),
                       complete.HasValue(), fixture.PageTable.MapCalls == (mapCallsBefore + 1U));
        if (complete.HasValue())
        {
            mapping.Commit();
        }
    }

    {
        AddressSpaceFixture source{runner};
        AddressSpaceFixture destination{runner};
        if (!source.IsReady() || !destination.IsReady())
        {
            return;
        }

        const auto sourceHeld = source.Space.ReserveProcessorTransition(CpuId{0U});
        const auto destinationHeld = destination.Space.ReserveProcessorTransition(CpuId{1U});
        if (!sourceHeld.HasValue() || !destinationHeld.HasValue())
        {
            ASTERIA_EXPECT(runner, "opposing transition fixture reserves both roots", false);
            return;
        }

        const auto firstOpposing = destination.Space.ReserveProcessorTransition(CpuId{0U});
        const auto secondOpposing = source.Space.ReserveProcessorTransition(CpuId{1U});
        const auto wrongRootActivation = source.Space.ActivateReservedProcessor(destinationHeld.GetValue());
        ASTERIA_EXPECT(runner, "opposing CPU transitions reserve both roots without waiting", firstOpposing.HasValue(),
                       secondOpposing.HasValue(), !wrongRootActivation.HasValue(),
                       wrongRootActivation.GetError().Error == AddressSpaceError::InvalidProcessorTransitionReceipt);

        if (firstOpposing.HasValue())
        {
            static_cast<void>(destination.Space.ReleaseProcessorTransition(firstOpposing.GetValue()));
        }
        if (secondOpposing.HasValue())
        {
            static_cast<void>(source.Space.ReleaseProcessorTransition(secondOpposing.GetValue()));
        }
        ASTERIA_EXPECT(runner, "opposing contention leaves both first reservations releasable",
                       source.Space.ReleaseProcessorTransition(sourceHeld.GetValue()) == AddressSpaceError::None,
                       destination.Space.ReleaseProcessorTransition(destinationHeld.GetValue()) ==
                           AddressSpaceError::None);
    }
}

void TestShootdownAndMappingPublication(TestContext& runner) noexcept
{
    AddressSpaceFixture fixture{runner};
    if (!fixture.IsReady())
    {
        return;
    }
    ASTERIA_EXPECT(runner, "three processors become residents of one AddressSpace",
                   ActivateProcessor(fixture.Space, CpuId{0U}), ActivateProcessor(fixture.Space, CpuId{1U}),
                   ActivateProcessor(fixture.Space, CpuId{3U}));

    PreparedFault mapping{fixture, runner, 0x0040'0000U, 2U, ReadWriteUser(), FaultAccess::Write};
    if (!mapping.Prepared)
    {
        return;
    }
    const auto begin = fixture.Space.BeginFaultMapping(mapping.Preparation);
    ASTERIA_EXPECT(runner, "page-table commit returns an exact three-CPU shootdown request", begin.HasValue(),
                   begin.GetValue().GetInvalidationStart().GetValue() == 0x0040'0000U,
                   begin.GetValue().GetInvalidationPages().GetValue() == 2U,
                   fixture.Space.GetSnapshot().TargetProcessorMask == 0x0BU, fixture.PageTable.MapCalls == 1U,
                   fixture.PageTable.InstalledMappings == 1U);
    if (!begin.HasValue())
    {
        return;
    }
    const auto request = begin.GetValue();
    const auto pending = fixture.Space.GetSnapshot();
    ASTERIA_EXPECT(runner, "provisional mapping is hidden until every target acknowledges",
                   pending.State == AddressSpaceState::ShootdownPending, pending.MappingCount == 3U,
                   pending.ShootdownSequence == request.GetToken().GetSequence(), pending.TargetProcessorMask == 0x0BU,
                   pending.AcknowledgedProcessorMask == 0U, !fixture.Space.TryGetMapping(3U).HasValue());
    const auto blockedActivation = fixture.Space.ActivateProcessor(CpuId{2U});
    ASTERIA_EXPECT(runner, "pending shootdown freezes residency and cannot publish early",
                   !fixture.Space.CompleteFaultMapping(request.GetToken()).HasValue(),
                   fixture.Space.CompleteFaultMapping(request.GetToken()).GetError().Error ==
                       AddressSpaceError::ShootdownIncomplete,
                   !blockedActivation.HasValue(),
                   blockedActivation.GetError().Error == AddressSpaceError::ShootdownPending,
                   !blockedActivation.GetError().ProcessorMayBeResident,
                   fixture.Space.DetachProcessor(CpuId{0U}) == AddressSpaceError::ShootdownPending);
    ASTERIA_EXPECT(runner, "non-target and invalid shootdown acknowledgements are rejected",
                   fixture.Space.AcknowledgeShootdown(request.GetToken(), CpuId{2U}) ==
                       AddressSpaceError::ProcessorNotTargeted,
                   fixture.Space.AcknowledgeShootdown(Kernel::Memory::AddressSpaceMappingToken{}, CpuId{0U}) ==
                       AddressSpaceError::InvalidShootdownToken);

    std::atomic<bool> start{false};
    AddressSpaceError acknowledgements[3]{AddressSpaceError::Faulted, AddressSpaceError::Faulted,
                                          AddressSpaceError::Faulted};
    std::thread workers[3];
    constexpr Foundation::UInt8 cpuValues[3]{0U, 1U, 3U};
    for (Foundation::Size index{0U}; index < 3U; ++index)
    {
        workers[index] = std::thread{[&, index]() noexcept
                                     {
                                         while (!start.load(std::memory_order_acquire))
                                         {
                                             std::this_thread::yield();
                                         }
                                         acknowledgements[index] = fixture.Space.AcknowledgeShootdown(
                                             request.GetToken(), CpuId{cpuValues[index]});
                                     }};
    }
    start.store(true, std::memory_order_release);
    for (std::thread& worker : workers)
    {
        worker.join();
    }
    ASTERIA_EXPECT(runner, "concurrent and duplicate acknowledgements are generation-safe",
                   acknowledgements[0] == AddressSpaceError::None, acknowledgements[1] == AddressSpaceError::None,
                   acknowledgements[2] == AddressSpaceError::None,
                   fixture.Space.GetSnapshot().AcknowledgedProcessorMask == 0x0BU,
                   fixture.Space.AcknowledgeShootdown(request.GetToken(), CpuId{1U}) == AddressSpaceError::None);

    const auto complete = fixture.Space.CompleteFaultMapping(request.GetToken());
    const auto record = fixture.Space.TryGetMapping(3U);
    ASTERIA_EXPECT(runner, "all acknowledgements atomically publish the retained mapping record", complete.HasValue(),
                   complete.GetValue().MappingSequence == request.GetToken().GetSequence(),
                   complete.GetValue().MappingCount == 4U,
                   fixture.Space.GetSnapshot().State == AddressSpaceState::Ready,
                   fixture.Space.GetSnapshot().ShootdownSequence == 0U, record.HasValue(),
                   RecordIsExpectedRuntimeMapping(record.GetValue(), fixture.GetMemoryObjectId(), fixture.GetFrameId(),
                                                  0x0040'0000U, 0x0200'2000U, ReadWriteUser()),
                   fixture.MemoryPolicy.Object.GetSnapshot().ActiveMappings == 1U,
                   fixture.MemoryPolicy.Object.Quiesce().Error == MemoryObjectError::InUse);
    ASTERIA_EXPECT(runner, "completed shootdown generation cannot be acknowledged again",
                   fixture.Space.AcknowledgeShootdown(request.GetToken(), CpuId{0U}) ==
                       AddressSpaceError::InvalidShootdownToken);
    mapping.Commit();
}

void TestShootdownDispatcherSuccess(TestContext& runner) noexcept
{
    AddressSpaceFixture fixture{runner};
    if (!fixture.IsReady())
    {
        return;
    }
    ASTERIA_EXPECT(runner, "dispatcher fixture activates local and remote processors",
                   ActivateProcessor(fixture.Space, CpuId{0U}), ActivateProcessor(fixture.Space, CpuId{1U}),
                   ActivateProcessor(fixture.Space, CpuId{3U}));

    PreparedFault mapping{fixture, runner, 0x0060'0000U, 4U, ReadWriteUser(), FaultAccess::Write};
    if (!mapping.Prepared)
    {
        return;
    }
    const auto begin = fixture.Space.BeginFaultMapping(mapping.Preparation);
    ASTERIA_EXPECT(runner, "dispatcher begins one retained mapping", begin.HasValue());
    if (!begin.HasValue())
    {
        return;
    }

    TestLocalTlbInvalidator localInvalidator;
    TlbShootdownDispatcher localDispatcher{CpuId{1U}, localInvalidator};
    TestTlbShootdownTransport transport;
    const auto dispatch = localDispatcher.Dispatch(fixture.Space, begin.GetValue(), transport);
    ASTERIA_EXPECT(runner, "dispatcher invalidates locally and publishes one all-or-none remote batch",
                   dispatch.HasValue(), dispatch.GetValue().QueuedProcessorMask == 0x09U,
                   dispatch.GetValue().AcknowledgedProcessorMask == 0x02U,
                   dispatch.GetValue().Processor.GetValue() == 1U, !dispatch.GetValue().RejectionObserved,
                   localInvalidator.Calls == 1U, localInvalidator.LastStart.GetValue() == 0x0060'0000U,
                   localInvalidator.LastPages.GetValue() == 2U, transport.PublishCalls == 1U, transport.HasBatch,
                   transport.Captured.GetCount() == 2U);
    if (!dispatch.HasValue() || !transport.HasBatch || (transport.Captured.GetCount() != 2U))
    {
        if (fixture.Space.GetSnapshot().State == AddressSpaceState::ShootdownPending)
        {
            static_cast<void>(fixture.Space.FailShootdown(begin.GetValue().GetToken()));
        }
        return;
    }

    const auto& first = transport.Captured.GetWork(0U);
    const auto& second = transport.Captured.GetWork(1U);
    ASTERIA_EXPECT(runner, "remote work preserves the exact opaque generation, range, and CPU order",
                   first.GetOwner() == &fixture.Space, first.GetToken().IsEqualTo(begin.GetValue().GetToken()),
                   first.GetInvalidationStart().GetValue() == 0x0060'0000U,
                   first.GetInvalidationPages().GetValue() == 2U, first.GetTargetProcessor().GetValue() == 0U,
                   second.GetOwner() == &fixture.Space, second.GetToken().IsEqualTo(begin.GetValue().GetToken()),
                   second.GetInvalidationStart().GetValue() == 0x0060'0000U,
                   second.GetInvalidationPages().GetValue() == 2U, second.GetTargetProcessor().GetValue() == 3U);

    std::atomic<bool> start{false};
    bool remoteSuccess[2]{false, false};
    TestLocalTlbInvalidator remoteInvalidators[2];
    std::thread workers[2];
    for (Foundation::Size index{0U}; index < 2U; ++index)
    {
        workers[index] = std::thread{
            [&, index]() noexcept
            {
                while (!start.load(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }
                TlbShootdownDispatcher remoteDispatcher{transport.Captured.GetWork(index).GetTargetProcessor(),
                                                        remoteInvalidators[index]};
                const auto result = remoteDispatcher.HandleRemote(transport.Captured.GetWork(index));
                remoteSuccess[index] =
                    result.HasValue() &&
                    (result.GetValue().AcknowledgedProcessorMask ==
                     static_cast<Foundation::UInt8>(
                         Foundation::UInt8{1U} << transport.Captured.GetWork(index).GetTargetProcessor().GetValue())) &&
                    !result.GetValue().RejectionObserved;
            }};
    }
    start.store(true, std::memory_order_release);
    for (std::thread& worker : workers)
    {
        worker.join();
    }
    ASTERIA_EXPECT(runner, "concurrent remote handlers invalidate before acknowledging every target", remoteSuccess[0],
                   remoteSuccess[1], remoteInvalidators[0].Calls == 1U, remoteInvalidators[1].Calls == 1U,
                   fixture.Space.GetSnapshot().AcknowledgedProcessorMask == 0x0BU);

    const auto complete = fixture.Space.CompleteFaultMapping(begin.GetValue().GetToken());
    ASTERIA_EXPECT(runner, "the initiator publishes only after local and remote completions drain", complete.HasValue(),
                   complete.GetValue().MappingCount == 4U,
                   fixture.Space.GetSnapshot().State == AddressSpaceState::Ready);
    if (complete.HasValue())
    {
        mapping.Commit();
    }
}

void TestDetachAcknowledges(TestContext& runner) noexcept
{
    AddressSpaceFixture fixture{runner};
    if (!fixture.IsReady())
    {
        return;
    }
    const bool resident = ActivateProcessor(fixture.Space, CpuId{0U}) && ActivateProcessor(fixture.Space, CpuId{1U});
    PreparedFault mapping{fixture, runner, 0x0066'0000U, 13U, ReadWriteUser(), FaultAccess::Write};
    const auto request = fixture.Space.BeginFaultMapping(mapping.Preparation);
    if (!resident || !request.HasValue())
    {
        ASTERIA_EXPECT(runner, "detach acknowledgement setup", false);
        return;
    }
    fixture.PageTable.AcknowledgeOnDetach = true;
    const auto reserved = fixture.Space.ReserveProcessorTransition(CpuId{1U});
    const bool detached =
        reserved.HasValue() && fixture.Space.DetachReservedProcessor(reserved.GetValue()) == AddressSpaceError::None;
    const auto pending = fixture.Space.GetSnapshot();
    const bool released =
        reserved.HasValue() && fixture.Space.ReleaseProcessorTransition(reserved.GetValue()) == AddressSpaceError::None;
    ASTERIA_EXPECT(runner, "an ISA-proven root switch acknowledges its pending target before clearing residency",
                   detached, released, pending.ResidentProcessorMask == 1U, pending.AcknowledgedProcessorMask == 2U,
                   !fixture.Space.CompleteMapping(request.GetValue().GetToken()).HasValue());
    const bool completed =
        fixture.Space.AcknowledgeShootdown(request.GetValue().GetToken(), CpuId{0U}) == AddressSpaceError::None &&
        fixture.Space.CompleteMapping(request.GetValue().GetToken()).HasValue();
    ASTERIA_EXPECT(runner, "detach cannot publish until the remaining CPU acknowledges", completed);
    if (completed)
    {
        mapping.Commit();
    }
}

// A source-root retry must not consume an acknowledgement ahead
// of its queued remote mailbox work.
void TestPendingSourceRootRetry(TestContext& runner) noexcept
{
    AddressSpaceFixture source{runner};
    AddressSpaceFixture idle{runner};
    if (!source.IsReady() || !idle.IsReady())
    {
        return;
    }
    Kernel::Memory::ProcessorAddressSpaceController controller;
    if (!controller.Initialize(CpuId{1U}, idle.Space).HasValue() || !controller.SwitchTo(source.Space).HasValue() ||
        !ActivateProcessor(source.Space, CpuId{0U}))
    {
        ASTERIA_EXPECT(runner, "pending-source setup", false);
        return;
    }
    source.PageTable.AcknowledgeOnDetach = true;
    PreparedFault mapping{source, runner, 0x0067'0000U, 14U, ReadWriteUser(), FaultAccess::Write};
    const auto request = source.Space.BeginFaultMapping(mapping.Preparation);
    if (!request.HasValue())
    {
        ASTERIA_EXPECT(runner, "pending-source mapping setup", false);
        return;
    }
    // A resident pending root is a retry, never a
    // Faulted controller with shared-root AP users.
    const auto reserved = source.Space.ReserveProcessorTransition(CpuId{1U});
    ASTERIA_EXPECT(runner, "pending resident can reserve its transition", reserved.HasValue());
    if (reserved.HasValue())
    {
        ASTERIA_EXPECT(runner, "a reservation excludes a new mapping transaction",
                       source.Space.BeginFaultMapping(mapping.Preparation).GetError().Error ==
                           AddressSpaceError::ProcessorTransitionPending);
        static_cast<void>(source.Space.ReleaseProcessorTransition(reserved.GetValue()));
    }
    for (Foundation::Size attempt{0U}; attempt < 4U; ++attempt)
    {
        const auto retry = controller.SwitchToIdle();
        ASTERIA_EXPECT(runner, "pending-root retry never faults or retains a reservation", !retry.HasValue(),
                       retry.GetError().AdmissionRollbackSafe,
                       controller.GetSnapshot().State == Kernel::Memory::ProcessorAddressSpaceControllerState::Ready,
                       source.Space.GetSnapshot().ResidentProcessorMask == 3U,
                       !controller.GetSnapshot().SourceTransition.IsValid());
    }
    const auto deferred = controller.SwitchToIdle();
    const auto pending = source.Space.GetSnapshot();
    ASTERIA_EXPECT(runner, "pending source stays resident without acknowledging or changing CR3", !deferred.HasValue(),
                   deferred.GetError().AdmissionRollbackSafe,
                   deferred.GetError().Transition == AddressSpaceError::ShootdownPending,
                   pending.ResidentProcessorMask == 3U, pending.AcknowledgedProcessorMask == 0U,
                   controller.GetSnapshot().ActiveStorage == &source.Space,
                   !controller.GetSnapshot().SourceTransition.IsValid());
    TestTlbShootdownNotifier notifier;
    TlbShootdownMailboxTransport mailbox{notifier, 3U};
    TestLocalTlbInvalidator local;
    TestLocalTlbInvalidator remote;
    TlbShootdownDispatcher publisher{CpuId{0U}, local};
    TlbShootdownDispatcher receiver{CpuId{1U}, remote};
    TlbShootdownDrain drain{mailbox, receiver};
    const bool delivered =
        publisher.Dispatch(source.Space, request.GetValue(), mailbox).HasValue() && drain.Drain().CompletedCount == 1U;
    const auto completed = source.Space.CompleteMapping(request.GetValue().GetToken());
    ASTERIA_EXPECT(runner, "actual remote invalidation completes before idle-root admission retries", delivered,
                   remote.Calls == 1U, completed.HasValue(), controller.SwitchToIdle().HasValue(),
                   source.Space.GetSnapshot().ResidentProcessorMask == 1U,
                   controller.GetSnapshot().ActiveStorage == &idle.Space);
    if (completed.HasValue())
    {
        mapping.Commit();
    }
}

void TestMutualInitiatorProgress(TestContext& runner) noexcept
{
    AddressSpaceFixture first{runner};
    AddressSpaceFixture second{runner};
    if (!first.IsReady() || !second.IsReady())
    {
        return;
    }
    const bool resident = ActivateProcessor(first.Space, CpuId{0U}) && ActivateProcessor(first.Space, CpuId{1U}) &&
                          ActivateProcessor(second.Space, CpuId{0U}) && ActivateProcessor(second.Space, CpuId{1U});
    PreparedFault firstMapping{first, runner, 0x0064'0000U, 11U, ReadWriteUser(), FaultAccess::Write};
    PreparedFault secondMapping{second, runner, 0x0065'0000U, 12U, ReadWriteUser(), FaultAccess::Write};
    const auto firstRequest = first.Space.BeginFaultMapping(firstMapping.Preparation);
    const auto secondRequest = second.Space.BeginFaultMapping(secondMapping.Preparation);
    if (!resident || !firstRequest.HasValue() || !secondRequest.HasValue())
    {
        ASTERIA_EXPECT(runner, "two initiator setup", false);
        return;
    }
    TestTlbShootdownNotifier notifier;
    TlbShootdownMailboxTransport mailbox{notifier, 3U};
    TestLocalTlbInvalidator invalidators[2];
    TlbShootdownDispatcher dispatchers[2]{TlbShootdownDispatcher{CpuId{0U}, invalidators[0]},
                                          TlbShootdownDispatcher{CpuId{1U}, invalidators[1]}};
    TlbShootdownDrain drains[2]{TlbShootdownDrain{mailbox, dispatchers[0]}, TlbShootdownDrain{mailbox, dispatchers[1]}};
    AddressSpace* spaces[2]{&first.Space, &second.Space};
    const Kernel::Memory::AddressSpaceShootdownRequest requests[2]{firstRequest.GetValue(), secondRequest.GetValue()};
    std::atomic<unsigned> published{0U};
    std::atomic<unsigned> completed{0U};
    bool finished[2]{};
    std::thread workers[2];
    for (Foundation::UInt8 cpu{0U}; cpu < 2U; ++cpu)
    {
        workers[cpu] = std::thread(
            [&, cpu]
            {
                Platform::Synchronization::BindProcessorProgress(
                    CpuId{cpu},
                    Platform::Synchronization::ProcessorProgressHooks{
                        &drains[cpu], [](void* context) noexcept
                        {
                            static_cast<void>(static_cast<TlbShootdownDrain*>(context)->Drain());
                        }});
                const auto dispatched = dispatchers[cpu].Dispatch(*spaces[cpu], requests[cpu], mailbox);
                published.fetch_add(1U, std::memory_order_release);
                while (published.load(std::memory_order_acquire) != 2U)
                {
                    std::this_thread::yield();
                }
                for (Kernel::Scheduler::CrossProcessorWait wait; dispatched.HasValue();)
                {
                    if (spaces[cpu]->CompleteMapping(requests[cpu].GetToken()).HasValue())
                    {
                        finished[cpu] = true;
                        break;
                    }
                    if (!wait.Poll())
                    {
                        break;
                    }
                }
                completed.fetch_add(1U, std::memory_order_release);
                while (completed.load(std::memory_order_acquire) != 2U)
                {
                    Platform::Synchronization::ProcessorWaitProgress();
                    std::this_thread::yield();
                }
            });
    }
    for (auto& worker : workers)
    {
        worker.join();
    }
    ASTERIA_EXPECT(runner, "interrupts-off initiators targeting each other finish by draining their own mailboxes",
                   finished[0], finished[1]);
    if (finished[0])
    {
        firstMapping.Commit();
    }
    if (finished[1])
    {
        secondMapping.Commit();
    }
}

void TestShootdownMailboxTransport(TestContext& runner) noexcept
{
    {
        AddressSpaceFixture fixture{runner};
        if (!fixture.IsReady())
        {
            return;
        }
        ASTERIA_EXPECT(runner, "mailbox fixture activates one local and two remote processors",
                       ActivateProcessor(fixture.Space, CpuId{0U}), ActivateProcessor(fixture.Space, CpuId{1U}),
                       ActivateProcessor(fixture.Space, CpuId{2U}));

        PreparedFault mapping{fixture, runner, 0x0064'0000U, 11U, ReadWriteUser(), FaultAccess::Write};
        if (!mapping.Prepared)
        {
            return;
        }
        const auto begin = fixture.Space.BeginFaultMapping(mapping.Preparation);
        if (!begin.HasValue())
        {
            ASTERIA_EXPECT(runner, "mailbox fixture begins one mapping", false);
            return;
        }

        TestLocalTlbInvalidator localInvalidator;
        TlbShootdownDispatcher localDispatcher{CpuId{0U}, localInvalidator};
        TestTlbShootdownTransport capture;
        const auto dispatch = localDispatcher.Dispatch(fixture.Space, begin.GetValue(), capture);
        ASTERIA_EXPECT(runner, "dispatcher supplies one legitimate two-target mailbox batch", dispatch.HasValue(),
                       capture.HasBatch, capture.Captured.GetCount() == 2U);
        if (!dispatch.HasValue() || !capture.HasBatch || (capture.Captured.GetCount() != 2U))
        {
            return;
        }

        TestTlbShootdownNotifier notifier;
        TlbShootdownMailboxTransport mailbox{notifier, 0x07U};
        TestLocalTlbInvalidator processorOneInvalidator;
        TestLocalTlbInvalidator processorTwoInvalidator;
        TlbShootdownDispatcher processorOne{CpuId{1U}, processorOneInvalidator};
        TlbShootdownDispatcher processorTwo{CpuId{2U}, processorTwoInvalidator};

        bool filledExactly{true};
        for (Foundation::UInt8 index{0U}; index < TlbShootdownMailboxSlotCount; ++index)
        {
            TlbShootdownWork work{};
            const auto publishError = mailbox.Publish(capture.Captured);
            const auto receiveError = mailbox.TryReceive(CpuId{2U}, work);
            const auto handled = (receiveError == TlbShootdownReceiveError::None)
                                     ? processorTwo.HandleRemote(work)
                                     : decltype(processorTwo.HandleRemote(work))::Failure({});
            filledExactly = filledExactly && (publishError == TlbShootdownTransportError::None) &&
                            (receiveError == TlbShootdownReceiveError::None) && handled.HasValue();
        }

        TlbShootdownWork unchanged{};
        const auto fullError = mailbox.Publish(capture.Captured);
        const auto emptyProcessorTwo = mailbox.TryReceive(CpuId{2U}, unchanged);
        const auto offlineProcessor = mailbox.TryReceive(CpuId{3U}, unchanged);
        ASTERIA_EXPECT(runner, "one full target rejects the whole batch without publishing to an empty target",
                       filledExactly, fullError == TlbShootdownTransportError::QueueFull,
                       emptyProcessorTwo == TlbShootdownReceiveError::Empty,
                       offlineProcessor == TlbShootdownReceiveError::InvalidProcessor, unchanged.GetOwner() == nullptr,
                       notifier.Calls.load(std::memory_order_acquire) == TlbShootdownMailboxSlotCount,
                       notifier.LastMask.load(std::memory_order_acquire) == 0x06U);

        TlbShootdownDrain processorOneDrain{mailbox, processorOne};
        TlbShootdownDrain processorTwoDrain{mailbox, processorTwo};
        const auto firstDrain = processorOneDrain.Drain();
        const bool drainedExactly =
            (firstDrain.ReceivedCount == TlbShootdownMailboxSlotCount) &&
            (firstDrain.CompletedCount == TlbShootdownMailboxSlotCount) && (firstDrain.RejectedCount == 0U) &&
            (firstDrain.ReceiveError == TlbShootdownReceiveError::None) &&
            (firstDrain.FirstRemoteError == TlbShootdownError::None) && (firstDrain.Processor.GetValue() == 1U);
        TlbShootdownWork emptyWork{};
        const bool processorOneEmpty = mailbox.TryReceive(CpuId{1U}, emptyWork) == TlbShootdownReceiveError::Empty;

        const bool reused = mailbox.Publish(capture.Captured) == TlbShootdownTransportError::None;
        processorTwoInvalidator.NextError = LocalTlbInvalidationError::InvalidRange;
        const auto reusedOne = processorOneDrain.Drain();
        const auto reusedTwo = processorTwoDrain.Drain();
        ASTERIA_EXPECT(runner, "bounded drains complete or reject every received work before slot reuse",
                       drainedExactly, processorOneEmpty, reused, reusedOne.ReceivedCount == 1U,
                       reusedOne.CompletedCount == 1U, reusedOne.RejectedCount == 0U, reusedTwo.ReceivedCount == 1U,
                       reusedTwo.CompletedCount == 0U, reusedTwo.RejectedCount == 1U,
                       reusedTwo.FirstRemoteError == TlbShootdownError::LocalInvalidationRejected,
                       reusedTwo.FirstLocalError == LocalTlbInvalidationError::InvalidRange,
                       reusedTwo.FirstAddressSpaceError == AddressSpaceError::None,
                       notifier.Calls.load(std::memory_order_acquire) ==
                           static_cast<Foundation::Size>(TlbShootdownMailboxSlotCount + 1U));

        const auto complete = fixture.Space.CompleteFaultMapping(begin.GetValue().GetToken());
        ASTERIA_EXPECT(runner, "mailbox delivery completes the retained AddressSpace generation", complete.HasValue(),
                       fixture.Space.GetSnapshot().State == AddressSpaceState::Ready);
        if (complete.HasValue())
        {
            mapping.Commit();
        }
    }

    {
        AddressSpaceFixture fixture{runner};
        if (!fixture.IsReady() || !ActivateProcessor(fixture.Space, CpuId{1U}))
        {
            return;
        }
        PreparedFault mapping{fixture, runner, 0x0066'0000U, 13U, ReadWriteUser(), FaultAccess::Write};
        if (!mapping.Prepared)
        {
            return;
        }
        const auto begin = fixture.Space.BeginFaultMapping(mapping.Preparation);
        if (!begin.HasValue())
        {
            return;
        }

        TestLocalTlbInvalidator originInvalidator;
        TlbShootdownDispatcher origin{CpuId{0U}, originInvalidator};
        TestTlbShootdownTransport capture;
        const auto dispatch = origin.Dispatch(fixture.Space, begin.GetValue(), capture);
        if (!dispatch.HasValue() || !capture.HasBatch || (capture.Captured.GetCount() != 1U))
        {
            ASTERIA_EXPECT(runner, "concurrent mailbox fixture captures one target", false);
            return;
        }

        TestTlbShootdownNotifier notifier;
        TlbShootdownMailboxTransport mailbox{notifier, 0x02U};
        TestLocalTlbInvalidator remoteInvalidator;
        TlbShootdownDispatcher remote{CpuId{1U}, remoteInvalidator};
        constexpr Foundation::Size workPerProducer{64U};
        constexpr Foundation::Size producerCount{2U};
        constexpr Foundation::Size expectedWork{workPerProducer * producerCount};
        std::atomic<bool> start{false};
        std::atomic<Foundation::Size> completedProducers{0U};
        std::atomic<Foundation::Size> published{0U};
        std::atomic<Foundation::Size> received{0U};
        std::atomic<bool> failed{false};

        std::thread producers[producerCount];
        for (Foundation::Size producerIndex{0U}; producerIndex < producerCount; ++producerIndex)
        {
            producers[producerIndex] =
                std::thread{[&]() noexcept
                            {
                                while (!start.load(std::memory_order_acquire))
                                {
                                    std::this_thread::yield();
                                }
                                for (Foundation::Size workIndex{0U}; workIndex < workPerProducer; ++workIndex)
                                {
                                    bool retained{false};
                                    for (Foundation::Size attempt{0U}; attempt < 100000U; ++attempt)
                                    {
                                        const auto error = mailbox.Publish(capture.Captured);
                                        if (error == TlbShootdownTransportError::None)
                                        {
                                            static_cast<void>(published.fetch_add(1U, std::memory_order_acq_rel));
                                            retained = true;
                                            break;
                                        }
                                        if (error != TlbShootdownTransportError::QueueFull)
                                        {
                                            break;
                                        }
                                        std::this_thread::yield();
                                    }
                                    if (!retained)
                                    {
                                        failed.store(true, std::memory_order_release);
                                        break;
                                    }
                                }
                                static_cast<void>(completedProducers.fetch_add(1U, std::memory_order_acq_rel));
                            }};
        }

        std::thread consumer{
            [&]() noexcept
            {
                while (!start.load(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }
                for (Foundation::Size attempt{0U}; attempt < 500000U; ++attempt)
                {
                    TlbShootdownWork work{};
                    const auto receiveError = mailbox.TryReceive(CpuId{1U}, work);
                    if (receiveError == TlbShootdownReceiveError::None)
                    {
                        if (!remote.HandleRemote(work).HasValue())
                        {
                            failed.store(true, std::memory_order_release);
                            break;
                        }
                        const Foundation::Size total = received.fetch_add(1U, std::memory_order_acq_rel) + 1U;
                        if (total == expectedWork)
                        {
                            break;
                        }
                        continue;
                    }
                    if (receiveError != TlbShootdownReceiveError::Empty)
                    {
                        failed.store(true, std::memory_order_release);
                        break;
                    }
                    if ((completedProducers.load(std::memory_order_acquire) == producerCount) &&
                        (received.load(std::memory_order_acquire) == published.load(std::memory_order_acquire)))
                    {
                        break;
                    }
                    std::this_thread::yield();
                }
            }};

        start.store(true, std::memory_order_release);
        for (std::thread& producer : producers)
        {
            producer.join();
        }
        consumer.join();

        TlbShootdownWork remaining{};
        const bool empty = mailbox.TryReceive(CpuId{1U}, remaining) == TlbShootdownReceiveError::Empty;
        const auto complete = fixture.Space.CompleteFaultMapping(begin.GetValue().GetToken());
        ASTERIA_EXPECT(runner, "concurrent publishers and one CPU-local consumer drain every atomic batch exactly once",
                       !failed.load(std::memory_order_acquire),
                       published.load(std::memory_order_acquire) == expectedWork,
                       received.load(std::memory_order_acquire) == expectedWork,
                       notifier.Calls.load(std::memory_order_acquire) == expectedWork,
                       notifier.ObservedMask.load(std::memory_order_acquire) == 0x02U, empty, complete.HasValue());
        if (complete.HasValue())
        {
            mapping.Commit();
        }
    }

    {
        AddressSpaceFixture fixture{runner};
        if (!fixture.IsReady() || !ActivateProcessor(fixture.Space, CpuId{1U}))
        {
            return;
        }
        PreparedFault mapping{fixture, runner, 0x006A'0000U, 15U, ReadWriteUser(), FaultAccess::Write};
        if (!mapping.Prepared)
        {
            return;
        }
        const auto begin = fixture.Space.BeginFaultMapping(mapping.Preparation);
        if (!begin.HasValue())
        {
            return;
        }

        TestLocalTlbInvalidator originInvalidator;
        TlbShootdownDispatcher origin{CpuId{0U}, originInvalidator};
        TestTlbShootdownTransport capture;
        const auto dispatch = origin.Dispatch(fixture.Space, begin.GetValue(), capture);
        if (!dispatch.HasValue() || !capture.HasBatch || (capture.Captured.GetCount() != 1U))
        {
            ASTERIA_EXPECT(runner, "failure-drain fixture captures one target", false);
            return;
        }

        TestTlbShootdownNotifier notifier;
        TlbShootdownMailboxTransport mailbox{notifier, 0x02U};
        const bool published = (mailbox.Publish(capture.Captured) == TlbShootdownTransportError::None) &&
                               (mailbox.Publish(capture.Captured) == TlbShootdownTransportError::None) &&
                               (mailbox.Publish(capture.Captured) == TlbShootdownTransportError::None);
        TestLocalTlbInvalidator scriptedInvalidator;
        scriptedInvalidator.ScriptedErrors[0U] = LocalTlbInvalidationError::InvalidRange;
        scriptedInvalidator.ScriptedErrors[1U] = LocalTlbInvalidationError::None;
        scriptedInvalidator.ScriptedErrors[2U] = LocalTlbInvalidationError::RangeTooLarge;
        scriptedInvalidator.ScriptedErrorCount = 3U;
        TlbShootdownDispatcher remote{CpuId{1U}, scriptedInvalidator};
        TlbShootdownDrain drain{mailbox, remote};
        const auto summary = drain.Drain();
        TlbShootdownWork remaining{};
        const bool empty = mailbox.TryReceive(CpuId{1U}, remaining) == TlbShootdownReceiveError::Empty;
        const auto complete = fixture.Space.CompleteFaultMapping(begin.GetValue().GetToken());
        ASTERIA_EXPECT(runner, "one rejected work preserves its first error while later mailbox work still drains",
                       published, summary.ReceivedCount == 3U, summary.CompletedCount == 1U,
                       summary.RejectedCount == 2U, summary.ReceiveError == TlbShootdownReceiveError::None,
                       summary.FirstRemoteError == TlbShootdownError::LocalInvalidationRejected,
                       summary.FirstLocalError == LocalTlbInvalidationError::InvalidRange,
                       scriptedInvalidator.Calls == 3U, empty, !complete.HasValue(),
                       complete.GetError().Error == AddressSpaceError::ShootdownFailed);
    }
}

void TestShootdownFirstOutcomeWins(TestContext& runner) noexcept
{
    {
        AddressSpaceFixture fixture{runner};
        if (!fixture.IsReady() || !ActivateProcessor(fixture.Space, CpuId{0U}))
        {
            return;
        }
        PreparedFault mapping{fixture, runner, 0x0068'0000U, 5U, ReadWriteUser(), FaultAccess::Write};
        if (!mapping.Prepared)
        {
            return;
        }
        const auto begin = fixture.Space.BeginFaultMapping(mapping.Preparation);
        if (!begin.HasValue())
        {
            return;
        }
        const auto token = begin.GetValue().GetToken();
        const AddressSpaceError acknowledged = fixture.Space.AcknowledgeShootdown(token, CpuId{0U});
        const AddressSpaceError lateRejection = fixture.Space.RejectShootdown(token, CpuId{0U});
        const auto complete = fixture.Space.CompleteFaultMapping(token);
        ASTERIA_EXPECT(runner, "a completed CPU acknowledgement cannot be changed by a late duplicate rejection",
                       acknowledged == AddressSpaceError::None, lateRejection == AddressSpaceError::None,
                       complete.HasValue(), fixture.Space.GetSnapshot().State == AddressSpaceState::Ready);
        if (complete.HasValue())
        {
            mapping.Commit();
        }
    }

    {
        AddressSpaceFixture fixture{runner};
        if (!fixture.IsReady() || !ActivateProcessor(fixture.Space, CpuId{0U}))
        {
            return;
        }
        PreparedFault mapping{fixture, runner, 0x006C'0000U, 7U, ReadWriteUser(), FaultAccess::Write};
        if (!mapping.Prepared)
        {
            return;
        }
        const auto begin = fixture.Space.BeginFaultMapping(mapping.Preparation);
        if (!begin.HasValue())
        {
            return;
        }
        const auto token = begin.GetValue().GetToken();
        const AddressSpaceError rejected = fixture.Space.RejectShootdown(token, CpuId{0U});
        const AddressSpaceError lateAcknowledgement = fixture.Space.AcknowledgeShootdown(token, CpuId{0U});
        const auto complete = fixture.Space.CompleteFaultMapping(token);
        ASTERIA_EXPECT(runner, "a first CPU rejection remains terminal after a late duplicate acknowledgement",
                       rejected == AddressSpaceError::None, lateAcknowledgement == AddressSpaceError::ShootdownFailed,
                       !complete.HasValue(), complete.GetError().Error == AddressSpaceError::ShootdownFailed,
                       fixture.Space.GetSnapshot().State == AddressSpaceState::Faulted);
    }
}

void TestShootdownDispatcherFailures(TestContext& runner) noexcept
{
    {
        AddressSpaceFixture fixture{runner};
        if (!fixture.IsReady() || !ActivateProcessor(fixture.Space, CpuId{0U}))
        {
            return;
        }
        PreparedFault mapping{fixture, runner, 0x0070'0000U, 6U, ReadWriteUser(), FaultAccess::Write};
        if (!mapping.Prepared)
        {
            return;
        }
        const auto begin = fixture.Space.BeginFaultMapping(mapping.Preparation);
        if (!begin.HasValue())
        {
            return;
        }
        TestLocalTlbInvalidator invalidator;
        invalidator.NextError = LocalTlbInvalidationError::RangeTooLarge;
        TlbShootdownDispatcher dispatcher{CpuId{0U}, invalidator};
        TestTlbShootdownTransport transport;
        const auto dispatch = dispatcher.Dispatch(fixture.Space, begin.GetValue(), transport);
        ASTERIA_EXPECT(runner, "local invalidation failure publishes no remote work and terminalizes the root",
                       !dispatch.HasValue(), dispatch.GetError().Error == TlbShootdownError::LocalInvalidationRejected,
                       dispatch.GetError().LocalError == LocalTlbInvalidationError::RangeTooLarge,
                       dispatch.GetError().AddressSpaceErrorCode == AddressSpaceError::None,
                       transport.PublishCalls == 0U, fixture.Space.GetSnapshot().State == AddressSpaceState::Faulted);
    }

    {
        AddressSpaceFixture fixture{runner};
        if (!fixture.IsReady() || !ActivateProcessor(fixture.Space, CpuId{0U}) ||
            !ActivateProcessor(fixture.Space, CpuId{2U}))
        {
            return;
        }
        PreparedFault mapping{fixture, runner, 0x0080'0000U, 8U, ReadWriteUser(), FaultAccess::Write};
        if (!mapping.Prepared)
        {
            return;
        }
        const auto begin = fixture.Space.BeginFaultMapping(mapping.Preparation);
        if (!begin.HasValue())
        {
            return;
        }
        TestLocalTlbInvalidator invalidator;
        TlbShootdownDispatcher dispatcher{CpuId{0U}, invalidator};
        TestTlbShootdownTransport transport;
        transport.NextError = TlbShootdownTransportError::QueueFull;
        const auto dispatch = dispatcher.Dispatch(fixture.Space, begin.GetValue(), transport);
        ASTERIA_EXPECT(runner, "all-or-none transport rejection retains no work and terminalizes committed state",
                       !dispatch.HasValue(), dispatch.GetError().Error == TlbShootdownError::TransportRejected,
                       dispatch.GetError().TransportError == TlbShootdownTransportError::QueueFull,
                       dispatch.GetError().AddressSpaceErrorCode == AddressSpaceError::None, invalidator.Calls == 1U,
                       transport.PublishCalls == 1U, !transport.HasBatch,
                       fixture.Space.GetSnapshot().State == AddressSpaceState::Faulted);
    }

    {
        AddressSpaceFixture fixture{runner};
        if (!fixture.IsReady() || !ActivateProcessor(fixture.Space, CpuId{0U}) ||
            !ActivateProcessor(fixture.Space, CpuId{1U}) || !ActivateProcessor(fixture.Space, CpuId{3U}))
        {
            return;
        }
        PreparedFault mapping{fixture, runner, 0x0090'0000U, 10U, ReadWriteUser(), FaultAccess::Write};
        if (!mapping.Prepared)
        {
            return;
        }
        const auto begin = fixture.Space.BeginFaultMapping(mapping.Preparation);
        if (!begin.HasValue())
        {
            return;
        }
        TestLocalTlbInvalidator localInvalidator;
        TlbShootdownDispatcher localDispatcher{CpuId{0U}, localInvalidator};
        TestTlbShootdownTransport transport;
        const auto dispatch = localDispatcher.Dispatch(fixture.Space, begin.GetValue(), transport);
        if (!dispatch.HasValue() || !transport.HasBatch || (transport.Captured.GetCount() != 2U))
        {
            if (fixture.Space.GetSnapshot().State == AddressSpaceState::ShootdownPending)
            {
                static_cast<void>(fixture.Space.FailShootdown(begin.GetValue().GetToken()));
            }
            ASTERIA_EXPECT(runner, "remote rejection fixture dispatches two works", false);
            return;
        }

        TestLocalTlbInvalidator rejectingInvalidator;
        rejectingInvalidator.NextError = LocalTlbInvalidationError::InvalidRange;
        TestLocalTlbInvalidator wrongCpuInvalidator;
        TlbShootdownDispatcher wrongCpuDispatcher{CpuId{2U}, wrongCpuInvalidator};
        const auto wrongCpu = wrongCpuDispatcher.HandleRemote(transport.Captured.GetWork(0U));
        TlbShootdownDispatcher rejectingDispatcher{CpuId{1U}, rejectingInvalidator};
        const auto rejected = rejectingDispatcher.HandleRemote(transport.Captured.GetWork(0U));
        TestLocalTlbInvalidator finalInvalidator;
        TlbShootdownDispatcher finalDispatcher{CpuId{3U}, finalInvalidator};
        const auto final = finalDispatcher.HandleRemote(transport.Captured.GetWork(1U));
        const auto complete = fixture.Space.CompleteFaultMapping(begin.GetValue().GetToken());
        ASTERIA_EXPECT(runner, "remote rejection drains every target before the initiator enters terminal state",
                       !wrongCpu.HasValue(), wrongCpu.GetError().Error == TlbShootdownError::InvalidRequest,
                       wrongCpuInvalidator.Calls == 0U, !rejected.HasValue(),
                       rejected.GetError().Error == TlbShootdownError::LocalInvalidationRejected, final.HasValue(),
                       final.GetValue().RejectionObserved,
                       fixture.Space.GetSnapshot().State == AddressSpaceState::Faulted, !complete.HasValue(),
                       complete.GetError().Error == AddressSpaceError::ShootdownFailed);
    }
}

void TestZeroCpuFailureAndAliasPolicies(TestContext& runner) noexcept
{
    AddressSpaceFixture fixture{runner};
    if (!fixture.IsReady())
    {
        return;
    }

    {
        PreparedFault first{fixture, runner, 0x0100'0000U, 4U, ReadWriteUser(), FaultAccess::Write};
        if (!first.Prepared)
        {
            return;
        }
        const auto begin = fixture.Space.BeginFaultMapping(first.Preparation);
        const auto complete =
            begin.HasValue()
                ? fixture.Space.CompleteFaultMapping(begin.GetValue().GetToken())
                : decltype(fixture.Space.CompleteFaultMapping(Kernel::Memory::AddressSpaceMappingToken{}))::Failure({});
        ASTERIA_EXPECT(runner, "address space with no resident CPUs publishes without IPIs", begin.HasValue(),
                       fixture.Space.GetSnapshot().TargetProcessorMask == 0U, complete.HasValue(),
                       complete.GetValue().MappingCount == 4U);
        if (complete.HasValue())
        {
            first.Commit();
        }
    }

    {
        PreparedFault overlap{fixture, runner, 0x0100'1000U, 8U, ReadWriteUser(), FaultAccess::Write};
        if (overlap.Prepared)
        {
            const auto result = fixture.Space.BeginFaultMapping(overlap.Preparation);
            ASTERIA_EXPECT(runner, "virtual overlap is rejected before the HAL or ledger changes", !result.HasValue(),
                           result.GetError().Error == AddressSpaceError::VirtualOverlap,
                           fixture.PageTable.MapCalls == 1U, fixture.Space.GetSnapshot().MappingCount == 4U);
        }
    }

    {
        PreparedFault executableAlias{fixture, runner, 0x0200'0000U, 4U, ReadExecuteUser(), FaultAccess::Execute};
        if (executableAlias.Prepared)
        {
            const auto result = fixture.Space.BeginFaultMapping(executableAlias.Preparation);
            ASTERIA_EXPECT(runner, "one Frame generation cannot acquire writable and executable aliases",
                           !result.HasValue(), result.GetError().Error == AddressSpaceError::PhysicalWriteExecuteAlias,
                           fixture.PageTable.MapCalls == 1U);
        }
    }

    {
        PreparedFault backendFailure{fixture, runner, 0x0300'0000U, 10U, ReadWriteUser(), FaultAccess::Write};
        fixture.PageTable.NextError = AddressSpacePageTableError::CapacityExceeded;
        if (backendFailure.Prepared)
        {
            const auto before = fixture.Space.GetSnapshot();
            const auto result = fixture.Space.BeginFaultMapping(backendFailure.Preparation);
            const auto after = fixture.Space.GetSnapshot();
            ASTERIA_EXPECT(runner, "HAL failure is propagated without publishing or faulting the owner",
                           !result.HasValue(), result.GetError().Error == AddressSpaceError::PageTableRejected,
                           result.GetError().PageTableError == AddressSpacePageTableError::CapacityExceeded,
                           before.MappingCount == after.MappingCount, before.State == after.State,
                           after.State == AddressSpaceState::Ready);
        }
        fixture.PageTable.NextError = AddressSpacePageTableError::None;
    }

    {
        PreparedFault terminal{fixture, runner, 0x0400'0000U, 12U, ReadWriteUser(), FaultAccess::Write};
        if (terminal.Prepared)
        {
            const auto begin = fixture.Space.BeginFaultMapping(terminal.Preparation);
            ASTERIA_EXPECT(runner, "shootdown transport failure is terminal and retains native state", begin.HasValue(),
                           fixture.Space.FailShootdown(begin.GetValue().GetToken()) == AddressSpaceError::None,
                           fixture.Space.GetSnapshot().State == AddressSpaceState::Faulted,
                           fixture.Space.BeginFaultMapping(terminal.Preparation).GetError().Error ==
                               AddressSpaceError::Faulted);
        }
    }
}

void TestMalformedBackendSuccessIsTerminal(TestContext& runner) noexcept
{
    AddressSpaceFixture fixture{runner};
    if (!fixture.IsReady())
    {
        return;
    }
    fixture.PageTable.Mode = PageTableResultMode::Reserved;
    PreparedFault mapping{fixture, runner, 0x0500'0000U, 14U, ReadWriteUser(), FaultAccess::Write};
    if (!mapping.Prepared)
    {
        return;
    }
    const auto result = fixture.Space.BeginFaultMapping(mapping.Preparation);
    ASTERIA_EXPECT(runner, "malformed success after native commit enters terminal ownership state", !result.HasValue(),
                   result.GetError().Error == AddressSpaceError::PageTableProtocolViolation,
                   fixture.PageTable.InstalledMappings == 1U,
                   fixture.Space.GetSnapshot().State == AddressSpaceState::Faulted,
                   fixture.Space.GetSnapshot().MappingCount == 3U);
}

void TestAmbiguousBackendCommitIsTerminal(TestContext& runner) noexcept
{
    AddressSpaceFixture fixture{runner};
    if (!fixture.IsReady())
    {
        return;
    }
    fixture.PageTable.NextError = AddressSpacePageTableError::CommittedStateUnknown;
    PreparedFault mapping{fixture, runner, 0x0580'0000U, 15U, ReadWriteUser(), FaultAccess::Write};
    if (!mapping.Prepared)
    {
        return;
    }
    const auto result = fixture.Space.BeginFaultMapping(mapping.Preparation);
    const auto snapshot = fixture.Space.GetSnapshot();
    ASTERIA_EXPECT(runner, "an ambiguous native commit retains ownership and faults the address space",
                   !result.HasValue(), result.GetError().Error == AddressSpaceError::PageTableProtocolViolation,
                   result.GetError().PageTableError == AddressSpacePageTableError::CommittedStateUnknown,
                   fixture.PageTable.InstalledMappings == 1U, snapshot.State == AddressSpaceState::Faulted,
                   snapshot.MappingCount == 3U);
}

void TestDeviceRegionMappingLifetime(TestContext& runner) noexcept
{
    AddressSpaceFixture fixture{runner, true};
    ASTERIA_EXPECT(runner, "device mapping fixture owns one AddressSpace, Frame, and DeviceRegion", fixture.IsReady());
    if (!fixture.IsReady())
    {
        return;
    }

    PreparedFault fault{fixture, runner, 0x0A00'0000U, 0U, ReadWriteUser(), FaultAccess::Write};
    if (!fault.Prepared)
    {
        return;
    }

    const auto begin = fixture.Space.BeginFaultMapping(fault.Preparation);
    const auto complete =
        begin.HasValue()
            ? fixture.Space.CompleteFaultMapping(begin.GetValue().GetToken())
            : decltype(fixture.Space.CompleteFaultMapping(Kernel::Memory::AddressSpaceMappingToken{}))::Failure({});
    ASTERIA_EXPECT(runner, "device-backed mapping retains exact MMIO authority through publication",
                   complete.HasValue(),
                   fixture.PageTable.LastMapping.DeviceRegion.IsEqualTo(fixture.GetDeviceRegionId()),
                   fixture.DeviceRegionStorage.GetSnapshot().ActiveMappings == 1U);
    if (!complete.HasValue())
    {
        return;
    }
    fault.Commit();

    ASTERIA_EXPECT(runner, "published device mapping prevents premature MMIO authority teardown",
                   fixture.DeviceRegionStorage.Quiesce().Error == DeviceRegionError::InUse);
    ASTERIA_EXPECT(runner, "AddressSpace quiescence releases the retained DeviceRegion mapping",
                   fixture.Space.Quiesce() == AddressSpaceError::None,
                   fixture.DeviceRegionStorage.GetSnapshot().ActiveMappings == 0U);
}

void TestCrossAddressSpaceWriteExecutePolicy(TestContext& runner) noexcept
{
    RegistryFixture registry;
    MemoryPolicyFixture memoryPolicy{registry, runner};
    TestPageTable firstPageTable;
    TestPageTable secondPageTable;
    InitialMappingPlan firstSeed;
    InitialMappingPlan secondSeed;
    AddressSpace firstSpace;
    AddressSpace secondSpace;
    TestKernelObject thread{ObjectType::Thread, ResourceDomainId{30U}};
    BootFrame sharedFrame;

    ASTERIA_EXPECT(runner, "two-root fixture initializes both spaces and one shared Frame", BuildSeedPlan(firstSeed),
                   BuildSeedPlan(secondSeed),
                   firstSpace.Initialize(ResourceDomainId{31U}, firstPageTable, firstSeed) == AddressSpaceError::None,
                   secondSpace.Initialize(ResourceDomainId{32U}, secondPageTable, secondSeed) ==
                       AddressSpaceError::None,
                   sharedFrame.Initialize(ResourceDomainId{33U}, MakeFrameDescriptor()) == FrameError::None);
    const auto firstObjectResult = registry.Issuer.CreateObject(firstSpace);
    const auto secondObjectResult = registry.Issuer.CreateObject(secondSpace);
    const auto threadObjectResult = registry.Issuer.CreateObject(thread);
    const auto frameObjectResult = registry.Issuer.CreateObject(sharedFrame);
    ASTERIA_EXPECT(runner, "two-root fixture registers every exact object generation", firstObjectResult.HasValue(),
                   secondObjectResult.HasValue(), threadObjectResult.HasValue(), frameObjectResult.HasValue(),
                   memoryPolicy.IsReady());
    if (!firstObjectResult.HasValue() || !secondObjectResult.HasValue() || !threadObjectResult.HasValue() ||
        !frameObjectResult.HasValue() || !memoryPolicy.IsReady())
    {
        return;
    }
    const ObjectId firstObject = firstObjectResult.GetValue();
    const ObjectId secondObject = secondObjectResult.GetValue();
    const ObjectId threadObject = threadObjectResult.GetValue();
    const ObjectId frameObject = frameObjectResult.GetValue();
    const AddressSpaceId firstId{registry.Registry, firstObject};
    const AddressSpaceId secondId{registry.Registry, secondObject};
    const FrameId frameId{registry.Registry, frameObject};
    bool frameDestructionStarted{false};
    ASTERIA_EXPECT(runner, "two independently rooted AddressSpaces bind in one registry",
                   firstSpace.BindIdentity(registry.Registry, firstId).Error == AddressSpaceError::None,
                   secondSpace.BindIdentity(registry.Registry, secondId).Error == AddressSpaceError::None);

    {
        FaultToken writableToken;
        FaultToken executableToken;
        FaultFrameMappingPreparation writablePreparation;
        FaultFrameMappingPreparation executablePreparation;
        FaultResolutionClaimToken writableClaim{};
        FaultResolutionClaimToken executableClaim{};

        const auto prepare = [&](FaultToken& token, FaultFrameMappingPreparation& preparation,
                                 FaultResolutionClaimToken& claim, const AddressSpaceId addressSpace,
                                 const Foundation::UInt64 virtualStart, const Foundation::UInt64 frameOffset,
                                 const PagePermissions permissions, const FaultAccess access) noexcept
        {
            if (token.Initialize(registry.Registry, ResourceDomainId{34U}, ThreadId{registry.Registry, threadObject},
                                 addressSpace, MakeFault(virtualStart, access)) != FaultTokenError::None)
            {
                return false;
            }
            const auto claimResult = token.ClaimResolution();
            if (!claimResult.HasValue())
            {
                return false;
            }
            claim = claimResult.GetValue().Token;
            return preparation.Prepare(registry.Registry, token, claim, memoryPolicy.Identity, memoryPolicy.Object, {},
                                       nullptr, frameId, sharedFrame,
                                       MakeRequest(virtualStart, frameOffset, permissions)) ==
                   FaultFrameMappingError::None;
        };

        {
            FaultToken failedWritableToken;
            FaultToken recoveredExecutableToken;
            FaultFrameMappingPreparation failedWritablePreparation;
            FaultFrameMappingPreparation recoveredExecutablePreparation;
            FaultResolutionClaimToken failedWritableClaim{};
            FaultResolutionClaimToken recoveredExecutableClaim{};

            const bool prepared =
                prepare(failedWritableToken, failedWritablePreparation, failedWritableClaim, firstId, 0x0800'0000U, 10U,
                        ReadWriteUser(), FaultAccess::Write) &&
                prepare(recoveredExecutableToken, recoveredExecutablePreparation, recoveredExecutableClaim, secondId,
                        0x0900'0000U, 10U, ReadExecuteUser(), FaultAccess::Execute);
            ASTERIA_EXPECT(runner, "ordinary HAL rollback fixture prepares opposite aliases", prepared);
            if (prepared)
            {
                firstPageTable.NextError = AddressSpacePageTableError::CapacityExceeded;
                const auto failed = firstSpace.BeginFaultMapping(failedWritablePreparation);
                firstPageTable.NextError = AddressSpacePageTableError::None;
                const FaultTokenError cancelError = failedWritableToken.CancelResolution(failedWritableClaim);
                const auto recovered = secondSpace.BeginFaultMapping(recoveredExecutablePreparation);
                const auto recoveredComplete = recovered.HasValue()
                                                   ? secondSpace.CompleteFaultMapping(recovered.GetValue().GetToken())
                                                   : decltype(secondSpace.CompleteFaultMapping(
                                                         Kernel::Memory::AddressSpaceMappingToken{}))::Failure({});
                ASTERIA_EXPECT(runner, "ordinary HAL rejection releases the cross-root mapping policy",
                               !failed.HasValue(), failed.GetError().Error == AddressSpaceError::PageTableRejected,
                               failed.GetError().PageTableError == AddressSpacePageTableError::CapacityExceeded,
                               cancelError == FaultTokenError::None, recoveredComplete.HasValue(),
                               recoveredExecutableToken.CommitResolution(
                                   recoveredExecutableClaim, FaultResolutionKind::Map) == FaultTokenError::None);
            }
        }

        ASTERIA_EXPECT(runner, "two roots authorize distinct aliases of one Frame generation",
                       prepare(writableToken, writablePreparation, writableClaim, firstId, 0x0600'0000U, 6U,
                               ReadWriteUser(), FaultAccess::Write),
                       prepare(executableToken, executablePreparation, executableClaim, secondId, 0x0700'0000U, 6U,
                               ReadExecuteUser(), FaultAccess::Execute));

        const auto writableBegin = firstSpace.BeginFaultMapping(writablePreparation);
        const auto writableComplete =
            writableBegin.HasValue()
                ? firstSpace.CompleteFaultMapping(writableBegin.GetValue().GetToken())
                : decltype(firstSpace.CompleteFaultMapping(Kernel::Memory::AddressSpaceMappingToken{}))::Failure({});
        ASTERIA_EXPECT(runner, "first root publishes the writable alias", writableComplete.HasValue(),
                       writableToken.CommitResolution(writableClaim, FaultResolutionKind::Map) ==
                           FaultTokenError::None);

        const Foundation::Size secondMapCallsBeforeBlocked = secondPageTable.MapCalls;
        const auto blocked = secondSpace.BeginFaultMapping(executablePreparation);
        ASTERIA_EXPECT(runner, "Frame-wide policy rejects executable alias in a different root", !blocked.HasValue(),
                       blocked.GetError().Error == AddressSpaceError::PhysicalWriteExecuteAlias,
                       secondPageTable.MapCalls == secondMapCallsBeforeBlocked);

        FaultToken ambiguousWritableToken;
        FaultToken ambiguousExecutableToken;
        FaultFrameMappingPreparation ambiguousWritablePreparation;
        FaultFrameMappingPreparation ambiguousExecutablePreparation;
        FaultResolutionClaimToken ambiguousWritableClaim{};
        FaultResolutionClaimToken ambiguousExecutableClaim{};
        const bool ambiguousPrepared =
            prepare(ambiguousWritableToken, ambiguousWritablePreparation, ambiguousWritableClaim, firstId, 0x0A00'0000U,
                    14U, ReadWriteUser(), FaultAccess::Write) &&
            prepare(ambiguousExecutableToken, ambiguousExecutablePreparation, ambiguousExecutableClaim, secondId,
                    0x0B00'0000U, 14U, ReadExecuteUser(), FaultAccess::Execute);
        ASTERIA_EXPECT(runner, "ambiguous-commit fixture prepares opposite aliases", ambiguousPrepared);
        if (ambiguousPrepared)
        {
            firstPageTable.NextError = AddressSpacePageTableError::CommittedStateUnknown;
            const auto ambiguous = firstSpace.BeginFaultMapping(ambiguousWritablePreparation);
            firstPageTable.NextError = AddressSpacePageTableError::None;
            const auto ambiguousBlocked = secondSpace.BeginFaultMapping(ambiguousExecutablePreparation);
            ASTERIA_EXPECT(runner, "ambiguous native commit retains its cross-root mapping policy",
                           !ambiguous.HasValue(),
                           ambiguous.GetError().Error == AddressSpaceError::PageTableProtocolViolation,
                           firstSpace.GetSnapshot().State == AddressSpaceState::Faulted, !ambiguousBlocked.HasValue(),
                           ambiguousBlocked.GetError().Error == AddressSpaceError::PhysicalWriteExecuteAlias);
        }

        ASTERIA_EXPECT(runner, "quiescing the first root releases its Frame-wide mapping policy",
                       firstSpace.Quiesce() == AddressSpaceError::None);

        if (ambiguousPrepared)
        {
            const auto ambiguousExecutableBegin = secondSpace.BeginFaultMapping(ambiguousExecutablePreparation);
            const auto ambiguousExecutableComplete =
                ambiguousExecutableBegin.HasValue()
                    ? secondSpace.CompleteFaultMapping(ambiguousExecutableBegin.GetValue().GetToken())
                    : decltype(secondSpace.CompleteFaultMapping(Kernel::Memory::AddressSpaceMappingToken{}))::Failure(
                          {});
            ASTERIA_EXPECT(runner, "quiescence releases an ambiguous commit's retained policy",
                           ambiguousExecutableComplete.HasValue(),
                           ambiguousExecutableToken.CommitResolution(
                               ambiguousExecutableClaim, FaultResolutionKind::Map) == FaultTokenError::None);
        }
        const auto executableBegin = secondSpace.BeginFaultMapping(executablePreparation);
        const auto executableComplete =
            executableBegin.HasValue()
                ? secondSpace.CompleteFaultMapping(executableBegin.GetValue().GetToken())
                : decltype(secondSpace.CompleteFaultMapping(Kernel::Memory::AddressSpaceMappingToken{}))::Failure({});
        ASTERIA_EXPECT(
            runner, "released writable root permits a later executable mapping", executableComplete.HasValue(),
            executableToken.CommitResolution(executableClaim, FaultResolutionKind::Map) == FaultTokenError::None);

        const ObjectRegistryError destroyError = registry.Issuer.DestroyObject(frameObject);
        const auto collectedWhilePinned = registry.Issuer.CollectDestroyedObject();
        frameDestructionStarted = destroyError == ObjectRegistryError::Ok;
        ASTERIA_EXPECT(runner, "published mappings retain their shared Frame until quiescence", frameDestructionStarted,
                       !collectedWhilePinned.HasValue(),
                       collectedWhilePinned.GetError() == ObjectRegistryError::NoDestructionReady);
    }

    ASTERIA_EXPECT(runner, "second root releases its executable policy before collection",
                   secondSpace.Quiesce() == AddressSpaceError::None);
    const auto destroyAndCollect = [&](const ObjectId object, IKernelObject* const expected) noexcept
    {
        const ObjectRegistryError destroyError = registry.Issuer.DestroyObject(object);
        const auto collected = registry.Issuer.CollectDestroyedObject();
        return (destroyError == ObjectRegistryError::Ok) && collected.HasValue() &&
               (collected.GetValue().Object.GetValue() == object.GetValue()) &&
               (collected.GetValue().Instance == expected);
    };
    bool frameCollected{false};
    if (frameDestructionStarted)
    {
        const auto collected = registry.Issuer.CollectDestroyedObject();
        frameCollected = collected.HasValue() && (collected.GetValue().Object.GetValue() == frameObject.GetValue()) &&
                         (collected.GetValue().Instance == &sharedFrame);
    }
    else
    {
        frameCollected = destroyAndCollect(frameObject, &sharedFrame);
    }
    const bool threadCollected = destroyAndCollect(threadObject, &thread);
    const bool firstCollected = destroyAndCollect(firstObject, &firstSpace);
    const bool secondCollected = destroyAndCollect(secondObject, &secondSpace);
    ASTERIA_EXPECT(runner, "both roots and their shared objects collect after all policies release", frameCollected,
                   threadCollected, firstCollected, secondCollected);
}

// Disjoint writable and executable sub-ranges of one Frame across two roots: a BootFrame's records
// Admit both; a CreatedFrame's two counters refuse the executable alias while any writable
// reservation is held, and admit it once the writable root's quiescence releases its reservations.
template <typename FrameType>
void TestDisjointAliasesAcrossRoots(TestContext& runner, const bool disjointAdmitted) noexcept
{
    RegistryFixture registry;
    MemoryPolicyFixture memoryPolicy{registry, runner};
    TestPageTable firstPageTable;
    TestPageTable secondPageTable;
    InitialMappingPlan firstSeed;
    InitialMappingPlan secondSeed;
    AddressSpace firstSpace;
    AddressSpace secondSpace;
    TestKernelObject thread{ObjectType::Thread, ResourceDomainId{40U}};
    FrameType sharedFrame;

    const bool initialized =
        BuildSeedPlan(firstSeed) && BuildSeedPlan(secondSeed) &&
        (firstSpace.Initialize(ResourceDomainId{41U}, firstPageTable, firstSeed) == AddressSpaceError::None) &&
        (secondSpace.Initialize(ResourceDomainId{42U}, secondPageTable, secondSeed) == AddressSpaceError::None) &&
        (sharedFrame.Initialize(ResourceDomainId{43U}, MakeFrameDescriptor()) == FrameError::None);
    const auto firstObjectResult = registry.Issuer.CreateObject(firstSpace);
    const auto secondObjectResult = registry.Issuer.CreateObject(secondSpace);
    const auto threadObjectResult = registry.Issuer.CreateObject(thread);
    const auto frameObjectResult = registry.Issuer.CreateObject(sharedFrame);
    const bool ready = initialized && firstObjectResult.HasValue() && secondObjectResult.HasValue() &&
                       threadObjectResult.HasValue() && frameObjectResult.HasValue() && memoryPolicy.IsReady();
    ASTERIA_EXPECT(runner, "disjoint-alias fixture builds two roots and one Frame", ready);
    if (!ready)
    {
        return;
    }
    const ObjectId firstObject = firstObjectResult.GetValue();
    const ObjectId secondObject = secondObjectResult.GetValue();
    const ObjectId threadObject = threadObjectResult.GetValue();
    const ObjectId frameObject = frameObjectResult.GetValue();
    const AddressSpaceId firstId{registry.Registry, firstObject};
    const AddressSpaceId secondId{registry.Registry, secondObject};
    const FrameId frameId{registry.Registry, frameObject};
    ASTERIA_EXPECT(runner, "disjoint-alias fixture binds both roots",
                   firstSpace.BindIdentity(registry.Registry, firstId).Error == AddressSpaceError::None,
                   secondSpace.BindIdentity(registry.Registry, secondId).Error == AddressSpaceError::None);

    const auto prepare = [&](FaultToken& token, FaultFrameMappingPreparation& preparation,
                             FaultResolutionClaimToken& claim, const AddressSpaceId addressSpace,
                             const Foundation::UInt64 virtualStart, const Foundation::UInt64 frameOffset,
                             const PagePermissions permissions, const FaultAccess access) noexcept
    {
        if (token.Initialize(registry.Registry, ResourceDomainId{44U}, ThreadId{registry.Registry, threadObject},
                             addressSpace, MakeFault(virtualStart, access)) != FaultTokenError::None)
        {
            return false;
        }
        const auto claimResult = token.ClaimResolution();
        if (!claimResult.HasValue())
        {
            return false;
        }
        claim = claimResult.GetValue().Token;
        return preparation.Prepare(registry.Registry, token, claim, memoryPolicy.Identity, memoryPolicy.Object, {},
                                   nullptr, frameId, sharedFrame,
                                   MakeRequest(virtualStart, frameOffset, permissions)) == FaultFrameMappingError::None;
    };
    const auto publish = [](AddressSpace& space, FaultFrameMappingPreparation& preparation, FaultToken& token,
                            const FaultResolutionClaimToken claim) noexcept
    {
        const auto begun = space.BeginFaultMapping(preparation);
        if (!begun.HasValue())
        {
            return begun.GetError().Error;
        }
        const auto completed = space.CompleteFaultMapping(begun.GetValue().GetToken());
        return (completed.HasValue() &&
                (token.CommitResolution(claim, FaultResolutionKind::Map) == FaultTokenError::None))
                   ? AddressSpaceError::None
                   : AddressSpaceError::PageTableProtocolViolation;
    };

    {
        FaultToken writableToken;
        FaultToken executableToken;
        FaultFrameMappingPreparation writablePreparation;
        FaultFrameMappingPreparation executablePreparation;
        FaultResolutionClaimToken writableClaim{};
        FaultResolutionClaimToken executableClaim{};
        // Pages 2-3 writable in the first root, pages 20-21 executable in
        // the second: no page in common.
        const bool prepared = prepare(writableToken, writablePreparation, writableClaim, firstId, 0x0600'0000U, 2U,
                                      ReadWriteUser(), FaultAccess::Write) &&
                              prepare(executableToken, executablePreparation, executableClaim, secondId, 0x0700'0000U,
                                      20U, ReadExecuteUser(), FaultAccess::Execute);
        ASTERIA_EXPECT(runner, "disjoint-alias fixture prepares a writable and an executable sub-range", prepared);
        if (prepared)
        {
            const AddressSpaceError writable = publish(firstSpace, writablePreparation, writableToken, writableClaim);
            const Foundation::Size mapCallsBefore = secondPageTable.MapCalls;
            const AddressSpaceError executable =
                publish(secondSpace, executablePreparation, executableToken, executableClaim);
            if (disjointAdmitted)
            {
                ASTERIA_EXPECT(runner, "a BootFrame's records admit disjoint writable and executable sub-ranges",
                               writable == AddressSpaceError::None, executable == AddressSpaceError::None);
            }
            else
            {
                ASTERIA_EXPECT(
                    runner,
                    "a CreatedFrame's counters refuse an executable alias of any sub-range while one is writable",
                    writable == AddressSpaceError::None, executable == AddressSpaceError::PhysicalWriteExecuteAlias,
                    secondPageTable.MapCalls == mapCallsBefore);
                ASTERIA_EXPECT(runner, "quiescing the writable root releases the counter for the executable alias",
                               firstSpace.Quiesce() == AddressSpaceError::None,
                               publish(secondSpace, executablePreparation, executableToken, executableClaim) ==
                                   AddressSpaceError::None);
            }
        }
    }

    if (firstSpace.GetSnapshot().State != AddressSpaceState::Quiesced)
    {
        ASTERIA_EXPECT(runner, "the first root quiesces", firstSpace.Quiesce() == AddressSpaceError::None);
    }
    ASTERIA_EXPECT(runner, "the second root quiesces", secondSpace.Quiesce() == AddressSpaceError::None);
    const auto destroyAndCollect = [&](const ObjectId object, IKernelObject* const expected) noexcept
    {
        const ObjectRegistryError destroyError = registry.Issuer.DestroyObject(object);
        const auto collected = registry.Issuer.CollectDestroyedObject();
        return (destroyError == ObjectRegistryError::Ok) && collected.HasValue() &&
               (collected.GetValue().Object.GetValue() == object.GetValue()) &&
               (collected.GetValue().Instance == expected);
    };
    ASTERIA_EXPECT(runner, "disjoint-alias fixture collects every object after both roots release",
                   destroyAndCollect(frameObject, &sharedFrame), destroyAndCollect(threadObject, &thread),
                   destroyAndCollect(firstObject, &firstSpace), destroyAndCollect(secondObject, &secondSpace));
}

void TestReadOnlyBootstrapFrame(TestContext& runner) noexcept
{
    AddressSpaceFixture fixture{runner, false, MemoryCommitment::Pinned, true};
    if (!fixture.IsReady())
    {
        return;
    }
    ASTERIA_EXPECT(runner, "bootstrap Frame exposes its immutable policy",
                   fixture.FrameStorage.GetSnapshot().ReadOnly == 1U);
    const PagePermissions permissions[3U]{
        PagePermissions{PagePermission::Read}.With(PagePermission::User), ReadWriteUser(),
        PagePermissions{PagePermission::Read}.With(PagePermission::Execute).With(PagePermission::User)};
    for (Foundation::Size index{0U}; index < 3U; ++index)
    {
        PinnedFrameMappingPreparation prepared;
        const PinnedFrameMappingRequest request{
            VirtualAddress{0x0900'0000U + index * 4096U}, PageCount{1U}, index, index, permissions[index], {}};
        const auto result =
            prepared.Prepare(fixture.Registry.Registry, fixture.GetMemoryObjectId(), fixture.MemoryPolicy.Object,
                             fixture.GetFrameId(), fixture.FrameStorage, request);
        ASTERIA_EXPECT(runner, "a bootstrap snapshot preparation uses the public interface",
                       result == PinnedFrameMappingError::None);
        if (result != PinnedFrameMappingError::None)
        {
            return;
        }
        const auto begun = fixture.Space.BeginPinnedFrameMapping(
            AddressSpaceId{fixture.Registry.Registry, fixture.AddressObject}, prepared);
        if (index == 0U)
        {
            ASTERIA_EXPECT(runner, "read-only bootstrap data may alias a kernel seed range", begun.HasValue());
            if (begun.HasValue())
            {
                ASTERIA_EXPECT(runner, "bootstrap read mapping commits",
                               fixture.Space.CompleteMapping(begun.GetValue().GetToken()).HasValue());
            }
        }
        else
        {
            ASTERIA_EXPECT(runner, "writable and executable bootstrap aliases are rejected", !begun.HasValue());
        }
    }
}

void TestPinnedFrameMappingTransaction(TestContext& runner) noexcept
{
    const PagePermissions supervisorReadWrite = PagePermissions{PagePermission::Read}.With(PagePermission::Write);
    const PagePermissions userReadWrite =
        PagePermissions{PagePermission::Read}.With(PagePermission::Write).With(PagePermission::User);
    const PinnedFrameMappingRequest request{
        .VirtualStart = VirtualAddress{0x0900'0000U},
        .Pages = PageCount{2U},
        .MemoryObjectOffsetPages = 4U,
        .FrameOffsetPages = 4U,
        .Permissions = userReadWrite,
        .Reserved = {},
    };

    {
        AddressSpaceFixture fixture{runner, false, MemoryCommitment::Pinned};
        if (!fixture.IsReady())
        {
            return;
        }

        PinnedFrameMappingRequest supervisorRequest = request;
        supervisorRequest.Permissions = supervisorReadWrite;
        PinnedFrameMappingPreparation preparation;
        const PinnedFrameMappingError prepareError =
            preparation.Prepare(fixture.Registry.Registry, fixture.GetMemoryObjectId(), fixture.MemoryPolicy.Object,
                                fixture.GetFrameId(), fixture.FrameStorage, supervisorRequest);
        const auto begin = fixture.Space.BeginPinnedFrameMapping(fixture.GetAddressSpaceId(), preparation);
        ASTERIA_EXPECT(runner, "zero-resident supervisor pinned mapping begins",
                       prepareError == PinnedFrameMappingError::None, begin.HasValue());
        if (!begin.HasValue())
        {
            return;
        }

        const auto pending = fixture.Space.GetSnapshot();
        const auto complete = fixture.Space.CompleteMapping(begin.GetValue().GetToken());
        const auto published = fixture.Space.TryGetMapping(3U);
        ASTERIA_EXPECT(runner, "zero-resident pinned mapping completes without acknowledgement",
                       pending.State == AddressSpaceState::ShootdownPending, pending.TargetProcessorMask == 0U,
                       pending.AcknowledgedProcessorMask == 0U, complete.HasValue(),
                       complete.GetValue().MappingCount == 4U, published.HasValue(),
                       published.GetValue().Permissions.GetBits() == supervisorReadWrite.GetBits());
    }

    {
        AddressSpaceFixture fixture{runner, false, MemoryCommitment::Pinned};
        if (!fixture.IsReady())
        {
            return;
        }

        PinnedFrameMappingPreparation unprepared;
        const auto unpreparedBegin = fixture.Space.BeginPinnedFrameMapping(fixture.GetAddressSpaceId(), unprepared);
        PinnedFrameMappingPreparation preparation;
        const PinnedFrameMappingError prepareError =
            preparation.Prepare(fixture.Registry.Registry, fixture.GetMemoryObjectId(), fixture.MemoryPolicy.Object,
                                fixture.GetFrameId(), fixture.FrameStorage, request);
        const auto wrongTarget = fixture.Space.BeginPinnedFrameMapping(AddressSpaceId{}, preparation);
        const auto activation = fixture.Space.ActivateProcessor(CpuId{0U});
        const Foundation::Size mapCallsBefore = fixture.PageTable.MapCalls;
        const auto begin = fixture.Space.BeginPinnedFrameMapping(fixture.GetAddressSpaceId(), preparation);
        const auto unpublished = fixture.Space.TryGetMapping(3U);
        ASTERIA_EXPECT(runner, "pinned mapping requires prepared ownership and the exact root",
                       !unpreparedBegin.HasValue(),
                       unpreparedBegin.GetError().Error == AddressSpaceError::InvalidMappingPlan,
                       prepareError == PinnedFrameMappingError::None, !wrongTarget.HasValue(),
                       wrongTarget.GetError().Error == AddressSpaceError::AddressSpaceMismatch, activation.HasValue(),
                       begin.HasValue(), !unpublished.HasValue(), fixture.PageTable.MapCalls == (mapCallsBefore + 1U),
                       fixture.PageTable.LastMapping.Permissions.GetBits() == userReadWrite.GetBits(),
                       fixture.PageTable.LastMapping.Permissions.Contains(PagePermission::User),
                       fixture.Space.GetSnapshot().State == AddressSpaceState::ShootdownPending,
                       fixture.Space.GetSnapshot().TargetProcessorMask == 0x01U);
        if (!begin.HasValue())
        {
            return;
        }

        const auto token = begin.GetValue().GetToken();
        const AddressSpaceError acknowledged = fixture.Space.AcknowledgeShootdown(token, CpuId{0U});
        const auto complete = fixture.Space.CompleteMapping(token);
        const auto published = fixture.Space.TryGetMapping(3U);
        ASTERIA_EXPECT(runner, "generic completion publishes one retained user pinned map",
                       acknowledged == AddressSpaceError::None, complete.HasValue(), published.HasValue(),
                       published.GetValue().MemoryObject.IsEqualTo(fixture.GetMemoryObjectId()),
                       published.GetValue().Frame.IsEqualTo(fixture.GetFrameId()),
                       published.GetValue().VirtualStart.GetValue() == request.VirtualStart.GetValue(),
                       published.GetValue().PhysicalStart.GetValue() == 0x0200'4000U,
                       published.GetValue().Permissions.GetBits() == userReadWrite.GetBits(),
                       published.GetValue().Commitment == MemoryCommitment::Pinned, published.GetValue().RuntimeMapping,
                       fixture.MemoryPolicy.Object.GetSnapshot().ActiveMappings == 1U);

        PinnedFrameMappingRequest retryRequest = request;
        retryRequest.VirtualStart = VirtualAddress{0x0900'4000U};
        retryRequest.MemoryObjectOffsetPages = 8U;
        retryRequest.FrameOffsetPages = 8U;
        PinnedFrameMappingPreparation retryPreparation;
        const PinnedFrameMappingError retryPrepareError = retryPreparation.Prepare(
            fixture.Registry.Registry, fixture.GetMemoryObjectId(), fixture.MemoryPolicy.Object, fixture.GetFrameId(),
            fixture.FrameStorage, retryRequest);
        fixture.PageTable.NextError = AddressSpacePageTableError::MappingConflict;
        const auto rejected = fixture.Space.BeginPinnedFrameMapping(fixture.GetAddressSpaceId(), retryPreparation);
        ASTERIA_EXPECT(runner, "ordinary backend rejection rolls back provisional pinned retention",
                       retryPrepareError == PinnedFrameMappingError::None, !rejected.HasValue(),
                       rejected.GetError().Error == AddressSpaceError::PageTableRejected,
                       fixture.Space.GetSnapshot().State == AddressSpaceState::Ready,
                       fixture.Space.GetSnapshot().MappingCount == 4U,
                       fixture.MemoryPolicy.Object.GetSnapshot().ActiveMappings == 1U);
    }

    {
        AddressSpaceFixture fixture{runner, false, MemoryCommitment::Pinned};
        if (!fixture.IsReady())
        {
            return;
        }

        PinnedFrameMappingPreparation preparation;
        const PinnedFrameMappingError prepareError =
            preparation.Prepare(fixture.Registry.Registry, fixture.GetMemoryObjectId(), fixture.MemoryPolicy.Object,
                                fixture.GetFrameId(), fixture.FrameStorage, request);
        fixture.PageTable.NextError = AddressSpacePageTableError::CommittedStateUnknown;
        const auto ambiguous = fixture.Space.BeginPinnedFrameMapping(fixture.GetAddressSpaceId(), preparation);
        ASTERIA_EXPECT(runner, "ambiguous pinned commit fails closed and retains all resources",
                       prepareError == PinnedFrameMappingError::None, !ambiguous.HasValue(),
                       ambiguous.GetError().Error == AddressSpaceError::PageTableProtocolViolation,
                       ambiguous.GetError().PageTableError == AddressSpacePageTableError::CommittedStateUnknown,
                       fixture.Space.GetSnapshot().State == AddressSpaceState::Faulted,
                       fixture.Space.GetSnapshot().MappingCount == 3U,
                       fixture.MemoryPolicy.Object.GetSnapshot().ActiveMappings == 1U);
    }

    {
        AddressSpaceFixture fixture{runner, false, MemoryCommitment::Pinned};
        if (!fixture.IsReady())
        {
            return;
        }

        PinnedFrameMappingPreparation preparation;
        const PinnedFrameMappingError prepareError =
            preparation.Prepare(fixture.Registry.Registry, fixture.GetMemoryObjectId(), fixture.MemoryPolicy.Object,
                                fixture.GetFrameId(), fixture.FrameStorage, request);
        fixture.PageTable.NextError = static_cast<AddressSpacePageTableError>(0xFFU);
        fixture.PageTable.CountErrorAsInstalled = true;
        const auto unknown = fixture.Space.BeginPinnedFrameMapping(fixture.GetAddressSpaceId(), preparation);
        ASTERIA_EXPECT(runner, "unknown backend result normalizes to ambiguous commit and retains resources",
                       prepareError == PinnedFrameMappingError::None, !unknown.HasValue(),
                       unknown.GetError().Error == AddressSpaceError::PageTableProtocolViolation,
                       unknown.GetError().PageTableError == AddressSpacePageTableError::CommittedStateUnknown,
                       fixture.PageTable.InstalledMappings == 1U,
                       fixture.Space.GetSnapshot().State == AddressSpaceState::Faulted,
                       fixture.Space.GetSnapshot().MappingCount == 3U,
                       fixture.MemoryPolicy.Object.GetSnapshot().ActiveMappings == 1U);
    }
}

// MemoryUnmap's transaction: the exact range only, the objects held until every resident processor
// acknowledged, then the slot free for the next mapping.
void TestPinnedFrameUnmapTransaction(TestContext& runner) noexcept
{
    AddressSpaceFixture fixture{runner, false, MemoryCommitment::Pinned};
    if (!fixture.IsReady())
    {
        return;
    }
    const PagePermissions userReadWrite =
        PagePermissions{PagePermission::Read}.With(PagePermission::Write).With(PagePermission::User);
    const PagePermissions userReadExecute =
        PagePermissions{PagePermission::Read}.With(PagePermission::Execute).With(PagePermission::User);
    const auto map = [&](const Foundation::UInt64 virtualStart, const PagePermissions permissions) noexcept
    {
        PinnedFrameMappingPreparation preparation;
        const bool prepared =
            preparation.Prepare(fixture.Registry.Registry, fixture.GetMemoryObjectId(), fixture.MemoryPolicy.Object,
                                fixture.GetFrameId(), fixture.FrameStorage,
                                PinnedFrameMappingRequest{
                                    .VirtualStart = VirtualAddress{virtualStart},
                                    .Pages = PageCount{2U},
                                    .MemoryObjectOffsetPages = 4U,
                                    .FrameOffsetPages = 4U,
                                    .Permissions = permissions,
                                    .Reserved = {},
                                }) == PinnedFrameMappingError::None;
        const auto begun = fixture.Space.BeginPinnedFrameMapping(fixture.GetAddressSpaceId(), preparation);
        if (!prepared || !begun.HasValue())
        {
            return begun.HasValue() ? AddressSpaceError::InvalidMappingPlan : begun.GetError().Error;
        }
        static_cast<void>(fixture.Space.AcknowledgeShootdown(begun.GetValue().GetToken(), CpuId{0U}));
        const auto completed = fixture.Space.CompleteMapping(begun.GetValue().GetToken());
        return completed.HasValue() ? AddressSpaceError::None : completed.GetError().Error;
    };

    const bool resident = fixture.Space.ActivateProcessor(CpuId{0U}).HasValue();
    const AddressSpaceError mapped = map(0x0900'0000U, userReadWrite);
    const AddressSpaceError alias = map(0x0A00'0000U, userReadExecute);
    const auto absent = fixture.Space.BeginUnmap(VirtualAddress{0x0900'1000U}, PageCount{1U});
    const auto shorter = fixture.Space.BeginUnmap(VirtualAddress{0x0900'0000U}, PageCount{1U});
    const auto seed = fixture.Space.BeginUnmap(fixture.Space.TryGetMapping(0U).GetValue().VirtualStart,
                                               fixture.Space.TryGetMapping(0U).GetValue().Pages);
    ASTERIA_EXPECT(runner, "only a completed runtime mapping's exact range can be unmapped", resident,
                   mapped == AddressSpaceError::None, alias == AddressSpaceError::PhysicalWriteExecuteAlias,
                   !absent.HasValue() && (absent.GetError().Error == AddressSpaceError::MappingNotFound),
                   !shorter.HasValue() && (shorter.GetError().Error == AddressSpaceError::MappingNotFound),
                   !seed.HasValue() && (seed.GetError().Error == AddressSpaceError::MappingNotFound),
                   fixture.PageTable.UnmapCalls == 0U, fixture.Space.GetSnapshot().State == AddressSpaceState::Ready);

    fixture.PageTable.NextUnmapError = AddressSpacePageTableError::UnsupportedConfiguration;
    const auto unsupported = fixture.Space.BeginUnmap(VirtualAddress{0x0900'0000U}, PageCount{2U});
    fixture.PageTable.NextUnmapError = AddressSpacePageTableError::None;
    const auto begun = fixture.Space.BeginUnmap(VirtualAddress{0x0900'0000U}, PageCount{2U});
    const auto pending = fixture.Space.GetSnapshot();
    const auto hidden = fixture.Space.TryGetMapping(3U);
    const Foundation::UInt64 activeDuring = fixture.MemoryPolicy.Object.GetSnapshot().ActiveMappings;
    const AddressSpaceError blocked = map(0x0A00'0000U, userReadExecute);
    const auto early = begun.HasValue() ? fixture.Space.CompleteMapping(begun.GetValue().GetToken())
                                        : fixture.Space.CompleteMapping(Kernel::Memory::AddressSpaceMappingToken{});
    ASTERIA_EXPECT(runner, "an unmap holds its objects and the space until every resident processor acknowledged",
                   !unsupported.HasValue(), unsupported.GetError().Error == AddressSpaceError::PageTableRejected,
                   begun.HasValue(), fixture.PageTable.UnmapCalls == 2U,
                   fixture.PageTable.LastUnmapping.VirtualStart.GetValue() == 0x0900'0000U,
                   begun.HasValue() && (begun.GetValue().GetInvalidationStart().GetValue() == 0x0900'0000U),
                   begun.HasValue() && (begun.GetValue().GetInvalidationPages().GetValue() == 2U),
                   pending.State == AddressSpaceState::ShootdownPending, pending.TargetProcessorMask == 0x01U,
                   pending.MappingCount == 4U, !hidden.HasValue(), activeDuring == 1U,
                   blocked == AddressSpaceError::ShootdownPending, !early.HasValue(),
                   early.GetError().Error == AddressSpaceError::ShootdownIncomplete);
    if (!begun.HasValue())
    {
        return;
    }

    const AddressSpaceError acknowledged = fixture.Space.AcknowledgeShootdown(begun.GetValue().GetToken(), CpuId{0U});
    const auto completed = fixture.Space.CompleteMapping(begun.GetValue().GetToken());
    const auto gone = fixture.Space.TryGetMapping(3U);
    const Foundation::UInt64 activeAfter = fixture.MemoryPolicy.Object.GetSnapshot().ActiveMappings;
    const AddressSpaceError executable = map(0x0A00'0000U, userReadExecute);
    const auto reused = fixture.Space.TryGetMapping(3U);
    ASTERIA_EXPECT(runner, "a completed unmap releases its objects and its slot, so the pages map executable",
                   acknowledged == AddressSpaceError::None, completed.HasValue(),
                   completed.HasValue() && (completed.GetValue().MappingCount == 3U),
                   completed.HasValue() && (completed.GetValue().VirtualStart.GetValue() == 0x0900'0000U),
                   completed.HasValue() && (completed.GetValue().Pages.GetValue() == 2U), !gone.HasValue(),
                   activeAfter == 0U, executable == AddressSpaceError::None,
                   reused.HasValue() && (reused.GetValue().VirtualStart.GetValue() == 0x0A00'0000U),
                   fixture.Space.GetSnapshot().MappingCount == 4U,
                   fixture.Space.GetSnapshot().State == AddressSpaceState::Ready);
}

void TestIommuAddressSpaceWriteExecutePolicy(TestContext& runner) noexcept
{
    AddressSpaceFixture fixture{runner};
    TestIommuBackend backend;
    IommuDomain domain{fixture.Registry.Registry, backend};
    ASTERIA_EXPECT(runner, "IOMMU W^X fixture initializes its AddressSpace and shared Frame", fixture.IsReady());
    const auto initialized =
        domain.Initialize(ResourceDomainObjectId{fixture.Registry.Registry, fixture.MemoryPolicy.DomainObject},
                          fixture.MemoryPolicy.Domain,
                          IommuDomainConfiguration{
                              .InputAddressBits = 48U,
                              .Reserved = {},
                          });
    ASTERIA_EXPECT(runner, "IOMMU domain shares the AddressSpace registry and Frame",
                   initialized.Error == IommuDomainError::None);

    backend.NextMap = IommuBackendError::CapacityExceeded;
    const auto ordinaryFailure = domain.Map(MakeDmaRequest(fixture, 0x00F0'0000U, 0U, 2U));
    ASTERIA_EXPECT(runner, "ordinary DMA Map rejection leaves no native translation", !ordinaryFailure.HasValue(),
                   ordinaryFailure.GetError().Backend == IommuBackendError::CapacityExceeded,
                   backend.LiveMappings == 0U);
    {
        PreparedFault executableAfterRollback{
            fixture, runner, 0x0800'0000U, 0U, ReadExecuteUser(), FaultAccess::Execute};
        const auto begin =
            executableAfterRollback.Prepared
                ? fixture.Space.BeginFaultMapping(executableAfterRollback.Preparation)
                : decltype(fixture.Space.BeginFaultMapping(executableAfterRollback.Preparation))::Failure({});
        const auto complete =
            begin.HasValue()
                ? fixture.Space.CompleteFaultMapping(begin.GetValue().GetToken())
                : decltype(fixture.Space.CompleteFaultMapping(Kernel::Memory::AddressSpaceMappingToken{}))::Failure({});
        ASTERIA_EXPECT(runner, "ordinary DMA Map rejection releases its Frame write policy", complete.HasValue());
        if (complete.HasValue())
        {
            executableAfterRollback.Commit();
        }
    }

    const auto dmaFirst = domain.Map(MakeDmaRequest(fixture, 0x0100'0000U, 4U, 2U));
    ASTERIA_EXPECT(runner, "DMA write mapping is admitted before a CPU executable alias", dmaFirst.HasValue());
    {
        const Foundation::Size pageTableCallsBeforeBlocked = fixture.PageTable.MapCalls;
        PreparedFault cpuAfterDma{fixture, runner, 0x1000'0000U, 5U, ReadExecuteUser(), FaultAccess::Execute};
        const auto blocked = cpuAfterDma.Prepared
                                 ? fixture.Space.BeginFaultMapping(cpuAfterDma.Preparation)
                                 : decltype(fixture.Space.BeginFaultMapping(cpuAfterDma.Preparation))::Failure({});
        ASTERIA_EXPECT(runner, "DMA write blocks a partially overlapping CPU executable map pre-HAL",
                       !blocked.HasValue(), blocked.GetError().Error == AddressSpaceError::PhysicalWriteExecuteAlias,
                       fixture.PageTable.MapCalls == pageTableCallsBeforeBlocked);
    }
    if (dmaFirst.HasValue())
    {
        ASTERIA_EXPECT(runner, "drained DMA Unmap releases the shared Frame write policy",
                       domain.Unmap(dmaFirst.GetValue().Mapping).Error == IommuDomainError::None);
    }

    {
        PreparedFault cpuFirst{fixture, runner, 0x2000'0000U, 4U, ReadExecuteUser(), FaultAccess::Execute};
        const auto begin = cpuFirst.Prepared
                               ? fixture.Space.BeginFaultMapping(cpuFirst.Preparation)
                               : decltype(fixture.Space.BeginFaultMapping(cpuFirst.Preparation))::Failure({});
        const auto complete =
            begin.HasValue()
                ? fixture.Space.CompleteFaultMapping(begin.GetValue().GetToken())
                : decltype(fixture.Space.CompleteFaultMapping(Kernel::Memory::AddressSpaceMappingToken{}))::Failure({});
        ASTERIA_EXPECT(runner, "CPU executable mapping of the unmapped DMA pages is admitted", complete.HasValue());
        if (complete.HasValue())
        {
            cpuFirst.Commit();
        }

        const Foundation::Size callsBeforeConflict = backend.MapCalls;
        const auto dmaAfterCpu = domain.Map(MakeDmaRequest(fixture, 0x0110'0000U, 5U, 1U));
        ASTERIA_EXPECT(runner, "CPU execute blocks a partially overlapping DMA write pre-HAL", !dmaAfterCpu.HasValue(),
                       IsDmaWriteExecuteConflict(dmaAfterCpu.GetError()), backend.MapCalls == callsBeforeConflict);

        const auto adjacent = domain.Map(MakeDmaRequest(fixture, 0x0120'0000U, 6U, 2U));
        ASTERIA_EXPECT(runner, "adjacent CPU-execute and DMA-write Frame ranges coexist", adjacent.HasValue());
        if (adjacent.HasValue())
        {
            ASTERIA_EXPECT(runner, "adjacent DMA mapping unmaps after its backend drain",
                           domain.Unmap(adjacent.GetValue().Mapping).Error == IommuDomainError::None);
        }
    }

    // An ambiguous DMA Map keeps its write reservation in the Faulted domain
    // until Quiesce, so no root can map those pages executable before then.
    backend.NextMap = IommuBackendError::CommittedStateUnknown;
    const auto ambiguous = domain.Map(MakeDmaRequest(fixture, 0x0130'0000U, 10U, 2U));
    {
        PreparedFault blockedByFaultedDomain{
            fixture, runner, 0x3000'0000U, 10U, ReadExecuteUser(), FaultAccess::Execute};
        const auto blocked =
            blockedByFaultedDomain.Prepared
                ? fixture.Space.BeginFaultMapping(blockedByFaultedDomain.Preparation)
                : decltype(fixture.Space.BeginFaultMapping(blockedByFaultedDomain.Preparation))::Failure({});
        ASTERIA_EXPECT(runner, "a Faulted IOMMU domain retains its DMA write reservation", !ambiguous.HasValue(),
                       ambiguous.GetError().Backend == IommuBackendError::CommittedStateUnknown, !blocked.HasValue(),
                       blocked.GetError().Error == AddressSpaceError::PhysicalWriteExecuteAlias);
    }

    ASTERIA_EXPECT(runner, "IOMMU domain quiesces before shared resource teardown",
                   domain.Quiesce().Error == IommuDomainError::None);

    PreparedFault afterQuiesce{fixture, runner, 0x3000'0000U, 10U, ReadExecuteUser(), FaultAccess::Execute};
    const auto afterQuiesceBegin =
        afterQuiesce.Prepared ? fixture.Space.BeginFaultMapping(afterQuiesce.Preparation)
                              : decltype(fixture.Space.BeginFaultMapping(afterQuiesce.Preparation))::Failure({});
    const auto afterQuiesceComplete =
        afterQuiesceBegin.HasValue()
            ? fixture.Space.CompleteFaultMapping(afterQuiesceBegin.GetValue().GetToken())
            : decltype(fixture.Space.CompleteFaultMapping(Kernel::Memory::AddressSpaceMappingToken{}))::Failure({});
    ASTERIA_EXPECT(runner, "IOMMU quiescence releases the retained DMA write reservation",
                   afterQuiesceComplete.HasValue());
    if (afterQuiesceComplete.HasValue())
    {
        afterQuiesce.Commit();
    }
}
// A record-backed accessor admits only a range one completed user runtime mapping holds, and copies
// through that address. A host buffer stands for the user pages, mapped at its own address.
void TestRecordUserMemoryAccess(TestContext& runner) noexcept
{
    alignas(4096) static Foundation::UInt8 user[4U * 4096U]{};
    const Foundation::UInt64 base = static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(user));
    AddressSpaceFixture fixture{runner, false, MemoryCommitment::Pinned};
    if (!fixture.IsReady())
    {
        return;
    }
    const auto map = [&](const Foundation::UInt64 virtualStart, const Foundation::UInt64 offset,
                         const PagePermissions permissions) noexcept
    {
        PinnedFrameMappingPreparation preparation;
        const PinnedFrameMappingError prepared =
            preparation.Prepare(fixture.Registry.Registry, fixture.GetMemoryObjectId(), fixture.MemoryPolicy.Object,
                                fixture.GetFrameId(), fixture.FrameStorage,
                                PinnedFrameMappingRequest{
                                    .VirtualStart = VirtualAddress{virtualStart},
                                    .Pages = PageCount{2U},
                                    .MemoryObjectOffsetPages = offset,
                                    .FrameOffsetPages = offset,
                                    .Permissions = permissions,
                                    .Reserved = {},
                                });
        const auto begin = fixture.Space.BeginPinnedFrameMapping(fixture.GetAddressSpaceId(), preparation);
        return (prepared == PinnedFrameMappingError::None) && begin.HasValue() &&
               fixture.Space.CompleteMapping(begin.GetValue().GetToken()).HasValue();
    };
    const PagePermissions readWrite = PagePermissions{PagePermission::Read}.With(PagePermission::Write);
    const bool mapped = map(base, 0U, readWrite) && map(base + 8192U, 2U, readWrite.With(PagePermission::User));
    Kernel::Memory::RecordUserMemoryAccess unbound;
    Kernel::Memory::RecordUserMemoryAccess access;
    const bool bound = access.Bind(fixture.Space, fixture.GetAddressSpaceId());
    const bool rebound = access.Bind(fixture.Space, fixture.GetAddressSpaceId());
    user[8192U + 5U] = 0x5AU;
    Foundation::UInt8 buffer[16U]{};
    const auto range = [](const Foundation::UInt64 start, const Foundation::Size size) noexcept
    {
        return Kernel::Memory::UserMemoryRange::Create(start, size).GetValue();
    };
    const Kernel::Memory::WritableKernelBuffer into{.Data = buffer, .SizeBytes = 8U};
    const auto copied = access.CopyFromUser(range(base + 8192U + 4U, 8U), into);
    const auto supervisorOnly = access.CopyFromUser(range(base + 4U, 8U), into);
    const auto beyondEnd = access.CopyFromUser(range(base + 16384U - 4U, 8U), into);
    const auto seed = access.CopyFromUser(range(0x1000U, 8U), into);
    const auto notReady = unbound.CopyFromUser(range(base + 8192U, 8U), into);
    ASTERIA_EXPECT(runner, "a record-backed copy reads only a user runtime mapping that holds the whole range", mapped,
                   bound, !rebound, copied == Kernel::Memory::UserMemoryAccessError::None, buffer[1U] == 0x5AU,
                   supervisorOnly == Kernel::Memory::UserMemoryAccessError::AccessDenied,
                   beyondEnd == Kernel::Memory::UserMemoryAccessError::AccessDenied,
                   seed == Kernel::Memory::UserMemoryAccessError::AccessDenied,
                   notReady == Kernel::Memory::UserMemoryAccessError::NotReady,
                   access.GetAddressSpaceId().IsEqualTo(fixture.GetAddressSpaceId()),
                   !unbound.GetAddressSpaceId().IsValid());

    // Existing reservations on different processors coexist; once mapping intent is published no
    // new switch can overtake the waiter.
    const auto firstRoot = fixture.Space.ReserveProcessorTransition(CpuId{0U});
    const auto secondRoot = fixture.Space.ReserveProcessorTransition(CpuId{1U});
    const bool independentReceipts = firstRoot.HasValue() && secondRoot.HasValue() &&
                                     fixture.Space.IsCurrentProcessorTransition(firstRoot.GetValue()) &&
                                     fixture.Space.IsCurrentProcessorTransition(secondRoot.GetValue());
    fixture.Space.BeginMappingIntent();
    const auto overtaker = fixture.Space.ReserveProcessorTransition(CpuId{2U});
    const auto blockedUnmap = fixture.Space.BeginUnmap(VirtualAddress{base + 8192U}, PageCount{2U});
    if (firstRoot.HasValue())
    {
        static_cast<void>(fixture.Space.ReleaseProcessorTransition(firstRoot.GetValue()));
    }
    if (secondRoot.HasValue())
    {
        static_cast<void>(fixture.Space.ReleaseProcessorTransition(secondRoot.GetValue()));
    }
    fixture.Space.EndMappingIntent();
    ASTERIA_EXPECT(runner, "per-processor reservations coexist and mapping intent prevents starvation",
                   firstRoot.HasValue(), secondRoot.HasValue(), independentReceipts, !overtaker.HasValue(),
                   !blockedUnmap.HasValue(),
                   !overtaker.HasValue() && overtaker.GetError() == AddressSpaceError::ProcessorTransitionPending,
                   fixture.Space.GetSnapshot().TransitionSequence == 0U);
    const Foundation::UInt8 written[4U]{1U, 2U, 3U, 4U};
    const auto reservation = access.PrepareCopyToUser(range(base + 12288U, 4U));
    const auto second = access.PrepareCopyToUser(range(base + 12288U, 4U));
    const bool busyReset = !access.Reset();
    const auto committed =
        reservation.HasValue()
            ? access.CommitCopyToUser(reservation.GetValue(), Kernel::Memory::ReadOnlyKernelBuffer{written, 4U})
            : Kernel::Memory::UserMemoryAccessError::StaleReservation;
    const auto supervisorWrite = access.PrepareCopyToUser(range(base, 4U));
    Kernel::Memory::RecordUserMemoryAccess sibling;
    const bool siblingBound = sibling.Bind(fixture.Space, fixture.GetAddressSpaceId());
    const auto pinnedWrite = access.PrepareCopyToUser(range(base + 12288U, 4U));
    const auto siblingWrite = sibling.PrepareCopyToUser(range(base + 12296U, 4U));
    Kernel::Memory::DirectUserMemoryAccess direct;
    const Kernel::Memory::DirectUserMemoryMapping directMapping{VirtualAddress{base + 8192U}, user + 8192U,
                                                                PageCount{2U}, readWrite.With(PagePermission::User),
                                                                Kernel::Memory::FrameKind::Ram};
    const bool directBound = direct.Initialize(fixture.GetAddressSpaceId(), &directMapping, 1U, &fixture.Space) ==
                             Kernel::Memory::DirectUserMemoryAccessError::None;
    const auto directWrite = direct.PrepareCopyToUser(range(base + 12304U, 4U));
    std::atomic<bool> unmapperWaiting{false};
    bool unmapWaitedForPins{false};
    bool unmapCompleted{false};
    bool copyDeniedAfterUnmap{false};
    std::thread unmapper{
        [&]() noexcept
        {
            fixture.Space.BeginMappingIntent();
            const auto busy = fixture.Space.BeginUnmap(VirtualAddress{base + 8192U}, PageCount{2U});
            unmapWaitedForPins = !busy.HasValue() && busy.GetError().Error == AddressSpaceError::UserMappingInUse;
            unmapperWaiting.store(true, std::memory_order_release);
            const auto begun = fixture.Space.WaitForUnmap(VirtualAddress{base + 8192U}, PageCount{2U});
            unmapCompleted = begun.HasValue() && fixture.Space.CompleteMapping(begun.GetValue().GetToken()).HasValue();
            fixture.Space.EndMappingIntent();
            copyDeniedAfterUnmap = !sibling.PrepareCopyToUser(range(base + 12296U, 4U)).HasValue();
        }};
    while (!unmapperWaiting.load(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }
    const auto published =
        pinnedWrite.HasValue()
            ? access.CommitCopyToUser(pinnedWrite.GetValue(), Kernel::Memory::ReadOnlyKernelBuffer{written, 4U})
            : Kernel::Memory::UserMemoryAccessError::StaleReservation;
    const auto canceled = siblingWrite.HasValue() ? sibling.CancelCopyToUser(siblingWrite.GetValue())
                                                  : Kernel::Memory::UserMemoryAccessError::StaleReservation;
    const auto directCanceled = directWrite.HasValue() ? direct.CancelCopyToUser(directWrite.GetValue())
                                                       : Kernel::Memory::UserMemoryAccessError::StaleReservation;
    unmapper.join();
    const auto stale =
        pinnedWrite.HasValue()
            ? access.CommitCopyToUser(pinnedWrite.GetValue(), Kernel::Memory::ReadOnlyKernelBuffer{written, 4U})
            : Kernel::Memory::UserMemoryAccessError::None;
    ASTERIA_EXPECT(
        runner, "unmap waits for both thread pins; no copy or stale commit writes after completion", siblingBound,
        pinnedWrite.HasValue(), siblingWrite.HasValue(), unmapWaitedForPins, unmapCompleted, copyDeniedAfterUnmap,
        published == Kernel::Memory::UserMemoryAccessError::None,
        canceled == Kernel::Memory::UserMemoryAccessError::None, directBound, directWrite.HasValue(),
        directCanceled == Kernel::Memory::UserMemoryAccessError::None,
        direct.CopyFromUser(range(base + 8192U, 8U), into) == Kernel::Memory::UserMemoryAccessError::AccessDenied,
        stale == Kernel::Memory::UserMemoryAccessError::StaleReservation, user[12296U] == 0U, user[12304U] == 0U);
    const bool reset = access.Reset();
    ASTERIA_EXPECT(
        runner, "a write reservation publishes into the mapping, one at a time, and pins the binding",
        reservation.HasValue(),
        !second.HasValue() && (second.GetError() == Kernel::Memory::UserMemoryAccessError::ReservationExhausted),
        busyReset, committed == Kernel::Memory::UserMemoryAccessError::None, user[12288U] == 1U, user[12291U] == 4U,
        !supervisorWrite.HasValue() &&
            (supervisorWrite.GetError() == Kernel::Memory::UserMemoryAccessError::AccessDenied),
        reset, !access.IsReady());
}
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"AddressSpaceTests"};
    Asteria::Test::TestInitializationAndIdentity(runner);
    Asteria::Test::TestActivationFailureClassification(runner);
    Asteria::Test::TestProcessorTransitionReservations(runner);
    Asteria::Test::TestShootdownAndMappingPublication(runner);
    Asteria::Test::TestShootdownDispatcherSuccess(runner);
    Asteria::Test::TestDetachAcknowledges(runner);
    Asteria::Test::TestPendingSourceRootRetry(runner);
    Asteria::Test::TestMutualInitiatorProgress(runner);
    Asteria::Test::TestShootdownMailboxTransport(runner);
    Asteria::Test::TestShootdownFirstOutcomeWins(runner);
    Asteria::Test::TestShootdownDispatcherFailures(runner);
    Asteria::Test::TestZeroCpuFailureAndAliasPolicies(runner);
    Asteria::Test::TestMalformedBackendSuccessIsTerminal(runner);
    Asteria::Test::TestAmbiguousBackendCommitIsTerminal(runner);
    Asteria::Test::TestDeviceRegionMappingLifetime(runner);
    Asteria::Test::TestCrossAddressSpaceWriteExecutePolicy(runner);
    Asteria::Test::TestDisjointAliasesAcrossRoots<Asteria::Kernel::Memory::BootFrame>(runner, true);
    Asteria::Test::TestDisjointAliasesAcrossRoots<Asteria::Kernel::Memory::CreatedFrame>(runner, false);
    Asteria::Test::TestReadOnlyBootstrapFrame(runner);
    Asteria::Test::TestPinnedFrameMappingTransaction(runner);
    Asteria::Test::TestRecordUserMemoryAccess(runner);
    Asteria::Test::TestPinnedFrameUnmapTransaction(runner);
    Asteria::Test::TestIommuAddressSpaceWriteExecutePolicy(runner);
    return runner.Finish();
}
