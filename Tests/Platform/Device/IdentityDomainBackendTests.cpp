// SPDX-License-Identifier: Apache-2.0
#include "Platform/Device/IdentityDomainBackend.hpp"

#include "Kernel/Device/IommuDomain.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"

#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::UInt64;
using Hal::Device::IIommuDevice;
using Hal::Device::IommuBackendError;
using Hal::Device::IommuBackendMapRequest;
using Hal::Device::IommuBackendOwnershipError;
using Hal::Device::IommuDeviceError;
using Kernel::Device::DmaAddress;
using Kernel::Device::DmaPermission;
using Kernel::Device::DmaPermissions;
using Kernel::Device::IommuDomain;
using Kernel::Device::IommuDomainConfiguration;
using Kernel::Device::IommuDomainError;
using Kernel::Device::IommuMappingRequest;
using Kernel::Memory::BootFrame;
using Kernel::Memory::Frame;
using Kernel::Memory::FrameDescriptor;
using Kernel::Memory::FrameError;
using Kernel::Memory::FrameId;
using Kernel::Memory::FrameKind;
using Kernel::Memory::MemoryCachePolicy;
using Kernel::Memory::PageCount;
using Kernel::Memory::PhysicalAddress;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Resource::ResourceDomain;
using Kernel::Resource::ResourceDomainError;
using Kernel::Resource::ResourceDomainLedger;
using Kernel::Resource::ResourceDomainLimits;
using Kernel::Resource::ResourceDomainObjectId;
using Platform::Device::IdentityDomainBackend;
using Platform::Synchronization::SpinLock;

// An endpoint the boot did, or did not, grant trusted DMA.
class Endpoint final : public IIommuDevice
{
public:
    explicit Endpoint(const bool trusted) noexcept : m_trusted{trusted}
    {
    }

    [[nodiscard]] IommuDeviceError Claim(const IommuDomain& owner) noexcept override
    {
        if (m_owner != nullptr)
        {
            return IommuDeviceError::AlreadyClaimed;
        }
        m_owner = &owner;
        return IommuDeviceError::None;
    }

    [[nodiscard]] IommuDeviceError Release(const IommuDomain& owner) noexcept override
    {
        if (m_owner != &owner)
        {
            return IommuDeviceError::InvalidOwner;
        }
        m_owner = nullptr;
        return IommuDeviceError::None;
    }

    [[nodiscard]] Foundation::UInt32 GetRequesterId() const noexcept override
    {
        return 0x00F8U;
    }

    [[nodiscard]] bool HasTrustedDma() const noexcept override
    {
        return m_trusted;
    }

private:
    const IommuDomain* m_owner{nullptr};
    bool m_trusted;
};

[[nodiscard]] constexpr IommuBackendMapRequest Request(const UInt64 device, const UInt64 physical) noexcept
{
    return IommuBackendMapRequest{.DmaStart = DmaAddress{device},
                                  .PhysicalStart = PhysicalAddress{physical},
                                  .Pages = PageCount{1U},
                                  .Permissions = DmaPermissions{DmaPermission::Write},
                                  .Reserved = {}};
}

// The backend alone: ownership, the identity address, the consistency of
// what it is asked to map, the attachment rule and its empty fault record.
void TestBackend(TestContext& runner) noexcept
{
    SpinLock synchronization;
    Asteria::TestSupport::TestObjectRegistry registry{synchronization};
    IdentityDomainBackend backend;
    IommuDomain first{registry, backend};
    IommuDomain second{registry, backend};
    ASTERIA_EXPECT(runner, "a claim takes 32 to 64 input bits and one owner", backend.IsReady(),
                   backend.Claim(first, 31U) == IommuBackendOwnershipError::UnsupportedConfiguration,
                   backend.Claim(first, 65U) == IommuBackendOwnershipError::UnsupportedConfiguration,
                   backend.Claim(first, 32U) == IommuBackendOwnershipError::None,
                   backend.Claim(second, 64U) == IommuBackendOwnershipError::AlreadyOwned,
                   backend.Release(second) == IommuBackendOwnershipError::InvalidOwner,
                   backend.Release(first) == IommuBackendOwnershipError::None,
                   backend.Claim(second, 64U) == IommuBackendOwnershipError::None,
                   backend.Release(second) == IommuBackendOwnershipError::None);
    ASTERIA_EXPECT(
        runner, "the device address is the physical one, whatever was asked for",
        backend.ChooseDeviceAddress(DmaAddress{0x0020'0000U}, PhysicalAddress{0x0123'4000U}).GetValue() == 0x0123'4000U,
        backend.ChooseDeviceAddress(DmaAddress{0U}, PhysicalAddress{0x1'0000'0000U}).GetValue() == 0x1'0000'0000U);
    ASTERIA_EXPECT(runner, "only identity mappings are programmed",
                   backend.Map(Request(0x5000U, 0x5000U)) == IommuBackendError::None,
                   backend.Map(Request(0x0020'0000U, 0x5000U)) == IommuBackendError::InvalidRequest,
                   backend.Unmap(Request(0x5000U, 0x5000U)) == IommuBackendError::None,
                   backend.Unmap(Request(0x5000U, 0x6000U)) == IommuBackendError::InvalidRequest);
    Endpoint trusted{true};
    Endpoint untrusted{false};
    const auto faults = backend.GetFaults();
    ASTERIA_EXPECT(runner, "only an endpoint granted trusted DMA attaches; nothing records a fault",
                   backend.Attach(untrusted) == IommuBackendError::UnsupportedConfiguration,
                   backend.Attach(trusted) == IommuBackendError::None,
                   backend.Detach(trusted) == IommuBackendError::None, faults.Count == 0U, faults.LastAddress == 0U);
}

[[nodiscard]] constexpr ResourceDomainLimits Limits() noexcept
{
    return ResourceDomainLimits{.CommittedMemoryPages = 0U,
                                .ResidentMemoryPages = 0U,
                                .KernelMetadataBytes = 1U << 20U,
                                .CapabilityHandles = 64U,
                                .IpcQueueBytes = 0U,
                                .Threads = 0U,
                                .ChildDomains = 0U};
}

// An IommuDomain of inputBits on the identity backend and one Frame of RAM
// at physicalStart, each registered.
class Fixture final
{
public:
    Fixture() noexcept : Registry{Synchronization}, Issuer{Registry}, Resource{Ledger}, Domain{Registry, Backend}
    {
    }

    [[nodiscard]] bool Initialize(const UInt64 physicalStart, const Foundation::UInt8 inputBits) noexcept
    {
        if (Resource.InitializeRoot(ResourceDomainId{71U}, Limits()).Error != ResourceDomainError::None)
        {
            return false;
        }
        const auto resource = Issuer.CreateObject(Resource);
        const FrameDescriptor descriptor{.PhysicalStart = PhysicalAddress{physicalStart},
                                         .Pages = PageCount{8U},
                                         .CachePolicy = MemoryCachePolicy::WriteBack,
                                         .Kind = FrameKind::Ram,
                                         .Reserved = {}};
        if (!resource.HasValue() || (FrameStorage.Initialize(ResourceDomainId{71U}, descriptor) != FrameError::None))
        {
            return false;
        }
        const auto frame = Issuer.CreateObject(FrameStorage);
        if (!frame.HasValue())
        {
            return false;
        }
        FrameIdentity = FrameId{Registry, frame.GetValue()};
        return Domain
                   .Initialize(ResourceDomainObjectId{Registry, resource.GetValue()}, Resource,
                               IommuDomainConfiguration{.InputAddressBits = inputBits, .Reserved = {}})
                   .Error == IommuDomainError::None;
    }

    [[nodiscard]] IommuMappingRequest Map(const UInt64 requested, const UInt64 offset, const UInt64 pages) noexcept
    {
        return IommuMappingRequest{.FrameStorage = &FrameStorage,
                                   .FrameIdentity = FrameIdentity,
                                   .DmaStart = DmaAddress{requested},
                                   .FrameOffsetPages = PageCount{offset},
                                   .Pages = PageCount{pages},
                                   .Permissions = DmaPermissions{DmaPermission::Read}.With(DmaPermission::Write),
                                   .Reserved = {}};
    }

    SpinLock Synchronization;
    Asteria::TestSupport::TestObjectRegistry Registry;
    RootCapabilityIssuer Issuer;
    ResourceDomainLedger Ledger;
    ResourceDomain Resource;
    IdentityDomainBackend Backend;
    IommuDomain Domain;
    BootFrame FrameStorage;
    FrameId FrameIdentity{};
};

// Through an IommuDomain: Map answers the physical address the driver must
// program and UnmapAt takes it; the domain's width bounds it, so a 32-bit
// domain never maps a Frame above 4 GiB; mappings of the same pages overlap;
// only a trusted endpoint attaches.
void TestDomain(TestContext& runner) noexcept
{
    Fixture low;
    const bool initialized = low.Initialize(0x0080'0000U, 32U);
    const auto mapped = low.Domain.Map(low.Map(0x0020'0000U, 3U, 2U));
    const auto overlapping = low.Domain.Map(low.Map(0x0040'0000U, 4U, 1U));
    const auto beside = low.Domain.Map(low.Map(0x0040'0000U, 5U, 1U));
    ASTERIA_EXPECT(runner, "Map answers the Frame's physical address and UnmapAt takes it", initialized,
                   mapped.HasValue(), mapped.HasValue() && (mapped.GetValue().DmaStart.GetValue() == 0x0080'3000U),
                   !overlapping.HasValue() && (overlapping.GetError().Error == IommuDomainError::DmaRangeOverlap),
                   beside.HasValue() && (beside.GetValue().DmaStart.GetValue() == 0x0080'5000U),
                   low.Domain.UnmapAt(DmaAddress{0x0020'0000U}).Error == IommuDomainError::InvalidMapping,
                   low.Domain.UnmapAt(DmaAddress{0x0080'3000U}).Error == IommuDomainError::None,
                   low.Domain.GetSnapshot().MappingCount == 1U);
    Endpoint untrusted{false};
    Endpoint trusted{true};
    const auto refused = low.Domain.Attach(untrusted);
    const auto attached = low.Domain.Attach(trusted);
    ASTERIA_EXPECT(runner, "an untrusted endpoint is refused without harm, a trusted one attaches",
                   !refused.HasValue() && (refused.GetError().Error == IommuDomainError::BackendRejected) &&
                       (refused.GetError().Backend == IommuBackendError::UnsupportedConfiguration),
                   attached.HasValue(), low.Domain.GetSnapshot().AttachmentCount == 1U,
                   low.Domain.Quiesce().Error == IommuDomainError::None);

    Fixture high;
    const bool highInitialized = high.Initialize(0x1'0000'0000U - 0x4000U, 32U);
    const auto below = high.Domain.Map(high.Map(0x0010'0000U, 0U, 4U));
    const auto across = high.Domain.Map(high.Map(0x0020'0000U, 4U, 1U));
    ASTERIA_EXPECT(runner, "a 32-bit domain maps pages below 4 GiB only, and a refusal pins nothing", highInitialized,
                   below.HasValue() && (below.GetValue().DmaStart.GetValue() == 0xFFFF'C000U),
                   !across.HasValue() && (across.GetError().Error == IommuDomainError::DmaRangeExceeded),
                   high.Domain.GetSnapshot().MappingCount == 1U, high.Domain.Quiesce().Error == IommuDomainError::None);

    Fixture wide;
    const bool wideInitialized = wide.Initialize(0x1'0000'0000U, 64U);
    const auto above = wide.Domain.Map(wide.Map(0x0010'0000U, 0U, 1U));
    ASTERIA_EXPECT(runner, "a 64-bit domain maps above 4 GiB at the physical address", wideInitialized,
                   above.HasValue() && (above.GetValue().DmaStart.GetValue() == 0x1'0000'0000U),
                   wide.Domain.Quiesce().Error == IommuDomainError::None);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"IdentityDomainBackendTests"};
    Asteria::Test::TestBackend(runner);
    Asteria::Test::TestDomain(runner);
    return runner.Finish();
}
