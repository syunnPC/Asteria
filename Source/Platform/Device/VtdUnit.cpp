// SPDX-License-Identifier: Apache-2.0

#include "Platform/Device/VtdUnit.hpp"

namespace Asteria::Platform::Device
{
namespace
{
using Foundation::Size;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;

// Register offsets (Intel VT-d revision 4.0, section 11.4).
constexpr Size VersionRegister{0x00U};
constexpr Size CapabilityRegister{0x08U};
constexpr Size ExtendedCapabilityRegister{0x10U};
constexpr Size CommandRegister{0x18U};
constexpr Size StatusRegister{0x1CU};
constexpr Size RootTableRegister{0x20U};
constexpr Size ProtectedMemoryRegister{0x64U};
constexpr Size QueueHeadRegister{0x80U};
constexpr Size QueueTailRegister{0x88U};
constexpr Size ContextCommandRegister{0x28U};
constexpr Size FaultStatusRegister{0x34U};
constexpr Size FaultEventControlRegister{0x38U};
constexpr Size QueueAddressRegister{0x90U};
constexpr Size InvalidationEventControlRegister{0xA0U};
constexpr Size TableBytes{4096U};
constexpr Size TableEntries{TableBytes / sizeof(UInt64)};

// GCMD and GSTS share bit positions.
constexpr UInt32 TranslationEnable{UInt32{1U} << 31U};
constexpr UInt32 RootTablePointer{UInt32{1U} << 30U};
constexpr UInt32 WriteBufferFlush{UInt32{1U} << 27U};
constexpr UInt32 QueuedInvalidation{UInt32{1U} << 26U};
constexpr UInt32 InterruptRemapping{UInt32{1U} << 25U};
constexpr UInt32 FaultInterruptMask{UInt32{1U} << 31U};
constexpr UInt32 PrimaryFaultOverflow{1U};
constexpr UInt32 PrimaryPendingFault{UInt32{1U} << 1U};

constexpr UInt64 CapabilityCachingMode{UInt64{1U} << 7U};
constexpr UInt64 CapabilityProtectedMemory{UInt64{3U} << 5U};
constexpr UInt64 CapabilityWriteBufferFlush{UInt64{1U} << 4U};
constexpr UInt64 CapabilityDrainWrites{UInt64{1U} << 54U};
constexpr UInt64 CapabilityDrainReads{UInt64{1U} << 55U};
constexpr UInt64 ExtendedCapabilityCoherent{1U};

constexpr UInt64 InvalidateContext{UInt64{1U} << 63U};
constexpr UInt64 ContextGlobal{UInt64{1U} << 61U};
constexpr UInt64 ContextDevice{UInt64{3U} << 61U};
constexpr UInt64 InvalidateIotlb{UInt64{1U} << 63U};
constexpr UInt64 IotlbGlobal{UInt64{1U} << 60U};
constexpr UInt64 IotlbDomain{UInt64{2U} << 60U};
constexpr UInt64 IotlbDrainReads{UInt64{1U} << 49U};
constexpr UInt64 IotlbDrainWrites{UInt64{1U} << 48U};

constexpr UInt64 FaultRecorded{UInt64{1U} << 63U};
constexpr UInt64 FaultRead{UInt64{1U} << 62U};
constexpr UInt64 EntryPresent{1U};
constexpr UInt64 TableAddressMask{~UInt64{0xFFFU}};

// Register commands complete within microseconds on hardware and at once
// under emulation.
constexpr Size SpinBound{1'000'000U};

[[nodiscard]] constexpr bool PageIsValid(const VtdTablePage& page) noexcept
{
    return (page.Entries != nullptr) && ((page.PhysicalAddress & ~TableAddressMask) == 0U) &&
           (page.PhysicalAddress != 0U);
}
} // Namespace

VtdUnitError VtdUnit::PrepareDisabled() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_configured)
    {
        return VtdUnitError::InvalidArguments;
    }
    if (m_failed)
    {
        return VtdUnitError::Failed;
    }
    ObserveFirmwareLocked();
    return ResetFirmwareLocked() ? VtdUnitError::None : HandoffErrorLocked();
}

VtdUnitError VtdUnit::Configure(const VtdTablePage& root, const VtdTablePage (&contexts)[MaximumBuses],
                                const UInt16* const requesters, const Size count, const Size registerBytes) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_failed)
    {
        return VtdUnitError::Failed;
    }
    if (m_configured || !PageIsValid(root) || (count > MaximumRequesters) || (registerBytes < 4096U) ||
        (registerBytes > (Size{4096U} << 15U)) || ((registerBytes & (registerBytes - 1U)) != 0U) ||
        ((requesters == nullptr) && (count != 0U)))
    {
        return VtdUnitError::InvalidArguments;
    }
    for (const VtdTablePage& context : contexts)
    {
        if (!PageIsValid(context))
        {
            return VtdUnitError::InvalidArguments;
        }
    }
    ObserveFirmwareLocked();
    if (!ResetFirmwareLocked())
    {
        return HandoffErrorLocked();
    }
    const UInt64 widths = (m_capability >> 8U) & 0x1FU;
    const Foundation::UInt8 levels = ((widths & 0x2U) != 0U) ? 3U : (((widths & 0x4U) != 0U) ? 4U : 0U);
    m_iotlbOffset = (((m_extendedCapability >> 8U) & 0x3FFU) * 16U) + 8U;
    m_faultOffset = ((m_capability >> 24U) & 0x3FFU) * 16U;
    m_faultRecords = ((m_capability >> 40U) & 0xFFU) + 1U;
    if ((levels == 0U) || ((m_capability & CapabilityCachingMode) != 0U) || (((m_capability >> 16U) & 0x3FU) < 11U) ||
        ((m_iotlbOffset + sizeof(UInt64)) > registerBytes) ||
        ((m_faultOffset + (m_faultRecords * 16U)) > registerBytes))
    {
        return VtdUnitError::Unsupported;
    }
    m_root = root;
    for (Size index{0U}; index < TableEntries; ++index)
    {
        m_root.Entries[index] = 0U;
    }
    for (Size index{0U}; (index < count) && (m_requesterCount < MaximumRequesters); ++index)
    {
        const UInt16 identifier = requesters[index];
        const auto bus = static_cast<Foundation::UInt8>(identifier >> 8U);
        if (FindLocked(identifier) != nullptr)
        {
            continue;
        }
        Size slot{0U};
        while ((slot < m_busCount) && (m_buses[slot] != bus))
        {
            ++slot;
        }
        if (slot == m_busCount)
        {
            if (m_busCount == MaximumBuses)
            {
                ++m_droppedRequesters;
                continue;
            }
            m_contexts[slot] = contexts[slot];
            for (Size entry{0U}; entry < TableEntries; ++entry)
            {
                m_contexts[slot].Entries[entry] = 0U;
            }
            m_buses[slot] = bus;
            m_root.Entries[2U * bus] = m_contexts[slot].PhysicalAddress | EntryPresent;
            ++m_busCount;
            PublishLocked(m_contexts[slot].Entries, TableBytes);
        }
        m_requesters[m_requesterCount] = Requester{.Identifier = identifier, .Domain = 0U, .Bus = bus, .Reserved = {}};
        ++m_requesterCount;
    }
    PublishLocked(m_root.Entries, TableBytes);
    m_levels = levels;
    m_configured = true;
    return VtdUnitError::None;
}

VtdUnitError VtdUnit::Enable() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!m_configured)
    {
        return VtdUnitError::NotConfigured;
    }
    if (m_failed)
    {
        return VtdUnitError::Failed;
    }
    if (m_enabled)
    {
        return VtdUnitError::AlreadyActive;
    }
    m_registers.Write32(FaultEventControlRegister, FaultInterruptMask);
    if (!FlushWritesLocked())
    {
        return VtdUnitError::TimedOut;
    }
    m_registers.Write64(RootTableRegister, m_root.PhysicalAddress);
    if (!CommandLocked(RootTablePointer, true) || !InvalidateContextLocked(ContextGlobal) ||
        !InvalidateIotlbLocked(IotlbGlobal))
    {
        return VtdUnitError::TimedOut;
    }
    if (!CommandLocked(TranslationEnable, true))
    {
        return VtdUnitError::TimedOut;
    }
    PollFaultsLocked();
    m_enabled = true;
    return VtdUnitError::None;
}

bool VtdUnit::IsEnabled() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return m_enabled && !m_failed;
}

Foundation::UInt8 VtdUnit::GetLevels() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return (m_enabled && !m_failed) ? m_levels : 0U;
}

Foundation::UInt8 VtdUnit::GetAddressBits() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!m_configured || m_failed)
    {
        return 0U;
    }
    const auto mgaw = static_cast<Foundation::UInt8>(((m_capability >> 16U) & 0x3FU) + 1U);
    const auto walk = static_cast<Foundation::UInt8>(12U + (9U * m_levels));
    return (mgaw < walk) ? mgaw : walk;
}

bool VtdUnit::Serves(const UInt16 requester) const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return FindLocked(requester) != nullptr;
}

VtdUnitError VtdUnit::AttachContext(const UInt16 requester, const UInt16 domain, const UInt64 topTable) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_failed)
    {
        return VtdUnitError::Failed;
    }
    if (!m_enabled)
    {
        return VtdUnitError::NotConfigured;
    }
    return InstallContextLocked(requester, domain, topTable);
}

Foundation::UInt8 VtdUnit::GetConfiguredLevels() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return (m_configured && !m_failed) ? m_levels : 0U;
}

VtdUnitError VtdUnit::SeedContext(const UInt16 requester, const UInt16 domain, const UInt64 topTable) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!m_configured || m_enabled || m_failed)
    {
        return VtdUnitError::NotConfigured;
    }
    return InstallContextLocked(requester, domain, topTable);
}

VtdUnitError VtdUnit::InstallContextLocked(const UInt16 requester, const UInt16 domain, const UInt64 topTable) noexcept
{
    if ((domain == 0U) || (domain > MaximumDomain) || (domain >= (UInt64{1U} << (4U + (2U * (m_capability & 7U))))) ||
        (topTable == 0U) || ((topTable & ~TableAddressMask) != 0U))
    {
        return VtdUnitError::InvalidArguments;
    }
    Requester* const served = FindLocked(requester);
    if (served == nullptr)
    {
        return VtdUnitError::NotServed;
    }
    if (served->Domain != 0U)
    {
        return VtdUnitError::InvalidArguments;
    }
    // The upper half first: the entry is not present until the lower half's
    // present bit, which the processor stores after it.
    volatile UInt64* const entry = ContextEntryLocked(*served);
    entry[1U] = ((m_levels == 3U) ? 1U : 2U) | (static_cast<UInt64>(domain) << 8U);
    entry[0U] = topTable | EntryPresent;
    PublishLocked(entry, 2U * sizeof(UInt64));
    if (m_enabled && !InvalidateContextLocked(ContextDevice | (static_cast<UInt64>(requester) << 16U) | domain))
    {
        return VtdUnitError::TimedOut;
    }
    served->Domain = domain;
    return VtdUnitError::None;
}

VtdUnitError VtdUnit::DetachContext(const UInt16 requester, const UInt16 domain) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_failed)
    {
        return VtdUnitError::Failed;
    }
    Requester* const served = FindLocked(requester);
    if (served == nullptr)
    {
        return VtdUnitError::NotServed;
    }
    if ((domain == 0U) || (served->Domain != domain))
    {
        return VtdUnitError::InvalidArguments;
    }
    volatile UInt64* const entry = ContextEntryLocked(*served);
    entry[0U] = 0U;
    PublishLocked(entry, 2U * sizeof(UInt64));
    if (!InvalidateContextLocked(ContextDevice | (static_cast<UInt64>(requester) << 16U) | domain) ||
        !InvalidateIotlbLocked(IotlbDomain | (static_cast<UInt64>(domain) << 32U)))
    {
        return VtdUnitError::TimedOut;
    }
    entry[1U] = 0U;
    PublishLocked(entry, 2U * sizeof(UInt64));
    served->Domain = 0U;
    return VtdUnitError::None;
}

VtdUnitError VtdUnit::InvalidateDomain(const UInt16 domain) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_failed)
    {
        return VtdUnitError::Failed;
    }
    if (!m_enabled)
    {
        return VtdUnitError::NotConfigured;
    }
    if ((domain == 0U) || (domain > MaximumDomain))
    {
        return VtdUnitError::InvalidArguments;
    }
    return InvalidateIotlbLocked(IotlbDomain | (static_cast<UInt64>(domain) << 32U)) ? VtdUnitError::None
                                                                                     : VtdUnitError::TimedOut;
}

VtdUnitError VtdUnit::FlushWrites() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_failed)
    {
        return VtdUnitError::Failed;
    }
    return FlushWritesLocked() ? VtdUnitError::None : VtdUnitError::TimedOut;
}

void VtdUnit::Publish(const volatile void* const start, const Size bytes) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    PublishLocked(start, bytes);
}

void VtdUnit::PollFaults() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_configured)
    {
        PollFaultsLocked();
    }
}

VtdDomainFaults VtdUnit::GetDomainFaults(const UInt16 domain) const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return (domain <= MaximumDomain) ? m_domainFaults[domain] : VtdDomainFaults{};
}

VtdUnitSnapshot VtdUnit::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    UInt64 attached{0U};
    for (Size index{0U}; index < m_requesterCount; ++index)
    {
        attached += (m_requesters[index].Domain != 0U) ? 1U : 0U;
    }
    return VtdUnitSnapshot{
        .Capability = m_capability,
        .ExtendedCapability = m_extendedCapability,
        .Version = m_version,
        .HandoffStatus = m_handoffStatus,
        .Status = (m_configured || m_observed) ? m_registers.Read32(StatusRegister) : 0U,
        .Levels = m_levels,
        .Enabled = m_enabled,
        .Failed = m_failed,
        .Reserved = 0U,
        .Served = m_requesterCount,
        .DroppedRequesters = m_droppedRequesters,
        .Attached = attached,
        .Faults = m_faults,
        .Overflows = m_overflows,
        .Unattributed = m_unattributed,
        .LastFault = m_lastFault,
    };
}

bool VtdUnit::FlushWritesLocked() noexcept
{
    if ((m_capability & CapabilityWriteBufferFlush) == 0U)
    {
        return true;
    }
    return CommandLocked(WriteBufferFlush, true);
}

bool VtdUnit::CommandLocked(const UInt32 bit, const bool set) noexcept
{
    // Intel VT-d section 11.4.4.1: update one field per write, read GSTS
    // afresh, and clear the one-shot fields with the specified 96FFFFFFh
    // mask. Await the corresponding status before issuing another command.
    const UInt32 status = m_registers.Read32(StatusRegister) & 0x96FF'FFFFU;
    m_registers.Write32(CommandRegister, set ? (status | bit) : (status & ~bit));
    return WaitForStatusLocked(bit, (bit == WriteBufferFlush) ? false : set);
}

void VtdUnit::ObserveFirmwareLocked() noexcept
{
    if (m_observed)
    {
        return;
    }
    m_version = m_registers.Read32(VersionRegister);
    m_capability = m_registers.Read64(CapabilityRegister);
    m_extendedCapability = m_registers.Read64(ExtendedCapabilityRegister);
    m_handoffStatus = m_registers.Read32(StatusRegister);
    // Reported before any write, so the screen and the diagnostic channel
    // show the state the firmware handed over.
    const VtdFirmwareState state{m_handoffStatus, m_registers.Read32(FaultStatusRegister),
                                 m_registers.Read64(QueueAddressRegister), m_registers.Read64(QueueHeadRegister),
                                 m_registers.Read64(QueueTailRegister)};
    m_registers.ReportHandoff(state);
    m_observed = true;
}

VtdHandoffFailure VtdUnit::GetHandoffFailure() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return m_handoffFailure;
}

bool VtdUnit::RefuseFirmwareLocked(const VtdHandoffFailure reason) noexcept
{
    m_handoffFailure = reason;
    FailLocked();
    m_registers.ReportRefusal(reason);
    return false;
}

VtdUnitError VtdUnit::HandoffErrorLocked() const noexcept
{
    switch (m_handoffFailure)
    {
    case VtdHandoffFailure::QueueTimeout:
    case VtdHandoffFailure::WaitTimeout:
    case VtdHandoffFailure::CommandTimeout:
    case VtdHandoffFailure::ProtectedMemoryTimeout:
        return VtdUnitError::TimedOut;
    case VtdHandoffFailure::None:
    case VtdHandoffFailure::QueueError:
    case VtdHandoffFailure::InvalidationTimeout:
    case VtdHandoffFailure::MalformedQueue:
    case VtdHandoffFailure::FirmwareMemory:
        return VtdUnitError::Failed;
    }
    return VtdUnitError::Failed;
}

bool VtdUnit::QuiesceQueueLocked() noexcept
{
    // VT-d revision 4.0 sections 11.4.7.2 (FECTL, masked by the caller) and
    // 11.4.9.5 (IECTL): IM is bit 31 and the other bits are RO or RsvdP, so
    // both event sources are masked by setting IM in the value read.
    m_registers.Write32(InvalidationEventControlRegister,
                        m_registers.Read32(InvalidationEventControlRegister) | FaultInterruptMask);
    constexpr UInt32 queueError{1U << 4U};
    constexpr UInt32 invalidationTimeout{1U << 6U};
    const UInt32 faults = m_registers.Read32(FaultStatusRegister);
    // Sections 6.5.2 and 6.5.2.10, FSTS in 11.4.7.1: while IQE or ITE is
    // set the unit fetches no descriptor, so no wait could complete. Neither
    // error is cleared (that would restart firmware's queue); the unit is
    // Given up as the firmware left it.
    if ((faults & queueError) != 0U)
    {
        return RefuseFirmwareLocked(VtdHandoffFailure::QueueError);
    }
    if ((faults & invalidationTimeout) != 0U)
    {
        return RefuseFirmwareLocked(VtdHandoffFailure::InvalidationTimeout);
    }
    const UInt64 address = m_registers.Read64(QueueAddressRegister);
    const UInt64 bytes = UInt64{4096U} << (address & 7U);
    const UInt64 width = ((address & 0x800U) != 0U) ? 32U : 16U;
    const UInt64 tail = m_registers.Read64(QueueTailRegister);
    const UInt64 base = address & ~UInt64{0xFFFU};
    // Sections 11.4.9.1-11.4.9.3: IQA holds a 4 KiB-aligned base, DW (bit
    // 11, 256-bit descriptors) and QS (2^QS pages); bits 10:3 are RsvdZ.
    // IQH/IQT are descriptor-aligned offsets into the queue. Section 6.5.2.8:
    // the queue, which holds the status target below, must be routable
    // memory, never the FEEx_xxxxh interrupt range.
    if (base == 0U || base >= (UInt64{1U} << 52U) || bytes > ((UInt64{1U} << 52U) - base) || (address & 0x7F8U) != 0U ||
        (base < 0xFEF0'0000U && base + bytes > 0xFEE0'0000U) || tail >= bytes || (tail & (width - 1U)) != 0U)
    {
        return RefuseFirmwareLocked(VtdHandoffFailure::MalformedQueue);
    }
    bool empty{false};
    for (Size spin{0U}; spin < SpinBound; ++spin)
    {
        const UInt64 head = m_registers.Read64(QueueHeadRegister);
        if (head >= bytes || (head & (width - 1U)) != 0U)
        {
            return RefuseFirmwareLocked(VtdHandoffFailure::MalformedQueue);
        }
        if (head == tail)
        {
            empty = true;
            break;
        }
        const UInt32 status = m_registers.Read32(FaultStatusRegister);
        if ((status & (queueError | invalidationTimeout)) != 0U)
        {
            return RefuseFirmwareLocked((status & queueError) != 0U ? VtdHandoffFailure::QueueError
                                                                    : VtdHandoffFailure::InvalidationTimeout);
        }
    }
    if (!empty)
    {
        return RefuseFirmwareLocked(VtdHandoffFailure::QueueTimeout);
    }
    // Section 6.5.2: the queue is quiesced when it is empty and the last
    // completed descriptor is an Invalidation Wait Descriptor. Read-only
    // proof first (section 6.5.2.8, figure 6-8: type 5 in bits 11:9 and
    // 3:0, SW in bit 5, status data in bits 63:32, status address in bits
    // 127:66): the descriptor before the tail is such a wait with SW=1 and
    // its status address already holds its status data.
    const UInt64 previous = base + ((tail + bytes - width) % bytes);
    UInt32 fields[4U]{};
    bool readable{true};
    for (Size word{0U}; word < 4U; ++word)
    {
        readable = m_registers.ReadQueueWord(previous + word * 4U, fields[word]) && readable;
    }
    if (readable && (fields[0U] & 0xE0FU) == 5U && (fields[0U] & 0x20U) != 0U)
    {
        const UInt64 statusAddress = (UInt64{fields[3U]} << 32U) | fields[2U];
        UInt32 completed{0U};
        if ((statusAddress & 3U) == 0U && statusAddress < (UInt64{1U} << 52U) &&
            !(statusAddress >= 0xFEE0'0000U && statusAddress < 0xFEF0'0000U) &&
            m_registers.ReadQueueWord(statusAddress, completed) && completed == fields[1U])
        {
            return true;
        }
    }
    const UInt64 slot = base + tail;
    // Otherwise a wait is appended, but only where the whole queue and the
    // status target lie in memory the firmware owned and the kernel does not
    // (VtdFirmwareQueueRangeIsWritable): Asteria policy, so that neither the
    // descriptor write nor the unit's status write can touch kernel memory.
    if (!m_registers.CanWriteFirmwareMemory(base, bytes) || !m_registers.CanWriteFirmwareMemory(slot, 4U))
    {
        return RefuseFirmwareLocked(VtdHandoffFailure::FirmwareMemory);
    }
    UInt32 initial{0U};
    if (!m_registers.ReadQueueWord(slot, initial))
    {
        return RefuseFirmwareLocked(VtdHandoffFailure::FirmwareMemory);
    }
    // Section 6.5.2.8: Type=5, SW=1, status data=1, IF=0, FN=0; a 256-bit
    // queue (DW=1) takes 128 zero bits of padding. The descriptor's own slot
    // is the dword-aligned status target: the unit fetches the descriptor
    // before it can complete it, and the queue is disabled afterwards.
    for (UInt64 offset{0U}; offset < width; offset += 4U)
    {
        const UInt32 value =
            offset == 0U ? 0x25U
                         : (offset == 4U ? 1U
                                         : (offset == 8U ? static_cast<UInt32>(slot)
                                                         : (offset == 12U ? static_cast<UInt32>(slot >> 32U) : 0U)));
        if (!m_registers.WriteQueueWord(slot + offset, value))
        {
            return RefuseFirmwareLocked(VtdHandoffFailure::FirmwareMemory);
        }
    }
    // The words went through the uncached physical window; order them
    // before the tail update that lets the unit fetch them (section 6.5.2).
    m_cache.Fence();
    const UInt64 next = (tail + width) % bytes;
    m_registers.Write64(QueueTailRegister, next);
    for (Size spin{0U}; spin < SpinBound; ++spin)
    {
        const UInt32 status = m_registers.Read32(FaultStatusRegister);
        if ((status & (queueError | invalidationTimeout)) != 0U)
        {
            return RefuseFirmwareLocked((status & queueError) != 0U ? VtdHandoffFailure::QueueError
                                                                    : VtdHandoffFailure::InvalidationTimeout);
        }
        UInt32 completed{0U};
        if (!m_registers.ReadQueueWord(slot, completed))
        {
            return RefuseFirmwareLocked(VtdHandoffFailure::FirmwareMemory);
        }
        if (completed == 1U && m_registers.Read64(QueueHeadRegister) == next)
        {
            return true;
        }
    }
    return RefuseFirmwareLocked(VtdHandoffFailure::WaitTimeout);
}

void VtdUnit::CopyHandoffRecord(VtdHandoffRecord& record) const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    const UInt64 base = record.RegisterBase;
    record = m_handoffRecord;
    record.RegisterBase = base;
}

bool VtdUnit::ResetFirmwareLocked() noexcept
{
    const bool taken = ResetFirmwareStateLocked();
    // Read once after the last handoff command, including a refusal before any command. Never use
    // Enabled/Failed from a later Configure as the takeover outcome. VT-d rev. 3.4 sections
    // 10.4.5/6/9/15, 10.4.21-23, 10.4.30/33.
    m_handoffRecord.Outcome = taken ? FirmwareHandoffOutcome::TakenOver : FirmwareHandoffOutcome::Refused;
    m_handoffRecord.Reason = static_cast<Foundation::UInt8>(m_handoffFailure);
    m_handoffRecord.Version = m_version;
    m_handoffRecord.Status = m_registers.Read32(StatusRegister);
    m_handoffRecord.FaultStatus = m_registers.Read32(FaultStatusRegister);
    m_handoffRecord.QueueAddress = m_registers.Read64(QueueAddressRegister);
    m_handoffRecord.QueueHead = m_registers.Read64(QueueHeadRegister);
    m_handoffRecord.QueueTail = m_registers.Read64(QueueTailRegister);
    m_handoffRecord.RootAddress = m_registers.Read64(RootTableRegister);
    m_handoffRecord.InterruptTable = m_registers.Read64(0xB8U);
    m_handoffRecord.PageRequestQueue = m_registers.Read64(0xD0U);
    m_handoffRecord.AdvancedFaultLog = ((m_version >> 4U) < 4U) ? m_registers.Read64(0x58U) : 0U;
    return taken;
}

bool VtdUnit::ResetFirmwareStateLocked() noexcept
{
    m_registers.Write32(FaultEventControlRegister, m_registers.Read32(FaultEventControlRegister) | FaultInterruptMask);
    // Section 6.5.2: QIE may be cleared only once the queue is quiesced, and
    // the unit fetches descriptors independently of TES and IRES, so the
    // queue is settled first. Every queue refusal therefore happens before
    // any command: TE, QIE, IRE and the root stay as the firmware left them.
    const bool queued = (m_registers.Read32(StatusRegister) & QueuedInvalidation) != 0U;
    if (queued && !QuiesceQueueLocked())
    {
        return false;
    }
    if (((m_registers.Read32(StatusRegister) & TranslationEnable) != 0U) && !CommandLocked(TranslationEnable, false))
    {
        return RefuseFirmwareLocked(VtdHandoffFailure::CommandTimeout);
    }
    if (queued && !CommandLocked(QueuedInvalidation, false))
    {
        return RefuseFirmwareLocked(VtdHandoffFailure::CommandTimeout);
    }
    if (((m_registers.Read32(StatusRegister) & InterruptRemapping) != 0U) && !CommandLocked(InterruptRemapping, false))
    {
        return RefuseFirmwareLocked(VtdHandoffFailure::CommandTimeout);
    }
    // Advanced fault logging: VT-d revision 3.4 section 10.4.4 defines EAFL
    // (GCMD bit 28, reported by GSTS.AFLS); revision 4.0 removed the feature
    // (revision history) and reserves bits 29:28, which then read zero. CFI
    // is left alone: revision 4.0 sections 11.4.4.1/11.4.4.2 make it
    // effective only with interrupt remapping on and x2APIC mode off, so its
    // status need not change once IRE is clear.
    constexpr UInt32 advancedFaultLog{UInt32{1U} << 28U};
    if (((m_registers.Read32(StatusRegister) & advancedFaultLog) != 0U) && !CommandLocked(advancedFaultLog, false))
    {
        return RefuseFirmwareLocked(VtdHandoffFailure::CommandTimeout);
    }
    // Revision 4.0 section 11.4.8.1: clear EPM (bit 31, other bits RsvdP
    // or RO), then wait for PRS (bit 0) to report the regions disabled.
    if ((m_capability & CapabilityProtectedMemory) != 0U)
    {
        constexpr UInt32 enable{UInt32{1U} << 31U};
        m_registers.Write32(ProtectedMemoryRegister, m_registers.Read32(ProtectedMemoryRegister) & ~enable);
        for (Size spin{0U}; spin < SpinBound; ++spin)
        {
            if ((m_registers.Read32(ProtectedMemoryRegister) & 1U) == 0U)
            {
                return true;
            }
        }
        return RefuseFirmwareLocked(VtdHandoffFailure::ProtectedMemoryTimeout);
    }
    return true;
}

bool VtdUnit::WaitForStatusLocked(const UInt32 bit, const bool set) noexcept
{
    for (Size spin{0U}; spin < SpinBound; ++spin)
    {
        if (((m_registers.Read32(StatusRegister) & bit) != 0U) == set)
        {
            return true;
        }
    }
    FailLocked();
    return false;
}

bool VtdUnit::WaitForClearLocked(const Size offset) noexcept
{
    for (Size spin{0U}; spin < SpinBound; ++spin)
    {
        if ((m_registers.Read64(offset) & (UInt64{1U} << 63U)) == 0U)
        {
            return true;
        }
    }
    FailLocked();
    return false;
}

bool VtdUnit::InvalidateContextLocked(const UInt64 granularity) noexcept
{
    if (!FlushWritesLocked())
    {
        return false;
    }
    m_registers.Write64(ContextCommandRegister, InvalidateContext | granularity);
    return WaitForClearLocked(ContextCommandRegister);
}

bool VtdUnit::InvalidateIotlbLocked(const UInt64 granularity) noexcept
{
    const UInt64 drain = (((m_capability & CapabilityDrainReads) != 0U) ? IotlbDrainReads : 0U) |
                         (((m_capability & CapabilityDrainWrites) != 0U) ? IotlbDrainWrites : 0U);
    m_registers.Write64(m_iotlbOffset, InvalidateIotlb | granularity | drain);
    return WaitForClearLocked(m_iotlbOffset);
}

VtdUnit::Requester* VtdUnit::FindLocked(const UInt16 requester) noexcept
{
    for (Size index{0U}; index < m_requesterCount; ++index)
    {
        if (m_requesters[index].Identifier == requester)
        {
            return &m_requesters[index];
        }
    }
    return nullptr;
}

const VtdUnit::Requester* VtdUnit::FindLocked(const UInt16 requester) const noexcept
{
    for (Size index{0U}; index < m_requesterCount; ++index)
    {
        if (m_requesters[index].Identifier == requester)
        {
            return &m_requesters[index];
        }
    }
    return nullptr;
}

volatile UInt64* VtdUnit::ContextEntryLocked(const Requester& requester) noexcept
{
    Size slot{0U};
    while (m_buses[slot] != requester.Bus)
    {
        ++slot;
    }
    return m_contexts[slot].Entries + (2U * (requester.Identifier & 0xFFU));
}

void VtdUnit::PublishLocked(const volatile void* const start, const Size bytes) noexcept
{
    // ECAP.C clear (VT-d revision 1.3 section 10.4.3): the unit's root,
    // context and page-table reads do not snoop, so the lines are written
    // back first. The fence is unconditional: a snooping unit still needs
    // the table stores ordered before the register write that follows.
    if ((bytes != 0U) && ((m_extendedCapability & ExtendedCapabilityCoherent) == 0U))
    {
        m_cache.FlushTables(start, bytes);
    }
    m_cache.Fence();
}

void VtdUnit::PollFaultsLocked() noexcept
{
    const UInt32 status = m_registers.Read32(FaultStatusRegister);
    if ((status & PrimaryPendingFault) != 0U)
    {
        for (Size index{0U}; index < m_faultRecords; ++index)
        {
            const Size offset = m_faultOffset + (index * 16U);
            const UInt64 high = m_registers.Read64(offset + sizeof(UInt64));
            if ((high & FaultRecorded) == 0U)
            {
                continue;
            }
            const auto source = static_cast<UInt16>(high);
            m_lastFault = VtdFaultRecord{
                .Address = m_registers.Read64(offset) & TableAddressMask,
                .Source = source,
                .Reason = static_cast<Foundation::UInt8>(high >> 32U),
                .Read = (high & FaultRead) != 0U,
                .Reserved = {},
            };
            ++m_faults;
            const Requester* const served = FindLocked(source);
            if ((served != nullptr) && (served->Domain != 0U))
            {
                ++m_domainFaults[served->Domain].Count;
                m_domainFaults[served->Domain].Last = m_lastFault;
            }
            else
            {
                ++m_unattributed;
            }
            // F is write-one-to-clear; the record's other bits are read-only.
            m_registers.Write64(offset + sizeof(UInt64), FaultRecorded);
        }
    }
    if ((status & PrimaryFaultOverflow) != 0U)
    {
        ++m_overflows;
        m_registers.Write32(FaultStatusRegister, PrimaryFaultOverflow);
    }
}

void VtdUnit::FailLocked() noexcept
{
    m_failed = true;
}
} // Namespace Asteria::Platform::Device
