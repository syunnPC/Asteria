// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_SESSION_ADDRESS_PLAN_HPP_INCLUDED
#define ASTERIA_USER_SESSION_ADDRESS_PLAN_HPP_INCLUDED
#include "User/Session/SessionFrame.hpp"
namespace Asteria::User::Session
{
/// Relative to a 1 GiB aligned region supplied by the component binding. Tables persist across
/// Unmap and slot reuse.
inline constexpr Foundation::UInt64 AddressGranule{2U * 1024U * 1024U};
inline constexpr Foundation::UInt64 AddressPoolSpan{SessionMaximumPoolBytes + AddressGranule};
inline constexpr Foundation::UInt64 AddressClientSpan{2U * AddressPoolSpan};
inline constexpr Foundation::UInt64 AddressOutputOffset{4U * AddressClientSpan};
inline constexpr Foundation::UInt64 AddressRegionBytes{AddressOutputOffset + AddressPoolSpan};
// Separate font data follows all pool guard gaps.
inline constexpr Foundation::UInt64 AddressUiFontOffset{AddressRegionBytes};
inline constexpr Foundation::UInt64 AddressAuxiliaryEnd{AddressUiFontOffset + AddressGranule};
static_assert(AddressAuxiliaryEnd < (1ULL << 30U));
static_assert(AddressRegionBytes == 306U * 1024U * 1024U);
/// Region alignment and addition checked in 64 bits before binding.
[[nodiscard]] constexpr bool IsAddressRegionBase(const Foundation::UInt64 base) noexcept
{
    return ((base & ((1ULL << 30U) - 1U)) == 0U) && (AddressAuxiliaryEnd <= ~Foundation::UInt64{0U} - base);
}
class AddressPlan final
{
public:
    void Configure(Foundation::UInt32 tableBudget, Foundation::UInt32 seedTables, Foundation::UInt32 seedRecords,
                   bool lifetimeLedger) noexcept;
    /// Slot 0-3/pool 1-2; output uses slot 4/pool 1. Refusal is atomic.
    [[nodiscard]] bool Map(Foundation::Size slot, Foundation::UInt32 pool, Foundation::UInt32 pages,
                           Foundation::UInt32 frames) noexcept;
    [[nodiscard]] bool Unmap(Foundation::Size slot, Foundation::UInt32 pool) noexcept;
    [[nodiscard]] Foundation::UInt32 Tables() const noexcept
    {
        return m_tables;
    }
    [[nodiscard]] Foundation::UInt32 Records() const noexcept
    {
        return m_records;
    }
    [[nodiscard]] static constexpr Foundation::UInt64 Offset(Foundation::Size slot, Foundation::UInt32 pool) noexcept
    {
        return slot * AddressClientSpan + (pool - 1U) * AddressPoolSpan;
    }

private:
    Foundation::UInt32 m_high[9U]{};
    Foundation::UInt32 m_live[9U]{};
    Foundation::UInt32 m_budget{0U};
    Foundation::UInt32 m_tables{0U};
    Foundation::UInt32 m_records{0U};
    bool m_lifetime{false};
    bool m_directory{false};
};
} // Namespace Asteria::User::Session
#endif
