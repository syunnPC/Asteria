// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Device/AmdViUnit.hpp"

namespace Asteria::Platform::Amd64::Device
{
namespace
{
using Foundation::Size;
using Foundation::UInt16;
using Foundation::UInt64;
constexpr UInt64 AddressMask{0x000F'FFFF'FFFF'F000ULL};
constexpr UInt64 ReadWrite{UInt64{3U} << 61U};
constexpr UInt64 Blocked{3U}; // V/TV, mode zero, IR/IW clear: no page-zero walk.
constexpr UInt64 InterruptPass{UInt64{1U} << 60U};
constexpr UInt64 Control{1U | 4U | (UInt64{1U} << 10U) | (UInt64{1U} << 12U)};
// Preserve the SMI filter, timeout and reserved fields. Turn off every
// optional translation/logging/interrupt engine before taking its storage.
constexpr UInt64 EngineMask{0x3'F01FU | (UInt64{1U} << 25U) | (UInt64{3U} << 28U) | (UInt64{3U} << 43U) |
                            (UInt64{1U} << 45U) | (UInt64{3U} << 50U) | (UInt64{1U} << 58U) | (UInt64{1U} << 61U)};
constexpr UInt64 RingBytes{8192U};
constexpr Size SpinBound{1'000'000U};
constexpr Size ControlOffset{0x18U};
constexpr Size CommandHead{0x2000U};
constexpr Size CommandTail{0x2008U};
constexpr Size EventHead{0x2010U};
constexpr Size EventTail{0x2018U};
constexpr Size Status{0x2020U};

[[nodiscard]] bool ValidPage(const VtdTablePage& page) noexcept
{
    return (page.Entries != nullptr) && ((reinterpret_cast<Foundation::UIntPointer>(page.Entries) & 0xFFFU) == 0U) &&
           (page.PhysicalAddress != 0U) && ((page.PhysicalAddress & ~AddressMask) == 0U);
}
} // namespace

bool AmdViUnit::Disable() noexcept
{
    if (!m_observed)
    {
        m_snapshot.HandoffStatus = static_cast<Foundation::UInt32>(m_registers.Read64(ControlOffset));
        m_observed = true;
    }
    // Stop translation first, then all firmware command/event/guest engines.
    m_registers.Write64(ControlOffset, m_registers.Read64(ControlOffset) & ~UInt64{1U});
    m_cache.Fence();
    m_registers.Write64(ControlOffset, m_registers.Read64(ControlOffset) & ~EngineMask);
    for (Size spin{0U}; spin < SpinBound; ++spin)
    {
        if (((m_registers.Read64(ControlOffset) & 1U) == 0U) && ((m_registers.Read64(Status) & 0x198U) == 0U))
        {
            m_registers.Write64(0x20U, 0U); // No global firmware exclusion bypass.
            CaptureHandoff(true);
            return true;
        }
    }
    CaptureHandoff(false);
    m_snapshot.Failed = true;
    return false;
}

void AmdViUnit::CaptureHandoff(const bool takenOver) noexcept
{
    m_handoff.Outcome = takenOver ? Platform::Device::FirmwareHandoffOutcome::TakenOver
                                  : Platform::Device::FirmwareHandoffOutcome::Refused;
    m_handoff.Reason = takenOver ? 0U : static_cast<Foundation::UInt8>(VtdUnitError::TimedOut);
    m_handoff.Control = m_registers.Read64(ControlOffset);
    m_handoff.Status = m_registers.Read64(Status);
    constexpr Size offsets[AmdViHandoffRecord::BaseCount]{0U, 0x100U, 0x108U, 0x110U, 0x118U, 0x120U, 0x128U, 0x130U,
                                                          8U, 0x10U,  0x38U,  0xE0U,  0xE8U,  0xF0U,  0xF8U};
    for (Size index{0U}; index < AmdViHandoffRecord::BaseCount; ++index)
    {
        m_handoff.Bases[index] = m_registers.Read64(offsets[index]);
    }
    m_handoff.CommandHead = m_registers.Read64(CommandHead);
    m_handoff.CommandTail = m_registers.Read64(CommandTail);
    m_handoff.EventHead = m_registers.Read64(EventHead);
    m_handoff.EventTail = m_registers.Read64(EventTail);
    constexpr Size secondary[AmdViHandoffRecord::SecondaryPointerCount]{0x2030U, 0x2038U, 0x2040U, 0x2048U,
                                                                        0x2050U, 0x2058U, 0x2070U, 0x2078U};
    for (Size index{0U}; index < AmdViHandoffRecord::SecondaryPointerCount; ++index)
    {
        m_handoff.SecondaryPointers[index] = m_registers.Read64(secondary[index]);
    }
}
void AmdViUnit::CopyHandoffRecord(AmdViHandoffRecord& output) const noexcept
{
    Synchronization::SpinLockGuard guard{m_lock};
    const UInt64 base{output.RegisterBase};
    output = m_handoff;
    output.RegisterBase = base;
}

VtdUnitError AmdViUnit::PrepareDisabled() noexcept
{
    Synchronization::SpinLockGuard guard{m_lock};
    if (m_snapshot.Enabled)
    {
        return VtdUnitError::AlreadyActive;
    }
    return Disable() ? VtdUnitError::None : VtdUnitError::TimedOut;
}

VtdUnitError AmdViUnit::Configure(const VtdTablePage devices, const Size devicePages, const VtdTablePage commands,
                                  const VtdTablePage events, const VtdTablePage completion,
                                  const UInt16* const requesters, const Size count,
                                  const Foundation::UInt8 ivhdFlags) noexcept
{
    Synchronization::SpinLockGuard guard{m_lock};
    if (m_snapshot.Enabled || m_configured)
    {
        return VtdUnitError::AlreadyActive;
    }
    if (!ValidPage(devices) || !ValidPage(commands) || !ValidPage(events) || !ValidPage(completion) ||
        (devicePages == 0U) || (devicePages > MaximumDevicePages) || (requesters == nullptr) ||
        (count > MaximumRequesters))
    {
        return VtdUnitError::InvalidArguments;
    }
    if (!Disable())
    {
        return VtdUnitError::TimedOut;
    }
    m_snapshot.ExtendedCapability = m_registers.Read64(0x30U);
    m_controlFlags = ((ivhdFlags & 2U) != 0U ? UInt64{1U} << 8U : 0U) |
                     ((ivhdFlags & 4U) != 0U ? UInt64{1U} << 9U : 0U) |
                     ((ivhdFlags & 8U) != 0U ? UInt64{1U} << 11U : 0U);
    if (((m_snapshot.ExtendedCapability >> 10U) & 3U) == 3U)
    {
        return VtdUnitError::Unsupported;
    }
    for (Size index{0U}; index < count; ++index)
    {
        if (requesters[index] >= (devicePages * 128U))
        {
            return VtdUnitError::InvalidArguments;
        }
        for (Size previous{0U}; previous < index; ++previous)
        {
            if (requesters[previous] == requesters[index])
            {
                return VtdUnitError::InvalidArguments;
            }
        }
    }
    m_devices = devices;
    m_devicePages = devicePages;
    m_commands = commands;
    m_events = events;
    m_completion = completion;
    m_count = count;
    for (Size index{0U}; index < count; ++index)
    {
        m_requesters[index] = {requesters[index], 0U};
    }
    // V=TV=1, mode zero and no IR/IW: every in-range RID is blocked, including
    // absent functions and the IOMMU's own function (never attachable).
    for (Size index{0U}; index < devicePages * 128U; ++index)
    {
        devices.Entries[index * 4U] = Blocked;
        devices.Entries[index * 4U + 1U] = 0U;
        devices.Entries[index * 4U + 2U] = InterruptPass;
        devices.Entries[index * 4U + 3U] = 0U;
    }
    for (Size index{0U}; index < RingBytes / sizeof(UInt64); ++index)
    {
        commands.Entries[index] = 0U;
        events.Entries[index] = 0U;
    }
    completion.Entries[0U] = 0U;
    PublishLocked(devices.Entries, devicePages * 4096U);
    PublishLocked(commands.Entries, RingBytes);
    PublishLocked(events.Entries, RingBytes);
    PublishLocked(completion.Entries, sizeof(UInt64));
    m_snapshot.Levels = 4U;
    m_snapshot.Version = 1U;
    m_snapshot.Served = count;
    m_configured = true;
    return VtdUnitError::None;
}

VtdUnitError AmdViUnit::Enable() noexcept
{
    Synchronization::SpinLockGuard guard{m_lock};
    if (!m_configured || m_snapshot.Failed)
    {
        return VtdUnitError::NotConfigured;
    }
    if (m_snapshot.Enabled)
    {
        return VtdUnitError::AlreadyActive;
    }
    m_registers.Write64(0U, m_devices.PhysicalAddress | (m_devicePages - 1U));
    m_registers.Write64(8U, m_commands.PhysicalAddress | (UInt64{9U} << 56U));
    m_registers.Write64(16U, m_events.PhysicalAddress | (UInt64{9U} << 56U));
    m_registers.Write64(CommandHead, 0U);
    m_registers.Write64(CommandTail, 0U);
    m_registers.Write64(EventHead, 0U);
    m_registers.Write64(EventTail, 0U);
    m_registers.Write64(Status, 7U);
    m_registers.Write64(ControlOffset,
                        (m_registers.Read64(ControlOffset) & ~(EngineMask | 0xF00U)) | Control | m_controlFlags);
    m_snapshot.Enabled = (m_registers.Read64(ControlOffset) & 1U) != 0U;
    // Flush cached firmware DTEs (including absent RIDs), interrupt tables,
    // and every domain ID this implementation may use, before assignment.
    if ((m_snapshot.ExtendedCapability & (UInt64{1U} << 6U)) != 0U)
    {
        if (!Queue(UInt64{8U} << 60U, 0U) || !Complete())
        {
            return VtdUnitError::TimedOut;
        }
        return m_snapshot.Enabled ? VtdUnitError::None : VtdUnitError::Failed;
    }
    for (Size requester{0U}; requester < m_devicePages * 128U; ++requester)
    {
        if (!Queue((UInt64{2U} << 60U) | requester, 0U) || !Queue((UInt64{5U} << 60U) | requester, 0U) ||
            (((requester % 64U) == 63U) && !Complete()))
        {
            return VtdUnitError::TimedOut;
        }
    }
    for (UInt16 domain{0U}; domain <= MaximumDomain; ++domain)
    {
        if (!Invalidate(domain))
        {
            return VtdUnitError::TimedOut;
        }
    }
    return m_snapshot.Enabled ? VtdUnitError::None : VtdUnitError::Failed;
}

bool AmdViUnit::Queue(const UInt64 low, const UInt64 high) noexcept
{
    const UInt64 next = (m_tail + 16U) % RingBytes;
    for (Size spin{0U}; spin < SpinBound; ++spin)
    {
        if (m_registers.Read64(CommandHead) != next)
        {
            m_commands.Entries[m_tail / 8U] = low;
            m_commands.Entries[m_tail / 8U + 1U] = high;
            PublishLocked(m_commands.Entries + m_tail / 8U, 16U);
            m_tail = next;
            m_registers.Write64(CommandTail, m_tail);
            return true;
        }
    }
    m_snapshot.Failed = true;
    return false;
}

bool AmdViUnit::Complete() noexcept
{
    ++m_sequence; // Unsigned wrap is safe: only one completion is outstanding.
    if (!Queue((UInt64{1U} << 60U) | m_completion.PhysicalAddress | 1U, m_sequence))
    {
        return false;
    }
    for (Size spin{0U}; spin < SpinBound; ++spin)
    {
        m_cache.Flush(m_completion.Entries);
        m_cache.Fence();
        if (m_completion.Entries[0U] == m_sequence)
        {
            return true;
        }
    }
    m_snapshot.Failed = true;
    return false;
}

bool AmdViUnit::Invalidate(const UInt16 domain) noexcept
{
    return Queue((UInt64{3U} << 60U) | (UInt64{domain} << 32U), 0x7FFF'FFFF'FFFF'F003ULL) &&
           Complete(); // S=1, PDE=1; completion drains earlier translated DMA.
}

AmdViUnit::Requester* AmdViUnit::Find(const UInt16 requester) noexcept
{
    for (Size index{0U}; index < m_count; ++index)
    {
        if (m_requesters[index].Identifier == requester)
        {
            return &m_requesters[index];
        }
    }
    return nullptr;
}

VtdUnitError AmdViUnit::Install(const UInt16 requester, const UInt16 domain, const UInt64 topTable,
                                const Foundation::UInt8 levels, const bool firmware) noexcept
{
    Requester* entry = Find(requester);
    if ((requester >= m_devicePages * 128U) || ((entry == nullptr) && !firmware))
    {
        return VtdUnitError::NotServed;
    }
    if ((domain == 0U) || (domain > MaximumDomain) || ((topTable & ~AddressMask) != 0U) || (topTable == 0U) ||
        (levels == 0U) || (levels > 4U) || ((entry != nullptr) && (entry->Domain != 0U)) ||
        ((m_devices.Entries[Size{requester} * 4U + 1U] & 0xFFFFU) != 0U))
    {
        return VtdUnitError::InvalidArguments;
    }
    volatile UInt64* dte = m_devices.Entries + (Size{requester} * 4U);
    dte[1U] = domain;
    PublishLocked(dte + 1U, sizeof(UInt64));
    dte[0U] = topTable | 3U | (UInt64{levels} << 9U) | ReadWrite;
    PublishLocked(dte, sizeof(UInt64));
    if (entry != nullptr)
    {
        entry->Domain = domain;
    }
    ++m_snapshot.Attached;
    return VtdUnitError::None;
}

VtdUnitError AmdViUnit::SeedContext(const UInt16 requester, const UInt16 domain, const UInt64 topTable,
                                    const Foundation::UInt8 levels) noexcept
{
    Synchronization::SpinLockGuard guard{m_lock};
    if (!m_configured || m_snapshot.Enabled || m_snapshot.Failed)
    {
        return VtdUnitError::NotConfigured;
    }
    return Install(requester, domain, topTable, levels, true);
}

VtdUnitError AmdViUnit::AttachContext(const UInt16 requester, const UInt16 domain, const UInt64 topTable,
                                      const Foundation::UInt8 levels) noexcept
{
    Synchronization::SpinLockGuard guard{m_lock};
    if (!m_snapshot.Enabled || m_snapshot.Failed)
    {
        return VtdUnitError::Failed;
    }
    const VtdUnitError result = Install(requester, domain, topTable, levels);
    if (result != VtdUnitError::None)
    {
        return result;
    }
    return (Queue((UInt64{2U} << 60U) | requester, 0U) && Invalidate(domain)) ? VtdUnitError::None
                                                                              : VtdUnitError::TimedOut;
}

VtdUnitError AmdViUnit::DetachContext(const UInt16 requester, const UInt16 domain) noexcept
{
    Synchronization::SpinLockGuard guard{m_lock};
    Requester* entry = Find(requester);
    if ((entry == nullptr) || (entry->Domain != domain) || (domain == 0U))
    {
        return VtdUnitError::InvalidArguments;
    }
    PollLocked(); // Attribute pending faults before dropping the domain owner.
    volatile UInt64* dte = m_devices.Entries + (Size{requester} * 4U);
    dte[0U] = Blocked;
    PublishLocked(dte, sizeof(UInt64));
    // Hardware failure cannot prevent revoking the in-memory context.
    // Keep its tables/domain owned until a completion can prove DMA drained.
    if (!m_snapshot.Enabled || m_snapshot.Failed)
    {
        return VtdUnitError::Failed;
    }
    if (!Queue((UInt64{2U} << 60U) | requester, 0U) || !Invalidate(domain))
    {
        return VtdUnitError::TimedOut;
    }
    dte[1U] = 0U;
    PublishLocked(dte + 1U, sizeof(UInt64));
    entry->Domain = 0U;
    --m_snapshot.Attached;
    return VtdUnitError::None;
}

VtdUnitError AmdViUnit::InvalidateDomain(const UInt16 domain) noexcept
{
    Synchronization::SpinLockGuard guard{m_lock};
    if (!m_snapshot.Enabled || m_snapshot.Failed)
    {
        return VtdUnitError::Failed;
    }
    if ((domain == 0U) || (domain > MaximumDomain))
    {
        return VtdUnitError::InvalidArguments;
    }
    return Invalidate(domain) ? VtdUnitError::None : VtdUnitError::TimedOut;
}

VtdUnitError AmdViUnit::FlushWrites() noexcept
{
    Synchronization::SpinLockGuard guard{m_lock};
    m_cache.Fence();
    return m_snapshot.Failed ? VtdUnitError::Failed : VtdUnitError::None;
}

void AmdViUnit::PublishLocked(const volatile void* const start, const Size bytes) noexcept
{
    // Conservatively flush even on coherent hardware. CPU WB, IOMMU coherent
    // control and the fence together order table/ring writes before MMIO.
    const Foundation::UIntPointer first =
        reinterpret_cast<Foundation::UIntPointer>(start) & ~Foundation::UIntPointer{63U};
    const Foundation::UIntPointer end = reinterpret_cast<Foundation::UIntPointer>(start) + bytes;
    for (Foundation::UIntPointer line{first}; line < end; line += 64U)
    {
        m_cache.Flush(reinterpret_cast<const volatile void*>(line));
    }
    m_cache.Fence();
}

void AmdViUnit::Publish(const volatile void* const start, const Size bytes) noexcept
{
    Synchronization::SpinLockGuard guard{m_lock};
    PublishLocked(start, bytes);
}

void AmdViUnit::PollLocked() noexcept
{
    if (!m_configured)
    {
        return;
    }
    const bool overflow = (m_registers.Read64(Status) & 1U) != 0U;
    if (overflow)
    {
        ++m_snapshot.Overflows;
        // AMD 48882 rev 3.09 section 2.5.1: EventLogRun must be zero
        // before recovery changes the log registers. Capture the final tail
        // only after that transition, so the last completed entries are read.
        bool stopped{false};
        for (Size poll{0U}; poll < SpinBound; ++poll)
        {
            if ((m_registers.Read64(Status) & 8U) == 0U)
            {
                stopped = true;
                break;
            }
        }
        if (!stopped)
        {
            m_snapshot.Failed = true;
            return;
        }
    }
    UInt64 head = m_registers.Read64(EventHead);
    const UInt64 tail = m_registers.Read64(EventTail);
    if ((head >= RingBytes) || (tail >= RingBytes) || ((head & 15U) != 0U) || ((tail & 15U) != 0U))
    {
        m_snapshot.Failed = true;
        return;
    }
    for (Size count{0U}; (head != tail) && (count < 512U); ++count)
    {
        volatile UInt64* entry = m_events.Entries + head / 8U;
        // AMD 48882 rev 3.09 section 2.5: hardware advances the tail
        // AFTER writing a record. Software consumes up to that tail and
        // returns ownership by advancing the head, without rewriting RAM.
        m_cache.Flush(entry);
        m_cache.Fence();
        const UInt64 low = entry[0U];
        const auto type = static_cast<Foundation::UInt8>((low >> 60U) & 15U);
        const UInt64 address = entry[1U];
        if ((type == 2U) || (type == 7U) || (type == 8U))
        {
            const auto source = static_cast<UInt16>(low & 0xFFFFU);
            m_snapshot.LastFault = {.Address = address,
                                    .Source = source,
                                    .Reason = type,
                                    .Read = (low & (UInt64{1U} << 53U)) == 0U,
                                    .Reserved = {}};
            ++m_snapshot.Faults;
            const Requester* requester = Find(source);
            const UInt16 domain = (requester != nullptr)
                                      ? requester->Domain
                                      : ((source < m_devicePages * 128U)
                                             ? static_cast<UInt16>(m_devices.Entries[Size{source} * 4U + 1U] & 0xFFFFU)
                                             : UInt16{0U});
            if ((domain != 0U) && (domain <= MaximumDomain))
            {
                ++m_faults[domain].Count;
                m_faults[domain].Last = m_snapshot.LastFault;
            }
            else
            {
                ++m_snapshot.Unattributed;
            }
        }
        else if ((type >= 3U) && (type <= 6U))
        {
            m_snapshot.Failed = true;
        }
        head = (head + 16U) % RingBytes;
        m_registers.Write64(EventHead, head);
    }
    if (overflow)
    {
        // Section 2.5.1: disable EventLogEn, retain the now-drained ring,
        // acknowledge only EventOverflow (RW1C), then enable logging.
        const UInt64 control = m_registers.Read64(ControlOffset);
        m_registers.Write64(ControlOffset, control & ~UInt64{4U});
        m_registers.Write64(Status, 1U);
        m_registers.Write64(ControlOffset, control | UInt64{4U});
    }
}

void AmdViUnit::PollFaults() noexcept
{
    Synchronization::SpinLockGuard guard{m_lock};
    PollLocked();
}

VtdDomainFaults AmdViUnit::GetDomainFaults(const UInt16 domain) const noexcept
{
    Synchronization::SpinLockGuard guard{m_lock};
    return (domain <= MaximumDomain) ? m_faults[domain] : VtdDomainFaults{};
}

VtdUnitSnapshot AmdViUnit::GetSnapshot() const noexcept
{
    Synchronization::SpinLockGuard guard{m_lock};
    VtdUnitSnapshot snapshot = m_snapshot;
    snapshot.Status = static_cast<Foundation::UInt32>(m_registers.Read64(ControlOffset));
    return snapshot;
}

bool AmdViUnit::IsEnabled() const noexcept
{
    Synchronization::SpinLockGuard guard{m_lock};
    return m_snapshot.Enabled && !m_snapshot.Failed;
}

Foundation::UInt8 AmdViUnit::GetLevels() const noexcept
{
    return IsEnabled() ? Foundation::UInt8{4U} : Foundation::UInt8{0U};
}

Foundation::UInt8 AmdViUnit::GetConfiguredLevels() const noexcept
{
    Synchronization::SpinLockGuard guard{m_lock};
    return (m_configured && !m_snapshot.Failed) ? Foundation::UInt8{4U} : Foundation::UInt8{0U};
}

Foundation::UInt8 AmdViUnit::GetAddressBits() const noexcept
{
    return 48U;
}

bool AmdViUnit::Serves(const UInt16 requester) const noexcept
{
    Synchronization::SpinLockGuard guard{m_lock};
    for (Size index{0U}; index < m_count; ++index)
    {
        if (m_requesters[index].Identifier == requester)
        {
            return true;
        }
    }
    return false;
}
} // namespace Asteria::Platform::Amd64::Device
