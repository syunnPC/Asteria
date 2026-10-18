// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Boot/Amd64BootRuntime.hpp"

#include "Platform/Amd64/Boot/I8042Gate.hpp"
#include "Platform/Amd64/Boot/Amd64MetadataArena.hpp"
#include "Boot/Acpi/I8042Evidence.hpp"
#include "Boot/Uefi/BootReport.hpp"
#include "Kernel/Boot/BootInventory.hpp"
#include "Kernel/Scheduler/CrossProcessorWait.hpp"
#include "Platform/Amd64/Memory/Amd64X2ApicTlbShootdownNotifier.hpp"

#include "Boot/BootModuleMapping.hpp"

#include "Abi/PagerBootstrap.hpp"
#include "Kernel/Diagnostics/DiagnosticChannel.hpp"
#include "Kernel/Ipc/Endpoint.hpp"
#include "Kernel/Memory/AddressSpace.hpp"
#include "Kernel/Memory/DirectUserMemoryAccess.hpp"
#include "Kernel/Memory/PinnedFrameMapping.hpp"
#include "Kernel/Object/CapabilityKeyDerivation.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Kernel/System/ObjectManager.hpp"
#include "Kernel/Memory/PhysicalPageImporter.hpp"
#include "Kernel/Device/FirmwareBusMasterGate.hpp"
#include "Platform/Amd64/Device/Amd64PortIo.hpp"
#include "Platform/Amd64/Device/Amd64VtdHardware.hpp"
#include "Platform/Device/PciEcamRuntime.hpp"
#include "Platform/Device/VtdDomainBackend.hpp"
#include "Platform/Device/VtdDomainGroup.hpp"
#include "Platform/Device/VtdTopology.hpp"
#include "Platform/Device/VtdUnit.hpp"
#include "Platform/Device/TrustedDmaRuntime.hpp"
#include "Platform/Device/FirmwareDmaReservations.hpp"
#include "Platform/Amd64/Device/AmdViUnit.hpp"
#include "Platform/Amd64/Device/AmdViDomainBackend.hpp"
#include "Platform/Amd64/Device/AmdViTopology.hpp"
#include "Kernel/Device/PciFunctionControl.hpp"
#include "Platform/Amd64/Interrupt/Amd64DeviceVectors.hpp"
#include "Platform/Amd64/Interrupt/Amd64InterruptLines.hpp"
#include "Platform/Device/PortPciConfigurationSpace.hpp"
#include "Kernel/System/SystemCallIngress.hpp"
#include "Kernel/System/SystemCallScratch.hpp"
#include "Kernel/Time/TimerQueue.hpp"
#include "Platform/Amd64/Boot/Amd64ProcessorTopology.hpp"
#include "Platform/Amd64/Diagnostics/Amd64EvidenceRecord.hpp"
#include "Platform/Amd64/Executable/Amd64InitialUserImage.hpp"
#include "Platform/Amd64/Device/Amd64BootstrapDevices.hpp"
#include "Platform/Amd64/Device/Amd64MmioWindow.hpp"
#include "Platform/Amd64/Interrupt/Amd64UserEntry.hpp"
#include "Platform/Amd64/Interrupt/Amd64UserException.hpp"
#include "Platform/Amd64/Interrupt/Amd64ApplicationProcessorStartup.hpp"
#include "Platform/Amd64/Memory/RuntimeAddressSpacePageTable.hpp"
#include "Platform/Amd64/Memory/LargePageTableArenaPool.hpp"
#include "Platform/Amd64/Memory/Amd64PageFaultDecoder.hpp"
#include "Platform/Amd64/Memory/Amd64FrameScrubber.hpp"
#include "Platform/Amd64/Memory/Amd64LocalTlbInvalidator.hpp"
#include "Platform/Amd64/Scheduler/Amd64ProcessorRunLoop.hpp"
#include "Platform/Amd64/Scheduler/Amd64ApplicationProcessorRuntime.hpp"
#include "Platform/Amd64/Scheduler/Amd64NativePreemptionProbe.hpp"
#include "Platform/Amd64/System/Amd64SystemCallRegisterCodec.hpp"
#include "Platform/Amd64/System/Amd64PagerRuntime.hpp"
#include "Platform/Amd64/System/Amd64SpinnerRuntime.hpp"
#include "Platform/Amd64/System/Amd64UserThreadTable.hpp"
#include "Platform/Amd64/System/Amd64WaitRuntime.hpp"
#include "Platform/Amd64/Time/Amd64ApicTimer.hpp"
#include "Platform/Amd64/Time/Amd64TscClock.hpp"
#include "Platform/Firmware/Diagnostic.hpp"
#include "Platform/Firmware/DiagnosticLineSink.hpp"
#include "Runtime/Memory.hpp"
#include "Platform/Amd64/Diagnostics/Amd64Stop.hpp"
#include "Platform/Amd64/Diagnostics/Amd64StopStage.hpp"

#if !defined(ASTERIA_AMD64_NATIVE_PREEMPTION_PROBE)
#define ASTERIA_AMD64_NATIVE_PREEMPTION_PROBE 0
#endif
#if ASTERIA_AMD64_NATIVE_PREEMPTION_PROBE != 0 && ASTERIA_AMD64_NATIVE_PREEMPTION_PROBE != 1
#error "ASTERIA_AMD64_NATIVE_PREEMPTION_PROBE must be zero or one"
#endif

#if defined(ASTERIA_TARGET_AMD64)
void* operator new(Asteria::Foundation::Size, void*) noexcept;
#else
#include <new>
#endif

#if defined(ASTERIA_TARGET_AMD64)
extern "C" Asteria::Foundation::UInt8 AsteriaAmd64ApInterruptWindowBegin[];
extern "C" Asteria::Foundation::UInt8 AsteriaAmd64ApInterruptWindowEnd[];
#endif

namespace Asteria::Platform::Amd64::Bootstrap
{
namespace
{
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt64;
using Kernel::Memory::AddressSpace;
using Kernel::Memory::AddressSpaceId;
using Kernel::Memory::AddressSpaceError;
using Kernel::Memory::InitialMapping;
using Kernel::Memory::InitialMappingOwner;
using Kernel::Memory::InitialMappingPlan;
using Kernel::Memory::InitialMappingPlanError;
using Kernel::Memory::MappingPermission;
using Kernel::Memory::MappingPermissions;
using Kernel::Memory::MemoryCachePolicy;
using Kernel::Memory::PageCount;
using Kernel::Memory::PagePermission;
using Kernel::Memory::PhysicalAddress;
using Kernel::Memory::VirtualAddress;
using Kernel::Object::ResourceDomainId;
using Kernel::Scheduler::CpuId;
using Kernel::Memory::TlbShootdownDispatcher;
using Kernel::Memory::TlbShootdownDrain;
constexpr ResourceDomainId BootstrapCharge{1U};
// The boot pager's management grant: a child of the boot domain whose limits fit this many
// Endpoints of the largest queue, WaitSets, Frames and MemoryObjects, and pages of created memory.
// The object counts allow five components at once: display, input broker,
// PS/2, xHCI and the USB host group, each a service with a WaitSet of its own.
constexpr ResourceDomainId PagerGrantCharge{2U};
// Watchdog.md: three root-owned private liveness reply Endpoints.
constexpr UInt64 PagerGrantEndpoints{14U + Abi::WatchdogServiceCount};
// The three root-owned watchdog replies also fit the grant's inherited per-type Endpoint limit.
static_assert(PagerGrantEndpoints <= Kernel::Ipc::EndpointPool::Capacity);
constexpr UInt64 PagerGrantWaitSets{5U};
static_assert(Abi::DesktopGrantEndpoints <= Kernel::Ipc::EndpointPool::Capacity);
static_assert(Abi::DesktopGrantWaitSets <= Kernel::Ipc::WaitSetPool::Capacity);
// Root-owned driver/ports probes run sequentially; PS/2 Interrupts belong to its child domain.
constexpr UInt64 PagerGrantInterrupts{3U};
// The common component image includes broker state/code. CheckPagerGrant.py derives
// sequential ports/input peaks from linked ELF segments. Input retains all images and runs four
// component incarnations (display, broker, probe, PS/2 or xHCI). Product USB-only init may keep two
// xHCI owners, each with 139 explicit DMA/state pages, and product init runs display, broker, PS/2,
// one xHCI and the USB host group at once (five components, the xHCI's scratchpads within the 128
// the two-controller peak assumes; CheckPagerGrant.py, scenario `five components`). The display
// broker lives in DISPLAY.ELF, so the console grid is charged once. CheckPagerGrant.py derives
// every peak from the linked images; report-protocol HID raises the minimum to 528 pages.
constexpr UInt64 PagerMinimumPages{528U};
// The extra display shared segments bring the five-component case to all 32 Frame/MemoryObject
// pairs budgeted by this grant. Further growth needs a grant adjustment even with physical memory
// management's larger object pools.
constexpr UInt64 PagerGrantMemoryObjects{32U};
static_assert(PagerGrantMemoryObjects <= Kernel::Memory::FrameObjectPool::Capacity);
static_assert(PagerGrantMemoryObjects <= Kernel::Memory::MemoryObjectPool::Capacity);
static_assert(Abi::DesktopGrantMemoryPairs <= Kernel::Memory::FrameObjectPool::Capacity);
static_assert(Abi::DesktopGrantMemoryPairs <= Kernel::Memory::MemoryObjectPool::Capacity);

// The threads the pager may have alive at once, one per component slot.
constexpr UInt64 PagerGrantThreads{Kernel::Scheduler::ThreadPool::Capacity};
// The domains the root service may have at once: each an AddressSpace on a reserved arena, a
// CapabilitySpace and a ProtectionDomain.
constexpr UInt64 PagerGrantDomains{Kernel::Memory::AddressSpacePool::Capacity};
// The IommuDomains alive at once: an xHCI driver's beside the USB host group's.
constexpr UInt64 PagerGrantIommuDomains{2U};
// Each component's ResourceDomain, a child of the grant, and the identity the first one takes,
// after the boot domain's and the grant's.
constexpr UInt64 PagerGrantChildDomains{Kernel::Resource::ResourceDomainPool::Capacity};
constexpr ResourceDomainId FirstComponentCharge{3U};
// Evaluate reservation values without O0 temporaries.
constexpr UInt64 WaitSetMetadataBytes{Kernel::Ipc::WaitSetPool::GetReservation().KernelMetadataBytes};
constexpr UInt64 CapabilitySpaceMetadataBytes{
    Kernel::Object::CapabilitySpacePool::GetReservation().KernelMetadataBytes};
constexpr UInt64 ResourceDomainMetadataBytes{
    Kernel::Resource::ResourceDomainPool::GetReservation().KernelMetadataBytes};
constexpr UInt64 InterruptMetadataBytes{Kernel::Interrupt::InterruptPool::GetReservation().KernelMetadataBytes};
// The revocation roots of the boot's registry: the boot space's and the diagnostic's, the wait
// fixture's, the pager's space and fault tokens, and the spinner's. With one per assignable device
// and one per FaultChannel they fit a registry's 64 domains.
constexpr Size BootRevocationRoots{6U};
static_assert(BootRevocationRoots + Platform::Device::PciEcamRevocationRoots +
                  Platform::Device::FaultChannelRevocationRoots <=
              Kernel::Object::RevocationRegistry::MaximumDomains);

// Bounded waits of the BSP for an application processor: 200 polls 10 ms apart.
// Times the idle loop arms one suspended wait's timer again because it fell
// due before its deadline could be programmed (ContinueUser).
constexpr Size WaitRearmAttempts{8U};
// Late deadlines the idle loop retries in a row while no timer interrupt
// shows that a thread ran (ContinueUser).
constexpr Size LateDispatchAttempts{64U};
// The staged user images, each in its own root: the initial image, the boot
// pager and, in the test firmware, the spinner.
constexpr Size ClientImage{0U};
constexpr Size PagerImage{1U};
constexpr Size SpinnerImage{2U};
constexpr Size UserImages{3U};
constexpr Size ImageRoots[UserImages]{UserRootIndex, PagerRootIndex, SpinnerRootIndex};
constexpr Size ImageStagingPages[UserImages]{UserStagingFirstPage, PagerStagingFirstPage, SpinnerStagingFirstPage};
static_assert(LazyRegionPages == Executable::InitialUserLazyPages);
// The #PF vector.
constexpr UInt64 PageFaultVector{14U};
// Idle waits at most this long for a replenishment tick when every runnable
// thread has spent its budget.
constexpr UInt64 IdleTickTimeoutTicks{1'000'000'000U};
constexpr unsigned TimerQueueDeadlineSource{static_cast<unsigned>(Kernel::Time::ProcessorDeadlineSource::TimerQueue)};
using ApplicationProcessor = Scheduler::Amd64ApplicationProcessorRuntime;
alignas(ApplicationProcessor) UInt8 g_applicationProcessorStorage[3U][sizeof(ApplicationProcessor)]{};
extern "C" [[noreturn]] void AsteriaAmd64ApplicationProcessorEntry(void*, UInt64) noexcept;

// Intel 64 and IA-32 SDM, Vol. 3A, 253668-093US, chapter 11 (local APIC):
// fixed physical destination IPIs.
inline constexpr Foundation::UInt8 ShootdownVector{0xF0U};
inline constexpr Foundation::UInt8 RescheduleVector{0xF1U};
class IpiNotifier final : public Hal::Memory::ITlbShootdownNotifier
{
public:
    [[nodiscard]] bool Initialize(const Foundation::ProcessorMask mask,
                                  const Foundation::UInt32 (&ids)[Kernel::Scheduler::MaximumProcessorCount]) noexcept
    {
        if (!m_apic.InitializeIdentity())
        {
            return false;
        }
        m_mask = mask;
        for (Size cpu{0U}; cpu < Kernel::Scheduler::MaximumProcessorCount; ++cpu)
        {
            m_ids[cpu] = ids[cpu];
        }
        return !m_apic.IsX2Apic() || m_x2.Initialize(ShootdownVector, mask, ids) ==
                                         Memory::Amd64X2ApicTlbShootdownNotifierInitializationError::None;
    }
    void Notify(const Foundation::ProcessorMask targets) noexcept override
    {
        if ((targets & ~m_mask) != 0U)
        {
            Kernel::Scheduler::CrossProcessorWait::Timeout(targets);
            return;
        }
        auto& evidence = Diagnostics::GetAmd64EvidenceRecord().Smp;
        for (Size cpu{0U}; cpu < Kernel::Scheduler::MaximumProcessorCount; ++cpu)
        {
            const Foundation::ProcessorMask bit = Foundation::ProcessorMask{1U} << cpu;
            if ((targets & bit) == 0U)
            {
                continue;
            }
            static_cast<void>(__atomic_fetch_add(&evidence.ShootdownSent[cpu], 1U, __ATOMIC_RELEASE));
            if (m_apic.IsX2Apic())
            {
                m_x2.Notify(bit);
            }
            else if (!m_apic.Send(m_ids[cpu], ShootdownVector))
            {
                Kernel::Scheduler::CrossProcessorWait::Timeout(bit);
            }
        }
    }
    void Kick(const Kernel::Scheduler::CpuId home) noexcept
    {
        const auto bit = Foundation::ProcessorMask{1U} << home.GetValue();
        if ((m_mask & bit) == 0U || !m_apic.Send(m_ids[home.GetValue()], RescheduleVector))
        {
            Kernel::Scheduler::CrossProcessorWait::Timeout(bit);
        }
    }

private:
    Memory::Amd64X2ApicTlbShootdownNotifier m_x2{};
    Interrupt::Amd64LocalApic m_apic{};
    Foundation::UInt32 m_ids[Kernel::Scheduler::MaximumProcessorCount]{};
    Foundation::ProcessorMask m_mask{};
};
class MappingProgress final : public Kernel::Memory::IPagerShootdownProgress
{
public:
    void Wait() noexcept override
    {
        Platform::Synchronization::ProcessorWaitProgress();
    }
};

[[nodiscard]] UInt64 PoolPage(const UInt64 poolPhysicalStart, const Size page) noexcept
{
    return RuntimePoolPageAddress(poolPhysicalStart, page);
}

/// The physical address of metadata-arena page `page`; the arena lies below 1 GiB, so the sum
/// cannot overflow.
[[nodiscard]] UInt64 ArenaPage(const UInt64 arenaPhysicalStart, const Size page) noexcept
{
    return arenaPhysicalStart + (static_cast<UInt64>(page) * RuntimePoolPageBytes);
}

// The roots of the AddressSpaces user code creates: each reserved arena is prepared from the boot
// seed like a boot root when ObjectCreate claims its slot, and its AddressSpace is initialized
// charged to the creator's ResourceDomain. Releasing a quiesced slot ends its space, page table and
// Preparation, so the next creation prepares the arena again.
class RuntimeRoots final : public Kernel::Memory::IAddressSpaceProvider
{
public:
    static constexpr Size Slots{Kernel::Memory::AddressSpacePool::Capacity};

    /// Keeps what every arena is prepared from, and the scrubber each
    /// prepared root is published to, and builds the slots in the metadata
    /// arena; once, before any creation. seed and scrubber outlive the
    /// provider.
    [[nodiscard]] bool Configure(const InitialMappingPlan& seed, const Memory::PagingCapabilities& paging,
                                 const UInt64 poolPhysicalStart, const UInt64 arenaPhysicalStart,
                                 Memory::Amd64FrameScrubber& scrubber) noexcept
    {
        if (m_seed != nullptr)
        {
            return false;
        }
        m_seed = &seed;
        m_paging = paging;
        m_poolPhysicalStart = poolPhysicalStart;
        m_arenaPhysicalStart = arenaPhysicalStart;
        m_scrubber = &scrubber;
        m_slots = reinterpret_cast<Amd64RuntimeRootSlot*>(
            static_cast<Foundation::UIntPointer>(ArenaPage(arenaPhysicalStart, MetadataArenaRootSlotFirstPage)));
        for (Size slot{0U}; slot < Slots; ++slot)
        {
            ::new (static_cast<void*>(&m_slots[slot])) Amd64RuntimeRootSlot{};
            m_largeForSlot[slot] = LargeRootArenaCount;
        }
        return true;
    }

    [[nodiscard]] Size GetSlotCount() const noexcept override
    {
        return (m_seed != nullptr) ? Slots : 0U;
    }

    [[nodiscard]] UInt64 GetSlotBytes(const bool largeArena = false) const noexcept override
    {
        return sizeof(AddressSpace) + ((largeArena ? LargeRootArenaPages : RootArenaPages) * RuntimePoolPageBytes);
    }

    [[nodiscard]] bool HasAvailableArena(const bool largeArena) const noexcept override
    {
        if (!largeArena)
        {
            return true;
        }
        return m_largeArenas.HasAvailable();
    }

    [[nodiscard]] AddressSpace* InitializeSlot(const Size slot, const ResourceDomainId chargeOwner,
                                               const bool largeArena = false) noexcept override
    {
        if ((m_seed == nullptr) || (slot >= Slots) || m_slots[slot].Held || !HasAvailableArena(largeArena))
        {
            return nullptr;
        }
        // AddressSpacePool serializes selection and release. A failed preparation spends its
        // storage just like a slot.
        if (largeArena)
        {
            m_largeForSlot[slot] = m_largeArenas.TryAcquire();
            if (m_largeForSlot[slot] == LargeRootArenaCount)
            {
                return nullptr;
            }
        }
        if (!PrepareSlot(slot) ||
            (m_slots[slot].Space.Initialize(chargeOwner, m_slots[slot].PageTable, *m_slots[slot].Plan) !=
             AddressSpaceError::None) ||
            !m_scrubber->PublishRoot(BootRootCount + slot, m_slots[slot].Space, ArenaPhysical(slot)))
        {
            return nullptr;
        }
        m_slots[slot].Held = true;
        __atomic_fetch_add(&m_prepared, 1U, __ATOMIC_RELAXED);
        return &m_slots[slot].Space;
    }

    [[nodiscard]] bool ReleaseSlot(const Size slot) noexcept override
    {
        if ((slot >= Slots) || (m_slots == nullptr) || !m_slots[slot].Held ||
            (m_slots[slot].Space.GetSnapshot().State != Kernel::Memory::AddressSpaceState::Quiesced))
        {
            return false;
        }
        // The space quiesced and its page table retired, so nothing reads the
        // arena any more: a fresh preparation may own it. The scrubber
        // forgets the root first.
        m_scrubber->WithdrawRoot(BootRootCount + slot);
        if (m_largeForSlot[slot] < LargeRootArenaCount)
        {
            if (!m_largeArenas.Release(m_largeForSlot[slot]))
            {
                return false;
            }
            m_largeForSlot[slot] = LargeRootArenaCount;
        }
        m_slots[slot].~Amd64RuntimeRootSlot();
        ::new (static_cast<void*>(&m_slots[slot])) Amd64RuntimeRootSlot{};
        return true;
    }

    /// The arenas prepared so far, the first preparation of each included.
    [[nodiscard]] UInt64 GetPreparedCount() const noexcept
    {
        return __atomic_load_n(&m_prepared, __ATOMIC_ACQUIRE);
    }

private:
    // Slot's page-table arena: the runtime pool's below ReservedArenaCount, the metadata arena's
    // above.
    [[nodiscard]] UInt64 ArenaPhysical(const Size slot) const noexcept
    {
        if (m_largeForSlot[slot] < LargeRootArenaCount)
        {
            return PoolPage(m_poolPhysicalStart, LargeRootArenaFirstPage + m_largeForSlot[slot] * LargeRootArenaPages);
        }
        return (slot < ReservedArenaCount)
                   ? PoolPage(m_poolPhysicalStart, ReservedArenaFirstPage + (slot * RootArenaPages))
                   : ArenaPage(m_arenaPhysicalStart, MetadataArenaRootArenaPage(slot));
    }

    // Builds slot's root from the seed over its arena (Build zeroes it first)
    // and the page table that claims it.
    [[nodiscard]] bool PrepareSlot(const Size slot) noexcept
    {
        const UInt64 physical = ArenaPhysical(slot);
        auto* const entries = reinterpret_cast<Foundation::UInt64*>(static_cast<Foundation::UIntPointer>(physical));
        const Memory::PageTableStorage storage{
            PhysicalAddress{physical}, entries,
            m_largeForSlot[slot] < LargeRootArenaCount ? LargeRootArenaPages : RootArenaPages, 0U};
        Amd64RuntimeRootSlot& root = m_slots[slot];
        if (!root.Preparation.Prepare(*m_seed, m_paging, storage).HasValue())
        {
            return false;
        }
        const auto plan = root.Preparation.TryGetMappingPlan();
        if (!plan.HasValue() || (root.PageTable.Initialize(m_paging, root.Preparation, entries) !=
                                 Memory::RuntimeAddressSpacePageTableInitializationError::None))
        {
            return false;
        }
        root.Plan = plan.GetValue();
        return true;
    }

    const InitialMappingPlan* m_seed{nullptr};
    Memory::PagingCapabilities m_paging{};
    UInt64 m_poolPhysicalStart{0U};
    UInt64 m_arenaPhysicalStart{0U};
    Memory::Amd64FrameScrubber* m_scrubber{nullptr};
    // In the metadata arena (MetadataArenaRootSlotFirstPage).
    Amd64RuntimeRootSlot* m_slots{nullptr};
    UInt64 m_prepared{0U};
    Size m_largeForSlot[Slots]{};
    Memory::LargePageTableArenaPool m_largeArenas{};
};

static_assert(RuntimeRoots::Slots == ReservedArenaCount + MetadataArenaRootArenaCount);
static_assert(Memory::Amd64FrameScrubberRoots == BootRootCount + RuntimeRoots::Slots);
static_assert(Memory::Amd64FrameScrubberProcessors == ProcessorStackSlots);

/// The physical address of page `offset` of processor `processor`'s stack extent.
[[nodiscard]] UInt64 StackPage(const UInt64 poolPhysicalStart, const Size processor, const Size offset) noexcept
{
    return RuntimePoolPageAddress(poolPhysicalStart, ProcessorStackExtentPage(processor) + offset);
}

[[nodiscard]] UInt64 ProcessorBit(const Size processor) noexcept
{
    return UInt64{1U} << processor;
}

[[nodiscard]] UInt8* Pointer(const UInt64 address) noexcept
{
    return reinterpret_cast<UInt8*>(static_cast<Foundation::UIntPointer>(address));
}

void Hex(const UInt64 value) noexcept
{
    char digits[19U]{'0', 'x'};
    constexpr char alphabet[]{"0123456789ABCDEF"};
    for (Size i{0U}; i < 16U; ++i)
    {
        digits[i + 2U] = alphabet[(value >> ((15U - i) * 4U)) & 15U];
    }
    Platform::Firmware::WriteDiagnosticAscii(digits);
}

[[noreturn]] void Fail(const Diagnostics::Amd64BootStage stage, const UInt64 detail = 0U) noexcept
{
    Diagnostics::StopAmd64(Kernel::Diagnostics::StopCode::BootStage, static_cast<UInt64>(stage), detail);
}

constinit const char* g_rejection{nullptr};
[[nodiscard]] bool Reject(const char* const stage, const UInt64 detail = 0U) noexcept
{
    if (g_rejection == nullptr)
    {
        g_rejection = stage;
    }
    Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AMD64-RUNTIME-PREPARE:FAIL:");
    Platform::Firmware::WriteDiagnosticAscii(stage);
    Platform::Firmware::WriteDiagnosticAscii(":");
    Hex(detail);
    Platform::Firmware::WriteDiagnosticAscii("\r\n");
    return false;
}

[[nodiscard]] bool IsReserved(const ::Asteria::Boot::BootInfo& info, const UInt64 start, const UInt64 pages) noexcept
{
    if ((start == 0U) || ((start & 0xFFFU) != 0U) || (start > 0xFFFF'FFFFU - pages * 4096U) ||
        (info.MemoryRegionCount > ::Asteria::Boot::MaximumMemoryRegions))
    {
        return false;
    }
    const UInt64 end = start + pages * 4096U;
    UInt64 cursor{start};
    for (Size index{0U}; index < info.MemoryRegionCount; ++index)
    {
        const auto& region = info.MemoryRegions[index];
        if (region.PageCount > (~UInt64{0U} - region.PhysicalStart) / 4096U)
        {
            return false;
        }
        const UInt64 regionEnd = region.PhysicalStart + region.PageCount * 4096U;
        if ((region.PhysicalStart >= end) || (regionEnd <= start))
        {
            continue;
        }
        if ((region.PhysicalStart > cursor) || (region.FirmwareType != 2U) ||
            ((region.Attributes & (UInt64{1U} << 63U)) != 0U) || ((region.Attributes & 8U) == 0U))
        {
            return false;
        }
        cursor = regionEnd < end ? regionEnd : end;
    }
    return cursor == end;
}

/// One staged user image and what maps it into its private root.
struct UserImageDomain
{
    Executable::Amd64InitialUserImage Image{};
    Kernel::Memory::BootFrame Frames[Executable::MaximumInitialUserSegments + 2U]{};
    Kernel::Memory::MemoryObject MemoryObjects[Executable::MaximumInitialUserSegments + 2U]{};
    Kernel::Memory::DirectUserMemoryMapping Mappings[Executable::MaximumInitialUserSegments + 2U]{};
    Kernel::Memory::DirectUserMemoryAccess Access;
};

/// Records each parked Frame pin a Device Manager's confirmation releases in the block evidence,
/// The last two by turns, and has the scrubber keep the first bytes of its page as the frame pool
/// zeroes it: the late write the pin kept from reused memory landed there. Called under
/// PciFunctionControl's lock on CPU0; it takes none.
class ParkedReleaseRecorder final : public Kernel::Device::IParkedPinObserver
{
public:
    explicit ParkedReleaseRecorder(Memory::Amd64FrameScrubber& scrubber) noexcept : m_scrubber{scrubber}
    {
    }

    void Released(const Kernel::Object::ObjectId frame, const Kernel::Memory::PhysicalAddress physicalStart,
                  const Kernel::Memory::PageCount pages) noexcept override
    {
        auto& evidence = Diagnostics::GetAmd64EvidenceRecord().Block;
        const Size slot = static_cast<Size>(evidence.Releases & 1U);
        evidence.Released[slot] = Diagnostics::Amd64ParkedReleaseEvidence{.Frame = frame.GetValue(),
                                                                          .PhysicalStart = physicalStart.GetValue(),
                                                                          .Pages = pages.GetValue(),
                                                                          .Sampled = 0U,
                                                                          .Sample = {}};
        ++evidence.Releases;
        m_scrubber.Watch(slot, physicalStart.GetValue());
    }

private:
    Memory::Amd64FrameScrubber& m_scrubber;
};

class BootRuntime final
{
public:
    BootRuntime(const Foundation::ProcessorMask processorMask, const Kernel::Object::CapabilityValidationKey& key,
                const Kernel::Object::CapabilityValidationKey& pagerKey,
                const Kernel::Object::CapabilityValidationKey& tokenKey,
                const Kernel::Object::CapabilityValidationKey& spinnerKey) noexcept
        : m_registry{m_registryLock}, m_issuer{m_registry}, m_resources{m_ledger}, m_pagerGrant{m_ledger},
          m_objects{m_registry, m_issuer}, m_revocations{m_registryLock},
          m_capabilities{m_registry, m_revocations, m_timer, key.Word0, key.Word1},
          m_endpoint{m_capabilities, BootstrapCharge}, m_diagnostic{m_diagnosticSink, BootstrapCharge},
          m_scheduling{m_registry}, m_timerQueue{m_registry, m_timer},
          m_controller{m_registry, m_timerQueue, m_timer, CpuId{0U}}, m_runLoop{m_controller, m_addressSpaces, m_idle},
          m_wait{m_registry, m_timerQueue, m_timer, m_controller, m_runLoop, m_thread, m_worker},
          m_mailbox{m_notifier, processorMask},
          m_pager{m_registry,       m_revocations, m_timer,  m_controller, m_runLoop,
                  m_shootdowns[0U], m_mailbox,     pagerKey, tokenKey},
          m_spinner{m_registry, m_revocations, m_timer, m_controller, m_runLoop, spinnerKey},
          m_threads{m_runLoop, m_timer, m_registry, m_controller.GetWakeFacade()}
    {
    }

    [[nodiscard]] bool Prepare(const Amd64BootRuntimeInput& input) noexcept;
    [[noreturn]] void Start() noexcept;
    void Trap(Interrupt::Amd64TrapFrame& frame) noexcept;
    void BindBspProgress() noexcept;

    [[nodiscard]] UInt64 GetTscTicksPerMicrosecond() const noexcept
    {
        return (m_calibration.FrequencyHz + 999'999U) / 1'000'000U;
    }
    [[noreturn]] void RunApplicationProcessor(UInt64 processor) noexcept;

private:
    [[nodiscard]] bool AddPoolMappings() noexcept;
    [[nodiscard, gnu::noinline]] bool PrepareProcessors() noexcept;
    [[nodiscard, gnu::noinline]] bool StartProcessors() noexcept;
    [[nodiscard, gnu::noinline]] bool PrepareSharedProbe() noexcept;
    [[nodiscard, gnu::noinline]] bool RunSharedProbe() noexcept;
    [[nodiscard]] bool RunService(Size processor, UInt64 address) noexcept;
    void RecordRuntime() noexcept;
    [[nodiscard, gnu::noinline]] bool RunDevices() noexcept;
    [[nodiscard, gnu::noinline]] bool EnableIommu() noexcept;
    [[nodiscard, gnu::noinline]] bool ImportMemory() noexcept;
    [[gnu::noinline]] void ApplyTrustedDma() noexcept;
    [[nodiscard, gnu::noinline]] bool HandoffIommu(Kernel::Device::PciEcamConfigurationSpace& configuration) noexcept;
    [[nodiscard]] static bool HandoffIommuCallback(void* context,
                                                   Kernel::Device::PciEcamConfigurationSpace& configuration) noexcept;
    [[nodiscard, gnu::noinline]] bool
    SweepFirmwareDma(Kernel::Device::PciEcamConfigurationSpace& configuration) noexcept;
    [[nodiscard, gnu::noinline]] bool EnableAmdVi() noexcept;
    [[nodiscard, gnu::noinline]] bool PrepareAmdViReserved(Foundation::UInt16 requester, UInt8 regions) noexcept;
    [[gnu::noinline]] void RecordAmdVi() noexcept;
    [[gnu::noinline]] void BuildAmdViBackends() noexcept;
    [[gnu::noinline]] void BuildIommuBackends() noexcept;
    [[nodiscard, gnu::noinline]] bool ConfigureIommuUnit(Size unit) noexcept;
    [[nodiscard]] bool RefuseIommu(bool enabledAny) noexcept;
    [[nodiscard, gnu::noinline]] bool PrepareReservedDma(Size function) noexcept;
    [[nodiscard]] Platform::Device::VtdTablePage IommuPage(Size page) const noexcept;
    [[gnu::noinline]] void RecordIommu() noexcept;
    [[gnu::noinline]] void RecordInterrupts() noexcept;
    [[nodiscard]] bool PrepareRoot(Size index) noexcept;
    [[nodiscard]] bool InitializeResourceDomain() noexcept;
    [[nodiscard, gnu::noinline]] bool InitializeManagement() noexcept;
    [[nodiscard, gnu::noinline]] bool ConfigureScrubber() noexcept;
    [[nodiscard, gnu::noinline]] bool ConfigureThreads() noexcept;
    [[nodiscard, gnu::noinline]] bool ConfigureDomains() noexcept;
    [[nodiscard, gnu::noinline]] bool StageImage(Size image, ::Asteria::Executable::Elf::Elf64ByteView bytes) noexcept;
    [[nodiscard]] bool MapUserRange(Size image, Size index) noexcept;
    [[nodiscard, gnu::noinline]] bool MapPreparedUserRange(Size image, Size index, Kernel::Object::ObjectId frame,
                                                           Kernel::Object::ObjectId memory) noexcept;
    void FillLazyRegion() noexcept;
    void ZeroFramePool() noexcept;
    [[nodiscard, gnu::noinline]] bool InitializePager() noexcept;
    [[nodiscard, gnu::noinline]] bool InitializeSpinner() noexcept;
    [[nodiscard, gnu::noinline]] bool AdmitThreads() noexcept;
    /// Whether the run loop's active thread is the initial image's.
    [[nodiscard]] bool IsClientRunning() const noexcept;
    void RecordTermination() noexcept;
    [[nodiscard, gnu::noinline]] bool CheckRegistryBudget() noexcept;
    /// Publishes the object management evidence after an ObjectCreate,
    /// ObjectDestroy, MemoryMap or MemoryUnmap of the pager.
    [[gnu::noinline]] void RecordAllocator(const Kernel::Memory::FrameObjectPoolSnapshot& frames) noexcept;
    [[gnu::noinline]] void RecordManagement() noexcept;
    [[gnu::noinline]] void RecordMapping(Diagnostics::Amd64ObjectManagementEvidence& evidence) noexcept;
    [[gnu::noinline]] void RecordThreads() noexcept;
    [[gnu::noinline]] void RecordCollection() noexcept;
    [[gnu::noinline]] void RecordComponents() noexcept;
    [[nodiscard, gnu::noinline]] bool BlockOnUserFault(const Interrupt::Amd64TrapFrame& frame) noexcept;
    [[nodiscard, gnu::noinline]] bool BlockUserOnFault(const Kernel::Memory::FaultRecord& record) noexcept;
    [[nodiscard, gnu::noinline]] bool BlockCreatedOnFault(Size created,
                                                          const Kernel::Memory::FaultRecord& record) noexcept;
    [[nodiscard, gnu::noinline]] bool DispatchSystemCall(Interrupt::Amd64TrapFrame& frame) noexcept;
    /// Returns `frame` to CPL3 on the thread whose kernel stack top is
    /// `kernelStackTop`, under the CPL3 RFLAGS policy.
    void ResumeUserOn(Interrupt::Amd64TrapFrame& frame, UInt64 kernelStackTop) noexcept;
    [[nodiscard, gnu::noinline]] bool PreemptUser() noexcept;
    [[nodiscard, gnu::noinline]] bool PreemptClient() noexcept;
    /// The active run's deadline refresh at a user tick: the reached
    /// Deadline's sources (ProcessorDeadlineSource bits) when it fired, 0 when the deadline was
    /// re-armed, -1 on a controller failure.
    [[nodiscard, gnu::noinline]] int RefreshRunStop() noexcept;
    [[nodiscard, gnu::noinline]] bool WaitForTickFromIdle() noexcept;
    [[nodiscard, gnu::noinline]] UInt64 MeasureTicksPerMicrosecond() noexcept;
    /// The idle dispatch's failure in one word: the run-loop, controller,
    /// scheduler and deadline errors, the native error (one byte each), the
    /// thread it names (1 the client, 2 the pager, 3 the spinner, 4 a created
    /// thread, 5 another, 0 none) and whether the run was retained.
    [[nodiscard]] UInt64 DescribeDispatchFailure() const noexcept;
    [[nodiscard, gnu::noinline]] bool AdmitWorker() noexcept;
    [[nodiscard, gnu::noinline]] bool PrepareThread() noexcept;
    [[nodiscard, gnu::noinline]] bool PrepareEndpoint() noexcept;
    [[nodiscard]] bool AddIdentity(UInt64 start, UInt64 pages, InitialMappingOwner owner,
                                   MappingPermissions permissions,
                                   MemoryCachePolicy cache = MemoryCachePolicy::WriteBack) noexcept;
    [[nodiscard, gnu::noinline]] bool AddArenaMappings() noexcept;
    /// Places the registry's entries and the Frame and MemoryObject pools' slots in the metadata
    /// arena.
    [[nodiscard, gnu::noinline]] bool AttachSlabs() noexcept;
    void RecordMetadata() noexcept;
    [[noreturn]] static void UserWorker(void* argument) noexcept;
    [[noreturn]] static void ContinueUser(void* argument) noexcept;
    [[maybe_unused, noreturn, gnu::noinline]] void StartPreemption() noexcept;

    Amd64BootRuntimeInput m_input{};
    ::Asteria::Boot::Acpi::AcpiTopology m_topology{};
    I8042Publication m_i8042Publication{};
    bool m_i8042DsdtMatches{false};
    InitialMapping m_originalMappings[16U]{};
    Size m_originalMappingCount{0U};
    bool m_attempted{false};
    Memory::PagingCapabilities m_paging{};
    InitialMappingPlan m_seed{};
    Memory::InitialPageTablePreparation m_preparations[BootRootCount]{};
    Memory::RuntimeAddressSpacePageTable m_pageTables[BootRootCount]{};
    AddressSpace m_spaces[BootRootCount]{};
    AddressSpaceId m_spaceIds[BootRootCount]{};
    UserImageDomain m_images[UserImages];
    Platform::Synchronization::SpinLock m_registryLock;
    Kernel::Object::ObjectRegistry m_registry;
    Kernel::Object::RootCapabilityIssuer m_issuer;
    Kernel::Resource::ResourceDomainLedger m_ledger;
    Kernel::Resource::ResourceDomain m_resources;
    Kernel::Resource::ResourceDomain m_pagerGrant;
    Kernel::System::ObjectManager m_objects;
    Kernel::Memory::PhysicalPageImporter m_importer{};
    Kernel::Memory::PhysicalPageImportExclusion m_importVtdExclusions[5U]{};
    Kernel::Memory::PhysicalPageImportExclusion m_importAmdExclusions[Device::AmdViHandoffRecord::BaseCount]{};
    Kernel::Device::PciEcamRegion m_importWindows[Kernel::Device::PciEcamConfigurationSpace::MaximumRegions]{};
    Kernel::Resource::ResourceDomainObjectId m_resourceId{};
    Kernel::Object::ObjectId m_pagerGrantId{};
    // The assignable devices the boot published: edu, and the AHCI controller once its diagnostic
    // ran; invalid identities where none was.
    Platform::Device::AssignableDevice m_assignable{};
    Platform::Device::AssignableDevice m_ahci{};
    /// Whether m_ahci holds no disk but the emulator's
    /// (PagerManagementDiskWritable).
    bool m_ahciDiskWritable{false};
    Platform::Device::AssignableDevice m_nic{};
    Platform::Device::AssignableDevice m_xhci[Abi::PagerXhciControllers]{};
    Platform::Device::AssignableDevice m_usbHost[Abi::PagerUsbHostControllers]{};
    // The scanout the boot published; invalid without one.
    Kernel::Object::ObjectId m_scanout{};
    // The device interrupts CPU0 takes, by vector.
    Interrupt::Amd64DeviceVectors m_deviceVectors;
    Interrupt::Amd64InterruptLines m_lines;
    Interrupt::Amd64IoApicAccess m_ioApicAccess;
    Kernel::Device::DeviceRegion m_i8042Ports[2U]{};
    alignas(Kernel::Interrupt::InterruptLine) UInt8 m_i8042LineStorage[2U][sizeof(Kernel::Interrupt::InterruptLine)]{};
    Kernel::Object::ObjectId m_i8042[4U]{};
    [[nodiscard, gnu::noinline]] bool PublishI8042() noexcept;
    // The run-time PCI configuration access DeviceControl writes through.
    Device::Amd64PortIo m_portIo;
    Platform::Device::PortPciConfigurationSpace m_pciConfiguration{m_portIo};
    // DMA remapping: the ACPI DMAR's unit, chosen when the seed mappings are planned, and its
    // tables in the pool.
    bool m_amdViSelected{false};
    alignas(Device::AmdViUnit) UInt8 m_amdViStorage[sizeof(Device::AmdViUnit)]{};
    Device::AmdViUnit* m_amdVi{nullptr};
    alignas(Device::AmdViDomainBackend) UInt8
        m_amdViBackendStorage[IommuDomainSlots][sizeof(Device::AmdViDomainBackend)]{};
    alignas(Device::AmdViDomainBackend) UInt8
        m_amdViReservedStorage[IommuReservedDomains][sizeof(Device::AmdViDomainBackend)]{};
    Platform::Device::VtdTopologySelection m_vtdSelection{};
    Platform::Device::VtdTopologyPlan m_vtdPlan{};
    // One durable record per inventory entry, including VT-d units built in the reused scratch
    // storage. AMD-Vi's complete register record is pending; no all-memory importer is enabled.
    Device::AmdViHandoffRecord m_amdHandoffRecords[::Asteria::Boot::Acpi::MaximumAcpiIommuHandoffs]{};
    Platform::Device::VtdHandoffRecord m_vtdHandoffRecords[::Asteria::Boot::Acpi::MaximumAcpiIommuHandoffs]{};
    bool m_iommuHandoffRefused{false};
    Device::Amd64VtdRegisters m_vtdRegisters[IommuUnits]{};
    Device::Amd64CacheMaintenance m_vtdCache;
    alignas(Platform::Device::VtdUnit) UInt8 m_vtdStorage[IommuUnits][sizeof(Platform::Device::VtdUnit)]{};
    Platform::Device::VtdUnit* m_vtd[IommuUnits]{};
    alignas(Platform::Device::VtdDomainBackend) UInt8
        m_vtdBackendStorage[IommuDomainSlots][IommuUnits][sizeof(Platform::Device::VtdDomainBackend)]{};
    alignas(Platform::Device::VtdDomainGroup) UInt8
        m_vtdGroupStorage[IommuDomainSlots][sizeof(Platform::Device::VtdDomainGroup)]{};
    alignas(Platform::Device::VtdDomainBackend) UInt8
        m_reservedStorage[IommuReservedDomains][sizeof(Platform::Device::VtdDomainBackend)]{};
    Platform::Device::VtdDomainBackend* m_reserved[IommuReservedDomains]{};
    Size m_reservedCount{0U};
    // The firmware DMA ranges every IommuDomain refuses to map for the requesters they are reserved
    // for.
    Platform::Device::FirmwareDmaReservations m_dmaReservations{};
    UInt8 m_amdReservedMasks[IommuReservedDomains]{};
    Device::AmdViDomainBackend* m_amdReserved[IommuReservedDomains]{};
    Foundation::UInt16 m_requesters[Platform::Device::VtdUnit::MaximumRequesters]{};
    Hal::Device::IIommuDomainBackend* m_vtdBackends[Kernel::Device::IommuDomainPool::Capacity]{};
    Kernel::Device::IommuDomainSlot m_iommuSlots[Kernel::Device::IommuDomainPool::Capacity]{};
    // The trusted-driver profile: the BOOT.CFG allowlist applied once EnableIommu decided, and the
    // identity backends the pool takes when no IOMMU backend did.
    Platform::Device::TrustedDmaRuntime m_trustedDma{};
    Kernel::Object::RevocationRegistry m_revocations;
    // Trap storage is private to its processor.
    struct ProcessorUserState
    {
        Kernel::System::SystemCallScratch Scratch{};
        Kernel::Scheduler::ProcessorKernelRunLoopReceipt DispatchReceipt{};
        Kernel::Scheduler::ProcessorKernelRunLoopFailure DispatchFailure{};
        Kernel::Scheduler::ProcessorKernelRunLoopReceipt PreemptReceipt{};
        Kernel::Scheduler::ProcessorKernelRunLoopFailure PreemptFailure{};
        Interrupt::Amd64TrapFrame LaunchFrame{};
        UInt64 TimerInterrupts{0U};
        Scheduler::Amd64ProcessorFaultState ClientFaultState{};
        UInt64 ClientFaultDeferrals{0U};
        UInt64 SpinnerFaultDeferrals{0U};
        UInt64 FaultDeferrals{0U};
    };
    ProcessorUserState m_userState[Kernel::Scheduler::MaximumProcessorCount]{};
    [[nodiscard]] Size CurrentIndex() const noexcept
    {
        const auto cpu = Platform::Synchronization::CurrentProcessor();
        return cpu.IsValid() ? cpu.GetValue() : 0U;
    }
    [[nodiscard]] ProcessorUserState& LocalState() noexcept
    {
        return m_userState[CurrentIndex()];
    }
    [[nodiscard]] const ProcessorUserState& LocalState() const noexcept
    {
        return m_userState[CurrentIndex()];
    }
    [[nodiscard]] Time::Amd64ApicTimer& LocalTimer() noexcept
    {
        const auto cpu = CurrentIndex();
        return cpu == 0U ? m_timer : m_processors[cpu]->GetLocalTimer();
    }
    [[nodiscard]] Kernel::Scheduler::ProcessorRunController& LocalController() noexcept
    {
        const auto cpu = CurrentIndex();
        return cpu == 0U ? m_controller : m_processors[cpu]->GetController();
    }
    [[nodiscard]] Scheduler::Amd64ProcessorRunLoop& LocalRunLoop() noexcept
    {
        const auto cpu = CurrentIndex();
        return cpu == 0U ? m_runLoop : m_processors[cpu]->GetRunLoop();
    }
    [[nodiscard]] const Scheduler::Amd64ProcessorRunLoop& LocalRunLoop() const noexcept
    {
        const auto cpu = CurrentIndex();
        return cpu == 0U ? m_runLoop : m_processors[cpu]->GetRunLoop();
    }
    [[nodiscard]] System::Amd64WaitRuntime& LocalWait() noexcept
    {
        const auto cpu = CurrentIndex();
        return cpu == 0U ? m_wait : m_processors[cpu]->GetWaitRuntime();
    }
    Time::Amd64ApicTimer m_timer;
    Time::Amd64TscCalibration m_calibration{};
    Kernel::Object::CapabilitySpace m_capabilities;
    Kernel::Ipc::Endpoint m_endpoint;
    Kernel::Object::CapabilityHandle m_endpointCapability{};
    Platform::Firmware::DiagnosticLineSink m_diagnosticSink{};
    Kernel::Diagnostics::DiagnosticChannel m_diagnostic;
    Kernel::Object::ObjectId m_diagnosticObject{};
    Kernel::Object::CapabilityHandle m_diagnosticCapability{};
    Kernel::Protection::ProtectionDomain m_protection;
    Kernel::Scheduler::Thread m_thread;
    Kernel::Scheduler::SchedulingContext m_scheduling;
    Kernel::Scheduler::ThreadId m_threadId{};
    Kernel::Scheduler::SchedulingContextId m_schedulingId{};
    Kernel::Time::TimerQueue m_timerQueue;
    Kernel::Scheduler::ProcessorRunController m_controller;
    Kernel::Memory::ProcessorAddressSpaceController m_addressSpaces;
    Scheduler::Amd64KernelContext m_idle;
    Scheduler::Amd64KernelContext m_worker;
    Scheduler::Amd64ExtendedStateArea m_workerExtendedState{};
    Scheduler::Amd64ProcessorRunLoop m_runLoop;
    System::Amd64WaitRuntime m_wait;
    Interrupt::Amd64DescriptorTables m_descriptors[4U]{};
    Interrupt::Amd64EntryContext m_entryContexts[4U]{};
    Kernel::Scheduler::ProcessorSchedulerWakeFacade* m_homeWakes[Kernel::Scheduler::MaximumProcessorCount]{};
    ApplicationProcessor* m_processors[4U]{};
    Foundation::UInt32 m_hardwareIds[4U]{};
    // The processors this runtime starts: the enabled MADT records (online-
    // capable ones are omitted), the BSP as logical CPU 0.
    Size m_processorCount{0U};
    IpiNotifier m_notifier;
    Memory::Amd64LocalTlbInvalidator m_invalidator;
    Kernel::Memory::TlbShootdownMailboxTransport m_mailbox;
    TlbShootdownDispatcher m_shootdowns[4U]{
        TlbShootdownDispatcher{CpuId{0U}, m_invalidator}, TlbShootdownDispatcher{CpuId{1U}, m_invalidator},
        TlbShootdownDispatcher{CpuId{2U}, m_invalidator}, TlbShootdownDispatcher{CpuId{3U}, m_invalidator}};
    TlbShootdownDrain m_drains[4U]{
        TlbShootdownDrain{m_mailbox, m_shootdowns[0U]}, TlbShootdownDrain{m_mailbox, m_shootdowns[1U]},
        TlbShootdownDrain{m_mailbox, m_shootdowns[2U]}, TlbShootdownDrain{m_mailbox, m_shootdowns[3U]}};
    MappingProgress m_mappingProgress;
    Memory::Amd64FrameScrubber m_scrubber;
    ParkedReleaseRecorder m_releases{m_scrubber};
    Kernel::Memory::BootFrame m_probeFrame;
    Kernel::Memory::MemoryObject m_probeMemory;
    Kernel::Memory::PinnedFrameMappingPreparation m_probePreparation;
    Kernel::Memory::AddressSpaceShootdownRequest m_probeRequest;
    System::Amd64PagerRuntime m_pager;
    // Preparation persists through Initialize without placing the growing ABI record on the idle stack.
    System::Amd64PagerRuntimeInput m_pagerInput{};
    // The test firmware's spinner; a firmware without a spinner image has none.
    System::Amd64SpinnerRuntime m_spinner;
    bool m_hasSpinner{false};
    // The native slots of the threads user code creates; only the test firmware's pager grant may
    // create them.
    System::Amd64UserThreadTable m_threads;
    System::Amd64UserThreadTableInput m_threadInput{};
    // The roots of created AddressSpaces, prepared with the test firmware's grant.
    RuntimeRoots m_runtimeRoots;
    // Consecutive deferrals of the client's and the spinner's current fault,
    // and every deferral of a boot thread's fault.
    // The client thread's kernel stack top (its system-call stack).
    UInt64 m_clientStackTop{0U};
    bool m_prepared{false};
};

alignas(BootRuntime) UInt8 g_runtimeStorage[sizeof(BootRuntime)]{};
BootRuntime* g_runtime{nullptr};

bool BootRuntime::AddIdentity(const UInt64 start, const UInt64 pages, const InitialMappingOwner owner,
                              const MappingPermissions permissions, const MemoryCachePolicy cache) noexcept
{
    return m_seed.AddMapping(InitialMapping{VirtualAddress{start}, PhysicalAddress{start}, PageCount{pages},
                                            permissions, cache, owner, 0U}) == InitialMappingPlanError::None;
}

bool BootRuntime::AddPoolMappings() noexcept
{
    const MappingPermissions writable = MappingPermissions{MappingPermission::Read}.With(MappingPermission::Write);
    if (!AddIdentity(PoolPage(m_input.PoolPhysicalStart, AhciIdentifyPage), AhciPages,
                     InitialMappingOwner::NucleusWritableData, writable) ||
        m_seed.AddMapping({VirtualAddress{Device::MmioWindowVirtualAddress},
                           PhysicalAddress{m_topology.LocalControllerPhysicalAddress}, PageCount{1U}, writable,
                           MemoryCachePolicy::Uncached, InitialMappingOwner::Device, 0U}) !=
            InitialMappingPlanError::None)
    {
        return false;
    }
    for (Size index{0U}; index < m_topology.IoControllerCount; ++index)
    {
        const auto& controller = m_topology.IoControllers[index];
        if ((controller.ControllerKind == ::Asteria::Boot::Acpi::AcpiIoControllerKind::IoApic) &&
            (controller.PhysicalAddress != 0U) && ((controller.PhysicalAddress & 0xFFFU) == 0U) &&
            AddIdentity(controller.PhysicalAddress, 1U, InitialMappingOwner::Device, writable,
                        MemoryCachePolicy::Uncached))
        {
            m_ioApicAccess.AllowRegisterPage(controller.PhysicalAddress);
        }
    }
    // The VT-d tables, and the unit's register page uncached where the DMAR names a usable unit.
    m_amdViSelected = (m_topology.DmarPhysicalAddress == 0U) && (m_topology.Ivrs.PhysicalAddress != 0U);
    m_vtdSelection = m_amdViSelected ? Device::SelectAmdViUnit(m_topology.Ivrs)
                                     : Platform::Device::SelectVtdUnit(m_topology, Device::Amd64VtdAddressBits);
    for (Size index{0U}; index < m_topology.IommuHandoffCount; ++index)
    {
        const auto& unit = m_topology.IommuHandoffs[index];
        UInt64 pages{unit.Kind == 2U ? 4U : 1U};
        if (unit.Kind == 1U && m_vtdSelection.Error == Platform::Device::VtdTopologyError::None)
        {
            for (Size slot{0U}; slot < m_topology.DmaRemappingUnitCount; ++slot)
            {
                const auto& descriptor = m_topology.DmaRemappingUnits[slot];
                if (descriptor.RegisterBaseAddress == unit.RegisterBase)
                {
                    pages = Platform::Device::VtdRegisterBytes(descriptor) / 4096U;
                }
            }
        }
        if (!AddIdentity(unit.RegisterBase, pages, InitialMappingOwner::Device, writable, MemoryCachePolicy::Uncached))
        {
            return false;
        }
    }
    if (!AddIdentity(PoolPage(m_input.PoolPhysicalStart, AmdViDeviceFirstPage),
                     AmdViCompletionPage + 1U - AmdViDeviceFirstPage, InitialMappingOwner::NucleusWritableData,
                     writable))
    {
        return false;
    }
    if (!AddIdentity(PoolPage(m_input.PoolPhysicalStart, IommuRootTablePage),
                     StopTableFirstPage + StopTablePages - IommuRootTablePage, InitialMappingOwner::NucleusWritableData,
                     writable))
    {
        return false;
    }
    if (!AddIdentity(m_input.PoolPhysicalStart, RootArenaCount * RootArenaPages, InitialMappingOwner::PageTable,
                     writable) ||
        !AddIdentity(PoolPage(m_input.PoolPhysicalStart, ReservedArenaFirstPage), ReservedArenaCount * RootArenaPages,
                     InitialMappingOwner::PageTable, writable) ||
        !AddIdentity(PoolPage(m_input.PoolPhysicalStart, LargeRootArenaFirstPage),
                     LargeRootArenaCount * LargeRootArenaPages, InitialMappingOwner::PageTable, writable) ||
        !AddIdentity(PoolPage(m_input.PoolPhysicalStart, ApIdleRootFirstPage), ApIdleRootCount * RootArenaPages,
                     InitialMappingOwner::PageTable, writable))
    {
        return false;
    }
    for (Size cpu{0U}; cpu < ProcessorStackSlots; ++cpu)
    {
        if (!AddIdentity(PoolPage(m_input.PoolPhysicalStart, NmiStackPage(cpu)), InterruptStackPages,
                         InitialMappingOwner::NucleusStack, writable))
        {
            return false;
        }
        constexpr Size stacks[5U][2U]{{IdleStackPage, KernelStackPages},
                                      {WorkerStackPage, KernelStackPages},
                                      {SystemCallStackPage, KernelStackPages},
                                      {DoubleFaultStackPage, InterruptStackPages},
                                      {MachineCheckStackPage, InterruptStackPages}};
        for (const auto& stack : stacks)
        {
            if (!AddIdentity(StackPage(m_input.PoolPhysicalStart, cpu, stack[0U]), stack[1U],
                             InitialMappingOwner::NucleusStack, writable))
            {
                return false;
            }
        }
    }
    // Every root gets every processor's scrub slot; they share the MMIO window's page table.
    static_assert((ScrubSlotVirtualAddress >> 21U) == (Device::MmioWindowVirtualAddress >> 21U));
    for (Size cpu{0U}; cpu < ProcessorStackSlots; ++cpu)
    {
        if (m_seed.AddMapping({VirtualAddress{ScrubSlotVirtualAddress + (cpu * RuntimePoolPageBytes)},
                               PhysicalAddress{PoolPage(m_input.PoolPhysicalStart, ScrubSlotFirstPage + cpu)},
                               PageCount{1U}, writable, MemoryCachePolicy::WriteBack,
                               InitialMappingOwner::NucleusWritableData, 0U}) != InitialMappingPlanError::None)
        {
            return false;
        }
    }
    if (!AddIdentity(PoolPage(m_input.PoolPhysicalStart, PagerKernelStackPage), KernelStackPages,
                     InitialMappingOwner::NucleusStack, writable) ||
        !AddIdentity(PoolPage(m_input.PoolPhysicalStart, SpinnerKernelStackPage), KernelStackPages,
                     InitialMappingOwner::NucleusStack, writable))
    {
        return false;
    }
    for (Size slot{0U}; slot < ThreadStackSlots; ++slot)
    {
        if (!AddIdentity(PoolPage(m_input.PoolPhysicalStart, ThreadStackPage(slot)), KernelStackPages,
                         InitialMappingOwner::NucleusStack, writable))
        {
            return false;
        }
    }
    if (!AddArenaMappings())
    {
        return false;
    }
    return AddIdentity(m_input.StartupPhysicalStart, 1U, InitialMappingOwner::NucleusCode,
                       MappingPermissions{MappingPermission::Read}.With(MappingPermission::Execute)) &&
           AddIdentity(m_input.StartupPhysicalStart + 4096U, 1U, InitialMappingOwner::NucleusWritableData, writable) &&
           AddIdentity(m_input.BootInformation->Acpi.LocalControllerPhysicalAddress, 1U, InitialMappingOwner::Device,
                       writable, MemoryCachePolicy::Uncached);
}

// The metadata arena, supervisor RW/NX in every root, as Amd64MetadataArena.cpp lists it.
bool BootRuntime::AddArenaMappings() noexcept
{
    return AddMetadataArenaMappings(m_seed, m_input.MetadataArenaPhysicalStart, m_input.MetadataArenaPages);
}

bool BootRuntime::PrepareProcessors() noexcept
{
    Foundation::UInt32 bsp{m_topology.Processors[0U].HardwareId};
#if defined(ASTERIA_TARGET_AMD64)
    Interrupt::Amd64LocalApic apic;
    if (!apic.Initialize(m_topology.LocalControllerPhysicalAddress))
    {
        return Reject("APIC-STATE");
    }
    bsp = apic.GetId();
    const auto codeStart = reinterpret_cast<Foundation::UIntPointer>(Interrupt::AsteriaAmd64StartupCodeStart);
    const auto codeEnd = reinterpret_cast<Foundation::UIntPointer>(Interrupt::AsteriaAmd64StartupCodeEnd);
    if (codeEnd <= codeStart || !Interrupt::Amd64ApplicationProcessorStartup::PrepareCode(
                                    Pointer(m_input.StartupPhysicalStart), m_input.StartupPhysicalStart,
                                    Interrupt::AsteriaAmd64StartupCodeStart, codeEnd - codeStart))
    {
        return false;
    }
#endif
    // The shootdown mailbox was sized for the enabled records
    // (PrepareAmd64BootRuntime); the selection admits exactly those.
    Amd64ProcessorSelection selection{};
    if (SelectAmd64Processors(m_topology, bsp, selection,
#if defined(ASTERIA_TARGET_AMD64)
                              apic.IsX2Apic() ? 0xFFFF'FFFEU : 254U
#else
                              254U
#endif
                              ) != Amd64ProcessorTopologyError::None ||
        selection.Count != CountEnabledAmd64Processors(m_topology))
    {
        return false;
    }
    for (Size cpu{0U}; cpu < selection.Count; ++cpu)
    {
        m_hardwareIds[cpu] = selection.HardwareIds[cpu];
    }
    m_processorCount = selection.Count;
    Scheduler::BindAmd64ProcessorRunLoop(CpuId{0U}, m_runLoop);
    m_homeWakes[0U] = &m_controller.GetWakeFacade();
    m_homeWakes[0U]->BindHomes(m_homeWakes);
    for (auto& dispatcher : m_shootdowns)
    {
        dispatcher.UseCurrentProcessor();
    }
    for (Size cpu{1U}; cpu < m_processorCount; ++cpu)
    {
        const UInt64 stack = StackPage(m_input.PoolPhysicalStart, cpu, WorkerStackPage);
        const Scheduler::Amd64ApplicationProcessorInput input{
            &m_registry,
            &m_resources,
            m_resourceId,
            &m_spaces[SharedRootIndex],
            &m_spaces[IdleRootIndex(cpu)],
            &m_threads,
            &m_objects,
            &m_descriptors[cpu],
            &m_entryContexts[cpu],
            {Pointer(stack), KernelStackPages * RuntimePoolPageBytes, stack},
            m_topology.LocalControllerPhysicalAddress,
            CpuId{static_cast<Foundation::UInt8>(cpu)},
            m_hardwareIds[cpu],
#if defined(ASTERIA_TARGET_AMD64)
            apic.IsX2Apic()
#else
            false
#endif
        };
        m_processors[cpu] = new (g_applicationProcessorStorage[cpu - 1U]) ApplicationProcessor{input};
        Scheduler::BindAmd64ProcessorRunLoop(CpuId{static_cast<Foundation::UInt8>(cpu)},
                                             m_processors[cpu]->GetRunLoop());
        m_homeWakes[cpu] = &m_processors[cpu]->GetController().GetWakeFacade();
        m_homeWakes[cpu]->BindHomes(m_homeWakes);
        m_processors[cpu]->BindDrain(m_drains[cpu]);
    }
    return true;
}

bool BootRuntime::RunService(const Size processor, const UInt64 address) noexcept
{
    auto& runtime = *m_processors[processor];
    const UInt64 sequence = runtime.GetCompleted() + 1U;
    if (!runtime.RequestService(address))
    {
        return false;
    }
    for (Kernel::Scheduler::CrossProcessorWait wait; runtime.GetCompleted() != sequence;)
    {
        if (!wait.Poll())
        {
            Kernel::Scheduler::CrossProcessorWait::Timeout(ProcessorBit(processor));
        }
    }
    return runtime.GetCompleted() == sequence;
}

// Starts every AP and waits until it serves: it proved its local timer and
// one deferred timer delivery, and its worker completed one service request.
// An AP that does not report serving, or does not complete the request,
// within the bound fails the boot; what each AP observed is recorded.
bool BootRuntime::StartProcessors() noexcept
{
#if defined(ASTERIA_TARGET_AMD64)
    const auto offset = reinterpret_cast<Foundation::UIntPointer>(Interrupt::AsteriaAmd64StartupLongEntry) -
                        reinterpret_cast<Foundation::UIntPointer>(Interrupt::AsteriaAmd64StartupCodeStart);
    auto& mailbox = *reinterpret_cast<Interrupt::Amd64StartupMailbox*>(
        static_cast<Foundation::UIntPointer>(m_input.StartupPhysicalStart + 4096U));
    auto& evidence = Diagnostics::GetAmd64EvidenceRecord().ApplicationProcessors;
    evidence.ProcessorCount = m_processorCount;
    for (Size cpu{1U}; cpu < m_processorCount; ++cpu)
    {
        if (!Interrupt::Amd64ApplicationProcessorStartup::PrepareMailbox(
                mailbox, m_input.StartupPhysicalStart, m_input.PoolPhysicalStart, offset,
                StackPage(m_input.PoolPhysicalStart, cpu, IdleStackPage + KernelStackPages),
                reinterpret_cast<Foundation::UIntPointer>(this), cpu,
                reinterpret_cast<Foundation::UIntPointer>(&AsteriaAmd64ApplicationProcessorEntry)) ||
            !Interrupt::Amd64ApplicationProcessorStartup::Start(m_timer, m_hardwareIds[cpu],
                                                                m_input.StartupPhysicalStart))
        {
            return false;
        }
        auto& processor = *m_processors[cpu];
        for (Kernel::Scheduler::CrossProcessorWait wait; processor.GetState() == 0U;)
        {
            if (!wait.Poll())
            {
                Kernel::Scheduler::CrossProcessorWait::Timeout(ProcessorBit(cpu));
            }
        }
        if (processor.GetState() != 1U)
        {
            return false;
        }
        auto& slot = evidence.Processors[cpu];
        slot.Processor = cpu;
        slot.State = processor.GetState();
        slot.TimerInterrupts = processor.GetTimerInterrupts();
        slot.TimerDeliveries = processor.GetTimerDeliveries();
        slot.Timer = processor.GetTimerObservation();
        if (!RunService(cpu, 0U))
        {
            return false;
        }
        slot.ServiceCompletions = processor.GetCompleted();
        slot.ServiceRuns = processor.GetRuns();
        Diagnostics::PublishAmd64Evidence(slot.Published);
        evidence.ServingProcessorMask |= ProcessorBit(cpu);
    }
    Diagnostics::PublishAmd64Evidence(evidence.Published);
    Platform::Firmware::WriteDiagnosticAscii("ASTERIA:STAGE:AMD64-AP-RUNTIME\r\n");
    return true;
#else
    return false;
#endif
}

[[noreturn]] void BootRuntime::RunApplicationProcessor(const UInt64 processor) noexcept
{
    if (processor == 0U || processor >= m_processorCount || m_processors[processor] == nullptr)
    {
        Fail(Diagnostics::Amd64BootStage::ApOwner, processor);
    }
    m_processors[processor]->Run();
}

extern "C" [[noreturn]] void AsteriaAmd64RunApplicationProcessor(void* const context, const UInt64 processor) noexcept
{
    if (context != g_runtime || g_runtime == nullptr)
    {
        Fail(Diagnostics::Amd64BootStage::ApContext);
    }
    g_runtime->RunApplicationProcessor(processor);
}

bool BootRuntime::PrepareSharedProbe() noexcept
{
    const UInt64 physical = PoolPage(m_input.PoolPhysicalStart, SharedProbePage);
    const Kernel::Memory::PagePermissions permissions =
        Kernel::Memory::PagePermissions{PagePermission::Read}.With(PagePermission::Write);
    static_cast<void>(memset(Pointer(physical), 0, 4096U));
    *reinterpret_cast<UInt64*>(static_cast<Foundation::UIntPointer>(physical)) = SharedProbeValue;
    if (m_probeFrame.Initialize(BootstrapCharge, {PhysicalAddress{physical},
                                                  PageCount{1U},
                                                  MemoryCachePolicy::WriteBack,
                                                  Kernel::Memory::FrameKind::Ram,
                                                  {}}) != Kernel::Memory::FrameError::None ||
        m_probeMemory
                .Initialize(m_registry, m_resourceId, m_resources,
                            {PageCount{1U},
                             MemoryCachePolicy::WriteBack,
                             permissions,
                             Kernel::Memory::MemoryCommitment::Pinned,
                             {}})
                .Error != Kernel::Memory::MemoryObjectError::None)
    {
        return false;
    }
    const auto frame = m_issuer.CreateObject(m_probeFrame);
    const auto memory = m_issuer.CreateObject(m_probeMemory);
    return frame.HasValue() && memory.HasValue() &&
           m_probePreparation.Prepare(
               m_registry, {m_registry, memory.GetValue()}, m_probeMemory, {m_registry, frame.GetValue()}, m_probeFrame,
               {VirtualAddress{SharedProbeVirtualAddress}, PageCount{1U}, 0U, 0U, permissions, {}}) ==
               Kernel::Memory::PinnedFrameMappingError::None;
}

// Maps the probe page into the shared root once every processor runs on it,
// waits until every targeted processor acknowledged the shootdown (a kernel
// must not complete a mapping change before that), and lets each processor
// read the page through the new mapping. A rejected or unacknowledged
// shootdown, or an AP that does not complete its read, fails the boot; the
// masks and the words read are recorded.
bool BootRuntime::RunSharedProbe() noexcept
{
    auto& space = m_spaces[SharedRootIndex];
    auto& evidence = Diagnostics::GetAmd64EvidenceRecord().SharedRootShootdown;
    const UInt64 mask = ProcessorBit(m_processorCount) - 1U;
    const auto resident = space.GetSnapshot().ResidentProcessorMask;
    evidence.VirtualAddress = SharedProbeVirtualAddress;
    evidence.PhysicalAddress = PoolPage(m_input.PoolPhysicalStart, SharedProbePage);
    evidence.Value = SharedProbeValue; // Stored by PrepareSharedProbe; the page is not mapped yet
    evidence.ResidentProcessorMask = resident;
    if (resident != mask)
    {
        return false;
    }
    const auto begun = space.BeginPinnedFrameMapping(m_spaceIds[SharedRootIndex], m_probePreparation);
    if (!begun.HasValue())
    {
        return false;
    }
    m_probeRequest = begun.GetValue();
    const UInt64 sequence = m_probeRequest.GetToken().GetSequence();
    evidence.ShootdownSequence = sequence;
    const auto dispatched = m_shootdowns[0U].Dispatch(space, m_probeRequest, m_mailbox);
    if (!dispatched.HasValue() || dispatched.GetValue().RejectionObserved)
    {
        return false;
    }
    evidence.QueuedProcessorMask = dispatched.GetValue().QueuedProcessorMask;
    evidence.DispatchAcknowledgedMask = dispatched.GetValue().AcknowledgedProcessorMask;
    bool acknowledged{false};
    for (Kernel::Scheduler::CrossProcessorWait wait;;)
    {
        const auto snapshot = space.GetSnapshot();
        if (snapshot.State != Kernel::Memory::AddressSpaceState::ShootdownPending ||
            snapshot.ResidentProcessorMask != resident || snapshot.ShootdownSequence != sequence)
        {
            return false;
        }
        evidence.TargetProcessorMask = snapshot.TargetProcessorMask;
        evidence.AcknowledgedProcessorMask = snapshot.AcknowledgedProcessorMask;
        if (snapshot.AcknowledgedProcessorMask == snapshot.TargetProcessorMask)
        {
            acknowledged = true;
            break;
        }
        if (!wait.Poll())
        {
            Kernel::Scheduler::CrossProcessorWait::Timeout(snapshot.TargetProcessorMask &
                                                           ~snapshot.AcknowledgedProcessorMask);
        }
    }
    if (!acknowledged || !space.CompleteMapping(m_probeRequest.GetToken()).HasValue())
    {
        return false;
    }
    evidence.ObservedValues[0U] =
        *reinterpret_cast<const UInt64*>(static_cast<Foundation::UIntPointer>(SharedProbeVirtualAddress));
    for (Size cpu{1U}; cpu < m_processorCount; ++cpu)
    {
        if (!RunService(cpu, SharedProbeVirtualAddress))
        {
            return false;
        }
        evidence.ObservedValues[cpu] = m_processors[cpu]->GetObservation();
        evidence.ServiceRuns[cpu] = m_processors[cpu]->GetRuns();
    }
#if defined(ASTERIA_TARGET_AMD64)
    auto& smp = Diagnostics::GetAmd64EvidenceRecord().Smp;
    smp.InterruptWindowStart = reinterpret_cast<Foundation::UIntPointer>(AsteriaAmd64ApInterruptWindowBegin);
    smp.InterruptWindowEnd = reinterpret_cast<Foundation::UIntPointer>(AsteriaAmd64ApInterruptWindowEnd);
#endif
    Diagnostics::PublishAmd64Evidence(Diagnostics::GetAmd64EvidenceRecord().Smp.Published);
    Diagnostics::PublishAmd64Evidence(evidence.Published);
    Platform::Firmware::WriteDiagnosticAscii("ASTERIA:STAGE:AMD64-SHOOTDOWN\r\n");
    return true;
}

bool BootRuntime::PublishI8042() noexcept
{
    m_i8042Publication = GateI8042(m_topology.IapcBootArchitecture, m_topology.Reserved, m_i8042DsdtMatches, m_portIo);
    if (m_i8042Publication.Reason != I8042PublicationReason::Published)
    {
        return false;
    }
    m_i8042Publication.Reason = I8042PublicationReason::Resources;
    if (!m_lines.Configure(m_topology, m_ioApicAccess, m_deviceVectors) || !m_objects.ConfigureInterruptLines(m_lines))
    {
        return false;
    }
    m_objects.ConfigurePorts(m_portIo);
    const Kernel::Device::DevicePortWritePolicy commands{.Lists = ::Asteria::Boot::Bootstrap::I8042CommandWrites,
                                                         .Count = 1U};
    for (Size index{0U}; index < 2U; ++index)
    {
        const Kernel::Device::DeviceRegionDescriptor descriptor{
            .BaseAddress = (index == 0U) ? 0x60U : 0x64U,
            .LengthBytes = 1U,
            .Kind = Kernel::Device::DeviceRegionKind::PortIo,
            .Permissions = Kernel::Device::DeviceRegionPermissions{Kernel::Device::DeviceRegionPermission::Read}.With(
                Kernel::Device::DeviceRegionPermission::Write),
            .Widths = Kernel::Device::DeviceAccessWidths{Kernel::Device::DeviceAccessWidth::Byte},
            .CachePolicy = MemoryCachePolicy::Uncached,
            .Reserved = {}};
        if (m_i8042Ports[index]
                .Initialize(m_registry, m_resourceId, m_resources, descriptor,
                            (index == 0U) ? Kernel::Device::DevicePortWritePolicy{} : commands)
                .Error != Kernel::Device::DeviceRegionError::None)
        {
            return false;
        }
        const auto region = m_issuer.CreateObject(m_i8042Ports[index]);
        if (!region.HasValue())
        {
            return false;
        }
        m_i8042[index] = region.GetValue();
        const auto route = ::Asteria::Boot::Bootstrap::ResolveI8042Route(m_topology, index == 0U ? 1U : 12U,
                                                                         m_i8042Publication.AmlInterrupts);
        const Foundation::UInt32 gsi = route.Gsi;
        // The evidence's trigger and polarity, ISA edge/high by default. The driver re-arms, then
        // drains the controller empty, which serves an edge as well as a level.
        if (!m_lines.CanRoute(gsi) || !m_lines.SetPolicy(gsi, route.Level, route.ActiveLow))
        {
            return false;
        }
        auto* const line = ::new (static_cast<void*>(m_i8042LineStorage[index]))
            Kernel::Interrupt::InterruptLine{gsi, m_resources.GetIdentifier()};
        const auto registered = m_issuer.CreateObject(*line);
        if (!registered.HasValue())
        {
            return false;
        }
        m_i8042[index + 2U] = registered.GetValue();
    }
    return true;
}

bool BootRuntime::RunDevices() noexcept
{
    const Device::Amd64BootstrapDeviceInput input{
        .BootInformation = m_input.BootInformation,
        .Ahci = m_input.AhciResource,
        .Registry = &m_registry,
        .RegistryLock = &m_registryLock,
        .Issuer = &m_issuer,
        .ResourceIdentity = m_resourceId,
        .Resources = &m_resources,
        .Clock = &m_timer,
        .RuntimePool = m_input.PoolPhysicalStart,
        .PhysicalAddressBits = m_paging.PhysicalAddressBits,
        .Device = m_input.DeviceResource,
        .Assignable = &m_assignable,
        .AhciAssignable = &m_ahci,
        .Nic = m_input.NicResource,
        .NicAssignable = &m_nic,
        .XhciAssignable = m_xhci,
        .UsbHostAssignable = &m_usbHost,
        .ScanoutRegion = &m_scanout,
        .AhciDiskWritable = &m_ahciDiskWritable,
        .VtdPlan = &m_vtdPlan,
        .VtdError = &m_vtdSelection.Error,
        .HandoffContext = this,
        .Handoff = HandoffIommuCallback,
        .DmaRefused = &m_iommuHandoffRefused,
    };
    m_portIo.Write(0x21U, 1U, 0xFFU);
    m_portIo.Write(0xA1U, 1U, 0xFFU);
    if (!Device::RunAmd64BootstrapDevices(input))
    {
        return false;
    }
    m_objects.ConfigurePorts(m_portIo);
    if (!PublishI8042())
    {
        for (auto& region : m_i8042Ports)
        {
            static_cast<void>(region.Quiesce());
        }
        for (auto& identity : m_i8042)
        {
            if (identity.IsValid())
            {
                static_cast<void>(m_issuer.DestroyObject(identity));
                static_cast<void>(m_issuer.CollectDestroyedObject(identity));
                identity = {};
            }
        }
        m_i8042Publication.Declared = 0U;
    }
    else
    {
        m_i8042Publication.Reason = I8042PublicationReason::Published;
    }
    ::Asteria::Boot::Bootstrap::ReportI8042(m_i8042Publication, m_topology.TrailingReserved);
    ::Asteria::Boot::Bootstrap::RecordBootInventoryI8042(m_i8042Publication.Declared);
    return true;
}

bool BootRuntime::HandoffIommuCallback(void* const context,
                                       Kernel::Device::PciEcamConfigurationSpace& configuration) noexcept
{
    return static_cast<BootRuntime*>(context)->HandoffIommu(configuration);
}

bool BootRuntime::SweepFirmwareDma(Kernel::Device::PciEcamConfigurationSpace& configuration) noexcept
{
    bool swept{true};
    for (Size region{0U}; region < m_topology.PciConfigurationRegionCount; ++region)
    {
        const auto& buses = m_topology.PciConfigurationRegions[region];
        for (Size bus = buses.FirstBus; bus <= buses.LastBus; ++bus)
        {
            for (UInt8 device{0U}; device < 32U; ++device)
            {
                for (UInt8 function{0U}; function < 8U; ++function)
                {
                    const Kernel::Device::PciFunctionAddress address{buses.Segment, static_cast<UInt8>(bus), device,
                                                                     function};
                    Foundation::UInt32 vendor{0U};
                    Foundation::UInt32 header{0U};
                    Foundation::UInt32 classCode{0U};
                    if (configuration.Read32(address, 0U, vendor) != Hal::Device::PciConfigurationReadError::None)
                    {
                        swept = false;
                        break;
                    }
                    if ((vendor & 0xFFFFU) == 0xFFFFU)
                    {
                        if (function == 0U)
                        {
                            break;
                        }
                        continue;
                    }
                    if ((configuration.Read32(address, 0x0CU, header) !=
                         Hal::Device::PciConfigurationReadError::None) ||
                        (configuration.Read32(address, 8U, classCode) != Hal::Device::PciConfigurationReadError::None))
                    {
                        swept = false;
                        break;
                    }
                    bool reserved{false};
                    for (Size index{0U}; index < m_vtdPlan.Count; ++index)
                    {
                        reserved = reserved || (m_vtdPlan.Functions[index].Address.IsEqualTo(address) &&
                                                (m_vtdPlan.Functions[index].Regions != 0U));
                    }
                    const bool own = m_amdViSelected && (address.GetRequesterId() == m_topology.Ivrs.IommuDevice);
                    const bool bridge = ((header >> 16U) & 0x7FU) == 1U;
                    if (!reserved && !own && !bridge && ((classCode >> 24U) != 3U))
                    {
                        const bool stopped =
                            Kernel::Device::SetPciBusMastering(configuration, configuration, address, false);
                        swept = swept && stopped;
                    }
                    if ((function == 0U) && ((header & (Foundation::UInt32{1U} << 23U)) == 0U))
                    {
                        break;
                    }
                }
            }
        }
    }
    return swept;
}

bool BootRuntime::HandoffIommu(Kernel::Device::PciEcamConfigurationSpace& configuration) noexcept
{
    if (m_topology.IommuHandoffCount == 0U)
    {
        return m_topology.IommuHandoffUnsafe == 0U;
    }
    // This full ECAM sweep also covers a failed/truncated catalog. No kernel
    // DMA has begun, and all known units are attempted even after a refusal.
    bool ready = SweepFirmwareDma(configuration);
    Device::Amd64VtdRegisters scratchRegisters;
    const Platform::Device::VtdFirmwareMemoryOwnership ownership{m_input.PoolPhysicalStart, Bootstrap::RuntimePoolPages,
                                                                 m_input.StartupPhysicalStart, 2U};
    scratchRegisters.SetFirmwareMemoryPolicy(*m_input.BootInformation, ownership);
    alignas(Device::AmdViUnit) static UInt8 amdStorage[sizeof(Device::AmdViUnit)]{};
    alignas(Platform::Device::VtdUnit) static UInt8 vtdStorage[sizeof(Platform::Device::VtdUnit)]{};
    for (Size index{0U}; index < m_topology.IommuHandoffCount; ++index)
    {
        const auto& descriptor = m_topology.IommuHandoffs[index];
        if (descriptor.Kind == 1U)
        {
            m_vtdHandoffRecords[index].RegisterBase = descriptor.RegisterBase;
        }
        scratchRegisters.Bind(descriptor.RegisterBase);
        if (descriptor.Kind == 2U)
        {
            Device::AmdViUnit* unit{nullptr};
            if (m_amdViSelected && (descriptor.RegisterBase == m_topology.Ivrs.RegisterBase))
            {
                m_vtdRegisters[0U].Bind(descriptor.RegisterBase);
                m_amdVi = new (m_amdViStorage) Device::AmdViUnit{m_vtdRegisters[0U], m_vtdCache};
                unit = m_amdVi;
            }
            else
            {
                unit = new (amdStorage) Device::AmdViUnit{scratchRegisters, m_vtdCache};
            }
            m_amdHandoffRecords[index].RegisterBase = descriptor.RegisterBase;
            const bool disabled = unit->PrepareDisabled() == Platform::Device::VtdUnitError::None;
            unit->CopyHandoffRecord(m_amdHandoffRecords[index]);
            ready = ready && disabled;
        }
        else
        {
            Platform::Device::VtdUnit* unit{nullptr};
            Size selected{IommuUnits};
            for (Size slot{0U}; (slot < m_topology.DmaRemappingUnitCount) && (slot < IommuUnits); ++slot)
            {
                if (m_topology.DmaRemappingUnits[slot].RegisterBaseAddress == descriptor.RegisterBase)
                {
                    m_vtdRegisters[slot].Bind(descriptor.RegisterBase);
                    m_vtdRegisters[slot].SetFirmwareMemoryPolicy(*m_input.BootInformation, ownership);
                    m_vtd[slot] = new (m_vtdStorage[slot]) Platform::Device::VtdUnit{m_vtdRegisters[slot], m_vtdCache};
                    unit = m_vtd[slot];
                    selected = slot;
                    break;
                }
            }
            if (unit == nullptr)
            {
                unit = new (vtdStorage) Platform::Device::VtdUnit{scratchRegisters, m_vtdCache};
            }
            const bool disabled = unit->PrepareDisabled() == Platform::Device::VtdUnitError::None;
            unit->CopyHandoffRecord(m_vtdHandoffRecords[index]);
            if (!disabled)
            {
                // No Asteria DMA has begun. The unit, failed, keeps the state
                // the firmware left (its queue included) and the unit printed
                // the reason. The boot continues without DMA devices:
                // RunAmd64BootstrapDevices withholds them and EnableIommu
                // takes the RefuseIommu path.
                m_iommuHandoffRefused = true;
                if (selected < IommuUnits)
                {
                    auto& topology = Diagnostics::GetAmd64EvidenceRecord().IommuTopology;
                    topology.Count = m_topology.DmaRemappingUnitCount;
                    topology.Units[selected].RegisterBase = descriptor.RegisterBase;
                    topology.Units[selected].State = 3U;
                    topology.Units[selected].ConfigureError =
                        static_cast<UInt64>(Platform::Device::VtdUnitError::Failed);
                }
            }
        }
    }
    // Failed endpoint quiescence or an unbounded/unmapped handoff inventory
    // cannot establish that ordinary DMA is stopped, so those remain fatal.
    return ready && (m_topology.IommuHandoffUnsafe == 0U);
}

// Sweep ordinary endpoints only. Bridge forwarding, SMM-owned USB and
// firmware display scanout must remain untouched throughout the handoff.
bool BootRuntime::EnableIommu() noexcept
{
    RecordInterrupts();
    auto& backend = Diagnostics::GetAmd64EvidenceRecord().IommuBackend;
    backend.Kind = (m_topology.DmarPhysicalAddress != 0U) ? 1U : (m_amdViSelected ? 2U : 0U);
    Diagnostics::PublishAmd64Evidence(backend.Published);
    Platform::Firmware::WriteDiagnosticAscii(
        (backend.Kind == 1U)
            ? "ASTERIA:AMD64-IOMMU:VT-D\r\n"
            : ((backend.Kind == 2U) ? "ASTERIA:AMD64-IOMMU:AMD-VI\r\n" : "ASTERIA:AMD64-IOMMU:NONE\r\n"));
    if (m_iommuHandoffRefused)
    {
        return RefuseIommu(false);
    }
    if (m_amdViSelected)
    {
        return EnableAmdVi();
    }
    auto& evidence = Diagnostics::GetAmd64EvidenceRecord().Iommu;
    auto& topologyEvidence = Diagnostics::GetAmd64EvidenceRecord().IommuTopology;
    topologyEvidence.IgnoredScopes = m_vtdPlan.IgnoredScopes;
    topologyEvidence.MalformedRmrr = m_topology.MalformedRmrrCount;
    evidence.SelectionError = static_cast<UInt64>(m_vtdSelection.Error);
    if (m_vtdSelection.Error != Platform::Device::VtdTopologyError::None)
    {
        Diagnostics::PublishAmd64Evidence(evidence.Published);
        Diagnostics::PublishAmd64Evidence(topologyEvidence.Published);
        return true;
    }
    topologyEvidence.Count = m_topology.DmaRemappingUnitCount;
    for (Size unit{0U}; unit < topologyEvidence.Count; ++unit)
    {
        auto& record = topologyEvidence.Units[unit];
        record.RegisterBase = m_topology.DmaRemappingUnits[unit].RegisterBaseAddress;
        record.DisabledReason = m_vtdPlan.NoFunctions[unit] ? 4U : (m_vtdPlan.KeepDisabled[unit] ? 1U : 0U);
    }
    for (Size region{0U}; region < m_topology.ReservedMemoryRegionCount; ++region)
    {
        const auto& range = m_topology.ReservedMemoryRegions[region];
        if ((range.Limit >= (UInt64{1U} << m_paging.PhysicalAddressBits)) ||
            !Platform::Device::VtdFirmwareRangeIsSafe(*m_input.BootInformation, range.Base, range.Limit))
        {
            evidence.SelectionError = static_cast<UInt64>(Platform::Device::VtdTopologyError::ReservedMemory);
            Diagnostics::PublishAmd64Evidence(evidence.Published);
            Diagnostics::PublishAmd64Evidence(topologyEvidence.Published);
            m_vtdSelection.Error = Platform::Device::VtdTopologyError::ReservedMemory;
            return true;
        }
    }
    // Authorities were reserved during capture even if selection failed.
    // Never interrupt bridge forwarding, firmware EHCI schedules or scanout.
    bool swept{true};
    for (Size index{0U}; index < m_vtdPlan.Count; ++index)
    {
        const auto& routing = m_vtdPlan.Functions[index];
        if (!Platform::Device::ShouldSweepVtdFunction(routing, m_vtdPlan))
        {
            continue;
        }
        if (!Kernel::Device::SetPciBusMastering(m_pciConfiguration, m_pciConfiguration, routing.Address, false))
        {
            swept = false;
            continue;
        }
        ++topologyEvidence.Units[routing.Unit].SweptFunctions;
        ++evidence.SweptFunctions;
    }
    // Configure every unit and seed every firmware context before enabling any.
    bool configured{true};
    for (Size unit{0U}; unit < m_topology.DmaRemappingUnitCount; ++unit)
    {
        // Attempt every handoff even if an earlier capability check failed:
        // no later firmware unit may be left translating on the fallback path.
        const bool ready = ConfigureIommuUnit(unit);
        configured = configured && ready;
    }
    if (!swept || !configured)
    {
        return RefuseIommu(false);
    }
    for (Size index{0U}; index < m_vtdPlan.Count; ++index)
    {
        if (!PrepareReservedDma(index))
        {
            return RefuseIommu(false);
        }
    }
    bool enabledAny{false};
    for (Size unit{0U}; unit < m_topology.DmaRemappingUnitCount; ++unit)
    {
        if (m_vtdPlan.KeepDisabled[unit])
        {
            continue;
        }
        auto& record = topologyEvidence.Units[unit];
        const auto enabled = m_vtd[unit]->Enable();
        record.EnableError = static_cast<UInt64>(enabled);
        record.State = (enabled == Platform::Device::VtdUnitError::None) ? 2U : 3U;
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AMD64-VTD:UNIT:");
        Hex(unit);
        Platform::Firmware::WriteDiagnosticAscii(":BASE:");
        Hex(record.RegisterBase);
        Platform::Firmware::WriteDiagnosticAscii(":STATE:");
        Hex(record.State);
        Platform::Firmware::WriteDiagnosticAscii("\r\n");
        if (enabled != Platform::Device::VtdUnitError::None)
        {
            // A timed-out enable with TES set has already enabled hardware.
            const bool translating = (m_vtd[unit]->GetSnapshot().Status & (UInt64{1U} << 31U)) != 0U;
            return RefuseIommu(enabledAny || translating);
        }
        enabledAny = true;
    }
    BuildIommuBackends();
    RecordIommu();
    return true;
}

// The BOOT.CFG allowlist for the published DMA functions, once the IOMMU takeover decided: Working
// when a backend serves the IommuDomain pool, Refused when the firmware names a DMAR or an IVRS the
// boot did not use, Absent otherwise. edu is never trusted: its demonstration needs a translating
// IOMMU.
void BootRuntime::ApplyTrustedDma() noexcept
{
    const auto& information = *m_input.BootInformation;
    const Kernel::Device::IommuAvailability availability =
        (m_vtdBackends[0U] != nullptr) ? Kernel::Device::IommuAvailability::Working
                                       : (((m_topology.DmarPhysicalAddress != 0U) ||
                                           (m_topology.Ivrs.PhysicalAddress != 0U) || m_iommuHandoffRefused)
                                              ? Kernel::Device::IommuAvailability::Refused
                                              : Kernel::Device::IommuAvailability::Absent);
    Platform::Device::TrustedDmaCandidate candidates[2U + Abi::PagerXhciControllers + Abi::PagerUsbHostControllers]{};
    candidates[0U] = {m_ahci.Function, ::Asteria::Boot::BootTrustDriver::Ahci, {}};
    candidates[1U] = {m_nic.Function, ::Asteria::Boot::BootTrustDriver::E1000e, {}};
    for (Size index{0U}; index < Abi::PagerXhciControllers; ++index)
    {
        candidates[2U + index] = {m_xhci[index].Function, ::Asteria::Boot::BootTrustDriver::Xhci, {}};
    }
    for (Size index{0U}; index < Abi::PagerUsbHostControllers; ++index)
    {
        candidates[2U + Abi::PagerXhciControllers + index] = {
            m_usbHost[index].Function, ::Asteria::Boot::BootTrustDriver::UsbHost, {}};
    }
    static_cast<void>(m_trustedDma.Apply(m_registry, information.Trust, information.Files, availability, candidates,
                                         2U + Abi::PagerXhciControllers + Abi::PagerUsbHostControllers));
}

bool BootRuntime::EnableAmdVi() noexcept
{
    auto& record = Diagnostics::GetAmd64EvidenceRecord().IommuTopology.Units[0U];
    auto& evidence = Diagnostics::GetAmd64EvidenceRecord().Iommu;
    auto& backend = Diagnostics::GetAmd64EvidenceRecord().IommuBackend;
    backend.MalformedIvrs = m_topology.Ivrs.Malformed;
    backend.SkippedHid = m_topology.Ivrs.SkippedHid;
    backend.IvhdType = m_topology.Ivrs.Type;
    Diagnostics::PublishAmd64Evidence(backend.Published);
    evidence.SelectionError = static_cast<UInt64>(m_vtdSelection.Error);
    if (m_vtdSelection.Error != Platform::Device::VtdTopologyError::None)
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AMD64-AMDVI:REFUSED:REASON:");
        Hex(static_cast<UInt64>(m_vtdSelection.Error));
        Platform::Firmware::WriteDiagnosticAscii("\r\n");
        RecordAmdVi();
        return true;
    }
    Diagnostics::GetAmd64EvidenceRecord().IommuTopology.Count = 1U;
    record.RegisterBase = m_topology.Ivrs.RegisterBase;
    bool reservedSafe{true};
    // Unity ranges must not expose allocatable RAM, including our table pool.
    for (Size region{0U}; region < m_topology.Ivrs.MemoryCount; ++region)
    {
        const auto& memory = m_topology.Ivrs.Memory[region];
        if ((memory.Limit >= (UInt64{1U} << m_paging.PhysicalAddressBits)) ||
            !Platform::Device::VtdFirmwareRangeIsSafe(*m_input.BootInformation, memory.Base, memory.Limit))
        {
            reservedSafe = false;
        }
    }
    bool swept{true};
    Size count{0U};
    for (Size index{0U}; index < m_vtdPlan.Count; ++index)
    {
        const auto& function = m_vtdPlan.Functions[index];
        const auto requester = function.Address.GetRequesterId();
        if (Platform::Device::ShouldSweepVtdFunction(function, m_vtdPlan))
        {
            const bool disabled =
                Kernel::Device::SetPciBusMastering(m_pciConfiguration, m_pciConfiguration, function.Address, false);
            swept = swept && disabled;
            if (disabled)
            {
                ++record.SweptFunctions;
                ++evidence.SweptFunctions;
            }
        }
        if ((function.Policy == Platform::Device::VtdFunctionPolicy::Assignable) || (function.Regions != 0U))
        {
            m_requesters[count++] = requester;
        }
    }
    m_vtdRegisters[0U].Bind(record.RegisterBase);
    if (m_amdVi == nullptr)
    {
        m_amdVi = new (m_amdViStorage) Device::AmdViUnit{m_vtdRegisters[0U], m_vtdCache};
    }
    // Preserve the original handoff evidence even when unity-range validation
    // refuses DMA. Never leave this supported unit on firmware tables.
    const auto handoff = m_amdVi->PrepareDisabled();
    record.ConfigureError = static_cast<UInt64>(handoff);
    if (handoff != Platform::Device::VtdUnitError::None)
    {
        return RefuseIommu(true);
    }
    if (!reservedSafe)
    {
        m_vtdSelection.Error = Platform::Device::VtdTopologyError::ReservedMemory;
        RecordAmdVi();
        return true;
    }
    // Include firmware DMA aliases and special RIDs even without a captured
    // PCI function. Round to a bus; DevTblSize is pages minus one.
    // The retained IVRS ranges may cover buses absent from the PCI catalog.
    // All 512 reserved pages are published for a Ryzen 0003..FFFE range.
    static Kernel::Device::PciFunctionAddress captured[Platform::Device::VtdUnit::MaximumRequesters]{};
    for (Size index{0U}; index < m_vtdPlan.Count; ++index)
    {
        captured[index] = m_vtdPlan.Functions[index].Address;
    }
    const Size devicePages = Device::AmdViDevicePages(m_topology.Ivrs, captured, m_vtdPlan.Count);
    const auto configured = m_amdVi->Configure(
        IommuPage(AmdViDeviceFirstPage), devicePages, IommuPage(AmdViCommandFirstPage), IommuPage(AmdViEventFirstPage),
        IommuPage(AmdViCompletionPage), m_requesters, count, m_topology.Ivrs.Flags);
    record.ConfigureError = static_cast<UInt64>(configured);
    if (!swept || (configured != Platform::Device::VtdUnitError::None))
    {
        return RefuseIommu(false);
    }
    for (Foundation::UInt32 requester{0U}; requester <= 0xFFFFU; ++requester)
    {
        const auto identifier = static_cast<Foundation::UInt16>(requester);
        const UInt8 regions = Device::AmdViReservedRegions(m_topology.Ivrs, identifier);
        if ((regions != 0U) && !PrepareAmdViReserved(identifier, regions))
        {
            return RefuseIommu(false);
        }
    }
    const auto enabled = m_amdVi->Enable();
    record.EnableError = static_cast<UInt64>(enabled);
    record.State = (enabled == Platform::Device::VtdUnitError::None) ? 2U : 3U;
    if (enabled != Platform::Device::VtdUnitError::None)
    {
        return RefuseIommu(m_amdVi->GetSnapshot().Enabled);
    }
    BuildAmdViBackends();
    RecordAmdVi();
    return true;
}

void BootRuntime::BuildAmdViBackends() noexcept
{
    for (Size slot{0U}; slot < IommuDomainSlots; ++slot)
    {
        Platform::Device::VtdTablePage pages[Device::AmdViDomainBackend::TablePages]{};
        for (Size index{0U}; index < Device::AmdViDomainBackend::TablePages; ++index)
        {
            pages[index] = IommuPage(IommuDomainTableFirstPage + slot * IommuDomainTablePages + index);
        }
        m_vtdBackends[slot] = new (m_amdViBackendStorage[slot])
            Device::AmdViDomainBackend{*m_amdVi, static_cast<Foundation::UInt16>(slot + 1U), pages};
    }
}

bool BootRuntime::PrepareAmdViReserved(const Foundation::UInt16 requester, const UInt8 regions) noexcept
{
    auto& record = Diagnostics::GetAmd64EvidenceRecord().IommuTopology.Units[0U];
    Size slot{0U};
    while ((slot < m_reservedCount) && (m_amdReservedMasks[slot] != regions))
    {
        ++slot;
    }
    if (slot == m_reservedCount)
    {
        if (m_reservedCount == IommuReservedDomains)
        {
            return false;
        }
        static Platform::Device::VtdTablePage pages[IommuReservedTablePages]{};
        for (Size index{0U}; index < IommuReservedTablePages; ++index)
        {
            pages[index] = IommuPage(IommuReservedTableFirstPage + slot * IommuReservedTablePages + index);
        }
        m_amdReserved[slot] = new (m_amdViReservedStorage[slot]) Device::AmdViDomainBackend{
            *m_amdVi, static_cast<Foundation::UInt16>(5U + slot), pages, IommuReservedTablePages};
        m_amdReservedMasks[slot] = regions;
        ++m_reservedCount;
        if (!m_amdReserved[slot]->PrepareReserved())
        {
            return false;
        }
        for (Size region{0U}; region < m_topology.Ivrs.MemoryCount; ++region)
        {
            if ((regions & (UInt64{1U} << region)) == 0U)
            {
                continue;
            }
            const auto& memory = m_topology.Ivrs.Memory[region];
            const UInt8 directions = ((memory.Flags & 8U) != 0U) ? UInt8{6U} : (memory.Flags & 6U);
            Kernel::Device::DmaPermissions permissions{};
            if ((directions & 2U) != 0U)
            {
                permissions = permissions.With(Kernel::Device::DmaPermission::Read);
            }
            if ((directions & 4U) != 0U)
            {
                permissions = permissions.With(Kernel::Device::DmaPermission::Write);
            }
            if (m_amdReserved[slot]->MapReserved(memory.Base, memory.Limit, permissions) !=
                Hal::Device::IommuBackendError::None)
            {
                return false;
            }
            record.ReservedPages += (memory.Limit - memory.Base + 1U) / 4096U;
        }
    }
    // A permanent IVMD context also exists for an absent PCI requester.
    if (m_amdReserved[slot]->SeedReserved(requester) != Platform::Device::VtdUnitError::None)
    {
        return false;
    }
    ++record.ReservedFunctions;
    return true;
}

void BootRuntime::RecordAmdVi() noexcept
{
    auto& evidence = Diagnostics::GetAmd64EvidenceRecord().Iommu;
    auto& topology = Diagnostics::GetAmd64EvidenceRecord().IommuTopology;
    evidence.SelectionError = static_cast<UInt64>(m_vtdSelection.Error);
    if (m_amdVi != nullptr)
    {
        m_amdVi->PollFaults();
        const auto unit = m_amdVi->GetSnapshot();
        auto& record = topology.Units[0U];
        record.ServedFunctions = unit.Served;
        record.Levels = unit.Levels;
        record.Version = unit.Version;
        record.ExtendedCapability = unit.ExtendedCapability;
        record.HandoffStatus = unit.HandoffStatus;
        record.Status = unit.Status;
        record.Faults = unit.Faults;
        record.Overflows = unit.Overflows;
        record.Unattributed = unit.Unattributed;
        evidence.RegisterBase = record.RegisterBase;
        evidence.ServedFunctions = unit.Served;
        evidence.Enabled = unit.Enabled && !unit.Failed ? 1U : 0U;
        evidence.ConfigureError = record.ConfigureError;
        evidence.EnableError = record.EnableError;
        evidence.Levels = unit.Levels;
        evidence.Version = unit.Version;
        evidence.ExtendedCapability = unit.ExtendedCapability;
        evidence.HandoffStatus = unit.HandoffStatus;
        evidence.Status = unit.Status;
        evidence.Faults = unit.Faults;
        evidence.Overflows = unit.Overflows;
        evidence.Unattributed = unit.Unattributed;
        evidence.LastFaultSource = unit.LastFault.Source;
        evidence.LastFaultReason = unit.LastFault.Reason;
        evidence.LastFaultAddress = unit.LastFault.Address;
        evidence.LastFaultRead = unit.LastFault.Read ? 1U : 0U;
    }
    const auto domains = m_objects.GetIommuDomainSnapshot();
    evidence.DomainsCreated = domains.Created;
    evidence.DomainsReclaimed = domains.Reclaimed;
    Diagnostics::PublishAmd64Evidence(topology.Published);
    Diagnostics::PublishAmd64Evidence(evidence.Published);
}

bool BootRuntime::RefuseIommu(const bool enabledAny) noexcept
{
    m_vtdSelection.Error = Platform::Device::VtdTopologyError::Hardware;
    auto& topology = Diagnostics::GetAmd64EvidenceRecord().IommuTopology;
    for (Size unit{0U}; unit < topology.Count; ++unit)
    {
        if (topology.Units[unit].State == 0U)
        {
            topology.Units[unit].DisabledReason = 3U;
        }
    }
    RecordIommu();
    Platform::Firmware::WriteDiagnosticAscii(m_amdViSelected ? "ASTERIA:AMD64-AMDVI:REFUSED:PRE-ENABLE:"
                                                             : "ASTERIA:AMD64-VTD:REFUSED:PRE-ENABLE:");
    Hex(enabledAny ? 0U : 1U);
    Platform::Firmware::WriteDiagnosticAscii("\r\n");
    return !enabledAny;
}

bool BootRuntime::ConfigureIommuUnit(const Size unit) noexcept
{
    auto& record = Diagnostics::GetAmd64EvidenceRecord().IommuTopology.Units[unit];
    record.RegisterBase = m_topology.DmaRemappingUnits[unit].RegisterBaseAddress;
    m_vtdRegisters[unit].Bind(record.RegisterBase);
    m_vtdRegisters[unit].SetFirmwareMemoryPolicy(
        *m_input.BootInformation,
        Platform::Device::VtdFirmwareMemoryOwnership{m_input.PoolPhysicalStart, Bootstrap::RuntimePoolPages,
                                                     m_input.StartupPhysicalStart, 2U});
    if (m_vtd[unit] == nullptr)
    {
        m_vtd[unit] = new (m_vtdStorage[unit]) Platform::Device::VtdUnit{m_vtdRegisters[unit], m_vtdCache};
    }
    if (m_vtdPlan.KeepDisabled[unit])
    {
        const auto observed = m_vtd[unit]->PrepareDisabled();
        record.ConfigureError = static_cast<UInt64>(observed);
        if (observed != Platform::Device::VtdUnitError::None)
        {
            record.State = 3U;
            return false;
        }
        record.State = 1U;
        Platform::Firmware::WriteDiagnosticAscii(m_vtdPlan.NoFunctions[unit] ? "ASTERIA:AMD64-VTD:UNIT-EMPTY-OFF:"
                                                                             : "ASTERIA:AMD64-VTD:DISPLAY-OFF:");
        Hex(unit);
        Platform::Firmware::WriteDiagnosticAscii(":BASE:");
        Hex(record.RegisterBase);
        Platform::Firmware::WriteDiagnosticAscii("\r\n");
        return true;
    }
    Size count{0U};
    for (Size index{0U}; index < m_vtdPlan.Count; ++index)
    {
        if (m_vtdPlan.Functions[index].Unit == unit)
        {
            m_requesters[count++] = m_vtdPlan.Functions[index].Address.GetRequesterId();
        }
    }
    const Size first = IommuContextTableFirstPage + (unit * IommuUnitPages);
    Platform::Device::VtdTablePage contexts[Platform::Device::VtdUnit::MaximumBuses]{};
    for (Size bus{0U}; bus < Platform::Device::VtdUnit::MaximumBuses; ++bus)
    {
        contexts[bus] = IommuPage(first + bus);
    }
    if (m_vtdPlan.DisplayConflict[unit])
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AMD64-VTD:SHARED-DISPLAY:");
        Hex(unit);
        Platform::Firmware::WriteDiagnosticAscii("\r\n");
    }
    const auto configured = m_vtd[unit]->Configure(
        IommuPage(IommuRootTablePage + (unit * IommuUnitPages)), contexts, m_requesters, count,
        static_cast<Size>(Platform::Device::VtdRegisterBytes(m_topology.DmaRemappingUnits[unit])));
    record.ConfigureError = static_cast<UInt64>(configured);
    if (configured != Platform::Device::VtdUnitError::None)
    {
        record.State = 3U;
        return false;
    }
    return true;
}

bool BootRuntime::PrepareReservedDma(const Size function) noexcept
{
    const auto& routing = m_vtdPlan.Functions[function];
    if ((routing.Regions == 0U) || m_vtdPlan.KeepDisabled[routing.Unit])
    {
        return true;
    }
    if (m_reservedCount == IommuReservedDomains)
    {
        return false;
    }
    auto& record = Diagnostics::GetAmd64EvidenceRecord().IommuTopology.Units[routing.Unit];
    static Platform::Device::VtdTablePage pages[IommuReservedTablePages]{};
    for (Size index{0U}; index < IommuReservedTablePages; ++index)
    {
        pages[index] = IommuPage(IommuReservedTableFirstPage + (m_reservedCount * IommuReservedTablePages) + index);
    }
    auto* const backend = new (m_reservedStorage[m_reservedCount]) Platform::Device::VtdDomainBackend{
        *m_vtd[routing.Unit], static_cast<Foundation::UInt16>(5U + m_reservedCount), pages, IommuReservedTablePages};
    m_reserved[m_reservedCount++] = backend;
    if (!backend->PrepareReserved())
    {
        return false;
    }
    for (Size region{0U}; region < m_topology.ReservedMemoryRegionCount; ++region)
    {
        if ((routing.Regions & (UInt64{1U} << region)) == 0U)
        {
            continue;
        }
        const auto& range = m_topology.ReservedMemoryRegions[region];
        // Firmware ranges must never expose the boot's reserved table pool.
        const UInt64 poolEnd = m_input.PoolPhysicalStart + (RuntimePoolPages * 4096U);
        if ((range.Base < poolEnd) && (range.Limit >= m_input.PoolPhysicalStart))
        {
            return false;
        }
        if (backend->MapReserved(range.Base, range.Limit) != Hal::Device::IommuBackendError::None)
        {
            return false;
        }
        record.ReservedPages += (range.Limit - range.Base + 1U) / 4096U;
    }
    if (backend->SeedReserved(routing.Address.GetRequesterId()) != Platform::Device::VtdUnitError::None)
    {
        return false;
    }
    ++record.ReservedFunctions;
    return true;
}

Platform::Device::VtdTablePage BootRuntime::IommuPage(const Size page) const noexcept
{
    const UInt64 physical = PoolPage(m_input.PoolPhysicalStart, page);
    return Platform::Device::VtdTablePage{
        reinterpret_cast<volatile UInt64*>(static_cast<Foundation::UIntPointer>(physical)), physical};
}

// One domain backend per IommuDomain slot, each with its eight table pages,
// once the unit translates.
void BootRuntime::BuildIommuBackends() noexcept
{
    static_assert(IommuDomainSlots == Kernel::Device::IommuDomainPool::Capacity);
    for (Size slot{0U}; slot < IommuDomainSlots; ++slot)
    {
        Platform::Device::VtdDomainBackend* backends[IommuUnits]{};
        Platform::Device::VtdUnit* units[IommuUnits]{};
        Size count{0U};
        for (Size unit{0U}; unit < m_topology.DmaRemappingUnitCount; ++unit)
        {
            if (!m_vtd[unit]->IsEnabled())
            {
                continue;
            }
            Platform::Device::VtdTablePage pages[Platform::Device::VtdDomainBackend::TablePages]{};
            for (Size index{0U}; index < Platform::Device::VtdDomainBackend::TablePages; ++index)
            {
                pages[index] = IommuPage(IommuDomainTableFirstPage + (unit * IommuUnitPages) +
                                         (slot * IommuDomainTablePages) + index);
            }
            backends[count] = new (m_vtdBackendStorage[slot][unit])
                Platform::Device::VtdDomainBackend{*m_vtd[unit], static_cast<Foundation::UInt16>(slot + 1U), pages};
            units[count++] = m_vtd[unit];
        }
        if (count != 0U)
        {
            m_vtdBackends[slot] =
                new (m_vtdGroupStorage[slot]) Platform::Device::VtdDomainGroup{backends, units, count};
        }
    }
}

void BootRuntime::RecordIommu() noexcept
{
    if (m_amdViSelected)
    {
        RecordAmdVi();
        return;
    }
    auto& evidence = Diagnostics::GetAmd64EvidenceRecord().Iommu;
    auto& topology = Diagnostics::GetAmd64EvidenceRecord().IommuTopology;
    evidence.SelectionError = static_cast<UInt64>(m_vtdSelection.Error);
    evidence.ServedFunctions = 0U;
    evidence.Enabled = 0U;
    evidence.Faults = 0U;
    evidence.Overflows = 0U;
    evidence.Unattributed = 0U;
    for (Size index{0U}; index < topology.Count; ++index)
    {
        if (m_vtd[index] == nullptr)
        {
            continue;
        }
        auto& record = topology.Units[index];
        if (!m_vtd[index]->GetSnapshot().Failed)
        {
            m_vtd[index]->PollFaults();
        }
        const auto unit = m_vtd[index]->GetSnapshot();
        record.ServedFunctions = unit.Served;
        record.DroppedRequesters = unit.DroppedRequesters;
        record.Levels = unit.Levels;
        record.Version = unit.Version;
        record.Capability = unit.Capability;
        record.ExtendedCapability = unit.ExtendedCapability;
        record.HandoffStatus = unit.HandoffStatus;
        record.Status = unit.Status;
        record.Faults = unit.Faults;
        record.Overflows = unit.Overflows;
        record.Unattributed = unit.Unattributed;
        if (record.State == 1U)
        {
            continue;
        }
        if (evidence.Enabled == 0U)
        {
            evidence.RegisterBase = record.RegisterBase;
            evidence.Levels = unit.Levels;
            evidence.Version = unit.Version;
            evidence.Capability = unit.Capability;
            evidence.ExtendedCapability = unit.ExtendedCapability;
            evidence.HandoffStatus = unit.HandoffStatus;
            evidence.Status = unit.Status;
            evidence.ConfigureError = record.ConfigureError;
            evidence.EnableError = record.EnableError;
        }
        evidence.ServedFunctions += unit.Served;
        evidence.Enabled += (unit.Enabled && !unit.Failed) ? 1U : 0U;
        evidence.Faults += unit.Faults;
        evidence.Overflows += unit.Overflows;
        evidence.Unattributed += unit.Unattributed;
        if (unit.Faults != 0U)
        {
            evidence.LastFaultSource = unit.LastFault.Source;
            evidence.LastFaultReason = unit.LastFault.Reason;
            evidence.LastFaultAddress = unit.LastFault.Address;
            evidence.LastFaultRead = unit.LastFault.Read ? 1U : 0U;
        }
    }
    const auto domains = m_objects.GetIommuDomainSnapshot();
    evidence.DomainsCreated = domains.Created;
    evidence.DomainsReclaimed = domains.Reclaimed;
    Diagnostics::PublishAmd64Evidence(topology.Published);
    Diagnostics::PublishAmd64Evidence(evidence.Published);
}

// Device interrupts: the pool's view and the device vectors' deliveries; and the control of
// assigned functions.
void BootRuntime::RecordInterrupts() noexcept
{
    auto& evidence = Diagnostics::GetAmd64EvidenceRecord().Interrupts;
    const Kernel::Interrupt::InterruptPoolSnapshot pool = m_objects.GetInterruptSnapshot();
    evidence.Created = pool.Created;
    evidence.Destroyed = pool.Destroyed;
    evidence.Reclaimed = pool.Reclaimed;
    evidence.Live = pool.ActiveSlots + pool.QuiescingSlots;
    evidence.Delivered = m_deviceVectors.GetDelivered();
    evidence.Spurious = m_deviceVectors.GetSpurious();
    Diagnostics::PublishAmd64Evidence(evidence.Published);
    auto& control = Diagnostics::GetAmd64EvidenceRecord().DeviceControl;
    const Kernel::Device::PciFunctionControlSnapshot functions = m_objects.GetDeviceControlSnapshot();
    control.Assignments = functions.Assignments;
    control.Releases = functions.Releases;
    control.BusMasterRefusals = functions.BusMasterRefusals;
    control.Quarantines = functions.Quarantines;
    control.Confirmations = functions.Confirmations;
    control.PinsParked = functions.PinsParked;
    control.PinsReleased = functions.PinsReleased;
    control.ParkedPins = functions.ParkedPins;
    Diagnostics::PublishAmd64Evidence(control.Published);
    // What the scrubs found in the released pins' pages.
    auto& block = Diagnostics::GetAmd64EvidenceRecord().Block;
    for (Size slot{0U}; slot < Memory::Amd64FrameScrubberWatches; ++slot)
    {
        const Memory::Amd64FrameScrubSample& sample = m_scrubber.GetSample(slot);
        Diagnostics::Amd64ParkedReleaseEvidence& released = block.Released[slot];
        if ((sample.Physical == 0U) || (sample.Physical != released.PhysicalStart) || (sample.Sampled == 0U))
        {
            continue;
        }
        released.Sampled = 1U;
        for (Size word{0U}; word < Memory::Amd64FrameScrubberSampleWords; ++word)
        {
            released.Sample[word] = sample.Words[word];
        }
    }
    Diagnostics::PublishAmd64Evidence(block.Published);
}

bool BootRuntime::InitializeResourceDomain() noexcept
{
    // Child domains: the pager's grant and, under it, the components' own; threads: the grant's,
    // one per thread-pool slot, beside sixteen of the boot's own.
    constexpr Kernel::Resource::ResourceDomainLimits limits{
        4096U, 4096U, 16U * 1024U * 1024U, 1024U, 1024U * 1024U, 16U + PagerGrantThreads, 1U + PagerGrantChildDomains};
    if (m_resources.InitializeRoot(BootstrapCharge, limits).Error != Kernel::Resource::ResourceDomainError::None)
    {
        return false;
    }
    const auto registered = m_issuer.CreateObject(m_resources);
    if (!registered.HasValue())
    {
        return false;
    }
    m_resourceId = {m_registry, registered.GetValue()};
    return true;
}

bool BootRuntime::AttachSlabs() noexcept
{
    // Before ExitBootServices the firmware's identity map reaches the arena;
    // afterwards every root's seed maps it at the same addresses.
    const UInt64 arena = m_input.MetadataArenaPhysicalStart;
    return m_registry.AttachEntries(Pointer(ArenaPage(arena, MetadataArenaRegistryFirstPage)),
                                    Kernel::Object::ObjectRegistry::MaximumObjects) &&
           m_objects.AttachSlabs(Pointer(ArenaPage(arena, MetadataArenaFrameSlabFirstPage)),
                                 Pointer(ArenaPage(arena, MetadataArenaMemoryObjectSlabFirstPage)));
}

void BootRuntime::RecordMetadata() noexcept
{
    auto& evidence = Diagnostics::GetAmd64EvidenceRecord().Metadata;
    evidence.ArenaPhysicalStart = m_input.MetadataArenaPhysicalStart;
    evidence.ArenaPages = m_input.MetadataArenaPages;
    evidence.SlabFirstPage = MetadataArenaSlabFirstPage;
    evidence.BitmapFirstPage = MetadataArenaBitmapFirstPage;
    // Before any run-time mapping of the shared root: its seed's tables.
    evidence.SeedTablePages = m_pageTables[SharedRootIndex].GetSnapshot().UsedTablePages;
    evidence.RegistryCapacity = m_registry.GetCapacity();
    const auto frames = m_objects.GetFrameSnapshot();
    const auto memory = m_objects.GetMemoryObjectSnapshot();
    evidence.FrameSlots = frames.FreeSlots + frames.ActiveSlots + frames.QuiescingSlots + frames.ZeroingSlots;
    evidence.MemoryObjectSlots = memory.FreeSlots + memory.ActiveSlots + memory.QuiescingSlots;
    Diagnostics::PublishAmd64Evidence(evidence.Published);
}

bool BootRuntime::PrepareRoot(const Size index) noexcept
{
    const UInt64 physical = PoolPage(m_input.PoolPhysicalStart, BootRootArenaPage(index));
    auto* const entries = reinterpret_cast<Foundation::UInt64*>(static_cast<Foundation::UIntPointer>(physical));
    const Memory::PageTableStorage storage{PhysicalAddress{physical}, entries, RootArenaPages, 0U};
    if (!m_preparations[index].Prepare(m_seed, m_paging, storage).HasValue())
    {
        return false;
    }
    const auto plan = m_preparations[index].TryGetMappingPlan();
    if (!plan.HasValue() || m_pageTables[index].Initialize(m_paging, m_preparations[index], entries) !=
                                Memory::RuntimeAddressSpacePageTableInitializationError::None)
    {
        return false;
    }
    if (m_spaces[index].Initialize(BootstrapCharge, m_pageTables[index], *plan.GetValue()) != AddressSpaceError::None)
    {
        return false;
    }
    const auto registered = m_issuer.CreateObject(m_spaces[index]);
    if (!registered.HasValue())
    {
        return false;
    }
    m_spaceIds[index] = {m_registry, registered.GetValue()};
    return m_spaces[index].BindIdentity(m_registry, m_spaceIds[index]).Error == AddressSpaceError::None;
}

bool BootRuntime::StageImage(const Size image, const ::Asteria::Executable::Elf::Elf64ByteView bytes) noexcept
{
    auto& domain = m_images[image];
    const UInt64 staging = PoolPage(m_input.PoolPhysicalStart, ImageStagingPages[image]);
    const auto staged =
        domain.Image.Prepare(bytes, Pointer(staging), staging, Executable::InitialUserStoragePages * 4096U);
    if (staged != Executable::Amd64InitialUserImageError::None)
    {
        return Reject("IMAGE-STAGING", static_cast<UInt64>(staged));
    }
    if (image == PagerImage)
    {
        const auto& info = *m_input.BootInformation;
        if (!::Asteria::Boot::ValidateBootModules(info))
        {
            return Reject("MODULE-REGION");
        }
        const UInt64 start = info.Files.PhysicalStart + info.Files.ModuleOffset;
        const UInt64 end = start + info.Files.ModuleBytes;
        for (Size index{0U}; index < m_seed.GetMappingCount(); ++index)
        {
            const auto mapping = m_seed.GetMapping(index);
            if (!mapping.HasValue())
            {
                return Reject("MODULE-SEED");
            }
            const auto& range = mapping.GetValue();
            if (start < range.PhysicalStart.GetValue() + range.Pages.GetValue() * 4096U &&
                range.PhysicalStart.GetValue() < end)
            {
                return Reject("MODULE-ALIAS");
            }
        }
        if (!domain.Image.MapModules(start, info.Files.ModuleBytes))
        {
            return Reject("MODULE-RANGE");
        }
    }
    for (Size index{0U}; index < domain.Image.GetRangeCount(); ++index)
    {
        if (!MapUserRange(image, index))
        {
            return Reject("IMAGE-MAPPING", index);
        }
    }
    return domain.Access.Initialize(m_spaceIds[ImageRoots[image]], domain.Mappings, domain.Image.GetRangeCount(),
                                    &m_spaces[ImageRoots[image]]) == Kernel::Memory::DirectUserMemoryAccessError::None;
}

bool BootRuntime::MapUserRange(const Size image, const Size index) noexcept
{
    auto& domain = m_images[image];
    const auto* const range = domain.Image.GetRange(index);
    if (range == nullptr)
    {
        return false;
    }
    const Kernel::Memory::FrameDescriptor descriptor{PhysicalAddress{range->PhysicalStart},
                                                     PageCount{range->Pages},
                                                     MemoryCachePolicy::WriteBack,
                                                     Kernel::Memory::FrameKind::Ram,
                                                     {}};
    const auto initialized = image == PagerImage && range->VirtualStart == Abi::Amd64PagerModuleBase
                                 ? domain.Frames[index].InitializeReadOnly(BootstrapCharge, descriptor)
                                 : domain.Frames[index].Initialize(BootstrapCharge, descriptor);
    if (initialized != Kernel::Memory::FrameError::None)
    {
        return false;
    }
    if (domain.MemoryObjects[index]
            .Initialize(m_registry, m_resourceId, m_resources,
                        {PageCount{range->Pages},
                         MemoryCachePolicy::WriteBack,
                         range->Permissions,
                         Kernel::Memory::MemoryCommitment::Pinned,
                         {}})
            .Error != Kernel::Memory::MemoryObjectError::None)
    {
        return false;
    }
    const auto frame = m_issuer.CreateObject(domain.Frames[index]);
    const auto memory = m_issuer.CreateObject(domain.MemoryObjects[index]);
    if (!frame.HasValue() || !memory.HasValue())
    {
        return false;
    }
    return MapPreparedUserRange(image, index, frame.GetValue(), memory.GetValue());
}

bool BootRuntime::MapPreparedUserRange(const Size image, const Size index, const Kernel::Object::ObjectId frame,
                                       const Kernel::Object::ObjectId memory) noexcept
{
    auto& domain = m_images[image];
    const Size root = ImageRoots[image];
    const auto* const range = domain.Image.GetRange(index);
    if (range == nullptr)
    {
        return false;
    }
    Kernel::Memory::PinnedFrameMappingPreparation prepared;
    const Kernel::Memory::PinnedFrameMappingRequest request{
        VirtualAddress{range->VirtualStart}, PageCount{range->Pages}, 0U, 0U, range->Permissions, {}};
    if (prepared.Prepare(m_registry, {m_registry, memory}, domain.MemoryObjects[index], {m_registry, frame},
                         domain.Frames[index], request) != Kernel::Memory::PinnedFrameMappingError::None)
    {
        return false;
    }
    const auto begun = m_spaces[root].BeginPinnedFrameMapping(m_spaceIds[root], prepared);
    if (!begun.HasValue() || !m_spaces[root].CompleteMapping(begun.GetValue().GetToken()).HasValue())
    {
        return false;
    }
    domain.Mappings[index] = {VirtualAddress{range->VirtualStart}, Pointer(range->VirtualStart),
                              PageCount{range->Pages}, range->Permissions, Kernel::Memory::FrameKind::Ram};
    return true;
}

// The frame pool is zeroed here, under the loader's identity map: no root maps it for the kernel
// afterwards.
void BootRuntime::ZeroFramePool() noexcept
{
}

// Each lazy page starts with LazyPagePattern and its page index, so the
// client can tell it reads the pager's Frame; the rest of the page is zero.
void BootRuntime::FillLazyRegion() noexcept
{
    for (Size page{0U}; page < LazyRegionPages; ++page)
    {
        const UInt64 address = PoolPage(m_input.PoolPhysicalStart, LazyRegionFirstPage + page);
        static_cast<void>(memset(Pointer(address), 0, RuntimePoolPageBytes));
        auto* const words = reinterpret_cast<UInt64*>(static_cast<Foundation::UIntPointer>(address));
        words[0U] = Abi::LazyPagePattern;
        words[1U] = page;
    }
}

bool BootRuntime::Prepare(const Amd64BootRuntimeInput& input) noexcept
{
    if (m_prepared)
    {
        if ((input.PoolPhysicalStart != m_input.PoolPhysicalStart) ||
            (input.StartupPhysicalStart != m_input.StartupPhysicalStart) ||
            (input.MetadataArenaPhysicalStart != m_input.MetadataArenaPhysicalStart) ||
            (input.MetadataArenaPages != m_input.MetadataArenaPages) ||
            (input.UserImage.Data != m_input.UserImage.Data) ||
            (input.UserImage.SizeBytes != m_input.UserImage.SizeBytes) ||
            (input.PagerImage.Data != m_input.PagerImage.Data) ||
            (input.PagerImage.SizeBytes != m_input.PagerImage.SizeBytes) ||
            (input.SpinnerImage.Data != m_input.SpinnerImage.Data) ||
            (input.SpinnerImage.SizeBytes != m_input.SpinnerImage.SizeBytes) ||
            memcmp(&input.AhciResource, &m_input.AhciResource, sizeof(input.AhciResource)) != 0 ||
            (memcmp(input.Capabilities, &m_paging, sizeof(m_paging)) != 0) ||
            (memcmp(&input.BootInformation->Acpi, &m_topology, sizeof(m_topology)) != 0) ||
            (input.InitialMappings->GetMappingCount() != m_originalMappingCount))
        {
            return Reject("RETRY");
        }
        for (Size index{0U}; index < m_originalMappingCount; ++index)
        {
            const auto mapping = input.InitialMappings->GetMapping(index);
            if (!mapping.HasValue() ||
                memcmp(&mapping.GetValue(), &m_originalMappings[index], sizeof(InitialMapping)) != 0)
            {
                return Reject("RETRY-MAPPING", index);
            }
        }
        return true;
    }
    if (m_attempted)
    {
        return Reject("PREVIOUS-FAILURE");
    }
    m_attempted = true;
#if defined(ASTERIA_TARGET_AMD64)
    // The entry install enables FXSAVE and SSE on every CPU.
    Foundation::UInt32 eax{0U};
    Foundation::UInt32 ebx{0U};
    Foundation::UInt32 ecx{0U};
    Foundation::UInt32 edx{0U};
    __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1U), "c"(0U));
    if ((edx & Interrupt::RequiredProcessorFeatures) != Interrupt::RequiredProcessorFeatures)
    {
        return Reject("CPU-FEATURES");
    }
#endif
    m_input = input;
    if (!AttachSlabs())
    {
        return Reject("METADATA-ARENA");
    }
    m_paging = *input.Capabilities;
    static_cast<void>(memcpy(&m_topology, &input.BootInformation->Acpi, sizeof(m_topology)));
    if (m_topology.Reserved != 0U && m_topology.Dsdt.PhysicalAddress != 0U && m_topology.Dsdt.Length >= 36U &&
        m_topology.Dsdt.Length <= 1024U * 1024U)
    {
        // Firmware ABI boundary: the final-map parser validated this immutable,
        // identity-addressable table. Re-read before switching to kernel maps.
        const UInt8* const bytes = reinterpret_cast<const UInt8*>(m_topology.Dsdt.PhysicalAddress);
        Foundation::UInt32 length{0U};
        Foundation::UInt32 sum{0U};
        for (Size byte{0U}; byte < 4U; ++byte)
        {
            length |= Foundation::UInt32{bytes[4U + byte]} << (byte * 8U);
        }
        if (length == m_topology.Dsdt.Length)
        {
            for (Size byte{0U}; byte < length; ++byte)
            {
                sum = (sum + bytes[byte]) & 255U;
            }
            m_i8042DsdtMatches =
                sum == 0U && bytes[0U] == 'D' && bytes[1U] == 'S' && bytes[2U] == 'D' && bytes[3U] == 'T' &&
                ::Asteria::Boot::Acpi::I8042Evidence::Matches(m_topology, m_topology.Dsdt.PhysicalAddress, length,
                                                              bytes[9U]);
        }
    }
    m_originalMappingCount = input.InitialMappings->GetMappingCount();
    if (m_originalMappingCount > 16U)
    {
        return Reject("MAPPING-COUNT");
    }
    for (Size index{0U}; index < m_originalMappingCount; ++index)
    {
        const auto mapping = input.InitialMappings->GetMapping(index);
        if (!mapping.HasValue() || m_seed.AddMapping(mapping.GetValue()) != InitialMappingPlanError::None)
        {
            return Reject("INITIAL-MAPPING", index);
        }
        m_originalMappings[index] = mapping.GetValue();
    }
    if (!AddPoolMappings() || m_seed.Seal() != InitialMappingPlanError::None)
    {
        return Reject("SEED");
    }
    if (!InitializeResourceDomain())
    {
        return Reject("RESOURCE");
    }
    if (!PrepareRoot(SharedRootIndex))
    {
        return Reject("SHARED-ROOT");
    }
    if (!PrepareRoot(UserRootIndex))
    {
        return Reject("USER-ROOT");
    }
    if (!PrepareRoot(PagerRootIndex))
    {
        return Reject("PAGER-ROOT");
    }
    m_hasSpinner = input.SpinnerImage.SizeBytes != 0U;
    // Management uses the scrub slot in every reserved root, including the
    // unused spinner root of production firmware.
    if (!PrepareRoot(SpinnerRootIndex))
    {
        return Reject("SPINNER-ROOT");
    }
    for (Size cpu{1U}; cpu < ProcessorStackSlots; ++cpu)
    {
        if (!PrepareRoot(IdleRootIndex(cpu)))
        {
            return Reject("AP-IDLE-ROOT", cpu);
        }
    }
    if (!PrepareSharedProbe())
    {
        return Reject("SHARED-PROBE");
    }
    if (!StageImage(ClientImage, input.UserImage))
    {
        return Reject("USER-IMAGE");
    }
    if (!StageImage(PagerImage, input.PagerImage))
    {
        return Reject("PAGER-IMAGE");
    }
    if (m_hasSpinner && !StageImage(SpinnerImage, input.SpinnerImage))
    {
        return Reject("SPINNER-IMAGE");
    }
    FillLazyRegion();
    ZeroFramePool();
    m_clientStackTop = StackPage(input.PoolPhysicalStart, 0U, SystemCallStackPage + KernelStackPages);
    for (Size cpu{0U}; cpu < ProcessorStackSlots; ++cpu)
    {
        // RSP0 and the SYSCALL stack top at the system-call stack's end; the
        // double-fault and NMI/machine-check ISTs at their stacks' ends.
        m_entryContexts[cpu] = {StackPage(input.PoolPhysicalStart, cpu, SystemCallStackPage + KernelStackPages), 0U,
                                cpu, 0U};
#if defined(ASTERIA_TARGET_AMD64)
        if (Interrupt::Amd64UserEntry::Prepare(
                m_descriptors[cpu], m_entryContexts[cpu],
                StackPage(input.PoolPhysicalStart, cpu, DoubleFaultStackPage + InterruptStackPages),
                PoolPage(input.PoolPhysicalStart, NmiStackPage(cpu) + InterruptStackPages),
                StackPage(input.PoolPhysicalStart, cpu, MachineCheckStackPage + InterruptStackPages),
                Interrupt::AsteriaAmd64InterruptHandlers, 256U) != Interrupt::Amd64UserEntryError::None)
        {
            return Reject("ENTRY", cpu);
        }
#endif
    }
    if (!PrepareProcessors())
    {
        return Reject("PROCESSORS");
    }
    m_calibration = Time::CalibrateTsc(m_topology);
    if (m_calibration.FrequencyHz == 0U)
    {
        return Reject("TSC-CALIBRATION");
    }
    Time::SetTscFrequency(m_calibration.FrequencyHz);
    Platform::Synchronization::ConfigureProcessorWatchdog(input.BootInformation->WatchdogSeconds,
                                                          m_calibration.FrequencyHz);
    Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AMD64-TSC:SOURCE=");
    Platform::Firmware::WriteDiagnosticAscii(Time::TscSourceName(m_calibration.Source));
    Platform::Firmware::WriteDiagnosticAscii(":HZ=");
    Hex(m_calibration.FrequencyHz);
    Platform::Firmware::WriteDiagnosticAscii("\r\n");
    Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AMD64-APIC:MODE=");
#if defined(ASTERIA_TARGET_AMD64)
    Interrupt::Amd64LocalApic bootApic;
    if (!bootApic.Initialize(m_topology.LocalControllerPhysicalAddress))
    {
        return Reject("APIC-STATE");
    }
    Platform::Firmware::WriteDiagnosticAscii(bootApic.IsX2Apic() ? "X2APIC" : "XAPIC");
#else
    Platform::Firmware::WriteDiagnosticAscii("XAPIC");
#endif
    Platform::Firmware::WriteDiagnosticAscii(":DEVICE-IRQS=BSP:DESTINATION-LIMIT=254\r\n");
    if (m_hardwareIds[0U] >= 255U)
    {
        return Reject("BSP-DEVICE-IRQ-ID", m_hardwareIds[0U]);
    }
    if (!Diagnostics::PrepareAmd64Stop(input.BootInformation->Display, input.PoolPhysicalStart,
                                       PoolPage(input.PoolPhysicalStart, StopTableFirstPage), StopTablePages,
                                       m_hardwareIds, m_processorCount, m_topology.LocalControllerPhysicalAddress))
    {
        return Reject("STOP-APERTURE");
    }
    m_prepared = true;
    return true;
}

bool BootRuntime::PrepareEndpoint() noexcept
{
    const auto object = m_issuer.CreateObject(m_endpoint);
    const auto root = m_revocations.CreateRoot();
    if (!object.HasValue() || !root.HasValue())
    {
        return false;
    }
    const auto capability = m_issuer.Mint(m_capabilities, object.GetValue(),
                                          Kernel::Object::CapabilityRights{Kernel::Object::CapabilityRight::Inspect}
                                              .With(Kernel::Object::CapabilityRight::Invoke)
                                              .With(Kernel::Object::CapabilityRight::Read),
                                          root.GetValue(), Kernel::Object::MonotonicTime{~UInt64{0U}}, 0U);
    if (!capability.HasValue())
    {
        return false;
    }
    m_endpointCapability = capability.GetValue();
    // The initial image may report through the kernel debug channel; it
    // receives an Inspect|Write DiagnosticChannel capability in its own
    // revocation root.
    const auto diagnostic = m_issuer.CreateObject(m_diagnostic);
    const auto diagnosticRoot = m_revocations.CreateRoot();
    if (!diagnostic.HasValue() || !diagnosticRoot.HasValue())
    {
        return false;
    }
    m_diagnosticObject = diagnostic.GetValue();
    const auto diagnosticCapability =
        m_issuer.Mint(m_capabilities, diagnostic.GetValue(),
                      Kernel::Object::CapabilityRights{Kernel::Object::CapabilityRight::Inspect}.With(
                          Kernel::Object::CapabilityRight::Write),
                      diagnosticRoot.GetValue(), Kernel::Object::MonotonicTime{~UInt64{0U}}, 0U);
    if (!diagnosticCapability.HasValue())
    {
        return false;
    }
    m_diagnosticCapability = diagnosticCapability.GetValue();
    return true;
}

bool BootRuntime::PrepareThread() noexcept
{
    if (m_protection.Initialize(m_registry, m_resourceId, m_resources, m_spaceIds[UserRootIndex], m_capabilities)
            .Error != Kernel::Protection::ProtectionDomainError::None)
    {
        return false;
    }
    m_protection.SetDiagnosticComponentKind(Abi::WatchdogInitialComponent);
    const auto protection = m_issuer.CreateObject(m_protection);
    if (!protection.HasValue() || m_protection.BindIdentity(m_registry, {m_registry, protection.GetValue()}).Error !=
                                      Kernel::Protection::ProtectionDomainError::None)
    {
        return false;
    }
    if (m_thread.Initialize(BootstrapCharge) != Kernel::Scheduler::ThreadError::None)
    {
        return false;
    }
    const auto thread = m_issuer.CreateObject(m_thread);
    if (!thread.HasValue())
    {
        return false;
    }
    m_threadId = {m_registry, thread.GetValue()};
    const UInt64 now = m_timer.GetTicks();
    const Kernel::Scheduler::SchedulingContextConfiguration configuration =
        System::MakeAmd64UserSchedulingConfiguration();
    if (m_scheduling.Initialize(configuration, BootstrapCharge, Kernel::Object::MonotonicTime{now}) !=
        Kernel::Scheduler::SchedulingContextError::None)
    {
        return false;
    }
    const auto scheduling = m_issuer.CreateObject(m_scheduling);
    if (!scheduling.HasValue())
    {
        return false;
    }
    m_schedulingId = {m_registry, scheduling.GetValue()};
    if (m_scheduling.Bind(m_threadId) != Kernel::Scheduler::SchedulingContextError::None)
    {
        return false;
    }
    // This single user thread owns RSP0/SYSCALL stack for its entire lifetime.
    // UserWorker never returns: its launch frames are dead before the first
    // syscall reuses this stack, and a blocked syscall retains it exclusively.
    const UInt64 stack = StackPage(m_input.PoolPhysicalStart, 0U, SystemCallStackPage);
    // The initial image's x87 and SSE state is saved and restored with its worker context.
    m_worker.AttachExtendedState(m_workerExtendedState);
    return m_worker.Prepare({Pointer(stack), KernelStackPages * RuntimePoolPageBytes, stack}, UserWorker, this) ==
           Scheduler::Amd64KernelContextError::None;
}

[[noreturn]] void BootRuntime::UserWorker(void* const argument) noexcept
{
    auto& runtime = *static_cast<BootRuntime*>(argument);
    runtime.LocalState().LaunchFrame.Rip = runtime.m_images[ClientImage].Image.GetEntry();
    runtime.LocalState().LaunchFrame.Rsp =
        Executable::InitialUserStackStart + Executable::InitialUserStackPages * 4096U;
    runtime.LocalState().LaunchFrame.Cs = Interrupt::UserCodeSelector;
    runtime.LocalState().LaunchFrame.Ss = Interrupt::UserDataSelector;
    runtime.LocalState().LaunchFrame.Rflags = Interrupt::UserReturnRequiredFlags;
    runtime.LocalState().LaunchFrame.Rdi = runtime.m_endpointCapability.GetSelector();
    runtime.LocalState().LaunchFrame.Rsi = runtime.m_endpointCapability.GetAuthenticator();
    runtime.LocalState().LaunchFrame.Rdx = runtime.m_wait.GetCapability().GetSelector();
    runtime.LocalState().LaunchFrame.Rcx = runtime.m_wait.GetCapability().GetAuthenticator();
    runtime.LocalState().LaunchFrame.R8 = runtime.m_diagnosticCapability.GetSelector();
    runtime.LocalState().LaunchFrame.R9 = runtime.m_diagnosticCapability.GetAuthenticator();
    // The pager may have entered CPL3 first, on its own kernel stack.
    if (!Interrupt::Amd64UserEntry::SelectKernelStack(runtime.m_descriptors[runtime.CurrentIndex()],
                                                      runtime.m_entryContexts[runtime.CurrentIndex()],
                                                      runtime.m_clientStackTop))
    {
        Fail(Diagnostics::Amd64BootStage::UserStack);
    }
    Platform::Firmware::WriteDiagnosticAscii("ASTERIA:STAGE:AMD64-USER-LAUNCH\r\n");
    Interrupt::Amd64UserEntry::Enter(runtime.LocalState().LaunchFrame);
}

bool BootRuntime::InitializePager() noexcept
{
    // The pager and its created threads time their waits in the same TSC ticks.
    const UInt64 ticksPerMicrosecond = MeasureTicksPerMicrosecond();
    if (m_pagerGrantId.IsValid() && !m_threads.ConfigureWaits(ticksPerMicrosecond))
    {
        return false;
    }
    const auto& image = m_images[PagerImage].Image;
    const UInt64 stack = PoolPage(m_input.PoolPhysicalStart, PagerKernelStackPage);
    const ::Asteria::Boot::BootDisplay& display = m_input.BootInformation->Display;
    System::Amd64PagerRuntimeInput& input = m_pagerInput;
    input.ResourceIdentity = m_resourceId;
    input.Resources = &m_resources;
    input.PagerSpace = m_spaceIds[PagerRootIndex];
    input.ClientSpace = m_spaceIds[UserRootIndex];
    input.Diagnostic = m_diagnosticObject;
    input.UserMemory = &m_images[PagerImage].Access;
    input.LazyPhysicalStart = PoolPage(m_input.PoolPhysicalStart, LazyRegionFirstPage);
    input.LazyVirtualStart = Executable::InitialUserLazyStart;
    input.LazyPages = LazyRegionPages;
    input.KernelStack = {Pointer(stack), KernelStackPages * RuntimePoolPageBytes, stack};
    input.Entry = image.GetEntry();
    input.UserStackTop = Executable::InitialUserStackStart + Executable::InitialUserStackPages * 4096U;
    input.Tables = &m_descriptors[CurrentIndex()];
    input.EntryContext = &m_entryContexts[CurrentIndex()];
    input.Management = m_pagerGrantId;
    input.ObjectManagement = m_pagerGrantId.IsValid() ? &m_objects : nullptr;
    input.ManagementFeatures = Abi::PagerManagementFrames | Abi::PagerManagementMapping | Abi::PagerManagementThreads |
                               Abi::PagerManagementDomains | Abi::PagerManagementWaitSets |
                               Abi::PagerManagementResourceDomains |
                               (m_ahciDiskWritable ? Abi::PagerManagementDiskWritable : 0U);
    input.MappingWindowStart = Executable::InitialUserMappingWindowStart;
    input.MappingWindowPages = Executable::InitialUserMappingWindowPages;
    input.TicksPerMicrosecond = ticksPerMicrosecond;
    input.WatchdogSeconds = m_input.BootInformation->WatchdogSeconds;
    input.SessionMode = m_input.BootInformation->SessionMode;
    input.LoginEnabled = m_input.BootInformation->LoginEnabled;
    input.I8042Declared = m_i8042Publication.Declared;
    input.Devices[Abi::PagerDeviceEdu] = {m_assignable.Function, m_assignable.Region};
    input.Devices[Abi::PagerDeviceAhci] = {m_ahci.Function, m_ahci.Region};
    input.Devices[Abi::PagerDeviceNic] = {m_nic.Function, m_nic.Region};
    for (Size index{0U}; index < Abi::PagerUsbHostControllers; ++index)
    {
        input.Devices[Abi::PagerDeviceUsbHost + index] = {m_usbHost[index].Function, m_usbHost[index].Region};
    }
    for (Size index{0U}; index < Abi::PagerXhciControllers; ++index)
    {
        input.Devices[Abi::PagerDeviceXhci + index] = {m_xhci[index].Function, m_xhci[index].Region};
    }
    input.Scanout = {m_scanout,
                     {static_cast<UInt64>(display.Width) | (static_cast<UInt64>(display.Height) << 32U),
                      static_cast<UInt64>(display.PixelsPerScanLine) | (static_cast<UInt64>(display.Format) << 32U)}};
    for (Size index{0U}; index < 4U; ++index)
    {
        input.I8042[index] = m_i8042[index];
    }
    input.InventoryPhysicalStart = m_input.BootInformation->Inventory.PhysicalStart;
    input.InventoryPages = m_input.BootInformation->Inventory.Pages;
    input.ModuleBytes = m_input.BootInformation->Files.ModuleBytes;
    // Threads the pager creates run in its domain.
    return m_pager.Initialize(input) &&
           (!m_pagerGrantId.IsValid() || m_threads.RegisterDomain(m_pager.GetDomainRecord()));
}

// One import, after EnableIommu.
bool BootRuntime::ImportMemory() noexcept
{
    class MasterProof final : public Kernel::Device::IFirmwareMasterProof
    {
    public:
        explicit MasterProof(const BootRuntime& runtime) noexcept : m_runtime{runtime}
        {
        }
        [[nodiscard]] bool IsConfined(const Kernel::Device::PciFunctionAddress address) const noexcept override
        {
            for (Size i{0U}; i < m_runtime.m_vtdPlan.Count; ++i)
            {
                const auto& route = m_runtime.m_vtdPlan.Functions[i];
                if (!route.Address.IsEqualTo(address))
                {
                    continue;
                }
                if (m_runtime.m_amdVi != nullptr)
                {
                    return m_runtime.m_amdVi->IsEnabled() &&
                           m_runtime.m_amdVi->Serves(static_cast<Foundation::UInt16>(
                               (address.GetBus() << 8U) | (address.GetDevice() << 3U) | address.GetFunction()));
                }
                return route.Unit < IommuUnits && m_runtime.m_vtd[route.Unit] != nullptr &&
                       m_runtime.m_vtd[route.Unit]->IsEnabled();
            }
            return false;
        }
        [[nodiscard]] bool IsHaltedEhci(Kernel::Device::PciFunctionAddress) const noexcept override
        {
            return false;
        }
        void Report(const Kernel::Device::PciFunctionAddress address, const Foundation::UInt32 kind,
                    const Kernel::Device::FirmwareMasterOutcome outcome) noexcept override
        {
            if (outcome == Kernel::Device::FirmwareMasterOutcome::Unreadable)
            {
                return;
            }
            auto& evidence = Diagnostics::GetAmd64EvidenceRecord().MemoryImport;
            if (evidence.MasterRecorded < 64U)
            {
                const Size index{static_cast<Size>(evidence.MasterRecorded++)};
                evidence.MasterRequester[index] =
                    (static_cast<UInt64>(address.GetSegment()) << 16U) | address.GetRequesterId();
                evidence.MasterClass[index] = kind;
                evidence.MasterOutcome[index] = static_cast<UInt64>(outcome);
            }
            if (outcome == Kernel::Device::FirmwareMasterOutcome::Unconfined)
            {
                Platform::Firmware::WriteDiagnosticAscii("ASTERIA:FIRMWARE-MASTER:UNCONFINED:");
                Hex((static_cast<UInt64>(address.GetSegment()) << 16U) | address.GetRequesterId());
                Platform::Firmware::WriteDiagnosticAscii(":CLASS=");
                Hex(kind);
                Platform::Firmware::WriteDiagnosticAscii("\r\n");
            }
        }

    private:
        const BootRuntime& m_runtime;
    } proof{*this};
    const Size windowCount{m_topology.PciConfigurationRegionCount};
    if (windowCount > Kernel::Device::PciEcamConfigurationSpace::MaximumRegions)
    {
        return false;
    }
    for (Size i{0U}; i < windowCount; ++i)
    {
        const auto& window = m_topology.PciConfigurationRegions[i];
        m_importWindows[i] = {window.BaseAddress, window.Segment, window.FirstBus, window.LastBus, {}};
    }
    const auto gate = Kernel::Device::CheckFirmwareBusMasters(m_pciConfiguration, m_pciConfiguration,
                                                              {m_importWindows, windowCount}, true, proof);
    auto& evidence = Diagnostics::GetAmd64EvidenceRecord().MemoryImport;
    evidence.GateComplete = gate.Complete ? 1U : 0U;
    evidence.FunctionsChecked = gate.Checked;
    evidence.MasteringFunctions = gate.Mastering;
    evidence.UnconfinedFunctions = gate.Unconfined;
    evidence.UnitCount = m_topology.IommuHandoffCount;
    bool services{gate.AllowsBootServices()};
    if (!m_importer.ExcludeBootInfo(*m_input.BootInformation))
    {
        return false;
    }
    for (Size i{0U}; i < m_topology.IommuHandoffCount; ++i)
    {
        const auto& record = m_vtdHandoffRecords[i];
        evidence.UnitBase[i] = m_topology.IommuHandoffs[i].RegisterBase;
        evidence.UnitKind[i] = m_topology.IommuHandoffs[i].Kind;
        evidence.UnitOutcome[i] = static_cast<UInt64>(record.Outcome);
        evidence.UnitReason[i] = record.Reason;
        evidence.UnitStatus[i] = record.Status;
        evidence.UnitFaultStatus[i] = record.FaultStatus;
        evidence.UnitQueue[i] = record.QueueAddress;
        evidence.UnitHead[i] = record.QueueHead;
        evidence.UnitTail[i] = record.QueueTail;
        evidence.UnitRoot[i] = record.RootAddress;
        evidence.UnitInterruptTable[i] = record.InterruptTable;
        evidence.UnitPageRequestQueue[i] = record.PageRequestQueue;
        evidence.UnitAdvancedFaultLog[i] = record.AdvancedFaultLog;
        if (m_topology.IommuHandoffs[i].Kind == 2U)
        {
            const auto& amd = m_amdHandoffRecords[i];
            services = services && amd.AllowsBootServices();
            evidence.UnitOutcome[i] = static_cast<UInt64>(amd.Outcome);
            evidence.UnitReason[i] = amd.Reason;
            evidence.UnitStatus[i] = amd.Control;
            evidence.UnitFaultStatus[i] = amd.Status;
            evidence.UnitRoot[i] = amd.Bases[0U];
            evidence.UnitQueue[i] = amd.Bases[8U];
            for (Size base{0U}; base < Device::AmdViHandoffRecord::BaseCount; ++base)
            {
                evidence.AmdBaseRegisters[i * Device::AmdViHandoffRecord::BaseCount + base] = amd.Bases[base];
            }
            evidence.UnitHead[i] = amd.CommandHead;
            evidence.UnitTail[i] = amd.CommandTail;
            const Size pointer{i * Device::AmdViHandoffRecord::PointerCount};
            static_assert(sizeof(evidence.AmdQueuePointers) / sizeof(UInt64) ==
                          Boot::Acpi::MaximumAcpiIommuHandoffs * Device::AmdViHandoffRecord::PointerCount);
            evidence.AmdQueuePointers[pointer] = amd.CommandHead;
            evidence.AmdQueuePointers[pointer + 1U] = amd.CommandTail;
            evidence.AmdQueuePointers[pointer + 2U] = amd.EventHead;
            evidence.AmdQueuePointers[pointer + 3U] = amd.EventTail;
            for (Size n{0U}; n < Device::AmdViHandoffRecord::SecondaryPointerCount; ++n)
            {
                evidence.AmdQueuePointers[pointer + 4U + n] = amd.SecondaryPointers[n];
            }
            const Size amdCount{amd.Exclusions(m_importAmdExclusions)};
            for (Size n{0U}; n < amdCount; ++n)
            {
                if (!m_importer.Exclude(m_importAmdExclusions[n].Start, m_importAmdExclusions[n].Bytes))
                {
                    return false;
                }
            }
            continue;
        }
        services = services && record.AllowsBootServices();
        const Size count{record.Exclusions(m_importVtdExclusions)};
        for (Size n{0U}; n < count; ++n)
        {
            if (!m_importer.Exclude(m_importVtdExclusions[n].Start, m_importVtdExclusions[n].Bytes))
            {
                return false;
            }
        }
    }
    if (!m_importer.Build(*m_input.BootInformation, services))
    {
        return false;
    }
    m_importer.Record(evidence, *m_input.BootInformation, services);
    if (evidence.BootInfoFaultRanges != 0U)
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:MEMORY:BOOTINFO-IN-BOOTSERVICES:");
        Hex(evidence.BootInfoFaultRanges);
        Platform::Firmware::WriteDiagnosticAscii("\r\n");
    }
    if (m_importer.Count() == 0U)
    {
        Diagnostics::StopAmd64(Kernel::Diagnostics::StopCode::MemoryGrantBelowMinimum, 0U, PagerMinimumPages);
    }
    const auto imported = m_objects.GetPhysicalPageAllocator().Import(
        m_importer.Ranges(), m_importer.Count(),
        Pointer(ArenaPage(m_input.MetadataArenaPhysicalStart, MetadataArenaBitmapFirstPage)),
        (m_input.MetadataArenaPages - MetadataArenaBitmapFirstPage) * RuntimePoolPageBytes,
        m_input.BootInformation->LowMemoryBlocks, Abi::BootDmaLowBlocks);
    if (!imported.HasValue())
    {
        return false;
    }
    m_objects.GetPhysicalPageAllocator().RecordImport(evidence);
    evidence.MetadataBudgetBytes = (m_input.MetadataArenaPages - MetadataArenaBitmapFirstPage) * RuntimePoolPageBytes;
    Diagnostics::PublishAmd64Evidence(evidence.Published);
    if (imported.GetValue().ManagedPages < PagerMinimumPages)
    {
        Diagnostics::StopAmd64(Kernel::Diagnostics::StopCode::MemoryGrantBelowMinimum, imported.GetValue().ManagedPages,
                               PagerMinimumPages);
    }
    return m_resources.FinalizeMemoryImport(imported.GetValue().ManagedPages);
}

// The pager's ObjectCreate charges this child of the boot domain, so its creations stay within the
// grant's limits. Both boot variants grant it: product init also builds resident components.
bool BootRuntime::InitializeManagement() noexcept
{
    if (!ImportMemory() || !ConfigureThreads() || !ConfigureDomains())
    {
        return false;
    }
    const UInt64 allocatable = m_objects.GetAllocatablePages();
    const bool desktop = m_input.BootInformation->SessionMode == Abi::SessionDesktop;
    const UInt64 endpoints = desktop ? Abi::DesktopGrantEndpoints : PagerGrantEndpoints;
    const UInt64 waits = desktop ? Abi::DesktopGrantWaitSets : PagerGrantWaitSets;
    const UInt64 pairs = desktop ? Abi::DesktopGrantMemoryPairs : PagerGrantMemoryObjects;
    const UInt64 iommuDomains = desktop ? Kernel::Device::IommuDomainPool::Capacity : PagerGrantIommuDomains;
    const UInt64 interrupts = desktop ? Kernel::Interrupt::InterruptPool::Capacity : PagerGrantInterrupts;
    const Kernel::Resource::ResourceDomainLimits limits{
        .CommittedMemoryPages = allocatable,
        .ResidentMemoryPages = allocatable,
        .KernelMetadataBytes =
            (endpoints * Kernel::Ipc::EndpointPool::MetadataBytes) + (waits * WaitSetMetadataBytes) +
            (pairs * (sizeof(Kernel::Memory::CreatedFrame) + sizeof(Kernel::Memory::MemoryObject))) +
            (PagerGrantThreads * (sizeof(Kernel::Scheduler::Thread) + sizeof(Kernel::Scheduler::SchedulingContext) +
                                  m_threads.GetNativeSlotBytes())) +
            (PagerGrantDomains * (m_runtimeRoots.GetSlotBytes() + CapabilitySpaceMetadataBytes +
                                  sizeof(Kernel::Protection::ProtectionDomain))) +
            (PagerGrantChildDomains * ResourceDomainMetadataBytes)
            // Two ordinary slots may select large arenas.
            + (LargeRootArenaCount * (LargeRootArenaPages - RootArenaPages) * RuntimePoolPageBytes)
            // The drivers' IommuDomains, Interrupts and a WaitSet.
            + (iommuDomains * sizeof(Kernel::Device::IommuDomain)) + (interrupts * InterruptMetadataBytes) +
            WaitSetMetadataBytes,
        .CapabilityHandles = 0U,
        .IpcQueueBytes = endpoints * Kernel::Ipc::Endpoint::MaximumQueueBytes,
        .Threads = PagerGrantThreads,
        .ChildDomains = PagerGrantChildDomains,
    };
    if ((m_pagerGrant.InitializeChild(m_registry, PagerGrantCharge, m_resourceId, m_resources, limits).Error !=
         Kernel::Resource::ResourceDomainError::None) ||
        !m_objects.ConfigureMapping(m_shootdowns[0U], m_mailbox, m_mappingProgress) ||
        !m_objects.ConfigureResourceDomains(m_ledger, FirstComponentCharge) ||
        !m_objects.ConfigureDeviceControl(m_pciConfiguration, m_pciConfiguration) ||
        ((m_vtdBackends[0U] != nullptr) &&
         !m_objects.ConfigureIommu(m_iommuSlots, m_vtdBackends, Kernel::System::IommuAdmission::Every,
                                   &m_dmaReservations)) ||
        ((m_vtdBackends[0U] == nullptr) && (m_trustedDma.GetTrustedCount() != 0U) &&
         !m_objects.ConfigureIommu(m_iommuSlots, m_trustedDma.GetBackends(),
                                   Kernel::System::IommuAdmission::TrustedAssignees, &m_dmaReservations))
        // Device messages target the BSP's xAPIC.
        || !m_deviceVectors.SetDestination(m_hardwareIds[0U]) || !m_objects.ConfigureInterrupts(m_deviceVectors) ||
        !ConfigureScrubber())
    {
        return false;
    }
    // Test firmware proves creation-time zeroing in the pager's runtime root, through the
    // production creation path.
    if (m_hasSpinner)
    {
        m_objects.EnableCreationZeroProbe();
    }
    const auto registered = m_issuer.CreateObject(m_pagerGrant);
    if (!registered.HasValue())
    {
        return false;
    }
    m_pagerGrantId = registered.GetValue();
    m_objects.ConfigurePower(Diagnostics::ConfigureAmd64Power(m_input.BootInformation->Power, &m_portIo),
                             m_pagerGrantId);
    return true;
}

// Created threads run on CPU0 on the seed-mapped thread stacks; the ObjectManager starts them
// through the table. The objects of the domains the root service creates: the reserved arenas'
// roots, the created spaces' keys, and the thread table's records, whose faults and exits go to the
// pager.
bool BootRuntime::ConfigureDomains() noexcept
{
    Kernel::Object::CapabilityValidationKey keys[Kernel::Object::CapabilitySpacePool::Capacity]{};
    for (Size slot{0U}; slot < Kernel::Object::CapabilitySpacePool::Capacity; ++slot)
    {
        const auto key = Kernel::Object::DeriveCapabilityValidationKey(m_input.BootInformation->Entropy,
                                                                       Kernel::Object::RuntimeSpaceKeyPurpose(slot));
        if (!key.HasValue())
        {
            return false;
        }
        keys[slot] = key.GetValue();
    }
    return m_runtimeRoots.Configure(m_seed, m_paging, m_input.PoolPhysicalStart, m_input.MetadataArenaPhysicalStart,
                                    m_scrubber) &&
           m_objects.ConfigureSpaces(m_revocations, m_timer, keys) &&
           m_objects.ConfigureFaultChannels(m_revocations, m_timer, keys, m_controller.GetWakeFacade(),
                                            m_shootdowns[0U], m_mailbox, m_mappingProgress) &&
           m_objects.ConfigureAddressSpaces(m_runtimeRoots) && m_threads.ConfigureCreatedDomains(m_pager, m_objects) &&
           m_objects.ConfigureDomains(m_threads);
}

bool BootRuntime::ConfigureThreads() noexcept
{
    static_assert((System::Amd64UserThreadSlots == ThreadStackSlots + MetadataArenaThreadStackSlots) &&
                  (Kernel::Scheduler::ThreadPool::Capacity == System::Amd64UserThreadSlots));
    // The runtime pool's stacks, then the metadata arena's. The input lives in the runtime, not on
    // the idle stack: thirty-two stack views outgrow a frame's budget.
    m_threadInput = {{}, &m_descriptors[0U], &m_entryContexts[0U]};
    for (Size slot{0U}; slot < System::Amd64UserThreadSlots; ++slot)
    {
        const UInt64 stack = (slot < ThreadStackSlots)
                                 ? PoolPage(m_input.PoolPhysicalStart, ThreadStackPage(slot))
                                 : ArenaPage(m_input.MetadataArenaPhysicalStart, MetadataArenaThreadStackPage(slot));
        m_threadInput.Stacks[slot] = {Pointer(stack), KernelStackPages * RuntimePoolPageBytes, stack};
    }
    m_threadInput.Loops[0U] = &m_runLoop;
    m_threadInput.Timers[0U] = &m_timer;
    for (Size cpu{1U}; cpu < m_processorCount; ++cpu)
    {
        m_threadInput.Loops[cpu] = &m_processors[cpu]->GetRunLoop();
        m_threadInput.Timers[cpu] = &m_processors[cpu]->GetLocalTimer();
    }
    return m_threads.Configure(m_threadInput) && m_objects.ConfigureThreads(m_threads);
}

// The frame pool's scrubber zeroes destroyed Frames' pages through the calling processor's own
// scrub slot whenever the active root is resident on that processor alone. The boot roots are
// published here, the created ones as RuntimeRoots prepares them.
bool BootRuntime::ConfigureScrubber() noexcept
{
    const Memory::Amd64FrameScrubberConfiguration configuration{
        .PoolPhysicalStart = 0U,
        .PoolPages = 0U,
        .SlotVirtualAddress = ScrubSlotVirtualAddress,
        .SlotPhysical = PoolPage(m_input.PoolPhysicalStart, ScrubSlotFirstPage),
        .Processors = ProcessorStackSlots,
        .ArenaPages = RootArenaPages,
        .Allocator = &m_objects.GetPhysicalPageAllocator(),
    };
    if (!m_scrubber.Configure(configuration) || !m_objects.ConfigureScrubber(m_scrubber))
    {
        return false;
    }
    for (Size root{0U}; root < BootRootCount; ++root)
    {
        if (!m_scrubber.PublishRoot(root, m_spaces[root],
                                    m_pageTables[root].GetSnapshot().RootPhysicalAddress.GetValue()))
        {
            return false;
        }
    }
    m_objects.SetParkedPinObserver(&m_releases);
    return true;
}

bool BootRuntime::InitializeSpinner() noexcept
{
    const auto& image = m_images[SpinnerImage].Image;
    const UInt64 stack = PoolPage(m_input.PoolPhysicalStart, SpinnerKernelStackPage);
    const System::Amd64SpinnerRuntimeInput input{m_resourceId,
                                                 &m_resources,
                                                 m_spaceIds[SpinnerRootIndex],
                                                 m_diagnosticObject,
                                                 &m_images[SpinnerImage].Access,
                                                 {Pointer(stack), KernelStackPages * RuntimePoolPageBytes, stack},
                                                 image.GetEntry(),
                                                 Executable::InitialUserStackStart +
                                                     Executable::InitialUserStackPages * 4096U,
                                                 &m_descriptors[0U],
                                                 &m_entryContexts[0U]};
    return m_spinner.Initialize(input);
}

// The queue order is the pager, the spinner, the client: the pager waits for
// faults first, and the spinner, CPU-bound, runs before the client, which
// therefore runs only by preemption until the spinner blocks.
bool BootRuntime::AdmitThreads() noexcept
{
    if (!m_pager.Admit(LocalState().DispatchFailure) ||
        (m_hasSpinner && !m_spinner.Admit(LocalState().DispatchFailure)) || !AdmitWorker())
    {
        return false;
    }
    auto& evidence = Diagnostics::GetAmd64EvidenceRecord().UserPreemption;
    evidence.SpinnerPresent = m_hasSpinner ? 1U : 0U;
    for (Size cpu{1U}; cpu < m_processorCount; ++cpu)
    {
        m_processors[cpu]->EnableUsers();
    }
    Diagnostics::PublishAmd64Evidence(evidence.Published);
    Diagnostics::PublishAmd64Evidence(Diagnostics::GetAmd64EvidenceRecord().Supervision.Published);
    return true;
}

// The pager ended a supervised thread with Terminate.
void BootRuntime::RecordTermination() noexcept
{
    auto& evidence = Diagnostics::GetAmd64EvidenceRecord().Supervision;
    ++evidence.Terminations;
    const auto spinner = m_spinner.GetThreadSnapshot();
    if (m_hasSpinner && (spinner.State == Kernel::Scheduler::ThreadState::Exited))
    {
        evidence.SpinnerExited = 1U;
        evidence.SpinnerExitCode = spinner.ExitCode;
    }
    Diagnostics::PublishAmd64Evidence(evidence.Published);
    Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AMD64-PAGER:TERMINATED:");
    Hex(evidence.Terminations);
    Platform::Firmware::WriteDiagnosticAscii("\r\n");
}

// Every boot object is registered once the boot domains are admitted; the
// registry must still hold the pager channel's tokens and full Endpoint,
// Frame, MemoryObject and thread pools, which register at run time.
bool BootRuntime::CheckRegistryBudget() noexcept
{
    const Size registered = m_registry.GetActiveCount();
    // A created Endpoint registers itself and its readiness Notification, a created WaitSet itself.
    // A created thread registers its Thread and its SchedulingContext. A created domain registers
    // its AddressSpace, CapabilitySpace and ProtectionDomain, and a component's ResourceDomain
    // itself.
    if ((registered + (Kernel::Ipc::EndpointPool::RegisteredObjectsPerSlot * Kernel::Ipc::EndpointPool::Capacity) +
         Kernel::Ipc::WaitSetPool::Capacity + Kernel::Memory::FrameObjectPool::Capacity +
         Kernel::Memory::MemoryObjectPool::Capacity + (2U * Kernel::Scheduler::ThreadPool::Capacity) +
         Kernel::Memory::FaultTokenFactory::MaximumTokens + Kernel::Memory::AddressSpacePool::Capacity +
         Kernel::Memory::FaultChannelPool::MaximumRegisteredObjects + Kernel::Object::CapabilitySpacePool::Capacity +
         Kernel::Protection::ProtectionDomainPool::Capacity + Kernel::Resource::ResourceDomainPool::Capacity +
         Kernel::Device::IommuDomainPool::Capacity +
         (Kernel::Interrupt::InterruptPool::RegisteredObjectsPerSlot * Kernel::Interrupt::InterruptPool::Capacity)) >
        m_registry.GetCapacity())
    {
        return false;
    }
    auto& evidence = Diagnostics::GetAmd64EvidenceRecord().ObjectManagement;
    evidence.EndpointMetadataBytes = Kernel::Ipc::EndpointPool::MetadataBytes;
    evidence.FrameMetadataBytes = sizeof(Kernel::Memory::CreatedFrame);
    evidence.MemoryObjectMetadataBytes = sizeof(Kernel::Memory::MemoryObject);
    evidence.RegisteredObjects = registered;
    RecordManagement();
    return true;
}

[[gnu::noinline]] void RecordApplicationLaunch(const Kernel::System::ObjectManager& objects) noexcept
{
    auto& evidence = Diagnostics::GetAmd64EvidenceRecord().ApplicationLaunch;
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
    Diagnostics::PublishAmd64Evidence(evidence.Published);
}

void BootRuntime::RecordManagement() noexcept
{
    if (Platform::Synchronization::CurrentProcessor().GetValue() != 0U)
    {
        return;
    }
    RecordApplicationLaunch(m_objects);
    const auto pool = m_objects.GetEndpointSnapshot();
    const auto usage = m_pagerGrant.GetUsage();
    auto& evidence = Diagnostics::GetAmd64EvidenceRecord().ObjectManagement;
    evidence.Created = pool.Created;
    evidence.Destroyed = pool.Destroyed;
    evidence.Reclaimed = pool.Reclaimed;
    evidence.OccupiedSlots = pool.ActiveSlots + pool.QuiescingSlots;
    evidence.MetadataBytes = usage.KernelMetadataBytes;
    evidence.IpcQueueBytes = usage.IpcQueueBytes;
    const auto frames = m_objects.GetFrameSnapshot();
    const auto memoryObjects = m_objects.GetMemoryObjectSnapshot();
    const auto framePool = m_objects.GetFramePoolSnapshot();
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
    RecordAllocator(frames);
    RecordMapping(evidence);
    if (usage.KernelMetadataBytes > evidence.PeakMetadataBytes)
    {
        evidence.PeakMetadataBytes = usage.KernelMetadataBytes;
    }
    Diagnostics::PublishAmd64Evidence(evidence.Published);
    RecordCollection();
    RecordThreads();
}

void BootRuntime::RecordAllocator(const Kernel::Memory::FrameObjectPoolSnapshot& frames) noexcept
{
    auto& allocatorEvidence = Diagnostics::GetAmd64EvidenceRecord().Allocator;
    const auto allocator = m_objects.GetAllocatorSnapshot();
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
    if (allocatorEvidence.CreationZeroedPages != frames.CreationZeroedPages)
    {
        // RecordManagement follows this processor's non-preemptible create before any root switch.
        // Count changes distinguish creation zeros from collection zeros (physical memory
        // management evidence).
        __asm__ __volatile__("mov %%cr3,%0" : "=r"(allocatorEvidence.CreationZeroRoot));
    }
    allocatorEvidence.CreationZeroedPages = frames.CreationZeroedPages;
    Diagnostics::PublishAmd64Evidence(allocatorEvidence.Published);
}

void BootRuntime::RecordCollection() noexcept
{
    const Kernel::System::ObjectCollectionSnapshot collected = m_objects.GetCollectionSnapshot();
    auto& evidence = Diagnostics::GetAmd64EvidenceRecord().Collection;
    evidence.Collections = collected.Collections;
    evidence.Reclaimed = collected.Reclaimed;
    evidence.ScrubRuns = collected.ScrubRuns;
    evidence.ScrubbedPages = collected.ScrubbedPages;
    Diagnostics::PublishAmd64Evidence(evidence.Published);
}

// The domains the root service created; only the test firmware's grant may create them.
void BootRuntime::RecordComponents() noexcept
{
    if (!m_pagerGrantId.IsValid())
    {
        return;
    }
    const auto spaces = m_objects.GetAddressSpaceSnapshot();
    const auto capabilities = m_objects.GetSpaceSnapshot();
    const auto domains = m_objects.GetDomainSnapshot();
    const auto table = m_threads.GetSnapshot();
    auto& evidence = Diagnostics::GetAmd64EvidenceRecord().Components;
    evidence.AddressSpacesCreated = spaces.Created;
    evidence.AddressSpacesLive = spaces.ActiveSlots;
    evidence.CapabilitySpacesCreated = capabilities.Created;
    evidence.CapabilitySpacesLive = capabilities.ActiveSlots;
    evidence.DomainsCreated = domains.Created;
    evidence.DomainsDestroyed = domains.Destroyed;
    evidence.DomainsReclaimed = domains.Reclaimed;
    evidence.TaggedFaults = table.TaggedFaults;
    evidence.LastTag = table.LastTag;
    evidence.DomainExits = table.DomainExits;
    evidence.AddressSpaceBytes = m_runtimeRoots.GetSlotBytes();
    evidence.CapabilitySpaceBytes = Kernel::Object::CapabilitySpacePool::GetReservation().KernelMetadataBytes;
    evidence.AddressSpacesReclaimed = spaces.Reclaimed;
    evidence.CapabilitySpacesReclaimed = capabilities.Reclaimed;
    evidence.ArenasPrepared = m_runtimeRoots.GetPreparedCount();
    evidence.AddressSpacesFree = spaces.FreeSlots;
    evidence.CapabilitySpacesFree = capabilities.FreeSlots;
    evidence.WaitExpiries = m_pager.GetWaitExpiries();
    const auto accounts = m_objects.GetResourceDomainSnapshot();
    evidence.ResourceDomainsCreated = accounts.Created;
    evidence.ResourceDomainsDestroyed = accounts.Destroyed;
    evidence.ResourceDomainsReclaimed = accounts.Reclaimed;
    evidence.ResourceDomainsLive = accounts.ActiveSlots + accounts.QuiescingSlots;
    Diagnostics::PublishAmd64Evidence(evidence.Published);
    RecordIommu();
    RecordInterrupts();
}

// Created threads: the pool's and the native table's view, and the grant's Threads usage.
void BootRuntime::RecordThreads() noexcept
{
    if (CurrentIndex() != 0U)
    {
        return;
    }
    const auto pool = m_objects.GetThreadSnapshot();
    const auto table = m_threads.GetSnapshot();
    auto& evidence = Diagnostics::GetAmd64EvidenceRecord().Threads;
    evidence.Created = pool.Created;
    evidence.Destroyed = pool.Destroyed;
    evidence.Reclaimed = pool.Reclaimed;
    evidence.Exits = table.Exits;
    evidence.Reaped = table.Reaped;
    evidence.Lines = table.Lines;
    evidence.LastExitCode = pool.LastExitCode;
    evidence.FreeSlots = pool.FreeSlots;
    evidence.FreeNativeSlots = table.FreeSlots;
    evidence.ThreadsUsage = m_pagerGrant.GetUsage().Threads;
    evidence.Faults = table.Faults;
    evidence.Deferrals = table.Deferrals;
    evidence.Exceptions = table.Exceptions;
    evidence.Kills = table.Kills;
    auto& services = Diagnostics::GetAmd64EvidenceRecord().Services;
    const auto waitSets = m_objects.GetWaitSetSnapshot();
    services.WaitSetsCreated = waitSets.Created;
    services.WaitSetsReclaimed = waitSets.Reclaimed;
    services.Binds = m_objects.GetBinds();
    services.WaitsBlocked = table.WaitsBlocked;
    services.WaitExpiries = table.WaitExpiries;
    services.WaitAborts = table.WaitAborts;
    services.AbortedExits = table.AbortedExits;
    Diagnostics::PublishAmd64Evidence(services.Published);
    evidence.SlotMetadataBytes = m_pagerGrantId.IsValid()
                                     ? sizeof(Kernel::Scheduler::Thread) +
                                           sizeof(Kernel::Scheduler::SchedulingContext) + m_threads.GetNativeSlotBytes()
                                     : 0U;
    Diagnostics::PublishAmd64Evidence(evidence.Published);
    RecordComponents();
}

// Only the pager maps created memory, and only into its own space.
void BootRuntime::RecordMapping(Diagnostics::Amd64ObjectManagementEvidence& evidence) noexcept
{
    const auto mapper = m_objects.GetMapperSnapshot();
    evidence.Mappings = mapper.Mappings;
    evidence.Unmappings = mapper.Unmappings;
    evidence.MappedVirtualStart = mapper.LastMapping.VirtualStart.GetValue();
    evidence.MappedPhysicalStart = mapper.LastMapping.PhysicalStart.GetValue();
    evidence.MappedPages = mapper.LastMapping.Pages.GetValue();
}

bool BootRuntime::IsClientRunning() const noexcept
{
    const auto snapshot = LocalRunLoop().GetSnapshot();
    return (snapshot.State == Kernel::Scheduler::ProcessorKernelRunLoopState::Active) &&
           (snapshot.ActiveThread == &m_thread);
}

bool BootRuntime::AdmitWorker() noexcept
{
    return m_thread.MakeReady() == Kernel::Scheduler::ThreadError::None &&
           m_runLoop.Bind(m_thread, m_worker, m_protection) == Kernel::Scheduler::ProcessorKernelRunLoopError::None &&
           m_runLoop.EnqueueBoundThread(m_schedulingId, m_scheduling, m_threadId, m_thread,
                                        LocalState().DispatchFailure);
}

void BootRuntime::RecordRuntime() noexcept
{
    auto& evidence = Diagnostics::GetAmd64EvidenceRecord().Runtime;
    evidence.PoolPhysicalStart = m_input.PoolPhysicalStart;
    evidence.SharedRootPhysicalAddress = m_pageTables[SharedRootIndex].GetSnapshot().RootPhysicalAddress.GetValue();
    evidence.UserRootPhysicalAddress = m_pageTables[UserRootIndex].GetSnapshot().RootPhysicalAddress.GetValue();
    evidence.LocalApicPhysicalAddress = m_topology.LocalControllerPhysicalAddress;
    evidence.ProcessorCount = m_processorCount;
    auto& record = Diagnostics::GetAmd64EvidenceRecord();
    record.TscFrequencyHz = m_calibration.FrequencyHz;
    record.TscSource = static_cast<UInt64>(m_calibration.Source);
    record.X2Apic = m_timer.GetApic().IsX2Apic() ? 1U : 0U;
    Diagnostics::PublishAmd64Evidence(evidence.Published);
}

[[nodiscard]] CpuId TrackingAmd64Processor() noexcept
{
    UInt64 cpu{0U};
#if defined(ASTERIA_TARGET_AMD64)
    __asm__ __volatile__("movq %%gs:16,%0" : "=r"(cpu));
#endif
    return cpu < Kernel::Scheduler::MaximumProcessorCount ? CpuId{static_cast<Foundation::UInt8>(cpu)} : CpuId{};
}
[[nodiscard]] UInt64 ProgressTicks(void*) noexcept
{
    return Time::ReadTsc();
}
void ProgressStop(const Kernel::Diagnostics::StopCode code, const UInt64 detail) noexcept
{
    Diagnostics::StopAmd64(
        code, detail,
        code == Kernel::Diagnostics::StopCode::Lockup ? Platform::Synchronization::ProcessorWatchdogPeriodMilliseconds()
                                                      : 0U,
        code == Kernel::Diagnostics::StopCode::Lockup ? Platform::Synchronization::CurrentProcessor().GetValue() : 0U,
        code == Kernel::Diagnostics::StopCode::Lockup ? Platform::Synchronization::ProcessorWatchdogFailureAge() : 0U);
}
void BootRuntime::BindBspProgress() noexcept
{
    using Platform::Synchronization::BindProcessorProgress;
    using Platform::Synchronization::InstallCurrentProcessor;
    using Platform::Synchronization::ProcessorProgressHooks;
    using Platform::Synchronization::RuntimeProcessorSet;
    using Platform::Synchronization::StopProcessorWait;
    InstallCurrentProcessor(TrackingAmd64Processor);
    BindProcessorProgress(
        CpuId{0U}, ProcessorProgressHooks{&m_drains[0U], Kernel::Memory::DrainProcessorShootdowns, nullptr,
                                          ProgressTicks, ProgressStop,
                                          m_calibration.FrequencyHz * Kernel::Scheduler::Amd64CrossProcessorWaitSeconds,
                                          &RuntimeProcessorSet()});
    if (!RuntimeProcessorSet().Admit(CpuId{0U}))
    {
        StopProcessorWait(Kernel::Diagnostics::StopCode::InvariantViolated, 0U);
    }
}

[[noreturn]] void BootRuntime::Start() noexcept
{
    if (!m_prepared || !m_addressSpaces.Initialize(CpuId{0U}, m_spaces[SharedRootIndex]).HasValue())
    {
        Fail(Diagnostics::Amd64BootStage::Root);
    }
    Interrupt::Amd64UserEntry::Install(m_descriptors[0U], m_entryContexts[0U]);
    if (!m_timer.Initialize(m_input.BootInformation->Acpi.LocalControllerPhysicalAddress))
    {
        Fail(Diagnostics::Amd64BootStage::Apic);
    }
    BindBspProgress();
    if (!Platform::Synchronization::SynchronizeOnlineProcessorClock(m_timer, m_calibration.FrequencyHz,
                                                                    Diagnostics::GetAmd64EvidenceRecord().Clocks))
    {
        Fail(Diagnostics::Amd64BootStage::Apic);
    }
    if (!m_notifier.Initialize(m_mailbox.GetOnlineProcessorMask(), m_hardwareIds))
    {
        Fail(Diagnostics::Amd64BootStage::Shootdown);
    }
    Platform::Synchronization::BindProcessorKick(&m_notifier,
                                                 [](void* owner, const Kernel::Scheduler::CpuId home) noexcept
                                                 {
                                                     static_cast<IpiNotifier*>(owner)->Kick(home);
                                                 });
    Diagnostics::Amd64StopProcessorOnline(0U);
    Platform::Synchronization::ProcessorWatchdogOnline();
    m_timer.EnableWatchdog(Platform::Synchronization::ProcessorWatchdogCheckTicks());
    RecordRuntime();
    RecordMetadata();
    Platform::Firmware::WriteDiagnosticAscii("ASTERIA:STAGE:AMD64-RUNTIME-ROOT\r\n");
    if (!PrepareEndpoint())
    {
        Fail(Diagnostics::Amd64BootStage::Endpoint);
    }
    if (!m_wait.Initialize(m_capabilities, m_revocations))
    {
        Fail(Diagnostics::Amd64BootStage::WaitObjects);
    }
    if (!RunDevices())
    {
        Fail(Diagnostics::Amd64BootStage::Devices);
    }
    if (!EnableIommu())
    {
        Fail(Diagnostics::Amd64BootStage::Iommu);
    }
    // Refresh selected units after their final takeover step. Scratch-unit records already survive
    // in their own inventory slots.
    for (Size index{0U}; index < m_topology.IommuHandoffCount; ++index)
    {
        if (m_amdVi != nullptr && m_topology.IommuHandoffs[index].Kind == 2U &&
            m_topology.IommuHandoffs[index].RegisterBase == m_topology.Ivrs.RegisterBase)
        {
            m_amdVi->CopyHandoffRecord(m_amdHandoffRecords[index]);
        }
        for (Size slot{0U}; slot < m_topology.DmaRemappingUnitCount && slot < IommuUnits; ++slot)
        {
            if (m_vtd[slot] != nullptr && m_vtdHandoffRecords[index].Kind == 1U &&
                m_vtdHandoffRecords[index].RegisterBase == m_topology.DmaRemappingUnits[slot].RegisterBaseAddress)
            {
                m_vtd[slot]->CopyHandoffRecord(m_vtdHandoffRecords[index]);
            }
        }
    }
    // From the plan capture reserved functions by, before any IommuDomain.
    static_cast<void>((m_topology.DmarPhysicalAddress != 0U)
                          ? m_dmaReservations.BuildFromDmar(m_topology, m_vtdPlan)
                          : m_dmaReservations.BuildFromIvrs(m_topology.Ivrs, m_vtdPlan));
    ApplyTrustedDma();
    Diagnostics::CheckAmd64EarlyStopPoint();
    if (!StartProcessors())
    {
        Fail(Diagnostics::Amd64BootStage::Processors);
    }
    m_mailbox.BindProcessorSet(Platform::Synchronization::RuntimeProcessorSet());
    // The BSP's IDT must take an IPI too.
    m_notifier.Notify(ProcessorBit(0U));
    for (Kernel::Scheduler::CrossProcessorWait wait;
         __atomic_load_n(&Diagnostics::GetAmd64EvidenceRecord().Smp.ShootdownTaken[0U], __ATOMIC_ACQUIRE) == 0U;)
    {
        __asm__ __volatile__("sti; pause; cli" : : : "memory");
        if (!wait.Poll())
        {
            Kernel::Scheduler::CrossProcessorWait::Timeout(ProcessorBit(0U));
        }
    }
    if (!RunSharedProbe())
    {
        Fail(Diagnostics::Amd64BootStage::Shootdown);
    }
#if ASTERIA_AMD64_NATIVE_PREEMPTION_PROBE == 1
    StartPreemption();
#else
    ContinueUser(this);
#endif
}

[[noreturn]] void BootRuntime::StartPreemption() noexcept
{
    // The BSP's worker stack runs the preempted loop, its system-call stack
    // the coordinator (it becomes the user thread's stack afterwards), and its
    // idle stack the fresh idle continuation.
    const UInt64 worker = StackPage(m_input.PoolPhysicalStart, 0U, WorkerStackPage);
    const UInt64 coordinator = StackPage(m_input.PoolPhysicalStart, 0U, SystemCallStackPage);
    const UInt64 idle = StackPage(m_input.PoolPhysicalStart, 0U, IdleStackPage);
    constexpr UInt64 stackBytes{KernelStackPages * RuntimePoolPageBytes};
    const Scheduler::Amd64NativePreemptionInput input{&m_registry,
                                                      m_resourceId,
                                                      &m_resources,
                                                      &m_spaces[SharedRootIndex],
                                                      &m_timer,
                                                      &m_controller,
                                                      &m_runLoop,
                                                      &m_idle,
                                                      {Pointer(worker), stackBytes, worker},
                                                      {Pointer(coordinator), stackBytes, coordinator},
                                                      {Pointer(idle), stackBytes, idle},
                                                      ContinueUser,
                                                      this};
    Scheduler::RunAmd64NativePreemption(input);
}

[[noreturn]] void BootRuntime::ContinueUser(void* const argument) noexcept
{
    auto& runtime = *static_cast<BootRuntime*>(argument);
    if (!runtime.InitializeManagement())
    {
        Fail(Diagnostics::Amd64BootStage::Management);
    }
    if (!runtime.InitializePager())
    {
        Fail(Diagnostics::Amd64BootStage::Pager);
    }
    if (runtime.m_hasSpinner && !runtime.InitializeSpinner())
    {
        Fail(Diagnostics::Amd64BootStage::Spinner);
    }
    if (!runtime.PrepareThread())
    {
        Fail(Diagnostics::Amd64BootStage::Thread);
    }
    if (!runtime.AdmitThreads())
    {
        Fail(Diagnostics::Amd64BootStage::Admission);
    }
    if (!runtime.CheckRegistryBudget())
    {
        Fail(Diagnostics::Amd64BootStage::RegistryBudget);
    }
    Diagnostics::GetAmd64EvidenceRecord().UserProcessors.Processors[0U].IdleRoot = runtime.m_input.PoolPhysicalStart;
    Size rearms{0U};
    Size late{0U};
    UInt64 ticks{runtime.LocalState().TimerInterrupts};
    for (;;)
    {
        Platform::Synchronization::ServiceThreadRequests(true);
        // Exited created threads are unbound at every scheduling point the idle loop reaches.
        const UInt64 reaped = runtime.m_threads.GetSnapshot().Reaped;
        if (!runtime.m_threads.Reap())
        {
            Fail(Diagnostics::Amd64BootStage::Reap);
        }
        if (runtime.m_threads.GetSnapshot().Reaped != reaped)
        {
            runtime.RecordThreads();
        }
        const bool dispatched = Scheduler::Amd64DispatchFromIdle(
            runtime.m_runLoop, runtime.LocalState().DispatchReceipt, runtime.LocalState().DispatchFailure);
        if (runtime.LocalState().TimerInterrupts != ticks)
        {
            ticks = runtime.LocalState().TimerInterrupts;
            late = 0U;
        }
        const auto& failure = runtime.LocalState().DispatchFailure;
        if (!dispatched &&
            failure.Error == Kernel::Scheduler::ProcessorKernelRunLoopError::AddressSpaceControllerRejected &&
            failure.AddressSpaceController.AdmissionRollbackSafe &&
            runtime.m_runLoop.GetSnapshot().State == Kernel::Scheduler::ProcessorKernelRunLoopState::Idle)
        {
            Platform::Synchronization::ProcessorWaitProgress();
            continue;
        }
        const bool rejected =
            !dispatched && (failure.Error == Kernel::Scheduler::ProcessorKernelRunLoopError::ControllerRejected);
        // An admission whose run stop passed before entry rolls back to Ready.
        const bool entryLate =
            !dispatched && (failure.Error == Kernel::Scheduler::ProcessorKernelRunLoopError::ControllerEntryRejected) &&
            (failure.ControllerError == Kernel::Scheduler::ProcessorRunError::DeadlineReached) &&
            (failure.ActiveRunRetained == 0U);
        if (entryLate ||
            (rejected && (failure.ControllerError == Kernel::Scheduler::ProcessorRunError::DeadlineReached)))
        {
            // This idle boundary owns no transaction lock. Give an already-pending timer one
            // bounded interrupt window before retrying its overdue wait with delivery masked.
            // Otherwise eight rearms can stop a progressing system without ever observing the
            // interrupt that completes the original wait.
            __asm__ __volatile__("sti; pause; cli" : : : "memory");
            // A deadline fell due (nonterminal, ProcessorRunController.md).
            // The wait's timer interrupt already arrived at a user tick: drain
            // it. Otherwise, if the wait's own deadline passed, it fell due
            // before it could be programmed or entered: the wait's timer is
            // armed again. An admission whose run stop passed is retried, and
            // does not count against the wait.
            if (++late > LateDispatchAttempts)
            {
                Fail(Diagnostics::Amd64BootStage::DispatchLate, late);
            }
            if (runtime.m_wait.HasObservedInterrupt())
            {
                if (!runtime.m_wait.ResumeFromIdle())
                {
                    Fail(Diagnostics::Amd64BootStage::WaitIdle, 0x100U | runtime.m_wait.GetFailureStage());
                }
                rearms = 0U;
            }
            else if (runtime.m_wait.IsTimerDue() && ((++rearms > WaitRearmAttempts) || !runtime.m_wait.RearmDueTimer()))
            {
                Fail(Diagnostics::Amd64BootStage::WaitRearm, rearms);
            }
            continue;
        }
        if (!rejected || ((failure.ControllerError != Kernel::Scheduler::ProcessorRunError::NoRunnableThread) &&
                          (failure.ControllerError != Kernel::Scheduler::ProcessorRunError::TimerWorkPending)))
        {
            Fail(Diagnostics::Amd64BootStage::DispatchReturn, runtime.DescribeDispatchFailure());
        }
        // Nothing can run now: a fault delivered to the waiting pager wakes
        // it; a suspended timer wait resumes through its timer; otherwise
        // every runnable thread has spent its budget until a replenishment.
        if (!runtime.m_objects.ProgressFaultChannels())
        {
            Fail(Diagnostics::Amd64BootStage::PagerIdle);
        }
        const auto pager = runtime.m_pager.ProgressFromIdle();
        if (pager == System::Amd64PagerIdleProgress::Failed)
        {
            Fail(Diagnostics::Amd64BootStage::PagerIdle);
        }
        // A created thread's wait wakes the same way.
        const auto created = runtime.m_threads.ProgressWaits();
        if (created == System::Amd64ThreadWaitProgress::Failed)
        {
            Fail(Diagnostics::Amd64BootStage::ThreadWait);
        }
        if (created == System::Amd64ThreadWaitProgress::Woken)
        {
            runtime.RecordThreads();
        }
        if ((pager == System::Amd64PagerIdleProgress::Waiting) && (created == System::Amd64ThreadWaitProgress::Waiting))
        {
            if (runtime.m_wait.IsSuspended())
            {
                if (!runtime.m_wait.ResumeFromIdle())
                {
                    Fail(Diagnostics::Amd64BootStage::WaitIdle, 0x200U | runtime.m_wait.GetFailureStage());
                }
                rearms = 0U;
            }
            else if (!runtime.WaitForTickFromIdle())
            {
                Fail(Diagnostics::Amd64BootStage::IdleStall);
            }
        }
        late = 0U;
    }
}

void BootRuntime::Trap(Interrupt::Amd64TrapFrame& frame) noexcept
{
    if (frame.Vector == 2U)
    {
        Diagnostics::HandleAmd64Nmi(frame);
    }
    const auto trapOwner = Platform::Synchronization::CurrentProcessor();
    if (!trapOwner.IsValid() || trapOwner.GetValue() >= m_processorCount)
    {
        Fail(Diagnostics::Amd64BootStage::TimerCpu);
    }
    if (frame.Vector == ShootdownVector || frame.Vector == RescheduleVector)
    {
        const auto cpu = Platform::Synchronization::CurrentProcessor();
        if (!cpu.IsValid())
        {
            Fail(Diagnostics::Amd64BootStage::TimerCpu);
        }
        auto& evidence = Diagnostics::GetAmd64EvidenceRecord().Smp;
        if (frame.Vector == ShootdownVector)
        {
            static_cast<void>(__atomic_fetch_add(&evidence.ShootdownTaken[cpu.GetValue()], 1U, __ATOMIC_RELEASE));
            const auto drained = m_drains[cpu.GetValue()].Drain();
            if (drained.RejectedCount != 0U)
            {
                Fail(Diagnostics::Amd64BootStage::Shootdown);
            }
        }
        else
        {
            __atomic_store_n(&evidence.KickPending[cpu.GetValue()], 1U, __ATOMIC_RELEASE);
        }
        if (cpu.GetValue() == 0U)
        {
            LocalTimer().EndOfInterrupt();
        }
        else
        {
            m_processors[cpu.GetValue()]->EndOfInterrupt();
        }
        if (frame.Vector == RescheduleVector && (frame.Cs & 3U) == 3U)
        {
            const UInt64 resumeStack = m_entryContexts[CurrentIndex()].KernelStackTop;
            if (!PreemptUser())
            {
                Fail(Diagnostics::Amd64BootStage::UserTick);
            }
            ResumeUserOn(frame, resumeStack);
        }
        return;
    }
    if (frame.Vector == 255U)
    {
        return;
    }
    // The APIC spurious vector never set an ISR bit: do not send EOI.
    if (frame.Vector == Time::ApicSpuriousVector)
    {
        return;
    }
    // Device messages target CPU0's local APIC; a vector nothing is bound to is only counted, since
    // a device can write any vector while interrupt remapping is off.
    if (Interrupt::Amd64DeviceVectors::IsDeviceVector(frame.Vector))
    {
        m_deviceVectors.Dispatch(frame.Vector);
        LocalTimer().EndOfInterrupt();
        return;
    }
    if (frame.Vector == Time::ApicTimerVector)
    {
        Platform::Synchronization::ProcessorWatchdogTick(Time::ReadTsc(), frame.Rip);
        UInt64 processor{0U};
#if defined(ASTERIA_TARGET_AMD64)
        __asm__ __volatile__("movq %%gs:16,%0" : "=r"(processor));
#endif
        if (processor >= m_processorCount)
        {
            Fail(Diagnostics::Amd64BootStage::TimerCpu, processor);
        }
        if (processor != 0U)
        {
            if (m_processors[processor]->OnTimerInterrupt() && (frame.Cs & 3U) == 3U)
            {
                ++LocalState().TimerInterrupts;
                const UInt64 resumeStack = m_entryContexts[processor].KernelStackTop;
                if (!PreemptUser())
                {
                    Fail(Diagnostics::Amd64BootStage::UserTick);
                }
                ResumeUserOn(frame, resumeStack);
            }
            return;
        }
        if (LocalTimer().AcknowledgeInterrupt())
        {
            ++LocalState().TimerInterrupts;
            if (Scheduler::CaptureAmd64NativePreemption(frame))
            {
                return;
            }
            // A deadline that fires while a user thread runs is a scheduling point on that thread's
            // own kernel stack.
            if ((frame.Cs & 3U) == 3U)
            {
                const UInt64 resumeStack = m_entryContexts[CurrentIndex()].KernelStackTop;
                if (!PreemptUser())
                {
                    Fail(Diagnostics::Amd64BootStage::UserTick);
                }
                ResumeUserOn(frame, resumeStack);
            }
            else
            {
                LocalWait().OnTimerInterrupt();
            }
        }
        return;
    }
    // External vectors outside our bindings are stray interrupts, not CPU
    // exceptions. Count on the owning CPU and acknowledge its identity LAPIC.
    if (frame.Vector >= 32U && frame.Vector < 256U)
    {
        Diagnostics::RecordAmd64StrayInterrupt();
        LocalTimer().EndOfInterrupt();
        return;
    }
    // The stack this trap entered on belongs to the thread it returns to; a
    // blocking call below lets other threads select their own stacks.
    const UInt64 resumeStack = m_entryContexts[CurrentIndex()].KernelStackTop;
    // The client's, the spinner's and created threads' page faults reach the pager, their
    // supervisor; any other user fault stops the machine below.
    if ((frame.Vector == PageFaultVector) && ((frame.Cs & 3U) == 3U) &&
        (IsClientRunning() || m_spinner.IsRunning() || (m_threads.FindRunning() < System::Amd64UserThreadSlots)))
    {
        if (!BlockOnUserFault(frame))
        {
            Fail(Diagnostics::Amd64BootStage::UserFault, m_pager.GetChannelSnapshot().Deliveries);
        }
        ResumeUserOn(frame, resumeStack);
        return;
    }
    // Supervised user exceptions can only terminate that thread; the boot
    // client and spinner use the same supervisor as their page faults.
    if (((frame.Cs & 3U) == 3U) && Interrupt::IsSupervisedUserExceptionVector(frame.Vector))
    {
        if (IsClientRunning() || m_spinner.IsRunning() || m_threads.FindRunning() < System::Amd64UserThreadSlots)
        {
            if (!BlockUserOnFault(Interrupt::DescribeUserException(frame.Vector, frame.Error, frame.Rip)))
            {
                Fail(Diagnostics::Amd64BootStage::UserException, frame.Vector);
            }
            ResumeUserOn(frame, resumeStack);
            return;
        }
    }
    // A SYSCALL frame carries the program's own RFLAGS and RSP, which any
    // legal user state may hold; PrepareSystemCallReturn sanitizes RFLAGS and
    // checks the selectors and return address separately from their origin.
    if (!Interrupt::Amd64UserEntry::PrepareSystemCallReturn(frame))
    {
        if (Interrupt::Amd64UserEntry::IsSystemCallReturnFault(frame))
        {
            if (IsClientRunning() || m_spinner.IsRunning() || m_threads.FindRunning() < System::Amd64UserThreadSlots)
            {
                // An exception can only terminate. A busy supervisor yields through this kernel
                // continuation; retry here, since IRETQ cannot resume this return address.
                for (;;)
                {
                    if (!BlockUserOnFault(Interrupt::DescribeUserException(13U, 0U, frame.Rip)))
                    {
                        Fail(Diagnostics::Amd64BootStage::UserException, frame.Rip);
                    }
                }
            }
        }
        Diagnostics::StopAmd64Trap(frame);
    }
    if (!DispatchSystemCall(frame))
    {
        Fail(Diagnostics::Amd64BootStage::Syscall, frame.Rax | (m_pager.IsRunning() ? (UInt64{1U} << 32U) : 0U));
    }
    ResumeUserOn(frame, resumeStack);
}

// The tick's scheduling-point service, then the active run's refresh: a
// deadline that has not fired resumes the thread; one that has ends the run
// Ready and dispatches the next thread, returning once this one runs again.
// A reached TimerQueue deadline is the wait fixture's interrupt; the dispatch
// is rejected until the idle loop drained the queue.
bool BootRuntime::PreemptUser() noexcept
{
    if (!m_objects.ProgressFaultChannels() ||
        (CurrentIndex() == 0U && m_pager.ProgressFromIdle() == System::Amd64PagerIdleProgress::Failed) ||
        (m_threads.ProgressWaits() == System::Amd64ThreadWaitProgress::Failed))
    {
        return false;
    }
    auto& users = Diagnostics::GetAmd64EvidenceRecord().UserProcessors;
    const Size running = m_threads.FindRunning();
    const bool selfTest =
        running < System::Amd64UserThreadSlots &&
        users.Threads[running].DomainIdentity == m_pager.GetDomainRecord().Protection->GetIdentity().GetValue();
    Kernel::Diagnostics::RecordUserProcessor(users, CurrentIndex(), running, selfTest, true);
    auto& evidence = Diagnostics::GetAmd64EvidenceRecord().UserPreemption;
    static_cast<void>(__atomic_fetch_add(&evidence.UserTicks, 1U, __ATOMIC_RELAXED));
    const int reached = RefreshRunStop();
    if (reached <= 0)
    {
        Diagnostics::PublishAmd64Evidence(evidence.Published);
        return reached == 0;
    }
    if ((static_cast<unsigned>(reached) & TimerQueueDeadlineSource) != 0U)
    {
        static_cast<void>(__atomic_fetch_add(&evidence.TimerQueueTicks, 1U, __ATOMIC_RELAXED));
        LocalWait().OnTimerInterrupt();
    }
    // A created thread first: it runs in a boot domain but owns its run.
    const Size created = m_threads.FindRunning();
    if (created < System::Amd64UserThreadSlots)
    {
        Kernel::Diagnostics::CountUserProcessor(users.Processors[CurrentIndex()].RunStops);
        Diagnostics::PublishAmd64Evidence(evidence.Published);
        static_cast<void>(
            __atomic_fetch_add(&Diagnostics::GetAmd64EvidenceRecord().Threads.RunStops, 1U, __ATOMIC_RELAXED));
        RecordThreads();
        return m_threads.Preempt(created);
    }
    const Size domain = m_pager.IsRunning()     ? Diagnostics::Amd64EvidencePagerDomain
                        : m_spinner.IsRunning() ? Diagnostics::Amd64EvidenceSpinnerDomain
                                                : Diagnostics::Amd64EvidenceClientDomain;
    ++evidence.RunStops[domain];
    Diagnostics::PublishAmd64Evidence(evidence.Published);
    if (domain == Diagnostics::Amd64EvidencePagerDomain)
    {
        return m_pager.Preempt();
    }
    if (domain == Diagnostics::Amd64EvidenceSpinnerDomain)
    {
        return m_spinner.Preempt();
    }
    return PreemptClient();
}

int BootRuntime::RefreshRunStop() noexcept
{
    const auto refreshed = LocalController().RefreshTimerQueueDeadline(LocalTimer().GetTicks());
    if (refreshed.HasValue())
    {
        return 0;
    }
    const auto& failure = refreshed.GetErrorReference();
    if ((failure.Error != Kernel::Scheduler::ProcessorRunError::DeadlineReached) ||
        (failure.Deadline.AffectedSources == 0U))
    {
        return -1;
    }
    return failure.Deadline.AffectedSources;
}

UInt64 BootRuntime::DescribeDispatchFailure() const noexcept
{
    const auto& failure = LocalState().DispatchFailure;
    const Kernel::Scheduler::Thread* const thread = failure.ThreadStorage;
    const UInt64 owner = (thread == nullptr)       ? 0U
                         : (thread == &m_thread)   ? 1U
                         : m_pager.Runs(thread)    ? 2U
                         : m_spinner.Runs(thread)  ? 3U
                         : m_threads.Holds(thread) ? 4U
                                                   : 5U;
    return static_cast<UInt64>(failure.Error) | (static_cast<UInt64>(failure.ControllerError) << 8U) |
           (static_cast<UInt64>(failure.SchedulerError) << 16U) | (static_cast<UInt64>(failure.DeadlineError) << 24U) |
           (static_cast<UInt64>(failure.NativeError) << 32U) | (owner << 40U) |
           (static_cast<UInt64>(failure.ActiveRunRetained) << 48U);
}

// Idle with every runnable thread out of budget: the controller armed the replenishment wakeup, so
// wait, bounded, for the next tick. The earliest deadline of the pager's or a created thread's wait
// ends the wait too, and the stall bound starts there: nothing else interrupts an idle processor
// then.
bool BootRuntime::WaitForTickFromIdle() noexcept
{
    const UInt64 seen = LocalState().TimerInterrupts;
    const UInt64 start = LocalTimer().GetTicks();
    const UInt64 pager = m_pager.GetWaitDeadline();
    const UInt64 created = m_threads.GetWaitDeadline();
    const UInt64 deadline = ((pager == 0U) || ((created != 0U) && (created < pager))) ? created : pager;
    const UInt64 bound = ((deadline > start) ? (deadline - start) : 0U) + IdleTickTimeoutTicks;
    while (LocalState().TimerInterrupts == seen)
    {
        Platform::Synchronization::ProcessorWaitProgress();
#if defined(ASTERIA_TARGET_AMD64)
        __asm__ __volatile__("sti; pause; cli" : : : "memory");
#endif
        const UInt64 now = LocalTimer().GetTicks();
        if ((deadline != 0U) && (now >= deadline))
        {
            return true;
        }
        if (now - start > bound)
        {
            return false;
        }
    }
    return true;
}

// Scheduling durations use the BSP calibration also used by INIT/SIPI.
UInt64 BootRuntime::MeasureTicksPerMicrosecond() noexcept
{
    return (m_calibration.FrequencyHz + 999'999U) / 1'000'000U;
}

// An interrupt or fault frame carries the RFLAGS the processor pushed, which
// may include RF, including on timer interrupts in QEMU. Sanitize it like a
// system call's so the program resumes under one flags policy.
void BootRuntime::ResumeUserOn(Interrupt::Amd64TrapFrame& frame, const UInt64 kernelStackTop) noexcept
{
    auto& users = Diagnostics::GetAmd64EvidenceRecord().UserProcessors;
    const Size running = m_threads.FindRunning();
    Kernel::Diagnostics::RecordUserProcessor(users, CurrentIndex(), running,
                                             running < System::Amd64UserThreadSlots &&
                                                 users.Threads[running].DomainIdentity ==
                                                     m_pager.GetDomainRecord().Protection->GetIdentity().GetValue(),
                                             false);
    Platform::Synchronization::ServiceThreadRequests(true);
    frame.Rflags = Interrupt::SanitizeUserFlags(frame.Rflags);
    if (!Interrupt::Amd64UserEntry::IsUserReturnValid(frame))
    {
        if (Interrupt::Amd64UserEntry::IsUserReturnAddressFault(frame) &&
            (IsClientRunning() || m_spinner.IsRunning() || m_threads.FindRunning() < System::Amd64UserThreadSlots))
        {
            // A user branch may fault while its RIP lies outside the user range. Busy fault
            // delivery must never return or stop that frame.
            for (;;)
            {
                if (!BlockUserOnFault(Interrupt::DescribeUserException(13U, 0U, frame.Rip)))
                {
                    Fail(Diagnostics::Amd64BootStage::UserException, frame.Rip);
                }
            }
        }
        Fail(Diagnostics::Amd64BootStage::ResumeFrame, frame.Rip);
    }
    if (!Interrupt::Amd64UserEntry::SelectKernelStack(m_descriptors[CurrentIndex()], m_entryContexts[CurrentIndex()],
                                                      kernelStackTop))
    {
        Fail(Diagnostics::Amd64BootStage::ResumeStack, kernelStackTop);
    }
}

// A supervised CPL3 page fault (the client's, the spinner's or a created thread's): the boot pager
// resolves it while the thread is blocked, and the trap then returns to the faulting instruction. A
// fault that finds the pager's channel holding another yields and faults again when its thread next
// runs, however often (Kernel::Memory::DecideFaultOutcome). A created thread's fault or exception
// goes to its domain's supervisor; a thread the supervisor terminates never returns here.
bool BootRuntime::BlockCreatedOnFault(const Size created, const Kernel::Memory::FaultRecord& record) noexcept
{
    // Published before the delivery, which a Terminate never returns from.
    if (Platform::Synchronization::CurrentProcessor().GetValue() == 0U)
    {
        const auto table = m_threads.GetSnapshot();
        auto& threads = Diagnostics::GetAmd64EvidenceRecord().Threads;
        threads.Faults = table.Faults + 1U;
        threads.Exceptions = table.Exceptions + ((record.Cause == Kernel::Memory::FaultCause::Exception) ? 1U : 0U);
        Diagnostics::PublishAmd64Evidence(threads.Published);
    }
    const bool resumed = m_threads.BlockOnFault(created, record);
    RecordThreads();
    return resumed;
}

bool BootRuntime::BlockOnUserFault(const Interrupt::Amd64TrapFrame& frame) noexcept
{
    UInt64 address{0U};
#if defined(ASTERIA_TARGET_AMD64)
    __asm__ __volatile__("mov %%cr2,%0" : "=r"(address));
#endif
    Kernel::Memory::FaultRecord record{};
    if (Memory::Amd64PageFaultDecoder::Decode({address, frame.Rip, frame.Error}, record) !=
        Memory::Amd64PageFaultDecodeError::None)
    {
        return false;
    }
    return BlockUserOnFault(record);
}

// A supervised boot client and spinner have the same user-state isolation as a created thread.
bool BootRuntime::BlockUserOnFault(const Kernel::Memory::FaultRecord& record) noexcept
{
    // A created thread first: it runs in the pager's domain but is no boot
    // thread.
    const Size created = m_threads.FindRunning();
    if (created < System::Amd64UserThreadSlots)
    {
        return BlockCreatedOnFault(created, record);
    }
    const bool spinner = m_spinner.IsRunning();
    const Kernel::Memory::FaultDelivery delivery =
        spinner ? m_spinner.DeliverFault(m_pager, record)
                : m_pager.DeliverFault(m_thread, m_worker,
                                       {m_threadId, m_spaceIds[UserRootIndex], m_schedulingId, record, 0U,
                                        m_protection.GetIdentity().GetValue()},
                                       LocalState().ClientFaultState);
    switch (Kernel::Memory::DecideFaultOutcome(delivery, spinner ? LocalState().SpinnerFaultDeferrals
                                                                 : LocalState().ClientFaultDeferrals))
    {
    case Kernel::Memory::FaultOutcome::Resume:
        return true;
    case Kernel::Memory::FaultOutcome::Yield:
    {
        auto& supervision = Diagnostics::GetAmd64EvidenceRecord().Supervision;
        supervision.Deferrals = ++LocalState().FaultDeferrals;
        Diagnostics::PublishAmd64Evidence(supervision.Published);
        return m_pager.Progress() && (spinner ? m_spinner.Preempt() : PreemptClient());
    }
    case Kernel::Memory::FaultOutcome::Stop:
        break;
    }
    return false;
}

// Ends the running client's run Ready and dispatches the next thread; returns
// once the client runs again.
bool BootRuntime::PreemptClient() noexcept
{
    return IsClientRunning() &&
           LocalRunLoop().FinishAndDispatch(m_thread, m_worker, Kernel::Scheduler::ThreadRunDisposition::Ready, 0U,
                                            LocalState().PreemptReceipt, LocalState().PreemptFailure) &&
           (LocalState().PreemptReceipt.ThreadStorage == &m_thread) &&
           (LocalState().PreemptReceipt.State == Kernel::Scheduler::ProcessorKernelRunLoopState::Active);
}

bool BootRuntime::DispatchSystemCall(Interrupt::Amd64TrapFrame& frame) noexcept
{
    System::Amd64SystemCallRegisterImage registers{frame.Rax, frame.Rdi, frame.Rsi, frame.Rdx,
                                                   frame.R10, frame.R8,  frame.R9};
    const auto request = System::Amd64SystemCallRegisterCodec::DecodeRequest(registers);
    Kernel::System::SystemCallResponse response{};
    Kernel::System::SystemCallIngressFailure failure{};
    const bool management =
        (request.Number == static_cast<UInt64>(Kernel::System::SystemCallNumber::ObjectCreate)) ||
        (request.Number == static_cast<UInt64>(Kernel::System::SystemCallNumber::ObjectDestroy)) ||
        (request.Number == static_cast<UInt64>(Kernel::System::SystemCallNumber::MemoryMap)) ||
        (request.Number == static_cast<UInt64>(Kernel::System::SystemCallNumber::MemoryUnmap)) ||
        (request.Number == static_cast<UInt64>(Kernel::System::SystemCallNumber::ThreadTerminate)) ||
        (request.Number == static_cast<UInt64>(Kernel::System::SystemCallNumber::WaitSetBind)) ||
        (request.Number == static_cast<UInt64>(Kernel::System::SystemCallNumber::DeviceControl)) ||
        (request.Number == static_cast<UInt64>(Kernel::System::SystemCallNumber::IommuControl)) ||
        (request.Number == static_cast<UInt64>(Kernel::System::SystemCallNumber::InterruptControl));
    // Exited created threads are unbound first, so the call's collection can free their slots.
    if (management && !m_threads.Reap())
    {
        return false;
    }
    // A created thread first: it runs in a boot domain but is none of the
    // boot threads, whose paths below assume their own thread.
    const Size created = m_threads.FindRunning();
    if (created < System::Amd64UserThreadSlots)
    {
        if (!m_threads.DispatchSystemCall(created, request, response, LocalState().Scratch))
        {
            return false;
        }
        if (management)
        {
            RecordManagement();
        }
    }
    else if (m_pager.IsRunning())
    {
        if (!m_pager.DispatchSystemCall(request, response, LocalState().Scratch))
        {
            return false;
        }
        if ((request.Number == static_cast<UInt64>(Kernel::System::SystemCallNumber::FaultResolve)) &&
            (request.Arguments[2U] == static_cast<UInt64>(Abi::FaultResolveKind::Terminate)) &&
            (response.Error == Kernel::System::SystemCallError::None))
        {
            RecordTermination();
        }
        if ((request.Number == static_cast<UInt64>(Kernel::System::SystemCallNumber::ObjectDestroy)) &&
            (request.Arguments[2U] == Abi::ObjectTypeThread) &&
            (response.Error == Kernel::System::SystemCallError::WouldBlock))
        {
            ++Diagnostics::GetAmd64EvidenceRecord().Threads.DestroyPolls;
        }
        if (management)
        {
            RecordManagement();
        }
    }
    else if (m_spinner.IsRunning())
    {
        if (!m_spinner.DispatchSystemCall(request, response, LocalState().Scratch))
        {
            return false;
        }
    }
    else if (request.Number == static_cast<UInt64>(Kernel::System::SystemCallNumber::WaitSetWait))
    {
        if (!LocalWait().Dispatch(request, response))
        {
            return false;
        }
    }
    else if (!Kernel::System::SystemCallIngress::Dispatch(LocalRunLoop(), m_images[ClientImage].Access,
                                                          LocalState().Scratch, request, response, failure))
    {
        return false;
    }
    System::Amd64SystemCallRegisterCodec::ApplyResponse(response, registers);
    frame.Rax = registers.Rax;
    frame.Rdx = registers.Rdx;
    frame.R8 = registers.R8;
    Diagnostics::CheckAmd64StopPoint();
    return true;
}
} // Namespace

Foundation::UInt32 GetAmd64FirmwareApicId() noexcept
{
    Interrupt::Amd64LocalApic apic;
    return apic.InitializeIdentity() ? apic.GetId() : 0xFFFF'FFFFU;
}

Foundation::UInt64 GetAmd64BootTscTicksPerMicrosecond() noexcept
{
    return g_runtime == nullptr ? 0U : g_runtime->GetTscTicksPerMicrosecond();
}

const char* GetAmd64BootRuntimeRejection() noexcept
{
    return g_rejection;
}

bool PrepareAmd64BootRuntime(const Amd64BootRuntimeInput& input) noexcept
{
    // The runtime starts the enabled processors; online-capable records are
    // omitted (SelectAmd64Processors).
    const Size processors =
        input.BootInformation == nullptr ? 0U : CountEnabledAmd64Processors(input.BootInformation->Acpi);
    if ((input.BootInformation == nullptr) || (input.InitialMappings == nullptr) || (input.Capabilities == nullptr) ||
        !input.InitialMappings->IsSealed() ||
        !IsReserved(*input.BootInformation, input.PoolPhysicalStart, RuntimePoolPages) ||
        !IsReserved(*input.BootInformation, input.StartupPhysicalStart, 2U) ||
        (ValidateMetadataArenaReceipt(*input.BootInformation) != MetadataArenaReceiptError::None) ||
        (input.MetadataArenaPhysicalStart != input.BootInformation->Kernel.MetadataArena.PhysicalStart) ||
        (input.MetadataArenaPages != input.BootInformation->Kernel.MetadataArena.PageCount) ||
        (input.StartupPhysicalStart > 0xFE000U) ||
        ((input.PoolPhysicalStart < input.StartupPhysicalStart + 8192U) &&
         (input.StartupPhysicalStart < input.PoolPhysicalStart + RuntimePoolPages * 4096U)) ||
        (processors == 0U) || (processors > 4U))
    {
        return Reject("INPUT");
    }
    if (g_runtime == nullptr)
    {
        const auto key = Kernel::Object::DeriveCapabilityValidationKey(
            input.BootInformation->Entropy, Kernel::Object::CapabilityKeyPurpose::BootstrapSpace);
        const auto pagerKey = Kernel::Object::DeriveCapabilityValidationKey(
            input.BootInformation->Entropy, Kernel::Object::CapabilityKeyPurpose::PagerSpace);
        const auto tokenKey = Kernel::Object::DeriveCapabilityValidationKey(
            input.BootInformation->Entropy, Kernel::Object::CapabilityKeyPurpose::FaultTokenSpace);
        const auto spinnerKey = Kernel::Object::DeriveCapabilityValidationKey(
            input.BootInformation->Entropy, Kernel::Object::CapabilityKeyPurpose::SpinnerSpace);
        if (!key.HasValue())
        {
            return Reject("CAPABILITY-KEY", static_cast<UInt64>(key.GetError()));
        }
        if (!pagerKey.HasValue() || !tokenKey.HasValue() || !spinnerKey.HasValue())
        {
            return Reject("PAGER-KEY");
        }
        g_runtime = new (g_runtimeStorage) BootRuntime{static_cast<UInt8>((1U << processors) - 1U), key.GetValue(),
                                                       pagerKey.GetValue(), tokenKey.GetValue(), spinnerKey.GetValue()};
    }
    return g_runtime->Prepare(input);
}

extern "C" [[noreturn]] void AsteriaAmd64InitialRuntimeEntry() noexcept
{
    // Entered once the owned root, stack and descriptors are in force.
    Platform::Firmware::WriteDiagnosticAscii("ASTERIA:STAGE:AMD64-INITIAL-ADDRESS-SPACE\r\n");
    if (g_runtime == nullptr)
    {
        Fail(Diagnostics::Amd64BootStage::NotPrepared);
    }
    g_runtime->Start();
}
} // Namespace Asteria::Platform::Amd64::Bootstrap

namespace Asteria::Platform::Amd64::Interrupt
{
extern "C" void AsteriaAmd64DispatchTrap(Amd64TrapFrame* const frame) noexcept
{
    if (frame != nullptr && frame->Vector == 2U)
    {
        Diagnostics::HandleAmd64Nmi(*frame);
        return;
    }
    if ((frame == nullptr) || (Bootstrap::g_runtime == nullptr))
    {
        Bootstrap::Fail(Diagnostics::Amd64BootStage::NoTrapOwner);
    }
    Bootstrap::g_runtime->Trap(*frame);
}
} // Namespace Asteria::Platform::Amd64::Interrupt
