// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_MEMORY_TYPES_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_MEMORY_TYPES_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Kernel::Memory
{
constexpr Foundation::UInt64 MappingPageSizeBytes{4096U};

class PhysicalAddress final
{
public:
    constexpr PhysicalAddress() noexcept = default;

    explicit constexpr PhysicalAddress(const Foundation::UInt64 value) noexcept : m_value{value}
    {
    }

    [[nodiscard]] constexpr Foundation::UInt64 GetValue() const noexcept
    {
        return m_value;
    }

private:
    Foundation::UInt64 m_value{0U};
};

class VirtualAddress final
{
public:
    constexpr VirtualAddress() noexcept = default;

    explicit constexpr VirtualAddress(const Foundation::UInt64 value) noexcept : m_value{value}
    {
    }

    [[nodiscard]] constexpr Foundation::UInt64 GetValue() const noexcept
    {
        return m_value;
    }

private:
    Foundation::UInt64 m_value{0U};
};

class PageCount final
{
public:
    constexpr PageCount() noexcept = default;

    explicit constexpr PageCount(const Foundation::UInt64 value) noexcept : m_value{value}
    {
    }

    [[nodiscard]] constexpr Foundation::UInt64 GetValue() const noexcept
    {
        return m_value;
    }

private:
    Foundation::UInt64 m_value{0U};
};

enum class MappingPermission : Foundation::UInt16
{
    Read = Foundation::UInt16{1U} << 0U,
    Write = Foundation::UInt16{1U} << 1U,
    Execute = Foundation::UInt16{1U} << 2U,
    Global = Foundation::UInt16{1U} << 3U,
};

class MappingPermissions final
{
public:
    constexpr MappingPermissions() noexcept = default;

    explicit constexpr MappingPermissions(const MappingPermission permission) noexcept
        : m_bits{static_cast<Foundation::UInt16>(permission)}
    {
    }

    [[nodiscard]] constexpr MappingPermissions With(const MappingPermission permission) const noexcept
    {
        return MappingPermissions{
            static_cast<Foundation::UInt16>(m_bits | static_cast<Foundation::UInt16>(permission)),
            RawValueTag{},
        };
    }

    [[nodiscard]] constexpr bool Contains(const MappingPermission permission) const noexcept
    {
        const Foundation::UInt16 bit = static_cast<Foundation::UInt16>(permission);
        return (m_bits & bit) == bit;
    }

    [[nodiscard]] constexpr bool HasOnlyKnownBits() const noexcept
    {
        return (m_bits & static_cast<Foundation::UInt16>(~KnownBits)) == 0U;
    }

    [[nodiscard]] constexpr Foundation::UInt16 GetBits() const noexcept
    {
        return m_bits;
    }

private:
    struct RawValueTag final
    {
    };

    constexpr MappingPermissions(const Foundation::UInt16 bits, RawValueTag) noexcept : m_bits{bits}
    {
    }

    static constexpr Foundation::UInt16 KnownBits = static_cast<Foundation::UInt16>(MappingPermission::Read) |
                                                    static_cast<Foundation::UInt16>(MappingPermission::Write) |
                                                    static_cast<Foundation::UInt16>(MappingPermission::Execute) |
                                                    static_cast<Foundation::UInt16>(MappingPermission::Global);

    Foundation::UInt16 m_bits{0U};
};

/// Runtime page protection requested by an AddressSpace mapper.
///
/// This is deliberately separate from MappingPermissions: bootstrap mappings
/// may be global, while a runtime mapping may instead be user-accessible.
enum class PagePermission : Foundation::UInt8
{
    Read = Foundation::UInt8{1U} << 0U,
    Write = Foundation::UInt8{1U} << 1U,
    Execute = Foundation::UInt8{1U} << 2U,
    User = Foundation::UInt8{1U} << 3U,
};

class PagePermissions final
{
public:
    constexpr PagePermissions() noexcept = default;

    explicit constexpr PagePermissions(const PagePermission permission) noexcept
        : m_bits{static_cast<Foundation::UInt8>(permission)}
    {
    }

    [[nodiscard]] constexpr PagePermissions With(const PagePermission permission) const noexcept
    {
        return PagePermissions{
            static_cast<Foundation::UInt8>(m_bits | static_cast<Foundation::UInt8>(permission)),
            RawValueTag{},
        };
    }

    [[nodiscard]] constexpr bool Contains(const PagePermission permission) const noexcept
    {
        const Foundation::UInt8 bit = static_cast<Foundation::UInt8>(permission);
        return (m_bits & bit) == bit;
    }

    [[nodiscard]] constexpr bool HasOnlyKnownBits() const noexcept
    {
        return (m_bits & static_cast<Foundation::UInt8>(~KnownBits)) == 0U;
    }

    [[nodiscard]] constexpr Foundation::UInt8 GetBits() const noexcept
    {
        return m_bits;
    }

private:
    struct RawValueTag final
    {
    };

    constexpr PagePermissions(const Foundation::UInt8 bits, RawValueTag) noexcept : m_bits{bits}
    {
    }

    static constexpr Foundation::UInt8 KnownBits =
        static_cast<Foundation::UInt8>(PagePermission::Read) | static_cast<Foundation::UInt8>(PagePermission::Write) |
        static_cast<Foundation::UInt8>(PagePermission::Execute) | static_cast<Foundation::UInt8>(PagePermission::User);

    Foundation::UInt8 m_bits{0U};
};

enum class MemoryCachePolicy : Foundation::UInt8
{
    WriteBack,
    Uncached,
    WriteCombining,
    WriteThrough,
};

/// A WC device publication also permits a UC fallback. RAM and UC register
/// publications require an exact match; callers must exclude mixed live types.
[[nodiscard]] constexpr bool IsDeviceCachePolicyCompatible(const MemoryCachePolicy published,
                                                           const MemoryCachePolicy requested) noexcept
{
    return (published == requested) ||
           ((published == MemoryCachePolicy::WriteCombining) && (requested == MemoryCachePolicy::Uncached));
}

static_assert(sizeof(PhysicalAddress) == 8U);
static_assert(sizeof(VirtualAddress) == 8U);
static_assert(sizeof(PageCount) == 8U);
static_assert(sizeof(MappingPermissions) == 2U);
static_assert(sizeof(PagePermission) == 1U);
static_assert(sizeof(PagePermissions) == 1U);
static_assert(sizeof(MemoryCachePolicy) == 1U);
} // namespace Asteria::Kernel::Memory

#endif // ASTERIA_KERNEL_MEMORY_MEMORY_TYPES_HPP_INCLUDED
