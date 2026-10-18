// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_OBJECT_SLOT_BITMAP_HPP_INCLUDED
#define ASTERIA_KERNEL_OBJECT_SLOT_BITMAP_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Kernel::Object
{
/// The index of the lowest set bit of a nonzero word, without a compiler
/// builtin (the IA-64 toolchain has no libgcc): the de Bruijn product of the
/// isolated bit selects it from a 64-entry table.
[[nodiscard]] constexpr Foundation::Size LowestSetBit(const Foundation::UInt64 word) noexcept
{
    constexpr Foundation::UInt64 DeBruijn{0x03F7'9D71'B4CB'0A89ULL};
    constexpr Foundation::UInt8 Positions[64]{
        0U,  1U,  48U, 2U,  57U, 49U, 28U, 3U,  61U, 58U, 50U, 42U, 38U, 29U, 17U, 4U,  62U, 55U, 59U, 36U, 53U, 51U,
        43U, 22U, 45U, 39U, 33U, 30U, 24U, 18U, 12U, 5U,  63U, 47U, 56U, 27U, 60U, 41U, 37U, 16U, 54U, 35U, 52U, 21U,
        44U, 32U, 23U, 11U, 46U, 26U, 40U, 15U, 34U, 20U, 31U, 10U, 25U, 14U, 19U, 9U,  13U, 8U,  7U,  6U};
    return Positions[((word & (~word + 1U)) * DeBruijn) >> 58U];
}

/// A set of slot indices below Capacity with a summary word per 64 words, so the lowest member at
/// or above an index is found in at most SummaryWords + 2 word reads instead of a scan of the
/// slots. Pools keep their free slots here, lowest first as their scans chose, and the registry its
/// free and quiescing entries. Not synchronized: the owner's lock covers it.
template <Foundation::Size Capacity> class SlotBitmap final
{
public:
    static constexpr Foundation::Size Words{(Capacity + 63U) / 64U};
    static constexpr Foundation::Size SummaryWords{(Words + 63U) / 64U};

    constexpr SlotBitmap() noexcept = default;

    [[nodiscard]] constexpr bool Contains(const Foundation::Size index) const noexcept
    {
        return (index < Capacity) && ((m_words[index / 64U] & Bit(index % 64U)) != 0U);
    }

    constexpr void Insert(const Foundation::Size index) noexcept
    {
        if (index < Capacity)
        {
            m_words[index / 64U] |= Bit(index % 64U);
            m_summary[index / 4096U] |= Bit((index / 64U) % 64U);
        }
    }

    constexpr void Remove(const Foundation::Size index) noexcept
    {
        if (index < Capacity)
        {
            Foundation::UInt64& word = m_words[index / 64U];
            word &= ~Bit(index % 64U);
            if (word == 0U)
            {
                m_summary[index / 4096U] &= ~Bit((index / 64U) % 64U);
            }
        }
    }

    /// The lowest member at or above first, or Capacity when there is none.
    [[nodiscard]] constexpr Foundation::Size FindFirst(const Foundation::Size first = 0U) const noexcept
    {
        if (first >= Capacity)
        {
            return Capacity;
        }
        const Foundation::Size wordIndex = first / 64U;
        const Foundation::UInt64 head = m_words[wordIndex] & ~(Bit(first % 64U) - 1U);
        if (head != 0U)
        {
            return (wordIndex * 64U) + LowestSetBit(head);
        }
        // The words after wordIndex, through the summary.
        const Foundation::Size next = wordIndex + 1U;
        for (Foundation::Size summary = next / 64U; summary < SummaryWords; ++summary)
        {
            Foundation::UInt64 candidates = m_summary[summary];
            if (summary == (next / 64U))
            {
                candidates &= (next % 64U == 0U) ? ~Foundation::UInt64{0U} : ~(Bit(next % 64U) - 1U);
            }
            if (candidates != 0U)
            {
                const Foundation::Size word = (summary * 64U) + LowestSetBit(candidates);
                return (word * 64U) + LowestSetBit(m_words[word]);
            }
        }
        return Capacity;
    }

    [[nodiscard]] constexpr bool IsEmpty() const noexcept
    {
        for (const Foundation::UInt64 summary : m_summary)
        {
            if (summary != 0U)
            {
                return false;
            }
        }
        return true;
    }

private:
    [[nodiscard]] static constexpr Foundation::UInt64 Bit(const Foundation::Size position) noexcept
    {
        return Foundation::UInt64{1U} << position;
    }

    Foundation::UInt64 m_words[Words]{};
    Foundation::UInt64 m_summary[SummaryWords]{};
};

/// Where a pool slot is in its life: free, holding a live object, or holding
/// a destroyed one that waits for its last reference.
enum class PoolSlotState : Foundation::UInt8
{
    Free,
    Active,
    Quiescing,
    Zeroing, // Allocated, without an object yet.
};

/// The free, live and quiescing slots of a pool of Capacity slots, each set a SlotBitmap, with the
/// live and quiescing counts: a pool finds its lowest free slot, and visits only its live or
/// quiescing ones, without a scan. A pool starts with no free slot until it attaches its storage
/// (AddFree).
template <Foundation::Size Capacity> class PoolSlotSets final
{
public:
    constexpr PoolSlotSets() noexcept = default;

    constexpr void AddFree(const Foundation::Size index) noexcept
    {
        m_free.Insert(index);
        ++m_slots;
    }

    /// Moves slot index from state from to state to.
    constexpr void Move(const Foundation::Size index, const PoolSlotState from, const PoolSlotState to) noexcept
    {
        Remove(index, from);
        Add(index, to);
    }

    /// The lowest slot at or above first in state, or Capacity.
    [[nodiscard]] constexpr Foundation::Size FindFirst(const PoolSlotState state,
                                                       const Foundation::Size first = 0U) const noexcept
    {
        switch (state)
        {
        case PoolSlotState::Free:
            return m_free.FindFirst(first);
        case PoolSlotState::Active:
            return m_active.FindFirst(first);
        case PoolSlotState::Quiescing:
            return m_quiescing.FindFirst(first);
        case PoolSlotState::Zeroing:
            return m_zeroing.FindFirst(first);
        }
        return Capacity;
    }

    [[nodiscard]] constexpr Foundation::Size GetFreeCount() const noexcept
    {
        return m_slots - m_activeCount - m_quiescingCount - m_zeroingCount;
    }

    [[nodiscard]] constexpr Foundation::Size GetActiveCount() const noexcept
    {
        return m_activeCount;
    }

    [[nodiscard]] constexpr Foundation::Size GetQuiescingCount() const noexcept
    {
        return m_quiescingCount;
    }

    [[nodiscard]] constexpr Foundation::Size GetZeroingCount() const noexcept
    {
        return m_zeroingCount;
    }

private:
    constexpr void Remove(const Foundation::Size index, const PoolSlotState state) noexcept
    {
        switch (state)
        {
        case PoolSlotState::Free:
            m_free.Remove(index);
            break;
        case PoolSlotState::Active:
            m_active.Remove(index);
            --m_activeCount;
            break;
        case PoolSlotState::Quiescing:
            m_quiescing.Remove(index);
            --m_quiescingCount;
            break;
        case PoolSlotState::Zeroing:
            m_zeroing.Remove(index);
            --m_zeroingCount;
            break;
        }
    }

    constexpr void Add(const Foundation::Size index, const PoolSlotState state) noexcept
    {
        switch (state)
        {
        case PoolSlotState::Free:
            m_free.Insert(index);
            break;
        case PoolSlotState::Active:
            m_active.Insert(index);
            ++m_activeCount;
            break;
        case PoolSlotState::Quiescing:
            m_quiescing.Insert(index);
            ++m_quiescingCount;
            break;
        case PoolSlotState::Zeroing:
            m_zeroing.Insert(index);
            ++m_zeroingCount;
            break;
        }
    }

    SlotBitmap<Capacity> m_zeroing{};
    Foundation::Size m_zeroingCount{0U};
    SlotBitmap<Capacity> m_free{};
    SlotBitmap<Capacity> m_active{};
    SlotBitmap<Capacity> m_quiescing{};
    Foundation::Size m_slots{0U};
    Foundation::Size m_activeCount{0U};
    Foundation::Size m_quiescingCount{0U};
};

static_assert(sizeof(PoolSlotState) == 1U);
static_assert(LowestSetBit(1U) == 0U);
static_assert(LowestSetBit(0x8000'0000'0000'0000ULL) == 63U);
static_assert(LowestSetBit(0x0000'0100'0000'0000ULL) == 40U);
static_assert(SlotBitmap<16384U>::Words == 256U);
static_assert(SlotBitmap<16384U>::SummaryWords == 4U);
static_assert(SlotBitmap<4096U>::SummaryWords == 1U);
} // Namespace Asteria::Kernel::Object

#endif // ASTERIA_KERNEL_OBJECT_SLOT_BITMAP_HPP_INCLUDED
