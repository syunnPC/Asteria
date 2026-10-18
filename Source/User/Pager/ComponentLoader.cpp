// SPDX-License-Identifier: Apache-2.0

#include "User/Pager/ComponentLoader.hpp"
#include "Abi/ObjectCreationLimits.hpp"
#include "User/Toolkit/SessionComponent.hpp"

#include "User/Pager/DeviceInventory.hpp"

#include "Abi/ComponentBootstrap.hpp"
#include "User/Client/ClientAbi.hpp"
#include "User/Component/BlockFrame.hpp"
#include "User/Component/DisplayFrame.hpp"
#include "User/Component/InputFrame.hpp"
#include "User/Session/SessionFrame.hpp"
#include "User/Component/DriverFrame.hpp"
#include "User/Component/FsFrame.hpp"
#include "User/Component/EchoFrame.hpp"
#include "User/Component/NetFrame.hpp"
#include "User/Component/SocketFrame.hpp"
#include "User/Directory/DirectoryAbi.hpp"
#include "User/Pager/PagerWords.hpp"
#include "User/Identity/Budget.hpp"
#include "User/Identity/SeatFrame.hpp"
#include "Inventory/ReportText.hpp"

namespace Asteria::User::Pager
{
namespace
{
constexpr Foundation::UInt64 PageBytes{4096U};
// FaultResolve and MemoryMap permission bits: Read 1, Write 2, Execute 4,
// User 8.
constexpr Foundation::UInt8 ReadPermission{0x01U};
constexpr Foundation::UInt8 WritePermission{0x02U};
constexpr Foundation::UInt8 ExecutePermission{0x04U};
constexpr Foundation::UInt8 UserPermission{0x08U};
constexpr Foundation::UInt8 StagingPermissions{ReadPermission | WritePermission | UserPermission};
// Rights and ObjectType of the report Endpoint's copy: Invoke only.
constexpr Foundation::UInt16 InvokeRight{0x0002U};
constexpr Foundation::UInt8 EndpointType{static_cast<Foundation::UInt8>(Abi::ObjectTypeEndpoint)};
// A service's copies: Read on its Endpoint, Wait on its WaitSet.
constexpr Foundation::UInt16 ReadRight{0x0004U};
constexpr Foundation::UInt16 WaitRight{0x0020U};
constexpr Foundation::UInt16 ManageRight{0x0800U};
constexpr Foundation::UInt8 WaitSetType{static_cast<Foundation::UInt8>(Abi::ObjectTypeWaitSet)};
// A client's send capability on its own Endpoint, for the reply attachments
// it copies into its requests: Invoke and Duplicate.
constexpr Foundation::UInt16 ReplyRights{InvokeRight | 0x0100U};
// A filesystem service's block-reply Endpoint: Read, Invoke and Duplicate.
constexpr Foundation::UInt16 BlockReplyRights{static_cast<Foundation::UInt16>(ReplyRights | ReadRight)};

// The first manifest: what a component's own domain may hold besides the pages of its image, of its
// stack and start block and of an interpreter's guest. A service and a client own one Endpoint,
// receiving in their space; an account incarnation creates one itself, and the platform's window's
// pages, or the pages of its own Frames it maps in a window of its own layout (a NIC driver's DMA
// pages, a network stack's packet pages); every component has one thread and creates no domain.
struct ComponentQuota
{
    Foundation::UInt64 Behavior;
    Foundation::UInt64 Endpoints;
    Foundation::UInt64 Threads;
    bool Window;
    Foundation::UInt64 OwnPages;
};

constexpr ComponentQuota ComponentQuotas[]{
    {.Behavior = Abi::ComponentBehaviorAuth,
     .Endpoints = Identity::AuthEndpoints,
     .Threads = 1U,
     .Window = false,
     .OwnPages = Identity::AuthRuntimePages + Identity::AuthFixturePages},
    {.Behavior = Abi::ComponentBehaviorLogin,
     .Endpoints = Identity::LoginEndpoints,
     .Threads = 1U,
     .Window = false,
     .OwnPages = Identity::LoginRuntimePages},
    {.Behavior = Abi::ComponentBehaviorToolkit,
     .Endpoints = 1U,
     .Threads = 1U,
     .Window = false,
     .OwnPages = Toolkit::SessionWorkspacePages + Abi::ComponentUiFontPages},
    // Supervisor plus its faulting child; the private channel owns a queue.
    {.Behavior = Abi::ComponentBehaviorFaultSupervisor,
     .Endpoints = 1U,
     .Threads = 2U,
     .Window = false,
     .OwnPages = 0U},
    {.Behavior = Abi::ComponentBehaviorCompositor,
     .Endpoints = 6U,
     .Threads = 1U,
     .Window = false,
     .OwnPages = Abi::ComponentCompositorPages + Abi::ComponentUiFontPages},
    {.Behavior = Abi::ComponentBehaviorPatternClient,
     .Endpoints = 2U,
     .Threads = 1U,
     .Window = false,
     .OwnPages = Abi::ComponentPatternPages},
    {.Behavior = Abi::ComponentBehaviorSessionProbe,
     .Endpoints = 1U,
     .Threads = 1U,
     .Window = false,
     .OwnPages = Abi::ComponentSessionProbePages},
    {.Behavior = Abi::ComponentBehaviorXhci,
     .Endpoints = 1U,
     .Threads = 1U,
     .Window = false,
     .OwnPages = Abi::XhciDmaPages + Abi::XhciStatePages},
    {.Behavior = Abi::ComponentBehaviorPs2, .Endpoints = 1U, .Threads = 1U, .Window = false, .OwnPages = 0U},
    // The broker's main Endpoint, three sources and two clients.
    {.Behavior = Abi::ComponentBehaviorInput,
     .Endpoints = 1U + Component::InputMaximumSources + Component::InputMaximumClients,
     .Threads = 1U,
     .Window = false,
     .OwnPages = 0U},
    {.Behavior = Abi::ComponentBehaviorInputClient, .Endpoints = 1U, .Threads = 1U, .Window = false, .OwnPages = 0U},
    {.Behavior = Abi::ComponentBehaviorCrash, .Endpoints = 0U, .Threads = 1U, .Window = false, .OwnPages = 0U},
    {.Behavior = Abi::ComponentBehaviorHang, .Endpoints = 0U, .Threads = 1U, .Window = false, .OwnPages = 0U},
    {.Behavior = Abi::ComponentBehaviorReport, .Endpoints = 0U, .Threads = 1U, .Window = false, .OwnPages = 0U},
    {.Behavior = Abi::ComponentBehaviorEcho, .Endpoints = 1U, .Threads = 1U, .Window = false, .OwnPages = 0U},
    {.Behavior = Abi::ComponentBehaviorDirectory, .Endpoints = 1U, .Threads = 1U, .Window = false, .OwnPages = 0U},
    {.Behavior = Abi::ComponentBehaviorClient, .Endpoints = 1U, .Threads = 1U, .Window = false, .OwnPages = 0U},
    {.Behavior = Abi::ComponentBehaviorInterpret, .Endpoints = 0U, .Threads = 1U, .Window = false, .OwnPages = 0U},
    {.Behavior = Abi::ComponentBehaviorAccount, .Endpoints = 1U, .Threads = 1U, .Window = true, .OwnPages = 0U},
    // A driver maps its device's registers, which charge no pages, and the window's pages for its
    // DMA.
    {.Behavior = Abi::ComponentBehaviorDriver, .Endpoints = 0U, .Threads = 1U, .Window = true, .OwnPages = 0U},
    // A driver service also owns its service Endpoint.
    {.Behavior = Abi::ComponentBehaviorDriverService, .Endpoints = 1U, .Threads = 1U, .Window = true, .OwnPages = 0U},
    // So does the AHCI driver, whose window holds its command page.
    {.Behavior = Abi::ComponentBehaviorBlockDriver,
     .Endpoints = 1U,
     .Threads = 1U,
     .Window = true,
     .OwnPages = Abi::AhciBouncePages},
    // service, block replies and the separate manager face; metadata and client
    // Frames still use Documentation/Architecture/FilesystemProtocol.md's window.
    {.Behavior = Abi::ComponentBehaviorFilesystem, .Endpoints = 3U, .Threads = 1U, .Window = true, .OwnPages = 0U},
    // The display broker owns its console Endpoint and maps the scanout, whose device Frame and
    // MemoryObject charge no pages.
    {.Behavior = Abi::ComponentBehaviorDisplay, .Endpoints = 4U, .Threads = 1U, .Window = false, .OwnPages = 0U},
    // The NIC driver owns its service Endpoint and its descriptor and packet pages; its window's
    // registers and the page it maps a client's Frame at charge none.
    {.Behavior = Abi::ComponentBehaviorNicDriver,
     .Endpoints = 1U,
     .Threads = 1U,
     .Window = false,
     .OwnPages = 1U + Abi::NicReceiveBufferPages + Abi::NicTransmitBufferPages},
    // The network stack owns its service Endpoint and its driver-reply Endpoint, and its receive
    // and transmit pages.
    {.Behavior = Abi::ComponentBehaviorNetwork,
     .Endpoints = 2U,
     .Threads = 1U,
     .Window = false,
     .OwnPages = Abi::NetworkWindowPages},
};
// A component's copies of its own domain's and space's capabilities
// Manage and Inspect, without Code, Duplicate, Transfer or Derive,
// so it cannot hand its domain on; Map only.
constexpr Foundation::UInt16 AccountRights{0x0801U};
constexpr Foundation::UInt8 ResourceDomainType{static_cast<Foundation::UInt8>(Abi::ObjectTypeResourceDomain)};
constexpr Foundation::UInt16 MapRight{0x0010U};
constexpr Foundation::UInt8 AddressSpaceType{static_cast<Foundation::UInt8>(Abi::ObjectTypeAddressSpace)};
// The display broker's copy of the scanout: Inspect, Read, Write and Map, a
// DeviceRegion's, as the Device Manager grants a driver its registers.
constexpr Foundation::UInt16 ScanoutRights{0x001DU};
constexpr Foundation::UInt8 DeviceRegionType{15U};

void IdentityAdmissionRefusal(ObjectCalls& calls, const PagerBootstrap& bootstrap, const ComponentRecord& component,
                              const Foundation::UInt64 stage, const Abi::SystemCallError error) noexcept
{
    if (component.Behavior != Abi::ComponentBehaviorAuth && component.Behavior != Abi::ComponentBehaviorLogin)
    {
        return;
    }
    // Report the refused public admission stage, never fixture contents, a credential or a reduced
    // hashing profile.
    char line[144U]{};
    Inventory::ReportText text{line, sizeof(line)};
    text.Text(component.Behavior == Abi::ComponentBehaviorAuth ? "# auth: loader refusal stage="
                                                               : "# login: loader refusal stage=");
    text.Decimal(stage);
    text.Text(" syscall-error=");
    text.Decimal(static_cast<Foundation::UInt64>(error));
    text.Character('\n');
    static_cast<void>(calls.GetClient().DebugWrite(bootstrap.Diagnostic, AddressOf(line), text.Bytes()));
}

[[nodiscard]] constexpr Foundation::UInt8 FinalPermissions(const Loader::ExecutableSegment& segment) noexcept
{
    return static_cast<Foundation::UInt8>(ReadPermission | UserPermission | (segment.Writable ? WritePermission : 0U) |
                                          (segment.Executable ? ExecutePermission : 0U));
}

// The device a driver behaviour drives (an Abi::PagerDevice* index), or ComponentNoDevice. A
// service's sealed selector and wait tag, where its behaviour is selected.
constexpr void SelectService(const bool selected, const Foundation::UInt64 sealed, const Foundation::UInt64 tag,
                             Foundation::UInt64& selector, Foundation::UInt64& waitTag) noexcept
{
    selector = selected ? sealed : selector;
    waitTag = selected ? tag : waitTag;
}

[[nodiscard]] constexpr Foundation::UInt8 DeviceOf(const Foundation::UInt64 behavior) noexcept
{
    if (behavior == Abi::ComponentBehaviorBlockDriver)
    {
        return static_cast<Foundation::UInt8>(Abi::PagerDeviceAhci);
    }
    if (behavior == Abi::ComponentBehaviorNicDriver)
    {
        return static_cast<Foundation::UInt8>(Abi::PagerDeviceNic);
    }
    return ((behavior == Abi::ComponentBehaviorDriver) || (behavior == Abi::ComponentBehaviorDriverService))
               ? static_cast<Foundation::UInt8>(Abi::PagerDeviceEdu)
               : ComponentNoDevice;
}
} // Namespace

void ComponentLoader::Configure(ObjectCalls& calls, const PagerBootstrap& bootstrap, const PagerThreadLaunch& launch,
                                DeviceManagerRole& devices) noexcept
{
    m_calls = &calls;
    m_bootstrap = &bootstrap;
    m_launch = &launch;
    m_devices = &devices;
}

bool ComponentLoader::Load(ComponentRecord& component, const ComponentBuild& build,
                           Abi::SystemCallError& systemCall) noexcept
{
    const LoadedImage& image = *build.Image;
    component.Image = &image;
    component.SharedImage = nullptr;
    component.Tag = build.Tag;
    component.Behavior = build.Behavior;
    component.Foreign = build.Foreign;
    component.Account = {};
    component.AccountParent = build.AccountParent;
    component.FaultChannel = build.FaultChannel;
    component.Space = {};
    component.Capabilities = {};
    component.Domain = {};
    component.Thread = {};
    component.Service = {};
    component.WaitSet = {};
    component.MappingCount = 0U;
    component.ExitCode = 0U;
    component.Device = ComponentNoDevice;
    if (!CleanWindow(systemCall) || !CreateAccount(component, image, build.ServiceFlags, systemCall) ||
        !CreateDomain(component, build.ServiceFlags, systemCall))
    {
        return false;
    }
    bool staged{false};
    if (!AcquireImage(component, staged, systemCall))
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < image.Layout.SegmentCount; ++index)
    {
        const Loader::ExecutableSegment& segment = image.Layout.Segments[index];
        if (segment.Writable)
        {
            if (!LoadSegment(component, segment, image.Bytes, systemCall))
            {
                IdentityAdmissionRefusal(*m_calls, *m_bootstrap, component, 3U, systemCall);
                return false;
            }
            continue;
        }
        const ComponentMapping& shared = component.SharedImage->Segments[index];
        if (!m_calls->Map(component.Space, shared, segment.VirtualStart, FinalPermissions(segment), systemCall))
        {
            return false;
        }
        component.Mappings[component.MappingCount++] = shared;
        if (!ReportSharedMapping(component, index, staged && (segment.FileBytes != 0U), systemCall))
        {
            return false;
        }
    }
    if ((build.Behavior == Abi::ComponentBehaviorInterpret) && !LoadForeign(component, systemCall))
    {
        return false;
    }
    if (!LoadData(component, build, systemCall))
    {
        IdentityAdmissionRefusal(*m_calls, *m_bootstrap, component, 4U, systemCall);
        return false;
    }
    if (!build.Deferred && !StartThread(component, systemCall))
    {
        IdentityAdmissionRefusal(*m_calls, *m_bootstrap, component, 5U, systemCall);
        return false;
    }
    return true;
}

// A child of the grant within the first manifest: its limits are caps, and the grant's remaining
// quota bounds every charge besides.
bool ComponentLoader::CreateAccount(ComponentRecord& component, const LoadedImage& image,
                                    const Foundation::UInt64 serviceFlags, Abi::SystemCallError& systemCall) noexcept
{
    component.Account = {};
    if ((m_bootstrap->ManagementFeatures & Abi::PagerManagementResourceDomains) == 0U)
    {
        return true;
    }
    const ComponentQuota* quota{nullptr};
    for (const ComponentQuota& candidate : ComponentQuotas)
    {
        quota = (candidate.Behavior == component.Behavior) ? &candidate : quota;
    }
    if (quota == nullptr)
    {
        return false;
    }
    Foundation::UInt64 pages{m_launch->Component.StackPages + 1U};
    for (Foundation::Size index{0U}; index < image.Layout.SegmentCount; ++index)
    {
        if (image.Layout.Segments[index].Writable)
        {
            pages += image.Layout.Segments[index].Pages;
        }
    }
    if (component.Behavior == Abi::ComponentBehaviorInterpret)
    {
        pages += (static_cast<Foundation::UInt64>(component.Foreign.Size) + PageBytes - 1U) / PageBytes;
    }
    pages += (quota->Window ? m_launch->Component.WindowPages : 0U) + quota->OwnPages;
    if (component.Behavior == Abi::ComponentBehaviorPatternClient && (serviceFlags >> 32U) == 2U)
    {
        pages -= Abi::ComponentPatternPages - Abi::ComponentPatternSecondaryPages;
    }
    if (component.Behavior == Abi::ComponentBehaviorPatternClient &&
        ((serviceFlags >> 32U) == 3U || (serviceFlags >> 32U) == 5U))
    {
        pages = pages - Abi::ComponentPatternPages + Abi::ComponentPatternInputPages;
    }
    const Foundation::UInt64 outputPages =
        (static_cast<Foundation::UInt64>(m_bootstrap->Scanout.Width) * m_bootstrap->Scanout.Height * 4U + PageBytes -
         1U) /
        PageBytes;
    if (component.Behavior == Abi::ComponentBehaviorCompositor &&
        (serviceFlags & Abi::ComponentSessionTestSmallOutputFlag) == 0U)
    {
        pages = pages - Abi::ComponentCompositorPages + outputPages;
    }
    if (component.Behavior == Abi::ComponentBehaviorPatternClient && (serviceFlags >> 32U) != 3U &&
        (serviceFlags >> 32U) != 5U)
    {
        pages = pages - Abi::ComponentPatternPages + 4U * outputPages;
    }
    if (component.Behavior == Abi::ComponentBehaviorToolkit)
    {
        pages += 2U * outputPages;
    }
    if (component.Behavior == Abi::ComponentBehaviorXhci)
    {
        // The USB host group's DMA and state pages, or one xHCI's captured
        // scratchpad need beside its fixed pages.
        pages -= Abi::XhciDmaPages + Abi::XhciStatePages;
        pages += ((serviceFlags & Abi::UsbHostGroupFlag) != 0U)
                     ? Abi::UsbHostDmaPages + Abi::UsbHostStatePages
                     : XhciDmaPageQuota(serviceFlags & Abi::XhciControllerIndexMask) + Abi::XhciStatePages;
    }
    if (component.Behavior == Abi::ComponentBehaviorFaultSupervisor)
    {
        pages *= 2U;
    }
    m_calls->ClearRequest();
    Foundation::UInt8* const request = m_calls->GetRequest();
    StoreWord(request + Abi::ObjectCreateDomainCommittedPagesOffset, pages);
    StoreWord(request + Abi::ObjectCreateDomainResidentPagesOffset, pages);
    StoreWord(request + Abi::ObjectCreateDomainQueueBytesOffset, quota->Endpoints * DefaultEndpointQueuedBytes);
    StoreWord(request + Abi::ObjectCreateDomainThreadsOffset, quota->Threads);
    if (component.Behavior == Abi::ComponentBehaviorFaultSupervisor)
    {
        StoreWord(request + Abi::ObjectCreateDomainChildrenOffset, 1U);
    }
    Foundation::Size size{Abi::ObjectCreateResourceDomainRequestSizeBytes};
    if (component.Behavior == Abi::ComponentBehaviorAccount)
    {
        // The account incarnation cannot create a WaitSet or a FaultChannel, and has one Endpoint
        // unit.
        size = Abi::ObjectCreateResourceDomainExtendedRequestSizeBytes;
        for (Foundation::Size type{0U}; type < Abi::LimitedObjectCount; ++type)
        {
            const Foundation::UInt16 limit = type == static_cast<Foundation::Size>(Abi::LimitedObject::Endpoint) ? 1U
                                             : (type == static_cast<Foundation::Size>(Abi::LimitedObject::WaitSet) ||
                                                type == static_cast<Foundation::Size>(Abi::LimitedObject::FaultChannel))
                                                 ? 0U
                                                 : Abi::ObjectPoolCapacities[type];
            request[Abi::ObjectCreateDomainObjectLimitsOffset + 2U * type] = static_cast<Foundation::UInt8>(limit);
            request[Abi::ObjectCreateDomainObjectLimitsOffset + 2U * type + 1U] =
                static_cast<Foundation::UInt8>(limit >> 8U);
        }
    }
    const bool largePattern = component.Behavior == Abi::ComponentBehaviorPatternClient &&
                              (serviceFlags >> 32U) == 4U && outputPages > Session::SessionMaximumPoolPages / 2U;
    if (component.Behavior == Abi::ComponentBehaviorCompositor || largePattern)
    {
        // Root grants 1 until the CM owns RD_s.
        size = Abi::ObjectCreateResourceDomainArenaRequestSizeBytes;
        for (Foundation::Size type{0U}; type < Abi::LimitedObjectCount; ++type)
        {
            const Foundation::UInt16 limit =
                type == static_cast<Foundation::Size>(Abi::LimitedObject::Endpoint) ? (largePattern ? 2U : 6U)
                : type == static_cast<Foundation::Size>(Abi::LimitedObject::Frame)
                    ? (largePattern ? 4U * Session::SessionMaximumPoolFrames + 8U : 24U)
                : type == static_cast<Foundation::Size>(Abi::LimitedObject::MemoryObject) ? (largePattern ? 8U : 16U)
                : type == static_cast<Foundation::Size>(Abi::LimitedObject::FaultChannel) ? 0U
                                                                                          : 1U;
            request[Abi::ObjectCreateDomainObjectLimitsOffset + type * 2U] = static_cast<Foundation::UInt8>(limit);
        }
        request[Abi::ObjectCreateDomainLargeArenaLimitOffset] = 1U;
    }
    if (component.Behavior == Abi::ComponentBehaviorAuth || component.Behavior == Abi::ComponentBehaviorLogin ||
        component.Behavior == Abi::ComponentBehaviorToolkit)
    {
        // Auth/login and toolkit components require explicit
        // resident budgets, no Code, child-domain, channel or large-arena authority. Ordinary
        // page-table arenas suffice.
        const bool toolkit = component.Behavior == Abi::ComponentBehaviorToolkit;
        const bool auth = component.Behavior == Abi::ComponentBehaviorAuth;
        size = Abi::ObjectCreateResourceDomainExtendedRequestSizeBytes;
        for (Foundation::Size type{0U}; type < Abi::LimitedObjectCount; ++type)
        {
            const Foundation::UInt16 limit = type == static_cast<Foundation::Size>(Abi::LimitedObject::Endpoint)
                                                 ? static_cast<Foundation::UInt16>(toolkit ? 1U
                                                                                   : auth  ? Identity::AuthEndpoints
                                                                                           : Identity::LoginEndpoints)
                                             : type == static_cast<Foundation::Size>(Abi::LimitedObject::Frame)
                                                 ? static_cast<Foundation::UInt16>(toolkit ? Toolkit::SessionFrameLimit
                                                                                   : auth  ? Identity::AuthFrames
                                                                                           : Identity::LoginFrames)
                                             : type == static_cast<Foundation::Size>(Abi::LimitedObject::MemoryObject)
                                                 ? static_cast<Foundation::UInt16>(toolkit ? Toolkit::SessionMemoryLimit
                                                                                   : auth  ? Identity::AuthMemories
                                                                                           : Identity::LoginMemories)
                                             : type == static_cast<Foundation::Size>(Abi::LimitedObject::FaultChannel)
                                                 ? 0U
                                                 : 1U;
            request[Abi::ObjectCreateDomainObjectLimitsOffset + 2U * type] = static_cast<Foundation::UInt8>(limit);
            request[Abi::ObjectCreateDomainObjectLimitsOffset + 2U * type + 1U] =
                static_cast<Foundation::UInt8>(limit >> 8U);
        }
    }
    const auto parent = component.AccountParent.Selector != 0U ? component.AccountParent : m_bootstrap->Management;
    return m_calls->Create(parent, Abi::ObjectTypeResourceDomain, size, component.Account, systemCall);
}

System::CapabilityHandle ComponentLoader::AuthorityOf(const ComponentRecord& component) const noexcept
{
    return (component.Account.Selector != 0U) ? component.Account : m_bootstrap->Management;
}

// A fresh AddressSpace and CapabilitySpace, bound by a ProtectionDomain whose faults reach this
// pager under tag.
bool ComponentLoader::CreateDomain(ComponentRecord& component, const Foundation::UInt64 serviceFlags,
                                   Abi::SystemCallError& systemCall) noexcept
{
    m_calls->ClearRequest();
    const System::CapabilityHandle authority = AuthorityOf(component);
    const auto pages = (static_cast<Foundation::UInt64>(m_bootstrap->Scanout.Width) * m_bootstrap->Scanout.Height * 4U +
                        PageBytes - 1U) /
                       PageBytes;
    if (component.Behavior == Abi::ComponentBehaviorCompositor ||
        (component.Behavior == Abi::ComponentBehaviorPatternClient && (serviceFlags >> 32U) == 4U &&
         pages > Session::SessionMaximumPoolPages / 2U))
    {
        m_calls->GetRequest()[0U] = Abi::ObjectCreateAddressSpaceFlagLargeArena;
    }
    if (!m_calls->Create(authority, Abi::ObjectTypeAddressSpace, Abi::ObjectCreateAddressSpaceRequestSizeBytes,
                         component.Space, systemCall))
    {
        return false;
    }
    m_calls->ClearRequest();
    if (!m_calls->Create(authority, Abi::ObjectTypeCapabilitySpace, Abi::ObjectCreateCapabilitySpaceRequestSizeBytes,
                         component.Capabilities, systemCall))
    {
        return false;
    }
    Foundation::UInt8* const request = m_calls->GetRequest();
    StoreHandle(request + Abi::ObjectCreateDomainAddressSpaceOffset, component.Space);
    StoreHandle(request + Abi::ObjectCreateDomainCapabilitySpaceOffset, component.Capabilities);
    StoreWord(request + Abi::ObjectCreateDomainSupervisionTagOffset, component.Tag);
    Foundation::Size size{Abi::ObjectCreateProtectionDomainRequestSizeBytes};
    if (component.FaultChannel.Selector != 0U)
    {
        StoreHandle(request + Abi::ObjectCreateDomainFaultChannelOffset, component.FaultChannel);
        size = Abi::ObjectCreateProtectionDomainChannelRequestSizeBytes;
    }
    StoreWord(request + (size == Abi::ObjectCreateProtectionDomainRequestSizeBytes
                             ? Abi::ObjectCreateDomainComponentKindOffset
                             : Abi::ObjectCreateDomainChannelComponentKindOffset),
              component.Behavior);
    return m_calls->Create(authority, Abi::ObjectTypeProtectionDomain, size, component.Domain, systemCall);
}

// Image records are keyed by the module bytes in the retained read-only loader slice. A new version
// needs a new record and generation. A failed stage is never retried over the same record; its
// objects safely stay on the undrained grant.
bool ComponentLoader::AcquireImage(ComponentRecord& component, bool& staged, Abi::SystemCallError& systemCall) noexcept
{
    const LoadedImage& image = *component.Image;
    SharedImageRecord* empty{nullptr};
    for (SharedImageRecord& record : m_images)
    {
        if ((record.Bytes == image.Bytes) && (record.Size == image.Size))
        {
            component.SharedImage = &record;
            break;
        }
        if ((record.Bytes == nullptr) && (empty == nullptr))
        {
            empty = &record;
        }
    }
    staged = component.SharedImage == nullptr;
    if (staged)
    {
        if (empty == nullptr)
        {
            systemCall = Abi::SystemCallError::ResourceExhausted;
            return false;
        }
        empty->Bytes = image.Bytes;
        empty->Size = image.Size;
        empty->Generation = ++m_imageGeneration;
        component.SharedImage = empty;
        for (Foundation::Size index{0U}; index < image.Layout.SegmentCount; ++index)
        {
            const Loader::ExecutableSegment& segment = image.Layout.Segments[index];
            if (!segment.Writable && !StageSharedSegment(empty->Segments[index], segment, image.Bytes, systemCall))
            {
                return false;
            }
        }
        empty->Ready = true;
    }
    if (!component.SharedImage->Ready)
    {
        systemCall = Abi::SystemCallError::InvalidConfiguration;
        return false;
    }
    return true;
}

bool ComponentLoader::MapWindow(const ComponentMapping& mapping, Abi::SystemCallError& systemCall) noexcept
{
    if (!CleanWindow(systemCall) ||
        !m_calls->Map(m_bootstrap->OwnSpace, mapping, m_bootstrap->WindowStart, StagingPermissions, systemCall))
    {
        return false;
    }
    m_stagingPages = mapping.Pages;
    return true;
}

bool ComponentLoader::CleanWindow(Abi::SystemCallError& systemCall) noexcept
{
    if (m_stagingPages == 0U)
    {
        return true;
    }
    if (!Check(m_calls->GetClient().UnmapMemory(m_bootstrap->OwnSpace, m_bootstrap->WindowStart, m_stagingPages),
               systemCall))
    {
        return false;
    }
    m_stagingPages = 0U;
    return true;
}

bool ComponentLoader::AttenuateMap(System::CapabilityHandle& handle, Abi::SystemCallError& systemCall) noexcept
{
    const Abi::SystemCallResponse derived =
        m_calls->GetClient().DeriveCapability(handle, Abi::CapabilityRightMap, ~Foundation::UInt64{0U});
    if (!Check(derived, systemCall) || !m_calls->Close(handle, systemCall))
    {
        return false;
    }
    handle = System::CapabilityHandle{derived.Value0, derived.Value1};
    return true;
}

bool ComponentLoader::StageSharedSegment(ComponentMapping& mapping, const Loader::ExecutableSegment& segment,
                                         const Foundation::UInt8* const source,
                                         Abi::SystemCallError& systemCall) noexcept
{
    const System::CapabilityHandle authority = m_bootstrap->Management;
    ComponentMapping staged{
        .Frame = {}, .Memory = {}, .VirtualStart = segment.VirtualStart, .Pages = segment.Pages, .Shared = true};
    if (!m_calls->CreateMemory(authority, Abi::ObjectTypeFrame, segment.Pages, 0U, staged.Frame, systemCall))
    {
        return false;
    }
    if (segment.FileBytes != 0U)
    {
        if (!m_calls->CreateMemory(authority, Abi::ObjectTypeMemoryObject, segment.Pages, StagingPermissions,
                                   staged.Memory, systemCall) ||
            !MapWindow(staged, systemCall))
        {
            return false;
        }
        volatile Foundation::UInt8* const window = reinterpret_cast<volatile Foundation::UInt8*>(
            static_cast<Foundation::UIntPointer>(m_bootstrap->WindowStart + segment.PageOffset));
        for (Foundation::UInt64 index{0U}; index < segment.FileBytes; ++index)
        {
            window[index] = source[segment.FileOffset + index];
        }
        if (segment.Executable && (m_launch->Component.FlushCode != nullptr))
        {
            m_launch->Component.FlushCode(m_bootstrap->WindowStart, segment.Pages * PageBytes);
        }
        // A successful MemoryUnmap has completed the shootdown and released
        // the writable mapping's Frame policy reservation, on both ISAs.
        // No executable object is created before this synchronous boundary.
        if (!CleanWindow(systemCall) || !m_calls->Destroy(staged.Memory, Abi::ObjectTypeMemoryObject, systemCall))
        {
            return false;
        }
        staged.Memory = {};
    }
    if (!m_calls->CreateMemory(authority, Abi::ObjectTypeMemoryObject, segment.Pages, FinalPermissions(segment),
                               staged.Memory, systemCall) ||
        !AttenuateMap(staged.Frame, systemCall) || !AttenuateMap(staged.Memory, systemCall))
    {
        return false;
    }
    mapping = staged;
    return true;
}

// Private writable segments are zero-filled. An interpreter's foreign
// bytes use this path too: staged and sealed read-only, charged to the
// interpreter, never executable.
bool ComponentLoader::LoadSegment(ComponentRecord& component, const Loader::ExecutableSegment& segment,
                                  const Foundation::UInt8* const source, Abi::SystemCallError& systemCall) noexcept
{
    if (segment.Executable || (segment.Writable && (segment.FileBytes != 0U)))
    {
        systemCall = Abi::SystemCallError::InvalidArguments;
        return false;
    }
    const System::CapabilityHandle authority = AuthorityOf(component);
    ComponentMapping& mapping = component.Mappings[component.MappingCount];
    mapping = ComponentMapping{
        .Frame = {}, .Memory = {}, .VirtualStart = segment.VirtualStart, .Pages = segment.Pages, .Shared = false};
    if (!m_calls->CreateMemory(authority, Abi::ObjectTypeFrame, segment.Pages, 0U, mapping.Frame, systemCall))
    {
        return false;
    }
    ++component.MappingCount;
    if (segment.FileBytes != 0U)
    {
        if (!m_calls->CreateMemory(authority, Abi::ObjectTypeMemoryObject, segment.Pages, StagingPermissions,
                                   mapping.Memory, systemCall) ||
            !MapWindow(mapping, systemCall))
        {
            return false;
        }
        volatile Foundation::UInt8* const window = reinterpret_cast<volatile Foundation::UInt8*>(
            static_cast<Foundation::UIntPointer>(m_bootstrap->WindowStart + segment.PageOffset));
        for (Foundation::UInt64 index{0U}; index < segment.FileBytes; ++index)
        {
            window[index] = source[segment.FileOffset + index];
        }
        if (!CleanWindow(systemCall) || !m_calls->Destroy(mapping.Memory, Abi::ObjectTypeMemoryObject, systemCall))
        {
            return false;
        }
        mapping.Memory = {};
    }
    const Foundation::UInt8 permissions = FinalPermissions(segment);
    return m_calls->CreateMemory(authority, Abi::ObjectTypeMemoryObject, segment.Pages, permissions, mapping.Memory,
                                 systemCall) &&
           m_calls->Map(component.Space, mapping, segment.VirtualStart, permissions, systemCall);
}

// Facts about each shared mapping: image generation, segment, incarnation,
// the full Frame/MemoryObject handles, and whether this launch staged it.
// The observer compares identities across crash/restart and echo restart.
bool ComponentLoader::ReportSharedMapping(const ComponentRecord& component, const Foundation::Size segment,
                                          const bool staged, Abi::SystemCallError& systemCall) noexcept
{
    const ComponentMapping& mapping = component.SharedImage->Segments[segment];
    const Foundation::UInt64 words[]{component.SharedImage->Generation,
                                     segment,
                                     component.Tag,
                                     mapping.Frame.Selector,
                                     mapping.Frame.Authenticator,
                                     mapping.Memory.Selector,
                                     mapping.Memory.Authenticator,
                                     staged ? 1U : 0U};
    char line[200]{"# shared image:"};
    Foundation::Size length{15U};
    for (const Foundation::UInt64 word : words)
    {
        line[length++] = ' ';
        char digits[20]{};
        Foundation::Size count{0U};
        Foundation::UInt64 left = word;
        do
        {
            digits[count++] = static_cast<char>('0' + (left % 10U));
            left /= 10U;
        } while (left != 0U);
        while (count != 0U)
        {
            line[length++] = digits[--count];
        }
    }
    line[length++] = '\n';
    return Check(m_calls->GetClient().DebugWrite(m_bootstrap->Diagnostic, AddressOf(line), length), systemCall);
}

// The stack and, right above it, the start block: the incarnation, a copy
// of the report Endpoint a task may only send on or a service's Endpoint and
// WaitSet, the ticks a crashing incarnation spins, and what the incarnation
// does; a client's handles besides. The block's other words stay zero, as
// the fresh Frame arrives.
bool ComponentLoader::LoadData(ComponentRecord& component, const ComponentBuild& build,
                               Abi::SystemCallError& systemCall) noexcept
{
    // The stack and the start-block page above it.
    const ComponentPlatform& platform = m_launch->Component;
    const Foundation::UInt64 dataPages = platform.StackPages + 1U;
    const Foundation::UInt64 stagedPages =
        dataPages +
        ((component.Behavior == Abi::ComponentBehaviorCompositor || component.Behavior == Abi::ComponentBehaviorToolkit)
             ? Abi::ComponentUiFontPages
         : component.Behavior == Abi::ComponentBehaviorAuth ? Identity::AuthFixturePages
                                                            : 0U);
    ComponentMapping& mapping = component.Mappings[component.MappingCount];
    mapping = ComponentMapping{
        .Frame = {}, .Memory = {}, .VirtualStart = platform.StackStart, .Pages = stagedPages, .Shared = false};
    if (!m_calls->CreateMemory(AuthorityOf(component), Abi::ObjectTypeFrame, stagedPages, 0U, mapping.Frame,
                               systemCall))
    {
        return false;
    }
    ++component.MappingCount;
    // A task gets a send-only copy of the report Endpoint; a service its own Endpoint and WaitSet.
    // The test supervisor creates its own channel and WaitSet and reports that
    // channel to the boot root.
    System::CapabilityHandle report{};
    System::CapabilityHandle receive{};
    System::CapabilityHandle waitSet{};
    if (component.Mode == ComponentMode::Service && component.Behavior != Abi::ComponentBehaviorFaultSupervisor)
    {
        const bool directory = component.Behavior == Abi::ComponentBehaviorDirectory;
        const bool driver = component.Behavior == Abi::ComponentBehaviorDriverService;
        const bool block = component.Behavior == Abi::ComponentBehaviorBlockDriver;
        const bool filesystem = component.Behavior == Abi::ComponentBehaviorFilesystem;
        const bool display = component.Behavior == Abi::ComponentBehaviorDisplay;
        const bool input = component.Behavior == Abi::ComponentBehaviorInput;
        const bool nic = component.Behavior == Abi::ComponentBehaviorNicDriver;
        const bool network = component.Behavior == Abi::ComponentBehaviorNetwork;
        Foundation::UInt64 selector = Component::EchoSealedSelector;
        Foundation::UInt64 waitTag = Component::EchoWaitTag;
        SelectService(directory, Directory::DirectorySealedSelector, Directory::DirectoryWaitTag, selector, waitTag);
        SelectService(driver, Component::DriverSealedSelector, Component::DriverServiceWaitTag, selector, waitTag);
        SelectService(block, Component::BlockSealedSelector, Component::BlockServiceWaitTag, selector, waitTag);
        SelectService(filesystem, Component::FsSealedSelector, Component::FsServiceWaitTag, selector, waitTag);
        SelectService(component.Behavior == Abi::ComponentBehaviorCompositor, Session::SessionSealedSelector,
                      Session::SessionServiceWaitTag, selector, waitTag);
        SelectService(display, Component::DisplaySealedSelector, Component::DisplayServiceWaitTag, selector, waitTag);
        SelectService(input, Component::InputSealedSelector, Component::InputServiceWaitTag, selector, waitTag);
        SelectService(component.Behavior == Abi::ComponentBehaviorAuth, Identity::AuthControlSelector,
                      Identity::AuthControlSelector, selector, waitTag);
        SelectService(component.Behavior == Abi::ComponentBehaviorLogin, Identity::LoginControlSelector,
                      Identity::LoginControlSelector, selector, waitTag);
        SelectService(nic, Component::NicSealedSelector, Component::NicServiceWaitTag, selector, waitTag);
        SelectService(network, Component::SocketSealedSelector, Component::SocketServiceWaitTag, selector, waitTag);
        if (!PrepareService(component, selector, waitTag, receive, waitSet, systemCall))
        {
            return false;
        }
    }
    else
    {
        const Abi::SystemCallResponse granted = m_calls->GetClient().GrantCapability(
            build.Report, component.Capabilities, InvokeRight, EndpointType, Abi::CapabilityGrantCopy);
        if (!Check(granted, systemCall))
        {
            return false;
        }
        report = System::CapabilityHandle{granted.Value0, granted.Value1};
    }
    if (!m_calls->CreateMemory(AuthorityOf(component), Abi::ObjectTypeMemoryObject,
                               stagedPages + (component.Behavior == Abi::ComponentBehaviorPatternClient
                                                  ? Abi::ComponentPatternInputPages
                                                  : 0U),
                               StagingPermissions, mapping.Memory, systemCall) ||
        !MapWindow(mapping, systemCall))
    {
        return false;
    }
    volatile Foundation::UInt64* const block = reinterpret_cast<volatile Foundation::UInt64*>(
        static_cast<Foundation::UIntPointer>(m_bootstrap->WindowStart + (platform.StackPages * PageBytes)));
    block[Abi::ComponentStartMagicWord] = Abi::ComponentStartMagic;
    block[Abi::ComponentIncarnationWord] = component.Tag;
    block[Abi::ComponentReportWord] = report.Selector;
    block[Abi::ComponentReportWord + 1U] = report.Authenticator;
    block[Abi::ComponentWaitSetWord] = waitSet.Selector;
    block[Abi::ComponentWaitSetWord + 1U] = waitSet.Authenticator;
    block[Abi::ComponentReceiveWord] = receive.Selector;
    block[Abi::ComponentReceiveWord + 1U] = receive.Authenticator;
    block[Abi::ComponentSpinTicksWord] = m_launch->ComponentSpinTicks;
    block[Abi::ComponentBehaviorWord] = component.Behavior;
    // A crashing incarnation divides by zero with the exception unmasked; the pager's own exception
    // thread executes an invalid instruction.
    block[Abi::ComponentCrashKindWord] = (component.Behavior == Abi::ComponentBehaviorCrash)
                                             ? Abi::ComponentCrashFloatingPoint
                                             : Abi::ComponentCrashInvalidInstruction;
    // An interpreter's extension record follows the 16 words.
    if (component.Behavior == Abi::ComponentBehaviorInterpret)
    {
        const Loader::ExecutableIdentity& identity = component.Foreign.Identity;
        block[Abi::ComponentForeignMagicWord] = Abi::ComponentForeignMagic;
        block[Abi::ComponentForeignVersionWord] = Abi::ComponentForeignVersion;
        block[Abi::ComponentForeignAddressWord] = platform.ForeignImageAddress;
        block[Abi::ComponentForeignBytesWord] = component.Foreign.Size;
        block[Abi::ComponentForeignFormatWord] = static_cast<Foundation::UInt64>(identity.Format);
        block[Abi::ComponentForeignMachineWord] = static_cast<Foundation::UInt64>(identity.Machine);
        block[Abi::ComponentForeignAbiWord] = static_cast<Foundation::UInt64>(identity.Abi);
        block[Abi::ComponentForeignByteOrderWord] = static_cast<Foundation::UInt64>(identity.ByteOrder);
    }
    if ((component.Behavior == Abi::ComponentBehaviorClient) &&
        !PrepareClient(component, build.Directory, block, systemCall))
    {
        return false;
    }
    if ((component.Behavior == Abi::ComponentBehaviorFilesystem) &&
        !PrepareDriverClient(component, build, block, Component::BlockReplyCapacity, Component::FsBlockReplyWaitTag,
                             systemCall))
    {
        return false;
    }
    if ((component.Behavior == Abi::ComponentBehaviorNetwork) &&
        !PrepareDriverClient(component, build, block, Component::SocketDriverReplyCapacity,
                             Component::SocketDriverReplyWaitTag, systemCall))
    {
        return false;
    }
    const Foundation::UInt64 controller = build.ServiceFlags & Abi::XhciControllerIndexMask;
    const bool group =
        (component.Behavior == Abi::ComponentBehaviorXhci) && ((build.ServiceFlags & Abi::UsbHostGroupFlag) != 0U);
    if ((component.Behavior == Abi::ComponentBehaviorXhci) &&
        ((controller >= Abi::PagerXhciControllers) || (group && (controller != 0U)) ||
         ((build.ServiceFlags &
           ~(Abi::XhciControllerIndexMask | Abi::XhciReportProgressFlag | Abi::UsbHostGroupFlag)) != 0U)))
    {
        systemCall = Abi::SystemCallError::InvalidArguments;
        return false;
    }
    block[Abi::ComponentMountFlagsWord] = build.ServiceFlags;
    // The USB host group gets every controller of the USB host table below;
    // its teardown confirms each of them quiet (UsbHostDriver.md).
    const Foundation::UInt8 device = group ? ComponentUsbHostGroup
                                     : (component.Behavior == Abi::ComponentBehaviorXhci)
                                         ? static_cast<Foundation::UInt8>(Abi::PagerDeviceXhci + controller)
                                         : DeviceOf(component.Behavior);
    const bool driver = (device != ComponentNoDevice) || group;
    const bool display = component.Behavior == Abi::ComponentBehaviorDisplay;
    const bool input = component.Behavior == Abi::ComponentBehaviorInput;
    // The filesystem service maps its bounce page and its clients' Frames in its own window, the
    // network stack its packet pages.
    const bool filesystem = (component.Behavior == Abi::ComponentBehaviorFilesystem) ||
                            (component.Behavior == Abi::ComponentBehaviorNetwork);
    if (((component.Behavior == Abi::ComponentBehaviorAccount) ||
         (component.Behavior == Abi::ComponentBehaviorFaultSupervisor) ||
         (component.Behavior == Abi::ComponentBehaviorAuth) || (component.Behavior == Abi::ComponentBehaviorLogin) ||
         driver || display || input || (component.Behavior == Abi::ComponentBehaviorSessionProbe) ||
         (component.Behavior == Abi::ComponentBehaviorCompositor) ||
         (component.Behavior == Abi::ComponentBehaviorPatternClient) ||
         (component.Behavior == Abi::ComponentBehaviorToolkit) || (component.Behavior == Abi::ComponentBehaviorPs2) ||
         filesystem) &&
        !PrepareAccount(component, block, systemCall))
    {
        return false;
    }
    if ((component.Behavior == Abi::ComponentBehaviorInputClient) ||
        (component.Behavior == Abi::ComponentBehaviorSessionProbe) ||
        (component.Behavior == Abi::ComponentBehaviorCompositor) ||
        (component.Behavior == Abi::ComponentBehaviorPatternClient) ||
        (component.Behavior == Abi::ComponentBehaviorToolkit) || (component.Behavior == Abi::ComponentBehaviorPs2) ||
        (component.Behavior == Abi::ComponentBehaviorXhci))
    {
        const System::SystemCallClient& client = m_calls->GetClient();
        const Abi::SystemCallResponse reply = client.GrantCapability(
            component.Service, component.Capabilities, ReplyRights, EndpointType, Abi::CapabilityGrantCopy);
        const Abi::SystemCallResponse broker = client.GrantCapability(
            build.DriverService, component.Capabilities,
            (component.Behavior == Abi::ComponentBehaviorPs2 || component.Behavior == Abi::ComponentBehaviorXhci ||
             component.Behavior == Abi::ComponentBehaviorSessionProbe ||
             component.Behavior == Abi::ComponentBehaviorCompositor ||
             component.Behavior == Abi::ComponentBehaviorPatternClient ||
             component.Behavior == Abi::ComponentBehaviorToolkit)
                ? ReplyRights
                : InvokeRight,
            EndpointType, Abi::CapabilityGrantCopy);
        if (!Check(reply, systemCall) || !Check(broker, systemCall))
        {
            return false;
        }
        block[Abi::ComponentReplyWord] = reply.Value0;
        block[Abi::ComponentReplyWord + 1U] = reply.Value1;
        block[Abi::ComponentInputServiceWord] = broker.Value0;
        block[Abi::ComponentInputServiceWord + 1U] = broker.Value1;
        block[Abi::ComponentInputEpochWord] = build.DriverGeneration;
    }
    if (component.Behavior == Abi::ComponentBehaviorPs2 || component.Behavior == Abi::ComponentBehaviorXhci)
    {
        const auto& client = m_calls->GetClient();
        const auto root = client.GrantCapability(build.Report, component.Capabilities, InvokeRight, EndpointType,
                                                 Abi::CapabilityGrantCopy);
        if (!Check(root, systemCall))
        {
            return false;
        }
        block[Abi::ComponentReportWord] = root.Value0;
        block[Abi::ComponentReportWord + 1U] = root.Value1;
        constexpr Foundation::Size words[]{Abi::ComponentPs2DataWord, Abi::ComponentPs2CommandWord,
                                           Abi::ComponentPs2KeyboardLineWord, Abi::ComponentPs2MouseLineWord};
        for (Foundation::Size index{0U}; (component.Behavior == Abi::ComponentBehaviorPs2) && (index < 4U); ++index)
        {
            const auto granted = client.GrantCapability(
                m_bootstrap->I8042[index], component.Capabilities,
                index < 2U ? static_cast<Foundation::UInt16>(ReadRight | 0x0008U) : InvokeRight,
                index < 2U ? DeviceRegionType : Abi::ObjectTypeInterruptLine, Abi::CapabilityGrantCopy);
            if (!Check(granted, systemCall))
            {
                return false;
            }
            block[words[index]] = granted.Value0;
            block[words[index] + 1U] = granted.Value1;
        }
    }
    if (!PrepareSession(component, build, mapping, block, dataPages, systemCall))
    {
        return false;
    }
    if (display && !PrepareScanout(component, block, systemCall))
    {
        return false;
    }
    // A failed grant leaves the record naming the device, so the teardown
    // confirms quiet whatever was assigned before the failure.
    component.Device = device;
    if (!group && driver && !m_devices->Grant(device, component.Capabilities, component.Domain, block, systemCall))
    {
        return false;
    }
    for (Foundation::Size slot{0U}, granted{0U}; group && (slot < Abi::PagerUsbHostControllers); ++slot)
    {
        if (m_devices->IsPresent(Abi::PagerDeviceUsbHost + slot) &&
            !m_devices->Grant(Abi::PagerDeviceUsbHost + slot, component.Capabilities, component.Domain, block,
                              systemCall, Abi::ComponentUsbHostRegionWord(granted++)))
        {
            return false;
        }
    }
    if (!CleanWindow(systemCall))
    {
        return false;
    }
    mapping.Pages = dataPages;
    return m_calls->Map(component.Space, mapping, platform.StackStart, StagingPermissions, systemCall);
}

// The guest's bytes are data to the interpreter: staged like a segment's
// file bytes, then mapped read-only and never executable, so W^X holds
// whatever they are.
bool ComponentLoader::LoadForeign(ComponentRecord& component, Abi::SystemCallError& systemCall) noexcept
{
    const Foundation::UInt64 address = m_launch->Component.ForeignImageAddress;
    const Foundation::UInt64 pages =
        (static_cast<Foundation::UInt64>(component.Foreign.Size) + PageBytes - 1U) / PageBytes;
    if ((address == 0U) || (component.Foreign.Bytes == nullptr) || (pages == 0U) ||
        (pages > Abi::ComponentForeignImagePages))
    {
        return false;
    }
    const Loader::ExecutableSegment segment{.VirtualStart = address,
                                            .Pages = pages,
                                            .PageOffset = 0U,
                                            .FileOffset = 0U,
                                            .FileBytes = component.Foreign.Size,
                                            .Writable = false,
                                            .Executable = false};
    return LoadSegment(component, segment, component.Foreign.Bytes, systemCall);
}

// The service Endpoint receives in the component's CapabilitySpace; the root service keeps its
// first capability, sealed with the service's selector, to send on and to destroy, and binds it to
// the WaitSet, as the only binder
bool ComponentLoader::PrepareService(ComponentRecord& component, const Foundation::UInt64 selector,
                                     const Foundation::UInt64 tag, System::CapabilityHandle& service,
                                     System::CapabilityHandle& waitSet, Abi::SystemCallError& systemCall) noexcept
{
    Foundation::UInt8(&request)[Abi::ObjectCreateEndpointRequestSizeBytes] =
        *reinterpret_cast<Foundation::UInt8(*)[Abi::ObjectCreateEndpointRequestSizeBytes]>(m_calls->GetRequest());
    BuildDefaultEndpointRequest(request);
    StoreHandle(request + Abi::ObjectCreateEndpointReceiverSpaceOffset, component.Capabilities);
    StoreWord(request + Abi::ObjectCreateEndpointSealedSelectorOffset, selector);
    if (!m_calls->Create(AuthorityOf(component), Abi::ObjectTypeEndpoint, Abi::ObjectCreateEndpointRequestSizeBytes,
                         component.Service, systemCall))
    {
        return false;
    }
    m_calls->ClearRequest();
    const System::SystemCallClient& client = m_calls->GetClient();
    if (!m_calls->Create(AuthorityOf(component), Abi::ObjectTypeWaitSet, Abi::ObjectCreateWaitSetRequestSizeBytes,
                         component.WaitSet, systemCall)
        // The test pattern polls replies on timed turns. Unread replies must not turn WouldBlock
        // retries into spins.
        || (component.Behavior != Abi::ComponentBehaviorPatternClient &&
            component.Behavior != Abi::ComponentBehaviorToolkit &&
            !Check(client.BindWaitSet(component.WaitSet, component.Service, tag), systemCall)))
    {
        return false;
    }
    const Abi::SystemCallResponse readable = client.GrantCapability(component.Service, component.Capabilities,
                                                                    ReadRight, EndpointType, Abi::CapabilityGrantCopy);
    // The block and NIC drivers bind their Interrupt beside their Endpoint.
    const Foundation::UInt16 waitRights =
        ((component.Behavior == Abi::ComponentBehaviorBlockDriver) ||
         (component.Behavior == Abi::ComponentBehaviorNicDriver) ||
         (component.Behavior == Abi::ComponentBehaviorInput) ||
         (component.Behavior == Abi::ComponentBehaviorCompositor) ||
         (component.Behavior == Abi::ComponentBehaviorDisplay) || (component.Behavior == Abi::ComponentBehaviorAuth) ||
         (component.Behavior == Abi::ComponentBehaviorPs2) || (component.Behavior == Abi::ComponentBehaviorXhci))
            ? static_cast<Foundation::UInt16>(WaitRight | ManageRight)
            : WaitRight;
    const Abi::SystemCallResponse waitable = client.GrantCapability(component.WaitSet, component.Capabilities,
                                                                    waitRights, WaitSetType, Abi::CapabilityGrantCopy);
    if (!Check(readable, systemCall) || !Check(waitable, systemCall))
    {
        return false;
    }
    service = System::CapabilityHandle{readable.Value0, readable.Value1};
    waitSet = System::CapabilityHandle{waitable.Value0, waitable.Value1};
    return true;
}

// A filesystem service is a client of a block service, a network stack of a NIC driver: a copy of
// the root service's send capability on the driver's Endpoint and the instance's tag, and a reply
// Endpoint of its own receiving in its space, of per-sender capacity capacity (2K + 1 for a block
// service, Source/User/System/AhciPort.hpp ), which the root service binds to the service's WaitSet
// beside its service Endpoint before the service runs; the service gets Read on it and Invoke with
// Duplicate to attach send copies, and the root service keeps no handle.
bool ComponentLoader::PrepareDriverClient(ComponentRecord& component, const ComponentBuild& build,
                                          volatile Foundation::UInt64* const block, const Foundation::Size capacity,
                                          const Foundation::UInt64 tag, Abi::SystemCallError& systemCall) noexcept
{
    const System::SystemCallClient& client = m_calls->GetClient();
    const Abi::SystemCallResponse service = client.GrantCapability(build.DriverService, component.Capabilities,
                                                                   InvokeRight, EndpointType, Abi::CapabilityGrantCopy);
    if (!Check(service, systemCall))
    {
        return false;
    }
    Foundation::UInt8(&request)[Abi::ObjectCreateEndpointRequestSizeBytes] =
        *reinterpret_cast<Foundation::UInt8(*)[Abi::ObjectCreateEndpointRequestSizeBytes]>(m_calls->GetRequest());
    m_calls->ClearRequest();
    BuildDefaultEndpointRequest(request);
    StoreHandle(request + Abi::ObjectCreateEndpointReceiverSpaceOffset, component.Capabilities);
    const Foundation::UInt16 bytesPerSender{
        static_cast<Foundation::UInt16>(capacity * Component::EchoMaximumFrameBytes)};
    request[Abi::ObjectCreateEndpointBytesPerSenderOffset] = static_cast<Foundation::UInt8>(bytesPerSender);
    request[Abi::ObjectCreateEndpointBytesPerSenderOffset + 1U] = static_cast<Foundation::UInt8>(bytesPerSender >> 8U);
    request[Abi::ObjectCreateEndpointMessagesPerSenderOffset] = static_cast<Foundation::UInt8>(capacity);
    System::CapabilityHandle replies{};
    if (!m_calls->Create(AuthorityOf(component), Abi::ObjectTypeEndpoint, Abi::ObjectCreateEndpointRequestSizeBytes,
                         replies, systemCall))
    {
        return false;
    }
    const bool bound = Check(client.BindWaitSet(component.WaitSet, replies, tag), systemCall);
    const Abi::SystemCallResponse granted =
        bound ? client.GrantCapability(replies, component.Capabilities, BlockReplyRights, EndpointType,
                                       Abi::CapabilityGrantCopy)
              : Abi::SystemCallResponse{};
    const bool closed = m_calls->Close(replies, systemCall);
    if (!bound || !Check(granted, systemCall) || !closed)
    {
        return false;
    }
    block[Abi::ComponentBlockServiceWord] = service.Value0;
    block[Abi::ComponentBlockServiceWord + 1U] = service.Value1;
    block[Abi::ComponentBlockGenerationWord] = build.DriverGeneration;
    block[Abi::ComponentMountFlagsWord] = build.ServiceFlags;
    block[Abi::ComponentBlockReplyWord] = granted.Value0;
    block[Abi::ComponentBlockReplyWord + 1U] = granted.Value1;
    if (component.Behavior == Abi::ComponentBehaviorFilesystem)
    {
        // a separate sealed manager face. Applications get Invoke on Service
        // only; CM gets Invoke on FsManager, root keeps Manage.
        BuildDefaultEndpointRequest(request);
        StoreHandle(request + Abi::ObjectCreateEndpointReceiverSpaceOffset, component.Capabilities);
        StoreWord(request + Abi::ObjectCreateEndpointSealedSelectorOffset, Component::FsManagerSelector);
        if (!m_calls->Create(AuthorityOf(component), Abi::ObjectTypeEndpoint, Abi::ObjectCreateEndpointRequestSizeBytes,
                             component.FsManager, systemCall) ||
            !Check(client.BindWaitSet(component.WaitSet, component.FsManager, Component::FsManagerWaitTag), systemCall))
        {
            return false;
        }
        const auto manager = client.GrantCapability(component.FsManager, component.Capabilities, ReadRight,
                                                    EndpointType, Abi::CapabilityGrantCopy);
        if (!Check(manager, systemCall))
        {
            return false;
        }
        block[Abi::ComponentFsManagerWord] = manager.Value0;
        block[Abi::ComponentFsManagerWord + 1U] = manager.Value1;
    }
    return true;
}

// A client receives the replies to its requests on an Endpoint of its own,
// unsealed (the Directory's and Echo's replies need no sealing), whose
// readiness wakes its WaitSet; it may only invoke the Directory, which a
// root-service handle with Manage must not reach.
bool ComponentLoader::PrepareClient(ComponentRecord& component, const System::CapabilityHandle directory,
                                    volatile Foundation::UInt64* const block, Abi::SystemCallError& systemCall) noexcept
{
    System::CapabilityHandle receive{};
    System::CapabilityHandle waitSet{};
    if (!PrepareService(component, 0U, Client::ClientWaitTag, receive, waitSet, systemCall))
    {
        return false;
    }
    const System::SystemCallClient& client = m_calls->GetClient();
    const Abi::SystemCallResponse reply = client.GrantCapability(component.Service, component.Capabilities, ReplyRights,
                                                                 EndpointType, Abi::CapabilityGrantCopy);
    const Abi::SystemCallResponse invoked =
        client.GrantCapability(directory, component.Capabilities, InvokeRight, EndpointType, Abi::CapabilityGrantCopy);
    if (!Check(reply, systemCall) || !Check(invoked, systemCall))
    {
        return false;
    }
    block[Abi::ComponentWaitSetWord] = waitSet.Selector;
    block[Abi::ComponentWaitSetWord + 1U] = waitSet.Authenticator;
    block[Abi::ComponentReceiveWord] = receive.Selector;
    block[Abi::ComponentReceiveWord + 1U] = receive.Authenticator;
    block[Abi::ComponentDirectoryWord] = invoked.Value0;
    block[Abi::ComponentDirectoryWord + 1U] = invoked.Value1;
    block[Abi::ComponentReplyWord] = reply.Value0;
    block[Abi::ComponentReplyWord + 1U] = reply.Value1;
    return true;
}

// What a component holds of its own domain: copies of its ResourceDomain's capability, Manage and
// Inspect only, and of its AddressSpace's, Map only, and the platform's window.
bool ComponentLoader::PrepareAccount(ComponentRecord& component, volatile Foundation::UInt64* const block,
                                     Abi::SystemCallError& systemCall) noexcept
{
    if (component.Account.Selector == 0U)
    {
        return false;
    }
    const System::SystemCallClient& client = m_calls->GetClient();
    const Abi::SystemCallResponse account = client.GrantCapability(
        component.Account, component.Capabilities, AccountRights, ResourceDomainType, Abi::CapabilityGrantCopy);
    const Abi::SystemCallResponse space = client.GrantCapability(component.Space, component.Capabilities, MapRight,
                                                                 AddressSpaceType, Abi::CapabilityGrantCopy);
    if (!Check(account, systemCall) || !Check(space, systemCall))
    {
        return false;
    }
    block[Abi::ComponentAccountWord] = account.Value0;
    block[Abi::ComponentAccountWord + 1U] = account.Value1;
    block[Abi::ComponentAccountSpaceWord] = space.Value0;
    block[Abi::ComponentAccountSpaceWord + 1U] = space.Value1;
    // A driver's window starts with a page more, for its device's registers; the NIC driver's is
    // laid out by its own ABI.
    block[Abi::ComponentWindowStartWord] = m_launch->Component.WindowStart;
    Foundation::UInt64 windowPages =
        m_launch->Component.WindowPages + ((DeviceOf(component.Behavior) != ComponentNoDevice) ? 1U : 0U);
    windowPages = (component.Behavior == Abi::ComponentBehaviorBlockDriver) ? Abi::AhciWindowPages : windowPages;
    windowPages = (component.Behavior == Abi::ComponentBehaviorNicDriver) ? Abi::NicWindowPages : windowPages;
    windowPages = (component.Behavior == Abi::ComponentBehaviorNetwork) ? Abi::NetworkWindowPages : windowPages;
    windowPages = component.Behavior == Abi::ComponentBehaviorXhci ? Abi::XhciWindowPages : windowPages;
    block[Abi::ComponentWindowPagesWord] = windowPages;
    block[Abi::ComponentPs2DeclaredWord] = m_bootstrap->I8042Declared;
    block[Abi::ComponentClockTicksPerMicrosecondWord] = m_bootstrap->ClockTicksPerMicrosecond;
    return true;
}

// The display broker's copy of the scanout's DeviceRegion and its mode; the root service keeps its
// own.
bool ComponentLoader::PrepareScanout(ComponentRecord& component, volatile Foundation::UInt64* const block,
                                     Abi::SystemCallError& systemCall) noexcept
{
    const PagerScanout& scanout = m_bootstrap->Scanout;
    if (scanout.Region.Selector == 0U)
    {
        systemCall = Abi::SystemCallError::InvalidConfiguration;
        return false;
    }
    const Abi::SystemCallResponse region = m_calls->GetClient().GrantCapability(
        scanout.Region, component.Capabilities, ScanoutRights, DeviceRegionType, Abi::CapabilityGrantCopy);
    if (!Check(region, systemCall))
    {
        return false;
    }
    block[Abi::ComponentDeviceRegionWord] = region.Value0;
    block[Abi::ComponentDeviceRegionWord + 1U] = region.Value1;
    block[Abi::ComponentScanoutAddressWord] = m_launch->Component.ScanoutAddress;
    block[Abi::ComponentScanoutModeWord] = scanout.Width | (static_cast<Foundation::UInt64>(scanout.Height) << 32U);
    block[Abi::ComponentScanoutModeWord + 1U] =
        scanout.Stride | (static_cast<Foundation::UInt64>(scanout.Format) << 32U);
    return true;
}

bool ComponentLoader::StartThread(ComponentRecord& component, Abi::SystemCallError& systemCall) noexcept
{
    m_calls->ClearRequest();
    Foundation::UInt8* const request = m_calls->GetRequest();
    // An IA-64 entry is a descriptor in this service's own memory, which the
    // kernel reads through its record.
    const ComponentPlatform& platform = m_launch->Component;
    const Loader::ExecutableImage& layout = component.Image->Layout;
    component.EntryDescriptor[0U] = layout.Entry;
    component.EntryDescriptor[1U] = layout.GlobalPointer;
    const Foundation::UInt64 entry =
        platform.Image.EntryDescriptor ? AddressOf(component.EntryDescriptor) : layout.Entry;
    const Foundation::UInt64 startBlock = platform.StackStart + (platform.StackPages * PageBytes);
    StoreHandle(request + Abi::ObjectCreateThreadDomainOffset, component.Domain);
    StoreWord(request + Abi::ObjectCreateThreadEntryOffset, entry);
    StoreWord(request + Abi::ObjectCreateThreadStackOffset, startBlock);
    StoreWord(request + Abi::ObjectCreateThreadArgumentOffset, startBlock);
    const Abi::SystemCallResponse created =
        m_calls->GetClient().CreateThread(AuthorityOf(component), AddressOf(request));
    component.Thread = System::CapabilityHandle{created.Value0, created.Value1};
    if (!Check(created, systemCall))
    {
        return false;
    }
    component.State = ComponentState::Running;
    return true;
}
bool ComponentLoader::PrepareUiFonts(ComponentRecord& component, const ComponentMapping& mapping,
                                     volatile Foundation::UInt64* const block, const Foundation::UInt64 dataPages,
                                     Abi::SystemCallError& systemCall) noexcept
{
    const auto frame = m_calls->GetClient().GrantCapability(mapping.Frame, component.Capabilities, MapRight,
                                                            Abi::ObjectTypeFrame, Abi::CapabilityGrantCopy);
    if (!Check(frame, systemCall))
    {
        return false;
    }
    block[Abi::ComponentUiFontFrameWord] = frame.Value0;
    block[Abi::ComponentUiFontFrameWord + 1U] = frame.Value1;
    const auto memory = m_calls->GetClient().GrantCapability(mapping.Memory, component.Capabilities, MapRight,
                                                             Abi::ObjectTypeMemoryObject, Abi::CapabilityGrantCopy);
    if (!Check(memory, systemCall))
    {
        return false;
    }
    block[Abi::ComponentUiFontMemoryWord] = memory.Value0;
    block[Abi::ComponentUiFontMemoryWord + 1U] = memory.Value1;
    block[Abi::ComponentUiFontPageWord] = dataPages;
    const auto* const table =
        reinterpret_cast<const Foundation::UInt8*>(static_cast<Foundation::UIntPointer>(m_bootstrap->ModuleBase));
    for (Foundation::Size face{0U}; face < 2U; ++face)
    {
        const Foundation::UInt8* bytes{};
        Foundation::Size length{};
        const bool found =
            FindComponentImage(table, static_cast<Foundation::Size>(m_bootstrap->ModuleBytes),
                               face == 0U ? Abi::ComponentUiBoldModule : Abi::ComponentUiRegularModule, bytes, length);
        if (!found || length == 0U || length > Abi::ComponentUiFontFaceBytes)
        {
            length = 0U;
        }
        auto* const out = reinterpret_cast<volatile Foundation::UInt8*>(static_cast<Foundation::UIntPointer>(
            m_bootstrap->WindowStart + dataPages * PageBytes + face * Abi::ComponentUiFontFaceBytes));
        for (Foundation::Size at{0U}; at < length; ++at)
        {
            out[at] = bytes[at];
        }
        block[face == 0U ? Abi::ComponentUiFontBoldBytesWord : Abi::ComponentUiFontRegularBytesWord] = length;
    }
    return true;
}
bool ComponentLoader::PrepareSession(ComponentRecord& component, const ComponentBuild& build,
                                     const ComponentMapping& mapping, volatile Foundation::UInt64* const block,
                                     const Foundation::UInt64 dataPages, Abi::SystemCallError& systemCall) noexcept
{
    if (component.Behavior == Abi::ComponentBehaviorAuth || component.Behavior == Abi::ComponentBehaviorLogin)
    {
        const auto report = m_calls->GetClient().GrantCapability(build.Report, component.Capabilities, InvokeRight,
                                                                 EndpointType, Abi::CapabilityGrantCopy);
        const auto reply = m_calls->GetClient().GrantCapability(component.Service, component.Capabilities, ReplyRights,
                                                                EndpointType, Abi::CapabilityGrantCopy);
        if (!Check(report, systemCall) || !Check(reply, systemCall))
        {
            return false;
        }
        block[Abi::ComponentReportWord] = report.Value0;
        block[Abi::ComponentReportWord + 1U] = report.Value1;
        block[Abi::ComponentReplyWord] = reply.Value0;
        block[Abi::ComponentReplyWord + 1U] = reply.Value1;
    }
    if (component.Behavior == Abi::ComponentBehaviorAuth)
    {
        // Synthetic read-only data receipt: loader transports opaque fixture
        // bytes; only auth parses them. No persistent provisioning is implied.
        const Foundation::UInt8* bytes{};
        Foundation::Size length{};
        if (!FindComponentImage(m_launch->ComponentImages, m_launch->ComponentImagesBytes, "accounts", bytes, length) ||
            length != Identity::StorageBytes)
        {
            systemCall = Abi::SystemCallError::InvalidConfiguration;
            return false;
        }
        auto* const out = reinterpret_cast<volatile Foundation::UInt8*>(
            static_cast<Foundation::UIntPointer>(m_bootstrap->WindowStart + dataPages * PageBytes));
        for (Foundation::Size i{0U}; i < length; ++i)
        {
            out[i] = bytes[i];
        }
        const auto fixture = m_calls->GetClient().GrantCapability(mapping.Frame, component.Capabilities, MapRight,
                                                                  Abi::ObjectTypeFrame, Abi::CapabilityGrantCopy);
        const auto limiter =
            m_calls->GetClient().GrantCapability(build.IdentityLimiter, component.Capabilities, MapRight | 0x0008U,
                                                 Abi::ObjectTypeFrame, Abi::CapabilityGrantCopy);
        if (!Check(fixture, systemCall) || !Check(limiter, systemCall))
        {
            return false;
        }
        block[Identity::AuthFixtureFrameWord] = fixture.Value0;
        block[Identity::AuthFixtureFrameWord + 1U] = fixture.Value1;
        block[Identity::AuthFixtureOffsetWord] = dataPages;
        block[Identity::AuthLimiterFrameWord] = limiter.Value0;
        block[Identity::AuthLimiterFrameWord + 1U] = limiter.Value1;
    }
    if (component.Behavior == Abi::ComponentBehaviorCompositor ||
        component.Behavior == Abi::ComponentBehaviorPatternClient ||
        component.Behavior == Abi::ComponentBehaviorToolkit)
    {
        const auto seed = build.ServiceFlags >> 32U;
        const bool small = component.Behavior == Abi::ComponentBehaviorPatternClient
                               ? seed == 3U || seed == 5U
                               : (build.ServiceFlags & Abi::ComponentSessionTestSmallOutputFlag) != 0U;
        block[Abi::ComponentSessionOutputModeWord] =
            Session::PackSessionPair(small ? Abi::ComponentSessionTestWidth : m_bootstrap->Scanout.Width,
                                     small ? Abi::ComponentSessionTestHeight : m_bootstrap->Scanout.Height);
    }
    if (component.Behavior == Abi::ComponentBehaviorCompositor || component.Behavior == Abi::ComponentBehaviorToolkit)
    {
        if (!PrepareUiFonts(component, mapping, block, dataPages, systemCall))
        {
            return false;
        }
    }
    if (component.Behavior == Abi::ComponentBehaviorPatternClient)
    {
        const auto memory = m_calls->GetClient().GrantCapability(mapping.Memory, component.Capabilities, MapRight,
                                                                 Abi::ObjectTypeMemoryObject, Abi::CapabilityGrantCopy);
        if (!Check(memory, systemCall))
        {
            return false;
        }
        block[Abi::ComponentPatternMemoryWord] = memory.Value0;
        block[Abi::ComponentPatternMemoryWord + 1U] = memory.Value1;
        block[Abi::ComponentPatternMemoryOffsetWord] = dataPages;
        if (build.SessionInput.Selector != 0U)
        {
            const auto second = m_calls->GetClient().GrantCapability(
                build.SessionInput, component.Capabilities, ReplyRights, EndpointType, Abi::CapabilityGrantCopy);
            if (!Check(second, systemCall))
            {
                return false;
            }
            block[Abi::ComponentPatternSecondClientWord] = second.Value0;
            block[Abi::ComponentPatternSecondClientWord + 1U] = second.Value1;
        }
    }
    if (component.Behavior == Abi::ComponentBehaviorCompositor ||
        component.Behavior == Abi::ComponentBehaviorPatternClient ||
        component.Behavior == Abi::ComponentBehaviorToolkit)
    {
        const auto report = m_calls->GetClient().GrantCapability(build.Report, component.Capabilities, InvokeRight,
                                                                 EndpointType, Abi::CapabilityGrantCopy);
        if (!Check(report, systemCall))
        {
            return false;
        }
        block[Abi::ComponentSessionReportWord] = report.Value0;
        block[Abi::ComponentSessionReportWord + 1U] = report.Value1;
        if (component.Behavior == Abi::ComponentBehaviorCompositor && build.SessionInput.Selector != 0U)
        {
            const auto input = m_calls->GetClient().GrantCapability(
                build.SessionInput, component.Capabilities, ReplyRights, EndpointType, Abi::CapabilityGrantCopy);
            if (!Check(input, systemCall))
            {
                return false;
            }
            block[Abi::ComponentSessionInputWord] = input.Value0;
            block[Abi::ComponentSessionInputWord + 1U] = input.Value1;
            block[Abi::ComponentSessionInputEpochWord] = build.SessionInputEpoch;
        }
    }
    return true;
}
} // Namespace Asteria::User::Pager
