// SPDX-License-Identifier: Apache-2.0
#include "Platform/Amd64/Device/AmdViDomainBackend.hpp"
#include "Kernel/Device/IommuDomain.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"

#include "Support/ObjectRegistryFixture.hpp"
#include "Support/Random.hpp"
#include "Support/TestContext.hpp"

namespace
{
using Asteria::Foundation::UInt16;
using Asteria::Foundation::UInt32;
using Asteria::Foundation::UInt64;
using Asteria::Foundation::Size;
namespace Native = Asteria::Platform::Amd64::Device;
namespace Hal = Asteria::Hal::Device;
namespace Device = Asteria::Kernel::Device;
namespace Memory = Asteria::Kernel::Memory;
constexpr UInt64 AddressMask{0x000F'FFFF'FFFF'F000ULL};

struct Tables
{
    alignas(4096) UInt64 Pages[528U][512U]{};
    [[nodiscard]] Native::VtdTablePage Page(const Size index) noexcept
    {
        return {Pages[index], 0x1000U + index * 4096U};
    }
};

/// Independent wire consumer: parses command opcodes, rings and page walks.
/// DMA caches both present and absent pages until a domain invalidation.
class Model final : public Native::IVtdRegisters, public Native::IVtdCacheMaintenance
{
public:
    explicit Model(Tables& tables) noexcept : m_tables{tables}
    {
    }
    void Reset() noexcept
    {
        for (UInt64& value : Registers)
        {
            value = 0U;
        }
        for (UInt64& value : Commands)
        {
            value = 0U;
        }
        for (bool& value : Cached)
        {
            value = false;
        }
        for (UInt64& value : Translation)
        {
            value = 0U;
        }
        Flushes = 0U;
        Stuck = false;
        EventRunStuck = false;
        EventRunPolls = 0U;
        RestartPhase = 0U;
        Valid = true;
    }
    [[nodiscard]] UInt32 Read32(const Size offset) const noexcept override
    {
        return static_cast<UInt32>(Read64(offset));
    }
    // AMD-Vi has no firmware invalidation queue to settle at handoff.
    [[nodiscard]] bool ReadQueueWord(UInt64, UInt32&) const noexcept override
    {
        return false;
    }
    [[nodiscard]] bool WriteQueueWord(UInt64, UInt32) noexcept override
    {
        return false;
    }
    [[nodiscard]] bool CanWriteFirmwareMemory(UInt64, UInt64) const noexcept override
    {
        return false;
    }
    void ReportHandoff(const Asteria::Platform::Device::VtdFirmwareState&) noexcept override
    {
    }
    void ReportRefusal(Asteria::Platform::Device::VtdHandoffFailure) noexcept override
    {
    }
    [[nodiscard]] UInt64 Read64(const Size offset) const noexcept override
    {
        if ((offset == 0x2020U) && ((Registers[offset / 8U] & 1U) != 0U) && !EventRunStuck)
        {
            if (EventRunPolls == 0U)
            {
                Registers[offset / 8U] &= ~UInt64{8U};
            }
            else
            {
                --EventRunPolls;
            }
        }
        return Registers[offset / 8U];
    }
    void Write32(const Size offset, const UInt32 value) noexcept override
    {
        Write64(offset, value);
    }
    void Write64(const Size offset, const UInt64 value) noexcept override
    {
        if (offset == 0x2020U)
        {
            // AMD 48882 status fields are RW1C or RO, never ordinary RAM.
            if (RestartPhase == 1U)
            {
                Valid = Valid && value == 1U && (Registers[0x18U / 8U] & 4U) == 0U;
                RestartPhase = 2U;
            }
            Registers[offset / 8U] &= ~(value & 7U);
            return;
        }
        if (offset == 0x18U)
        {
            const bool wasEnabled = (Registers[offset / 8U] & 4U) != 0U;
            const bool enabled = (value & 4U) != 0U;
            if (wasEnabled && !enabled && (Registers[0x2020U / 8U] & 1U) != 0U)
            {
                Valid = Valid && (Registers[0x2020U / 8U] & 8U) == 0U;
                RestartPhase = 1U;
            }
            if (!wasEnabled && enabled)
            {
                if (RestartPhase == 2U)
                {
                    RestartPhase = 3U;
                }
                Registers[0x2020U / 8U] |= 8U;
            }
        }
        Registers[offset / 8U] = value;
        if ((offset != 0x2008U) || Stuck)
        {
            return;
        }
        UInt64 head = Registers[0x2000U / 8U];
        while (head != value)
        {
            const UInt64 low = m_tables.Pages[CommandPage + head / 4096U][(head % 4096U) / 8U];
            const UInt64 high = m_tables.Pages[CommandPage + head / 4096U][(head % 4096U) / 8U + 1U];
            const UInt64 type = low >> 60U;
            ++Commands[type];
            if (type == 1U)
            {
                Valid = Valid && ((low & 7U) == 1U) && ((low & AddressMask) == (CompletionPage + 1U) * 4096U);
                m_tables.Pages[CompletionPage][0U] = high;
            }
            else if (type == 3U)
            {
                Valid = Valid && ((low & 0xFFFFFFFFU) == 0U) && (high == 0x7FFFFFFFFFFFF003ULL);
                for (bool& cached : Cached)
                {
                    cached = false;
                }
            }
            else if (type == 8U)
            {
                Valid = Valid && ((low & 0x0FFFFFFFFFFFFFFFULL) == 0U) && (high == 0U);
                for (bool& cached : Cached)
                {
                    cached = false;
                }
            }
            else
            {
                Valid = Valid && ((type == 2U) || (type == 5U)) && (high == 0U);
            }
            head = (head + 16U) % 8192U;
        }
        Registers[0x2000U / 8U] = head;
    }
    void Flush(const volatile void*) noexcept override
    {
        ++Flushes;
    }
    // AMD-Vi flushes single entries only.
    void FlushTables(const volatile void*, const Size) noexcept override
    {
    }
    void Fence() noexcept override
    {
    }

    [[nodiscard]] UInt64 Translate(const Size page, const bool write) noexcept
    {
        if ((Registers[0x18U / 8U] & 1U) == 0U)
        {
            return page * 4096U;
        }
        const UInt64 dte = m_tables.Pages[0U][0x10U * 4U];
        if ((dte & 3U) != 3U)
        {
            return 0U;
        }
        const UInt64 permission = UInt64{1U} << (write ? 62U : 61U);
        if (Cached[page])
        {
            return ((Translation[page] & permission) != 0U) ? (Translation[page] & AddressMask) : 0U;
        }
        UInt64 entry = dte;
        for (Size level{static_cast<Size>((dte >> 9U) & 7U)}; level > 0U; --level)
        {
            if ((entry & permission) == 0U || (entry & 1U) == 0U)
            {
                entry = 0U;
                break;
            }
            const UInt64 root = entry & AddressMask;
            if ((root < 0x1000U) || (root >= 0x11000U))
            {
                entry = 0U;
                break;
            }
            const Size index = static_cast<Size>((page * 4096U >> (12U + (level - 1U) * 9U)) & 511U);
            entry = m_tables.Pages[(root - 0x1000U) / 4096U][index];
            if ((level == 1U) && (entry != 0U))
            {
                Valid = Valid && ((entry & (UInt64{1U} << 60U)) != 0U);
            }
            Valid = Valid && (((entry >> 9U) & 7U) == (level - 1U) || entry == 0U);
        }
        Translation[page] = ((entry & 1U) != 0U) ? entry : 0U;
        Cached[page] = true;
        return ((entry & 1U) != 0U && (entry & permission) != 0U) ? (entry & AddressMask) : 0U;
    }
    Size CommandPage{2U};
    Size CompletionPage{6U};
    // Include optional head/tail registers through 2078h (AMD IOMMU
    // Specification 48882 rev. 3.09 sections 3.4.17–20).
    mutable UInt64 Registers[0x2080U / sizeof(UInt64)]{};
    UInt64 Commands[16U]{};
    UInt64 Flushes{0U};
    bool Stuck{false};
    bool EventRunStuck{false};
    mutable Size EventRunPolls{0U};
    Size RestartPhase{0U};
    bool Valid{true};
    bool Cached[32U]{};
    UInt64 Translation[32U]{};

private:
    Tables& m_tables;
};

class Endpoint final : public Hal::IIommuDevice
{
public:
    [[nodiscard]] Hal::IommuDeviceError Claim(const Device::IommuDomain&) noexcept override
    {
        return Hal::IommuDeviceError::None;
    }
    [[nodiscard]] Hal::IommuDeviceError Release(const Device::IommuDomain&) noexcept override
    {
        return Hal::IommuDeviceError::None;
    }
    [[nodiscard]] bool HasTrustedDma() const noexcept override
    {
        return false;
    }
    [[nodiscard]] UInt32 GetRequesterId() const noexcept override
    {
        return 0x10U;
    }
};

[[nodiscard]] Native::VtdUnitError Configure(Native::AmdViUnit& unit, Tables& tables) noexcept
{
    constexpr UInt16 requesters[]{0x10U};
    return unit.Configure(tables.Page(0U), 2U, tables.Page(2U), tables.Page(4U), tables.Page(6U), requesters, 1U);
}

void RunModel(Asteria::TestSupport::TestContext& context, const UInt64 seed)
{
    static Tables tables;
    static Asteria::Platform::Synchronization::SpinLock lock;
    static Asteria::TestSupport::TestObjectRegistry registry{lock, 1U};
    tables = {};
    static Model model{tables};
    model.Reset();
    Native::AmdViUnit unit{model, model};
    model.Registers[0x18U / 8U] = 0x11005U;
    ASTERIA_EXPECT(context, "firmware translation is disabled before kernel tables are installed",
                   Configure(unit, tables) == Native::VtdUnitError::None, model.Registers[0x18U / 8U] == 0U,
                   unit.GetSnapshot().HandoffStatus == 0x11005U, tables.Pages[0U][0x10U * 4U] == 3U,
                   tables.Pages[0U][0x10U * 4U + 2U] == (UInt64{1U} << 60U), !unit.Serves(0x30U),
                   unit.Enable() == Native::VtdUnitError::None, model.Registers[0U] == 0x1001U,
                   model.Registers[8U / 8U] == (0x3000U | (UInt64{9U} << 56U)), model.Commands[2U] == 256U,
                   model.Commands[5U] == 256U, model.Commands[3U] == 13U, model.Valid);
    Native::VtdTablePage pages[8U]{};
    for (Size index{0U}; index < 8U; ++index)
    {
        pages[index] = tables.Page(index + 8U);
    }
    Native::AmdViDomainBackend backend{unit, 1U, pages};
    const Device::IommuDomain owner{registry, backend};
    Endpoint endpoint;
    ASTERIA_EXPECT(context, "claim installs a root before attach and rejects excessive input width",
                   backend.Claim(owner, 65U) == Hal::IommuBackendOwnershipError::UnsupportedConfiguration,
                   backend.Claim(owner, 32U) == Hal::IommuBackendOwnershipError::None,
                   backend.Attach(endpoint) == Hal::IommuBackendError::None);
    UInt64 reference[32U]{};
    Asteria::TestSupport::Random random{seed};
    for (Size step{0U}; step < 512U; ++step)
    {
        const Size page = static_cast<Size>(random.Below(32U));
        const Hal::IommuBackendMapRequest request{
            .DmaStart = Device::DmaAddress{page * 4096U},
            .PhysicalStart = Memory::PhysicalAddress{0x100000U + page * 4096U},
            .Pages = Memory::PageCount{1U},
            .Permissions = Device::DmaPermissions{Device::DmaPermission::Read}.With(Device::DmaPermission::Write),
            .Reserved = {}};
        const Hal::IommuBackendError result = (reference[page] == 0U) ? backend.Map(request) : backend.Unmap(request);
        reference[page] = (reference[page] == 0U) ? request.PhysicalStart.GetValue() : 0U;
        if (!ASTERIA_EXPECT(context, "mapping changes invalidate even cached non-present pages",
                            result == Hal::IommuBackendError::None, model.Translate(page, true) == reference[page],
                            backend.GetUsedPages() == 3U, model.Valid))
        {
            static_cast<void>(std::fprintf(stderr, "seed=%llu step=%zu map/unmap page=%zu\n",
                                           static_cast<unsigned long long>(seed), step, page));
            return;
        }
    }
    Hal::IommuBackendMapRequest oversized{.DmaStart = Device::DmaAddress{0x200000U},
                                          .PhysicalStart = Memory::PhysicalAddress{0x100000U},
                                          .Pages = Memory::PageCount{4096U},
                                          .Permissions = Device::DmaPermissions{Device::DmaPermission::Read},
                                          .Reserved = {}};
    ASTERIA_EXPECT(context, "capacity and out-of-range physical addresses fail without reserving more tables",
                   backend.Map(oversized) == Hal::IommuBackendError::CapacityExceeded, backend.GetUsedPages() == 3U);
    oversized.Pages = Memory::PageCount{1U};
    oversized.PhysicalStart = Memory::PhysicalAddress{UInt64{1U} << 52U};
    ASTERIA_EXPECT(context, "AMD v1 PTE address bits cannot silently truncate a physical address",
                   backend.Map(oversized) == Hal::IommuBackendError::InvalidRequest, backend.GetUsedPages() == 3U);
    // QEMU emits no domain field; the served requester supplies ownership.
    tables.Pages[4U][0U] = 0x10U | (UInt64{2U} << 60U) | (UInt64{1U} << 53U);
    tables.Pages[4U][1U] = 0x200000U;
    model.Registers[0x2018U / 8U] = 16U;
    const auto faults = backend.GetFaults();
    ASTERIA_EXPECT(
        context, "IO_PAGE_FAULT is consumed and attributed to the attached domain", faults.Count == 1U,
        faults.LastSource == 0x10U, faults.LastReason == 2U, unit.GetSnapshot().LastFault.Address == 0x200000U,
        !unit.GetSnapshot().LastFault.Read, model.Registers[0x2010U / 8U] == 16U, backend.GetFaults().Count == 1U,
        tables.Pages[4U][0U] == (0x10U | (UInt64{2U} << 60U) | (UInt64{1U} << 53U)), tables.Pages[4U][1U] == 0x200000U);
    ASTERIA_EXPECT(context, "release reblocks the requester and scrubs its bounded table reservation",
                   backend.Release(owner) == Hal::IommuBackendOwnershipError::None, model.Translate(0U, true) == 0U,
                   tables.Pages[0U][0x10U * 4U] == 3U, tables.Pages[8U][0U] == 0U, backend.GetUsedPages() == 0U);
}
} // namespace

void ReservedAndFailed(Asteria::TestSupport::TestContext& context)
{
    static Tables tables;
    static Model model{tables};
    Native::AmdViUnit unit{model, model};
    ASTERIA_EXPECT(context, "reserved firmware domains prepare before enable",
                   Configure(unit, tables) == Native::VtdUnitError::None);
    Native::VtdTablePage pages[8U]{};
    for (Size index{0U}; index < 8U; ++index)
    {
        pages[index] = tables.Page(index + 8U);
    }
    Native::AmdViDomainBackend reserved{unit, 5U, pages};
    ASTERIA_EXPECT(context, "IVMD identity domains keep device read-only permission", reserved.PrepareReserved(),
                   reserved.MapReserved(0x10000U, 0x10FFFU, Device::DmaPermissions{Device::DmaPermission::Read}) ==
                       Hal::IommuBackendError::None,
                   reserved.SeedReserved(0x10U) == Native::VtdUnitError::None,
                   unit.Enable() == Native::VtdUnitError::None, model.Translate(16U, false) == 0x10000U,
                   model.Translate(16U, true) == 0U);
    static Asteria::Platform::Synchronization::SpinLock lock;
    static Asteria::TestSupport::TestObjectRegistry registry{lock, 2U};
    const Device::IommuDomain owner{registry, reserved};
    ASTERIA_EXPECT(context, "a permanent firmware domain cannot be claimed by user code",
                   reserved.Claim(owner, 32U) == Hal::IommuBackendOwnershipError::AlreadyOwned);
    model.Stuck = true;
    ASTERIA_EXPECT(context, "missing invalidation completion retains firmware tables and ownership",
                   unit.InvalidateDomain(5U) == Native::VtdUnitError::TimedOut, unit.GetSnapshot().Failed,
                   reserved.GetUsedPages() == 4U);
}

void ReviewBoundaries(Asteria::TestSupport::TestContext& context)
{
    static Tables tables;
    static Model model{tables};
    tables = {};
    model.Reset();
    model.Registers[0x18U / 8U] = (UInt64{3U} << 22U) | 1U;
    model.Registers[0x30U / 8U] = UInt64{1U} << 6U;
    model.CommandPage = 512U;
    model.CompletionPage = 516U;
    Native::AmdViUnit full{model, model};
    constexpr UInt16 requesters[]{0x10U};
    ASTERIA_EXPECT(context, "full IVRS range publishes all 64K blocking entries and uses IASup",
                   full.Configure(tables.Page(0U), 512U, tables.Page(512U), tables.Page(514U), tables.Page(516U),
                                  requesters, 1U, 14U) == Native::VtdUnitError::None,
                   tables.Pages[0U][0U] == 3U, tables.Pages[511U][508U] == 3U,
                   model.Registers[0x18U / 8U] == (UInt64{3U} << 22U), full.Enable() == Native::VtdUnitError::None,
                   model.Registers[0U] == 0x11FFU, model.Commands[8U] == 1U, model.Commands[2U] == 0U, model.Valid,
                   (model.Registers[0x18U / 8U] & ((UInt64{3U} << 22U) | 0xF00U)) == ((UInt64{3U} << 22U) | 0xF00U));
    model.CommandPage = 2U;
    model.CompletionPage = 6U;
    tables = {};
    model.Reset();
    Native::AmdViUnit unit{model, model};
    ASTERIA_EXPECT(context, "fault-boundary unit prepares", Configure(unit, tables) == Native::VtdUnitError::None);
    Native::VtdTablePage pages[8U]{};
    for (Size index{0U}; index < 8U; ++index)
    {
        pages[index] = tables.Page(index + 8U);
    }
    Native::AmdViDomainBackend reserved{unit, 5U, pages};
    ASTERIA_EXPECT(context, "IVMD can seed an absent PCI requester without creating assignment authority",
                   reserved.PrepareReserved(), reserved.SeedReserved(0x20U) == Native::VtdUnitError::None,
                   reserved.SeedReserved(0x10U) == Native::VtdUnitError::None, !unit.Serves(0x20U),
                   (tables.Pages[0U][0x20U * 4U + 1U] & 0xFFFFU) == 5U, unit.Enable() == Native::VtdUnitError::None);
    Hal::IommuBackendMapRequest request{.DmaStart = Device::DmaAddress{0xFEE00000U},
                                        .PhysicalStart = Memory::PhysicalAddress{0x100000U},
                                        .Pages = Memory::PageCount{1U},
                                        .Permissions = Device::DmaPermissions{Device::DmaPermission::Read},
                                        .Reserved = {}};
    ASTERIA_EXPECT(context, "MSI and HyperTransport windows cannot be ordinary DMA mappings",
                   reserved.Map(request) == Hal::IommuBackendError::InvalidRequest);
    request.DmaStart = Device::DmaAddress{0xFD00000000ULL};
    ASTERIA_EXPECT(context, "HyperTransport starts are rejected at 48-bit input width",
                   reserved.Map(request) == Hal::IommuBackendError::InvalidRequest);
    // Section 2.5 ownership: publication is ordered before the tail.
    const UInt64 event = (UInt64{8U} << 60U) | 0x20U | (UInt64{1U} << 49U);
    tables.Pages[4U][0U] = event;
    tables.Pages[4U][1U] = 0x123000U;
    model.Registers[0x2018U / 8U] = 16U;
    unit.PollFaults();
    ASTERIA_EXPECT(context, "published event is consumed once without changing hardware record RAM",
                   model.Registers[0x2010U / 8U] == 16U, !unit.GetSnapshot().Failed, reserved.GetFaults().Count == 1U,
                   unit.GetSnapshot().LastFault.Read, tables.Pages[4U][0U] == event, tables.Pages[4U][1U] == 0x123000U);
    tables.Pages[4U][2U] = (UInt64{7U} << 60U) | 0x10U;
    tables.Pages[4U][3U] = 0xFD00000000ULL;
    model.Registers[0x2018U / 8U] = 32U;
    ASTERIA_EXPECT(context, "device errors remain attributed and do not disable other domains",
                   reserved.GetFaults().Count == 2U, unit.IsEnabled(), reserved.GetFaults().LastReason == 7U);
    tables.Pages[4U][4U] = 0U;
    tables.Pages[4U][5U] = 0xABCDEFU;
    model.Registers[0x2018U / 8U] = 48U;
    unit.PollFaults();
    ASTERIA_EXPECT(context, "reserved code is consumed by the pointer protocol and not retried",
                   model.Registers[0x2010U / 8U] == 48U, tables.Pages[4U][5U] == 0xABCDEFU,
                   reserved.GetFaults().Count == 2U);
    tables.Pages[4U][6U] = UInt64{5U} << 60U;
    model.Registers[0x2018U / 8U] = 64U;
    unit.PollFaults();
    ASTERIA_EXPECT(context, "command errors fail the unit but detach still writes a blocking DTE",
                   unit.GetSnapshot().Failed, unit.DetachContext(0x10U, 5U) == Native::VtdUnitError::Failed,
                   tables.Pages[0U][0x10U * 4U] == 3U, reserved.GetUsedPages() == 1U);
}

// AMD 48882 section 2.5 pointer-ownership model. Staged RAM is invisible
// before tail publication; every published record is consumed once, including
// reserved type zero, and hardware-owned bytes remain intact across wraps.
void EventProtocol(Asteria::TestSupport::TestContext& context, const UInt64 seed)
{
    static Tables tables;
    static Model model{tables};
    tables = {};
    model.Reset();
    Native::AmdViUnit unit{model, model};
    if (!ASTERIA_EXPECT(context, "event model starts", Configure(unit, tables) == Native::VtdUnitError::None,
                        unit.Enable() == Native::VtdUnitError::None))
    {
        return;
    }
    Asteria::TestSupport::Random random{seed};
    UInt64 tail{0U};
    UInt64 faults{0U};
    for (Size step{0U}; step < 1024U; ++step)
    {
        const Size records = static_cast<Size>(random.Below(15U)) + 1U;
        const UInt64 old = tail;
        UInt64 added{0U};
        for (Size record{0U}; record < records; ++record)
        {
            const UInt64 types[]{0U, 2U, 7U, 8U};
            const UInt64 type = types[random.Below(4U)];
            const Size slot = static_cast<Size>(tail / 16U);
            tables.Pages[4U + slot / 256U][(slot % 256U) * 2U] = (type << 60U) | 0x10U;
            tables.Pages[4U + slot / 256U][(slot % 256U) * 2U + 1U] = seed * 4096U + step;
            added += type != 0U ? 1U : 0U;
            tail = (tail + 16U) % 8192U;
        }
        unit.PollFaults();
        bool good = model.Registers[0x2010U / 8U] == old && unit.GetSnapshot().Faults == faults;
        model.Registers[0x2018U / 8U] = tail;
        unit.PollFaults();
        unit.PollFaults();
        faults += added;
        good = good && model.Registers[0x2010U / 8U] == tail && unit.GetSnapshot().Faults == faults;
        for (Size record{0U}; record < records; ++record)
        {
            const Size slot = static_cast<Size>(((old + record * 16U) % 8192U) / 16U);
            good = good && tables.Pages[4U + slot / 256U][(slot % 256U) * 2U + 1U] == seed * 4096U + step;
        }
        if (!ASTERIA_EXPECT(context, "publication/consumption and retained bytes model", good,
                            !unit.GetSnapshot().Failed))
        {
            static_cast<void>(std::fprintf(stderr, "seed=%llu step=%zu publish records=%zu tail=%llu\n",
                                           static_cast<unsigned long long>(seed), step, records,
                                           static_cast<unsigned long long>(tail)));
            return;
        }
    }
}

// Section 2.5.1 recovery model checks the RO run transition and RW1C
// acknowledgement separately from event-ring publication.
void EventOverflow(Asteria::TestSupport::TestContext& context)
{
    for (Size delay{0U}; delay < 5U; ++delay)
    {
        static Tables tables;
        static Model model{tables};
        tables = {};
        model.Reset();
        Native::AmdViUnit unit{model, model};
        if (!ASTERIA_EXPECT(context, "overflow model starts", Configure(unit, tables) == Native::VtdUnitError::None,
                            unit.Enable() == Native::VtdUnitError::None))
        {
            return;
        }
        model.Registers[0x2020U / 8U] = 11U; // Run, interrupt, overflow.
        model.EventRunPolls = delay;
        model.EventRunStuck = delay == 4U;
        unit.PollFaults();
        const bool timedOut = delay == 4U;
        ASTERIA_EXPECT(context, "restart waits for stopped log; timeout changes no ownership registers", model.Valid,
                       unit.GetSnapshot().Overflows == 1U, unit.GetSnapshot().Failed == timedOut,
                       model.RestartPhase == (timedOut ? 0U : 3U),
                       model.Registers[0x2020U / 8U] == (timedOut ? 11U : 10U),
                       (model.Registers[0x18U / 8U] & 4U) != 0U);
    }
}

void HandoffRecord(Asteria::TestSupport::TestContext& context)
{
    static Tables tables;
    static Model model{tables};
    model.Reset();
    constexpr Size offsets[15U]{0U, 0x100U, 0x108U, 0x110U, 0x118U, 0x120U, 0x128U, 0x130U,
                                8U, 0x10U,  0x38U,  0xE0U,  0xE8U,  0xF0U,  0xF8U};
    for (Size i{0U}; i < 15U; ++i)
    {
        model.Registers[offsets[i] / 8U] = (0x100000U + i * 0x20000U) | (UInt64{9U} << 56U);
    }
    model.Registers[0x2000U / 8U] = 16U;
    model.Registers[0x2008U / 8U] = 32U;
    model.Registers[0x2010U / 8U] = 48U;
    model.Registers[0x2018U / 8U] = 64U;
    constexpr Size secondary[8U]{0x2030U, 0x2038U, 0x2040U, 0x2048U, 0x2050U, 0x2058U, 0x2070U, 0x2078U};
    for (Size i{0U}; i < 8U; ++i)
    {
        model.Registers[secondary[i] / 8U] = 80U + i * 16U;
    }
    Native::AmdViUnit unit{model, model};
    ASTERIA_EXPECT(context, "handoff completes", unit.PrepareDisabled() == Native::VtdUnitError::None);
    Native::AmdViHandoffRecord record{};
    record.RegisterBase = 0xFED90000U;
    unit.CopyHandoffRecord(record);
    bool complete{true};
    for (Size i{0U}; i < 15U; ++i)
    {
        complete = complete && record.Bases[i] == model.Registers[offsets[i] / 8U];
    }
    for (Size i{0U}; i < 8U; ++i)
    {
        complete = complete && record.SecondaryPointers[i] == 80U + i * 16U;
    }
    ASTERIA_EXPECT(context, "all named bases and queue pointers retained", complete, record.RegisterBase == 0xFED90000U,
                   record.CommandHead == 16U, record.CommandTail == 32U, record.EventHead == 48U,
                   record.EventTail == 64U, record.AllowsBootServices());
    Memory::PhysicalPageImportExclusion excluded[15U]{};
    ASTERIA_EXPECT(context, "taken-over firmware extents reusable", record.Exclusions(excluded) == 0U);
    record.Outcome = Asteria::Platform::Device::FirmwareHandoffOutcome::Refused;
    ASTERIA_EXPECT(context, "all named bases excluded after refusal", !record.AllowsBootServices(),
                   record.Exclusions(excluded) == 15U, excluded[0U].Start == 0x100000U, excluded[0U].Bytes == 4096U,
                   excluded[8U].Bytes == 8192U, excluded[11U].Bytes == 4096U, excluded[12U].Bytes == 4096U);
}

int main()
{
    Asteria::TestSupport::TestContext context{"AmdViUnit"};
    const auto seeds = Asteria::TestSupport::GetSeedRange(1U, 8U);
    for (UInt64 seed{seeds.First}; seed < seeds.First + seeds.Count; ++seed)
    {
        RunModel(context, seed);
        EventProtocol(context, seed);
    }
    ReservedAndFailed(context);
    ReviewBoundaries(context);
    EventOverflow(context);
    HandoffRecord(context);
    static Tables tables;
    static Model model{tables};
    Native::AmdViUnit unit{model, model};
    ASTERIA_EXPECT(context, "timeout retains resources and fails the unit",
                   Configure(unit, tables) == Native::VtdUnitError::None);
    model.Stuck = true;
    ASTERIA_EXPECT(context, "a missing completion store cannot be mistaken for a drained command",
                   unit.Enable() == Native::VtdUnitError::TimedOut, unit.GetSnapshot().Failed, !unit.IsEnabled());
    return context.Finish();
}
