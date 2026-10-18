// SPDX-License-Identifier: Apache-2.0

#include "User/Input/Ps2Controller.hpp"

// Controller bring-up: only the PS/2 component links it; images that only
// quiesce the controller (the root's Device Manager) need Ps2Controller.cpp.
namespace Asteria::User::Input
{
namespace
{
// PC/AT controller configuration byte (IBM PS/2 Technical Reference, 8042
// command 20/60): bit 0 keyboard interrupt, bit 1 auxiliary interrupt, bit 4
// keyboard clock disabled, bit 5 auxiliary clock disabled, bit 6 set 2 to
// set 1 translation, bit 7 reserved (zero).
constexpr UInt8 ConfigurationKeyboardInterrupt{0x01U};
constexpr UInt8 ConfigurationInterrupts{0x03U};
// Clears only the keyboard clock disable (bit 4).
constexpr UInt8 ConfigurationKeepForAuxiliary{0xEFU};
constexpr UInt8 ConfigurationTranslation{0x40U};
constexpr UInt8 ConfigurationReserved{0x80U};
// Unsolicited bytes skipped while a configuration byte or a scan-set number
// is awaited, each under its own wait bound.
constexpr Size UnsolicitedLimit{4U};
}

bool Ps2Controller::Receive(const bool auxiliary, UInt8& value, const bool acknowledgment) noexcept
{
    const UInt64 start = Now();
    for (Size poll{0U};; ++poll)
    {
        const bool expired = Expired(poll, start);
        UInt8 status{0U};
        if (!ReadStatus(status))
        {
            return Fail(Ps2Reason::Transport);
        }
        if (status == 0xFFU)
        {
            return Fail(Ps2Reason::Unexpected);
        }
        if ((status & 1U) != 0U)
        {
            UInt8 byte{0U};
            if (!ReadData(byte))
            {
                return Fail(Ps2Reason::Transport);
            }
            // Parity and timeout errors discard the byte.
            if ((status & 0xC0U) == 0U)
            {
                if ((((status & 0x20U) != 0U) == auxiliary) && (!acknowledgment || (byte == 0xFAU) || (byte == 0xFEU)))
                {
                    value = byte;
                    return true;
                }
                if (!m_transport.Observe(m_transport.Context, status, byte))
                {
                    return Fail(Ps2Reason::Transport);
                }
            }
        }
        if (expired)
        {
            return Fail(Ps2Reason::Timeout);
        }
    }
}

bool Ps2Controller::Device(const bool auxiliary, const UInt8 value) noexcept
{
    for (Size attempt{0U}; attempt < 3U; ++attempt)
    {
        UInt8 response{0U};
        if ((auxiliary && !Write(true, 0xD4U)) || !Write(false, value) || !Receive(auxiliary, response, true))
        {
            return false;
        }
        if (response == 0xFAU)
        {
            return true;
        }
    }
    return Fail(Ps2Reason::Resend);
}

bool Ps2Controller::KeyboardDevice(const Ps2Step step, const UInt8 value) noexcept
{
    m_step = step;
    return Device(false, value);
}

// The configuration byte's reserved bit 7 is zero, so a byte with it set
// (an unsolicited 0xAA or 0xFA from the keyboard) is not the answer.
bool Ps2Controller::ReceiveConfiguration(UInt8& value) noexcept
{
    for (Size skipped{0U}; skipped < UnsolicitedLimit; ++skipped)
    {
        UInt8 byte{0U};
        if (!Receive(false, byte, false))
        {
            return false;
        }
        if ((byte & ConfigurationReserved) == 0U)
        {
            value = byte;
            return true;
        }
        if (!m_transport.Observe(m_transport.Context, m_status, byte))
        {
            return Fail(Ps2Reason::Transport);
        }
    }
    return Fail(Ps2Reason::Unexpected);
}

// F0 00 answers 1, 2 or 3 after its ACK. Through the controller's
// translation those arrive as 0x43, 0x41 or 0x3F (the set-1 bytes of set-2
// codes 01, 02 and 03), which proves translation is still on.
bool Ps2Controller::ReceiveScanSet(UInt8& value) noexcept
{
    for (Size skipped{0U}; skipped < UnsolicitedLimit; ++skipped)
    {
        UInt8 byte{0U};
        if (!Receive(false, byte, false))
        {
            return false;
        }
        if ((byte == 0xFAU) || (byte == 0xAAU))
        {
            continue;
        }
        if ((byte >= 1U) && (byte <= 3U))
        {
            value = byte;
            return true;
        }
        m_translated = (byte == 0x43U) || (byte == 0x41U) || (byte == 0x3FU);
        return Fail(Ps2Reason::Unexpected);
    }
    return Fail(Ps2Reason::Unexpected);
}

// The configuration keeps unrelated bits and the port-disable flags, clears
// translation and the auxiliary interrupt and sets the keyboard interrupt;
// the read-back proves the controller honoured the translation change.
// The scan set is queried while scanning is disabled, so a scan byte cannot
// be mistaken for the set number. No reset or self-test is issued.
bool Ps2Controller::NegotiateSet2(const UInt8 firmware) noexcept
{
    UInt8 readBack{0U};
    UInt8 scanSet{0U};
    m_configuration = static_cast<UInt8>((firmware & 0xBDU) | ConfigurationKeyboardInterrupt);
    if (!Command(Ps2Step::WriteConfiguration, 0x60U) || !Write(false, m_configuration) ||
        !Command(Ps2Step::VerifyConfiguration, 0x20U) || !ReceiveConfiguration(readBack))
    {
        return false;
    }
    if ((readBack & ConfigurationTranslation) != 0U)
    {
        m_translated = true;
        return Fail(Ps2Reason::Unexpected);
    }
    if (!Command(Ps2Step::EnableKeyboard, 0xAEU) || !KeyboardDevice(Ps2Step::KeyboardDisable, 0xF5U) ||
        !KeyboardDevice(Ps2Step::KeyboardGetSet, 0xF0U) || !Device(false, 0U) || !ReceiveScanSet(scanSet))
    {
        return false;
    }
    if ((scanSet != 2U) && (!KeyboardDevice(Ps2Step::KeyboardSetSet2, 0xF0U) || !Device(false, 2U)))
    {
        return false;
    }
    return KeyboardDevice(Ps2Step::KeyboardEnable, 0xF4U);
}

// Firmware and embedded-controller emulations commonly run the keyboard in
// set 2 with the controller translating to set 1 and implement nothing
// else. Restore the firmware's translation (forced on when translation was
// observed), send no scan-set command, and enable scanning: a keyboard that
// does not acknowledge F4 is tolerated, since it may never have stopped.
// Without a configuration byte the controller is left as the firmware set
// it and the PC default, translation on, is assumed.
bool Ps2Controller::FallBack(const bool known, const UInt8 firmware) noexcept
{
    m_fallback = m_failure;
    m_keyboardMode = Ps2KeyboardMode::Set1Translated;
    m_step = Ps2Step::FallbackFlush;
    if (!Drain())
    {
        return false;
    }
    if (known)
    {
        const bool translate = m_translated || ((firmware & ConfigurationTranslation) != 0U);
        m_configuration = static_cast<UInt8>((firmware & 0xBDU) | ConfigurationKeyboardInterrupt |
                                             (translate ? ConfigurationTranslation : 0U));
        if (!Command(Ps2Step::RestoreConfiguration, 0x60U) || !Write(false, m_configuration))
        {
            return false;
        }
        m_keyboardMode = translate ? Ps2KeyboardMode::Set1Translated : Ps2KeyboardMode::Set2Unnegotiated;
    }
    if (!Command(Ps2Step::FallbackEnableKeyboard, 0xAEU))
    {
        return false;
    }
    if (!KeyboardDevice(Ps2Step::FallbackEnable, 0xF4U))
    {
        // A keyboard left waiting for a parameter rejects the first F4.
        static_cast<void>(KeyboardDevice(Ps2Step::FallbackEnable, 0xF4U));
    }
    m_step = Ps2Step::FallbackFlush;
    return Drain();
}

bool Ps2Controller::Initialize(const AuxiliaryPolicy auxiliary) noexcept
{
    UInt8 firmware{0U};
    m_auxiliary = AuxiliaryState::Disabled;
    m_keyboardMode = Ps2KeyboardMode::Set2;
    m_translated = false;
    m_failure = {};
    m_fallback = {};
    if (!Quiesce())
    {
        return false;
    }
    const bool known = Command(Ps2Step::ReadConfiguration, 0x20U) && ReceiveConfiguration(firmware);
    if ((!known || !NegotiateSet2(firmware)) && !FallBack(known, firmware))
    {
        return false;
    }
    if (auxiliary == AuxiliaryPolicy::Probe)
    {
        // The auxiliary configuration derives from the keyboard's, so it
        // needs a known configuration byte.
        m_auxiliary = (known && InitializeMouse()) ? AuxiliaryState::Ready : AuxiliaryState::Failed;
        if (m_auxiliary == AuxiliaryState::Failed)
        {
            // An optional aux device cannot revoke a working keyboard. Disable
            // its interface after the bounded probe; preserve queued scan bytes.
            static_cast<void>(Write(true, 0xA7U));
        }
    }
    return true;
}

// Both interrupts on, keyboard clock enabled, translation as the keyboard
// path decided.
bool Ps2Controller::InitializeMouse() noexcept
{
    UInt8 id{0U};
    if (!Write(true, 0x60U) ||
        !Write(false,
               static_cast<UInt8>((m_configuration & ConfigurationKeepForAuxiliary) | ConfigurationInterrupts)) ||
        !Write(true, 0xA8U) || !Device(true, 0xF6U) || !Device(true, 0xEAU))
    {
        return false;
    }
    constexpr UInt8 rates[]{200U, 100U, 80U};
    for (UInt8 rate : rates)
    {
        if (!Device(true, 0xF3U) || !Device(true, rate))
        {
            return false;
        }
    }
    if (!Device(true, 0xF2U) || !Receive(true, id, false) || ((id != 0U) && (id != 3U)))
    {
        return false;
    }
    m_mode = (id == 3U) ? MouseMode::IntelliMouse : MouseMode::Standard;
    return Device(true, 0xF4U);
}
}
