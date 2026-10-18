// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_INPUT_PS2_CONTROLLER_HPP_INCLUDED
#define ASTERIA_USER_INPUT_PS2_CONTROLLER_HPP_INCLUDED

#include "User/Input/Ps2Diagnostic.hpp"
#include "User/Input/Ps2Mouse.hpp"

namespace Asteria::User::Input
{
using Foundation::UInt64;

/// Serialized port adapter. Read returns false on a transport failure; status
/// belongs to the same byte as data. Observe preserves unsolicited scan bytes
/// during command acknowledgments. Clock, when not null, returns a monotonic
/// tick count (the scale is the controller's ticks per millisecond).
struct Ps2Transport
{
    void* Context;
    bool (*Read)(void* context, bool status, UInt8& value) noexcept;
    bool (*Write)(void* context, bool command, UInt8 value) noexcept;
    bool (*Observe)(void* context, UInt8 status, UInt8 value) noexcept;
    UInt64 (*Clock)(void* context) noexcept;
};
enum class AuxiliaryPolicy : UInt8
{
    Disabled,
    Probe,
};
enum class AuxiliaryState : UInt8
{
    Disabled,
    Ready,
    Failed,
};
/// PC/AT 8042 bring-up. Every hardware wait polls at least `polls` times; with a clock it also
/// lasts at least WaitMilliseconds, re-reads status once after that bound, and stops after
/// MaximumPollFactor times `polls` whatever the clock says. A failure records the step and evidence
/// (GetFailure).
class Ps2Controller final
{
public:
    static constexpr UInt64 WaitMilliseconds{500U};
    static constexpr Size MaximumPollFactor{256U};
    explicit constexpr Ps2Controller(Ps2Transport transport, Size polls = 65'536U,
                                     UInt64 ticksPerMillisecond = 0U) noexcept
        : m_transport{transport}, m_polls{polls},
          m_waitTicks{(ticksPerMillisecond <= ~UInt64{0U} / WaitMilliseconds) ? ticksPerMillisecond * WaitMilliseconds
                                                                              : ~UInt64{0U}}
    {
    }
    /// Untranslated scan set 2 first; when a keyboard step or the translation
    /// change fails, restores the firmware's configuration and translation and
    /// Keeps the keyboard scanning without scan-set commands (GetKeyboardMode, GetFallbackReason).
    /// False only when the controller itself fails.
    [[nodiscard]] bool Initialize(AuxiliaryPolicy auxiliary) noexcept;
    [[nodiscard]] AuxiliaryState GetAuxiliaryState() const noexcept
    {
        return m_auxiliary;
    }
    /// Quiet rule: both ports disabled, bounded output drain proves empty.
    [[nodiscard]] bool Quiesce() noexcept;
    /// Reads and observes bytes until status reports the output buffer empty.
    [[nodiscard]] bool Drain() noexcept;
    [[nodiscard]] MouseMode GetMouseMode() const noexcept
    {
        return m_mode;
    }
    [[nodiscard]] Ps2KeyboardMode GetKeyboardMode() const noexcept
    {
        return m_keyboardMode;
    }
    /// The set-2 path's failure when the keyboard mode is a fallback.
    [[nodiscard]] const Ps2Diagnostic& GetFallbackReason() const noexcept
    {
        return m_fallback;
    }
    /// The most recent failure, with the step a caller last named.
    [[nodiscard]] const Ps2Diagnostic& GetFailure() const noexcept
    {
        return m_failure;
    }
    /// The evidence so far, attributed to step (for a caller's own failure).
    [[nodiscard]] Ps2Diagnostic Describe(Ps2Step step, Ps2Reason reason) const noexcept;

private:
    [[nodiscard]] bool Fail(Ps2Reason reason) noexcept;
    [[nodiscard]] bool Expired(Size poll, UInt64 start) const noexcept;
    [[nodiscard]] UInt64 Now() const noexcept;
    [[nodiscard]] bool ReadStatus(UInt8& status) noexcept;
    [[nodiscard]] bool ReadData(UInt8& value) noexcept;
    [[nodiscard]] bool Write(bool command, UInt8 value) noexcept;
    [[nodiscard]] bool Command(Ps2Step step, UInt8 value) noexcept;
    [[nodiscard]] bool Receive(bool auxiliary, UInt8& value, bool acknowledgment) noexcept;
    [[nodiscard]] bool Device(bool auxiliary, UInt8 value) noexcept;
    [[nodiscard]] bool KeyboardDevice(Ps2Step step, UInt8 value) noexcept;
    [[nodiscard]] bool ReceiveConfiguration(UInt8& value) noexcept;
    [[nodiscard]] bool ReceiveScanSet(UInt8& value) noexcept;
    [[nodiscard, gnu::noinline]] bool NegotiateSet2(UInt8 firmware) noexcept;
    [[nodiscard, gnu::noinline]] bool FallBack(bool known, UInt8 firmware) noexcept;
    [[nodiscard, gnu::noinline]] bool InitializeMouse() noexcept;
    Ps2Transport m_transport;
    Size m_polls;
    UInt64 m_waitTicks;
    MouseMode m_mode{MouseMode::Standard};
    AuxiliaryState m_auxiliary{AuxiliaryState::Disabled};
    Ps2KeyboardMode m_keyboardMode{Ps2KeyboardMode::Set2};
    Ps2Step m_step{Ps2Step::None};
    UInt8 m_configuration{0U};
    UInt8 m_command{0U};
    UInt8 m_status{0U};
    UInt8 m_data{0U};
    bool m_dataValid{false};
    bool m_translated{false};
    Ps2Diagnostic m_failure{};
    Ps2Diagnostic m_fallback{};
};
}
#endif
