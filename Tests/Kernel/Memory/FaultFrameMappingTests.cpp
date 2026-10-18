// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Kernel/Device/DeviceRegion.hpp"
#include "Kernel/Memory/FaultFrameMapping.hpp"
#include "Kernel/Memory/PinnedFrameMapping.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"

#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
using Kernel::Device::DeviceAccessWidth;
using Kernel::Device::DeviceAccessWidths;
using Kernel::Device::DeviceRegion;
using Kernel::Device::DeviceRegionDescriptor;
using Kernel::Device::DeviceRegionError;
using Kernel::Device::DeviceRegionId;
using Kernel::Device::DeviceRegionKind;
using Kernel::Device::DeviceRegionPermission;
using Kernel::Device::DeviceRegionPermissions;
using Kernel::Memory::AddressSpaceId;
using Kernel::Memory::FaultAccess;
using Kernel::Memory::FaultCause;
using Kernel::Memory::FaultFrameMappingError;
using Kernel::Memory::FaultFrameMappingPlan;
using Kernel::Memory::FaultFrameMappingPreparation;
using Kernel::Memory::FaultFrameMappingRequest;
using Kernel::Memory::FaultPrivilege;
using Kernel::Memory::FaultRecord;
using Kernel::Memory::FaultResolutionKind;
using Kernel::Memory::FaultToken;
using Kernel::Memory::FaultTokenError;
using Kernel::Memory::BootFrame;
using Kernel::Memory::Frame;
using Kernel::Memory::FrameDescriptor;
using Kernel::Memory::FrameError;
using Kernel::Memory::FrameId;
using Kernel::Memory::FrameKind;
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

[[nodiscard]] constexpr ResourceDomainLimits MakeDomainLimits() noexcept
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

[[nodiscard]] constexpr MemoryObjectDescriptor MakeMemoryObjectDescriptor(
    const MemoryCachePolicy cachePolicy = MemoryCachePolicy::WriteBack,
    const PagePermissions maximumPermissions =
        PagePermissions{PagePermission::Read}.With(PagePermission::Write).With(PagePermission::User)) noexcept
{
    return MemoryObjectDescriptor{
        .Pages = PageCount{8U},
        .CachePolicy = cachePolicy,
        .MaximumPermissions = maximumPermissions,
        .Commitment = MemoryCommitment::BestEffort,
        .Reserved = {},
    };
}

class MemoryObjectFixture final
{
private:
    RegistryFixture& m_registry;
    TestContext& m_runner;

public:
    MemoryObjectFixture(RegistryFixture& registry, TestContext& runner,
                        const MemoryObjectDescriptor& descriptor = MakeMemoryObjectDescriptor()) noexcept
        : m_registry{registry}, m_runner{runner}, Domain{Ledger}
    {
        const auto domainInitialization = Domain.InitializeRoot(ResourceDomainId{90U}, MakeDomainLimits());
        if (domainInitialization.Error != ResourceDomainError::None)
        {
            return;
        }
        const auto domainRegistration = m_registry.Issuer.CreateObject(Domain);
        DomainObject = domainRegistration.HasValue() ? domainRegistration.GetValue() : ObjectId{};
        if (!DomainObject.IsValid())
        {
            return;
        }
        const ResourceDomainObjectId domainIdentity{m_registry.Registry, DomainObject};
        const auto objectInitialization = Object.Initialize(m_registry.Registry, domainIdentity, Domain, descriptor);
        if (objectInitialization.Error != MemoryObjectError::None)
        {
            return;
        }
        const auto objectRegistration = m_registry.Issuer.CreateObject(Object);
        ObjectIdValue = objectRegistration.HasValue() ? objectRegistration.GetValue() : ObjectId{};
        Identity = ObjectIdValue.IsValid() ? MemoryObjectId{m_registry.Registry, ObjectIdValue} : MemoryObjectId{};
    }

    ~MemoryObjectFixture() noexcept
    {
        if (ObjectIdValue.IsValid())
        {
            const auto quiesced = Object.Quiesce();
            const bool collected = (quiesced.Error == MemoryObjectError::None) &&
                                   (m_registry.Issuer.DestroyObject(ObjectIdValue) == ObjectRegistryError::Ok) &&
                                   m_registry.Issuer.CollectDestroyedObject().HasValue();
            ASTERIA_EXPECT(m_runner, "mapping fixture retires its MemoryObject", collected);
        }
        if (DomainObject.IsValid())
        {
            const bool collected = (Domain.Quiesce() == ResourceDomainError::None) &&
                                   (m_registry.Issuer.DestroyObject(DomainObject) == ObjectRegistryError::Ok) &&
                                   m_registry.Issuer.CollectDestroyedObject().HasValue();
            ASTERIA_EXPECT(m_runner, "mapping fixture retires its resource domain", collected);
        }
    }

    MemoryObjectFixture(const MemoryObjectFixture&) = delete;
    MemoryObjectFixture& operator=(const MemoryObjectFixture&) = delete;

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

[[nodiscard]] FaultFrameMappingError
PrepareForMemoryObject(FaultFrameMappingPreparation& owner, MemoryObjectFixture& memoryObject, ObjectRegistry& registry,
                       const FaultToken& faultToken, const Kernel::Memory::FaultResolutionClaimToken claim,
                       const FrameId frameId, Frame& frame, const FaultFrameMappingRequest& request,
                       const Kernel::Device::DeviceRegionId deviceRegionId = {},
                       Kernel::Device::DeviceRegion* const deviceRegion = nullptr) noexcept
{
    return owner.Prepare(registry, faultToken, claim, memoryObject.Identity, memoryObject.Object, deviceRegionId,
                         deviceRegion, frameId, frame, request);
}

class RegisteredObjectCleanup final
{
public:
    constexpr RegisteredObjectCleanup(RegistryFixture& fixture, TestContext& runner, const ObjectId object) noexcept
        : m_fixture{fixture}, m_runner{runner}, m_object{object}
    {
    }

    ~RegisteredObjectCleanup() noexcept
    {
        if (!m_object.IsValid())
        {
            return;
        }
        const ObjectRegistryError destroyError =
            m_wasDestroyed ? ObjectRegistryError::Ok : m_fixture.Issuer.DestroyObject(m_object);
        const auto collected = m_fixture.Issuer.CollectDestroyedObject();
        ASTERIA_EXPECT(m_runner, "mapping fixture collects registered storage after pins release",
                       destroyError == ObjectRegistryError::Ok, collected.HasValue(),
                       collected.GetValue().Object.GetValue() == m_object.GetValue());
    }

    RegisteredObjectCleanup(const RegisteredObjectCleanup&) = delete;
    RegisteredObjectCleanup& operator=(const RegisteredObjectCleanup&) = delete;
    RegisteredObjectCleanup(RegisteredObjectCleanup&&) = delete;
    RegisteredObjectCleanup& operator=(RegisteredObjectCleanup&&) = delete;

    /// Records that the fixture already started destruction while a retained
    /// pin was present. The destructor will collect without destroying twice.
    void MarkDestroyed() noexcept
    {
        m_wasDestroyed = true;
    }

    /// Records that the fixture already completed destruction and collection.
    void MarkCollected() noexcept
    {
        m_object = ObjectId{};
        m_wasDestroyed = false;
    }

private:
    RegistryFixture& m_fixture;
    TestContext& m_runner;
    ObjectId m_object;
    bool m_wasDestroyed{false};
};

[[nodiscard]] ObjectId Register(RegistryFixture& fixture, IKernelObject& object, TestContext& runner,
                                const char* const description) noexcept
{
    const auto result = fixture.Issuer.CreateObject(object);
    ASTERIA_EXPECT(runner, description, result.HasValue());
    return result.HasValue() ? result.GetValue() : ObjectId{};
}

[[nodiscard]] constexpr PagePermissions ReadWriteUser() noexcept
{
    return PagePermissions{PagePermission::Read}.With(PagePermission::Write).With(PagePermission::User);
}

[[nodiscard]] constexpr PagePermissions ReadExecuteUser() noexcept
{
    return PagePermissions{PagePermission::Read}.With(PagePermission::Execute).With(PagePermission::User);
}

[[nodiscard]] constexpr FaultRecord MakeFaultRecord(const FaultAccess access = FaultAccess::Write) noexcept
{
    return FaultRecord{
        .VirtualAddress = 0x0000'0000'0040'1234ULL,
        .InstructionPointer = 0x0000'0000'0080'0000ULL,
        .ArchitectureSyndrome = 0xA55AU,
        .Access = access,
        .Cause = FaultCause::MissingMapping,
        .Privilege = FaultPrivilege::User,
        .Reserved = {},
    };
}

[[nodiscard]] constexpr FrameDescriptor
MakeFrameDescriptor(const FrameKind kind = FrameKind::Ram,
                    const MemoryCachePolicy cachePolicy = MemoryCachePolicy::WriteBack) noexcept
{
    return FrameDescriptor{
        .PhysicalStart = PhysicalAddress{0x0020'0000U},
        .Pages = PageCount{8U},
        .CachePolicy = cachePolicy,
        .Kind = kind,
        .Reserved = {},
    };
}

[[nodiscard]] constexpr FaultFrameMappingRequest MakeRequest() noexcept
{
    return FaultFrameMappingRequest{
        .VirtualStart = VirtualAddress{0x0040'0000U},
        .Pages = PageCount{2U},
        .MemoryObjectOffsetPages = 1U,
        .FrameOffsetPages = 2U,
        .Permissions = ReadWriteUser(),
        .Reserved = {},
    };
}

[[nodiscard]] bool PlanReservedIsZero(const FaultFrameMappingPlan& plan) noexcept
{
    for (Foundation::Size index{0U}; index < sizeof(plan.Reserved); ++index)
    {
        if (plan.Reserved[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

void TestValidationAndSuccessfulPreparation(TestContext& runner) noexcept
{
    TestKernelObject thread{ObjectType::Thread, ResourceDomainId{1U}};
    TestKernelObject addressSpace{ObjectType::AddressSpace, ResourceDomainId{2U}};
    BootFrame frame;
    BootFrame otherFrame;
    RegistryFixture fixture;
    RegistryFixture otherFixture;
    MemoryObjectFixture memoryObject{fixture, runner};
    MemoryObject otherMemoryObject;

    ASTERIA_EXPECT(runner, "mapping fixture initializes immutable RAM Frames",
                   frame.Initialize(ResourceDomainId{3U}, MakeFrameDescriptor()) == FrameError::None,
                   otherFrame.Initialize(ResourceDomainId{4U}, MakeFrameDescriptor()) == FrameError::None);
    const ObjectId threadObject = Register(fixture, thread, runner, "mapping fixture registers its Thread");
    const ObjectId addressSpaceObject =
        Register(fixture, addressSpace, runner, "mapping fixture registers its AddressSpace");
    const ObjectId frameObject = Register(fixture, frame, runner, "mapping fixture registers its Frame");
    RegisteredObjectCleanup threadCleanup{fixture, runner, threadObject};
    RegisteredObjectCleanup addressSpaceCleanup{fixture, runner, addressSpaceObject};
    RegisteredObjectCleanup frameCleanup{fixture, runner, frameObject};
    if (!threadObject.IsValid() || !addressSpaceObject.IsValid() || !frameObject.IsValid() || !memoryObject.IsReady())
    {
        return;
    }

    FaultToken faultToken;
    ASTERIA_EXPECT(runner, "mapping fixture publishes one user write fault",
                   faultToken.Initialize(fixture.Registry, ResourceDomainId{5U},
                                         ThreadId{fixture.Registry, threadObject},
                                         AddressSpaceId{fixture.Registry, addressSpaceObject},
                                         MakeFaultRecord()) == FaultTokenError::None);
    const auto firstClaim = faultToken.ClaimResolution();
    ASTERIA_EXPECT(runner, "fault resolution acquires its first claim", firstClaim.HasValue());
    if (!firstClaim.HasValue())
    {
        return;
    }

    FaultFrameMappingPreparation preparation;
    ASTERIA_EXPECT(runner, "cancelled claim cannot prepare a mapping or retain a Frame",
                   faultToken.CancelResolution(firstClaim.GetValue().Token) == FaultTokenError::None,
                   PrepareForMemoryObject(preparation, memoryObject, fixture.Registry, faultToken,
                                          firstClaim.GetValue().Token, FrameId{fixture.Registry, frameObject}, frame,
                                          MakeRequest()) == FaultFrameMappingError::InvalidClaim,
                   !preparation.IsPrepared());
    const auto secondClaim = faultToken.ClaimResolution();
    ASTERIA_EXPECT(runner, "mapping preparation uses a fresh nonreused claim generation", secondClaim.HasValue(),
                   secondClaim.GetValue().Token.GetSequence() > firstClaim.GetValue().Token.GetSequence());
    if (!secondClaim.HasValue())
    {
        return;
    }
    const auto claim = secondClaim.GetValue().Token;

    ASTERIA_EXPECT(runner, "invalid MemoryObject identity is rejected",
                   preparation.Prepare(fixture.Registry, faultToken, claim, MemoryObjectId{}, memoryObject.Object, {},
                                       nullptr, FrameId{fixture.Registry, frameObject}, frame,
                                       MakeRequest()) == FaultFrameMappingError::InvalidMemoryObject,
                   !preparation.IsPrepared());
    ASTERIA_EXPECT(runner, "MemoryObject registry provenance must match the fault claim",
                   preparation.Prepare(fixture.Registry, faultToken, claim,
                                       MemoryObjectId{otherFixture.Registry, memoryObject.ObjectIdValue},
                                       memoryObject.Object, {}, nullptr, FrameId{fixture.Registry, frameObject}, frame,
                                       MakeRequest()) == FaultFrameMappingError::RegistryMismatch,
                   !preparation.IsPrepared());
    ASTERIA_EXPECT(runner, "typed registry pin rejects a non-MemoryObject",
                   preparation.Prepare(fixture.Registry, faultToken, claim,
                                       MemoryObjectId{fixture.Registry, threadObject}, memoryObject.Object, {}, nullptr,
                                       FrameId{fixture.Registry, frameObject}, frame,
                                       MakeRequest()) == FaultFrameMappingError::MemoryObjectTypeMismatch,
                   !preparation.IsPrepared());
    ASTERIA_EXPECT(runner, "MemoryObject identity names the exact logical policy object",
                   preparation.Prepare(fixture.Registry, faultToken, claim, memoryObject.Identity, otherMemoryObject,
                                       {}, nullptr, FrameId{fixture.Registry, frameObject}, frame,
                                       MakeRequest()) == FaultFrameMappingError::MemoryObjectInstanceMismatch,
                   !preparation.IsPrepared());

    ASTERIA_EXPECT(runner, "invalid Frame identity is rejected without changing the owner",
                   PrepareForMemoryObject(preparation, memoryObject, fixture.Registry, faultToken, claim, FrameId{},
                                          frame, MakeRequest()) == FaultFrameMappingError::InvalidFrame,
                   !preparation.IsPrepared());
    ASTERIA_EXPECT(runner, "Frame registry provenance must match the claimed AddressSpace",
                   PrepareForMemoryObject(preparation, memoryObject, fixture.Registry, faultToken, claim,
                                          FrameId{otherFixture.Registry, frameObject}, frame,
                                          MakeRequest()) == FaultFrameMappingError::RegistryMismatch,
                   !preparation.IsPrepared());
    ASTERIA_EXPECT(runner, "typed registry pin rejects a non-Frame object",
                   PrepareForMemoryObject(preparation, memoryObject, fixture.Registry, faultToken, claim,
                                          FrameId{fixture.Registry, threadObject}, frame,
                                          MakeRequest()) == FaultFrameMappingError::FrameTypeMismatch,
                   !preparation.IsPrepared());
    ASTERIA_EXPECT(runner, "unknown Frame generation is rejected before any plan is published",
                   PrepareForMemoryObject(preparation, memoryObject, fixture.Registry, faultToken, claim,
                                          FrameId{fixture.Registry, ObjectId{~Foundation::UInt64{0U}}}, frame,
                                          MakeRequest()) == FaultFrameMappingError::FrameNotFound,
                   !preparation.IsPrepared());
    ASTERIA_EXPECT(runner, "FrameId must name the exact concrete Frame supplied by the mapper",
                   PrepareForMemoryObject(preparation, memoryObject, fixture.Registry, faultToken, claim,
                                          FrameId{fixture.Registry, frameObject}, otherFrame,
                                          MakeRequest()) == FaultFrameMappingError::FrameInstanceMismatch,
                   !preparation.IsPrepared());

    FaultFrameMappingRequest invalid = MakeRequest();
    invalid.VirtualStart = VirtualAddress{0x0040'0001U};
    ASTERIA_EXPECT(runner, "runtime mapping virtual start must be page aligned",
                   PrepareForMemoryObject(preparation, memoryObject, fixture.Registry, faultToken, claim,
                                          FrameId{fixture.Registry, frameObject}, frame,
                                          invalid) == FaultFrameMappingError::InvalidRequest);
    invalid = MakeRequest();
    invalid.Pages = PageCount{};
    ASTERIA_EXPECT(runner, "empty runtime mapping is rejected",
                   PrepareForMemoryObject(preparation, memoryObject, fixture.Registry, faultToken, claim,
                                          FrameId{fixture.Registry, frameObject}, frame,
                                          invalid) == FaultFrameMappingError::InvalidRequest);
    invalid = MakeRequest();
    invalid.Pages = PageCount{(~Foundation::UInt64{0U} / 4096U) + 1U};
    ASTERIA_EXPECT(runner, "runtime mapping page-to-byte multiplication overflow is rejected",
                   PrepareForMemoryObject(preparation, memoryObject, fixture.Registry, faultToken, claim,
                                          FrameId{fixture.Registry, frameObject}, frame,
                                          invalid) == FaultFrameMappingError::InvalidRequest);
    invalid = MakeRequest();
    invalid.VirtualStart = VirtualAddress{0xFFFF'FFFF'FFFF'F000ULL};
    ASTERIA_EXPECT(runner, "runtime mapping exclusive-end overflow is rejected",
                   PrepareForMemoryObject(preparation, memoryObject, fixture.Registry, faultToken, claim,
                                          FrameId{fixture.Registry, frameObject}, frame,
                                          invalid) == FaultFrameMappingError::InvalidRequest);
    invalid = MakeRequest();
    invalid.Permissions = PagePermissions{static_cast<PagePermission>(0x80U)};
    ASTERIA_EXPECT(runner, "unknown page protection bits are rejected",
                   PrepareForMemoryObject(preparation, memoryObject, fixture.Registry, faultToken, claim,
                                          FrameId{fixture.Registry, frameObject}, frame,
                                          invalid) == FaultFrameMappingError::InvalidRequest);
    invalid = MakeRequest();
    invalid.Permissions = PagePermissions{PagePermission::Write}.With(PagePermission::User);
    ASTERIA_EXPECT(runner, "runtime mappings always require readable pages",
                   PrepareForMemoryObject(preparation, memoryObject, fixture.Registry, faultToken, claim,
                                          FrameId{fixture.Registry, frameObject}, frame,
                                          invalid) == FaultFrameMappingError::InvalidRequest);
    invalid = MakeRequest();
    invalid.Permissions = ReadWriteUser().With(PagePermission::Execute);
    ASTERIA_EXPECT(runner, "write and execute permission cannot be combined",
                   PrepareForMemoryObject(preparation, memoryObject, fixture.Registry, faultToken, claim,
                                          FrameId{fixture.Registry, frameObject}, frame,
                                          invalid) == FaultFrameMappingError::InvalidRequest);
    invalid = MakeRequest();
    invalid.Permissions = PagePermissions{PagePermission::Read}.With(PagePermission::User);
    ASTERIA_EXPECT(runner, "write fault requires writable page protection",
                   PrepareForMemoryObject(preparation, memoryObject, fixture.Registry, faultToken, claim,
                                          FrameId{fixture.Registry, frameObject}, frame,
                                          invalid) == FaultFrameMappingError::AccessDenied);
    invalid = MakeRequest();
    invalid.Permissions = PagePermissions{PagePermission::Read}.With(PagePermission::Write);
    ASTERIA_EXPECT(runner, "user fault requires user-accessible page protection",
                   PrepareForMemoryObject(preparation, memoryObject, fixture.Registry, faultToken, claim,
                                          FrameId{fixture.Registry, frameObject}, frame,
                                          invalid) == FaultFrameMappingError::AccessDenied);
    invalid = MakeRequest();
    invalid.VirtualStart = VirtualAddress{0x0040'2000U};
    ASTERIA_EXPECT(runner, "prepared range must contain the faulting virtual address",
                   PrepareForMemoryObject(preparation, memoryObject, fixture.Registry, faultToken, claim,
                                          FrameId{fixture.Registry, frameObject}, frame,
                                          invalid) == FaultFrameMappingError::FaultOutsideMapping);
    invalid = MakeRequest();
    invalid.Pages = PageCount{1U};
    ASTERIA_EXPECT(runner, "fault at or beyond the exclusive virtual end is rejected",
                   PrepareForMemoryObject(preparation, memoryObject, fixture.Registry, faultToken, claim,
                                          FrameId{fixture.Registry, frameObject}, frame,
                                          invalid) == FaultFrameMappingError::FaultOutsideMapping);
    invalid = MakeRequest();
    invalid.MemoryObjectOffsetPages = 7U;
    ASTERIA_EXPECT(runner, "requested logical slice remains inside the MemoryObject",
                   PrepareForMemoryObject(preparation, memoryObject, fixture.Registry, faultToken, claim,
                                          FrameId{fixture.Registry, frameObject}, frame,
                                          invalid) == FaultFrameMappingError::MemoryObjectRangeExceeded);
    invalid = MakeRequest();
    invalid.FrameOffsetPages = 7U;
    ASTERIA_EXPECT(runner, "requested Frame slice must remain within immutable ownership",
                   PrepareForMemoryObject(preparation, memoryObject, fixture.Registry, faultToken, claim,
                                          FrameId{fixture.Registry, frameObject}, frame,
                                          invalid) == FaultFrameMappingError::FrameRangeExceeded);
    for (Foundation::Size index{0U}; index < sizeof(invalid.Reserved); ++index)
    {
        invalid = MakeRequest();
        invalid.Reserved[index] = 1U;
        ASTERIA_EXPECT(runner, "every request reserved byte must remain zero",
                       PrepareForMemoryObject(preparation, memoryObject, fixture.Registry, faultToken, claim,
                                              FrameId{fixture.Registry, frameObject}, frame,
                                              invalid) == FaultFrameMappingError::InvalidRequest);
    }

    ASTERIA_EXPECT(runner, "valid claimed user write prepares and pins one Frame slice",
                   PrepareForMemoryObject(preparation, memoryObject, fixture.Registry, faultToken, claim,
                                          FrameId{fixture.Registry, frameObject}, frame,
                                          MakeRequest()) == FaultFrameMappingError::None);
    const FaultFrameMappingPlan plan = preparation.GetPlan();
    ASTERIA_EXPECT(
        runner, "prepared plan preserves authoritative identities, extent, and cache policy", preparation.IsPrepared(),
        plan.Claim.IsEqualTo(claim), plan.AddressSpace.IsEqualTo(AddressSpaceId{fixture.Registry, addressSpaceObject}),
        plan.MemoryObject.IsEqualTo(memoryObject.Identity),
        plan.Frame.IsEqualTo(FrameId{fixture.Registry, frameObject}), plan.PhysicalStart.GetValue() == 0x0020'2000U,
        plan.VirtualStart.GetValue() == 0x0040'0000U, plan.Pages.GetValue() == 2U, plan.MemoryObjectOffsetPages == 1U,
        plan.Permissions.GetBits() == ReadWriteUser().GetBits(), plan.CachePolicy == MemoryCachePolicy::WriteBack,
        plan.Kind == FrameKind::Ram, plan.Commitment == MemoryCommitment::BestEffort, PlanReservedIsZero(plan));
    ASTERIA_EXPECT(runner, "successful mapping preparation is single-use",
                   PrepareForMemoryObject(preparation, memoryObject, fixture.Registry, faultToken, claim,
                                          FrameId{fixture.Registry, frameObject}, frame,
                                          MakeRequest()) == FaultFrameMappingError::AlreadyPrepared);
    ASTERIA_EXPECT(runner, "mapper can commit the exact prepared claim after installation",
                   faultToken.CommitResolution(plan.Claim, FaultResolutionKind::Map) == FaultTokenError::None);
    const ObjectRegistryError destroyFrameError = fixture.Issuer.DestroyObject(frameObject);
    const auto pinnedCollection = fixture.Issuer.CollectDestroyedObject();
    ASTERIA_EXPECT(runner, "prepared mapping retains its Frame generation until owner destruction",
                   destroyFrameError == ObjectRegistryError::Ok, !pinnedCollection.HasValue(),
                   pinnedCollection.GetError() == ObjectRegistryError::NoDestructionReady);
    if (destroyFrameError == ObjectRegistryError::Ok)
    {
        frameCleanup.MarkDestroyed();
    }
}

void TestDeviceExecutionDenied(TestContext& runner) noexcept
{
    TestKernelObject thread{ObjectType::Thread, ResourceDomainId{11U}};
    TestKernelObject addressSpace{ObjectType::AddressSpace, ResourceDomainId{12U}};
    BootFrame deviceFrame;
    RegistryFixture fixture;
    MemoryObjectFixture memoryObject{fixture, runner,
                                     MakeMemoryObjectDescriptor(MemoryCachePolicy::Uncached, ReadExecuteUser())};
    FrameDescriptor descriptor = MakeFrameDescriptor(FrameKind::Device, MemoryCachePolicy::Uncached);
    descriptor.PhysicalStart = PhysicalAddress{0x8000'0000U};
    ASTERIA_EXPECT(runner, "device execution fixture initializes an uncached Frame",
                   deviceFrame.Initialize(ResourceDomainId{13U}, descriptor) == FrameError::None);
    const ObjectId threadObject = Register(fixture, thread, runner, "device fixture registers its Thread");
    const ObjectId addressSpaceObject =
        Register(fixture, addressSpace, runner, "device fixture registers its AddressSpace");
    const ObjectId frameObject = Register(fixture, deviceFrame, runner, "device fixture registers its Frame");
    RegisteredObjectCleanup threadCleanup{fixture, runner, threadObject};
    RegisteredObjectCleanup addressSpaceCleanup{fixture, runner, addressSpaceObject};
    RegisteredObjectCleanup frameCleanup{fixture, runner, frameObject};
    if (!threadObject.IsValid() || !addressSpaceObject.IsValid() || !frameObject.IsValid() || !memoryObject.IsReady())
    {
        return;
    }

    FaultToken faultToken;
    ASTERIA_EXPECT(runner, "device fixture publishes one user execute fault",
                   faultToken.Initialize(fixture.Registry, ResourceDomainId{14U},
                                         ThreadId{fixture.Registry, threadObject},
                                         AddressSpaceId{fixture.Registry, addressSpaceObject},
                                         MakeFaultRecord(FaultAccess::Execute)) == FaultTokenError::None);
    const auto claim = faultToken.ClaimResolution();
    ASTERIA_EXPECT(runner, "device fixture acquires one fault claim", claim.HasValue());
    if (!claim.HasValue())
    {
        return;
    }

    FaultFrameMappingRequest request = MakeRequest();
    request.FrameOffsetPages = 0U;
    request.Permissions = ReadExecuteUser();
    FaultFrameMappingPreparation preparation;
    ASTERIA_EXPECT(runner, "device Frame can never back executable user memory",
                   PrepareForMemoryObject(preparation, memoryObject, fixture.Registry, faultToken,
                                          claim.GetValue().Token, FrameId{fixture.Registry, frameObject}, deviceFrame,
                                          request) == FaultFrameMappingError::DeviceExecutionDenied,
                   !preparation.IsPrepared());
    const ObjectRegistryError destroyFrameError = fixture.Issuer.DestroyObject(frameObject);
    const auto collectedFrame = fixture.Issuer.CollectDestroyedObject();
    ASTERIA_EXPECT(runner, "failed device preparation releases its provisional Frame pin",
                   destroyFrameError == ObjectRegistryError::Ok, collectedFrame.HasValue(),
                   collectedFrame.GetValue().Object.GetValue() == frameObject.GetValue());
    if ((destroyFrameError == ObjectRegistryError::Ok) && collectedFrame.HasValue())
    {
        frameCleanup.MarkCollected();
    }
    ASTERIA_EXPECT(runner, "failed device mapping leaves the fault claim cancellable",
                   faultToken.CancelResolution(claim.GetValue().Token) == FaultTokenError::None);
}

// The device downgrade must not make a WC RAM Frame compatible with UC,
// including the ordinary pinned mapping preparation used by MemoryMap.
void TestRamCachePolicyRemainsExact(TestContext& runner) noexcept
{
    RegistryFixture fixture;
    auto descriptor = MakeMemoryObjectDescriptor(MemoryCachePolicy::Uncached, ReadWriteUser());
    descriptor.Commitment = MemoryCommitment::Pinned;
    MemoryObjectFixture memory{fixture, runner, descriptor};
    BootFrame frame;
    ASTERIA_EXPECT(runner, "RAM cache mismatch fixture initializes", memory.IsReady(),
                   frame.Initialize(ResourceDomainId{3U},
                                    MakeFrameDescriptor(FrameKind::Ram, MemoryCachePolicy::WriteCombining)) ==
                       FrameError::None);
    const ObjectId frameObject = Register(fixture, frame, runner, "RAM policy fixture registers its Frame");
    RegisteredObjectCleanup cleanup{fixture, runner, frameObject};
    Kernel::Memory::PinnedFrameMappingPreparation preparation;
    ASTERIA_EXPECT(runner, "UC downgrade of a WC RAM Frame stays refused without retaining pins",
                   preparation.Prepare(fixture.Registry, memory.Identity, memory.Object,
                                       FrameId{fixture.Registry, frameObject}, frame,
                                       Kernel::Memory::PinnedFrameMappingRequest{
                                           .VirtualStart = VirtualAddress{0x0040'0000U},
                                           .Pages = PageCount{1U},
                                           .MemoryObjectOffsetPages = 0U,
                                           .FrameOffsetPages = 0U,
                                           .Permissions = ReadWriteUser(),
                                           .Reserved = {},
                                       }) == Kernel::Memory::PinnedFrameMappingError::MemoryObjectCacheMismatch,
                   !preparation.IsPrepared());
}

void TestDeviceRegionAuthority(TestContext& runner, const MemoryCachePolicy selected) noexcept
{
    TestKernelObject thread{ObjectType::Thread, ResourceDomainId{21U}};
    TestKernelObject addressSpace{ObjectType::AddressSpace, ResourceDomainId{22U}};
    RegistryFixture fixture;
    RegistryFixture foreign;
    MemoryObjectFixture memoryObject{fixture, runner, MakeMemoryObjectDescriptor(selected, ReadWriteUser())};
    DeviceRegion deviceRegion;
    DeviceRegion otherRegion;
    BootFrame deviceFrame;

    const DeviceRegionDescriptor regionDescriptor{
        .BaseAddress = 0x8000'0000U,
        .LengthBytes = 8U * 4096U,
        .Kind = DeviceRegionKind::MemoryMappedIo,
        .Permissions = DeviceRegionPermissions{DeviceRegionPermission::Read}.With(DeviceRegionPermission::Write),
        .Widths = DeviceAccessWidths{DeviceAccessWidth::Byte}
                      .With(DeviceAccessWidth::Word)
                      .With(DeviceAccessWidth::DoubleWord)
                      .With(DeviceAccessWidth::QuadWord),
        .CachePolicy = MemoryCachePolicy::WriteCombining,
        .Reserved = {},
    };
    ASTERIA_EXPECT(
        runner, "device authority fixture initializes exact and alternate MMIO regions", memoryObject.IsReady(),
        deviceRegion
                .Initialize(fixture.Registry, ResourceDomainObjectId{fixture.Registry, memoryObject.DomainObject},
                            memoryObject.Domain, regionDescriptor)
                .Error == DeviceRegionError::None,
        otherRegion
                .Initialize(fixture.Registry, ResourceDomainObjectId{fixture.Registry, memoryObject.DomainObject},
                            memoryObject.Domain, regionDescriptor)
                .Error == DeviceRegionError::None);

    FrameDescriptor frameDescriptor = MakeFrameDescriptor(FrameKind::Device, MemoryCachePolicy::WriteCombining);
    frameDescriptor.PhysicalStart = PhysicalAddress{0x8000'0000U};
    ASTERIA_EXPECT(runner, "device authority fixture initializes matching physical Frame",
                   deviceFrame.Initialize(ResourceDomainId{23U}, frameDescriptor) == FrameError::None);

    const ObjectId threadObject = Register(fixture, thread, runner, "device authority fixture registers Thread");
    const ObjectId addressSpaceObject =
        Register(fixture, addressSpace, runner, "device authority fixture registers AddressSpace");
    const ObjectId deviceRegionObject =
        Register(fixture, deviceRegion, runner, "device authority fixture registers DeviceRegion");
    const ObjectId frameObject = Register(fixture, deviceFrame, runner, "device authority fixture registers Frame");
    RegisteredObjectCleanup threadCleanup{fixture, runner, threadObject};
    RegisteredObjectCleanup addressSpaceCleanup{fixture, runner, addressSpaceObject};
    RegisteredObjectCleanup deviceRegionCleanup{fixture, runner, deviceRegionObject};
    RegisteredObjectCleanup frameCleanup{fixture, runner, frameObject};
    if (!threadObject.IsValid() || !addressSpaceObject.IsValid() || !deviceRegionObject.IsValid() ||
        !frameObject.IsValid() || !memoryObject.IsReady())
    {
        return;
    }

    {
        FaultToken faultToken;
        ASTERIA_EXPECT(runner, "device authority fixture publishes one user write fault",
                       faultToken.Initialize(fixture.Registry, ResourceDomainId{24U},
                                             ThreadId{fixture.Registry, threadObject},
                                             AddressSpaceId{fixture.Registry, addressSpaceObject},
                                             MakeFaultRecord(FaultAccess::Write)) == FaultTokenError::None);
        const auto claim = faultToken.ClaimResolution();
        ASTERIA_EXPECT(runner, "device authority fixture claims its fault", claim.HasValue());
        if (!claim.HasValue())
        {
            return;
        }

        FaultFrameMappingRequest request = MakeRequest();
        request.FrameOffsetPages = 0U;
        FaultFrameMappingPreparation missing;
        ASTERIA_EXPECT(runner, "device Frame cannot be mapped without DeviceRegion authority",
                       PrepareForMemoryObject(missing, memoryObject, fixture.Registry, faultToken,
                                              claim.GetValue().Token, FrameId{fixture.Registry, frameObject},
                                              deviceFrame, request) == FaultFrameMappingError::DeviceRegionRequired);

        FaultFrameMappingPreparation wrongRegistry;
        ASTERIA_EXPECT(runner, "DeviceRegion identity must share the claimed registry",
                       PrepareForMemoryObject(wrongRegistry, memoryObject, fixture.Registry, faultToken,
                                              claim.GetValue().Token, FrameId{fixture.Registry, frameObject},
                                              deviceFrame, request,
                                              DeviceRegionId{foreign.Registry, deviceRegionObject},
                                              &deviceRegion) == FaultFrameMappingError::RegistryMismatch);

        FaultFrameMappingPreparation wrongType;
        ASTERIA_EXPECT(runner, "typed DeviceRegion pin rejects a different object type",
                       PrepareForMemoryObject(wrongType, memoryObject, fixture.Registry, faultToken,
                                              claim.GetValue().Token, FrameId{fixture.Registry, frameObject},
                                              deviceFrame, request, DeviceRegionId{fixture.Registry, threadObject},
                                              &deviceRegion) == FaultFrameMappingError::DeviceRegionTypeMismatch);

        FaultFrameMappingPreparation wrongInstance;
        ASTERIA_EXPECT(runner, "DeviceRegion identity names the exact MMIO policy instance",
                       PrepareForMemoryObject(wrongInstance, memoryObject, fixture.Registry, faultToken,
                                              claim.GetValue().Token, FrameId{fixture.Registry, frameObject},
                                              deviceFrame, request,
                                              DeviceRegionId{fixture.Registry, deviceRegionObject},
                                              &otherRegion) == FaultFrameMappingError::DeviceRegionInstanceMismatch);

        FaultFrameMappingPreparation preparation;
        ASTERIA_EXPECT(
            runner, "prepared device mapping retains exact DeviceRegion authority",
            PrepareForMemoryObject(preparation, memoryObject, fixture.Registry, faultToken, claim.GetValue().Token,
                                   FrameId{fixture.Registry, frameObject}, deviceFrame, request,
                                   DeviceRegionId{fixture.Registry, deviceRegionObject},
                                   &deviceRegion) == FaultFrameMappingError::None,
            preparation.IsPrepared(), preparation.GetPlan().CachePolicy == selected,
            preparation.GetPlan().DeviceRegion.IsEqualTo(DeviceRegionId{fixture.Registry, deviceRegionObject}));
        ASTERIA_EXPECT(runner, "device authority fixture commits its prepared claim",
                       faultToken.CommitResolution(claim.GetValue().Token, FaultResolutionKind::Map) ==
                           FaultTokenError::None);
    }

    ASTERIA_EXPECT(runner, "preparation scope releases MMIO pins without creating live mappings",
                   deviceRegion.Quiesce().Error == DeviceRegionError::None,
                   otherRegion.Quiesce().Error == DeviceRegionError::None);
}
} // namespace Asteria::Test

int main()
{
    TestContext runner{"FaultFrameMappingTests"};
    Asteria::Test::TestValidationAndSuccessfulPreparation(runner);
    Asteria::Test::TestDeviceExecutionDenied(runner);
    Asteria::Test::TestDeviceRegionAuthority(runner, Asteria::Kernel::Memory::MemoryCachePolicy::Uncached);
    Asteria::Test::TestDeviceRegionAuthority(runner, Asteria::Kernel::Memory::MemoryCachePolicy::WriteCombining);
    Asteria::Test::TestRamCachePolicyRemainsExact(runner);
    return runner.Finish();
}
