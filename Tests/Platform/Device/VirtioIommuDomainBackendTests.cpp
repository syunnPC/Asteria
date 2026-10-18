// SPDX-License-Identifier: Apache-2.0
#include "Platform/Device/VirtioIommuDomainBackend.hpp"

#include <cstdio>

#include <memory>

#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Platform/Device/VirtioIommuModel.hpp"
#include "Support/Random.hpp"
#include "Support/TestContext.hpp"

namespace Asteria::Test
{
namespace
{
using TestSupport::TestContext;
using TestSupport::VirtioFixture;
using TestSupport::Put;
using Platform::Device::VirtioTransportError;
using Hal::Device::IommuBackendError;
using Hal::Device::IommuBackendOwnershipError;
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;

void Ready(TestContext& context, VirtioFixture& fixture) noexcept
{
    ASTERIA_CHECK(context, fixture.Discover() == VirtioTransportError::None);
    ASTERIA_CHECK(context, fixture.Initialize() == VirtioTransportError::None);
    ASTERIA_EXPECT(context, "bypass is explicit and domain ownership waits for translation", !fixture.Backend.IsReady(),
                   fixture.Model.Config[36U] == 1U,
                   fixture.Backend.Claim(fixture.Owner, 32U) == IommuBackendOwnershipError::NotReady);
    ASTERIA_CHECK(context, fixture.Device.ClearBypass() == VirtioTransportError::None);
    ASTERIA_CHECK(context, fixture.Model.Config[36U] == 0U);
    ASTERIA_CHECK(context, fixture.Backend.Claim(fixture.Owner, 32U) == IommuBackendOwnershipError::None);
}

void TestWideDeviceNarrowRemapper(TestContext& context) noexcept
{
    auto storage = std::make_unique<VirtioFixture>();
    VirtioFixture& fixture = *storage;
    Ready(context, fixture);
    ASTERIA_EXPECT(context, "a 64-bit device domain is admitted on a narrower virtio input aperture",
                   fixture.Backend.Release(fixture.Owner) == IommuBackendOwnershipError::None,
                   fixture.Backend.Claim(fixture.Owner, 64U) == IommuBackendOwnershipError::None,
                   fixture.Backend.Map(VirtioFixture::Mapping()) == IommuBackendError::None,
                   fixture.Backend.Map(VirtioFixture::Mapping(0x1'0000'0000ULL)) == IommuBackendError::InvalidRequest);
}

void TestRoundTrips(TestContext& context) noexcept
{
    auto storage = std::make_unique<VirtioFixture>();
    VirtioFixture& fixture = *storage;
    Ready(context, fixture);
    ASTERIA_EXPECT(
        context, "ownership and input width are checked without requests", fixture.Backend.GetDomainId() == 7U,
        fixture.Backend.Claim(fixture.Owner, 32U) == IommuBackendOwnershipError::AlreadyOwned,
        fixture.OtherBackend.Claim(fixture.Owner, 32U) == IommuBackendOwnershipError::AlreadyOwned,
        fixture.OtherBackend.Claim(fixture.OtherOwner, 65U) == IommuBackendOwnershipError::UnsupportedConfiguration,
        fixture.Backend.Release(fixture.OtherOwner) == IommuBackendOwnershipError::InvalidOwner,
        fixture.Model.Requests == 0U);
    ASTERIA_CHECK(context, fixture.Backend.Map(VirtioFixture::Mapping()) == IommuBackendError::None);
    ASTERIA_CHECK(context, fixture.Backend.Unmap(VirtioFixture::Mapping()) == IommuBackendError::None);
    ASTERIA_CHECK(context, fixture.Model.Requests == 0U);
    ASTERIA_CHECK(context, fixture.Backend.Attach(fixture.Endpoint) == IommuBackendError::None);
    ASTERIA_CHECK(context, fixture.OtherBackend.Claim(fixture.OtherOwner, 32U) == IommuBackendOwnershipError::None);
    ASTERIA_CHECK(context, fixture.OtherBackend.GetDomainId() == 8U);
    ASTERIA_CHECK(context, fixture.OtherBackend.Attach(fixture.OtherEndpoint) == IommuBackendError::None);
    const auto mapping = VirtioFixture::Mapping(0x4000U, 2U, 1U);
    ASTERIA_CHECK(context, fixture.Backend.Map(mapping) == IommuBackendError::None);
    const auto& stored = fixture.Model.Domains[0U].Mappings[63U];
    ASTERIA_EXPECT(context, "MAP is an inclusive virtual extent, physical address and read-only permission",
                   stored.Present, stored.Start == 0x4000U, stored.End == 0x5FFFU, stored.Physical == 0x204000U,
                   stored.Flags == 1U, fixture.Model.MappingCount(8U) == 0U);
    ASTERIA_CHECK(context,
                  fixture.Backend.Unmap(VirtioFixture::Mapping()) == IommuBackendError::UnsupportedConfiguration);
    ASTERIA_CHECK(context, fixture.Backend.Detach(fixture.Endpoint) == IommuBackendError::None);
    ASTERIA_EXPECT(context, "last DETACH destroys device domain", !fixture.Model.Domains[0U].Exists,
                   fixture.Model.MappingCount(7U) == 0U);
    ASTERIA_CHECK(context, fixture.Backend.Attach(fixture.Endpoint) == IommuBackendError::None);
    ASTERIA_CHECK(context, fixture.Model.MappingCount(7U) == 1U);
    ASTERIA_CHECK(context, fixture.Backend.Unmap(mapping) == IommuBackendError::None);
    ASTERIA_CHECK(context, fixture.Model.MappingCount(7U) == 0U);
    ASTERIA_CHECK(context, fixture.Backend.Release(fixture.Owner) == IommuBackendOwnershipError::None);
    ASTERIA_EXPECT(context, "Release removes this domain and preserves another domain",
                   !fixture.Model.Domains[0U].Exists, fixture.Model.Domains[1U].Exists,
                   fixture.Model.Endpoints[0x10U] == 0xFFFF'FFFFU, fixture.Model.Endpoints[0x18U] == 8U);
    ASTERIA_CHECK(context, fixture.OtherBackend.Release(fixture.OtherOwner) == IommuBackendOwnershipError::None);
    ASTERIA_CHECK(context, fixture.Backend.Claim(fixture.Owner, 32U) == IommuBackendOwnershipError::None);
    ASTERIA_CHECK(context, fixture.Backend.GetDomainId() == 7U);
    ASTERIA_CHECK(context, fixture.Model.Violations == 0U);
}

void TestReservedRanges(TestContext& context) noexcept
{
    for (UInt32 attached{0U}; attached < 2U; ++attached)
    {
        auto storage = std::make_unique<VirtioFixture>();
        VirtioFixture& fixture = *storage;
        Ready(context, fixture);
        if (attached != 0U)
        {
            ASTERIA_CHECK(context, fixture.Backend.Attach(fixture.Endpoint) == IommuBackendError::None);
        }
        const UInt32 requests = fixture.Model.Requests;
        const UInt16 available = fixture.Model.Available(0U);
        const UInt64 starts[5U]{0xFEE00000U, 0xFEEFF000U, 0xFEDFF000U, 0xFEEFF000U, 0x900000U};
        const UInt64 pages[5U]{1U, 1U, 2U, 2U, 1U};
        for (Size index{0U}; index < 5U; ++index)
        {
            ASTERIA_EXPECT(context, "any intersection with a platform reserved extent is a no-write refusal",
                           fixture.Backend.Map(VirtioFixture::Mapping(starts[index], pages[index])) ==
                               IommuBackendError::UnsupportedConfiguration,
                           fixture.Model.Requests == requests, fixture.Model.Available(0U) == available);
        }
        ASTERIA_CHECK(context, fixture.Backend.Map(VirtioFixture::Mapping(0xFEDFF000U)) == IommuBackendError::None);
        ASTERIA_CHECK(context, fixture.Backend.Map(VirtioFixture::Mapping(0xFEF00000U)) == IommuBackendError::None);
        ASTERIA_CHECK(context, fixture.Backend.Map(VirtioFixture::Mapping(0x901000U)) == IommuBackendError::None);
        ASTERIA_CHECK(context, fixture.Backend.Map(VirtioFixture::Mapping(0x8FF000U)) == IommuBackendError::None);
        ASTERIA_CHECK(context, fixture.Backend.Release(fixture.Owner) == IommuBackendOwnershipError::None);
    }
    auto storage = std::make_unique<VirtioFixture>();
    VirtioFixture& fixture = *storage;
    fixture.ReservedRanges[0U].End = fixture.ReservedRanges[0U].Start - 1U;
    ASTERIA_CHECK(context, fixture.Discover() == VirtioTransportError::None);
    ASTERIA_EXPECT(context, "malformed platform extents fail before initializing hardware",
                   fixture.Initialize() == VirtioTransportError::InvalidArguments, fixture.Model.ResetWrites == 0U);
}

void TestUnattachedMappings(TestContext& context) noexcept
{
    for (UInt32 order{0U}; order < 2U; ++order)
    {
        auto storage = std::make_unique<VirtioFixture>();
        VirtioFixture& fixture = *storage;
        Ready(context, fixture);
        const auto mapping = VirtioFixture::Mapping(0x4000U, 2U);
        if (order == 0U)
        {
            ASTERIA_EXPECT(context, "Map before first Attach only records metadata",
                           fixture.Backend.Map(mapping) == IommuBackendError::None, fixture.Model.Requests == 0U,
                           !fixture.Model.Domains[0U].Exists);
            ASTERIA_CHECK(context, fixture.Backend.Attach(fixture.Endpoint) == IommuBackendError::None);
            ASTERIA_CHECK(context, fixture.Model.MappingCount(7U) == 1U);
        }
        else
        {
            ASTERIA_CHECK(context, fixture.Backend.Attach(fixture.Endpoint) == IommuBackendError::None);
            ASTERIA_CHECK(context, fixture.Backend.Map(mapping) == IommuBackendError::None);
        }
        ASTERIA_CHECK(context, fixture.Backend.Detach(fixture.Endpoint) == IommuBackendError::None);
        const UInt32 requests = fixture.Model.Requests;
        ASTERIA_EXPECT(context, "whole-extent validation applies to metadata-only Unmap",
                       fixture.Backend.Unmap(VirtioFixture::Mapping()) == IommuBackendError::UnsupportedConfiguration,
                       fixture.Model.Requests == requests);
        ASTERIA_EXPECT(context, "Unmap after last Detach succeeds without a request",
                       fixture.Backend.Unmap(mapping) == IommuBackendError::None, fixture.Model.Requests == requests,
                       fixture.Backend.IsReady());
        ASTERIA_CHECK(context, fixture.Backend.Attach(fixture.Endpoint) == IommuBackendError::None);
        ASTERIA_EXPECT(context, "removed metadata is not replayed", fixture.Model.MappingCount(7U) == 0U,
                       fixture.Model.Requests == requests + 1U);
        ASTERIA_CHECK(context, fixture.Backend.Release(fixture.Owner) == IommuBackendOwnershipError::None);
    }
}

class KernelDomainFixture final
{
public:
    VirtioFixture Native;
    Kernel::Resource::ResourceDomainLedger Ledger;
    Kernel::Resource::ResourceDomain Resource{Ledger};
    Kernel::Memory::BootFrame Frame;
    Kernel::Object::RootCapabilityIssuer Issuer{Native.Registry};
};

class OneStorageParker final : public Kernel::Device::IIommuStorageParker
{
public:
    [[nodiscard]] bool CanPark(const Size count) noexcept override
    {
        return count <= 1U && !Pin.IsValid();
    }
    void Park(const Kernel::Device::IommuParkedMapping& mapping) noexcept override
    {
        Pin.~ObjectPin();
        ::new (static_cast<void*>(&Pin))
            Kernel::Object::ObjectPin{static_cast<Kernel::Object::ObjectPin&&>(*mapping.Pin)};
        Mapping = mapping;
    }
    void ConfirmQuiet() noexcept
    {
        Kernel::Device::IommuDomain::ReleaseParkedMapping(Pin, *Mapping.FrameStorage, *Mapping.Registry,
                                                          Mapping.FrameObject, Mapping.PhysicalStart, Mapping.Pages,
                                                          Mapping.PolicyPermissions);
    }
    Kernel::Object::ObjectPin Pin;
    Kernel::Device::IommuParkedMapping Mapping{};
};

void TestKernelUnattachedMappings(TestContext& context, const bool translationOnly = false) noexcept
{
    for (UInt32 order{0U}; order < 2U; ++order)
    {
        auto storage = std::make_unique<KernelDomainFixture>();
        KernelDomainFixture& fixture = *storage;
        VirtioFixture& native = fixture.Native;
        ASTERIA_CHECK(context, native.Discover() == VirtioTransportError::None);
        ASTERIA_CHECK(context,
                      native.Initialize(translationOnly ? Platform::Device::VirtioIommuCompletionPolicy::TranslationOnly
                                                        : Platform::Device::VirtioIommuCompletionPolicy::DrainsDma) ==
                          VirtioTransportError::None);
        ASTERIA_CHECK(context, native.Device.ClearBypass() == VirtioTransportError::None);
        ASTERIA_CHECK(context,
                      fixture.Resource
                              .InitializeRoot(Kernel::Object::ResourceDomainId{91U}, {.CommittedMemoryPages = 0U,
                                                                                      .ResidentMemoryPages = 0U,
                                                                                      .KernelMetadataBytes = 1U << 20U,
                                                                                      .CapabilityHandles = 64U,
                                                                                      .IpcQueueBytes = 0U,
                                                                                      .Threads = 0U,
                                                                                      .ChildDomains = 0U})
                              .Error == Kernel::Resource::ResourceDomainError::None);
        const auto resource = fixture.Issuer.CreateObject(fixture.Resource);
        ASTERIA_CHECK(context, resource.HasValue());
        ASTERIA_CHECK(context, fixture.Frame.Initialize(Kernel::Object::ResourceDomainId{91U},
                                                        {.PhysicalStart = Kernel::Memory::PhysicalAddress{0x200000U},
                                                         .Pages = Kernel::Memory::PageCount{2U},
                                                         .CachePolicy = Kernel::Memory::MemoryCachePolicy::WriteBack,
                                                         .Kind = Kernel::Memory::FrameKind::Ram,
                                                         .Reserved = {}}) == Kernel::Memory::FrameError::None);
        const auto frame = fixture.Issuer.CreateObject(fixture.Frame);
        ASTERIA_CHECK(context, frame.HasValue());
        ASTERIA_CHECK(
            context, native.Owner
                             .Initialize(Kernel::Resource::ResourceDomainObjectId{native.Registry, resource.GetValue()},
                                         fixture.Resource, {.InputAddressBits = 32U, .Reserved = {}})
                             .Error == Kernel::Device::IommuDomainError::None);
        Kernel::Device::IommuAttachmentToken attachment{};
        if (order == 1U)
        {
            const auto attached = native.Owner.Attach(native.Endpoint);
            ASTERIA_CHECK(context, attached.HasValue());
            attachment = attached.GetValue().Attachment;
        }
        const auto mapped =
            native.Owner.Map({.FrameStorage = &fixture.Frame,
                              .FrameIdentity = Kernel::Memory::FrameId{native.Registry, frame.GetValue()},
                              .DmaStart = Kernel::Device::DmaAddress{0x4000U},
                              .FrameOffsetPages = Kernel::Memory::PageCount{0U},
                              .Pages = Kernel::Memory::PageCount{2U},
                              .Permissions = Kernel::Device::DmaPermissions{Kernel::Device::DmaPermission::Read},
                              .Reserved = {}});
        ASTERIA_CHECK(context, mapped.HasValue());
        if (order == 0U)
        {
            ASTERIA_CHECK(context, native.Model.Requests == 0U);
            const auto attached = native.Owner.Attach(native.Endpoint);
            ASTERIA_CHECK(context, attached.HasValue());
            attachment = attached.GetValue().Attachment;
            ASTERIA_CHECK(context, native.Model.MappingCount(7U) == 1U);
        }
        if (translationOnly)
        {
            ASTERIA_EXPECT(
                context, "translation-only Unmap retires the token and retains storage in either attach order",
                !native.Backend.DrainsDma(), native.Owner.RequiresDmaQuiescence(),
                native.Owner.Unmap(mapped.GetValue().Mapping).Error == Kernel::Device::IommuDomainError::None,
                native.Model.MappingCount(7U) == 0U, native.Owner.GetSnapshot().MappingCount == 1U,
                native.Owner.Unmap(mapped.GetValue().Mapping).Error == Kernel::Device::IommuDomainError::StaleMapping,
                native.Owner.Detach(attachment).Error == Kernel::Device::IommuDomainError::ParkingRefused,
                native.Owner.Quiesce().Error == Kernel::Device::IommuDomainError::ParkingRefused);
            ASTERIA_CHECK(context,
                          fixture.Issuer.DestroyObject(frame.GetValue()) == Kernel::Object::ObjectRegistryError::Ok);
            OneStorageParker parker;
            ASTERIA_EXPECT(context, "backend release hands the retired pin to the quiet-confirmation owner",
                           !fixture.Issuer.CollectDestroyedObject(frame.GetValue()).HasValue(),
                           native.Owner.Quiesce(&parker).Error == Kernel::Device::IommuDomainError::None,
                           parker.Pin.IsValid(), native.Model.MappingCount(7U) == 0U,
                           !fixture.Issuer.CollectDestroyedObject(frame.GetValue()).HasValue());
            parker.ConfirmQuiet();
            ASTERIA_CHECK(context, fixture.Issuer.CollectDestroyedObject(frame.GetValue()).HasValue());
            ASTERIA_CHECK(context,
                          fixture.Issuer.DestroyObject(resource.GetValue()) == Kernel::Object::ObjectRegistryError::Ok);
            ASTERIA_CHECK(context, fixture.Issuer.CollectDestroyedObject(resource.GetValue()).HasValue());
            continue;
        }
        ASTERIA_CHECK(context, native.Owner.Detach(attachment).Error == Kernel::Device::IommuDomainError::None);
        const UInt32 requests = native.Model.Requests;
        ASTERIA_EXPECT(context, "kernel Unmap after last Detach preserves a healthy domain in either order",
                       native.Owner.Unmap(mapped.GetValue().Mapping).Error == Kernel::Device::IommuDomainError::None,
                       native.Owner.GetSnapshot().State == Kernel::Device::IommuDomainState::Ready,
                       native.Owner.GetSnapshot().MappingCount == 0U, native.Model.Requests == requests);
        ASTERIA_CHECK(context, native.Owner.Quiesce().Error == Kernel::Device::IommuDomainError::None);
        ASTERIA_CHECK(context,
                      fixture.Issuer.DestroyObject(frame.GetValue()) == Kernel::Object::ObjectRegistryError::Ok);
        ASTERIA_CHECK(context, fixture.Issuer.CollectDestroyedObject(frame.GetValue()).HasValue());
        ASTERIA_CHECK(context,
                      fixture.Issuer.DestroyObject(resource.GetValue()) == Kernel::Object::ObjectRegistryError::Ok);
        ASTERIA_CHECK(context, fixture.Issuer.CollectDestroyedObject(resource.GetValue()).HasValue());
    }
}

void TestFailedInitializationRecovery(TestContext& context) noexcept
{
    for (UInt32 cleanupFails{0U}; cleanupFails < 2U; ++cleanupFails)
    {
        auto storage = std::make_unique<VirtioFixture>();
        VirtioFixture& fixture = *storage;
        ASTERIA_CHECK(context, fixture.Discover() == VirtioTransportError::None);
        fixture.Model.FailEventNotify = true;
        fixture.Model.FailCleanupReset = cleanupFails != 0U;
        ASTERIA_EXPECT(context, "late initialization failure attempts reset after posting events",
                       fixture.Initialize() == VirtioTransportError::HardwareFailure,
                       fixture.Model.PostedEventsAtFailure == 8U, fixture.Model.ResetWrites == 2U,
                       fixture.Model.Queues[0U].Enabled == cleanupFails,
                       fixture.Model.Queues[1U].Enabled == cleanupFails, !fixture.Backend.IsReady(),
                       fixture.Device.ClearBypass() == VirtioTransportError::NotReady);
        fixture.Model.StuckReset = false;
        fixture.Model.FailEventNotify = false;
        ASTERIA_EXPECT(context, "uninitialized recovery resets queues and verifies bypass",
                       fixture.Device.QuiesceForRecovery() == VirtioTransportError::None,
                       fixture.Model.Queues[0U].Enabled == 0U, fixture.Model.Queues[1U].Enabled == 0U,
                       fixture.Model.Config[36U] == 0U);
        ASTERIA_CHECK(context, fixture.Initialize() == VirtioTransportError::None);
        ASTERIA_EXPECT(context, "reinitialization requires the platform to clear bypass again",
                       !fixture.Backend.IsReady(), fixture.Device.ClearBypass() == VirtioTransportError::None,
                       fixture.Backend.IsReady());
    }
}

void TestAttachErrorsAndRelease(TestContext& context) noexcept
{
    for (UInt8 status{1U}; status <= 9U; ++status)
    {
        auto storage = std::make_unique<VirtioFixture>();
        VirtioFixture& fixture = *storage;
        Ready(context, fixture);
        // Simulate an endpoint in a previous device domain unknown to this backend.
        fixture.Model.Domains[1U].Exists = true;
        fixture.Model.Domains[1U].EndpointCount = 1U;
        fixture.Model.Endpoints[0x10U] = 8U;
        fixture.Model.AttachStatus = status;
        ASTERIA_CHECK(context, fixture.Backend.Attach(fixture.Endpoint) != IommuBackendError::None);
        ASTERIA_EXPECT(context, "device error can follow destruction of the previous domain",
                       fixture.Model.Reattachments == 1U, fixture.Model.DestroyedDomains == 1U,
                       !fixture.Model.Domains[1U].Exists, fixture.Model.Endpoints[0x10U] == 0xFFFF'FFFFU);
        fixture.Model.AttachStatus = 0xFFU;
        const UInt32 requests = fixture.Model.Requests;
        ASTERIA_EXPECT(context, "Release cleans up every possibly attached endpoint after any device status",
                       fixture.Backend.Release(fixture.Owner) == IommuBackendOwnershipError::None,
                       fixture.Model.Requests == requests + 2U, !fixture.Model.Domains[0U].Exists,
                       fixture.Model.Endpoints[0x10U] == 0xFFFF'FFFFU, fixture.Model.Violations == 0U);
    }
    auto storage = std::make_unique<VirtioFixture>();
    VirtioFixture& fixture = *storage;
    Ready(context, fixture);
    ASTERIA_CHECK(context, fixture.Backend.Attach(fixture.Endpoint) == IommuBackendError::None);
    ASTERIA_CHECK(context, fixture.Backend.Map(VirtioFixture::Mapping()) == IommuBackendError::None);
    ASTERIA_EXPECT(context, "Release re-ATTACH destroys and recreates a sole-endpoint domain before DETACH",
                   fixture.Backend.Release(fixture.Owner) == IommuBackendOwnershipError::None,
                   fixture.Model.Reattachments == 1U, fixture.Model.DestroyedDomains == 2U,
                   fixture.Model.MappingCount(7U) == 0U, !fixture.Model.Domains[0U].Exists,
                   fixture.Model.Endpoints[0x10U] == 0xFFFF'FFFFU);
}

void TestStatusesAndTimeout(TestContext& context) noexcept
{
    const IommuBackendError errors[10U]{IommuBackendError::None,
                                        IommuBackendError::HardwareFailure,
                                        IommuBackendError::HardwareFailure,
                                        IommuBackendError::HardwareFailure,
                                        IommuBackendError::InvalidRequest,
                                        IommuBackendError::InvalidRequest,
                                        IommuBackendError::InvalidRequest,
                                        IommuBackendError::HardwareFailure,
                                        IommuBackendError::HardwareFailure,
                                        IommuBackendError::CorruptState};
    for (UInt8 status{0U}; status < 10U; ++status)
    {
        auto storage = std::make_unique<VirtioFixture>();
        VirtioFixture& fixture = *storage;
        Ready(context, fixture);
        ASTERIA_CHECK(context, fixture.Backend.Attach(fixture.Endpoint) == IommuBackendError::None);
        fixture.Model.ForcedStatus = status;
        ASTERIA_CHECK(context, fixture.Backend.Map(VirtioFixture::Mapping()) == errors[status]);
    }
    for (UInt32 mutation{0U}; mutation < 2U; ++mutation)
    {
        auto storage = std::make_unique<VirtioFixture>();
        VirtioFixture& fixture = *storage;
        Ready(context, fixture);
        fixture.Model.UnwrittenStatus = mutation == 0U;
        fixture.Model.ZeroUsedLength = mutation == 1U;
        ASTERIA_CHECK(context, fixture.Backend.Attach(fixture.Endpoint) == IommuBackendError::CorruptState);
    }
    auto storage = std::make_unique<VirtioFixture>();
    VirtioFixture& fixture = *storage;
    Ready(context, fixture);
    fixture.Model.NeverComplete = true;
    const UInt32 reads = fixture.Model.StatusReads;
    ASTERIA_EXPECT(context, "bounded request timeout retains owner and all exposed buffers",
                   fixture.Backend.Attach(fixture.Endpoint) == IommuBackendError::CommittedStateUnknown,
                   fixture.Model.StatusReads - reads == 10U, // Event drain, Submit, eight completion polls.
                   !fixture.Backend.IsReady(), fixture.Backend.GetDomainId() == 7U,
                   fixture.Backend.Release(fixture.Owner) == IommuBackendOwnershipError::HardwareFailure,
                   fixture.Backend.Claim(fixture.Owner, 32U) == IommuBackendOwnershipError::AlreadyOwned);
    fixture.Model.StuckReset = true;
    ASTERIA_CHECK(context, fixture.Device.QuiesceForRecovery() == VirtioTransportError::TimedOut);
    ASTERIA_CHECK(context, fixture.Backend.Release(fixture.Owner) == IommuBackendOwnershipError::HardwareFailure);
    fixture.Model.StuckReset = false;
    ASTERIA_CHECK(context, fixture.Device.QuiesceForRecovery() == VirtioTransportError::None);
    ASTERIA_CHECK(context, fixture.Initialize() == VirtioTransportError::NotReady);
    ASTERIA_CHECK(context, fixture.Backend.Release(fixture.Owner) == IommuBackendOwnershipError::None);
    ASTERIA_CHECK(context, fixture.Initialize() == VirtioTransportError::None);
    ASTERIA_CHECK(context, fixture.Device.ClearBypass() == VirtioTransportError::None);
}

void TestFaults(TestContext& context) noexcept
{
    auto storage = std::make_unique<VirtioFixture>();
    VirtioFixture& fixture = *storage;
    Ready(context, fixture);
    ASTERIA_CHECK(context, fixture.Backend.Attach(fixture.Endpoint) == IommuBackendError::None);
    ASTERIA_CHECK(context, fixture.OtherBackend.Claim(fixture.OtherOwner, 32U) == IommuBackendOwnershipError::None);
    ASTERIA_CHECK(context, fixture.OtherBackend.Attach(fixture.OtherEndpoint) == IommuBackendError::None);
    for (UInt32 round{0U}; round < 4U; ++round)
    {
        for (UInt32 event{0U}; event < 8U; ++event)
        {
            ASTERIA_CHECK(context, fixture.Model.Emit(event % 2U == 0U ? 0x10U : 0x18U, 2U));
        }
        ASTERIA_CHECK(context, !fixture.Model.Emit(0x10U, 2U));
        const auto faults = fixture.Backend.GetFaults();
        ASTERIA_EXPECT(context, "events are attributed across domains and every consumed buffer is reposted",
                       faults.Count == (round + 1U) * 4U, faults.LastSource == 0x10U, faults.LastReason == 2U,
                       faults.LastAddress == 0x4000U, fixture.OtherBackend.GetFaults().Count == (round + 1U) * 4U,
                       fixture.Model.Available(1U) == fixture.Model.Queues[1U].Consumed + 8U);
    }
    ASTERIA_CHECK(context, fixture.Model.Emit(0x10U, 2U, 1U));
    ASTERIA_CHECK(context, fixture.Model.Emit(0x1'0010U, 2U));
    ASTERIA_CHECK(context, fixture.Model.Emit(0x10U, 0xFFU));
    const auto faults = fixture.Backend.GetFaults();
    ASTERIA_EXPECT(context, "reserved reports and oversized endpoints ignored; unknown reason normalized",
                   faults.Count == 17U, faults.LastReason == 0U, fixture.Model.Violations == 0U);
    ASTERIA_CHECK(context, fixture.Backend.Release(fixture.Owner) == IommuBackendOwnershipError::None);
    const auto cumulative = fixture.Device.GetFaults();
    ASTERIA_EXPECT(context, "device evidence retains attributed fault address after domain release",
                   cumulative.Count == 33U, cumulative.LastAddress == 0x4000U, cumulative.LastSource == 0x10U);
}

void TestConfigurationAndBoundaries(TestContext& context) noexcept
{
    for (UInt32 mutation{0U}; mutation < 5U; ++mutation)
    {
        auto storage = std::make_unique<VirtioFixture>();
        VirtioFixture& fixture = *storage;
        if (mutation == 0U)
        {
            Put(fixture.Model.Config, 0U, 8U, 8192U);
        }
        if (mutation == 1U)
        {
            Put(fixture.Model.Config, 24U, 4U, 23U);
        }
        if (mutation == 2U)
        {
            Put(fixture.Model.Config, 8U, 8U, 0x1'0000'0000ULL);
        }
        if (mutation == 3U)
        {
            fixture.Model.UnstableConfig = true;
        }
        if (mutation == 4U)
        {
            fixture.Model.IgnoreBypassWrite = true;
        }
        ASTERIA_CHECK(context, fixture.Discover() == VirtioTransportError::None);
        const VirtioTransportError initialized = fixture.Initialize();
        ASTERIA_CHECK(context, (initialized == VirtioTransportError::None) == (mutation == 4U));
        if (mutation == 4U)
        {
            ASTERIA_CHECK(context, fixture.Device.ClearBypass() == VirtioTransportError::HardwareFailure);
            ASTERIA_CHECK(context, !fixture.Backend.IsReady());
        }
    }
    auto storage = std::make_unique<VirtioFixture>();
    VirtioFixture& fixture = *storage;
    Ready(context, fixture);
    ASTERIA_CHECK(context, fixture.Backend.Attach(fixture.Endpoint) == IommuBackendError::None);
    const UInt32 requests = fixture.Model.Requests;
    ASTERIA_CHECK(context, fixture.Backend.Map(VirtioFixture::Mapping(0x4001U)) == IommuBackendError::InvalidRequest);
    ASTERIA_CHECK(context,
                  fixture.Backend.Map(VirtioFixture::Mapping(0x4000U, 0U)) == IommuBackendError::InvalidRequest);
    ASTERIA_CHECK(context,
                  fixture.Backend.Map(VirtioFixture::Mapping(0x1'0000'0000ULL)) == IommuBackendError::InvalidRequest);
    ASTERIA_CHECK(context, fixture.Backend.Map(VirtioFixture::Mapping(0x4000U, ~UInt64{0U})) ==
                               IommuBackendError::InvalidRequest);
    TestSupport::VirtioEndpoint outside{0x1'0010U};
    ASTERIA_CHECK(context, fixture.Backend.Attach(outside) == IommuBackendError::UnsupportedConfiguration);
    TestSupport::VirtioEndpoint self{0x08U};
    ASTERIA_CHECK(context, fixture.Backend.Attach(self) == IommuBackendError::UnsupportedConfiguration);
    ASTERIA_CHECK(context, fixture.Model.Requests == requests);
    auto overflow = VirtioFixture::Mapping(0x4000U, 2U);
    overflow.PhysicalStart = Kernel::Memory::PhysicalAddress{0xFFFF'FFFF'FFFF'F000ULL};
    ASTERIA_CHECK(context, fixture.Backend.Map(overflow) == IommuBackendError::InvalidRequest);
    ASTERIA_CHECK(context, fixture.Backend.Map(VirtioFixture::Mapping(0xFFFF'F000U)) == IommuBackendError::None);
    ASTERIA_CHECK(context, fixture.Backend.Unmap(VirtioFixture::Mapping(0xFFFF'F000U)) == IommuBackendError::None);
}

void TestCapacityAndRecovery(TestContext& context) noexcept
{
    auto storage = std::make_unique<VirtioFixture>();
    VirtioFixture& fixture = *storage;
    Ready(context, fixture);
    ASTERIA_CHECK(context, fixture.Backend.Attach(fixture.Endpoint) == IommuBackendError::None);
    for (Size index{0U}; index < Platform::Device::VirtioIommuDevice::MaximumMappings; ++index)
    {
        ASTERIA_CHECK(context,
                      fixture.Backend.Map(VirtioFixture::Mapping(0x10000U + index * 4096U)) == IommuBackendError::None);
    }
    const UInt32 requests = fixture.Model.Requests;
    ASTERIA_EXPECT(context, "mapping capacity is an ordinary no-write failure",
                   fixture.Backend.Map(VirtioFixture::Mapping(0x80000U)) == IommuBackendError::CapacityExceeded,
                   fixture.Model.Requests == requests, fixture.Model.MappingCount(7U) == 64U);
    fixture.Model.ForcedStatus = 3U;
    ASTERIA_CHECK(context, fixture.Backend.Release(fixture.Owner) == IommuBackendOwnershipError::HardwareFailure);
    ASTERIA_EXPECT(context, "failed Release retains domain id and ownership", fixture.Backend.GetDomainId() == 7U,
                   fixture.Backend.Claim(fixture.Owner, 32U) == IommuBackendOwnershipError::AlreadyOwned,
                   fixture.Model.MappingCount(7U) == 64U);
    fixture.Model.ForcedStatus = 0xFFU;
    ASTERIA_CHECK(context, fixture.Backend.Release(fixture.Owner) == IommuBackendOwnershipError::None);
    ASTERIA_CHECK(context, fixture.Model.MappingCount(7U) == 0U);
    ASTERIA_CHECK(context, fixture.Model.Violations == 0U);
}

void TestDomainRanges(TestContext& context) noexcept
{
    auto storage = std::make_unique<VirtioFixture>();
    VirtioFixture& fixture = *storage;
    Put(fixture.Model.Config, 24U, 4U, 0xFFFF'FFFFU);
    Put(fixture.Model.Config, 28U, 4U, 0xFFFF'FFFFU);
    Ready(context, fixture);
    ASTERIA_CHECK(context, fixture.Backend.GetDomainId() == 0xFFFF'FFFFU);
    ASTERIA_CHECK(context, fixture.OtherBackend.Claim(fixture.OtherOwner, 32U) ==
                               IommuBackendOwnershipError::UnsupportedConfiguration);
    ASTERIA_CHECK(context, fixture.Model.Requests == 0U);
    ASTERIA_CHECK(context, fixture.Backend.Release(fixture.Owner) == IommuBackendOwnershipError::None);
    ASTERIA_CHECK(context, fixture.OtherBackend.Claim(fixture.OtherOwner, 32U) == IommuBackendOwnershipError::None);
}

void TestFullWidthAndPolicy(TestContext& context) noexcept
{
    auto storage = std::make_unique<VirtioFixture>();
    VirtioFixture& fixture = *storage;
    Put(fixture.Model.Config, 16U, 8U, ~UInt64{0U});
    ASTERIA_CHECK(context, fixture.Discover() == VirtioTransportError::None);
    const Platform::Device::VirtioDmaPage requestQueue[3U]{fixture.Page(0U), fixture.Page(1U), fixture.Page(2U)};
    const Platform::Device::VirtioDmaPage eventQueue[3U]{fixture.Page(3U), fixture.Page(4U), fixture.Page(5U)};
    ASTERIA_EXPECT(context, "translation-only completion is advertised honestly to the storage owner",
                   fixture.Device.Initialize(requestQueue, eventQueue, fixture.Page(6U), fixture.Page(7U), 8U,
                                             Platform::Device::VirtioIommuCompletionPolicy::TranslationOnly) ==
                       VirtioTransportError::None,
                   !fixture.Backend.DrainsDma());
    ASTERIA_CHECK(context, fixture.Device.ClearBypass() == VirtioTransportError::None);
    ASTERIA_CHECK(context, fixture.Backend.Claim(fixture.Owner, 64U) == IommuBackendOwnershipError::None);
    ASTERIA_CHECK(context, fixture.Backend.Attach(fixture.Endpoint) == IommuBackendError::None);
    auto request = VirtioFixture::Mapping(0xFFFF'FFFF'FFFF'F000ULL);
    request.PhysicalStart = Kernel::Memory::PhysicalAddress{0x200000U};
    ASTERIA_CHECK(context, fixture.Backend.Map(request) == IommuBackendError::None);
    ASTERIA_CHECK(context, fixture.Backend.Unmap(request) == IommuBackendError::None);
    ASTERIA_CHECK(context,
                  fixture.Backend.Map(VirtioFixture::Mapping(0x4000U, 1U, 0U)) == IommuBackendError::InvalidRequest);
    ASTERIA_CHECK(context, fixture.Backend.Release(fixture.Owner) == IommuBackendOwnershipError::None);
    fixture.Model.Features &= ~3ULL;
    ASTERIA_CHECK(context, fixture.Device.QuiesceForRecovery() == VirtioTransportError::None);
    ASTERIA_CHECK(context, fixture.Initialize() == VirtioTransportError::None);
    const auto configuration = fixture.Device.GetConfiguration();
    ASTERIA_EXPECT(context, "absent optional ranges use full-width specification defaults",
                   configuration.InputStart == 0U, configuration.InputEnd == ~UInt64{0U},
                   configuration.DomainStart == 0U, configuration.DomainEnd == 0xFFFF'FFFFU);
}

void TestModelSequences(TestContext& context) noexcept
{
    const auto seeds = TestSupport::GetSeedRange(0xA570U, 8U);
    for (std::uint64_t seed = seeds.First; seed < seeds.First + seeds.Count; ++seed)
    {
        auto storage = std::make_unique<VirtioFixture>();
        VirtioFixture& fixture = *storage;
        Ready(context, fixture);
        ASTERIA_CHECK(context, fixture.Backend.Attach(fixture.Endpoint) == IommuBackendError::None);
        TestSupport::Random random{seed};
        bool present[32U]{};
        UInt32 flags[32U]{};
        Size expectedCount{0U};
        for (UInt32 step{0U}; step < 512U; ++step)
        {
            const Size index = static_cast<Size>(random.Below(32U));
            const UInt32 operation = static_cast<UInt32>(random.Below(2U));
            const UInt8 permission = static_cast<UInt8>(random.Below(3U) + 1U);
            const auto request = VirtioFixture::Mapping(0x10000U + index * 4096U, 1U, permission);
            const IommuBackendError expected =
                (operation == 0U && present[index]) ? IommuBackendError::InvalidRequest : IommuBackendError::None;
            const IommuBackendError result =
                operation == 0U ? fixture.Backend.Map(request) : fixture.Backend.Unmap(request);
            if (result != expected)
            {
                static_cast<void>(std::fprintf(stderr, "seed=%llu step=%u operation=%u\n",
                                               static_cast<unsigned long long>(seed), step, operation));
            }
            ASTERIA_CHECK(context, result == expected);
            if (result == IommuBackendError::None)
            {
                if (operation == 0U)
                {
                    present[index] = true;
                    flags[index] = permission;
                    ++expectedCount;
                }
                else if (present[index])
                {
                    present[index] = false;
                    --expectedCount;
                }
            }
            ASTERIA_CHECK(context, fixture.Model.MappingCount(7U) == expectedCount);
            for (const auto& mapping : fixture.Model.Domains[0U].Mappings)
            {
                if (mapping.Present)
                {
                    const Size slot = static_cast<Size>((mapping.Start - 0x10000U) / 4096U);
                    ASTERIA_EXPECT(context, "model state equals logical mapping set", slot < 32U, present[slot],
                                   mapping.End == mapping.Start + 4095U, mapping.Physical == 0x200000U + mapping.Start,
                                   mapping.Flags == flags[slot]);
                }
            }
        }
        ASTERIA_CHECK(context, fixture.Backend.Release(fixture.Owner) == IommuBackendOwnershipError::None);
        ASTERIA_CHECK(context, !fixture.Model.Domains[0U].Exists);
        ASTERIA_CHECK(context, fixture.Model.MappingCount(7U) == 0U);
        ASTERIA_CHECK(context, fixture.Model.Violations == 0U);
    }
}

void TestRingWrap(TestContext& context) noexcept
{
    auto storage = std::make_unique<VirtioFixture>();
    VirtioFixture& fixture = *storage;
    Ready(context, fixture);
    ASTERIA_CHECK(context, fixture.Backend.Attach(fixture.Endpoint) == IommuBackendError::None);
    for (UInt32 index{0U}; index < 65540U; ++index)
    {
        ASTERIA_CHECK(context, fixture.Backend.Unmap(VirtioFixture::Mapping()) == IommuBackendError::None);
    }
    ASTERIA_CHECK(context, fixture.Model.Queues[0U].Used == 5U);
    ASTERIA_CHECK(context, fixture.Model.Violations == 0U);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    Asteria::TestSupport::TestContext context{"VirtioIommuDomainBackendTests"};
    Asteria::Test::TestWideDeviceNarrowRemapper(context);
    Asteria::Test::TestRoundTrips(context);
    Asteria::Test::TestReservedRanges(context);
    Asteria::Test::TestUnattachedMappings(context);
    Asteria::Test::TestKernelUnattachedMappings(context);
    Asteria::Test::TestKernelUnattachedMappings(context, true);
    Asteria::Test::TestFailedInitializationRecovery(context);
    Asteria::Test::TestAttachErrorsAndRelease(context);
    Asteria::Test::TestStatusesAndTimeout(context);
    Asteria::Test::TestFaults(context);
    Asteria::Test::TestConfigurationAndBoundaries(context);
    Asteria::Test::TestCapacityAndRecovery(context);
    Asteria::Test::TestDomainRanges(context);
    Asteria::Test::TestFullWidthAndPolicy(context);
    Asteria::Test::TestModelSequences(context);
    Asteria::Test::TestRingWrap(context);
    return context.Finish();
}
