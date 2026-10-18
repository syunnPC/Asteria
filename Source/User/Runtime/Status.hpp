// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_RUNTIME_STATUS_HPP_INCLUDED
#define ASTERIA_USER_RUNTIME_STATUS_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"

namespace Asteria::User::Runtime
{
/// Opaque facility identity, independent of the facility registry.
class Facility final
{
public:
    /// Precondition: number <= 127. Facility constants are checked by the registry test.
    explicit constexpr Facility(const Foundation::UInt32 number) noexcept : m_number{number}
    {
    }

    [[nodiscard]] constexpr Foundation::UInt32 Number() const noexcept
    {
        return m_number;
    }

    [[nodiscard]] friend constexpr bool operator==(Facility, Facility) noexcept = default;

private:
    Foundation::UInt32 m_number;
};

class [[nodiscard]] Status final
{
public:
    constexpr Status() noexcept = default;

    /// The small structural concept uses only the facility's ADL hooks, not standard traits.
    /// Precondition: value is a known nonzero code of its facility.
    template <typename TCode>
        requires requires(TCode value, Foundation::UInt32 code) {
            { FacilityOf(value) } noexcept;
            { IsKnownStatusCode(value, code) } noexcept;
            static_cast<Foundation::UInt32>(value);
        }
    constexpr Status(const TCode value) noexcept
        : m_raw{0x8000'0000U | (FacilityOf(value).Number() << 24U) | static_cast<Foundation::UInt32>(value)}
    {
    }

    /// The only integer constructor. Rejects nonzero success encodings and failure code zero.
    /// Unknown nonzero failure codes/facilities are preserved for forward compatibility.
    [[nodiscard]] static Foundation::Result<Status, Status> FromRaw(Foundation::UInt32 raw) noexcept;

    [[nodiscard]] constexpr Foundation::UInt32 Raw() const noexcept
    {
        return m_raw;
    }

    [[nodiscard]] constexpr bool IsSuccess() const noexcept
    {
        return m_raw == 0U;
    }

    [[nodiscard]] constexpr bool IsFailure() const noexcept
    {
        return (m_raw & 0x8000'0000U) != 0U;
    }

    [[nodiscard]] constexpr Facility GetFacility() const noexcept
    {
        return Facility{(m_raw >> 24U) & 0x7FU};
    }

    [[nodiscard]] constexpr Foundation::UInt32 Code() const noexcept
    {
        return m_raw & 0x00FF'FFFFU;
    }

    template <typename TCode>
        requires requires(TCode value, Foundation::UInt32 code) {
            { FacilityOf(value) } noexcept;
            { IsKnownStatusCode(value, code) } noexcept;
            static_cast<TCode>(code);
        }
    [[nodiscard]] constexpr Foundation::Result<TCode, Status> TryAs() const noexcept
    {
        if (!IsFailure() || (GetFacility() != FacilityOf(TCode{})) || !IsKnownStatusCode(TCode{}, Code()))
        {
            return Foundation::Result<TCode, Status>::Failure(*this);
        }
        // Both identity and meaning have been validated before the enum conversion.
        return Foundation::Result<TCode, Status>::Success(static_cast<TCode>(Code()));
    }

    /// Precondition: TryAs<TCode>.HasValue. This is a checked-input accessor.
    template <typename TCode> [[nodiscard]] constexpr TCode As() const noexcept
    {
        return TryAs<TCode>().GetValue();
    }

    [[nodiscard]] friend constexpr bool operator==(Status, Status) noexcept = default;

private:
    Foundation::UInt32 m_raw{0U};
};
static_assert(sizeof(Status) == 4U);
static_assert(alignof(Status) == alignof(Foundation::UInt32));
} // Namespace Asteria::User::Runtime

#endif
