// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_TIME_POSIX_HPP_INCLUDED
#define ASTERIA_USER_TIME_POSIX_HPP_INCLUDED
#include "User/Time/Civil.hpp"
namespace Asteria::User::Time
{
struct TextView final
{
    const char* Data{nullptr};
    Size Length{0U};
};
struct LocalType final
{
    Int32 OffsetSeconds{0};
    bool Daylight{false};
    TextView Designation{};
};
namespace Detail
{
struct Rule final
{
    char Kind{'\0'};
    UInt32 Day{0U};
    UInt32 Month{0U};
    UInt32 Week{0U};
    Int32 Seconds{7200};
};
struct Footer final
{
    LocalType Standard{};
    LocalType Daylight{};
    Rule Start{};
    Rule End{};
    bool Present{false};
    bool HasDaylight{false};
};
[[nodiscard]] bool ParseFooter(const char* data, Size length, bool version3, Footer& footer) noexcept;
[[nodiscard]] LocalType FooterType(const Footer& footer, Int64 utc) noexcept;
}
} // namespace Asteria::User::Time
#endif
