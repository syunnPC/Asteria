// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_PAGER_INPUT_TERMINAL_HPP_INCLUDED
#define ASTERIA_USER_PAGER_INPUT_TERMINAL_HPP_INCLUDED

#include "User/Component/InputFrame.hpp"
#include "User/Pager/RootContext.hpp"

namespace Asteria::User::Pager
{
/// Twelve seconds of ten-millisecond polls cover the controller's ownership,
/// halt/reset/start waits, power stabilization and five-second enumeration.
inline constexpr Foundation::UInt64 InputDriverReadyAttempts{1200U};
/// How long a repeat that fell due while the terminal was drawing waits on a
/// fresh credit for releases queued meanwhile (the broker answers a credit at
/// once when it holds events, so this only needs to cover a scheduling delay).
inline constexpr Foundation::UInt64 RepeatSettleMicroseconds{100'000U};
/// The PS/2 driver's lines, each with its newline; return the bytes written. A failure code
/// Carrying a diagnostic (Component::Ps2FailureCode) names the step and its evidence; any other
/// nonzero code is shown as it is. The mode line names the keyboard decoding of a ready report's
/// method and, for a fallback, the set-2 failure the report's transaction identifier carries.
inline constexpr Foundation::Size Ps2LineBytes{192U};
[[nodiscard]] Foundation::Size FormatPs2Failure(char (&line)[Ps2LineBytes], Foundation::UInt64 code) noexcept;
[[nodiscard]] Foundation::Size FormatPs2Mode(char (&line)[Ps2LineBytes], Foundation::UInt32 method,
                                             Foundation::UInt64 transaction) noexcept;
/// The terminal's echo while it waits for a restarted driver's readiness: an
/// Events delivery answering its outstanding credit is echoed meanwhile.
struct TerminalEcho;
[[nodiscard]] char InputTerminalCharacter(const Input::InputEvent& event) noexcept;
/// Starts a source component using the ordinary driver bootstrap; readiness
/// means hardware setup succeeded. ServiceFlags selects an xHCI catalog entry.
/// echo, where given, takes the broker's Events that arrive meanwhile.
[[nodiscard]] ComponentRecord* StartInputDriver(ScenarioContext& context, System::CapabilityHandle source,
                                                Foundation::UInt64 epoch, Foundation::UInt64 behavior,
                                                Abi::SystemCallError& error, TerminalEcho* echo = nullptr) noexcept;
/// The restarts a USB input driver (an xHCI driver or the USB host group) gets after it failed
/// while serving; then it is given up.
inline constexpr Foundation::UInt8 InputDriverRestarts{3U};
/// Makes the ready USB input driver driver restartable: started with flags
/// (its start block's word 37), it publishes on the broker's source id
/// through source, which the root keeps. Other drivers are ignored.
void TrackInputDriver(const ComponentRecord& driver, Foundation::UInt64 flags, System::CapabilityHandle source,
                      Foundation::UInt64 sourceId) noexcept;
/// What RecoverInputDrivers did: drivers it found ended, restarted, gave up
/// on (their restarts spent) and left quarantined (a device not proven
/// quiet, which is never restarted).
struct InputRecovery
{
    Foundation::Size Ended;
    Foundation::Size Restarted;
    Foundation::Size GaveUp;
    Foundation::Size Quarantined;
    /// The last driver restarted, null for none.
    ComponentRecord* Driver;
};
/// The Device Manager's recovery of every input driver whose build or thread
/// ended: reports it (`# init: <driver> failed, continuing...`), tears it
/// down (its drain, then the proof that each of its devices is quiet,
/// reported as one `# usb:` line per controller), and restarts a tracked USB
/// driver whose devices were all released and which has restarts left: its
/// broker source registered again (the dead instance's held keys released),
/// a fresh domain started and its readiness awaited (`# init: <driver>
/// restarting (n of 3)`, then `restarted` or a failure that the next pass
/// handles). A driver whose restarts are spent is given up (`# init: <driver>
/// gave up after 3 restarts`), one whose device stays quarantined is not
/// restarted. Without broker nothing restarts. Bounded; the waits serve pager
/// faults and reap components; a refused step affects only that driver.
[[nodiscard]] InputRecovery RecoverInputDrivers(ScenarioContext& context, const ComponentRecord* broker,
                                                TerminalEcho* echo) noexcept;
/// Starts input and available PS/2/xHCI beside display, publishes readiness, then echoes
/// forever. Drivers are optional; failed startup or exit retires only that
/// source. Waits serve pager faults and reap resident components.
[[nodiscard]] PagerServeError StartInputTerminal(ScenarioContext& context, Abi::SystemCallError& error) noexcept;
/// Echoes input, retaining any credit already outstanding on control.
/// The source handle may be replaced when the driver requests registration.
/// Repeats use the launch clock and bootstrap scale, with 500 ms delay/30 Hz;
/// timed supervisor steps still serve faults and reap exits. Driver loss and
/// broker-generated focus-loss/unplug releases cancel held repeat state.
[[nodiscard]] bool EchoInput(ScenarioContext& context, bool creditOutstanding, const ComponentRecord& broker,
                             System::CapabilityHandle control, System::CapabilityHandle& source,
                             Foundation::UInt64& transaction, Abi::SystemCallError& error) noexcept;
/// Reissues the driver's source grant after a controller restart.
[[nodiscard]] bool RegisterInputAgain(ScenarioContext& context, const ComponentRecord& broker,
                                      const Component::InputMessage& request, System::CapabilityHandle& source,
                                      Foundation::UInt64& nextTransaction, Abi::SystemCallError& error) noexcept;
} // Namespace Asteria::User::Pager

#endif // ASTERIA_USER_PAGER_INPUT_TERMINAL_HPP_INCLUDED
