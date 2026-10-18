// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_TIME_CIVIL_HPP_INCLUDED
#define ASTERIA_USER_TIME_CIVIL_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::User::Time
{
using Foundation::Int32;
using Foundation::Int64;
using Foundation::UInt32;
using Foundation::Size;

enum class Status : Foundation::UInt8
{
    Ok,
    Invalid,
    OutOfRange,
    Unspecified,
    Gap,
    Ambiguous,
    BufferTooSmall
};

inline constexpr Int32 MinimumYear{-999999};
inline constexpr Int32 MaximumYear{999999};
inline constexpr Int64 SecondsPerDay{86400};
struct Civil final
{
    Int32 Year{1970};
    UInt32 Month{1U};
    UInt32 Day{1U};
    UInt32 Hour{0U};
    UInt32 Minute{0U};
    UInt32 Second{0U};
    [[nodiscard]] constexpr bool operator==(const Civil&) const noexcept = default;
};
struct IsoWeek final
{
    Int32 Year;
    UInt32 Week;
    UInt32 Weekday;
};

[[nodiscard]] bool IsLeapYear(Int32 year) noexcept;
[[nodiscard]] UInt32 DaysInMonth(Int32 year, UInt32 month) noexcept;
[[nodiscard]] bool IsValid(const Civil& civil) noexcept;
/// Results remain unchanged on failure. Leap seconds are never emitted.
[[nodiscard]] Status ToSeconds(const Civil& civil, Int64& seconds) noexcept;
[[nodiscard]] Status FromSeconds(Int64 seconds, Civil& civil) noexcept;
[[nodiscard]] Status AddDays(const Civil& civil, Int64 days, Civil& result) noexcept;
/// Monday=1 through Sunday=7; zero on invalid input.
[[nodiscard]] UInt32 DayOfWeek(const Civil& civil) noexcept;
[[nodiscard]] Status GetIsoWeek(const Civil& civil, IsoWeek& week) noexcept;
/// Extended calendar and week dates. Capacity includes NUL; format years
/// 0000..9999. Failure leaves buffer and length unchanged.
[[nodiscard]] Status FormatIsoDate(const Civil& civil, char* buffer, Size capacity, Size& length) noexcept;
[[nodiscard]] Status FormatIsoWeekDate(const Civil& civil, char* buffer, Size capacity, Size& length) noexcept;
inline constexpr Size IsoDateLength{10U};
inline constexpr Size IsoWeekDateLength{10U};
static_assert(IsoDateLength == 4U + 1U + 2U + 1U + 2U);
static_assert(IsoWeekDateLength == 4U + 2U + 2U + 1U + 1U);
/// Capacity includes NUL. Year 0000..9999; absolute offset <24h. An offset
/// with seconds uses the documented:ss extension to ISO 8601. Failure leaves
/// both buffer and length unchanged. Zero offset uses Z.
[[nodiscard]] Status FormatIso8601(const Civil& civil, Int32 offsetSeconds, char* buffer, Size capacity,
                                   Size& length) noexcept;
inline constexpr Size MaximumIso8601Length{28U};
static_assert(MaximumIso8601Length == 19U + 9U);
} // Namespace Asteria::User::Time
#endif
