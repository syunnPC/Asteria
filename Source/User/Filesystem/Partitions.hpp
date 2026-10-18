// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_FILESYSTEM_PARTITIONS_HPP_INCLUDED
#define ASTERIA_USER_FILESYSTEM_PARTITIONS_HPP_INCLUDED
#include "User/Filesystem/Fat/FatVolume.hpp"
namespace Asteria::User::Filesystem
{
// UEFI Specification 2.11 sections 5.2/5.3: legacy MBR, protective MBR, GPT
// header and partition entries. Read-only, 512-byte media. No untrusted table can cause an
// unbounded scan or a write to the medium.
class Partitions final
{
public:
    static constexpr Foundation::Size Capacity{128U};
    struct Entry
    {
        Foundation::UInt64 First{0U};
        Foundation::UInt64 Sectors{0U};
        bool Esp{false};
    };
    // UEFI 2.11 section 5.2.3, tables 5.3/5.4: one protective record,
    // three zero records, starting at LBA 1 and covering the remaining disk.
    // Boot code is opaque; CHS and BootIndicator are not used for addressing.
    [[nodiscard]] static bool IsProtectiveMbr(const Foundation::UInt8* sector, Foundation::UInt64 sectors) noexcept
    {
        if (sectors < 2U || sector[510U] != 0x55U || sector[511U] != 0xAAU)
        {
            return false;
        }
        const auto size = sectors - 1U > 0xFFFF'FFFFU ? 0xFFFF'FFFFU : sectors - 1U;
        bool found{false};
        for (Foundation::Size n{0U}; n < 4U; ++n)
        {
            const auto* entry = sector + 446U + n * 16U;
            if (entry[4U] == 0xEEU)
            {
                if (found || U32(entry + 8U) != 1U || U32(entry + 12U) != size)
                {
                    return false;
                }
                found = true;
            }
            else
            {
                for (Foundation::Size at{0U}; at < 16U; ++at)
                {
                    if (entry[at] != 0U)
                    {
                        return false;
                    }
                }
            }
        }
        return found;
    }
    [[nodiscard]] FatStatus Read(const FatBlockDevice& device) noexcept
    {
        m_device = device;
        m_count = 0U;
        if (!Load(0U))
        {
            return FatStatus::Failed;
        }
        if (m_sector[510U] != 0x55U || m_sector[511U] != 0xAAU)
        {
            return FatStatus::NotFound;
        }
        bool protective{false};
        for (Foundation::Size n{0U}; n < 4U; ++n)
        {
            protective = protective || m_sector[446U + n * 16U + 4U] == 0xEEU;
        }
        if (protective)
        {
            return IsProtectiveMbr(m_sector, device.Sectors) ? Gpt() : FatStatus::Corrupt;
        }
        Foundation::UInt64 extended{0U};
        Foundation::UInt64 extendedEnd{0U};
        for (Foundation::Size n{0U}; n < 4U; ++n)
        {
            const auto* e = m_sector + 446U + n * 16U;
            const auto type = e[4U];
            const auto first = U32(e + 8U);
            const auto size = U32(e + 12U);
            if (type == 0U)
            {
                continue;
            }
            if (type == 5U || type == 0xFU || type == 0x85U)
            {
                if (extended != 0U || first == 0U || size == 0U || first >= m_device.Sectors ||
                    size > m_device.Sectors - first)
                {
                    return FatStatus::Corrupt;
                }
                extended = first;
                extendedEnd = Foundation::UInt64{first} + size;
            }
            else if (!Add(first, size, type == 0xEFU))
            {
                return FatStatus::Corrupt;
            }
        }
        for (Foundation::Size n{0U}; extended != 0U && n < m_count; ++n)
        {
            if (m_entries[n].First < extendedEnd && extended < m_entries[n].First + m_entries[n].Sectors)
            {
                return FatStatus::Corrupt;
            }
        }
        Foundation::UInt64 next = extended;
        for (Foundation::Size step{0U}; next != 0U && step < Capacity; ++step)
        {
            if (next < extended || next >= extendedEnd)
            {
                return FatStatus::Corrupt;
            }
            if (!Load(next))
            {
                return FatStatus::Failed;
            }
            if (m_sector[510U] != 0x55U || m_sector[511U] != 0xAAU)
            {
                return FatStatus::Corrupt;
            }
            const auto* first = m_sector + 446U;
            const auto* link = first + 16U;
            if (first[4U] != 0U &&
                (next + U32(first + 8U) >= extendedEnd || U32(first + 12U) > extendedEnd - (next + U32(first + 8U)) ||
                 !Add(next + U32(first + 8U), U32(first + 12U), first[4U] == 0xEFU)))
            {
                return FatStatus::Corrupt;
            }
            const auto offset = U32(link + 8U);
            if (link[4U] == 0U)
            {
                next = 0U;
            }
            else if ((link[4U] == 5U || link[4U] == 0xFU || link[4U] == 0x85U) && offset != 0U)
            {
                const auto following = extended + offset;
                if (following <= next)
                {
                    return FatStatus::Corrupt;
                }
                next = following;
            }
            else
            {
                return FatStatus::Corrupt;
            }
        }
        return next != 0U ? FatStatus::Corrupt : (m_count != 0U ? FatStatus::Done : FatStatus::NotFound);
    }
    [[nodiscard]] Foundation::Size Count() const noexcept
    {
        return m_count;
    }
    [[nodiscard]] const Entry& Get(Foundation::Size index) const noexcept
    {
        return m_entries[index];
    }

private:
    [[nodiscard]] static Foundation::UInt32 U32(const Foundation::UInt8* p) noexcept
    {
        Foundation::UInt32 v{0U};
        for (Foundation::Size n{0U}; n < 4U; ++n)
        {
            v |= Foundation::UInt32{p[n]} << (n * 8U);
        }
        return v;
    }
    [[nodiscard]] static Foundation::UInt64 U64(const Foundation::UInt8* p) noexcept
    {
        return U32(p) | (Foundation::UInt64{U32(p + 4U)} << 32U);
    }
    [[nodiscard]] static Foundation::UInt32 Crc(Foundation::UInt32 sum, Foundation::UInt8 byte) noexcept
    {
        sum ^= byte;
        for (Foundation::Size n{0U}; n < 8U; ++n)
        {
            sum = (sum >> 1U) ^ ((sum & 1U) != 0U ? 0xEDB88320U : 0U);
        }
        return sum;
    }
    [[nodiscard]] bool Load(Foundation::UInt64 sector) noexcept
    {
        return sector < m_device.Sectors && m_device.ReadSectors(m_device.Context, sector, 1U, m_sector);
    }
    [[nodiscard]] bool Add(Foundation::UInt64 first, Foundation::UInt64 size, bool esp) noexcept
    {
        if (first == 0U || size == 0U || first >= m_device.Sectors || size > m_device.Sectors - first ||
            m_count == Capacity)
        {
            return false;
        }
        for (Foundation::Size n{0U}; n < m_count; ++n)
        {
            const auto& e = m_entries[n];
            if (first < e.First + e.Sectors && e.First < first + size)
            {
                return false;
            }
        }
        m_entries[m_count++] = {first, size, esp};
        return true;
    }
    [[nodiscard]] FatStatus Gpt() noexcept
    {
        if (!Load(1U))
        {
            return FatStatus::Failed;
        }
        constexpr Foundation::UInt8 magic[]{'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'};
        for (Foundation::Size n{0U}; n < 8U; ++n)
        {
            if (m_sector[n] != magic[n])
            {
                return FatStatus::Corrupt;
            }
        }
        const auto bytes = U32(m_sector + 12U);
        const auto expected = U32(m_sector + 16U);
        const auto backup = U64(m_sector + 32U);
        const auto first = U64(m_sector + 40U);
        const auto last = U64(m_sector + 48U);
        const auto table = U64(m_sector + 72U);
        const auto count = U32(m_sector + 80U);
        const auto entryBytes = U32(m_sector + 84U);
        const auto tableCrc = U32(m_sector + 88U);
        if (U32(m_sector + 8U) != 0x10000U || bytes < 92U || bytes > 512U || U32(m_sector + 20U) != 0U ||
            U64(m_sector + 24U) != 1U || backup != m_device.Sectors - 1U || first > last || last >= backup ||
            table < 2U || count == 0U || count > Capacity || entryBytes != 128U || table >= first ||
            ((Foundation::UInt64{count} * entryBytes + 511U) >> 9U) > first - table)
        {
            return FatStatus::Corrupt;
        }
        Foundation::UInt32 sum{~0U};
        for (Foundation::Size n{0U}; n < bytes; ++n)
        {
            sum = Crc(sum, n >= 16U && n < 20U ? 0U : m_sector[n]);
        }
        if (~sum != expected)
        {
            return FatStatus::Corrupt;
        }
        constexpr Foundation::UInt8 esp[16U]{0x28U, 0x73U, 0x2AU, 0xC1U, 0x1FU, 0xF8U, 0xD2U, 0x11U,
                                             0xBAU, 0x4BU, 0x00U, 0xA0U, 0xC9U, 0x3EU, 0xC9U, 0x3BU};
        sum = ~0U;
        for (Foundation::Size n{0U}; n < count; ++n)
        {
            if ((n & 3U) == 0U && !Load(table + (n >> 2U)))
            {
                return FatStatus::Failed;
            }
            const auto* e = m_sector + (n & 3U) * 128U;
            bool used{false};
            bool system{true};
            for (Foundation::Size at{0U}; at < 128U; ++at)
            {
                sum = Crc(sum, e[at]);
                if (at < 16U)
                {
                    used = used || e[at] != 0U;
                    system = system && e[at] == esp[at];
                }
            }
            const auto begin = U64(e + 32U);
            const auto end = U64(e + 40U);
            if (used && (begin < first || end > last || begin > end || !Add(begin, end - begin + 1U, system)))
            {
                return FatStatus::Corrupt;
            }
        }
        return ~sum == tableCrc ? FatStatus::Done : FatStatus::Corrupt;
    }
    FatBlockDevice m_device{};
    Entry m_entries[Capacity]{};
    Foundation::Size m_count{0U};
    Foundation::UInt8 m_sector[FatSectorBytes]{};
};
} // Namespace Asteria::User::Filesystem
#endif
