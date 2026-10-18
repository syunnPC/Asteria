// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_INPUT_PS2KEYBOARD_HPP_INCLUDED
#define ASTERIA_USER_INPUT_PS2KEYBOARD_HPP_INCLUDED

#include "User/Input/InputEvent.hpp"

namespace Asteria::User::Input
{
enum class KeyboardGeometry : UInt8
{
    Ansi,
    Iso,
    Jis
};
enum class ScanResult : UInt8
{
    None,
    Event,
    Unknown,
    Overrun,
    SelfTestPassed,
    Ack,
    Resend,
    Echo,
};
struct ScanOutput
{
    ScanResult Result;
    InputEvent Event;
};
/// Set2 is untranslated scan set 2; Set1 is scan set 1, as a keyboard in set 2 produces it through
/// the controller's translation.
enum class ScanCodeSet : UInt8
{
    Set2,
    Set1,
};

/// Scan set 2 (translation disabled) or set 1. Serialized, bounded, allocation-free.
/// Set-2 0x5D / set-1 0x2B are physically ambiguous: ANSI backslash vs ISO Non-US #; layout selects it.
/// Pause (Microsoft rev 1.3a Appendix A note 5) is never retained as a pressed key.
/// In set 1, 0xAA is Left Shift's break code, not a self-test result.
class Ps2Keyboard final
{
public:
    explicit constexpr Ps2Keyboard(UInt32 source = 0U, KeyboardGeometry layout = KeyboardGeometry::Ansi,
                                   ScanCodeSet set = ScanCodeSet::Set2) noexcept
        : m_source{source}, m_layout{layout}, m_set{set}
    {
    }
    [[nodiscard]] ScanOutput Feed(UInt8 byte) noexcept;
    void Reset() noexcept;
    /// Selects the decoding and resets the decoder (Reset).
    void SetScanCodeSet(ScanCodeSet set) noexcept;
    [[nodiscard]] ScanCodeSet GetScanCodeSet() const noexcept
    {
        return m_set;
    }
    [[nodiscard]] UInt8 GetModifiers() const noexcept
    {
        return m_modifiers;
    }
    [[nodiscard]] bool IsPressed(UInt8 usage) const noexcept
    {
        return m_pressed[usage];
    }

private:
    [[nodiscard]] ScanOutput Key(UInt8 usage, bool release, bool momentary = false) noexcept;
    [[nodiscard]] ScanOutput Pause(UInt8 byte, const UInt8* sequence, UInt8 length) noexcept;
    [[nodiscard]] ScanOutput FeedSet1(UInt8 byte) noexcept;
    UInt32 m_source;
    KeyboardGeometry m_layout;
    ScanCodeSet m_set;
    bool m_pressed[256U]{};
    UInt8 m_modifiers{0U};
    UInt8 m_pauseAt{0U};
    bool m_extended{false};
    bool m_break{false};
};
} // Namespace Asteria::User::Input

#endif
