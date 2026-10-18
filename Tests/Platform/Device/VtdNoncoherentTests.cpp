// SPDX-License-Identifier: Apache-2.0
#include "Kernel/Device/IommuDomain.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Platform/Device/VtdDomainBackend.hpp"
#include "Platform/Device/VtdDomainGroup.hpp"
#include "Platform/Device/VtdUnit.hpp"

#include <cstdio>
#include <initializer_list>

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
using Foundation::UInt8;
using Foundation::UIntPointer;
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
using Platform::Device::VtdDomainGroup;
using Platform::Device::VtdTablePage;
using Platform::Device::VtdUnit;
using Platform::Device::VtdUnitError;
using Platform::Synchronization::SpinLock;

// Intel 7500 chipset datasheet section 21.14.1.2 (VTD_CAP, VTBAR + 08h): 256
// domains (ND 2), protected low and high memory, no RWBF, no caching mode,
// 4-level walks only (SAGAW bit 2), MGAW 2Fh, eight fault records at 100h
// (FRO 10h, NFR 7), page-selective invalidation, MAMV 9, read and write
// draining.
constexpr UInt64 Ioh7500Capability{UInt64{2U} | (UInt64{3U} << 5U) | (UInt64{0x4U} << 8U) | (UInt64{0x2FU} << 16U) |
                                   (UInt64{0x10U} << 24U) | (UInt64{1U} << 39U) | (UInt64{7U} << 40U) |
                                   (UInt64{9U} << 48U) | (UInt64{3U} << 54U)};
// Section 21.14.1.3 (EXT_VTD_CAP, VTBAR + 10h): QI, device TLBs, interrupt
// remapping, caching hints, pass-through and snoop control, the IOTLB
// registers at 200h (IRO 20h), MHMV Fh, and C (bit 0) clear: the default the
// firmware may leave (VT-d revision 1.3 section 10.4.3: remapping-structure
// reads do not snoop).
constexpr UInt64 Ioh7500ExtendedCapability{UInt64{0xEEU} | (UInt64{0x20U} << 8U) | (UInt64{0xFU} << 20U)};
constexpr UInt64 CoherentWalks{1U};
constexpr UInt64 WriteBufferFlushCapability{UInt64{1U} << 4U};

constexpr Size CommandRegister{0x18U};
constexpr Size RootTableRegister{0x20U};
constexpr Size ContextCommandRegister{0x28U};
constexpr Size IotlbRegister{0x208U};
constexpr UInt32 TranslationEnable{UInt32{1U} << 31U};
constexpr UInt32 RootTablePointer{UInt32{1U} << 30U};
constexpr UInt32 WriteBufferFlush{UInt32{1U} << 27U};

/// Table pages as the processor stores them (Memory) and as the unit reads
/// them (Visible). Without ECAP.C the unit reads memory, so a line reaches
/// Visible only when FlushTables writes it back, and that write-back stays
/// Unordered until the next Fence. With ECAP.C the unit snoops, so a Fence
/// makes every earlier store visible.
struct TableMemory
{
    static constexpr Size PageCount{21U};
    static constexpr Size Entries{512U};
    static constexpr Size PageBytes{Entries * sizeof(UInt64)};
    /// IA-64's fc stride, the finer of the two adapters' lines.
    static constexpr Size LineBytes{32U};
    static constexpr Size LineEntries{LineBytes / sizeof(UInt64)};
    static constexpr Size Lines{PageBytes / LineBytes};

    void Reset() noexcept
    {
        for (Size page{0U}; page < PageCount; ++page)
        {
            for (Size entry{0U}; entry < Entries; ++entry)
            {
                Memory[page][entry] = 0U;
                Visible[page][entry] = 0U;
            }
            for (bool& line : Unordered[page])
            {
                line = false;
            }
        }
    }

    [[nodiscard]] VtdTablePage Page(const Size index) noexcept
    {
        return VtdTablePage{Memory[index], static_cast<UInt64>(reinterpret_cast<UIntPointer>(Memory[index]))};
    }

    alignas(4096) UInt64 Memory[PageCount][Entries];
    UInt64 Visible[PageCount][Entries];
    bool Unordered[PageCount][Lines];
};

enum class Disorder : UInt8
{
    None,
    /// The unit would read a line other than the processor last stored.
    Stale,
    /// A written-back line no Fence has ordered yet.
    Unfenced,
};

struct OrderingViolation
{
    Size Offset;
    Size Page;
    Size Line;
    Disorder Kind;
};

/// One 7500-shaped unit: commands complete at once, GCMD's persistent bits
/// are reflected in GSTS, there are no faults and no invalidation queue.
/// Before every register write the model checks that the unit would read
/// exactly the tables the processor holds, with every write-back fenced,
/// and records the first write that breaks this.
class OrderingVtd final : public IVtdRegisters, public IVtdCacheMaintenance
{
public:
    OrderingVtd(TableMemory& tables, const UInt64 capability, const UInt64 extendedCapability) noexcept
        : Capability{capability}, ExtendedCapability{extendedCapability}, m_tables{tables}
    {
    }

    [[nodiscard]] UInt32 Read32(const Size offset) const noexcept override
    {
        switch (offset)
        {
        case 0x00U:
            return 0x10U;
        case 0x1CU:
            return Status;
        case 0x38U:
            return FaultEventControl;
        case 0xA0U:
            return InvalidationEventControl;
        default:
            return 0U;
        }
    }

    [[nodiscard]] UInt64 Read64(const Size offset) const noexcept override
    {
        switch (offset)
        {
        case 0x08U:
            return Capability;
        case 0x10U:
            return ExtendedCapability;
        default:
            return 0U;
        }
    }

    void Write32(const Size offset, const UInt32 value) noexcept override
    {
        CheckOrdered(offset);
        if (offset == CommandRegister)
        {
            constexpr UInt32 persistent{TranslationEnable | (UInt32{1U} << 26U) | (UInt32{1U} << 25U) |
                                        (UInt32{1U} << 28U) | (UInt32{1U} << 23U)};
            Status = (Status & ~persistent) | (value & persistent) | (value & RootTablePointer);
            WriteBufferFlushes += ((value & WriteBufferFlush) != 0U) ? 1U : 0U;
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
        CheckOrdered(offset);
        if (offset == RootTableRegister)
        {
            RootAddress = value;
        }
        ContextInvalidations += (offset == ContextCommandRegister) ? 1U : 0U;
        IotlbInvalidations += (offset == IotlbRegister) ? 1U : 0U;
    }

    [[nodiscard]] bool ReadQueueWord(const UInt64, UInt32&) const noexcept override
    {
        return false;
    }

    [[nodiscard]] bool WriteQueueWord(const UInt64, const UInt32) noexcept override
    {
        return false;
    }

    [[nodiscard]] bool CanWriteFirmwareMemory(const UInt64, const UInt64) const noexcept override
    {
        return false;
    }

    void ReportHandoff(const Platform::Device::VtdFirmwareState&) noexcept override
    {
    }

    void ReportRefusal(const Platform::Device::VtdHandoffFailure) noexcept override
    {
    }

    void Flush(const volatile void*) noexcept override
    {
        ++LineFlushes;
    }

    void FlushTables(const volatile void* const start, const Size bytes) noexcept override
    {
        ++TableFlushes;
        const auto first = reinterpret_cast<UIntPointer>(start);
        const auto base = reinterpret_cast<UIntPointer>(m_tables.Memory[0U]);
        if ((bytes == 0U) || (first < base) || ((first - base) >= (TableMemory::PageCount * TableMemory::PageBytes)) ||
            (bytes > ((TableMemory::PageCount * TableMemory::PageBytes) - (first - base))))
        {
            ++MalformedFlushes;
            return;
        }
        const UIntPointer end = first + bytes;
        for (UIntPointer line = first & ~UIntPointer{TableMemory::LineBytes - 1U}; line < end;
             line += TableMemory::LineBytes)
        {
            const auto offset = static_cast<Size>(line - base);
            const Size page = offset / TableMemory::PageBytes;
            const Size index = (offset % TableMemory::PageBytes) / TableMemory::LineBytes;
            for (Size entry{0U}; entry < TableMemory::LineEntries; ++entry)
            {
                const Size at = (index * TableMemory::LineEntries) + entry;
                m_tables.Visible[page][at] = m_tables.Memory[page][at];
            }
            m_tables.Unordered[page][index] = true;
            ++FlushedLines;
        }
    }

    void Fence() noexcept override
    {
        ++Fences;
        for (Size page{0U}; page < TableMemory::PageCount; ++page)
        {
            for (bool& line : m_tables.Unordered[page])
            {
                line = false;
            }
            if ((ExtendedCapability & CoherentWalks) == 0U)
            {
                continue;
            }
            for (Size entry{0U}; entry < TableMemory::Entries; ++entry)
            {
                m_tables.Visible[page][entry] = m_tables.Memory[page][entry];
            }
        }
    }

    /// The first line the unit would read wrongly now, if any.
    [[nodiscard]] OrderingViolation FindDisorder() const noexcept
    {
        for (Size page{0U}; page < TableMemory::PageCount; ++page)
        {
            for (Size line{0U}; line < TableMemory::Lines; ++line)
            {
                if (m_tables.Unordered[page][line])
                {
                    return {.Offset = 0U, .Page = page, .Line = line, .Kind = Disorder::Unfenced};
                }
                for (Size entry{0U}; entry < TableMemory::LineEntries; ++entry)
                {
                    const Size at = (line * TableMemory::LineEntries) + entry;
                    if (m_tables.Visible[page][at] != m_tables.Memory[page][at])
                    {
                        return {.Offset = 0U, .Page = page, .Line = line, .Kind = Disorder::Stale};
                    }
                }
            }
        }
        return {};
    }

    UInt64 Capability;
    UInt64 ExtendedCapability;
    UInt32 Status{0U};
    UInt32 FaultEventControl{0U};
    UInt32 InvalidationEventControl{0U};
    UInt64 RootAddress{0U};
    Size RegisterWrites{0U};
    Size ContextInvalidations{0U};
    Size IotlbInvalidations{0U};
    Size WriteBufferFlushes{0U};
    Size TableFlushes{0U};
    Size FlushedLines{0U};
    Size MalformedFlushes{0U};
    Size LineFlushes{0U};
    Size Fences{0U};
    OrderingViolation First{};

private:
    void CheckOrdered(const Size offset) noexcept
    {
        ++RegisterWrites;
        if (First.Kind != Disorder::None)
        {
            return;
        }
        First = FindDisorder();
        First.Offset = offset;
    }

    TableMemory& m_tables;
};

[[nodiscard]] const char* DisorderName(const Disorder kind) noexcept
{
    switch (kind)
    {
    case Disorder::None:
        return "none";
    case Disorder::Stale:
        return "stale";
    case Disorder::Unfenced:
        return "unfenced";
    }
    return "unknown";
}

/// True when no register write saw the unit's view out of order and the
/// view is settled now; otherwise prints the first violation.
[[nodiscard]] bool IsOrdered(const OrderingVtd& unit, const char* const label) noexcept
{
    const OrderingViolation now = unit.FindDisorder();
    if ((unit.First.Kind == Disorder::None) && (now.Kind == Disorder::None) && (unit.MalformedFlushes == 0U) &&
        (unit.LineFlushes == 0U))
    {
        return true;
    }
    if (unit.First.Kind != Disorder::None)
    {
        static_cast<void>(std::fprintf(stderr, "%s: %s line (page %zu, line %zu) at the write of register %#zx\n",
                                       label, DisorderName(unit.First.Kind), unit.First.Page, unit.First.Line,
                                       unit.First.Offset));
    }
    else if (now.Kind != Disorder::None)
    {
        static_cast<void>(std::fprintf(stderr, "%s: %s line (page %zu, line %zu) when the call returned\n", label,
                                       DisorderName(now.Kind), now.Page, now.Line));
    }
    static_cast<void>(std::fprintf(stderr, "%s: malformed flushes %zu, single-line flushes %zu\n", label,
                                   unit.MalformedFlushes, unit.LineFlushes));
    return false;
}

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

// The unit at VTBAR (INCLUDE_PCI_ALL) serves 00:00.0, the display 00:02.0,
// the endpoint 00:06.0 and 01:00.0; the unit at VTBAR + 1000h the bridge
// 00:07.0 alone.
constexpr UInt16 IncludeAllRequesters[]{0x0000U, 0x0010U, 0x0030U, 0x0100U};
constexpr UInt16 BridgeRequesters[]{0x0038U};

// Page 0 is the root, 1-4 the context tables, 5-12 a user domain's tables
// and 13-20 a firmware domain's.
[[nodiscard]] VtdUnitError Configure(VtdUnit& unit, TableMemory& tables, const UInt16* const requesters,
                                     const Size count) noexcept
{
    const VtdTablePage contexts[VtdUnit::MaximumBuses]{tables.Page(1U), tables.Page(2U), tables.Page(3U),
                                                       tables.Page(4U)};
    return unit.Configure(tables.Page(0U), contexts, requesters, count);
}

void DomainPages(TableMemory& tables, const Size first, VtdTablePage (&pages)[VtdDomainBackend::TablePages]) noexcept
{
    for (Size index{0U}; index < VtdDomainBackend::TablePages; ++index)
    {
        pages[index] = tables.Page(first + index);
    }
}

[[nodiscard]] IommuBackendMapRequest MakeRequest(const UInt64 dma, const UInt64 pages,
                                                 const DmaPermissions permissions) noexcept
{
    return IommuBackendMapRequest{
        .DmaStart = DmaAddress{dma},
        .PhysicalStart = Kernel::Memory::PhysicalAddress{0x7000'0000U + dma},
        .Pages = Kernel::Memory::PageCount{pages},
        .Permissions = permissions,
        .Reserved = {},
    };
}

// DMA isolation: the root and context tables, and a firmware domain's
// Seeded context and identity mappings, are written back and fenced before the unit is pointed at
// them (RTADDR, SRTP) and translates (TE). A snooping unit gets no write-back, only the fences.
void TestBootPublication(TestContext& runner) noexcept
{
    static TableMemory tables;
    for (const bool coherent : {false, true})
    {
        tables.Reset();
        OrderingVtd registers{tables, Ioh7500Capability, Ioh7500ExtendedCapability | (coherent ? CoherentWalks : 0U)};
        VtdUnit unit{registers, registers};
        VtdTablePage pages[VtdDomainBackend::TablePages]{};
        DomainPages(tables, 13U, pages);
        VtdDomainBackend firmware{unit, 5U, pages};
        const bool configured = Configure(unit, tables, IncludeAllRequesters, 4U) == VtdUnitError::None;
        const bool seeded = firmware.PrepareReserved() &&
                            (firmware.MapReserved(0x000E'0000U, 0x000E'1FFFU) == IommuBackendError::None) &&
                            (firmware.SeedReserved(0x0030U) == VtdUnitError::None);
        const Size writesBeforeEnable = registers.RegisterWrites;
        const bool enabled = unit.Enable() == VtdUnitError::None;
        ASTERIA_EXPECT(
            runner, "root, context and seeded firmware tables are readable before RTADDR, SRTP and TE", configured,
            seeded, enabled, IsOrdered(registers, coherent ? "coherent boot" : "non-coherent boot"),
            registers.RegisterWrites > writesBeforeEnable, (registers.Status & TranslationEnable) != 0U,
            registers.RootAddress == tables.Page(0U).PhysicalAddress,
            tables.Visible[0U][0U] == (tables.Page(1U).PhysicalAddress | 1U),
            tables.Visible[0U][2U] == (tables.Page(2U).PhysicalAddress | 1U),
            tables.Visible[1U][2U * 0x30U] == (tables.Page(13U).PhysicalAddress | 1U),
            tables.Visible[1U][(2U * 0x30U) + 1U] == ((UInt64{5U} << 8U) | 2U),
            coherent ? (registers.TableFlushes == 0U) : (registers.FlushedLines != 0U), registers.Fences != 0U);
        // The model itself: a table store no write-back published is caught
        // at the next register write, here the domain's IOTLB invalidation.
        tables.Memory[13U][1U] = tables.Page(14U).PhysicalAddress | 3U;
        const VtdUnitError invalidated = unit.InvalidateDomain(5U);
        ASTERIA_EXPECT(runner, "the ordering model reports an unpublished table store at the next register write",
                       invalidated == VtdUnitError::None, registers.First.Kind == Disorder::Stale,
                       registers.First.Offset == IotlbRegister, registers.First.Page == 13U,
                       registers.First.Line == 0U);
    }
}

enum class Operation : UInt8
{
    Claim,
    Release,
    Map,
    Unmap,
    Attach,
    Detach,
};

[[nodiscard]] const char* OperationName(const Operation operation) noexcept
{
    switch (operation)
    {
    case Operation::Claim:
        return "claim";
    case Operation::Release:
        return "release";
    case Operation::Map:
        return "map";
    case Operation::Unmap:
        return "unmap";
    case Operation::Attach:
        return "attach";
    case Operation::Detach:
        return "detach";
    }
    return "unknown";
}

struct Variant
{
    const char* Name;
    bool Coherent;
    bool WriteBuffer;
};

constexpr Size Units{2U};
constexpr Size Windows{6U};
constexpr Size WindowPages{4U};
constexpr Size Requesters{3U};
constexpr UInt16 RequesterIds[Requesters]{0x0030U, 0x0100U, 0x0038U};
constexpr Size RequesterUnits[Requesters]{0U, 0U, 1U};
constexpr UInt64 WindowBytes{UInt64{1U} << 21U};

/// What a group of identical 4-level units must do: one top table at Claim,
/// a page-directory-pointer table, a directory and a table for the first
/// 2 MiB window, one table per further window, and no more than eight pages.
struct DomainModel
{
    bool Claimed;
    bool Attached[Requesters];
    bool Mapped[Windows][WindowPages];
    bool Touched[Windows];
    Size TouchedCount;
};

struct Step
{
    Operation Kind;
    Size Window;
    Size Page;
    UInt64 Pages;
    Size Requester;
    DmaPermissions Permissions;
};

/// What one step must issue on one unit, and whether it publishes tables
/// there (fences, and write-backs where the walks do not snoop).
struct UnitEffect
{
    Size Context;
    Size Iotlb;
    Size WriteBuffer;
    bool Published;
};

struct UnitCounters
{
    Size Context;
    Size Iotlb;
    Size WriteBuffer;
    Size Fences;
    Size Flushes;
};

[[nodiscard]] constexpr unsigned Code(const IommuBackendError error) noexcept
{
    return static_cast<unsigned>(error);
}

[[nodiscard]] constexpr unsigned Code(const IommuBackendOwnershipError error) noexcept
{
    return 0x100U + static_cast<unsigned>(error);
}

[[nodiscard]] UnitCounters Count(const OrderingVtd& unit) noexcept
{
    return {.Context = unit.ContextInvalidations,
            .Iotlb = unit.IotlbInvalidations,
            .WriteBuffer = unit.WriteBufferFlushes,
            .Fences = unit.Fences,
            .Flushes = unit.TableFlushes};
}

/// Mostly steps on a claimed domain, which maps only free pages and unmaps
/// only mapped ones.
[[nodiscard]] Step ChooseStep(TestSupport::Random& random, const DomainModel& model) noexcept
{
    constexpr Operation weighted[]{Operation::Claim,  Operation::Release, Operation::Map,   Operation::Map,
                                   Operation::Map,    Operation::Unmap,   Operation::Unmap, Operation::Attach,
                                   Operation::Attach, Operation::Detach,  Operation::Detach};
    const DmaPermissions permissions[]{DmaPermissions{DmaPermission::Read}, DmaPermissions{DmaPermission::Write},
                                       DmaPermissions{DmaPermission::Read}.With(DmaPermission::Write)};
    Step step{.Kind = weighted[random.Below(sizeof(weighted) / sizeof(weighted[0U]))],
              .Window = static_cast<Size>(random.Below(Windows)),
              .Page = static_cast<Size>(random.Below(WindowPages)),
              .Pages = 1U,
              .Requester = static_cast<Size>(random.Below(Requesters)),
              .Permissions = permissions[random.Below(3U)]};
    const bool mapped = model.Mapped[step.Window][step.Page];
    if (!model.Claimed && (step.Kind != Operation::Claim) && random.Chance(1U, 2U))
    {
        step.Kind = Operation::Claim;
    }
    else if (model.Claimed && (step.Kind == Operation::Map) && mapped)
    {
        step.Kind = Operation::Unmap;
    }
    else if (model.Claimed && (step.Kind == Operation::Unmap) && !mapped)
    {
        step.Kind = Operation::Map;
    }
    if (((step.Page + 1U) < WindowPages) && random.Chance(1U, 2U) &&
        (model.Mapped[step.Window][step.Page + 1U] == mapped))
    {
        step.Pages = 2U;
    }
    return step;
}

/// Advances the model by one step and returns the result the group must give.
[[nodiscard]] unsigned Predict(const Step& step, const bool writeBuffer, DomainModel& model,
                               UnitEffect (&effects)[Units]) noexcept
{
    const Size flush = writeBuffer ? 1U : 0U;
    switch (step.Kind)
    {
    case Operation::Claim:
        if (model.Claimed)
        {
            return Code(IommuBackendOwnershipError::AlreadyOwned);
        }
        model = DomainModel{};
        model.Claimed = true;
        effects[0U].Published = true;
        effects[1U].Published = true;
        return Code(IommuBackendOwnershipError::None);
    case Operation::Release:
        if (!model.Claimed)
        {
            return Code(IommuBackendOwnershipError::InvalidOwner);
        }
        // Each attached requester is detached, then the domain invalidated.
        for (UnitEffect& effect : effects)
        {
            effect = {.Context = 0U, .Iotlb = 1U, .WriteBuffer = 0U, .Published = true};
        }
        for (Size index{0U}; index < Requesters; ++index)
        {
            UnitEffect& effect = effects[RequesterUnits[index]];
            effect.Context += model.Attached[index] ? 1U : 0U;
            effect.Iotlb += model.Attached[index] ? 1U : 0U;
            effect.WriteBuffer += model.Attached[index] ? flush : 0U;
        }
        model = DomainModel{};
        return Code(IommuBackendOwnershipError::None);
    case Operation::Map:
    {
        if (!model.Claimed)
        {
            return Code(IommuBackendError::NotReady);
        }
        const Size used = (model.TouchedCount == 0U) ? 1U : (3U + model.TouchedCount);
        Size cost{0U};
        if (!model.Touched[step.Window])
        {
            cost = (model.TouchedCount == 0U) ? 3U : 1U;
        }
        if ((used + cost) > VtdDomainBackend::TablePages)
        {
            return Code(IommuBackendError::CapacityExceeded);
        }
        model.TouchedCount += model.Touched[step.Window] ? 0U : 1U;
        model.Touched[step.Window] = true;
        for (UInt64 offset{0U}; offset < step.Pages; ++offset)
        {
            model.Mapped[step.Window][step.Page + offset] = true;
        }
        for (UnitEffect& effect : effects)
        {
            effect = {.Context = 0U, .Iotlb = 0U, .WriteBuffer = flush, .Published = true};
        }
        return Code(IommuBackendError::None);
    }
    case Operation::Unmap:
        if (!model.Claimed)
        {
            return Code(IommuBackendError::NotReady);
        }
        for (UInt64 offset{0U}; offset < step.Pages; ++offset)
        {
            model.Mapped[step.Window][step.Page + offset] = false;
        }
        for (UnitEffect& effect : effects)
        {
            effect = {.Context = 0U, .Iotlb = 1U, .WriteBuffer = 0U, .Published = true};
        }
        return Code(IommuBackendError::None);
    case Operation::Attach:
        if (!model.Claimed)
        {
            return Code(IommuBackendError::NotReady);
        }
        if (model.Attached[step.Requester])
        {
            return Code(IommuBackendError::InvalidRequest);
        }
        model.Attached[step.Requester] = true;
        effects[RequesterUnits[step.Requester]] = {.Context = 1U, .Iotlb = 0U, .WriteBuffer = flush, .Published = true};
        return Code(IommuBackendError::None);
    case Operation::Detach:
        if (!model.Attached[step.Requester])
        {
            return Code(IommuBackendError::InvalidRequest);
        }
        model.Attached[step.Requester] = false;
        effects[RequesterUnits[step.Requester]] = {.Context = 1U, .Iotlb = 1U, .WriteBuffer = flush, .Published = true};
        return Code(IommuBackendError::None);
    }
    return 0xFFFFU;
}

[[nodiscard]] unsigned Perform(const Step& step, VtdDomainGroup& group, const IommuDomain& owner,
                               FakeRequester (&devices)[Requesters]) noexcept
{
    const IommuBackendMapRequest request =
        MakeRequest((step.Window * WindowBytes) + (step.Page * 4096U), step.Pages, step.Permissions);
    switch (step.Kind)
    {
    case Operation::Claim:
        return Code(group.Claim(owner, 32U));
    case Operation::Release:
        return Code(group.Release(owner));
    case Operation::Map:
        return Code(group.Map(request));
    case Operation::Unmap:
        return Code(group.Unmap(request));
    case Operation::Attach:
        return Code(group.Attach(devices[step.Requester]));
    case Operation::Detach:
        return Code(group.Detach(devices[step.Requester]));
    }
    return 0xFFFFU;
}

/// The one comparison: the unit's view stayed ordered and the step issued
/// exactly the commands, fences and write-backs it had to.
[[nodiscard]] bool Matches(const OrderingVtd& unit, const UnitCounters& before, const UnitEffect& effect,
                           const Variant& variant) noexcept
{
    const UnitCounters after = Count(unit);
    const bool written =
        variant.Coherent ? (after.Flushes == 0U) : (!effect.Published || (after.Flushes > before.Flushes));
    return IsOrdered(unit, variant.Name) && ((after.Context - before.Context) == effect.Context) &&
           ((after.Iotlb - before.Iotlb) == effect.Iotlb) &&
           ((after.WriteBuffer - before.WriteBuffer) == effect.WriteBuffer) &&
           (!effect.Published || (after.Fences > before.Fences)) && written;
}

/// One seeded run of random domain operations on a two-unit group, checked
/// step by step against DomainModel; every register write, and the end of
/// every call, must find both units' views ordered.
[[nodiscard]] bool RunDomainSeed(const Variant& variant, const UInt64 seed) noexcept
{
    static TableMemory memories[Units];
    for (TableMemory& memory : memories)
    {
        memory.Reset();
    }
    const UInt64 capability = Ioh7500Capability | (variant.WriteBuffer ? WriteBufferFlushCapability : 0U);
    const UInt64 extended = Ioh7500ExtendedCapability | (variant.Coherent ? CoherentWalks : 0U);
    OrderingVtd registers[Units]{{memories[0U], capability, extended}, {memories[1U], capability, extended}};
    VtdUnit first{registers[0U], registers[0U]};
    VtdUnit second{registers[1U], registers[1U]};
    VtdTablePage firstPages[VtdDomainBackend::TablePages]{};
    VtdTablePage secondPages[VtdDomainBackend::TablePages]{};
    DomainPages(memories[0U], 5U, firstPages);
    DomainPages(memories[1U], 5U, secondPages);
    VtdDomainBackend firstBackend{first, 1U, firstPages};
    VtdDomainBackend secondBackend{second, 1U, secondPages};
    VtdDomainBackend* const backends[VtdDomainGroup::MaximumUnits]{&firstBackend, &secondBackend};
    VtdUnit* const units[VtdDomainGroup::MaximumUnits]{&first, &second};
    VtdDomainGroup group{backends, units, Units};
    SpinLock synchronization;
    Asteria::TestSupport::TestObjectRegistry registry{synchronization};
    const IommuDomain owner{registry, group};
    FakeRequester devices[Requesters]{FakeRequester{RequesterIds[0U]}, FakeRequester{RequesterIds[1U]},
                                      FakeRequester{RequesterIds[2U]}};
    if ((Configure(first, memories[0U], IncludeAllRequesters, 4U) != VtdUnitError::None) ||
        (Configure(second, memories[1U], BridgeRequesters, 1U) != VtdUnitError::None) ||
        (first.Enable() != VtdUnitError::None) || (second.Enable() != VtdUnitError::None) ||
        !IsOrdered(registers[0U], variant.Name) || !IsOrdered(registers[1U], variant.Name))
    {
        static_cast<void>(
            std::fprintf(stderr, "%s seed=%llu: bring-up\n", variant.Name, static_cast<unsigned long long>(seed)));
        return false;
    }
    DomainModel model{};
    TestSupport::Random random{seed};
    for (Size index{0U}; index < 128U; ++index)
    {
        const Step step = ChooseStep(random, model);
        const UnitCounters before[Units]{Count(registers[0U]), Count(registers[1U])};
        UnitEffect effects[Units]{};
        const unsigned expected = Predict(step, variant.WriteBuffer, model, effects);
        const unsigned actual = Perform(step, group, owner, devices);
        bool good = actual == expected;
        for (Size unit{0U}; unit < Units; ++unit)
        {
            good = good && Matches(registers[unit], before[unit], effects[unit], variant);
        }
        if (!good)
        {
            static_cast<void>(std::fprintf(
                stderr,
                "%s seed=%llu step=%zu op=%s window=%zu page=%zu pages=%llu requester=%04x result=%#x expected=%#x\n",
                variant.Name, static_cast<unsigned long long>(seed), index, OperationName(step.Kind), step.Window,
                step.Page, static_cast<unsigned long long>(step.Pages),
                static_cast<unsigned>(RequesterIds[step.Requester]), actual, expected));
            return false;
        }
    }
    return true;
}

// The same decision for live domains: a new mapping is readable when Map
// returns (and, with RWBF, before the write-buffer flush), a context entry
// before its device-selective context invalidation, a cleared entry before
// the IOTLB invalidation of Unmap and Detach, and every table of a group
// Claim and Release when the call returns.
void TestDomainPublication(TestContext& runner) noexcept
{
    constexpr Variant variants[]{
        {.Name = "non-coherent", .Coherent = false, .WriteBuffer = false},
        {.Name = "non-coherent with RWBF", .Coherent = false, .WriteBuffer = true},
        {.Name = "coherent", .Coherent = true, .WriteBuffer = false},
    };
    const TestSupport::SeedRange seeds = TestSupport::GetSeedRange(0x7500U, 16U);
    for (const Variant& variant : variants)
    {
        bool good{true};
        for (UInt64 seed{seeds.First}; good && (seed < (seeds.First + seeds.Count)); ++seed)
        {
            good = RunDomainSeed(variant, seed);
        }
        ASTERIA_EXPECT(runner, "domain tables are published before the register write or return that exposes them",
                       good);
    }
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"VtdNoncoherentTests"};
    Asteria::Test::TestBootPublication(runner);
    Asteria::Test::TestDomainPublication(runner);
    return runner.Finish();
}
