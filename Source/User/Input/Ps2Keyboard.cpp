// SPDX-License-Identifier: Apache-2.0

#include "User/Input/Ps2Keyboard.hpp"

namespace Asteria::User::Input
{
namespace
{
[[nodiscard]] UInt8 Lookup(const UInt8 byte, const bool extended, const KeyboardGeometry layout) noexcept
{
    // Microsoft Keyboard Scan Code Specification, rev 1.3a, Appendix A
    // (set-2 make codes and notes 2-5), joined by key position to Appendix C.
    // Usage names/values: USB HID Usage Tables 1.7, section 10, page 0x07.
    // Unnamed/reserved scan rows have no mapping. Generic Desktop power
    // controls cannot be represented by this keyboard-page-only interface.
    if (extended)
    {
        switch (byte)
        {
        case 0x7CU:
            return 0x46U; // Keyboard PrintScreen (Appendix A note 4)
        case 0x7EU:
            return 0x48U; // Keyboard Pause (Appendix A note 5, with Control)
        case 0x70U:
            return 0x49U; // Keyboard Insert
        case 0x6CU:
            return 0x4AU; // Keyboard Home
        case 0x7DU:
            return 0x4BU; // Keyboard PageUp
        case 0x71U:
            return 0x4CU; // Keyboard Delete Forward
        case 0x69U:
            return 0x4DU; // Keyboard End
        case 0x7AU:
            return 0x4EU; // Keyboard PageDown
        case 0x74U:
            return 0x4FU; // Keyboard Right Arrow
        case 0x6BU:
            return 0x50U; // Keyboard Left Arrow
        case 0x72U:
            return 0x51U; // Keyboard Down Arrow
        case 0x75U:
            return 0x52U; // Keyboard Up Arrow
        case 0x4AU:
            return 0x54U; // Keypad /
        case 0x5AU:
            return 0x58U; // Keypad ENTER
        case 0x2FU:
            return 0x65U; // Keyboard Application
        case 0x1FU:
            return 0xE3U; // Keyboard Left GUI
        case 0x14U:
            return 0xE4U; // Keyboard RightControl
        case 0x11U:
            return 0xE6U; // Keyboard RightAlt
        case 0x27U:
            return 0xE7U; // Keyboard Right GUI
        default:
            return 0U;
        }
    }
    // Appendix A positions 29/42 share 5D; Appendix C assigns usages 31/32.
    if (byte == 0x5DU)
    {
        return (layout == KeyboardGeometry::Iso) ? 0x32U : 0x31U;
    }
    switch (byte)
    {
    case 0x1CU:
        return 0x04U; // Keyboard a and A
    case 0x32U:
        return 0x05U; // Keyboard b and B
    case 0x21U:
        return 0x06U; // Keyboard c and C
    case 0x23U:
        return 0x07U; // Keyboard d and D
    case 0x24U:
        return 0x08U; // Keyboard e and E
    case 0x2BU:
        return 0x09U; // Keyboard f and F
    case 0x34U:
        return 0x0AU; // Keyboard g and G
    case 0x33U:
        return 0x0BU; // Keyboard h and H
    case 0x43U:
        return 0x0CU; // Keyboard i and I
    case 0x3BU:
        return 0x0DU; // Keyboard j and J
    case 0x42U:
        return 0x0EU; // Keyboard k and K
    case 0x4BU:
        return 0x0FU; // Keyboard l and L
    case 0x3AU:
        return 0x10U; // Keyboard m and M
    case 0x31U:
        return 0x11U; // Keyboard n and N
    case 0x44U:
        return 0x12U; // Keyboard o and O
    case 0x4DU:
        return 0x13U; // Keyboard p and P
    case 0x15U:
        return 0x14U; // Keyboard q and Q
    case 0x2DU:
        return 0x15U; // Keyboard r and R
    case 0x1BU:
        return 0x16U; // Keyboard s and S
    case 0x2CU:
        return 0x17U; // Keyboard t and T
    case 0x3CU:
        return 0x18U; // Keyboard u and U
    case 0x2AU:
        return 0x19U; // Keyboard v and V
    case 0x1DU:
        return 0x1AU; // Keyboard w and W
    case 0x22U:
        return 0x1BU; // Keyboard x and X
    case 0x35U:
        return 0x1CU; // Keyboard y and Y
    case 0x1AU:
        return 0x1DU; // Keyboard z and Z
    case 0x16U:
        return 0x1EU; // Keyboard 1 and !
    case 0x1EU:
        return 0x1FU; // Keyboard 2 and @
    case 0x26U:
        return 0x20U; // Keyboard 3 and #
    case 0x25U:
        return 0x21U; // Keyboard 4 and $
    case 0x2EU:
        return 0x22U; // Keyboard 5 and %
    case 0x36U:
        return 0x23U; // Keyboard 6 and ^
    case 0x3DU:
        return 0x24U; // Keyboard 7 and &
    case 0x3EU:
        return 0x25U; // Keyboard 8 and *
    case 0x46U:
        return 0x26U; // Keyboard 9 and (
    case 0x45U:
        return 0x27U; // Keyboard 0 and )
    case 0x5AU:
        return 0x28U; // Keyboard Return (ENTER)
    case 0x76U:
        return 0x29U; // Keyboard ESCAPE
    case 0x66U:
        return 0x2AU; // Keyboard DELETE (Backspace)
    case 0x0DU:
        return 0x2BU; // Keyboard Tab
    case 0x29U:
        return 0x2CU; // Keyboard Spacebar
    case 0x4EU:
        return 0x2DU; // Keyboard - and _
    case 0x55U:
        return 0x2EU; // Keyboard = and +
    case 0x54U:
        return 0x2FU; // Keyboard [ and {
    case 0x5BU:
        return 0x30U; // Keyboard ] and }
    case 0x4CU:
        return 0x33U; // Keyboard; and:
    case 0x52U:
        return 0x34U; // Keyboard ' and double quote
    case 0x0EU:
        return 0x35U; // Keyboard Grave Accent and Tilde
    case 0x41U:
        return 0x36U; // Keyboard, and <
    case 0x49U:
        return 0x37U; // Keyboard. and >
    case 0x4AU:
        return 0x38U; // Keyboard / and ?
    case 0x58U:
        return 0x39U; // Keyboard Caps Lock
    case 0x05U:
        return 0x3AU; // Keyboard F1
    case 0x06U:
        return 0x3BU; // Keyboard F2
    case 0x04U:
        return 0x3CU; // Keyboard F3
    case 0x0CU:
        return 0x3DU; // Keyboard F4
    case 0x03U:
        return 0x3EU; // Keyboard F5
    case 0x0BU:
        return 0x3FU; // Keyboard F6
    case 0x83U:
        return 0x40U; // Keyboard F7
    case 0x0AU:
        return 0x41U; // Keyboard F8
    case 0x01U:
        return 0x42U; // Keyboard F9
    case 0x09U:
        return 0x43U; // Keyboard F10
    case 0x78U:
        return 0x44U; // Keyboard F11
    case 0x07U:
        return 0x45U; // Keyboard F12
    case 0x84U:
        return 0x46U; // Keyboard PrintScreen (Appendix A note 4, with Alt)
    case 0x7EU:
        return 0x47U; // Keyboard Scroll Lock
    case 0x77U:
        return 0x53U; // Keypad Num Lock and Clear
    case 0x7CU:
        return 0x55U; // Keypad *
    case 0x7BU:
        return 0x56U; // Keypad -
    case 0x79U:
        return 0x57U; // Keypad +
    case 0x69U:
        return 0x59U; // Keypad 1 and End
    case 0x72U:
        return 0x5AU; // Keypad 2 and Down Arrow
    case 0x7AU:
        return 0x5BU; // Keypad 3 and PageDn
    case 0x6BU:
        return 0x5CU; // Keypad 4 and Left Arrow
    case 0x73U:
        return 0x5DU; // Keypad 5
    case 0x74U:
        return 0x5EU; // Keypad 6 and Right Arrow
    case 0x6CU:
        return 0x5FU; // Keypad 7 and Home
    case 0x75U:
        return 0x60U; // Keypad 8 and Up Arrow
    case 0x7DU:
        return 0x61U; // Keypad 9 and PageUp
    case 0x70U:
        return 0x62U; // Keypad 0 and Insert
    case 0x71U:
        return 0x63U; // Keypad. and Delete
    case 0x61U:
        return 0x64U; // Keyboard Non-US backslash and |
    case 0x6DU:
        return 0x85U; // Keypad Comma (position 107)
    case 0x51U:
        return 0x87U; // Keyboard International1 (position 56)
    case 0x13U:
        return 0x88U; // Keyboard International2 (Appendix A Far East row)
    case 0x64U:
        return 0x8AU; // Keyboard International4 (Appendix A Convert row)
    case 0x67U:
        return 0x8BU; // Keyboard International5 (Appendix A Nonconvert row)
    case 0x14U:
        return 0xE0U; // Keyboard LeftControl
    case 0x12U:
        return 0xE1U; // Keyboard LeftShift
    case 0x11U:
        return 0xE2U; // Keyboard LeftAlt
    case 0x59U:
        return 0xE5U; // Keyboard RightShift
    default:
        return 0U;
    }
}

[[nodiscard]] UInt8 LookupSet1(const UInt8 make, const bool extended, const KeyboardGeometry layout) noexcept
{
    // Microsoft Keyboard Scan Code Specification, rev 1.3a, Appendix A, scan code set 1 make codes
    // (break = make | 0x80), the same key rows as the set-2 table above; usages from HID Usage
    // Tables 1.7 page 0x07. Set 1 is what the controller's translation makes of set 2, so every
    // set-2 row has exactly one row here. The specification gives the Far East rows no usage:
    // International1/2/4/5 (0x87, 0x88, 0x8A, 0x8B) are the HID Usage Tables' names for them.
    if (extended)
    {
        switch (make)
        {
        case 0x37U:
            return 0x46U; // Keyboard PrintScreen (Appendix A note 4)
        case 0x46U:
            return 0x48U; // Keyboard Pause (Appendix A note 5, with Control)
        case 0x52U:
            return 0x49U; // Keyboard Insert
        case 0x47U:
            return 0x4AU; // Keyboard Home
        case 0x49U:
            return 0x4BU; // Keyboard PageUp
        case 0x53U:
            return 0x4CU; // Keyboard Delete Forward
        case 0x4FU:
            return 0x4DU; // Keyboard End
        case 0x51U:
            return 0x4EU; // Keyboard PageDown
        case 0x4DU:
            return 0x4FU; // Keyboard Right Arrow
        case 0x4BU:
            return 0x50U; // Keyboard Left Arrow
        case 0x50U:
            return 0x51U; // Keyboard Down Arrow
        case 0x48U:
            return 0x52U; // Keyboard Up Arrow
        case 0x35U:
            return 0x54U; // Keypad /
        case 0x1CU:
            return 0x58U; // Keypad ENTER
        case 0x5DU:
            return 0x65U; // Keyboard Application
        case 0x5BU:
            return 0xE3U; // Keyboard Left GUI
        case 0x1DU:
            return 0xE4U; // Keyboard RightControl
        case 0x38U:
            return 0xE6U; // Keyboard RightAlt
        case 0x5CU:
            return 0xE7U; // Keyboard Right GUI
        default:
            return 0U;
        }
    }
    // Appendix A positions 29/42 share 2B, as they share set-2 5D.
    if (make == 0x2BU)
    {
        return (layout == KeyboardGeometry::Iso) ? 0x32U : 0x31U;
    }
    switch (make)
    {
    case 0x1EU:
        return 0x04U; // Keyboard a and A
    case 0x30U:
        return 0x05U; // Keyboard b and B
    case 0x2EU:
        return 0x06U; // Keyboard c and C
    case 0x20U:
        return 0x07U; // Keyboard d and D
    case 0x12U:
        return 0x08U; // Keyboard e and E
    case 0x21U:
        return 0x09U; // Keyboard f and F
    case 0x22U:
        return 0x0AU; // Keyboard g and G
    case 0x23U:
        return 0x0BU; // Keyboard h and H
    case 0x17U:
        return 0x0CU; // Keyboard i and I
    case 0x24U:
        return 0x0DU; // Keyboard j and J
    case 0x25U:
        return 0x0EU; // Keyboard k and K
    case 0x26U:
        return 0x0FU; // Keyboard l and L
    case 0x32U:
        return 0x10U; // Keyboard m and M
    case 0x31U:
        return 0x11U; // Keyboard n and N
    case 0x18U:
        return 0x12U; // Keyboard o and O
    case 0x19U:
        return 0x13U; // Keyboard p and P
    case 0x10U:
        return 0x14U; // Keyboard q and Q
    case 0x13U:
        return 0x15U; // Keyboard r and R
    case 0x1FU:
        return 0x16U; // Keyboard s and S
    case 0x14U:
        return 0x17U; // Keyboard t and T
    case 0x16U:
        return 0x18U; // Keyboard u and U
    case 0x2FU:
        return 0x19U; // Keyboard v and V
    case 0x11U:
        return 0x1AU; // Keyboard w and W
    case 0x2DU:
        return 0x1BU; // Keyboard x and X
    case 0x15U:
        return 0x1CU; // Keyboard y and Y
    case 0x2CU:
        return 0x1DU; // Keyboard z and Z
    case 0x02U:
        return 0x1EU; // Keyboard 1 and !
    case 0x03U:
        return 0x1FU; // Keyboard 2 and @
    case 0x04U:
        return 0x20U; // Keyboard 3 and #
    case 0x05U:
        return 0x21U; // Keyboard 4 and $
    case 0x06U:
        return 0x22U; // Keyboard 5 and %
    case 0x07U:
        return 0x23U; // Keyboard 6 and ^
    case 0x08U:
        return 0x24U; // Keyboard 7 and &
    case 0x09U:
        return 0x25U; // Keyboard 8 and *
    case 0x0AU:
        return 0x26U; // Keyboard 9 and (
    case 0x0BU:
        return 0x27U; // Keyboard 0 and )
    case 0x1CU:
        return 0x28U; // Keyboard Return (ENTER)
    case 0x01U:
        return 0x29U; // Keyboard ESCAPE
    case 0x0EU:
        return 0x2AU; // Keyboard DELETE (Backspace)
    case 0x0FU:
        return 0x2BU; // Keyboard Tab
    case 0x39U:
        return 0x2CU; // Keyboard Spacebar
    case 0x0CU:
        return 0x2DU; // Keyboard - and _
    case 0x0DU:
        return 0x2EU; // Keyboard = and +
    case 0x1AU:
        return 0x2FU; // Keyboard [ and {
    case 0x1BU:
        return 0x30U; // Keyboard ] and }
    case 0x27U:
        return 0x33U; // Keyboard; and:
    case 0x28U:
        return 0x34U; // Keyboard ' and double quote
    case 0x29U:
        return 0x35U; // Keyboard Grave Accent and Tilde
    case 0x33U:
        return 0x36U; // Keyboard, and <
    case 0x34U:
        return 0x37U; // Keyboard. and >
    case 0x35U:
        return 0x38U; // Keyboard / and ?
    case 0x3AU:
        return 0x39U; // Keyboard Caps Lock
    case 0x3BU:
        return 0x3AU; // Keyboard F1
    case 0x3CU:
        return 0x3BU; // Keyboard F2
    case 0x3DU:
        return 0x3CU; // Keyboard F3
    case 0x3EU:
        return 0x3DU; // Keyboard F4
    case 0x3FU:
        return 0x3EU; // Keyboard F5
    case 0x40U:
        return 0x3FU; // Keyboard F6
    case 0x41U:
        return 0x40U; // Keyboard F7
    case 0x42U:
        return 0x41U; // Keyboard F8
    case 0x43U:
        return 0x42U; // Keyboard F9
    case 0x44U:
        return 0x43U; // Keyboard F10
    case 0x57U:
        return 0x44U; // Keyboard F11
    case 0x58U:
        return 0x45U; // Keyboard F12
    case 0x54U:
        return 0x46U; // Keyboard PrintScreen (Appendix A note 4, with Alt)
    case 0x46U:
        return 0x47U; // Keyboard Scroll Lock
    case 0x45U:
        return 0x53U; // Keypad Num Lock and Clear
    case 0x37U:
        return 0x55U; // Keypad *
    case 0x4AU:
        return 0x56U; // Keypad -
    case 0x4EU:
        return 0x57U; // Keypad +
    case 0x4FU:
        return 0x59U; // Keypad 1 and End
    case 0x50U:
        return 0x5AU; // Keypad 2 and Down Arrow
    case 0x51U:
        return 0x5BU; // Keypad 3 and PageDn
    case 0x4BU:
        return 0x5CU; // Keypad 4 and Left Arrow
    case 0x4CU:
        return 0x5DU; // Keypad 5
    case 0x4DU:
        return 0x5EU; // Keypad 6 and Right Arrow
    case 0x47U:
        return 0x5FU; // Keypad 7 and Home
    case 0x48U:
        return 0x60U; // Keypad 8 and Up Arrow
    case 0x49U:
        return 0x61U; // Keypad 9 and PageUp
    case 0x52U:
        return 0x62U; // Keypad 0 and Insert
    case 0x53U:
        return 0x63U; // Keypad. and Delete
    case 0x56U:
        return 0x64U; // Keyboard Non-US backslash and |
    // Keypad Comma (position 107) is 7E, but its break FE is the
    // keyboard's RESEND answer: the key stays unmapped in set 1.
    case 0x73U:
        return 0x87U; // Keyboard International1 (position 56)
    case 0x70U:
        return 0x88U; // Keyboard International2 (Appendix A Far East row)
    case 0x79U:
        return 0x8AU; // Keyboard International4 (Appendix A Convert row)
    case 0x7BU:
        return 0x8BU; // Keyboard International5 (Appendix A Nonconvert row)
    case 0x1DU:
        return 0xE0U; // Keyboard LeftControl
    case 0x2AU:
        return 0xE1U; // Keyboard LeftShift
    case 0x38U:
        return 0xE2U; // Keyboard LeftAlt
    case 0x36U:
        return 0xE5U; // Keyboard RightShift
    default:
        return 0U;
    }
}

// Microsoft rev 1.3a Appendix A note 5: Pause has no break code.
constexpr UInt8 PauseSet2[8U]{0xE1U, 0x14U, 0x77U, 0xE1U, 0xF0U, 0x14U, 0xF0U, 0x77U};
constexpr UInt8 PauseSet1[6U]{0xE1U, 0x1DU, 0x45U, 0xE1U, 0x9DU, 0xC5U};
} // Namespace

void Ps2Keyboard::SetScanCodeSet(const ScanCodeSet set) noexcept
{
    m_set = set;
    Reset();
}

void Ps2Keyboard::Reset() noexcept
{
    for (bool& pressed : m_pressed)
    {
        pressed = false;
    }
    m_modifiers = 0U;
    m_pauseAt = 0U;
    m_extended = false;
    m_break = false;
}

ScanOutput Ps2Keyboard::Key(const UInt8 usage, const bool release, const bool momentary) noexcept
{
    if (usage == 0U)
    {
        return {.Result = ScanResult::Unknown, .Event = {}};
    }
    if (!momentary && (m_pressed[usage] == !release))
    {
        return {.Result = ScanResult::None, .Event = {}};
    }
    if (!momentary)
    {
        m_pressed[usage] = !release;
    }
    if ((usage >= 0xE0U) && (usage <= 0xE7U))
    {
        const UInt8 bit = static_cast<UInt8>(1U << (usage - 0xE0U));
        m_modifiers = release ? static_cast<UInt8>(m_modifiers & static_cast<UInt8>(~bit))
                              : static_cast<UInt8>(m_modifiers | bit);
    }
    return {.Result = ScanResult::Event,
            .Event = {
                .Kind = release ? EventKind::KeyUp : EventKind::KeyDown,
                .Modifiers = m_modifiers,
                .Usage = usage,
                .Source = m_source,
                .Dx = 0,
                .Dy = 0,
                .Buttons = 0U,
                .Reserved = {},
                .Wheel = 0,
            }};
}

ScanOutput Ps2Keyboard::Pause(const UInt8 byte, const UInt8* const sequence, const UInt8 length) noexcept
{
    if (byte == sequence[m_pauseAt])
    {
        ++m_pauseAt;
        if (m_pauseAt == length)
        {
            m_pauseAt = 0U;
            return Key(0x48U, false, true);
        }
        return {.Result = ScanResult::None, .Event = {}};
    }
    m_pauseAt = 0U;
    // Consume a malformed sequence's mismatching byte: never invent a key.
    return {.Result = ScanResult::Unknown, .Event = {}};
}

// Every byte below 0x80 is a make code, and its break is the byte | 0x80;
// E0 and E1 prefix as in set 2. FA, FE and EE are answers (break codes of
// unused positions, and of Keypad Comma, which stays unmapped); 00 and FF
// are overruns. 0xAA is Left Shift's break, so no self-test result exists.
ScanOutput Ps2Keyboard::FeedSet1(const UInt8 byte) noexcept
{
    ScanResult reply = ScanResult::None;
    switch (byte)
    {
    case 0x00U:
    case 0xFFU:
        reply = ScanResult::Overrun;
        break;
    case 0xFAU:
        reply = ScanResult::Ack;
        break;
    case 0xFEU:
        reply = ScanResult::Resend;
        break;
    case 0xEEU:
        reply = ScanResult::Echo;
        break;
    default:
        break;
    }
    if (reply != ScanResult::None)
    {
        m_pauseAt = 0U;
        m_extended = false;
        return {.Result = reply, .Event = {}};
    }
    if (m_pauseAt != 0U)
    {
        return Pause(byte, PauseSet1, sizeof(PauseSet1));
    }
    if (byte == 0xE1U)
    {
        m_pauseAt = 1U;
        m_extended = false;
        return {.Result = ScanResult::None, .Event = {}};
    }
    if (byte == 0xE0U)
    {
        m_extended = true;
        return {.Result = ScanResult::None, .Event = {}};
    }
    const bool extended = m_extended;
    const bool release = (byte & 0x80U) != 0U;
    const UInt8 make = static_cast<UInt8>(byte & 0x7FU);
    m_extended = false;
    // Microsoft rev 1.3a Appendix A notes 2-4: E0 2A/AA and E0 36/B6 are
    // shift compensation, not keys (set 2's E0 12 and E0 59).
    if (extended && ((make == 0x2AU) || (make == 0x36U)))
    {
        return {.Result = ScanResult::None, .Event = {}};
    }
    return Key(LookupSet1(make, extended, m_layout), release);
}

ScanOutput Ps2Keyboard::Feed(const UInt8 byte) noexcept
{
    if (m_set == ScanCodeSet::Set1)
    {
        return FeedSet1(byte);
    }
    ScanResult reply = ScanResult::None;
    switch (byte)
    {
    case 0x00U:
    case 0xFFU:
        reply = ScanResult::Overrun;
        break;
    case 0xAAU:
        reply = ScanResult::SelfTestPassed;
        break;
    case 0xFAU:
        reply = ScanResult::Ack;
        break;
    case 0xFEU:
        reply = ScanResult::Resend;
        break;
    case 0xEEU:
        reply = ScanResult::Echo;
        break;
    default:
        break;
    }
    if (reply != ScanResult::None)
    {
        m_pauseAt = 0U;
        m_extended = false;
        m_break = false;
        return {.Result = reply, .Event = {}};
    }
    if (m_pauseAt != 0U)
    {
        return Pause(byte, PauseSet2, sizeof(PauseSet2));
    }
    if (byte == 0xE1U)
    {
        m_pauseAt = 1U;
        m_extended = false;
        m_break = false;
        return {.Result = ScanResult::None, .Event = {}};
    }
    if (byte == 0xE0U)
    {
        m_extended = true;
        return {.Result = ScanResult::None, .Event = {}};
    }
    if (byte == 0xF0U)
    {
        m_break = true;
        return {.Result = ScanResult::None, .Event = {}};
    }
    const bool extended = m_extended;
    const bool release = m_break;
    m_extended = false;
    m_break = false;
    // Microsoft rev 1.3a Appendix A notes 2-4: E0 shift/unshift bytes
    // compensate for modifier state; they do not identify physical keys.
    if (extended && ((byte == 0x12U) || (byte == 0x59U)))
    {
        return {.Result = ScanResult::None, .Event = {}};
    }
    return Key(Lookup(byte, extended, m_layout), release);
}
} // Namespace Asteria::User::Input
