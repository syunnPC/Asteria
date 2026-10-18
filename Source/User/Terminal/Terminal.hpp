// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_TERMINAL_TERMINAL_HPP_INCLUDED
#define ASTERIA_USER_TERMINAL_TERMINAL_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "User/Terminal/TerminalParser.hpp"
#include "User/Terminal/TerminalScreen.hpp"

namespace Asteria::User::Terminal
{
/// The terminal emulator of the desktop session's terminal windows: a screen, the parser that
/// applies the output a program writes to it, and the answers to its reports, waiting for the input
/// path. One object per window, in static storage (it holds the maximum grid). Single-threaded; no
/// allocation; Write's work is bounded by its byte count times the grid's size.
class Terminal final
{
public:
    /// Sizes the screen (each clamped to 1 up to the maximum), resets it and
    /// the parser and discards queued answers.
    void Configure(Foundation::UInt32 columns, Foundation::UInt32 rows) noexcept;
    /// Changes the size without reflowing (TerminalScreen::Resize); the parser's state is kept.
    void Resize(Foundation::UInt32 columns, Foundation::UInt32 rows) noexcept;
    /// Applies output bytes; ignored before Configure.
    void Write(const Foundation::UInt8* bytes, Foundation::Size count) noexcept;
    /// Moves up to capacity bytes of queued report answers into out.
    [[nodiscard]] Foundation::Size TakeResponses(Foundation::UInt8* out, Foundation::Size capacity) noexcept;
    /// Answers dropped because the queue was full.
    [[nodiscard]] Foundation::UInt64 DroppedResponses() const noexcept;
    [[nodiscard]] const TerminalScreen& Screen() const noexcept;
    /// Forgets the screen's damage once its rows were drawn.
    void ClearDamage() noexcept;

private:
    TerminalScreen m_screen{};
    TerminalParser m_parser{};
    TerminalResponses m_responses{};
};
} // Namespace Asteria::User::Terminal

#endif // ASTERIA_USER_TERMINAL_TERMINAL_HPP_INCLUDED
