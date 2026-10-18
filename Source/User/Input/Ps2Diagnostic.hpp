// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_INPUT_PS2_DIAGNOSTIC_HPP_INCLUDED
#define ASTERIA_USER_INPUT_PS2_DIAGNOSTIC_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::User::Input
{
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt8;

/// The step of PS/2 bring-up or service a diagnostic names. Values are part
/// of the PS/2 driver's exit code (bits 32-36) and never reordered.
enum class Ps2Step : UInt8
{
    None = 0U,
    Connect = 1U,
    DisableKeyboard = 2U,
    DisableAuxiliary = 3U,
    Flush = 4U,
    ReadConfiguration = 5U,
    WriteConfiguration = 6U,
    VerifyConfiguration = 7U,
    EnableKeyboard = 8U,
    KeyboardDisable = 9U,
    KeyboardGetSet = 10U,
    KeyboardSetSet2 = 11U,
    KeyboardEnable = 12U,
    RestoreConfiguration = 13U,
    FallbackEnableKeyboard = 14U,
    FallbackEnable = 15U,
    FallbackFlush = 16U,
    Arm = 17U,
    Ready = 18U,
    Serve = 19U,
};
inline constexpr UInt8 Ps2StepCount{20U};

/// Why the step failed: no byte or no free input buffer before the bound,
/// an answer outside the protocol, RESEND on every transmission, or a
/// refused port access or system call.
enum class Ps2Reason : UInt8
{
    Unexpected = 0U,
    Timeout = 1U,
    Resend = 2U,
    Transport = 3U,
};

/// The keyboard decoding the driver selected: negotiated untranslated set 2, the controller's set-1
/// translation the firmware left on, or set 2 as the firmware left it untranslated when negotiation
/// failed.
enum class Ps2KeyboardMode : UInt8
{
    Set2 = 0U,
    Set1Translated = 1U,
    Set2Unnegotiated = 2U,
};

/// Evidence of one failed step: the last byte written to either port, the
/// last status byte read from 0x64 and the last byte read from 0x60 (any
/// step, accepted or discarded; DataValid is false before the first).
struct Ps2Diagnostic
{
    Ps2Step Step{Ps2Step::None};
    Ps2Reason Reason{Ps2Reason::Unexpected};
    bool DataValid{false};
    UInt8 Command{0U};
    UInt8 Status{0U};
    UInt8 Data{0U};
};

/// 32-bit wire form: step bits 0-4, reason 5-6, DataValid 7, command 8-15,
/// status 16-23, data 24-31. The driver places it in exit-code bits 32-63.
[[nodiscard]] constexpr UInt32 EncodePs2Diagnostic(const Ps2Diagnostic& diagnostic) noexcept
{
    return (static_cast<UInt32>(diagnostic.Step) & 31U) | ((static_cast<UInt32>(diagnostic.Reason) & 3U) << 5U) |
           (diagnostic.DataValid ? 0x80U : 0U) | (UInt32{diagnostic.Command} << 8U) |
           (UInt32{diagnostic.Status} << 16U) | (UInt32{diagnostic.Data} << 24U);
}

/// Inverse of EncodePs2Diagnostic; an unknown step decodes as None.
[[nodiscard]] constexpr Ps2Diagnostic DecodePs2Diagnostic(const UInt32 value) noexcept
{
    const UInt32 step = value & 31U;
    return Ps2Diagnostic{
        .Step = step < Ps2StepCount ? static_cast<Ps2Step>(step) : Ps2Step::None,
        .Reason = static_cast<Ps2Reason>((value >> 5U) & 3U),
        .DataValid = (value & 0x80U) != 0U,
        .Command = static_cast<UInt8>((value >> 8U) & 0xFFU),
        .Status = static_cast<UInt8>((value >> 16U) & 0xFFU),
        .Data = static_cast<UInt8>(value >> 24U),
    };
}

[[nodiscard]] constexpr const char* Ps2StepText(const Ps2Step step) noexcept
{
    switch (step)
    {
    case Ps2Step::None:
        return "unknown step";
    case Ps2Step::Connect:
        return "interrupt setup";
    case Ps2Step::DisableKeyboard:
        return "controller AD";
    case Ps2Step::DisableAuxiliary:
        return "controller A7";
    case Ps2Step::Flush:
        return "output flush";
    case Ps2Step::ReadConfiguration:
        return "controller 20";
    case Ps2Step::WriteConfiguration:
        return "controller 60";
    case Ps2Step::VerifyConfiguration:
        return "controller 20 read-back";
    case Ps2Step::EnableKeyboard:
        return "controller AE";
    case Ps2Step::KeyboardDisable:
        return "keyboard F5";
    case Ps2Step::KeyboardGetSet:
        return "keyboard F0 00";
    case Ps2Step::KeyboardSetSet2:
        return "keyboard F0 02";
    case Ps2Step::KeyboardEnable:
        return "keyboard F4";
    case Ps2Step::RestoreConfiguration:
        return "fallback controller 60";
    case Ps2Step::FallbackEnableKeyboard:
        return "fallback controller AE";
    case Ps2Step::FallbackEnable:
        return "fallback keyboard F4";
    case Ps2Step::FallbackFlush:
        return "fallback output flush";
    case Ps2Step::Arm:
        return "interrupt arm";
    case Ps2Step::Ready:
        return "ready report";
    case Ps2Step::Serve:
        return "service";
    }
    return "unknown step";
}

[[nodiscard]] constexpr const char* Ps2ReasonText(const Ps2Reason reason) noexcept
{
    switch (reason)
    {
    case Ps2Reason::Unexpected:
        return "unexpected";
    case Ps2Reason::Timeout:
        return "timeout";
    case Ps2Reason::Resend:
        return "resend exhausted";
    case Ps2Reason::Transport:
        return "transport";
    }
    return "unexpected";
}

[[nodiscard]] constexpr const char* Ps2KeyboardModeText(const Ps2KeyboardMode mode) noexcept
{
    switch (mode)
    {
    case Ps2KeyboardMode::Set2:
        return "set2";
    case Ps2KeyboardMode::Set1Translated:
        return "set1-translated";
    case Ps2KeyboardMode::Set2Unnegotiated:
        return "set2-unnegotiated";
    }
    return "set2";
}
} // Namespace Asteria::User::Input

#endif
