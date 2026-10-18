// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_TERMINAL_TERMINAL_PARSER_HPP_INCLUDED
#define ASTERIA_USER_TERMINAL_TERMINAL_PARSER_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "User/Terminal/TerminalScreen.hpp"

namespace Asteria::User::Terminal
{
/// The answers to reports (DSR, DA) waiting for the terminal's input path.
inline constexpr Foundation::Size TerminalResponseCapacity{128U};
/// Control-sequence parameters: at most this many, each at most the value
/// below (a larger number keeps the maximum). A sequence with more
/// parameters is ignored.
inline constexpr Foundation::Size TerminalMaximumParameters{16U};
inline constexpr Foundation::UInt32 TerminalMaximumParameterValue{9999U};

/// The bounded queue of report answers. An answer is queued whole or, when
/// it does not fit, dropped and counted, never cut.
class TerminalResponses final
{
public:
    void Clear() noexcept;
    void Append(const Foundation::UInt8* bytes, Foundation::Size count) noexcept;
    /// Moves up to capacity queued bytes into out; returns how many.
    [[nodiscard]] Foundation::Size Take(Foundation::UInt8* out, Foundation::Size capacity) noexcept;
    [[nodiscard]] Foundation::Size Pending() const noexcept;
    [[nodiscard]] Foundation::UInt64 Dropped() const noexcept;

private:
    Foundation::UInt8 m_bytes[TerminalResponseCapacity]{};
    Foundation::Size m_count{0U};
    Foundation::UInt64 m_dropped{0U};
};

/// A bounded parser of the terminal's input bytes, written from ECMA-48 (5th edition) and DEC's
/// VT100 conventions, applying what it recognizes to a TerminalScreen. Every byte costs bounded
/// work and the parser stores no string: it keeps a state, at most TerminalMaximumParameters
/// parameters, a private marker and a partial UTF-8 character. - Text is UTF-8. Every scalar is
/// stored unchanged; malformed bytes or interrupted partial sequences store Text:
/// ReplacementCodePoint. The interrupting byte is processed afresh. Rendering alone chooses a
/// glyph. The C1 code points U+0080-U+009F are ignored. DEL (0x7F) is ignored. - C0 controls: BS,
/// HT, LF, VT and FF (as LF), CR act on the screen; BEL and the others are ignored. A C0 control
/// inside an escape or control sequence acts and the sequence continues; CAN and SUB abandon any
/// sequence or string. - Escape sequences (ECMA-48 5.3, and DEC's private ones): ESC 7 and ESC 8
/// save and restore the cursor (DECSC, DECRC), ESC D index (IND), ESC M reverse index (RI), ESC E
/// next line (NEL: CR then index), ESC c full reset (RIS). Others, and every sequence with an
/// intermediate byte (such as a character-set designation), are recognized and ignored. - Control
/// sequences (ECMA-48 5.4: CSI, parameter bytes 0x30-0x3F, intermediate bytes 0x20-0x2F, a final
/// byte 0x40-0x7E): ICH '@', CUU 'A', CUD 'B', CUF 'C', CUB 'D', CNL 'E', CPL 'F', CHA 'G', CUP
/// 'H', ED 'J', EL 'K', IL 'L', DL 'M', DCH 'P', SU 'S', SD 'T', ECH 'X', DA 'c', VPA 'd', HVP 'f',
/// SGR 'm', DSR 'n', and DEC's DECSTBM 'r' and private modes (CSI ? Pm h / l) 1, 7 and 25. A
/// missing or zero count means one; positions are one-based. A sequence with an intermediate byte,
/// a sub-parameter separator ':', a private marker other than '?' (or '?' with another final), or
/// more than TerminalMaximumParameters parameters is ignored whole. - SGR: 0 resets; 1, 4, 7 set
/// bold, underline, reverse; 22, 24, 27 clear them; 30-37 and 90-97 set the foreground (palette 0-7
/// and 8-15), 39 the default; 40-47, 100-107 and 49 the background. 38 and 48 (extended colours)
/// are skipped with their arguments (5;n or 2;r;g;b) and change nothing. Others are ignored. -
/// Reports, queued in TerminalResponses: DSR 5 answers ESC [ 0 n (ready); DSR 6 answers ESC [
/// <row>; <column> R, one-based (CPR); a primary DA (CSI c or CSI 0 c) answers ESC [ ? 1; 2 c (a
/// VT100 with the advanced video option, which bold, underline and reverse need). - Control strings
/// (ECMA-48 5.6): OSC (ESC ]) ends at ST (ESC \) or BEL; DCS (ESC P), SOS (ESC X), PM (ESC ^) and
/// APC (ESC _) end at ST. Their content is discarded. An ESC inside a string that is not followed
/// by '\' ends the string and starts a new escape sequence. - A byte of 0x80 or above inside an
/// escape or control sequence abandons it and is processed as text.
class TerminalParser final
{
public:
    /// Back to the ground state with no partial character or sequence.
    void Reset() noexcept;
    /// Processes one byte.
    void Feed(Foundation::UInt8 byte, TerminalScreen& screen, TerminalResponses& responses) noexcept;

private:
    enum class State : Foundation::UInt8
    {
        Ground,
        Escape,
        EscapeIntermediate,
        CsiEntry,
        CsiParameter,
        CsiIntermediate,
        CsiIgnore,
        OscString,
        IgnoredString,
        StringEscape,
    };

    void FeedGround(Foundation::UInt8 byte, TerminalScreen& screen) noexcept;
    void FeedEscape(Foundation::UInt8 byte, TerminalScreen& screen) noexcept;
    void FeedEscapeIntermediate(Foundation::UInt8 byte, TerminalScreen& screen) noexcept;
    void FeedCsi(Foundation::UInt8 byte, TerminalScreen& screen, TerminalResponses& responses) noexcept;
    void FeedCsiIntermediate(Foundation::UInt8 byte, TerminalScreen& screen) noexcept;
    void FeedCsiIgnore(Foundation::UInt8 byte, TerminalScreen& screen) noexcept;
    void FeedString(Foundation::UInt8 byte) noexcept;
    void FeedStringEscape(Foundation::UInt8 byte, TerminalScreen& screen) noexcept;
    void StartUtf8(Foundation::UInt8 byte, TerminalScreen& screen) noexcept;
    void PrintCodePoint(Foundation::UInt32 codePoint, TerminalScreen& screen) noexcept;
    void StartCsi() noexcept;
    void AddDigit(Foundation::UInt8 digit) noexcept;
    void AddSeparator() noexcept;
    void Dispatch(Foundation::UInt8 final, TerminalScreen& screen, TerminalResponses& responses) noexcept;
    void DispatchModes(Foundation::UInt8 final, TerminalScreen& screen) noexcept;
    void SelectGraphicRendition(TerminalScreen& screen) const noexcept;
    void Report(Foundation::UInt8 final, TerminalScreen& screen, TerminalResponses& responses) noexcept;
    /// Parameter index as a count or position: one when missing or zero.
    [[nodiscard]] Foundation::UInt32 Count(Foundation::Size index) const noexcept;
    /// Parameter index as a selector: zero when missing.
    [[nodiscard]] Foundation::UInt32 Selector(Foundation::Size index) const noexcept;

    State m_state{State::Ground};
    Foundation::UInt8 m_private{0U};
    Foundation::Size m_count{0U};
    Foundation::UInt32 m_parameters[TerminalMaximumParameters]{};
    Foundation::UInt32 m_codePoint{0U};
    Foundation::UInt32 m_remaining{0U};
    Foundation::UInt8 m_lower{0x80U};
    Foundation::UInt8 m_upper{0xBFU};
    /// Scratch for a report's answer.
    Foundation::UInt8 m_answer[16U]{};
};
} // Namespace Asteria::User::Terminal

#endif // ASTERIA_USER_TERMINAL_TERMINAL_PARSER_HPP_INCLUDED
