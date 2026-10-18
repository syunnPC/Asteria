// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TEST_SUPPORT_STATIC_PIE_IMAGE_HPP_INCLUDED
#define ASTERIA_TEST_SUPPORT_STATIC_PIE_IMAGE_HPP_INCLUDED

#include "User/Loader/ExecutableFormats.hpp"

namespace Asteria::TestSupport
{
[[nodiscard]] inline Foundation::UInt64 PieLoad(const Foundation::UInt8* const bytes) noexcept
{
    Foundation::UInt64 value{0U};
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        value |= static_cast<Foundation::UInt64>(bytes[index]) << (index * 8U);
    }
    return value;
}

inline void PieStore(Foundation::UInt8* const bytes, const Foundation::UInt64 value,
                     const Foundation::Size count = 8U) noexcept
{
    for (Foundation::Size index{0U}; index < count; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>((value >> (index * 8U)) & 0xFFU);
    }
}

[[nodiscard]] inline User::Loader::MachineRules PieRules(const bool ia64) noexcept
{
    return User::Loader::MachineRules{
        .Machine = ia64 ? User::Loader::ExecutableMachine::Ia64 : User::Loader::ExecutableMachine::Amd64,
        .UserStart = ia64 ? 0x2000'0000'0000'1000ULL : 0x1000U,
        .UserLimit = ia64 ? 0x2000'0000'8000'0000ULL : 0x0000'8000'0000'0000ULL,
        .EntryDescriptor = ia64,
        .MaximumSegments = 8U,
        .MaximumSegmentPages = 32U,
        .WritableFileBytes = true,
        .PositionIndependentImages = true,
    };
}

struct StaticPieImage
{
    Foundation::UInt8 Bytes[4096U]{};

    explicit StaticPieImage(const bool ia64 = false) noexcept
    {
        Bytes[0U] = 0x7FU;
        Bytes[1U] = 'E';
        Bytes[2U] = 'L';
        Bytes[3U] = 'F';
        Bytes[4U] = 2U;
        Bytes[5U] = 1U;
        Bytes[6U] = 1U;
        PieStore(Bytes + 16U, 3U, 2U);
        PieStore(Bytes + 18U, ia64 ? 50U : 62U, 2U);
        PieStore(Bytes + 20U, 1U, 4U);
        PieStore(Bytes + 24U, ia64 ? 0x2B00U : 0x300U);
        PieStore(Bytes + 32U, 64U);
        PieStore(Bytes + 48U, ia64 ? 0x10U : 0U, 4U);
        PieStore(Bytes + 52U, 64U, 2U);
        PieStore(Bytes + 54U, 56U, 2U);
        PieStore(Bytes + 56U, 3U, 2U);
        Header(0U, 1U, 5U, 0U, 0U, 0x500U, 0x500U);
        Header(1U, 1U, 6U, 0x800U, 0x2800U, 0x800U, 0x800U);
        Header(2U, 2U, 6U, 0x800U, 0x2800U, 0x100U, 0x100U);
        Tag(0U, 7U, 0x2900U);
        Tag(1U, 8U, 24U);
        Tag(2U, 9U, 24U);
        Tag(3U, 0x6FFF'FFF9U, 1U);
        Tag(4U, 0x6FFF'FFFBU, 0x0800'0001U);
        Tag(5U, 6U, 0x2A00U);
        Tag(6U, 11U, 24U);
        Tag(7U, 5U, 0x2A80U);
        Tag(8U, 10U, 16U);
        Tag(9U, 3U, 0x2C00U);
        Rela(0U, 0x2B20U, ia64 ? 0x6FU : 8U, 0x2C00U);
        PieStore(Bytes + 0xB00U, 0x300U);
        PieStore(Bytes + 0xB08U, 0x2C00U);
        // Dynamic symbol 1: a defined local function, with a bounded name.
        Bytes[0xA18U + 4U] = 2U;
        PieStore(Bytes + 0xA18U + 6U, 1U, 2U);
        PieStore(Bytes + 0xA18U + 8U, 0x300U);
    }

    void Header(const Foundation::Size number, const Foundation::UInt64 type, const Foundation::UInt64 flags,
                const Foundation::UInt64 offset, const Foundation::UInt64 address, const Foundation::UInt64 fileBytes,
                const Foundation::UInt64 memoryBytes) noexcept
    {
        Foundation::UInt8* const header = Bytes + 64U + number * 56U;
        PieStore(header, type, 4U);
        PieStore(header + 4U, flags, 4U);
        PieStore(header + 8U, offset);
        PieStore(header + 16U, address);
        PieStore(header + 32U, fileBytes);
        PieStore(header + 40U, memoryBytes);
        PieStore(header + 48U, 4096U);
    }

    void Tag(const Foundation::Size index, const Foundation::UInt64 tag, const Foundation::UInt64 value) noexcept
    {
        PieStore(Bytes + 0x800U + index * 16U, tag);
        PieStore(Bytes + 0x808U + index * 16U, value);
    }

    void Rela(const Foundation::Size index, const Foundation::UInt64 address, const Foundation::UInt64 info,
              const Foundation::UInt64 addend) noexcept
    {
        PieStore(Bytes + 0x900U + index * 24U, address);
        PieStore(Bytes + 0x908U + index * 24U, info);
        PieStore(Bytes + 0x910U + index * 24U, addend);
    }
};
} // namespace Asteria::TestSupport

#endif // ASTERIA_TEST_SUPPORT_STATIC_PIE_IMAGE_HPP_INCLUDED
