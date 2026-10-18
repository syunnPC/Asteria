// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TEST_SUPPORT_KERNEL_ELF_IMAGE_HPP_INCLUDED
#define ASTERIA_TEST_SUPPORT_KERNEL_ELF_IMAGE_HPP_INCLUDED

#include "Boot/KernelElf.hpp"
#include "Boot/BootInfo.hpp"

namespace Asteria::TestSupport
{
/// Synthetic ELF gABI/psABI input, containing no executable project or third-
/// party code. Independent bit-field encoding makes relocation expectations
/// observable without invoking a production encoder.
class KernelElfImage final
{
public:
    static constexpr Foundation::Size Bytes{0x9000U};
    static constexpr Foundation::UInt64 LinkedBase{0x4000000U};
    Foundation::UInt8 Data[Bytes]{};

    void Put(Foundation::Size offset, Foundation::UInt64 value, Foundation::Size bytes = 8U) noexcept
    {
        for (Foundation::Size index{0U}; index < bytes; ++index)
        {
            Data[offset + index] = static_cast<Foundation::UInt8>(value & 0xFFU);
            value >>= 8U;
        }
    }

    [[nodiscard]] Foundation::UInt64 Get(Foundation::Size offset, Foundation::Size bytes = 8U) const noexcept
    {
        Foundation::UInt64 value{0U};
        for (Foundation::Size index{0U}; index < bytes; ++index)
        {
            value |= static_cast<Foundation::UInt64>(Data[offset + index]) << (8U * index);
        }
        return value;
    }

    void Build(Foundation::UInt32 architecture, Foundation::UInt64 linkedBase = LinkedBase) noexcept
    {
        m_linkedBase = linkedBase;
        for (auto& byte : Data)
        {
            byte = 0U;
        }
        const bool ia64 = architecture == Boot::BootArchitectureIa64;
        Put(0U, 0x464C457FU, 4U);
        Data[4U] = 2U;
        Data[5U] = 1U;
        Data[6U] = 1U;
        Put(16U, 2U, 2U);
        Put(18U, ia64 ? 50U : 62U, 2U);
        Put(20U, 1U, 4U);
        Put(24U, m_linkedBase + (ia64 ? 0x802010U : 0x1000U));
        Put(32U, 64U);
        Put(40U, 0x800U);
        Put(48U, ia64 ? 0x10U : 0U, 4U);
        Put(52U, 64U, 2U);
        Put(54U, 56U, 2U);
        Put(56U, ia64 ? 3U : 5U, 2U);
        Put(58U, 64U, 2U);
        Put(60U, ia64 ? 12U : 13U, 2U);
        Put(62U, ia64 ? 11U : 12U, 2U);
        if (ia64)
        {
            BuildIa64();
        }
        else
        {
            BuildAmd64();
        }
        Note(architecture);
    }

    [[nodiscard]] Executable::Elf::Elf64ByteView View() const noexcept
    {
        return {Data, Bytes};
    }

private:
    Foundation::UInt64 m_linkedBase{LinkedBase};

    void Program(Foundation::Size index, Foundation::UInt64 offset, Foundation::UInt64 rva,
                 Foundation::UInt64 fileBytes, Foundation::UInt64 memoryBytes, Foundation::UInt32 flags) noexcept
    {
        const Foundation::Size field = 64U + index * 56U;
        Put(field, 1U, 4U);
        Put(field + 4U, flags, 4U);
        Put(field + 8U, offset);
        Put(field + 16U, m_linkedBase + rva);
        Put(field + 24U, m_linkedBase + rva);
        Put(field + 32U, fileBytes);
        Put(field + 40U, memoryBytes);
        Put(field + 48U, 4096U);
    }

    void Section(Foundation::Size index, Foundation::UInt32 type, Foundation::UInt64 flags, Foundation::UInt64 rva,
                 Foundation::UInt64 offset, Foundation::UInt64 bytes, Foundation::UInt32 link = 0U,
                 Foundation::UInt32 target = 0U, Foundation::UInt64 stride = 0U) noexcept
    {
        const Foundation::Size field = 0x800U + index * 64U;
        Put(field + 4U, type, 4U);
        Put(field + 8U, flags);
        Put(field + 16U, (flags & 2U) != 0U ? m_linkedBase + rva : 0U);
        Put(field + 24U, offset);
        Put(field + 32U, bytes);
        Put(field + 40U, link, 4U);
        Put(field + 44U, target, 4U);
        Put(field + 48U, 1U);
        Put(field + 56U, stride);
    }

    void Symbol(Foundation::Size index, Foundation::UInt64 rva, Foundation::UInt16 section) noexcept
    {
        const Foundation::Size field = 0x7000U + index * 24U;
        Data[field + 4U] = 0x12U;
        Put(field + 6U, section, 2U);
        Put(field + 8U, m_linkedBase + rva);
    }

    void Relocation(Foundation::Size offset, Foundation::UInt64 rva, Foundation::UInt32 type, Foundation::UInt32 symbol,
                    Foundation::UInt64 addend = 0U) noexcept
    {
        Put(offset, m_linkedBase + rva);
        Put(offset + 8U, (static_cast<Foundation::UInt64>(symbol) << 32U) | type);
        Put(offset + 16U, addend);
    }

    void Ownership(Foundation::Size index, Foundation::UInt64 rva, Foundation::UInt64 pages,
                   Foundation::UInt32 purpose) noexcept
    {
        const Foundation::Size field = 0x2000U + 116U + index * 24U;
        Put(field, rva);
        Put(field + 8U, pages);
        Put(field + 16U, purpose, 4U);
    }

    void Reservation(Foundation::Size index, Foundation::UInt64 pages, Foundation::UInt64 alignment,
                     Foundation::UInt64 ceiling, Foundation::UInt32 kind, Foundation::UInt32 flags) noexcept
    {
        const Foundation::Size field = 0x2000U + 284U + index * 32U;
        Put(field, pages);
        Put(field + 8U, alignment);
        Put(field + 16U, ceiling);
        Put(field + 24U, kind, 4U);
        Put(field + 28U, flags, 4U);
    }

    void Note(Foundation::UInt32 architecture) noexcept
    {
        const bool ia64 = architecture == Boot::BootArchitectureIa64;
        Put(0x2000U, 8U, 4U);
        Put(0x2004U, 360U, 4U);
        Put(0x2008U, 0x41535401U, 4U);
        Put(0x200CU, 0x0041495245545341ULL);
        Put(0x2014U, 1U, 4U);
        Put(0x2018U, Boot::BootInfoVersion, 4U);
        Put(0x201CU, architecture, 4U);
        Put(0x2020U, ia64 ? 6U : 7U, 4U);
        Put(0x2024U, m_linkedBase);
        Put(0x202CU, ia64 ? 0x1000000U : 0x8000U);
        Put(0x2034U, ia64 ? 0x1000000U : 0x200000U);
        Put(0x203CU, ia64 ? 0x802000U : 0x1000U);
        Put(0x2044U, ia64 ? 0x802010U : 0x1000U);
        Put(0x204CU, ia64 ? 0x802020U : 0x1000U);
        if (ia64)
        {
            Put(0x2054U, 0x880000U);
            Put(0x205CU, 0x802000U);
            Put(0x2064U, 48U);
            Ownership(0U, 0x400000U, 1024U, 0U);
            Ownership(1U, 0x800000U, 1U, 1U);
            Ownership(2U, 0x801000U, 1022U, 1U);
            Ownership(3U, 0xBFF000U, 1U, 1U);
            Ownership(4U, 0xC00000U, 1U, 2U);
            Ownership(5U, 0xC01000U, 1023U, 2U);
            Reservation(0U, 32U, 1U, 0xFFFFFFFFU, 0U, 1U);
            Reservation(1U, 4096U, 4096U, 0xFFFFFFFFU, 3U, 2U);
        }
        else
        {
            constexpr Foundation::UInt32 Purposes[]{0U, 1U, 1U, 2U, 1U, 1U, 2U};
            for (Foundation::Size index{0U}; index < 7U; ++index)
            {
                Ownership(index, (index + 1U) * 4096U, 1U, Purposes[index]);
            }
            Reservation(0U, 23U, 1U, ~Foundation::UInt64{0U}, 0U, 0U);
            Reservation(1U, 3849U, 1U, 0xFFFFFFFFU, 1U, 0U);
            Reservation(2U, 2U, 1U, 0xFFFFFU, 2U, 0U);
        }
    }

    void BuildAmd64() noexcept
    {
        Program(0U, 0x1000U, 0x1000U, 0x1000U, 0x1000U, 5U);
        Program(1U, 0x2000U, 0x2000U, 0x2000U, 0x2000U, 4U);
        Program(2U, 0x4000U, 0x4000U, 0U, 0x1000U, 6U);
        Program(3U, 0x4000U, 0x5000U, 0x2000U, 0x2000U, 4U);
        Program(4U, 0x6000U, 0x7000U, 0x1000U, 0x1000U, 6U);
        Section(1U, 1U, 6U, 0x1000U, 0x1000U, 0x1000U);
        Section(2U, 7U, 2U, 0x2000U, 0x2000U, 0x1000U);
        Section(3U, 1U, 2U, 0x3000U, 0x3000U, 0x1000U);
        Section(4U, 8U, 3U, 0x4000U, 0x4000U, 0x1000U);
        Section(5U, 1U, 2U, 0x5000U, 0x4000U, 0x1000U);
        Section(6U, 1U, 2U, 0x6000U, 0x5000U, 0x1000U);
        Section(7U, 1U, 3U, 0x7000U, 0x6000U, 0x1000U);
        Section(8U, 2U, 0U, 0U, 0x7000U, 96U, 9U, 0U, 24U);
        Section(9U, 3U, 0U, 0U, 0x7060U, 1U);
        Section(10U, 4U, 0U, 0U, 0x7100U, 48U, 8U, 3U, 24U);
        Section(11U, 4U, 0U, 0U, 0x7200U, 24U, 8U, 1U, 24U);
        Section(12U, 3U, 0U, 0U, 0x7060U, 1U);
        Symbol(1U, 0x1000U, 1U);
        Symbol(2U, 0x7000U, 7U);
        Relocation(0x7100U, 0x3000U, 1U, 1U);
        Relocation(0x7118U, 0x3008U, 8U, 0U, m_linkedBase + 0x7080U);
        Relocation(0x7200U, 0x1080U, 2U, 1U, 0x100U);
        Put(0x3000U, m_linkedBase + 0x1000U);
        Put(0x3008U, m_linkedBase + 0x7080U);
        Put(0x1080U, 0x80U, 4U);
    }

    void Bit(Foundation::Size byteOffset, Foundation::Size bit, bool value) noexcept
    {
        const auto mask = static_cast<Foundation::UInt8>(1U << (bit % 8U));
        auto& byte = Data[byteOffset + bit / 8U];
        byte = value ? static_cast<Foundation::UInt8>(byte | mask) : static_cast<Foundation::UInt8>(byte & ~mask);
    }

    [[nodiscard]] static Foundation::Size ImmediateField(const Foundation::Size bit) noexcept
    {
        if (bit < 7U)
        {
            return 100U + bit;
        }
        if (bit < 16U)
        {
            return 114U + bit - 7U;
        }
        if (bit < 21U)
        {
            return 109U + bit - 16U;
        }
        if (bit == 21U)
        {
            return 108U;
        }
        if (bit < 63U)
        {
            return 46U + bit - 22U;
        }
        return 123U;
    }

    void ImmediateBundle(Foundation::Size offset, Foundation::UInt64 value) noexcept
    {
        Put(offset, 4U);
        Put(offset + 8U, Foundation::UInt64{6U} << 60U);
        for (Foundation::Size bit{0U}; bit < 64U; ++bit)
        {
            const Foundation::Size field = ImmediateField(bit);
            Bit(offset, field, ((value >> bit) & 1U) != 0U);
        }
    }

    void BuildIa64() noexcept
    {
        Program(0U, 0x1000U, 0x400000U, 0x1000U, 0x400000U, 5U);
        Program(1U, 0x2000U, 0x800000U, 0x3000U, 0x400000U, 4U);
        Program(2U, 0x5000U, 0xC00000U, 0x1000U, 0x400000U, 6U);
        Section(1U, 1U, 6U, 0x400000U, 0x1000U, 0x1000U);
        Section(2U, 7U, 2U, 0x800000U, 0x2000U, 0x1000U);
        Section(3U, 1U, 2U, 0x801000U, 0x3000U, 0x1000U);
        Section(4U, 1U, 2U, 0x802000U, 0x4000U, 0x1000U);
        Section(5U, 1U, 3U, 0xC00000U, 0x5000U, 0x1000U);
        Section(6U, 8U, 3U, 0xC01000U, 0x6000U, 0x3FF000U);
        Section(7U, 2U, 0U, 0U, 0x7000U, 96U, 8U, 0U, 24U);
        Section(8U, 3U, 0U, 0U, 0x7060U, 1U);
        Section(9U, 4U, 0U, 0U, 0x7100U, 72U, 7U, 3U, 24U);
        Section(10U, 4U, 0U, 0U, 0x7200U, 96U, 7U, 1U, 24U);
        Section(11U, 3U, 0U, 0U, 0x7060U, 1U);
        Symbol(1U, 0x400000U, 1U);
        Symbol(2U, 0x880000U, 0xFFF1U);
        Symbol(3U, 0xC00000U, 5U);
        for (Foundation::Size index{0U}; index < 3U; ++index)
        {
            Put(0x4000U + index * 16U, m_linkedBase + 0x400000U + index * 16U);
            Put(0x4008U + index * 16U, m_linkedBase + 0x880000U);
        }
        Relocation(0x7100U, 0x801000U, 0x27U, 1U);
        Relocation(0x7118U, 0x801008U, 0x47U, 1U);
        Relocation(0x7130U, 0x801010U, 0x81U, 1U);
        Relocation(0x7200U, 0x400021U, 0x23U, 3U, 0x2BB357U);
        Relocation(0x7218U, 0x400080U, 0x49U, 1U, 0x100U);
        Relocation(0x7230U, 0x4000C0U, 0x2AU, 2U, 0x100U);
        Relocation(0x7248U, 0x4000E1U, 0x2BU, 2U, 0x200U);
        Put(0x3000U, m_linkedBase + 0x400000U);
        Put(0x3008U, m_linkedBase + 0x802000U);
        Put(0x3010U, m_linkedBase + 0x400000U);
        Put(0x3018U, m_linkedBase + 0x880000U);
        ImmediateBundle(0x1020U, m_linkedBase + 0xEBB357U);
        Put(0x1080U, Foundation::UInt64{8U} << 18U);
        Put(0x10C0U, Foundation::UInt64{2U} << 32U);
        ImmediateBundle(0x10E0U, 0x200U);
    }
};
} // namespace Asteria::TestSupport

#endif // ASTERIA_TEST_SUPPORT_KERNEL_ELF_IMAGE_HPP_INCLUDED
