// SPDX-License-Identifier: Apache-2.0
#include "Kernel/Device/IommuDomain.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Platform/Device/VtdDomainBackend.hpp"
#include "Platform/Device/VtdDomainGroup.hpp"
#include "Platform/Device/VtdUnit.hpp"
#include "Platform/Device/VtdTopology.hpp"

#include <cstdio>

#include "Support/ObjectRegistryFixture.hpp"
#include "Support/Random.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt16;
using Foundation::UInt32;
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
using Kernel::Object::ObjectRegistry;
using Platform::Device::IVtdCacheMaintenance;
using Platform::Device::IVtdRegisters;
using Platform::Device::VtdDomainBackend;
using Platform::Device::VtdTablePage;
using Platform::Device::VtdUnit;
using Platform::Device::VtdUnitError;
using Platform::Device::VtdHandoffFailure;
using Platform::Synchronization::SpinLock;

constexpr UInt32 TranslationEnable{UInt32{1U} << 31U};
constexpr UInt32 RootTablePointer{UInt32{1U} << 30U};
constexpr UInt64 Busy{UInt64{1U} << 63U};
constexpr Size IotlbRegister{0xF8U};
constexpr Size FaultRecord{0x220U};
// QEMU's q35 unit: 39- and 48-bit walks, the drain bits, one fault record at
// 0x220, the IOTLB registers at 0xF0, and no coherency bit.
constexpr UInt64 QemuCapability{(UInt64{47U} << 16U) | (UInt64{0x6U} << 8U) | (UInt64{0x22U} << 24U) |
                                (UInt64{3U} << 54U)};
constexpr UInt64 QemuExtendedCapability{UInt64{0xFU} << 8U};

/// A register model of one unit: commands complete at once unless Stuck,
/// GCMD's TE and SRTP are reflected in GSTS, and one fault record can be
/// injected; every invalidation command is logged.
class FakeVtd final : public IVtdRegisters, public IVtdCacheMaintenance
{
public:
    [[nodiscard]] UInt32 Read32(const Size offset) const noexcept override
    {
        switch (offset)
        {
        case 0x00U:
            return 0x10U;
        case 0x1CU:
            return Status;
        case 0x34U:
            return FaultStatus;
        case 0x38U:
            return FaultEventControl;
        case 0xA0U:
            return InvalidationEventControl;
        case 0x64U:
            return ProtectedMemory;
        default:
            return 0U;
        }
    }

    [[nodiscard]] UInt64 Read64(const Size offset) const noexcept override
    {
        switch (offset)
        {
        case 0x20U:
            return RootAddress;
        case 0x08U:
            return Capability;
        case 0x10U:
            return ExtendedCapability;
        case 0x28U:
            return Stuck ? Busy : 0U;
        case IotlbRegister:
            return Stuck ? Busy : 0U;
        case 0x80U:
            ++QueueReads;
            return (!QueueStuck && (QueueReads >= 3U)) ? QueueTail : 0U;
        case 0x88U:
            return QueueTail;
        case 0x90U:
            return QueueAddress;
        case FaultRecord:
            return FaultLow;
        case FaultRecord + 8U:
            return FaultHigh;
        default:
            return 0U;
        }
    }

    void Write32(const Size offset, const UInt32 value) noexcept override
    {
        RecordEvent(offset, value);
        if (offset == 0x18U)
        {
            constexpr UInt32 persistent{TranslationEnable | (UInt32{1U} << 26U) | (UInt32{1U} << 25U) |
                                        (UInt32{1U} << 28U) | (UInt32{1U} << 23U)};
            const UInt32 before = Status;
            Status = (Status & ~persistent) | (value & persistent);
            if (HandoffStuck)
            {
                Status |= before & TranslationEnable;
            }
            if ((value & (UInt32{1U} << 27U)) != 0U)
            {
                ++WriteFlushes;
                if (FlushStuck)
                {
                    Status |= UInt32{1U} << 27U;
                }
            }
            if ((value & RootTablePointer) != 0U)
            {
                Status |= RootTablePointer;
                InstalledRoot = RootAddress;
            }
            ++Commands;
        }
        else if (offset == 0x64U)
        {
            ProtectedMemory = PmrStuck ? 1U : 0U;
        }
        else if (offset == 0x34U)
        {
            FaultStatus &= ~(value & 1U);
        }
        else if (offset == 0x38U)
        {
            FaultEventControl = value;
        }
        else if (offset == 0xA0U)
        {
            InvalidationEventControl = value;
        }
    }

    void Write64(const Size offset, const UInt64 value) noexcept override
    {
        RecordEvent(offset, value);
        if (offset == 0x20U)
        {
            RootAddress = value;
        }
        else if (offset == 0x88U)
        {
            const Size words = ((QueueAddress & 0x800U) != 0U) ? 8U : 4U;
            WaitValid = Queue[0U] == 0x25U && Queue[1U] == 1U &&
                        Queue[2U] == static_cast<UInt32>((QueueAddress & ~UInt64{0xFFFU}) + QueueTail) &&
                        Queue[3U] == static_cast<UInt32>(QueueAddress >> 32U);
            for (Size word{4U}; word < words; ++word)
            {
                WaitValid = WaitValid && Queue[word] == 0U;
            }
            PostedSlot = (QueueAddress & ~UInt64{0xFFFU}) + QueueTail;
            Posted = true;
            QueueTail = value;
            if (!WaitStuck && WaitValid)
            {
                Queue[0U] = 1U;
            }
        }
        else if (((offset == 0x28U) || (offset == IotlbRegister)) && (Logged < 16U))
        {
            Log[Logged] = value;
            LogOffsets[Logged] = offset;
            ++Logged;
        }
        else if ((offset == (FaultRecord + 8U)) && ((value & Busy) != 0U))
        {
            FaultHigh &= ~Busy;
            FaultStatus &= ~2U;
        }
    }

    [[nodiscard]] bool WriteQueueWord(const UInt64 address, const UInt32 value) noexcept override
    {
        if (QueueDenied)
        {
            return false;
        }
        const UInt64 slot = (QueueAddress & ~UInt64{0xFFFU}) + QueueTail;
        if (address < slot || address >= slot + 32U || (address & 3U) != 0U)
        {
            return false;
        }
        Queue[static_cast<Size>((address - slot) / 4U)] = value;
        ++QueueWrites;
        return true;
    }
    [[nodiscard]] bool ReadQueueWord(const UInt64 address, UInt32& value) const noexcept override
    {
        if (QueueDenied || (address & 3U) != 0U)
        {
            return false;
        }
        if (address == ProofStatusAddress)
        {
            value = ProofStatus;
            return true;
        }
        if (Posted && address == PostedSlot)
        {
            value = Queue[0U];
            return true;
        }
        const UInt64 bytes = UInt64{4096U} << (QueueAddress & 7U);
        const UInt64 width = (QueueAddress & 0x800U) != 0U ? 32U : 16U;
        const UInt64 base = QueueAddress & ~UInt64{0xFFFU};
        const UInt64 previous = base + ((QueueTail + bytes - width) % bytes);
        if (address >= previous && address < previous + 16U)
        {
            value = Previous[static_cast<Size>((address - previous) / 4U)];
            return true;
        }
        if (address == base + QueueTail)
        {
            value = Queue[0U];
            return true;
        }
        return false;
    }
    [[nodiscard]] bool CanWriteFirmwareMemory(const UInt64 address, const UInt64 bytes) const noexcept override
    {
        if (QueueDenied || (bytes > 4U ? !QueueFirmwareOwned : !StatusFirmwareOwned))
        {
            return false;
        }
        return MemoryMap == nullptr ||
               Platform::Device::VtdFirmwareQueueRangeIsWritable(*MemoryMap, address, address + bytes - 1U, Ownership);
    }
    void ReportHandoff(const Platform::Device::VtdFirmwareState& state) noexcept override
    {
        Observed = state;
        ReportBeforeWrites = Events == 0U && QueueWrites == 0U;
        ++Reports;
    }
    void ReportRefusal(const VtdHandoffFailure failure) noexcept override
    {
        Refusal = failure;
        ++Refusals;
    }

    void Flush(const volatile void*) noexcept override
    {
        ++Flushes;
    }

    void FlushTables(const volatile void* const start, const Size bytes) noexcept override
    {
        const auto first = reinterpret_cast<Foundation::UIntPointer>(start) & ~Foundation::UIntPointer{63U};
        const auto end = reinterpret_cast<Foundation::UIntPointer>(start) + bytes;
        for (Foundation::UIntPointer line{first}; line < end; line += 64U)
        {
            ++Flushes;
        }
    }

    void Fence() noexcept override
    {
        ++Fences;
    }

    void InjectFault(const UInt16 source, const UInt64 reason, const UInt64 address, const bool read) noexcept
    {
        FaultLow = address;
        FaultHigh = Busy | (read ? (UInt64{1U} << 62U) : 0U) | (reason << 32U) | source;
        FaultStatus |= 2U;
    }

    void RecordEvent(const Size offset, const UInt64 value) noexcept
    {
        if (Events < 64U)
        {
            EventOffsets[Events] = offset;
            EventValues[Events] = value;
            ++Events;
        }
    }

    UInt64 Capability{QemuCapability};
    UInt64 ExtendedCapability{QemuExtendedCapability};
    UInt32 Status{0U};
    UInt32 FaultStatus{0U};
    UInt32 FaultEventControl{0U};
    UInt32 InvalidationEventControl{0U};
    Platform::Device::VtdFirmwareState Observed{};
    Size Reports{0U};
    Size Refusals{0U};
    bool ReportBeforeWrites{false};
    VtdHandoffFailure Refusal{VtdHandoffFailure::None};
    UInt64 FaultLow{0U};
    UInt64 FaultHigh{0U};
    UInt64 RootAddress{0U};
    UInt64 InstalledRoot{0U};
    UInt64 Log[16U]{};
    Size LogOffsets[16U]{};
    Size Logged{0U};
    Size Commands{0U};
    Size Flushes{0U};
    Size Fences{0U};
    UInt64 EventValues[64U]{};
    Size EventOffsets[64U]{};
    Size Events{0U};
    Size WriteFlushes{0U};
    UInt32 ProtectedMemory{0U};
    UInt64 QueueTail{0U};
    UInt64 QueueAddress{0x4000U};
    UInt32 Queue[8U]{};
    UInt32 Previous[4U]{};
    UInt64 ProofStatusAddress{0x9000U};
    UInt32 ProofStatus{0U};
    UInt64 PostedSlot{0U};
    bool Posted{false};
    Size QueueWrites{0U};
    bool QueueFirmwareOwned{true};
    bool StatusFirmwareOwned{true};
    const Boot::BootInfo* MemoryMap{nullptr};
    Platform::Device::VtdFirmwareMemoryOwnership Ownership{};
    bool WaitValid{false};
    bool WaitStuck{false};
    bool QueueDenied{false};
    mutable Size QueueReads{0U};
    bool QueueStuck{false};
    bool HandoffStuck{false};
    bool PmrStuck{false};
    bool FlushStuck{false};
    bool Stuck{false};
};

/// Table pages whose "physical" addresses are their own.
struct Tables
{
    alignas(4096) UInt64 Pages[13U][512U]{};

    [[nodiscard]] VtdTablePage Page(const Size index) noexcept
    {
        return VtdTablePage{Pages[index], static_cast<UInt64>(reinterpret_cast<Foundation::UIntPointer>(Pages[index]))};
    }
};

class FakeRequester final : public IIommuDevice
{
public:
    explicit FakeRequester(const UInt32 requester) noexcept : m_requester{requester}
    {
    }

    [[nodiscard]] IommuDeviceError Claim(const IommuDomain&) noexcept override
    {
        return IommuDeviceError::None;
    }

    [[nodiscard]] IommuDeviceError Release(const IommuDomain&) noexcept override
    {
        return IommuDeviceError::None;
    }

    [[nodiscard]] bool HasTrustedDma() const noexcept override
    {
        return false;
    }
    [[nodiscard]] UInt32 GetRequesterId() const noexcept override
    {
        return m_requester;
    }

private:
    UInt32 m_requester;
};

[[nodiscard]] VtdUnitError Configure(VtdUnit& unit, Tables& tables) noexcept
{
    // 00:02.0, 00:1f.0 and 01:00.0, and one requester on each of four more
    // buses, of which only the first fits.
    constexpr UInt16 requesters[]{0x0010U, 0x00F8U, 0x0100U, 0x0200U, 0x0300U, 0x0400U, 0x0010U};
    const VtdTablePage contexts[VtdUnit::MaximumBuses]{tables.Page(1U), tables.Page(2U), tables.Page(3U),
                                                       tables.Page(4U)};
    return unit.Configure(tables.Page(0U), contexts, requesters, sizeof(requesters) / sizeof(requesters[0U]));
}

// A unit installs a root table with one context table per served bus, every entry blocked,
// invalidates globally and translates after disabling any firmware translation. Unsupported walks
// or commands that do not complete refuse the unit.
void TestEnable(TestContext& runner) noexcept
{
    static Tables tables;
    FakeVtd registers;
    VtdUnit unit{registers, registers};
    const VtdUnitError configured = Configure(unit, tables);
    const VtdUnitError enabled = unit.Enable();
    const UInt64 root = tables.Page(0U).PhysicalAddress;
    ASTERIA_EXPECT(runner, "a unit roots one context table per served bus and starts translating",
                   configured == VtdUnitError::None, enabled == VtdUnitError::None, unit.IsEnabled(),
                   unit.GetLevels() == 3U, registers.InstalledRoot == root,
                   (registers.Status & TranslationEnable) != 0U, registers.FaultEventControl == (UInt32{1U} << 31U),
                   tables.Pages[0U][0U] == (tables.Page(1U).PhysicalAddress | 1U),
                   tables.Pages[0U][2U] == (tables.Page(2U).PhysicalAddress | 1U),
                   tables.Pages[0U][4U] == (tables.Page(3U).PhysicalAddress | 1U),
                   tables.Pages[0U][6U] == (tables.Page(4U).PhysicalAddress | 1U), tables.Pages[0U][8U] == 0U,
                   unit.Serves(0x0010U), unit.Serves(0x00F8U), unit.Serves(0x0300U), !unit.Serves(0x0400U),
                   !unit.Serves(0x0018U), tables.Pages[1U][2U * 0x10U] == 0U, registers.Logged == 2U,
                   registers.Log[0U] == (Busy | (UInt64{1U} << 61U)),
                   registers.Log[1U] == (Busy | (UInt64{1U} << 60U) | (UInt64{3U} << 48U)), registers.Flushes != 0U,
                   unit.GetSnapshot().Served == 5U, unit.GetSnapshot().DroppedRequesters == 1U);

    static Tables otherTables;
    FakeVtd active;
    active.Status = TranslationEnable;
    VtdUnit activeUnit{active, active};
    FakeVtd unsupported;
    unsupported.Capability = QemuCapability & ~(UInt64{0x1FU} << 8U);
    VtdUnit unsupportedUnit{unsupported, unsupported};
    FakeVtd outside;
    outside.Capability = QemuCapability | (UInt64{0x3FFU} << 24U);
    VtdUnit outsideUnit{outside, outside};
    FakeVtd stuck;
    stuck.Stuck = true;
    VtdUnit stuckUnit{stuck, stuck};
    const VtdUnitError stuckConfigured = Configure(stuckUnit, otherTables);
    const VtdUnitError timedOut = stuckUnit.Enable();
    ASTERIA_EXPECT(runner, "firmware translation is taken over; unsupported walks and incomplete commands are refused",
                   Configure(activeUnit, otherTables) == VtdUnitError::None,
                   activeUnit.GetSnapshot().HandoffStatus == TranslationEnable,
                   (active.Status & TranslationEnable) == 0U,
                   Configure(unsupportedUnit, otherTables) == VtdUnitError::Unsupported,
                   Configure(outsideUnit, otherTables) == VtdUnitError::Unsupported,
                   stuckConfigured == VtdUnitError::None, timedOut == VtdUnitError::TimedOut, !stuckUnit.IsEnabled(),
                   stuckUnit.GetLevels() == 0U, stuckUnit.AttachContext(0x0010U, 1U, 0x1000U) == VtdUnitError::Failed);
}

// Attaching writes the entry's domain half, then the present half, and
// invalidates the device's context; detaching clears the present half,
// invalidates the context and the domain's IOTLB, then the domain half.
void TestContexts(TestContext& runner) noexcept
{
    static Tables tables;
    FakeVtd registers;
    VtdUnit unit{registers, registers};
    const bool ready = (Configure(unit, tables) == VtdUnitError::None) && (unit.Enable() == VtdUnitError::None);
    registers.Logged = 0U;
    const UInt64 top = tables.Page(5U).PhysicalAddress;
    const VtdUnitError attached = unit.AttachContext(0x0010U, 2U, top);
    const UInt64 low = tables.Pages[1U][2U * 0x10U];
    const UInt64 high = tables.Pages[1U][(2U * 0x10U) + 1U];
    const UInt64 contextCommand = registers.Log[0U];
    const VtdUnitError twice = unit.AttachContext(0x0010U, 2U, top);
    const VtdUnitError unserved = unit.AttachContext(0x0018U, 2U, top);
    const VtdUnitError foreign = unit.DetachContext(0x0010U, 3U);
    const VtdUnitError detached = unit.DetachContext(0x0010U, 2U);
    ASTERIA_EXPECT(runner, "a requester translates through its domain only while attached", ready,
                   attached == VtdUnitError::None, low == (top | 1U), high == (1U | (UInt64{2U} << 8U)),
                   contextCommand == (Busy | (UInt64{3U} << 61U) | (UInt64{0x0010U} << 16U) | 2U),
                   twice == VtdUnitError::InvalidArguments, unserved == VtdUnitError::NotServed,
                   foreign == VtdUnitError::InvalidArguments, detached == VtdUnitError::None,
                   tables.Pages[1U][2U * 0x10U] == 0U, tables.Pages[1U][(2U * 0x10U) + 1U] == 0U,
                   registers.Logged == 3U,
                   registers.Log[2U] == (Busy | (UInt64{2U} << 60U) | (UInt64{2U} << 32U) | (UInt64{3U} << 48U)),
                   unit.AttachContext(0x0010U, 0U, top) == VtdUnitError::InvalidArguments,
                   unit.AttachContext(0x0010U, VtdUnit::MaximumDomain + 1U, top) == VtdUnitError::InvalidArguments);
}

// Fault records are read, attributed to the domain holding their source and
// cleared; an overflow is counted and cleared.
void TestFaults(TestContext& runner) noexcept
{
    static Tables tables;
    FakeVtd registers;
    VtdUnit unit{registers, registers};
    const bool ready = (Configure(unit, tables) == VtdUnitError::None) && (unit.Enable() == VtdUnitError::None) &&
                       (unit.AttachContext(0x0010U, 1U, tables.Page(5U).PhysicalAddress) == VtdUnitError::None);
    registers.InjectFault(0x0010U, 5U, 0x0020'0123U, false);
    registers.FaultStatus |= 1U;
    unit.PollFaults();
    const auto attributed = unit.GetSnapshot();
    registers.InjectFault(0x00F8U, 6U, 0x0030'0000U, true);
    unit.PollFaults();
    const auto unattributed = unit.GetSnapshot();
    unit.PollFaults();
    ASTERIA_EXPECT(runner, "a polled fault is recorded against its domain and cleared", ready, attributed.Faults == 1U,
                   unit.GetDomainFaults(1U).Count == 1U, unit.GetDomainFaults(1U).Last.Reason == 5U,
                   unit.GetDomainFaults(2U).Count == 0U, attributed.LastFault.Source == 0x0010U,
                   attributed.LastFault.Reason == 5U, attributed.LastFault.Address == 0x0020'0000U,
                   !attributed.LastFault.Read, attributed.Overflows == 1U, (registers.FaultStatus & 1U) == 0U,
                   unattributed.Faults == 2U, unattributed.Unattributed == 1U, unattributed.LastFault.Read,
                   unit.GetSnapshot().Faults == 2U, (registers.FaultHigh & Busy) == 0U);
}

[[nodiscard]] IommuBackendMapRequest MakeRequest(const UInt64 dma, const UInt64 physical, const UInt64 pages,
                                                 const DmaPermissions permissions) noexcept
{
    return IommuBackendMapRequest{
        .DmaStart = DmaAddress{dma},
        .PhysicalStart = Kernel::Memory::PhysicalAddress{physical},
        .Pages = Kernel::Memory::PageCount{pages},
        .Permissions = permissions,
        .Reserved = {},
    };
}

// A domain backend: the top table at Claim, tables as mappings need them,
// none past its eight pages, and Release blocks its requesters again.
void TestBackend(TestContext& runner) noexcept
{
    static Tables tables;
    FakeVtd registers;
    VtdUnit unit{registers, registers};
    const bool ready = (Configure(unit, tables) == VtdUnitError::None) && (unit.Enable() == VtdUnitError::None);
    const VtdTablePage pages[VtdDomainBackend::TablePages]{tables.Page(5U),  tables.Page(6U), tables.Page(7U),
                                                           tables.Page(8U),  tables.Page(9U), tables.Page(10U),
                                                           tables.Page(11U), tables.Page(12U)};
    VtdDomainBackend backend{unit, 1U, pages};
    SpinLock synchronization;
    Asteria::TestSupport::TestObjectRegistry registry{synchronization};
    const IommuDomain owner{registry, backend};
    const IommuDomain other{registry, backend};
    const DmaPermissions read{DmaPermission::Read};
    const DmaPermissions write{DmaPermission::Write};

    const IommuBackendOwnershipError wide = backend.Claim(owner, 65U);
    const IommuBackendOwnershipError claimed = backend.Claim(owner, 32U);
    const IommuBackendOwnershipError taken = backend.Claim(other, 32U);
    FakeRequester edu{0x0010U};
    FakeRequester stranger{0x0018U};
    FakeRequester segment{0x0001'0010U};
    const IommuBackendError attached = backend.Attach(edu);
    const IommuBackendError unserved = backend.Attach(stranger);
    const IommuBackendError foreignSegment = backend.Attach(segment);
    const UInt64 context = tables.Pages[1U][2U * 0x10U];
    const IommuBackendError mappedRead = backend.Map(MakeRequest(0x0010'0000U, 0x7000'0000U, 1U, read));
    const Size afterRead = backend.GetUsedPages();
    const IommuBackendError mappedWrite = backend.Map(MakeRequest(0x0020'0000U, 0x7100'0000U, 2U, write));
    const IommuBackendError beyond = backend.Map(MakeRequest(0xFFFF'F000U, 0x7200'0000U, 2U, write));
    ASTERIA_EXPECT(runner, "a claimed domain attaches served requesters and maps through tables it allocates", ready,
                   wide == IommuBackendOwnershipError::UnsupportedConfiguration,
                   claimed == IommuBackendOwnershipError::None, taken == IommuBackendOwnershipError::AlreadyOwned,
                   attached == IommuBackendError::None, unserved == IommuBackendError::UnsupportedConfiguration,
                   foreignSegment == IommuBackendError::UnsupportedConfiguration,
                   context == (tables.Page(5U).PhysicalAddress | 1U), mappedRead == IommuBackendError::None,
                   afterRead == 3U, tables.Pages[5U][0U] == (tables.Page(6U).PhysicalAddress | 3U),
                   tables.Pages[6U][0U] == (tables.Page(7U).PhysicalAddress | 3U),
                   tables.Pages[7U][0x100U] == (0x7000'0000U | 1U), mappedWrite == IommuBackendError::None,
                   backend.GetUsedPages() == 4U, tables.Pages[8U][0U] == (0x7100'0000U | 2U),
                   tables.Pages[8U][1U] == (0x7100'1000U | 2U), beyond == IommuBackendError::InvalidRequest);

    // Four more 2 MiB windows fill the eight pages; a fifth is refused before
    // anything is written.
    bool filled{true};
    for (UInt64 window{2U}; window < 6U; ++window)
    {
        filled = filled &&
                 (backend.Map(MakeRequest(window * 0x0020'0000U, 0x7300'0000U, 1U, read)) == IommuBackendError::None);
    }
    const IommuBackendError full = backend.Map(MakeRequest(6U * 0x0020'0000U, 0x7300'0000U, 1U, read));
    const IommuBackendError spanning = backend.Map(MakeRequest(0x001F'F000U, 0x7400'0000U, 1U, read));
    registers.Logged = 0U;
    const IommuBackendError unmapped = backend.Unmap(MakeRequest(0x0020'0000U, 0x7100'0000U, 2U, write));
    const UInt64 invalidation = registers.Log[0U];
    const IommuBackendError again = backend.Unmap(MakeRequest(0x0020'0000U, 0x7100'0000U, 2U, write));
    ASTERIA_EXPECT(runner, "tables run out before anything is written, and an unmap invalidates the domain", filled,
                   backend.GetUsedPages() == 8U, full == IommuBackendError::CapacityExceeded,
                   spanning == IommuBackendError::None, unmapped == IommuBackendError::None, tables.Pages[8U][0U] == 0U,
                   tables.Pages[8U][1U] == 0U,
                   invalidation == (Busy | (UInt64{2U} << 60U) | (UInt64{1U} << 32U) | (UInt64{3U} << 48U)),
                   again == IommuBackendError::CorruptState);

    const IommuBackendOwnershipError wrongOwner = backend.Release(other);
    const IommuBackendOwnershipError released = backend.Release(owner);
    bool zero{true};
    for (Size page{5U}; page < 13U; ++page)
    {
        for (const UInt64 entry : tables.Pages[page])
        {
            zero = zero && (entry == 0U);
        }
    }
    const IommuBackendOwnershipError reclaimed = backend.Claim(other, 39U);
    ASTERIA_EXPECT(runner, "release blocks the domain's requesters again and clears its tables",
                   wrongOwner == IommuBackendOwnershipError::InvalidOwner, released == IommuBackendOwnershipError::None,
                   tables.Pages[1U][2U * 0x10U] == 0U, zero, unit.GetSnapshot().Attached == 0U,
                   reclaimed == IommuBackendOwnershipError::None, backend.GetUsedPages() == 1U,
                   backend.Detach(edu) == IommuBackendError::InvalidRequest);
}
void TestDisplayObservation(TestContext& runner) noexcept
{
    FakeVtd displayRegisters;
    displayRegisters.Capability = 0U;
    VtdUnit display{displayRegisters, displayRegisters};
    ASTERIA_EXPECT(runner, "an untranslated display unit is observed without translation capabilities or table writes",
                   display.PrepareDisabled() == VtdUnitError::None, !display.IsEnabled(),
                   display.GetSnapshot().Version == 0x10U, display.GetSnapshot().Capability == 0U,
                   display.GetConfiguredLevels() == 0U, displayRegisters.Commands == 0U, displayRegisters.Flushes == 0U,
                   display.Enable() == VtdUnitError::NotConfigured);
    FakeVtd activeRegisters;
    activeRegisters.Status = TranslationEnable;
    VtdUnit active{activeRegisters, activeRegisters};
    ASTERIA_EXPECT(runner, "an active display handoff is disabled without installing a translation root",
                   active.PrepareDisabled() == VtdUnitError::None,
                   active.GetSnapshot().HandoffStatus == TranslationEnable, activeRegisters.Status == 0U,
                   activeRegisters.Commands == 1U, activeRegisters.RootAddress == 0U);
}

// Physical memory management: immutable post-handoff registers drive admission, independently of
// configuration failures and later changes to registers.
void TestImportHandoffRecords(TestContext& runner) noexcept
{
    using Platform::Device::FirmwareHandoffOutcome;
    using Platform::Device::VtdHandoffRecord;
    FakeVtd registers;
    registers.Status = (UInt32{1U} << 31U) | (UInt32{1U} << 26U);
    registers.FaultStatus = 0x10U;
    registers.RootAddress = 0x9000U;
    registers.QueueAddress = 0x20002U; // four pages from IQA.QS.
    registers.QueueTail = 0U;
    VtdUnit unit{registers, registers};
    ASTERIA_CHECK(runner, unit.PrepareDisabled() == VtdUnitError::Failed);
    VtdHandoffRecord record{};
    record.RegisterBase = 0xFED90000U;
    unit.CopyHandoffRecord(record);
    ASTERIA_EXPECT(runner, "refused TES excludes BootServices and retains raw bases with a typed reason",
                   record.RegisterBase == 0xFED90000U, record.Outcome == FirmwareHandoffOutcome::Refused,
                   record.Reason == static_cast<Foundation::UInt8>(VtdHandoffFailure::QueueError),
                   !record.AllowsBootServices(), record.RootAddress == 0x9000U, record.QueueAddress == 0x20002U);
    registers.Status = 0U;
    registers.RootAddress = 0U;
    VtdHandoffRecord copied{};
    unit.CopyHandoffRecord(copied);
    ASTERIA_CHECK(runner, copied.Status == record.Status && copied.RootAddress == record.RootAddress);
    record.Status &= ~(UInt32{1U} << 31U);
    Kernel::Memory::PhysicalPageImportExclusion exclusions[5U]{};
    ASTERIA_EXPECT(runner, "QIES with an empty queue admits BootServices minus queue/root extents",
                   record.AllowsBootServices(), record.Exclusions(exclusions) == 2U, exclusions[0U].Start == 0x20000U,
                   exclusions[0U].Bytes == 4U * 4096U, exclusions[1U].Start == 0x9000U, exclusions[1U].Bytes == 4096U);
    record.QueueTail = 16U;
    ASTERIA_CHECK(runner, !record.AllowsBootServices());
    record.QueueTail = 0U;
    record.Status |= UInt32{1U} << 25U;
    ASTERIA_CHECK(runner, !record.AllowsBootServices());
    record.Outcome = FirmwareHandoffOutcome::NotAddressed;
    record.Status = 0U;
    ASTERIA_CHECK(runner, !record.AllowsBootServices());
    record.Outcome = FirmwareHandoffOutcome::TakenOver;
    ASTERIA_CHECK(runner, record.AllowsBootServices() && record.Exclusions(exclusions) == 0U);
}

void TestFirmwareTakeover(TestContext& runner) noexcept
{
    static Tables tables;
    static Boot::BootInfo memory;
    memory = {};
    memory.MemoryRegionCount = 1U;
    memory.MemoryRegions[0U].PhysicalStart = 0x4000U;
    memory.MemoryRegions[0U].PageCount = 128U;
    memory.MemoryRegions[0U].Kind = Boot::PhysicalMemoryKind::BootServices;
    constexpr UInt32 qi{UInt32{1U} << 26U};
    constexpr UInt32 ir{UInt32{1U} << 25U};
    constexpr UInt32 cfis{UInt32{1U} << 23U};
    constexpr UInt32 afls{UInt32{1U} << 28U};
    // Rev. 4.0 sections 6.5.2/6.5.2.8, 11.4.4.1 and 11.4.7.2:
    // queue proof precedes all global commands; errors prohibit fetching.
    // Each randomized transaction is compared with the independently selected
    // failure/completion model, then a second call checks permanent failure
    // or fresh GSTS composition (never replay the initial QI/IR snapshot).
    const auto seeds = Asteria::TestSupport::GetSeedRange(173U, 8U);
    for (Size seedAt{0U}; seedAt < seeds.Count; ++seedAt)
    {
        Asteria::TestSupport::Random random{seeds.First + seedAt};
        for (Size step{0U}; step < 256U; ++step)
        {
            FakeVtd state;
            const UInt32 controls =
                (random.Chance(1U, 2U) ? TranslationEnable : 0U) | qi | (random.Chance(1U, 2U) ? ir : 0U) | afls | cfis;
            state.Status = controls | RootTablePointer | (UInt32{1U} << 24U);
            const bool wide = random.Chance(1U, 2U);
            const Size mode = random.Next() % 12U;
            state.QueueAddress = 0x4000U | (wide ? 0x800U : 0U);
            state.QueueTail = 4096U - (wide ? 32U : 16U);
            state.FaultEventControl = 0x4000'015AU;
            state.InvalidationEventControl = 0x4000'023CU;
            state.MemoryMap = &memory;
            VtdHandoffFailure failure{VtdHandoffFailure::None};
            if (mode == 0U || mode == 9U)
            {
                state.Previous[0U] = 0x25U;
                state.Previous[1U] = 0xABCDU;
                state.Previous[2U] = static_cast<UInt32>(state.ProofStatusAddress);
                state.ProofStatus = mode == 0U ? 0xABCDU : 0xABCEU;
                state.QueueFirmwareOwned = mode != 0U;
            }
            else if (mode == 2U)
            {
                state.Ownership.PoolStart = 0x4000U;
                state.Ownership.PoolPages = 1U;
                failure = VtdHandoffFailure::FirmwareMemory;
            }
            else if (mode == 3U)
            {
                state.StatusFirmwareOwned = false;
                failure = VtdHandoffFailure::FirmwareMemory;
            }
            else if (mode == 4U || mode == 5U)
            {
                state.FaultStatus = mode == 4U ? 0x10U : 0x40U;
                failure = mode == 4U ? VtdHandoffFailure::QueueError : VtdHandoffFailure::InvalidationTimeout;
            }
            else if (mode == 6U)
            {
                state.WaitStuck = true;
                failure = VtdHandoffFailure::WaitTimeout;
            }
            else if (mode == 7U)
            {
                state.QueueStuck = true;
                failure = VtdHandoffFailure::QueueTimeout;
            }
            else if (mode == 8U)
            {
                state.QueueTail = 4095U;
                failure = VtdHandoffFailure::MalformedQueue;
            }
            else if (mode == 10U)
            {
                state.QueueDenied = true;
                failure = VtdHandoffFailure::FirmwareMemory;
            }
            else if (mode == 11U)
            {
                state.QueueAddress |= 0x8U;
                failure = VtdHandoffFailure::MalformedQueue;
            }
            const bool success = failure == VtdHandoffFailure::None;
            const VtdUnitError expectedError =
                success ? VtdUnitError::None
                        : ((mode == 6U || mode == 7U) ? VtdUnitError::TimedOut : VtdUnitError::Failed);
            VtdUnit handoff{state, state};
            bool good = handoff.PrepareDisabled() == expectedError && handoff.GetHandoffFailure() == failure &&
                        state.Refusal == failure && state.Reports == 1U && state.ReportBeforeWrites &&
                        state.Observed.Status == (controls | RootTablePointer | (UInt32{1U} << 24U)) &&
                        state.Observed.FaultStatus == state.FaultStatus &&
                        state.Observed.QueueAddress == state.QueueAddress && state.FaultEventControl == 0xC000'015AU &&
                        state.InvalidationEventControl == 0xC000'023CU && state.EventOffsets[0U] == 0x38U &&
                        state.EventOffsets[1U] == 0xA0U;
            UInt32 expected = controls;
            for (Size event{0U}; event < state.Events; ++event)
            {
                if (state.EventOffsets[event] != 0x18U)
                {
                    continue;
                }
                const UInt32 after = static_cast<UInt32>(state.EventValues[event]);
                const UInt32 changed = expected ^ after;
                good =
                    good && success && changed != 0U && (changed & (changed - 1U)) == 0U && (after & ~controls) == 0U;
                expected = after;
            }
            if (success)
            {
                good =
                    good && expected == cfis && !handoff.GetSnapshot().Failed &&
                    (mode == 0U ? state.QueueWrites == 0U && !state.Posted : state.WaitValid && state.QueueTail == 0U);
                good = good && Configure(handoff, tables) == VtdUnitError::None && state.Reports == 1U &&
                       (state.Status & (qi | ir)) == 0U;
            }
            else
            {
                const Size events = state.Events;
                good = good && handoff.GetSnapshot().Failed && state.Status == state.Observed.Status &&
                       state.RootAddress == 0U && state.Commands == 0U && state.Refusals == 1U &&
                       handoff.PrepareDisabled() == VtdUnitError::Failed &&
                       Configure(handoff, tables) == VtdUnitError::Failed && state.Events == events;
                if (mode == 4U || mode == 5U)
                {
                    good = good && state.QueueReads == 2U && state.QueueWrites == 0U;
                }
                if (mode != 6U)
                {
                    good = good && state.QueueWrites == 0U && !state.Posted;
                }
            }
            if (!ASTERIA_EXPECT(runner, "firmware takeover transaction model", good))
            {
                static_cast<void>(std::fprintf(stderr, "seed=%llu step=%zu mode=%zu width=%u reason=%u\n",
                                               static_cast<unsigned long long>(seeds.First + seedAt), step, mode,
                                               wide ? 32U : 16U, static_cast<unsigned>(handoff.GetHandoffFailure())));
                return;
            }
        }
    }
    FakeVtd pmr;
    pmr.Capability |= UInt64{1U} << 5U;
    pmr.PmrStuck = true;
    pmr.ProtectedMemory = 0x8000'0001U;
    VtdUnit pmrUnit{pmr, pmr};
    ASTERIA_EXPECT(runner, "a PMR disable timeout cannot proceed to translation",
                   Configure(pmrUnit, tables) == VtdUnitError::TimedOut, !pmrUnit.IsEnabled(),
                   pmrUnit.GetHandoffFailure() == VtdHandoffFailure::ProtectedMemoryTimeout);
    FakeVtd te;
    te.Status = TranslationEnable;
    te.HandoffStuck = true;
    VtdUnit teUnit{te, te};
    ASTERIA_EXPECT(runner, "a firmware TES disable timeout is diagnosed without installing new tables",
                   Configure(teUnit, tables) == VtdUnitError::TimedOut, te.RootAddress == 0U,
                   teUnit.GetSnapshot().HandoffStatus == TranslationEnable,
                   teUnit.GetHandoffFailure() == VtdHandoffFailure::CommandTimeout);
}

void TestWriteBufferOrdering(TestContext& runner) noexcept
{
    static Tables tables;
    FakeVtd registers;
    registers.Capability |= UInt64{1U} << 4U;
    VtdUnit unit{registers, registers};
    ASTERIA_EXPECT(runner, "the takeover masks fault events before any command (VT-d 4.0 section 11.4.7.2)",
                   Configure(unit, tables) == VtdUnitError::None, registers.Events == 1U,
                   registers.EventOffsets[0U] == 0x38U, (registers.EventValues[0U] & (UInt64{1U} << 31U)) != 0U,
                   registers.Commands == 0U);
    registers.Events = 0U;
    ASTERIA_EXPECT(runner, "RWBF flush precedes root-pointer writes and the global context invalidation",
                   unit.Enable() == VtdUnitError::None, registers.EventOffsets[0U] == 0x38U,
                   registers.EventOffsets[1U] == 0x18U, registers.EventValues[1U] == (UInt64{1U} << 27U),
                   registers.EventOffsets[2U] == 0x20U, registers.EventOffsets[4U] == 0x18U,
                   registers.EventOffsets[5U] == 0x28U, registers.WriteFlushes == 2U);
    registers.Events = 0U;
    ASTERIA_EXPECT(runner, "each context attachment and detachment flushes writes before invalidating",
                   unit.AttachContext(0x0010U, 1U, tables.Page(5U).PhysicalAddress) == VtdUnitError::None,
                   unit.DetachContext(0x0010U, 1U) == VtdUnitError::None, registers.EventOffsets[0U] == 0x18U,
                   registers.EventOffsets[1U] == 0x28U, registers.EventOffsets[2U] == 0x18U,
                   registers.EventOffsets[3U] == 0x28U, registers.WriteFlushes == 4U);
    FakeVtd stuck;
    stuck.Capability |= UInt64{1U} << 4U;
    stuck.FlushStuck = true;
    VtdUnit stuckUnit{stuck, stuck};
    ASTERIA_EXPECT(runner, "a write-buffer timeout refuses enable before replacing the root pointer",
                   Configure(stuckUnit, tables) == VtdUnitError::None, stuckUnit.Enable() == VtdUnitError::TimedOut,
                   stuck.RootAddress == 0U, !stuckUnit.IsEnabled());
}

void TestCapabilityBounds(TestContext& runner) noexcept
{
    static Tables tables;
    const VtdTablePage contexts[VtdUnit::MaximumBuses]{tables.Page(1U), tables.Page(2U), tables.Page(3U),
                                                       tables.Page(4U)};
    for (Size layout{0U}; layout < 5U; ++layout)
    {
        FakeVtd registers;
        // FRO/NFR give faultOffset + (NFR + 1) * 16; IRO covers two qwords.
        const UInt64 faultOffset{layout == 0U ? 0xFFU : 0x3FFU};
        const UInt64 records{layout == 0U ? 0U : 0xFFU};
        registers.Capability = (QemuCapability & ~((UInt64{0x3FFU} << 24U) | (UInt64{0xFFU} << 40U))) |
                               (faultOffset << 24U) | (records << 40U);
        registers.ExtendedCapability = (layout == 0U ? UInt64{0xFFU} : UInt64{0x3FFU}) << 8U;
        VtdUnit sized{registers, registers};
        const Size bytes{layout == 0U || layout == 1U ? 4096U : (layout == 2U ? 16384U : 65536U)};
        const auto result = sized.Configure(tables.Page(0U), contexts, nullptr, 0U, layout == 4U ? 65535U : bytes);
        const VtdUnitError expected{layout == 0U || layout == 3U
                                        ? VtdUnitError::None
                                        : (layout == 4U ? VtdUnitError::InvalidArguments : VtdUnitError::Unsupported)};
        ASTERIA_EXPECT(runner, "capability extents fit the declared register set", result == expected,
                       !sized.IsEnabled());
    }
    FakeVtd caching;
    caching.Capability |= UInt64{1U} << 7U;
    VtdUnit cachingUnit{caching, caching};
    ASTERIA_EXPECT(runner, "caching-mode units require an unsupported invalidation policy",
                   Configure(cachingUnit, tables) == VtdUnitError::Unsupported, !cachingUnit.IsEnabled());
    FakeVtd narrow;
    narrow.Capability = (QemuCapability & ~(UInt64{0x3FU} << 16U)) | (UInt64{31U} << 16U);
    VtdUnit unit{narrow, narrow};
    const bool ready = (Configure(unit, tables) == VtdUnitError::None) && (unit.Enable() == VtdUnitError::None);
    const VtdTablePage pages[VtdDomainBackend::TablePages]{tables.Page(5U),  tables.Page(6U), tables.Page(7U),
                                                           tables.Page(8U),  tables.Page(9U), tables.Page(10U),
                                                           tables.Page(11U), tables.Page(12U)};
    VtdDomainBackend backend{unit, 1U, pages};
    SpinLock synchronization;
    Asteria::TestSupport::TestObjectRegistry registry{synchronization};
    const IommuDomain owner{registry, backend};
    ASTERIA_EXPECT(runner, "MGAW constrains domains even when SAGAW supports a wider walk", ready,
                   unit.GetAddressBits() == 32U,
                   backend.Claim(owner, 65U) == IommuBackendOwnershipError::UnsupportedConfiguration,
                   backend.Claim(owner, 64U) == IommuBackendOwnershipError::None,
                   backend.Map(MakeRequest(0x1'0000'0000U, 0x7000'0000U, 1U, DmaPermissions{DmaPermission::Read})) ==
                       IommuBackendError::InvalidRequest,
                   backend.Release(owner) == IommuBackendOwnershipError::None);
    using Platform::Device::VtdDomainGroup;
    VtdDomainBackend* backends[VtdDomainGroup::MaximumUnits]{&backend};
    VtdUnit* units[VtdDomainGroup::MaximumUnits]{&unit};
    VtdDomainGroup group{backends, units, 1U};
    ASTERIA_EXPECT(runner, "the group preflight also applies MGAW",
                   group.Claim(owner, 65U) == IommuBackendOwnershipError::UnsupportedConfiguration,
                   group.Claim(owner, 64U) == IommuBackendOwnershipError::None,
                   group.Release(owner) == IommuBackendOwnershipError::None);
}

void TestFirmwareDomain(TestContext& runner) noexcept
{
    static Tables tables;
    FakeVtd registers;
    VtdUnit unit{registers, registers};
    const bool configured = Configure(unit, tables) == VtdUnitError::None;
    const VtdTablePage pages[VtdDomainBackend::TablePages]{tables.Page(5U),  tables.Page(6U), tables.Page(7U),
                                                           tables.Page(8U),  tables.Page(9U), tables.Page(10U),
                                                           tables.Page(11U), tables.Page(12U)};
    VtdDomainBackend firmware{unit, 5U, pages};
    SpinLock synchronization;
    Asteria::TestSupport::TestObjectRegistry registry{synchronization};
    const IommuDomain owner{registry, firmware};
    ASTERIA_EXPECT(runner, "firmware identity mappings and present context exist before TE", configured,
                   firmware.PrepareReserved(), !unit.IsEnabled(),
                   firmware.MapReserved(0x000E'0000U, 0x000E'1FFFU) == IommuBackendError::None,
                   firmware.SeedReserved(0x0010U) == VtdUnitError::None,
                   tables.Pages[1U][2U * 0x10U] == (tables.Page(5U).PhysicalAddress | 1U),
                   tables.Pages[1U][2U * 0x10U + 1U] == ((UInt64{5U} << 8U) | 1U),
                   tables.Pages[7U][0xE0U] == 0x000E'0003U, tables.Pages[7U][0xE1U] == 0x000E'1003U,
                   tables.Pages[7U][0xDFU] == 0U, tables.Pages[7U][0xE2U] == 0U, tables.Pages[1U][2U * 0xF8U] == 0U,
                   (registers.Status & TranslationEnable) == 0U,
                   firmware.Claim(owner, 39U) == IommuBackendOwnershipError::AlreadyOwned);
    ASTERIA_EXPECT(runner, "overlapping RMRRs retain only identity pages and reject malformed ranges",
                   firmware.MapReserved(0x000E'1000U, 0x000E'2FFFU) == IommuBackendError::None,
                   tables.Pages[7U][0xE2U] == 0x000E'2003U,
                   firmware.MapReserved(0x000E'0001U, 0x000E'1FFFU) == IommuBackendError::InvalidRequest,
                   firmware.MapReserved(0x000E'0000U, 0x000D'FFFFU) == IommuBackendError::InvalidRequest,
                   firmware.MapReserved(0U, 0x0200'0FFFU) == IommuBackendError::InvalidRequest,
                   unit.Enable() == VtdUnitError::None, unit.GetSnapshot().Attached == 1U,
                   firmware.SeedReserved(0x00F8U) == VtdUnitError::NotConfigured);
}

void TestLargeGraphicsRegion(TestContext& runner) noexcept
{
    static Tables unitTables;
    alignas(4096) static UInt64 graphicsTables[VtdDomainBackend::MaximumTablePages][512U]{};
    static VtdTablePage pages[VtdDomainBackend::MaximumTablePages]{};
    for (Size index{0U}; index < VtdDomainBackend::MaximumTablePages; ++index)
    {
        pages[index] = {.Entries = graphicsTables[index],
                        .PhysicalAddress =
                            static_cast<UInt64>(reinterpret_cast<Foundation::UIntPointer>(graphicsTables[index]))};
    }
    FakeVtd registers;
    VtdUnit unit{registers, registers};
    const bool configured = Configure(unit, unitTables) == VtdUnitError::None;
    VtdDomainBackend graphics{unit, 5U, pages, VtdDomainBackend::MaximumTablePages};
    constexpr UInt64 base{0x7D00'0000U};
    constexpr UInt64 limit{0x811F'FFFFU};
    ASTERIA_EXPECT(runner, "a shared-unit graphics RMRR of 66 MiB is identity RW before translation starts", configured,
                   graphics.PrepareReserved(), graphics.MapReserved(base, limit) == IommuBackendError::None,
                   graphics.GetUsedPages() == 36U, graphics.SeedReserved(0x0010U) == VtdUnitError::None,
                   !unit.IsEnabled(), unitTables.Pages[1U][0x20U] == (pages[0U].PhysicalAddress | 1U));
    // Walk both ends, including the 2 GiB boundary: every leaf is 4 KiB RW,
    // with no superpage capability required on Sandy Bridge.
    constexpr UInt64 ends[]{base, limit - 4095U};
    for (const UInt64 address : ends)
    {
        const auto pdAddress = graphicsTables[0U][(address >> 30U) & 511U] & ~UInt64{0xFFFU};
        const auto* const pd = reinterpret_cast<const UInt64*>(static_cast<Foundation::UIntPointer>(pdAddress));
        const auto ptAddress = pd[(address >> 21U) & 511U] & ~UInt64{0xFFFU};
        const auto* const pt = reinterpret_cast<const UInt64*>(static_cast<Foundation::UIntPointer>(ptAddress));
        ASTERIA_EXPECT(runner, "both ends of the large graphics range retain identity read/write leaves",
                       pt[(address >> 12U) & 511U] == (address | 3U));
    }
    const Size used = graphics.GetUsedPages();
    ASTERIA_EXPECT(runner, "firmware mappings still obey the table-capacity bound before any writes",
                   graphics.MapReserved(0x1000'0000U, 0x2FFF'FFFFU) == IommuBackendError::InvalidRequest,
                   graphics.GetUsedPages() == used, unit.Enable() == VtdUnitError::None,
                   unit.GetSnapshot().Attached == 1U);
}

void TestMultipleUnits(TestContext& runner) noexcept
{
    using Platform::Device::VtdDomainGroup;
    static Tables first;
    static Tables second;
    FakeVtd firstRegisters;
    FakeVtd secondRegisters;
    // Exercise independent SAGAW choices as well as independent root tables.
    secondRegisters.Capability = (QemuCapability & ~(UInt64{2U} << 8U));
    VtdUnit firstUnit{firstRegisters, firstRegisters};
    VtdUnit secondUnit{secondRegisters, secondRegisters};
    const VtdTablePage contexts[VtdUnit::MaximumBuses]{second.Page(1U), second.Page(2U), second.Page(3U),
                                                       second.Page(4U)};
    constexpr UInt16 requesters[]{0x0018U};
    const bool ready = (Configure(firstUnit, first) == VtdUnitError::None) &&
                       (secondUnit.Configure(second.Page(0U), contexts, requesters, 1U) == VtdUnitError::None) &&
                       (firstUnit.Enable() == VtdUnitError::None) && (secondUnit.Enable() == VtdUnitError::None);
    const VtdTablePage firstPages[VtdDomainBackend::TablePages]{first.Page(5U),  first.Page(6U), first.Page(7U),
                                                                first.Page(8U),  first.Page(9U), first.Page(10U),
                                                                first.Page(11U), first.Page(12U)};
    const VtdTablePage secondPages[VtdDomainBackend::TablePages]{second.Page(5U),  second.Page(6U), second.Page(7U),
                                                                 second.Page(8U),  second.Page(9U), second.Page(10U),
                                                                 second.Page(11U), second.Page(12U)};
    VtdDomainBackend firstBackend{firstUnit, 1U, firstPages};
    VtdDomainBackend secondBackend{secondUnit, 1U, secondPages};
    VtdDomainBackend* backends[VtdDomainGroup::MaximumUnits]{&firstBackend, &secondBackend};
    VtdUnit* units[VtdDomainGroup::MaximumUnits]{&firstUnit, &secondUnit};
    VtdDomainGroup group{backends, units, 2U};
    SpinLock synchronization;
    Asteria::TestSupport::TestObjectRegistry registry{synchronization};
    const IommuDomain owner{registry, group};
    const IommuDomain other{registry, group};
    FakeRequester firstRequester{0x0010U};
    FakeRequester secondRequester{0x0018U};
    ASTERIA_EXPECT(runner, "a domain routes requesters across independent units with different table depths", ready,
                   group.IsReady(), group.Claim(owner, 65U) == IommuBackendOwnershipError::UnsupportedConfiguration,
                   group.Claim(owner, 32U) == IommuBackendOwnershipError::None,
                   group.Claim(other, 32U) == IommuBackendOwnershipError::AlreadyOwned,
                   group.Map(MakeRequest(0x0010'0000U, 0x7000'0000U, 1U, DmaPermissions{DmaPermission::Read})) ==
                       IommuBackendError::None,
                   firstBackend.GetUsedPages() == 3U, secondBackend.GetUsedPages() == 4U,
                   first.Pages[7U][0x100U] == 0x7000'0001U, second.Pages[8U][0x100U] == 0x7000'0001U,
                   group.Attach(firstRequester) == IommuBackendError::None,
                   group.Attach(secondRequester) == IommuBackendError::None,
                   first.Pages[1U][2U * 0x10U] == (first.Page(5U).PhysicalAddress | 1U),
                   second.Pages[1U][2U * 0x18U] == (second.Page(5U).PhysicalAddress | 1U),
                   second.Pages[1U][2U * 0x18U + 1U] == ((UInt64{1U} << 8U) | 2U));
    firstRegisters.InjectFault(0x0010U, 5U, 0x0020'0123U, false);
    const auto firstFault = group.GetFaults();
    ASTERIA_EXPECT(runner, "a group's fault tuple survives a later unit with no faults", firstFault.Count == 1U,
                   firstFault.LastSource == 0x0010U, firstFault.LastReason == 5U,
                   firstFault.LastAddress == 0x0020'0000U);
    secondRegisters.InjectFault(0x0018U, 6U, 0x0030'0456U, true);
    const auto combinedFaults = group.GetFaults();
    const auto repeatedFaults = group.GetFaults();
    ASTERIA_EXPECT(runner, "group fault counts sum and source, reason and address come from the same unit",
                   combinedFaults.Count == 2U, combinedFaults.LastSource == 0x0018U, combinedFaults.LastReason == 6U,
                   combinedFaults.LastAddress == 0x0030'0000U, repeatedFaults.Count == combinedFaults.Count,
                   repeatedFaults.LastSource == combinedFaults.LastSource,
                   repeatedFaults.LastReason == combinedFaults.LastReason,
                   repeatedFaults.LastAddress == combinedFaults.LastAddress);
    ASTERIA_EXPECT(runner, "both units detach and release their tables after fault polling",
                   group.Release(other) == IommuBackendOwnershipError::InvalidOwner,
                   group.Unmap(MakeRequest(0x0010'0000U, 0x7000'0000U, 1U, DmaPermissions{DmaPermission::Read})) ==
                       IommuBackendError::None,
                   first.Pages[7U][0x100U] == 0U, second.Pages[8U][0x100U] == 0U,
                   group.Release(owner) == IommuBackendOwnershipError::None, first.Pages[1U][2U * 0x10U] == 0U,
                   second.Pages[1U][2U * 0x18U] == 0U, firstBackend.GetUsedPages() == 0U,
                   secondBackend.GetUsedPages() == 0U);
    // A failure in a later Claim must roll back earlier software ownership
    // without issuing a hardware invalidation or losing a pin.
    ASTERIA_EXPECT(runner, "a later busy backend rolls back the earlier claim",
                   secondBackend.Claim(other, 32U) == IommuBackendOwnershipError::None,
                   group.Claim(owner, 32U) == IommuBackendOwnershipError::AlreadyOwned,
                   firstBackend.GetUsedPages() == 0U, secondBackend.GetUsedPages() == 1U,
                   secondBackend.Release(other) == IommuBackendOwnershipError::None,
                   group.Claim(owner, 64U) == IommuBackendOwnershipError::None,
                   group.Release(owner) == IommuBackendOwnershipError::None);
    bool filled = group.Claim(owner, 32U) == IommuBackendOwnershipError::None;
    for (UInt64 window{0U}; window < 5U; ++window)
    {
        filled = filled && (group.Map(MakeRequest(window * 0x0020'0000U, 0x7000'0000U, 1U,
                                                  DmaPermissions{DmaPermission::Read})) == IommuBackendError::None);
    }
    ASTERIA_EXPECT(runner, "a later unit's table capacity is checked before writing the earlier unit", filled,
                   firstBackend.GetUsedPages() == 7U, secondBackend.GetUsedPages() == 8U,
                   group.Map(MakeRequest(5U * 0x0020'0000U, 0x7000'0000U, 1U, DmaPermissions{DmaPermission::Read})) ==
                       IommuBackendError::CapacityExceeded,
                   firstBackend.GetUsedPages() == 7U, secondBackend.GetUsedPages() == 8U,
                   group.Release(owner) == IommuBackendOwnershipError::None);
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"VtdUnitTests"};
    Asteria::Test::TestEnable(runner);
    Asteria::Test::TestContexts(runner);
    Asteria::Test::TestFaults(runner);
    Asteria::Test::TestBackend(runner);
    Asteria::Test::TestDisplayObservation(runner);
    Asteria::Test::TestFirmwareTakeover(runner);
    Asteria::Test::TestImportHandoffRecords(runner);
    Asteria::Test::TestWriteBufferOrdering(runner);
    Asteria::Test::TestCapabilityBounds(runner);
    Asteria::Test::TestFirmwareDomain(runner);
    Asteria::Test::TestLargeGraphicsRegion(runner);
    Asteria::Test::TestMultipleUnits(runner);
    return runner.Finish();
}
