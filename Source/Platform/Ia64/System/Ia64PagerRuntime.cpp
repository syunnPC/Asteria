// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/System/Ia64PagerRuntime.hpp"
#include "Abi/ComponentBootstrap.hpp"
#include "Platform/Synchronization/ProcessorContext.hpp"

#include "Platform/Firmware/Diagnostic.hpp"
#include "Platform/Ia64/Diagnostics/Ia64EvidenceRecord.hpp"
#include "Platform/Ia64/Diagnostics/Ia64Stop.hpp"
#include "Platform/Ia64/Ia64UserVirtualLayout.hpp"

#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
extern "C" void AsteriaIa64InitializePagerEntryDescriptor(Asteria::Foundation::UInt64* descriptor) noexcept;
#endif

extern "C" [[noreturn]] ASTERIA_NOINLINE_NOIPA void AsteriaIa64PagerRuntimeEntry(void* argument) noexcept;

namespace Asteria::Platform::Ia64::System
{
using Foundation::Size;
using Foundation::UInt64;
using Kernel::Object::CapabilityRight;
using Kernel::Object::CapabilityRights;

namespace
{
// The pager grant covers component service/report/reply Endpoints, input source/client channels,
// and five WaitSets. The five-domain test also retains two independent pattern-client replies.
constexpr Kernel::Object::ResourceDomainId GrantCharge{2U};
// Watchdog.md: three root-owned private liveness reply Endpoints.
constexpr UInt64 GrantEndpoints{14U + Abi::WatchdogServiceCount};
constexpr UInt64 GrantWaitSets{5U};
// CheckPagerGrant.py derives image, DMA, bounce-buffer and retained-pin peaks, including
// simultaneous USB input, storage and filesystem activity during clean shutdown.
constexpr UInt64 GrantMemoryObjects{29U};
// Admission floor for the integrated session/input images. CheckPagerGrant.py verifies that it
// covers all linked-image peaks; the runtime grant contains all allocatable RAM.
constexpr UInt64 MinimumGrantPages{328U};
static_assert(GrantEndpoints <= Kernel::Ipc::EndpointPool::Capacity);
static_assert(GrantWaitSets <= Kernel::Ipc::WaitSetPool::Capacity);
static_assert(Abi::DesktopGrantEndpoints <= Kernel::Ipc::EndpointPool::Capacity);
static_assert(Abi::DesktopGrantWaitSets <= Kernel::Ipc::WaitSetPool::Capacity);
static_assert(GrantMemoryObjects <= Kernel::Memory::FrameObjectPool::Capacity);
static_assert(GrantMemoryObjects <= Kernel::Memory::MemoryObjectPool::Capacity);
static_assert(Abi::DesktopGrantMemoryPairs <= Kernel::Memory::FrameObjectPool::Capacity);
static_assert(Abi::DesktopGrantMemoryPairs <= Kernel::Memory::MemoryObjectPool::Capacity);
static_assert(MinimumGrantPages <= Ia64FramePoolPages);
// The threads the pager may have alive at once, one per created-thread slot, and the domains, one
// per run-time AddressSpace slot: AMD64's thirty-two and sixteen.
constexpr UInt64 GrantThreads{Ia64UserThreadTable::Slots};
constexpr UInt64 GrantDomains{Memory::Ia64RuntimeAddressSpaceSlotCount};
static_assert(GrantThreads == Ia64UserThreadTable::Slots);
static_assert(GrantDomains == Memory::Ia64RuntimeAddressSpaceSlotCount);
// Each component's ResourceDomain, a child of the grant, one per live component, and the identity
// the first one takes, after the boot domain's and the grant's.
constexpr UInt64 GrantChildDomains{GrantDomains};
constexpr Kernel::Object::ResourceDomainId FirstComponentCharge{3U};

// Evaluate metadata constants at compile time so O0 grant creation stays within 512 bytes.
constexpr UInt64 WaitSetMetadataBytes{Kernel::Ipc::WaitSetPool::GetReservation().KernelMetadataBytes};
constexpr UInt64 SpaceMetadataBytes{Kernel::Object::CapabilitySpacePool::GetReservation().KernelMetadataBytes};
constexpr UInt64 DomainMetadataBytes{Kernel::Resource::ResourceDomainPool::GetReservation().KernelMetadataBytes};
constexpr UInt64 InterruptMetadataBytes{Kernel::Interrupt::InterruptPool::GetReservation().KernelMetadataBytes};

[[nodiscard]] UInt64 AddressOf(const void* const value) noexcept
{
    return static_cast<UInt64>(reinterpret_cast<Foundation::UIntPointer>(value));
}

[[nodiscard]] bool RangeContains(const Executable::Ia64InitialUserImageMemoryRangePlan& plan,
                                 const UInt64 address) noexcept
{
    return (address >= plan.VirtualStart) &&
           ((address - plan.VirtualStart) < (plan.Pages.GetValue() * Ia64UserPageSizeBytes));
}
// The Frames and MemoryObjects the pager created, the frame pool and the pager's mappings, apart
// from RecordManagement to keep each frame within the stack budget.
[[gnu::noinline]] void RecordMemory(const Kernel::System::ObjectManager& objects,
                                    const Kernel::Resource::ResourceDomainUsage& usage,
                                    Diagnostics::Ia64ObjectManagementEvidence& evidence) noexcept
{
    const auto frames = objects.GetFrameSnapshot();
    const auto memoryObjects = objects.GetMemoryObjectSnapshot();
    const auto framePool = objects.GetFramePoolSnapshot();
    evidence.OccupiedSlots +=
        frames.ActiveSlots + frames.QuiescingSlots + memoryObjects.ActiveSlots + memoryObjects.QuiescingSlots;
    evidence.FramesCreated = frames.Created;
    evidence.FramesReclaimed = frames.Reclaimed;
    evidence.MemoryObjectsCreated = memoryObjects.Created;
    evidence.MemoryObjectsReclaimed = memoryObjects.Reclaimed;
    evidence.CommittedPages = usage.CommittedMemoryPages;
    evidence.ResidentPages = usage.ResidentMemoryPages;
    evidence.AllocatorCleanPages = framePool.FreePages;
    evidence.AllocatorDirtyPages = framePool.DirtyPages;
    auto& allocatorEvidence = Diagnostics::GetIa64EvidenceRecord().Allocator;
    const auto allocator = objects.GetAllocatorSnapshot();
    allocatorEvidence.ManagedPages = allocator.ManagedPages;
    allocatorEvidence.AllocatablePages = allocator.AllocatablePages;
    allocatorEvidence.CleanPages = allocator.CleanPages;
    allocatorEvidence.DirtyPages = allocator.DirtyPages;
    allocatorEvidence.ClaimedPages = allocator.ClaimedPages;
    allocatorEvidence.RetiredPages = allocator.RetiredPages;
    allocatorEvidence.AllocatedPages = allocator.AllocatedPages;
    allocatorEvidence.FreeLowBlocks = allocator.FreeLowBlocks;
    allocatorEvidence.ZeroingSlots = frames.ZeroingSlots;
    allocatorEvidence.PeakZeroingSlots = frames.PeakZeroingSlots;
    allocatorEvidence.ZeroDeferrals = frames.ZeroDeferrals;
    allocatorEvidence.ZeroFaults = frames.ZeroFaults;
    allocatorEvidence.CreationZeroedPages = frames.CreationZeroedPages;
    Diagnostics::PublishIa64Evidence(allocatorEvidence.Published);
    const auto mapper = objects.GetMapperSnapshot();
    evidence.Mappings = mapper.Mappings;
    evidence.MappedVirtualStart = mapper.LastMapping.VirtualStart.GetValue();
    evidence.MappedPhysicalStart = mapper.LastMapping.PhysicalStart.GetValue();
    evidence.MappedPages = mapper.LastMapping.Pages.GetValue();
    evidence.Unmappings = mapper.Unmappings;
}
// The created-thread table's counts, apart from RecordManagement to keep
// each frame within the stack budget.
[[gnu::noinline]] void RecordThreadTable(const Ia64UserThreadTable& threads, Diagnostics::Ia64ThreadEvidence& thread,
                                         Diagnostics::Ia64ComponentEvidence& components) noexcept
{
    const Ia64UserThreadTableSnapshot table = threads.GetSnapshot();
    thread.Exits = table.Exits;
    thread.Reaped = table.Reaped;
    thread.FreeNativeSlots = table.FreeSlots;
    thread.Faults = table.Faults;
    thread.Deferrals = table.Deferrals;
    thread.LongestDeferral = table.LongestDeferral;
    thread.Kills = table.Kills;
    thread.Exceptions = table.Exceptions;
    thread.SlotMetadataBytes = sizeof(Kernel::Scheduler::Thread) + sizeof(Kernel::Scheduler::SchedulingContext) +
                               Ia64UserThreadTable::ComputeNativeSlotBytes();
    components.TaggedFaults = table.TaggedFaults;
    components.LastTag = table.LastTag;
    components.DomainExits = table.DomainExits;
    auto& services = Diagnostics::GetIa64EvidenceRecord().Services;
    services.WaitsBlocked = table.WaitsBlocked;
    services.WaitExpiries = table.WaitExpiries;
    services.WaitAborts = table.WaitAborts;
    services.AbortedExits = table.AbortedExits;
}
// Record collection and device-control results separately from RecordManagement
// to keep each frame within the stack budget.
[[gnu::noinline]] void RecordCollection(const Kernel::System::ObjectManager& objects) noexcept
{
    const Kernel::System::ObjectCollectionSnapshot collected = objects.GetCollectionSnapshot();
    auto& evidence = Diagnostics::GetIa64EvidenceRecord().Collection;
    evidence.Collections = collected.Collections;
    evidence.Reclaimed = collected.Reclaimed;
    evidence.ScrubRuns = collected.ScrubRuns;
    evidence.ScrubbedPages = collected.ScrubbedPages;
    Diagnostics::PublishIa64Evidence(evidence.Published);
    // The WaitSets created threads wait on; the waits' counts come with the thread table's.
    const auto waitSets = objects.GetWaitSetSnapshot();
    auto& services = Diagnostics::GetIa64EvidenceRecord().Services;
    services.WaitSetsCreated = waitSets.Created;
    services.WaitSetsReclaimed = waitSets.Reclaimed;
    services.Binds = objects.GetBinds();
    Diagnostics::PublishIa64Evidence(services.Published);
    // The assigned function's control.
    const Kernel::Device::PciFunctionControlSnapshot functions = objects.GetDeviceControlSnapshot();
    auto& control = Diagnostics::GetIa64EvidenceRecord().DeviceControl;
    control.Assignments = functions.Assignments;
    control.Releases = functions.Releases;
    control.BusMasterRefusals = functions.BusMasterRefusals;
    control.Quarantines = functions.Quarantines;
    control.Confirmations = functions.Confirmations;
    Diagnostics::PublishIa64Evidence(control.Published);
}
// The parked Frame pins and what the scrubs found in the released ones' pages, apart from
// RecordCollection to keep each frame within the stack budget.
[[gnu::noinline]] void RecordBlock(const Kernel::System::ObjectManager& objects,
                                   const Memory::Ia64FrameScrubber& scrubber) noexcept
{
    const Kernel::Device::PciFunctionControlSnapshot functions = objects.GetDeviceControlSnapshot();
    auto& block = Diagnostics::GetIa64EvidenceRecord().Block;
    block.PinsParked = functions.PinsParked;
    block.PinsReleased = functions.PinsReleased;
    block.ParkedPins = functions.ParkedPins;
    for (Size slot{0U}; slot < Memory::Ia64FrameScrubberWatches; ++slot)
    {
        const Memory::Ia64FrameScrubSample& sample = scrubber.GetSample(slot);
        Diagnostics::Ia64ParkedReleaseEvidence& released = block.Released[slot];
        if ((sample.Physical == 0U) || (sample.Physical != released.PhysicalStart) || (sample.Sampled == 0U))
        {
            continue;
        }
        released.Sampled = 1U;
        for (Size word{0U}; word < Memory::Ia64FrameScrubberSampleWords; ++word)
        {
            released.Sample[word] = sample.Words[word];
        }
    }
    Diagnostics::PublishIa64Evidence(block.Published);
}
// The device's Interrupts: the pool's view and the lines' deliveries.
[[gnu::noinline]] void RecordInterrupts(const Kernel::System::ObjectManager& objects,
                                        const Interrupt::Ia64InterruptLines* const lines) noexcept
{
    const Kernel::Interrupt::InterruptPoolSnapshot pool = objects.GetInterruptSnapshot();
    auto& evidence = Diagnostics::GetIa64EvidenceRecord().Interrupts;
    evidence.Created = pool.Created;
    evidence.Destroyed = pool.Destroyed;
    evidence.Reclaimed = pool.Reclaimed;
    evidence.Live = pool.ActiveSlots + pool.QuiescingSlots;
    evidence.Delivered = (lines != nullptr) ? lines->GetDelivered() : 0U;
    evidence.Spurious = (lines != nullptr) ? lines->GetSpurious() : 0U;
    Diagnostics::PublishIa64Evidence(evidence.Published);
}
// The components' ResourceDomains, apart from RecordManagement to keep each frame within the stack
// budget.
[[gnu::noinline]] void RecordAccounts(const Kernel::System::ObjectManager& objects,
                                      Diagnostics::Ia64ComponentEvidence& evidence) noexcept
{
    const auto accounts = objects.GetResourceDomainSnapshot();
    evidence.ResourceDomainsCreated = accounts.Created;
    evidence.ResourceDomainsDestroyed = accounts.Destroyed;
    evidence.ResourceDomainsReclaimed = accounts.Reclaimed;
    evidence.ResourceDomainsLive = accounts.ActiveSlots + accounts.QuiescingSlots;
}
// The domains the pager created, apart from RecordManagement to keep each frame within the stack
// budget.
[[gnu::noinline]] void RecordDomains(const Kernel::System::ObjectManager& objects,
                                     const Memory::Ia64BspRuntimeMemory& memory,
                                     const Ia64UserThreadTable* const threads) noexcept
{
    const auto spaces = objects.GetAddressSpaceSnapshot();
    const auto capabilities = objects.GetSpaceSnapshot();
    const auto domains = objects.GetDomainSnapshot();
    const auto slots = memory.GetRuntimeAddressSpaceSnapshot();
    auto& evidence = Diagnostics::GetIa64EvidenceRecord().Domains;
    evidence.AddressSpacesCreated = spaces.Created;
    evidence.AddressSpacesReclaimed = spaces.Reclaimed;
    evidence.CapabilitySpacesCreated = capabilities.Created;
    evidence.CapabilitySpacesReclaimed = capabilities.Reclaimed;
    evidence.DomainsCreated = domains.Created;
    evidence.DomainsReclaimed = domains.Reclaimed;
    evidence.FreeAddressSpaceSlots = spaces.FreeSlots;
    evidence.FreeCapabilitySpaceSlots = capabilities.FreeSlots;
    evidence.FreeDomainSlots = domains.FreeSlots;
    evidence.SlotsPrepared = slots.Prepared;
    evidence.SlotsReleased = slots.Released;
    evidence.SlotsSpent = slots.Spent;
    evidence.AllocatedAddressSpaces = memory.GetAllocatedAddressSpaceCount();
    if (threads != nullptr)
    {
        const auto table = threads->GetSnapshot();
        evidence.DomainsRecorded = table.DomainsRecorded;
        evidence.DomainsRetired = table.DomainsRetired;
    }
    Diagnostics::PublishIa64Evidence(evidence.Published);
}
} // Namespace

void Ia64PagerRuntime::ParkedReleases::Released(const Kernel::Object::ObjectId frame,
                                                const Kernel::Memory::PhysicalAddress physicalStart,
                                                const Kernel::Memory::PageCount pages) noexcept
{
    auto& evidence = Diagnostics::GetIa64EvidenceRecord().Block;
    const Size slot = static_cast<Size>(evidence.Releases & 1U);
    evidence.Released[slot] = Diagnostics::Ia64ParkedReleaseEvidence{.Frame = frame.GetValue(),
                                                                     .PhysicalStart = physicalStart.GetValue(),
                                                                     .Pages = pages.GetValue(),
                                                                     .Sampled = 0U,
                                                                     .Sample = {}};
    ++evidence.Releases;
    m_scrubber.Watch(slot, physicalStart.GetValue());
}

Ia64PagerRuntime::Ia64PagerRuntime(const Ia64PagerCollaborators& collaborators,
                                   Hal::Memory::ITlbShootdownTransport& transport,
                                   Kernel::Resource::ResourceDomainLedger& ledger,
                                   const Kernel::Object::CapabilityValidationKey& pagerKey,
                                   const Kernel::Object::CapabilityValidationKey& tokenKey) noexcept
    : m_collaborators{collaborators}, m_space{*collaborators.Registry, *collaborators.Revocations, *collaborators.Clock,
                                              pagerKey.Word0, pagerKey.Word1},
      m_tokens{*collaborators.Registry, *collaborators.Revocations, *collaborators.Clock, tokenKey.Word0,
               tokenKey.Word1},
      m_endpoint{m_space, collaborators.Resources->GetIdentifier()}, m_waitSet{*collaborators.Registry},
      m_wait{*collaborators.Registry, collaborators.Controller->GetWakeFacade()},
      m_waitDispatcher{*collaborators.Registry, m_wait}, m_channel{*collaborators.Registry,
                                                                   *collaborators.Issuer,
                                                                   m_endpoint,
                                                                   collaborators.Controller->GetWakeFacade(),
                                                                   *collaborators.Shootdown,
                                                                   transport},
      m_ledger{ledger}, m_grant{ledger}, m_objects{*collaborators.Registry, *collaborators.Issuer},
      m_scheduling{*collaborators.Registry}, m_pagerFaults{collaborators.Preemption}
{
}

void Ia64PagerRuntime::Wait() noexcept
{
    Platform::Synchronization::ProcessorWaitProgress();
}

Ia64PagerRuntimeError Ia64PagerRuntime::Fail(const Ia64PagerRuntimeError error) noexcept
{
    m_error = error;
    return error;
}

Ia64PagerRuntimeError Ia64PagerRuntime::Initialize(const Ia64PagerRuntimeInput& input) noexcept
{
    if (m_initialized)
    {
        return Fail(Ia64PagerRuntimeError::AlreadyInitialized);
    }
    if ((input.ImageMemory == nullptr) || (input.ImageMapping == nullptr) ||
        (m_collaborators.RuntimeMemory == nullptr) || (m_collaborators.RunLoop == nullptr) ||
        (m_collaborators.Scratch == nullptr) || (m_collaborators.Preemption == nullptr) ||
        !m_collaborators.Processor.IsValid() ||
        ((input.LazyPages != 0U) && ((input.LazyVirtualStart % Ia64UserPageSizeBytes) != 0U)))
    {
        return Fail(Ia64PagerRuntimeError::InvalidInput);
    }
    const auto root = m_collaborators.RuntimeMemory->BorrowReadyUserRoot(Memory::Ia64BspRuntimeUserDomain::Pager);
    if (!root.HasValue())
    {
        return Fail(Ia64PagerRuntimeError::InvalidInput);
    }
    m_pagerSpace = root.GetValue().AddressSpaceIdentity;
    if ((input.TicksPerMicrosecond != 0U) &&
        !m_waitDispatcher.ConfigureTimeouts(*m_collaborators.Clock, input.TicksPerMicrosecond))
    {
        return Fail(Ia64PagerRuntimeError::InvalidInput);
    }
    m_ticksPerMicrosecond = input.TicksPerMicrosecond;
    if (!CreateObjects() || !CreateGrant(input))
    {
        return Fail(Ia64PagerRuntimeError::ObjectRejected);
    }
    if (!CreateLazyRegion(input))
    {
        return Fail(Ia64PagerRuntimeError::CapabilityRejected);
    }
    if (!CaptureImage(input))
    {
        return Fail(Ia64PagerRuntimeError::ImageRejected);
    }
    if (!PrepareThread())
    {
        return Fail(Ia64PagerRuntimeError::ThreadRejected);
    }
    if (!PrepareContext())
    {
        return Fail(Ia64PagerRuntimeError::ContextRejected);
    }
    if (!CheckRegistryBudget())
    {
        return Fail(Ia64PagerRuntimeError::ObjectRejected);
    }
    m_initialized = true;
    return Ia64PagerRuntimeError::None;
}

bool Ia64PagerRuntime::CreateObjects() noexcept
{
    const Kernel::Object::ResourceDomainId charge = m_collaborators.Resources->GetIdentifier();
    if ((m_readiness.Initialize(charge) != Kernel::Ipc::NotificationError::None) ||
        (m_waitSet.Initialize(charge) != Kernel::Ipc::WaitSetError::None))
    {
        return false;
    }
    Kernel::Object::RootCapabilityIssuer& issuer = *m_collaborators.Issuer;
    const auto endpoint = issuer.CreateObject(m_endpoint);
    const auto readiness = issuer.CreateObject(m_readiness);
    const auto waitSet = issuer.CreateObject(m_waitSet);
    const auto root = m_collaborators.Revocations->CreateRoot();
    const auto tokenRoot = m_collaborators.Revocations->CreateRoot();
    if (!endpoint.HasValue() || !readiness.HasValue() || !waitSet.HasValue() || !root.HasValue() ||
        !tokenRoot.HasValue())
    {
        return false;
    }
    m_root = root.GetValue();
    const Kernel::Ipc::NotificationId readinessId{*m_collaborators.Registry, readiness.GetValue()};
    if (!m_waitSet.RegisterNotification(readinessId, m_readiness, Abi::PagerReadinessTag).HasValue() ||
        (m_channel.Configure(Kernel::Memory::PagerChannelConfiguration{
             .TokenSpace = &m_tokens,
             .Readiness = &m_readiness,
             .ShootdownProgress = this,
             .ChargeOwner = charge,
             .Sender = charge,
             .RevocationDomain = tokenRoot.GetValue(),
         }) != Kernel::Memory::PagerChannelError::None))
    {
        return false;
    }
    return MintInto(Abi::PagerBootstrapEndpointWord, endpoint.GetValue(),
                    CapabilityRights{CapabilityRight::Inspect}.With(CapabilityRight::Read)) &&
           MintInto(Abi::PagerBootstrapWaitSetWord, waitSet.GetValue(), CapabilityRights{CapabilityRight::Wait}) &&
           MintInto(Abi::PagerBootstrapDiagnosticWord, m_collaborators.Diagnostic,
                    CapabilityRights{CapabilityRight::Inspect}.With(CapabilityRight::Write)) &&
           MintInto(Abi::PagerBootstrapClientSpaceWord, m_collaborators.ClientSpace.GetObjectId(),
                    CapabilityRights{CapabilityRight::Map});
}

// Pager objects charge a child of the boot domain within the grant's limits.
// Both IA-64 firmware profiles grant it. MemoryMap and MemoryUnmap use the pager's
// window and BSP shootdown dispatcher. Import scratch stays off the corridor stack.
bool Ia64PagerRuntime::ImportMemory(const Ia64PagerRuntimeInput& input) noexcept
{
    if (input.ImportRangeCount == 0U)
    {
        Diagnostics::StopIa64(Kernel::Diagnostics::StopCode::MemoryGrantBelowMinimum, 0U, MinimumGrantPages);
    }
    const auto result =
        m_objects.GetPhysicalPageAllocator().Import(input.ImportRanges, input.ImportRangeCount, input.BitmapStorage,
                                                    input.BitmapBytes, input.LowMemoryBlocks, Abi::BootDmaLowBlocks);
    if (!result.HasValue())
    {
        return false;
    }
    auto& evidence = Diagnostics::GetIa64EvidenceRecord().MemoryImport;
    m_objects.GetPhysicalPageAllocator().RecordImport(evidence);
    evidence.MetadataBudgetBytes = input.BitmapBytes;
    Diagnostics::PublishIa64Evidence(evidence.Published);
    if (result.GetValue().ManagedPages < MinimumGrantPages)
    {
        Diagnostics::StopIa64(Kernel::Diagnostics::StopCode::MemoryGrantBelowMinimum, result.GetValue().ManagedPages,
                              MinimumGrantPages);
    }
    if (!m_collaborators.Resources->FinalizeMemoryImport(result.GetValue().ManagedPages))
    {
        return false;
    }
    return true;
}

bool Ia64PagerRuntime::CreateGrant(const Ia64PagerRuntimeInput& input) noexcept
{
    m_startBlock[Abi::PagerBootstrapModuleBaseWord] = input.ModuleBytes == 0U ? 0U : Abi::Ia64PagerModuleBase;
    m_startBlock[Abi::PagerBootstrapModuleBytesWord] = input.ModuleBytes;

    if (input.InventoryPhysicalStart != 0U && input.InventoryPages != 0U)
    {
        const auto charge = m_collaborators.Resources->GetIdentifier();
        if (m_inventoryFrame.InitializeReadOnly(charge, {Kernel::Memory::PhysicalAddress{input.InventoryPhysicalStart},
                                                         Kernel::Memory::PageCount{input.InventoryPages},
                                                         Kernel::Memory::MemoryCachePolicy::WriteBack,
                                                         Kernel::Memory::FrameKind::Ram,
                                                         {}}) != Kernel::Memory::FrameError::None)
        {
            return false;
        }
        const auto inventory = m_collaborators.Issuer->CreateObject(m_inventoryFrame);
        if (!inventory.HasValue() || !MintInto(Abi::PagerBootstrapInventoryFrameWord, inventory.GetValue(),
                                               CapabilityRights{CapabilityRight::Map}.With(CapabilityRight::Read)))
        {
            return false;
        }
        m_startBlock[Abi::PagerBootstrapInventoryPagesWord] = input.InventoryPages;
    }
    const bool imported = input.ImportRanges != nullptr;
    if (imported && !ImportMemory(input))
    {
        return false;
    }
    const bool frames = imported || input.FramePoolPages != 0U;
    const bool mapping = frames && (input.Transport != nullptr);
    if (frames &&
        (!m_objects.AttachSlabs(input.FrameSlab, input.MemoryObjectSlab) ||
         (!imported && m_objects.InitializeFramePool(Kernel::Memory::PhysicalAddress{input.FramePoolPhysicalStart},
                                                     input.FramePoolPages) != Kernel::Memory::FramePoolError::None) ||
         !(imported ? m_scrubber.Configure(m_objects.GetPhysicalPageAllocator())
                    : m_scrubber.Configure(input.FramePoolPhysicalStart, input.FramePoolPages)) ||
         !m_objects.ConfigureScrubber(m_scrubber)))
    {
        return false;
    }
    if (mapping && !m_objects.ConfigureMapping(*m_collaborators.Shootdown, *input.Transport, *this))
    {
        return false;
    }
    // Created domains need the mapping to load anything.
    m_domains = mapping && (input.SpaceKeys != nullptr);
    if (m_domains && !ConfigureDomainPools(input))
    {
        return false;
    }
    const bool desktop = input.SessionMode == Abi::SessionDesktop;
    const UInt64 endpoints = desktop ? Abi::DesktopGrantEndpoints : GrantEndpoints;
    const UInt64 iommuDomains = desktop ? Kernel::Device::IommuDomainPool::Capacity : 2U;
    const UInt64 interruptSlots = desktop ? Kernel::Interrupt::InterruptPool::Capacity : 1U;
    const UInt64 pairs = frames ? (desktop ? Abi::DesktopGrantMemoryPairs : GrantMemoryObjects) : 0U;
    // Created threads may wait, with timeouts in the measured ITC rate, once the table's waits are
    // configured.
    const bool waitSets = input.TicksPerMicrosecond != 0U;
    const UInt64 waits = waitSets ? (desktop ? Abi::DesktopGrantWaitSets : GrantWaitSets) : 0U;
    const UInt64 domains = m_domains ? GrantDomains : 0U;
    const UInt64 children = m_domains ? GrantChildDomains : 0U;
    // The assignable devices, edu, the AHCI controller and the NIC, for the pager as Device
    // Manager, where the kernel controls their functions through ECAM and components have domains
    // of their own to drive them from; a driver's Interrupt and the WaitSet it waits on, where its
    // line is routed.
    const bool ahci = input.PublishedDevice(Abi::PagerDeviceAhci).IsPublished();
    const bool device = m_domains && (input.DeviceConfiguration != nullptr) && input.HasPublishedDevices();
    const bool interrupts = m_domains && waitSets && (input.DeviceInterrupts != nullptr);
    const UInt64 allocatable = frames ? (imported ? m_objects.GetAllocatablePages() : MinimumGrantPages) : 0U;
    const Kernel::Resource::ResourceDomainLimits limits{
        .CommittedMemoryPages = allocatable,
        .ResidentMemoryPages = allocatable,
        .KernelMetadataBytes =
            (endpoints * Kernel::Ipc::EndpointPool::MetadataBytes) + (waits * WaitSetMetadataBytes) +
            (pairs * (sizeof(Kernel::Memory::CreatedFrame) + sizeof(Kernel::Memory::MemoryObject))) +
            (GrantThreads * (sizeof(Kernel::Scheduler::Thread) + sizeof(Kernel::Scheduler::SchedulingContext) +
                             Ia64UserThreadTable::ComputeNativeSlotBytes())) +
            (domains * (Memory::Ia64RuntimeAddressSpaceSlots::SlotBytes + SpaceMetadataBytes +
                        sizeof(Kernel::Protection::ProtectionDomain))) +
            (children * DomainMetadataBytes)
            // An xHCI driver's IommuDomain beside the USB host group's, as on AMD64.
            + ((input.Iommu != nullptr) ? (iommuDomains * sizeof(Kernel::Device::IommuDomain)) : 0U) +
            (interrupts ? (interruptSlots * InterruptMetadataBytes + WaitSetMetadataBytes) : 0U),
        .CapabilityHandles = 0U,
        .IpcQueueBytes = endpoints * Kernel::Ipc::Endpoint::MaximumQueueBytes,
        .Threads = GrantThreads,
        .ChildDomains = children,
    };
    if ((m_grant
             .InitializeChild(*m_collaborators.Registry, GrantCharge, m_collaborators.ResourceIdentity,
                              *m_collaborators.Resources, limits)
             .Error != Kernel::Resource::ResourceDomainError::None) ||
        (m_domains && !m_objects.ConfigureResourceDomains(m_ledger, FirstComponentCharge)))
    {
        return false;
    }
    const auto grant = m_collaborators.Issuer->CreateObject(m_grant);
    if (!grant.HasValue() ||
        !MintInto(Abi::PagerBootstrapManagementWord, grant.GetValue(),
                  CapabilityRights{CapabilityRight::Manage}.With(CapabilityRight::Inspect).With(CapabilityRight::Code)))
    {
        return false;
    }
    if ((input.Iommu != nullptr) && !input.Iommu->Configure(m_objects))
    {
        return false;
    }
    if (device)
    {
        m_objects.SetParkedPinObserver(&m_releases);
    }
    if (device && (!m_objects.ConfigureDeviceControl(*input.DeviceConfiguration, *input.DeviceConfiguration) ||
                   !MintDevices(input)))
    {
        return false;
    }
    if (interrupts && !m_objects.ConfigureInterruptLines(*input.DeviceInterrupts))
    {
        return false;
    }
    if (m_domains && input.ScanoutRegion.IsValid() && !MintScanout(input))
    {
        return false;
    }
    m_deviceInterrupts = interrupts ? input.DeviceInterrupts : nullptr;
    m_iommu = input.Iommu;
    // The BSP runtime gives the pager a created-thread table before either boot thread runs.
    if (m_ports.Configure(input.PortSpaceBase, input.PortSpaceBytes))
    {
        m_objects.ConfigurePorts(m_ports);
    }
    PublishI8042(input);
    m_startBlock[Abi::PagerBootstrapClockTicksPerMicrosecondWord] = input.TicksPerMicrosecond;
    m_startBlock[Abi::PagerBootstrapWatchdogSecondsWord] = input.WatchdogSeconds;
    m_startBlock[Abi::PagerBootstrapSessionModeWord] = input.SessionMode;
    m_startBlock[Abi::PagerBootstrapLoginEnabledWord] = input.LoginEnabled;
    if (input.PowerFacts != nullptr)
    {
        m_objects.ConfigurePower(Diagnostics::ConfigureIa64Power(*input.PowerFacts,
                                                                 m_ports.IsConfigured() ? &m_ports : nullptr,
                                                                 input.PowerSalFacts, input.I8042Topology),
                                 grant.GetValue());
    }
    m_startBlock[Abi::PagerBootstrapManagementFeaturesWord] =
        Abi::PagerManagementThreads | (frames ? Abi::PagerManagementFrames : 0U) |
        (mapping ? Abi::PagerManagementMapping : 0U) |
        (m_domains ? (Abi::PagerManagementDomains | Abi::PagerManagementResourceDomains) : 0U) |
        (device ? Abi::PagerManagementDevices : 0U) | (waitSets ? Abi::PagerManagementWaitSets : 0U) |
        ((device && ahci && input.AhciDiskWritable) ? Abi::PagerManagementDiskWritable : 0U);
    if (!mapping)
    {
        return true;
    }
    m_startBlock[Abi::PagerBootstrapWindowStartWord] = Ia64InitialUserMappingWindowStart;
    m_startBlock[Abi::PagerBootstrapWindowPagesWord] = Ia64InitialUserMappingWindowPages;
    return MintInto(Abi::PagerBootstrapOwnSpaceWord, m_pagerSpace.GetObjectId(),
                    CapabilityRights{CapabilityRight::Map});
}

// The root service keeps the region and gives the broker a copy.
bool Ia64PagerRuntime::MintScanout(const Ia64PagerRuntimeInput& input) noexcept
{
    m_startBlock[Abi::PagerBootstrapScanoutModeWord] = input.ScanoutMode[0U];
    m_startBlock[Abi::PagerBootstrapScanoutModeWord + 1U] = input.ScanoutMode[1U];
    return MintInto(Abi::PagerBootstrapScanoutRegionWord, input.ScanoutRegion,
                    CapabilityRights{CapabilityRight::Inspect}
                        .With(CapabilityRight::Map)
                        .With(CapabilityRight::Read)
                        .With(CapabilityRight::Write)
                        .With(CapabilityRight::Duplicate)
                        .With(CapabilityRight::Transfer));
}

bool Ia64PagerRuntime::MintDevices(const Ia64PagerRuntimeInput& input) noexcept
{
    for (Size index{0U}; index < Abi::PagerBootstrapDevices; ++index)
    {
        const auto entry = input.PublishedDevice(index);
        if (entry.IsPublished() && !MintDevice(index, entry.Function, entry.Region))
        {
            return false;
        }
    }
    return true;
}

// The root service, as Device Manager, gives a driver copies of both and keeps its own for the next
// instance.
bool Ia64PagerRuntime::MintDevice(const Size device, const Kernel::Object::ObjectId function,
                                  const Kernel::Object::ObjectId region) noexcept
{
    const CapabilityRights handOn =
        CapabilityRights{CapabilityRight::Inspect}.With(CapabilityRight::Duplicate).With(CapabilityRight::Transfer);
    return MintInto(Abi::PagerBootstrapDeviceFunctionWord(device), function,
                    handOn.With(CapabilityRight::Invoke).With(CapabilityRight::Manage)) &&
           MintInto(Abi::PagerBootstrapDeviceRegionWord(device), region,
                    handOn.With(CapabilityRight::Map).With(CapabilityRight::Read).With(CapabilityRight::Write));
}

bool Ia64PagerRuntime::ConfigureDomainPools(const Ia64PagerRuntimeInput& input) noexcept
{
    Kernel::Object::CapabilityValidationKey keys[Kernel::Object::CapabilitySpacePool::Capacity]{};
    for (Size slot{0U}; slot < Kernel::Object::CapabilitySpacePool::Capacity; ++slot)
    {
        keys[slot] = input.SpaceKeys[slot];
    }
    Kernel::Memory::IAddressSpaceProvider* const provider =
        m_collaborators.RuntimeMemory->ConfigureRuntimeAddressSpaces();
    return (provider != nullptr) &&
           m_objects.ConfigureSpaces(*m_collaborators.Revocations, *m_collaborators.Clock, keys) &&
           m_objects.ConfigureAddressSpaces(*provider) &&
           m_objects.ConfigureFaultChannels(*m_collaborators.Revocations, *m_collaborators.Clock, keys,
                                            m_collaborators.Controller->GetWakeFacade(), *m_collaborators.Shootdown,
                                            *input.Transport, *this);
}

// Once the pager's own objects are registered, the registry must still hold
// the channel's tokens and full object and thread pools (a created Endpoint
// registers itself and its readiness Notification, a WaitSet itself, a created thread its
// Thread and its SchedulingContext, a created domain its AddressSpace,
// CapabilitySpace and ProtectionDomain, a component's ResourceDomain
// itself), which register at run time.
bool Ia64PagerRuntime::CheckRegistryBudget() noexcept
{
    const Size registered = m_collaborators.Registry->GetActiveCount();
    if ((registered + (Kernel::Ipc::EndpointPool::RegisteredObjectsPerSlot * Kernel::Ipc::EndpointPool::Capacity) +
         Kernel::Ipc::WaitSetPool::Capacity + Kernel::Memory::FrameObjectPool::Capacity +
         Kernel::Memory::MemoryObjectPool::Capacity + (2U * Kernel::Scheduler::ThreadPool::Capacity) +
         Kernel::Memory::FaultTokenFactory::MaximumTokens + Kernel::Memory::AddressSpacePool::Capacity +
         Kernel::Memory::FaultChannelPool::MaximumRegisteredObjects + Kernel::Object::CapabilitySpacePool::Capacity +
         Kernel::Protection::ProtectionDomainPool::Capacity + Kernel::Resource::ResourceDomainPool::Capacity +
         Kernel::Device::IommuDomainPool::Capacity + Kernel::Interrupt::InterruptPool::Capacity) >
        m_collaborators.Registry->GetCapacity())
    {
        return false;
    }
    auto& evidence = Diagnostics::GetIa64EvidenceRecord().ObjectManagement;
    evidence.EndpointMetadataBytes = Kernel::Ipc::EndpointPool::MetadataBytes;
    evidence.FrameMetadataBytes = sizeof(Kernel::Memory::CreatedFrame);
    evidence.MemoryObjectMetadataBytes = sizeof(Kernel::Memory::MemoryObject);
    evidence.RegisteredObjects = registered;
    Diagnostics::PublishIa64Evidence(evidence.Published);
    return true;
}

[[gnu::noinline]] void RecordApplicationLaunch(const Kernel::System::ObjectManager& objects) noexcept
{
    auto& evidence = Diagnostics::GetIa64EvidenceRecord().ApplicationLaunch;
    const auto pool = objects.GetFaultChannelSnapshot();
    evidence.Created = pool.Created;
    evidence.Destroyed = pool.Destroyed;
    evidence.Reclaimed = pool.Reclaimed;
    evidence.Live = pool.ActiveSlots + pool.QuiescingSlots;
    evidence.TagRefusals = objects.GetTagRefusals();
    evidence.CodeFlagRefusals = objects.GetCodeFlagRefusals();
    for (Foundation::Size type{0U}; type < Abi::LimitedObjectCount; ++type)
    {
        evidence.TypeLimitRefusals[type] = objects.GetTypeLimitRefusals(type);
    }
    for (Foundation::Size channel{0U}; channel < Kernel::Memory::FaultChannelPool::Capacity; ++channel)
    {
        const auto metrics = objects.GetFaultChannelMetrics(channel);
        evidence.ChannelIdentity[channel] = metrics.Identity;
        evidence.Deliveries[channel] = metrics.Deliveries;
        evidence.Resolutions[channel] = metrics.Resolutions;
        evidence.Terminations[channel] = metrics.Terminations;
        evidence.Busy[channel] = metrics.Busy;
    }
    Diagnostics::PublishIa64Evidence(evidence.Published);
}

void Ia64PagerRuntime::RecordManagement() const noexcept
{
    RecordApplicationLaunch(m_objects);
    const auto pool = m_objects.GetEndpointSnapshot();
    const auto usage = m_grant.GetUsage();
    auto& evidence = Diagnostics::GetIa64EvidenceRecord().ObjectManagement;
    evidence.Created = pool.Created;
    evidence.Destroyed = pool.Destroyed;
    evidence.Reclaimed = pool.Reclaimed;
    evidence.OccupiedSlots = pool.ActiveSlots + pool.QuiescingSlots;
    evidence.MetadataBytes = usage.KernelMetadataBytes;
    evidence.IpcQueueBytes = usage.IpcQueueBytes;
    RecordMemory(m_objects, usage, evidence);
    evidence.TranslationCachePurges = m_collaborators.RuntimeMemory->GetTranslationCachePurgeSnapshot().Purges;
    Diagnostics::PublishIa64Evidence(evidence.Published);
    // Created threads: the pool's and the table's view and the grant's Threads usage. The BSP
    // runtime counts their run stops.
    const auto threads = m_objects.GetThreadSnapshot();
    auto& thread = Diagnostics::GetIa64EvidenceRecord().Threads;
    thread.Created = threads.Created;
    thread.Destroyed = threads.Destroyed;
    thread.Reclaimed = threads.Reclaimed;
    thread.LastExitCode = threads.LastExitCode;
    thread.FreeSlots = threads.FreeSlots;
    thread.ThreadsUsage = usage.Threads;
    auto& components = Diagnostics::GetIa64EvidenceRecord().Components;
    if (m_threads != nullptr)
    {
        RecordThreadTable(*m_threads, thread, components);
    }
    // The initial thread's deferrals: beside the created threads', whose longest run of them the
    // table recorded.
    thread.InitialDeferrals = m_clientDeferralTotal;
    thread.LongestDeferral =
        (m_clientLongestDeferral > thread.LongestDeferral) ? m_clientLongestDeferral : thread.LongestDeferral;
    Diagnostics::PublishIa64Evidence(thread.Published);
    RecordDomains(m_objects, *m_collaborators.RuntimeMemory, m_threads);
    components.ItcTicksPerMicrosecond = m_ticksPerMicrosecond;
    components.WaitExpiries = m_waitExpiries;
    RecordAccounts(m_objects, components);
    Diagnostics::PublishIa64Evidence(components.Published);
    RecordCollection(m_objects);
    RecordInterrupts(m_objects, m_deviceInterrupts);
    RecordBlock(m_objects, m_scrubber);
    if (m_iommu != nullptr)
    {
        m_iommu->Record(m_objects);
    }
}

bool Ia64PagerRuntime::CreateLazyRegion(const Ia64PagerRuntimeInput& input) noexcept
{
    m_startBlock[Abi::PagerBootstrapLazyStartWord] = input.LazyVirtualStart;
    m_startBlock[Abi::PagerBootstrapLazyPagesWord] = input.LazyPages;
    if (input.LazyPages == 0U)
    {
        return true;
    }
    const Kernel::Object::ResourceDomainId charge = m_collaborators.Resources->GetIdentifier();
    const Kernel::Memory::PagePermissions permissions =
        Kernel::Memory::PagePermissions{Kernel::Memory::PagePermission::Read}
            .With(Kernel::Memory::PagePermission::Write)
            .With(Kernel::Memory::PagePermission::User);
    if ((m_lazyFrame.Initialize(charge, {Kernel::Memory::PhysicalAddress{input.LazyPhysicalStart},
                                         Kernel::Memory::PageCount{input.LazyPages},
                                         Kernel::Memory::MemoryCachePolicy::WriteBack,
                                         Kernel::Memory::FrameKind::Ram,
                                         {}}) != Kernel::Memory::FrameError::None) ||
        (m_lazyMemory
             .Initialize(*m_collaborators.Registry, m_collaborators.ResourceIdentity, *m_collaborators.Resources,
                         {Kernel::Memory::PageCount{input.LazyPages},
                          Kernel::Memory::MemoryCachePolicy::WriteBack,
                          permissions,
                          Kernel::Memory::MemoryCommitment::Pinned,
                          {}})
             .Error != Kernel::Memory::MemoryObjectError::None))
    {
        return false;
    }
    const auto frame = m_collaborators.Issuer->CreateObject(m_lazyFrame);
    const auto memory = m_collaborators.Issuer->CreateObject(m_lazyMemory);
    const CapabilityRights map{CapabilityRight::Map};
    return frame.HasValue() && memory.HasValue() &&
           MintInto(Abi::PagerBootstrapLazyMemoryWord, memory.GetValue(), map)
           // The lazy fault installs a writable mapping.
           && MintInto(Abi::PagerBootstrapLazyFrameWord, frame.GetValue(), map.With(CapabilityRight::Write));
}

bool Ia64PagerRuntime::MintInto(const Size word, const Kernel::Object::ObjectId object,
                                const CapabilityRights rights) noexcept
{
    const auto handle =
        m_collaborators.Issuer->Mint(m_space, object, rights, m_root, Kernel::Object::MonotonicTime::Infinite(), 0U);
    if (!handle.HasValue())
    {
        return false;
    }
    m_startBlock[word] = handle.GetValue().GetSelector();
    m_startBlock[word + 1U] = handle.GetValue().GetAuthenticator();
    return true;
}

// The pager image's user mappings for its system calls, and its launch
// request: the code range holding the entry bundle and the fixed memory
// stack and RSE ranges of the user layout, in the pager's own region.
bool Ia64PagerRuntime::CaptureImage(const Ia64PagerRuntimeInput& input) noexcept
{
    const auto summaryResult = input.ImageMemory->GetSummary();
    if (!summaryResult.HasValue())
    {
        return false;
    }
    const Executable::Ia64InitialUserImageMemorySummary& summary = summaryResult.GetValue();
    if ((summary.RangeCount == 0U) || (summary.RangeCount > Kernel::Memory::MaximumDirectUserMemoryMappings))
    {
        return false;
    }
    Scheduler::Ia64UserModeVirtualRange code{};
    Scheduler::Ia64UserModeVirtualRange stack{};
    Scheduler::Ia64UserModeVirtualRange rse{};
    for (Size index{0U}; index < summary.RangeCount; ++index)
    {
        const auto range = input.ImageMemory->BorrowRange(index);
        if (!range.HasValue())
        {
            return false;
        }
        const Executable::Ia64InitialUserImageMemoryRangePlan& plan = range.GetValue().Plan;
        if ((plan.VirtualStart == 0U) || (plan.Pages.GetValue() == 0U) ||
            !plan.Permissions.Contains(Kernel::Memory::PagePermission::Read) ||
            !plan.Permissions.Contains(Kernel::Memory::PagePermission::User))
        {
            return false;
        }
        m_mappings[index] = Kernel::Memory::DirectUserMemoryMapping{
            .UserStart = Kernel::Memory::VirtualAddress{plan.VirtualStart},
            .DirectStart =
                reinterpret_cast<Foundation::UInt8*>(static_cast<Foundation::UIntPointer>(plan.VirtualStart)),
            .Pages = plan.Pages,
            .Permissions = plan.Permissions,
            .Kind = Kernel::Memory::FrameKind::Ram,
        };
        const Scheduler::Ia64UserModeVirtualRange extent{
            .VirtualStart = plan.VirtualStart,
            .SizeBytes = plan.Pages.GetValue() * Ia64UserPageSizeBytes,
        };
        if (plan.Purpose == Executable::Ia64InitialUserImageMemoryPurpose::MemoryStack)
        {
            stack = extent;
        }
        else if (plan.Purpose == Executable::Ia64InitialUserImageMemoryPurpose::RseBackingStore)
        {
            rse = extent;
        }
        else if (RangeContains(plan, summary.Image.EntryBundleAddress))
        {
            code = extent;
        }
    }
    m_mappingCount = summary.RangeCount;
    if ((code.SizeBytes == 0U) || (stack.SizeBytes == 0U) || (rse.SizeBytes == 0U) ||
        (stack.SizeBytes < (Scheduler::Ia64UserModeInitialStackScratchBytes + sizeof(m_startBlock))) ||
        (m_userMemory.Initialize(m_pagerSpace, m_mappings, m_mappingCount) !=
         Kernel::Memory::DirectUserMemoryAccessError::None))
    {
        return false;
    }
    // The block sits just below the initial memory stack pointer; the entry
    // stub moves the stack below it (Ia64PagerImage.S).
    m_startBlockAddress =
        stack.VirtualStart + stack.SizeBytes - Scheduler::Ia64UserModeInitialStackScratchBytes - sizeof(m_startBlock);
    m_launchRequest = Scheduler::Ia64UserModeLaunchRequest{
        .Code = code,
        .MemoryStack = stack,
        .RseBackingStore = rse,
        .EntryBundleAddress = summary.Image.EntryBundleAddress,
        .InitialGlobalPointer = summary.Image.GlobalPointer,
        .InitialArguments = {.Register4 = m_startBlockAddress, .Register5 = 0U, .Register6 = 0U, .Register7 = 0U},
        .Requirements =
            Scheduler::Ia64UserModeLaunchRequirements{
                .CurrentProcessorStatusMask = Scheduler::Ia64UserModeCurrentProcessorStatusMask,
                .CurrentProcessorStatusValue = Scheduler::Ia64UserModeCurrentProcessorStatusValue,
                .UserProcessorStatus = Scheduler::Ia64UserModePreemptibleProcessorStatus,
                .ImplementedVirtualAddressMostSignificantBit =
                    m_collaborators.ImplementedVirtualAddressMostSignificantBit,
                .CurrentPrivilegeLevel = 0U,
                .CurrentVirtualMachineMode = 0U,
                .CurrentInstructionSet = 0U,
                .CurrentRegisterBank = 1U,
                .Reserved = {},
            },
        .SupervisorContinuation = &m_supervisor,
        .Reserved = 0U,
    };
    return true;
}

bool Ia64PagerRuntime::PrepareThread() noexcept
{
    Kernel::Object::ObjectRegistry& registry = *m_collaborators.Registry;
    Kernel::Object::RootCapabilityIssuer& issuer = *m_collaborators.Issuer;
    const Kernel::Object::ResourceDomainId charge = m_collaborators.Resources->GetIdentifier();
    m_protection.SetDiagnosticComponentKind(Abi::WatchdogRootComponent);
    if (m_protection
            .Initialize(registry, m_collaborators.ResourceIdentity, *m_collaborators.Resources, m_pagerSpace, m_space)
            .Error != Kernel::Protection::ProtectionDomainError::None)
    {
        return false;
    }
    const auto protection = issuer.CreateObject(m_protection);
    if (!protection.HasValue() ||
        m_protection.BindIdentity(registry, {registry, protection.GetValue()}).Error !=
            Kernel::Protection::ProtectionDomainError::None ||
        (m_thread.Initialize(charge) != Kernel::Scheduler::ThreadError::None))
    {
        return false;
    }
    const auto thread = issuer.CreateObject(m_thread);
    if (!thread.HasValue())
    {
        return false;
    }
    m_threadId = {registry, thread.GetValue()};
    // The initial thread's time slice.
    const auto binding = m_protection.GetBinding();
    if (!binding.HasValue() || !m_userMemory.BindLifetime(*binding.GetValue().AddressSpaceStorage))
    {
        return false;
    }
    const UInt64 now = m_collaborators.Clock->GetTicks();
    const Kernel::Scheduler::SchedulingContextConfiguration configuration = MakeIa64UserSchedulingConfiguration(
        static_cast<Foundation::UInt8>(Foundation::UInt8{1U} << m_collaborators.Processor.GetValue()));
    if (m_scheduling.Initialize(configuration, charge, Kernel::Object::MonotonicTime{now}) !=
        Kernel::Scheduler::SchedulingContextError::None)
    {
        return false;
    }
    const auto scheduling = issuer.CreateObject(m_scheduling);
    if (!scheduling.HasValue())
    {
        return false;
    }
    m_schedulingId = {registry, scheduling.GetValue()};
    return m_scheduling.Bind(m_threadId) == Kernel::Scheduler::SchedulingContextError::None;
}

// The pager's kernel context runs on its entry storage; its system-call context names this bridge.
bool Ia64PagerRuntime::PrepareContext() noexcept
{
    const auto storage = m_collaborators.RuntimeMemory->GetUserEntryStorage(Memory::Ia64BspRuntimeUserDomain::Pager);
    if (!storage.HasValue())
    {
        return false;
    }
    const Memory::Ia64BspRuntimeUserEntryStorageView& view = storage.GetValue();
    const Scheduler::WritableIa64KernelStorageView stack{
        .Data = view.MemoryStack,
        .SizeBytes = view.MemoryStackSizeBytes,
        .VirtualStart = view.MemoryStackStart,
    };
    const Scheduler::WritableIa64KernelStorageView rse{
        .Data = view.RseBackingStore,
        .SizeBytes = view.RseBackingStoreSizeBytes,
        .VirtualStart = view.RseBackingStoreStart,
    };
#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
    AsteriaIa64InitializePagerEntryDescriptor(m_entryDescriptor);
    const auto entry = reinterpret_cast<Scheduler::Ia64KernelContextEntry>(
        static_cast<Foundation::UIntPointer>(AddressOf(m_entryDescriptor)));
#else
    const Scheduler::Ia64KernelContextEntry entry{&AsteriaIa64PagerRuntimeEntry};
#endif
    if (!m_context.AttachHighFloatingArea(m_highFloating) ||
        (m_context.Prepare(m_collaborators.ImplementedVirtualAddressMostSignificantBit, stack, rse, entry, this) !=
         Scheduler::Ia64KernelContextError::None))
    {
        return false;
    }
    m_supervisor = Scheduler::Ia64UserModeSupervisorContinuation{
        .Registers = {},
        .MemoryStack = {.VirtualStart = view.MemoryStackStart, .SizeBytes = view.MemoryStackSizeBytes},
        .RseBackingStore = {.VirtualStart = view.RseBackingStoreStart, .SizeBytes = view.RseBackingStoreSizeBytes},
        .State = Scheduler::Ia64UserModeSupervisorContinuationState::Fresh,
        .Owner = Scheduler::Ia64UserModeSupervisorContinuationOwner::None,
    };
    if (!Scheduler::Ia64UserModeLaunch::Validate(m_launchRequest).HasValue())
    {
        return false;
    }
    m_bridge = Ia64SystemCallBridgeContext{
        .RunLoop = m_collaborators.RunLoop,
        .UserMemory = &m_userMemory,
        .Scratch = m_collaborators.Scratch,
        .WaitContinuation = nullptr,
        .WaitSetDispatcher = nullptr,
        .Thread = this,
        .PagerResolution = &m_channel,
        .ObjectManagement = &m_objects,
    };
    return m_collaborators.RuntimeMemory->AdmitPagerSystemCallEntryContext(m_collaborators.Processor,
                                                                           AddressOf(&m_bridge)) ==
           Memory::Ia64BspRuntimeSystemCallPublicationError::None;
}

bool Ia64PagerRuntime::Admit(Kernel::Scheduler::ProcessorKernelRunLoopFailure& failure) noexcept
{
    return m_initialized && (m_thread.MakeReady() == Kernel::Scheduler::ThreadError::None) &&
           (m_collaborators.RunLoop->Bind(m_thread, m_context, m_protection) ==
            Kernel::Scheduler::ProcessorKernelRunLoopError::None) &&
           m_collaborators.RunLoop->EnqueueBoundThread(m_schedulingId, m_scheduling, m_threadId, m_thread, failure);
}

void Ia64PagerRuntime::Launch() noexcept
{
#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
    // The run loop selected the pager's root before entering this context,
    // so the block is written through the pager's own stack mapping.
    auto* const words = reinterpret_cast<UInt64*>(static_cast<Foundation::UIntPointer>(m_startBlockAddress));
    for (Size index{0U}; index < Abi::PagerBootstrapWords; ++index)
    {
        words[index] = m_startBlock[index];
    }
#endif
    if (!m_collaborators.RuntimeMemory->SelectUserEntryContexts(m_collaborators.Processor,
                                                                Memory::Ia64BspRuntimeUserDomain::Pager))
    {
        m_error = Ia64PagerRuntimeError::EntryContextRejected;
        return;
    }
    static_cast<void>(Scheduler::Ia64UserModeLaunch::Launch(m_launchRequest));
    m_error = Ia64PagerRuntimeError::LaunchRequestRejected;
}

bool Ia64PagerRuntime::AttachClient(const Ia64PagerClient& client) noexcept
{
    if (!m_initialized || (m_client.Thread != nullptr) || (client.Thread == nullptr) || (client.Context == nullptr) ||
        !client.ThreadIdentity.IsValid() || !client.SchedulingContextIdentity.IsValid())
    {
        return false;
    }
    m_client = client;
    const Interrupt::IIa64UserFaultHandler* const clientHandler = this;
    const Interrupt::IIa64UserFaultHandler* const pagerHandler = &m_pagerFaults;
    return (m_collaborators.RuntimeMemory->AdmitUserFaultEntryContext(
                m_collaborators.Processor, Memory::Ia64BspRuntimeUserDomain::InitialComponent,
                AddressOf(clientHandler)) == Memory::Ia64RuntimeInterruptVectorTablePublicationError::None) &&
           (m_collaborators.RuntimeMemory->AdmitUserFaultEntryContext(
                m_collaborators.Processor, Memory::Ia64BspRuntimeUserDomain::Pager, AddressOf(pagerHandler)) ==
            Memory::Ia64RuntimeInterruptVectorTablePublicationError::None);
}

Interrupt::Ia64UserFaultDisposition Ia64PagerRuntime::HandleUserFault(Interrupt::Ia64PageFaultFrame&,
                                                                      const Kernel::Memory::FaultRecord& fault) noexcept
{
    if (!m_initialized || (m_client.Thread == nullptr))
    {
        return Interrupt::Ia64UserFaultDisposition::FailStop;
    }
    const Kernel::Memory::FaultDelivery delivery =
        DeliverFault(*m_client.Thread, *m_client.Context,
                     Kernel::Memory::PagerFaultSource{
                         .Thread = m_client.ThreadIdentity,
                         .AddressSpace = m_collaborators.ClientSpace,
                         .SchedulingContext = m_client.SchedulingContextIdentity,
                         .Fault = fault,
                         .SupervisionTag = 0U,
                         .ProtectionDomainIdentity = m_client.ProtectionDomainIdentity,
                     });
    bool resumed{false};
    switch (Kernel::Memory::DecideFaultOutcome(delivery, m_clientDeferrals))
    {
    case Kernel::Memory::FaultOutcome::Resume:
        // The pager selected its own entry contexts meanwhile.
        resumed = m_collaborators.RuntimeMemory->SelectUserEntryContexts(
            m_collaborators.Processor, Memory::Ia64BspRuntimeUserDomain::InitialComponent);
        break;
    case Kernel::Memory::FaultOutcome::Yield:
        // Behind another fault: the client yields and faults again when it next runs, however long
        // that takes.
        ++m_clientDeferralTotal;
        m_clientLongestDeferral =
            (m_clientDeferrals > m_clientLongestDeferral) ? m_clientDeferrals : m_clientLongestDeferral;
        resumed = Progress() && (m_collaborators.Preemption != nullptr) &&
                  m_collaborators.Preemption->YieldUser(Memory::Ia64BspRuntimeUserDomain::InitialComponent);
        break;
    case Kernel::Memory::FaultOutcome::Stop:
        break;
    }
    return resumed ? Interrupt::Ia64UserFaultDisposition::Resume : Interrupt::Ia64UserFaultDisposition::FailStop;
}

Kernel::Memory::FaultDelivery Ia64PagerRuntime::DeliverFault(Kernel::Scheduler::Thread& thread,
                                                             Scheduler::Ia64KernelContext& context,
                                                             const Kernel::Memory::PagerFaultSource& source) noexcept
{
    if (!m_initialized)
    {
        return Kernel::Memory::FaultDelivery::Failed;
    }
    const Kernel::Memory::PagerChannelError prepared = m_channel.PrepareFault(source);
    if (prepared != Kernel::Memory::PagerChannelError::None)
    {
        return (prepared == Kernel::Memory::PagerChannelError::Busy) ? Kernel::Memory::FaultDelivery::Busy
                                                                     : Kernel::Memory::FaultDelivery::Failed;
    }
    // The thread runs again only once the pager's resolution woke it. The
    // channel holds one fault, so one thread at a time uses this receipt.
    return (m_collaborators.RunLoop->FinishFaultAndDispatch(thread, context, m_channel, m_faultReceipt,
                                                            m_faultSwitchFailure) &&
            (m_faultReceipt.ThreadStorage == &thread) &&
            (m_faultReceipt.State == Kernel::Scheduler::ProcessorKernelRunLoopState::Active))
               ? Kernel::Memory::FaultDelivery::Resolved
               : Kernel::Memory::FaultDelivery::Failed;
}

bool Ia64PagerRuntime::Progress() noexcept
{
    return ProgressFromIdle() != Ia64PagerIdleProgress::Failed;
}

void Ia64PagerRuntime::OnExit() noexcept
{
    // A signal is coalesced with any pending one; Signal fails only before
    // Initialize, when no thread can exist.
    static_cast<void>(m_readiness.Signal());
}

Ia64UserDomainRecord Ia64PagerRuntime::GetDomainRecord() noexcept
{
    return Ia64UserDomainRecord{
        .Protection = &m_protection,
        .UserMemory = &m_userMemory,
        .ObjectManagement = &m_objects,
        .PagerResolution = &m_channel,
        .Supervisor = this,
        .SupervisionTag = 0U,
    };
}

bool Ia64PagerRuntime::AttachThreads(Ia64UserThreadTable& threads) noexcept
{
    // The records of the domains the pager creates are the table's; their faults and exits come to
    // the pager.
    if (!m_initialized || (m_threads != nullptr) || !m_objects.ConfigureThreads(threads) ||
        (m_domains && (!threads.ConfigureCreatedDomains(*this, m_objects) || !m_objects.ConfigureDomains(threads))) ||
        ((m_ticksPerMicrosecond != 0U) && !threads.ConfigureWaits(m_ticksPerMicrosecond)))
    {
        return false;
    }
    m_threads = &threads;
    return true;
}

Interrupt::Ia64UserFaultDisposition Ia64PagerRuntime::HandleUserInterrupt(Interrupt::Ia64PageFaultFrame&) noexcept
{
    return m_initialized && (m_collaborators.Preemption != nullptr) &&
                   m_collaborators.Preemption->PreemptUser(Memory::Ia64BspRuntimeUserDomain::InitialComponent)
               ? Interrupt::Ia64UserFaultDisposition::Resume
               : Interrupt::Ia64UserFaultDisposition::FailStop;
}

bool Ia64PagerRuntime::Preempt() noexcept
{
    return m_collaborators.RunLoop->FinishAndDispatch(m_thread, m_context,
                                                      Kernel::Scheduler::ThreadRunDisposition::Ready, 0U,
                                                      m_preemptReceipt, m_preemptFailure) &&
           (m_preemptReceipt.ThreadStorage == &m_thread) &&
           (m_preemptReceipt.State == Kernel::Scheduler::ProcessorKernelRunLoopState::Active);
}

bool Ia64PagerRuntime::RetryDelivery() noexcept
{
    const auto channel = m_channel.GetSnapshot();
    if ((channel.State == Kernel::Memory::PagerChannelState::FaultPending) &&
        (channel.Ingress.State != Kernel::Memory::PageFaultIngressState::Delivered))
    {
        m_channel.Retry();
    }
    return m_objects.ProgressFaultChannels() &&
           m_channel.GetSnapshot().State != Kernel::Memory::PagerChannelState::Faulted;
}

Ia64PagerIdleProgress Ia64PagerRuntime::ProgressFromIdle() noexcept
{
    if (!m_initialized || !RetryDelivery())
    {
        return Ia64PagerIdleProgress::Failed;
    }
    // The created threads' waits progress at the pager's scheduling points.
    const Ia64ThreadWaitProgress threads =
        (m_threads != nullptr) ? m_threads->ProgressWaits() : Ia64ThreadWaitProgress::Waiting;
    if (threads == Ia64ThreadWaitProgress::Failed)
    {
        return Ia64PagerIdleProgress::Failed;
    }
    const Ia64PagerIdleProgress own = ProgressOwnWait();
    return ((own == Ia64PagerIdleProgress::Waiting) && (threads == Ia64ThreadWaitProgress::Woken))
               ? Ia64PagerIdleProgress::Woken
               : own;
}

UInt64 Ia64PagerRuntime::GetWaitDeadline() const noexcept
{
    const UInt64 own =
        (m_wait.GetSnapshot().State == Kernel::Ipc::WaitSetThreadDispatcherState::Blocked) ? m_wait.GetDeadline() : 0U;
    const UInt64 threads = (m_threads != nullptr) ? m_threads->GetWaitDeadline() : 0U;
    return ((own == 0U) || ((threads != 0U) && (threads < own))) ? threads : own;
}

Ia64PagerIdleProgress Ia64PagerRuntime::ProgressOwnWait() noexcept
{
    const auto state = m_wait.GetSnapshot().State;
    if ((state != Kernel::Ipc::WaitSetThreadDispatcherState::Blocked) &&
        (state != Kernel::Ipc::WaitSetThreadDispatcherState::QueuePressure))
    {
        return Ia64PagerIdleProgress::Waiting;
    }
    const auto polled = m_wait.Poll();
    if (!polled.HasValue())
    {
        return Ia64PagerIdleProgress::Failed;
    }
    return (polled.GetValue().State == Kernel::Ipc::WaitSetThreadDispatcherState::Completed)
               ? Ia64PagerIdleProgress::Woken
               : ExpireWait();
}

// Nothing ready: a passed deadline wakes the pager timed out; queue pressure
// leaves the wait for the next scheduling point.
Ia64PagerIdleProgress Ia64PagerRuntime::ExpireWait() noexcept
{
    const auto expired = m_wait.Expire(m_collaborators.Clock->GetTicks());
    if (!expired.HasValue())
    {
        return (m_wait.GetSnapshot().State == Kernel::Ipc::WaitSetThreadDispatcherState::Faulted)
                   ? Ia64PagerIdleProgress::Failed
                   : Ia64PagerIdleProgress::Waiting;
    }
    if (expired.GetValue().State != Kernel::Ipc::WaitSetThreadDispatcherState::Completed)
    {
        return Ia64PagerIdleProgress::Waiting;
    }
    ++m_waitExpiries;
    return Ia64PagerIdleProgress::Woken;
}

bool Ia64PagerRuntime::DispatchExit(const Kernel::System::SystemCallRequest&,
                                    Kernel::System::SystemCallResponse& response) noexcept
{
    response = Kernel::System::SystemCallResponse{
        .Error = Kernel::System::SystemCallError::UnknownSystemCall, .Value0 = 0U, .Value1 = 0U};
    return true;
}

bool Ia64PagerRuntime::PrepareManagement() noexcept
{
    return (m_threads == nullptr) || m_threads->Reap();
}

bool Ia64PagerRuntime::DispatchWait(const Kernel::System::SystemCallRequest& request,
                                    Kernel::System::SystemCallResponse& response) noexcept
{
    // The pager's management calls all precede its waits (its self-test runs
    // before it serves), so each wait records where they left the pool.
    RecordManagement();
    Scheduler::Ia64ProcessorRunLoop& loop = *m_collaborators.RunLoop;
    if (!Kernel::System::WaitSetSystemCallIngress::Dispatch(loop, m_waitDispatcher, request, m_result, m_waitFailure))
    {
        return false;
    }
    if (m_result.Disposition == Kernel::System::WaitSetSystemCallDisposition::SuspendPrepared)
    {
        // The pager runs again only once its wait completed; another thread
        // selected its own entry contexts meanwhile.
        if (!loop.FinishWaitAndDispatch(m_thread, m_context, m_wait, m_waitReceipt, m_waitSwitchFailure) ||
            (m_waitReceipt.ThreadStorage != &m_thread) ||
            (m_waitReceipt.State != Kernel::Scheduler::ProcessorKernelRunLoopState::Active) ||
            !m_collaborators.RuntimeMemory->SelectUserEntryContexts(m_collaborators.Processor,
                                                                    Memory::Ia64BspRuntimeUserDomain::Pager) ||
            !Kernel::System::WaitSetSystemCallIngress::ConsumeCompletion(loop, m_waitDispatcher, m_result,
                                                                         m_waitFailure))
        {
            return false;
        }
    }
    response = m_result.Response;
    return true;
}
} // Namespace Asteria::Platform::Ia64::System

extern "C" [[noreturn]] ASTERIA_NOINLINE_NOIPA void AsteriaIa64PagerRuntimeEntry(void* const argument) noexcept
{
    if (argument != nullptr)
    {
        static_cast<Asteria::Platform::Ia64::System::Ia64PagerRuntime*>(argument)->Launch();
    }
    Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:IA64-PAGER:LAUNCH:FAIL\r\n");
    Asteria::Platform::Firmware::HaltAfterFirmwareExit();
}
