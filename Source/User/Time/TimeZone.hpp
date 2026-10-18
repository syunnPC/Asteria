// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_TIME_TIME_ZONE_HPP_INCLUDED
#define ASTERIA_USER_TIME_TIME_ZONE_HPP_INCLUDED
#include "User/Time/Posix.hpp"
namespace Asteria::User::Time
{
struct LocalTime final
{
    Civil Date{};
    LocalType Type{};
};
struct LocalMatches final
{
    Int64 Earliest{0};
    Int64 Latest{0};
    UInt32 Count{0U};
};
namespace Detail
{
struct Block final
{
    const Foundation::UInt8* Times{nullptr};
    const Foundation::UInt8* Indices{nullptr};
    const Foundation::UInt8* Types{nullptr};
    const Foundation::UInt8* Names{nullptr};
    UInt32 TimeCount{0U};
    UInt32 TypeCount{0U};
    UInt32 NameCount{0U};
    Size TimeWidth{4U};
};
} // namespace Detail
/// Immutable borrowed TZif view. The owner keeps bytes alive and unchanged.
/// Copies share those bytes; calls require no allocation or external state.
class TimeZone final
{
public:
    static constexpr Size MaximumFileSize{1024U * 1024U};
    static constexpr UInt32 MaximumTransitions{65536U};
    static constexpr UInt32 MaximumTypes{256U};
    static constexpr UInt32 MaximumDesignations{65536U};
    static constexpr UInt32 MaximumLeaps{4096U};
    static constexpr Size MaximumFooterSize{1024U};
    /// Failure invalidates this object. Reserved header bytes are ignored.
    [[nodiscard]] Status Load(const Foundation::UInt8* bytes, Size size) noexcept;
    [[nodiscard]] bool IsLoaded() const noexcept
    {
        return m_loaded;
    }
    /// Offset lookup supports all Int64 seconds; civil conversion is bounded
    /// by MinimumYear/MaximumYear. Outputs unchanged on failure.
    [[nodiscard]] Status TypeAt(Int64 utc, LocalType& type) const noexcept;
    [[nodiscard]] Status ToLocal(Int64 utc, LocalTime& local) const noexcept;
    /// Matches are zeroed on failure/gap. Unknown candidate coverage returns
    /// Unspecified. Ambiguous returns all distinct candidates' extrema/count.
    [[nodiscard]] Status FromLocal(const Civil& local, LocalMatches& matches) const noexcept;

private:
    [[nodiscard]] LocalType TableType(UInt32 index) const noexcept;
    [[nodiscard]] Int32 CandidateOffset(UInt32 index) const noexcept;
    Detail::Block m_block{};
    Detail::Footer m_footer{};
    bool m_loaded{false};
};
static_assert(sizeof(TimeZone) <= 256U);
static_assert(TimeZone::MaximumTypes == (1U << (sizeof(Foundation::UInt8) * 8U)));
} // namespace Asteria::User::Time
#endif
