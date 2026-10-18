// SPDX-License-Identifier: Apache-2.0

#include "Platform/Device/VtdDomainBackend.hpp"

namespace Asteria::Platform::Device
{
namespace
{
using Foundation::Size;
using Foundation::UInt16;
using Foundation::UInt64;
using Hal::Device::IommuBackendError;
using Hal::Device::IommuBackendOwnershipError;

constexpr Size TableEntries{512U};
constexpr UInt64 PageBytes{4096U};
constexpr UInt64 WindowBytes{UInt64{1U} << 21U};
constexpr UInt64 PageOffsetMask{PageBytes - 1U};
constexpr UInt64 AddressMask{0x000F'FFFF'FFFF'F000ULL};
constexpr UInt64 EntryRead{1U};
constexpr UInt64 EntryWrite{2U};

[[nodiscard]] constexpr Size IndexAt(const UInt64 address, const Foundation::UInt8 level) noexcept
{
    return static_cast<Size>((address >> (12U + (9U * (level - 1U)))) & (TableEntries - 1U));
}

[[nodiscard]] IommuBackendError MapUnitError(const VtdUnitError error) noexcept
{
    switch (error)
    {
    case VtdUnitError::None:
        return IommuBackendError::None;
    case VtdUnitError::NotServed:
        return IommuBackendError::UnsupportedConfiguration;
    case VtdUnitError::InvalidArguments:
        return IommuBackendError::InvalidRequest;
    case VtdUnitError::NotConfigured:
        return IommuBackendError::NotReady;
    case VtdUnitError::Unsupported:
    case VtdUnitError::AlreadyActive:
    case VtdUnitError::TimedOut:
    case VtdUnitError::Failed:
        break;
    }
    return IommuBackendError::HardwareFailure;
}
} // namespace

VtdDomainBackend::VtdDomainBackend(VtdUnit& unit, const UInt16 domain, const VtdTablePage (&pages)[TablePages]) noexcept
    : VtdDomainBackend{unit, domain, pages, TablePages}
{
}

VtdDomainBackend::VtdDomainBackend(VtdUnit& unit, const UInt16 domain, const VtdTablePage* const pages,
                                   const Size count) noexcept
    : m_unit{unit}, m_domain{domain}
{
    if ((pages == nullptr) || (count == 0U) || (count > MaximumTablePages))
    {
        return;
    }
    for (Size index{0U}; index < count; ++index)
    {
        if ((pages[index].Entries == nullptr) || (pages[index].PhysicalAddress == 0U) ||
            ((pages[index].PhysicalAddress & PageOffsetMask) != 0U))
        {
            return;
        }
        m_pages[index] = pages[index];
    }
    m_pageCount = count;
}

bool VtdDomainBackend::CancelClaim(const Kernel::Device::IommuDomain& owner) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if ((m_owner != &owner) || (m_attachedCount != 0U) || (m_usedPages != 1U))
    {
        return false;
    }
    // No context or mapping was published by Claim: no hardware drain needed.
    ZeroPages();
    m_owner = nullptr;
    m_usedPages = 0U;
    return true;
}

bool VtdDomainBackend::PrepareReserved() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if ((m_owner != nullptr) || m_reserved || (m_domain <= 4U) || (m_pageCount == 0U))
    {
        return false;
    }
    m_levels = m_unit.GetConfiguredLevels();
    if (m_levels == 0U)
    {
        return false;
    }
    m_inputAddressBits = m_unit.GetAddressBits();
    ZeroPages();
    m_usedPages = 1U;
    m_reserved = true;
    return true;
}

IommuBackendError VtdDomainBackend::MapReserved(const UInt64 base, const UInt64 limit) noexcept
{
    if (!m_reserved || (base > limit) || ((base & PageOffsetMask) != 0U) ||
        ((limit & PageOffsetMask) != PageOffsetMask) || (limit == ~UInt64{0U}))
    {
        return IommuBackendError::InvalidRequest;
    }
    return Map({.DmaStart = Kernel::Device::DmaAddress{base},
                .PhysicalStart = Kernel::Memory::PhysicalAddress{base},
                .Pages = Kernel::Memory::PageCount{(limit - base + 1U) / PageBytes},
                .Permissions = Kernel::Device::DmaPermissions{Kernel::Device::DmaPermission::Read}.With(
                    Kernel::Device::DmaPermission::Write),
                .Reserved = {}});
}

VtdUnitError VtdDomainBackend::SeedReserved(const UInt16 requester) noexcept
{
    return m_reserved ? m_unit.SeedContext(requester, m_domain, m_pages[0U].PhysicalAddress)
                      : VtdUnitError::NotConfigured;
}

IommuBackendError VtdDomainBackend::CheckMap(const Hal::Device::IommuBackendMapRequest& request) const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if ((m_owner == nullptr) && !m_reserved)
    {
        return IommuBackendError::NotReady;
    }
    if (!RangeIsValid(request) || ((request.PhysicalStart.GetValue() & PageOffsetMask) != 0U))
    {
        return IommuBackendError::InvalidRequest;
    }
    return ((m_usedPages + CountMissingTables(request.DmaStart.GetValue(), request.Pages.GetValue())) > m_pageCount)
               ? IommuBackendError::CapacityExceeded
               : IommuBackendError::None;
}

bool VtdDomainBackend::IsReady() const noexcept
{
    return m_unit.IsEnabled();
}

IommuBackendOwnershipError VtdDomainBackend::Claim(const Kernel::Device::IommuDomain& owner,
                                                   const Foundation::UInt8 inputAddressBits) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if ((m_owner != nullptr) || m_reserved)
    {
        return IommuBackendOwnershipError::AlreadyOwned;
    }
    if (m_pageCount == 0U)
    {
        return IommuBackendOwnershipError::UnsupportedConfiguration;
    }
    const Foundation::UInt8 levels = m_unit.GetLevels();
    if (levels == 0U)
    {
        return IommuBackendOwnershipError::NotReady;
    }
    if ((inputAddressBits < 12U) || (inputAddressBits > 64U))
    {
        return IommuBackendOwnershipError::UnsupportedConfiguration;
    }
    // The top table exists before any requester can be attached to it.
    ZeroPages();
    m_usedPages = 1U;
    m_levels = levels;
    // The domain stores device width; the translating backend
    // separately bounds its hardware walk.
    m_inputAddressBits = inputAddressBits < m_unit.GetAddressBits() ? inputAddressBits : m_unit.GetAddressBits();
    m_owner = &owner;
    return IommuBackendOwnershipError::None;
}

IommuBackendOwnershipError VtdDomainBackend::Release(const Kernel::Device::IommuDomain& owner) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_owner != &owner)
    {
        return IommuBackendOwnershipError::InvalidOwner;
    }
    while (m_attachedCount != 0U)
    {
        if (m_unit.DetachContext(m_attached[m_attachedCount - 1U], m_domain) != VtdUnitError::None)
        {
            return IommuBackendOwnershipError::HardwareFailure;
        }
        --m_attachedCount;
    }
    if (m_unit.InvalidateDomain(m_domain) != VtdUnitError::None)
    {
        return IommuBackendOwnershipError::HardwareFailure;
    }
    ZeroPages();
    m_usedPages = 0U;
    m_owner = nullptr;
    return IommuBackendOwnershipError::None;
}

IommuBackendError VtdDomainBackend::Map(const Hal::Device::IommuBackendMapRequest& request) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if ((m_owner == nullptr) && !m_reserved)
    {
        return IommuBackendError::NotReady;
    }
    if (!RangeIsValid(request) || ((request.PhysicalStart.GetValue() & PageOffsetMask) != 0U))
    {
        return IommuBackendError::InvalidRequest;
    }
    const UInt64 start = request.DmaStart.GetValue();
    const UInt64 pages = request.Pages.GetValue();
    if ((m_usedPages + CountMissingTables(start, pages)) > m_pageCount)
    {
        return IommuBackendError::CapacityExceeded;
    }
    const UInt64 permissions = (request.Permissions.Contains(Kernel::Device::DmaPermission::Read) ? EntryRead : 0U) |
                               (request.Permissions.Contains(Kernel::Device::DmaPermission::Write) ? EntryWrite : 0U);
    for (UInt64 page{0U}; page < pages; ++page)
    {
        volatile UInt64* const leaf = FindLeaf(start + (page * PageBytes), true);
        if ((leaf == nullptr) ||
            ((!m_reserved || (*leaf != ((start + (page * PageBytes)) | permissions))) && (*leaf != 0U)))
        {
            return IommuBackendError::CorruptState;
        }
        *leaf = (request.PhysicalStart.GetValue() + (page * PageBytes)) | permissions;
        m_unit.Publish(leaf, sizeof(UInt64));
    }
    return (m_unit.FlushWrites() == VtdUnitError::None) ? IommuBackendError::None : IommuBackendError::HardwareFailure;
}

IommuBackendError VtdDomainBackend::Unmap(const Hal::Device::IommuBackendMapRequest& request) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_owner == nullptr)
    {
        return IommuBackendError::NotReady;
    }
    if (!RangeIsValid(request))
    {
        return IommuBackendError::InvalidRequest;
    }
    const UInt64 start = request.DmaStart.GetValue();
    const UInt64 pages = request.Pages.GetValue();
    for (UInt64 page{0U}; page < pages; ++page)
    {
        volatile UInt64* const leaf = FindLeaf(start + (page * PageBytes), false);
        if ((leaf == nullptr) || (*leaf == 0U))
        {
            return IommuBackendError::CorruptState;
        }
    }
    for (UInt64 page{0U}; page < pages; ++page)
    {
        volatile UInt64* const leaf = FindLeaf(start + (page * PageBytes), false);
        *leaf = 0U;
        m_unit.Publish(leaf, sizeof(UInt64));
    }
    return (m_unit.InvalidateDomain(m_domain) == VtdUnitError::None) ? IommuBackendError::None
                                                                     : IommuBackendError::HardwareFailure;
}

IommuBackendError VtdDomainBackend::Attach(Hal::Device::IIommuDevice& device) noexcept
{
    // The requester identifier is read before this backend's lock: the
    // Endpoint's lock ranks beside it.
    const Foundation::UInt32 requester = device.GetRequesterId();
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_owner == nullptr)
    {
        return IommuBackendError::NotReady;
    }
    if (((requester >> 16U) != 0U) || !m_unit.Serves(static_cast<UInt16>(requester)))
    {
        return IommuBackendError::UnsupportedConfiguration;
    }
    if (m_attachedCount == MaximumAttachments)
    {
        return IommuBackendError::CapacityExceeded;
    }
    const IommuBackendError attached =
        MapUnitError(m_unit.AttachContext(static_cast<UInt16>(requester), m_domain, m_pages[0U].PhysicalAddress));
    if (attached == IommuBackendError::None)
    {
        m_attached[m_attachedCount] = static_cast<UInt16>(requester);
        ++m_attachedCount;
    }
    return attached;
}

IommuBackendError VtdDomainBackend::Detach(Hal::Device::IIommuDevice& device) noexcept
{
    const Foundation::UInt32 requester = device.GetRequesterId();
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    Size index{0U};
    while ((index < m_attachedCount) && (m_attached[index] != static_cast<UInt16>(requester)))
    {
        ++index;
    }
    if ((m_owner == nullptr) || ((requester >> 16U) != 0U) || (index == m_attachedCount))
    {
        return IommuBackendError::InvalidRequest;
    }
    const IommuBackendError detached = MapUnitError(m_unit.DetachContext(m_attached[index], m_domain));
    if (detached == IommuBackendError::None)
    {
        m_attached[index] = m_attached[m_attachedCount - 1U];
        --m_attachedCount;
    }
    return (detached == IommuBackendError::None) ? detached : IommuBackendError::HardwareFailure;
}

Hal::Device::IommuBackendFaults VtdDomainBackend::GetFaults() noexcept
{
    m_unit.PollFaults();
    const VtdDomainFaults faults = m_unit.GetDomainFaults(m_domain);
    return Hal::Device::IommuBackendFaults{.Count = faults.Count,
                                           .LastSource = faults.Last.Source,
                                           .LastReason = faults.Last.Reason,
                                           .Reserved = {},
                                           .LastAddress = faults.Last.Address};
}

Size VtdDomainBackend::GetUsedPages() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return m_usedPages;
}

bool VtdDomainBackend::RangeIsValid(const Hal::Device::IommuBackendMapRequest& request) const noexcept
{
    const UInt64 start = request.DmaStart.GetValue();
    const UInt64 pages = request.Pages.GetValue();
    const UInt64 limit = UInt64{1U} << m_inputAddressBits;
    return ((start & PageOffsetMask) == 0U) && (pages != 0U) && (start < limit) &&
           (pages <= (m_reserved ? (m_pageCount * TableEntries) : 4096U)) && (pages <= ((limit - start) / PageBytes)) &&
           request.Permissions.HasOnlyKnownBits() && !request.Permissions.IsEmpty();
}

// Counts the tables a mapping of [start, start + pages) would add, walking
// each 2 MiB window it touches. Windows are visited in address order, so a
// missing table is counted once, at the first window that needs it.
Size VtdDomainBackend::CountMissingTables(const UInt64 start, const UInt64 pages) const noexcept
{
    UInt64 lastCounted[5U]{~UInt64{0U}, ~UInt64{0U}, ~UInt64{0U}, ~UInt64{0U}, ~UInt64{0U}};
    Size missing{0U};
    const UInt64 end = start + (pages * PageBytes);
    for (UInt64 window = start & ~(WindowBytes - 1U); window < end; window += WindowBytes)
    {
        volatile UInt64* table = m_pages[0U].Entries;
        for (Foundation::UInt8 level = m_levels; level > 1U; --level)
        {
            const UInt64 entry = (table == nullptr) ? 0U : table[IndexAt(window, level)];
            table = ((entry & (EntryRead | EntryWrite)) != 0U) ? FindTable(entry & AddressMask) : nullptr;
            if (table == nullptr)
            {
                const UInt64 prefix = window >> (12U + (9U * (level - 1U)));
                if (lastCounted[level - 1U] != prefix)
                {
                    lastCounted[level - 1U] = prefix;
                    ++missing;
                }
            }
        }
    }
    return missing;
}

volatile UInt64* VtdDomainBackend::FindTable(const UInt64 physicalAddress) const noexcept
{
    for (Size index{0U}; index < m_usedPages; ++index)
    {
        if (m_pages[index].PhysicalAddress == physicalAddress)
        {
            return m_pages[index].Entries;
        }
    }
    return nullptr;
}

volatile UInt64* VtdDomainBackend::Descend(volatile UInt64* const table, const Size index, const bool allocate) noexcept
{
    const UInt64 entry = table[index];
    if ((entry & (EntryRead | EntryWrite)) != 0U)
    {
        return FindTable(entry & AddressMask);
    }
    if (!allocate || (m_usedPages == m_pageCount))
    {
        return nullptr;
    }
    // Pages past m_usedPages are zero; an intermediate entry grants both
    // directions, the leaf decides.
    const VtdTablePage& page = m_pages[m_usedPages];
    ++m_usedPages;
    table[index] = page.PhysicalAddress | EntryRead | EntryWrite;
    m_unit.Publish(&table[index], sizeof(UInt64));
    return page.Entries;
}

volatile UInt64* VtdDomainBackend::FindLeaf(const UInt64 address, const bool allocate) noexcept
{
    volatile UInt64* table = m_pages[0U].Entries;
    for (Foundation::UInt8 level = m_levels; (level > 1U) && (table != nullptr); --level)
    {
        table = Descend(table, IndexAt(address, level), allocate);
    }
    return (table == nullptr) ? nullptr : &table[IndexAt(address, 1U)];
}

void VtdDomainBackend::ZeroPages() noexcept
{
    for (Size pageIndex{0U}; pageIndex < m_pageCount; ++pageIndex)
    {
        const VtdTablePage& page = m_pages[pageIndex];
        for (Size index{0U}; index < TableEntries; ++index)
        {
            page.Entries[index] = 0U;
        }
        m_unit.Publish(page.Entries, TableEntries * sizeof(UInt64));
    }
}
} // Namespace Asteria::Platform::Device
