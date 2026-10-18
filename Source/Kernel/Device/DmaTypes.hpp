// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_DEVICE_DMA_TYPES_HPP_INCLUDED
#define ASTERIA_KERNEL_DEVICE_DMA_TYPES_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Kernel::Device
{
/// One byte address in an IOMMU domain's device-visible address space.
class DmaAddress final
{
public:
    constexpr DmaAddress() noexcept = default;

    explicit constexpr DmaAddress(const Foundation::UInt64 value) noexcept : m_value{value}
    {
    }

    [[nodiscard]] constexpr Foundation::UInt64 GetValue() const noexcept
    {
        return m_value;
    }

private:
    Foundation::UInt64 m_value{0U};
};

/// Permissions are named from the device's point of view.
enum class DmaPermission : Foundation::UInt8
{
    Read = Foundation::UInt8{1U} << 0U,
    Write = Foundation::UInt8{1U} << 1U,
};

class DmaPermissions final
{
public:
    constexpr DmaPermissions() noexcept = default;

    explicit constexpr DmaPermissions(const DmaPermission permission) noexcept
        : m_bits{static_cast<Foundation::UInt8>(permission)}
    {
    }

    [[nodiscard]] constexpr DmaPermissions With(const DmaPermission permission) const noexcept
    {
        return DmaPermissions{
            static_cast<Foundation::UInt8>(m_bits | static_cast<Foundation::UInt8>(permission)),
            RawValueTag{},
        };
    }

    [[nodiscard]] constexpr bool Contains(const DmaPermission permission) const noexcept
    {
        const Foundation::UInt8 bit = static_cast<Foundation::UInt8>(permission);
        return (m_bits & bit) == bit;
    }

    [[nodiscard]] constexpr bool HasOnlyKnownBits() const noexcept
    {
        return (m_bits & static_cast<Foundation::UInt8>(~KnownBits)) == 0U;
    }

    [[nodiscard]] constexpr bool IsEmpty() const noexcept
    {
        return m_bits == 0U;
    }

    [[nodiscard]] constexpr Foundation::UInt8 GetBits() const noexcept
    {
        return m_bits;
    }

private:
    struct RawValueTag final
    {
    };

    constexpr DmaPermissions(const Foundation::UInt8 bits, RawValueTag) noexcept : m_bits{bits}
    {
    }

    static constexpr Foundation::UInt8 KnownBits =
        static_cast<Foundation::UInt8>(DmaPermission::Read) | static_cast<Foundation::UInt8>(DmaPermission::Write);

    Foundation::UInt8 m_bits{0U};
};

static_assert(sizeof(DmaAddress) == 8U);
static_assert(alignof(DmaAddress) == 8U);
static_assert(sizeof(DmaPermission) == 1U);
static_assert(sizeof(DmaPermissions) == 1U);
} // namespace Asteria::Kernel::Device

#endif // ASTERIA_KERNEL_DEVICE_DMA_TYPES_HPP_INCLUDED
