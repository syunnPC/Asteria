// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_FIRMWARE_DIAGNOSTIC_LINE_SINK_HPP_INCLUDED
#define ASTERIA_PLATFORM_FIRMWARE_DIAGNOSTIC_LINE_SINK_HPP_INCLUDED

#include "Kernel/Diagnostics/DiagnosticChannel.hpp"

namespace Asteria::Platform::Firmware
{
/// Emits DiagnosticChannel lines on the early debug channel as
/// `USER:<text>` lines. Each line is written by one caller in one pass with
/// a leading line terminator, so it starts at a line boundary even if a
/// kernel diagnostic line was left unterminated, and a user line can never
/// begin with the kernel's `ASTERIA:` prefix. The system-call paths that
/// reach this sink run with local interrupts disabled, so nothing on the
/// same processor interleaves with a line; another processor's diagnostics
/// could, which the host detects as a malformed or missing test line.
class DiagnosticLineSink final : public Kernel::Diagnostics::IDiagnosticLineSink
{
public:
    constexpr DiagnosticLineSink() noexcept = default;

    void WriteLine(const Foundation::UInt8* text, Foundation::Size lengthBytes) noexcept override;
};
} // namespace Asteria::Platform::Firmware

#endif // ASTERIA_PLATFORM_FIRMWARE_DIAGNOSTIC_LINE_SINK_HPP_INCLUDED
