// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_INPUT_HIDBOOT_HPP_INCLUDED
#define ASTERIA_USER_INPUT_HIDBOOT_HPP_INCLUDED

#include "User/Input/InputEvent.hpp"

namespace Asteria::User::Input
{
enum class ReportResult : UInt8
{
    Accepted,
    Short,
    Rollover,
    Invalid
};
/// Boot protocol only; report IDs are absent. Rejected reports leave state intact.
/// Keyboard modifiers are emitted before ordinary key releases/makes; every event
/// carries the final modifier snapshot. Reserved byte must be zero. Duplicates
/// are treated as a set, and modifier usages in the six slots are rejected.
/// Serialized, bounded, allocation-free; buffers/output may not overlap.
class HidBoot final
{
public:
    explicit constexpr HidBoot(UInt32 source = 0U) noexcept : m_source{source}
    {
    }
    void Reset() noexcept;
    /// Device-gone key/modifier and button releases, followed by Reset. Replaces output.
    void Release(EventBatch& output) noexcept;
    [[nodiscard]] ReportResult Keyboard(const UInt8* report, Size length, EventBatch& output) noexcept;
    [[nodiscard]] ReportResult Mouse(const UInt8* report, Size length, EventBatch& output) noexcept;

private:
    UInt32 m_source;
    UInt8 m_keys[6U]{};
    UInt8 m_modifiers{0U};
    UInt8 m_buttons{0U};
};
} // namespace Asteria::User::Input

#endif
