// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TEST_PS2_CONTROLLER_MODEL_HPP_INCLUDED
#define ASTERIA_TEST_PS2_CONTROLLER_MODEL_HPP_INCLUDED
#include "User/Input/Ps2Controller.hpp"
#include <vector>
namespace Asteria::Test::Input
{
namespace In = User::Input;
struct Byte
{
    In::UInt8 Status;
    In::UInt8 Value;
};
class Model final
{
public:
    std::vector<Byte> Output{};
    std::vector<unsigned> Writes{};
    std::vector<unsigned> Observed{};
    unsigned Delay{0U};
    unsigned Latency{0U};
    unsigned Resends{0U};
    unsigned AuxResends{0U};
    bool Wheel{true};
    bool Inject{false};
    bool InjectAux{false};
    bool Stuck{false};
    bool Absent{false};
    bool AuxAbsent{false};
    bool ConfigNext{false};
    bool AuxNext{false};
    bool KeyboardParameter{false};
    bool MouseParameter{false};
    In::UInt8 Config{0x47U};
    In::UInt8 ScanSet{2U};
    In::UInt8 Id{0U};
    unsigned RateAt{0U};
    bool Valid{true};
    // Embedded-controller behaviours: the translation bit stays set, F0 is
    // always answered RESEND, F0 00 is answered through translation, the
    // keyboard never answers, 0xAA/0xFA precede answers, 0x20 is ignored.
    bool TranslationLocked{false};
    bool RejectScanSet{false};
    bool TranslatedAnswer{false};
    bool SilentKeyboard{false};
    bool Unsolicited{false};
    bool MuteConfiguration{false};
    // Clock: every status read advances it.
    unsigned long long Ticks{0U};
    unsigned TicksPerRead{0U};

    bool Read(bool status, In::UInt8& value) noexcept
    {
        if (status)
        {
            Ticks += TicksPerRead;
            value = Absent ? 0xFFU : (Stuck ? 2U : 0U);
            if (Delay != 0U)
            {
                --Delay;
                value |= 2U;
            }
            else if (!Output.empty())
            {
                value |= static_cast<In::UInt8>(1U | Output.front().Status);
            }
            return true;
        }
        if (Output.empty() || Delay != 0U)
        {
            Valid = false;
            return false;
        }
        value = Output.front().Value;
        Output.erase(Output.begin());
        return true;
    }
    bool Write(bool command, In::UInt8 value) noexcept
    {
        Writes.push_back((command ? 0x100U : 0U) | value);
        if (command)
        {
            switch (value)
            {
            case 0xADU:
                Config |= 16U;
                break;
            case 0xA7U:
                Config |= 32U;
                break;
            case 0xAEU:
                Config &= 0xEFU;
                break;
            case 0xA8U:
                Config &= 0xDFU;
                break;
            case 0x20U:
                if (MuteConfiguration)
                {
                    break;
                }
                if (Unsolicited)
                {
                    Output.push_back(Byte{0U, 0xAAU});
                }
                Output.push_back(Byte{0U, Config});
                break;
            case 0x60U:
                ConfigNext = true;
                break;
            case 0xD4U:
                AuxNext = true;
                break;
            default:
                Valid = false;
                return false;
            }
            return true;
        }
        if (ConfigNext)
        {
            ConfigNext = false;
            Config = static_cast<In::UInt8>(TranslationLocked ? (value | 0x40U) : value);
            return true;
        }
        const bool auxiliary = AuxNext;
        AuxNext = false;
        if ((auxiliary && AuxAbsent) || (!auxiliary && SilentKeyboard))
        {
            return true;
        }
        if (!auxiliary && RejectScanSet && !KeyboardParameter && (value == 0xF0U))
        {
            Output.push_back(Byte{0U, 0xFEU});
            return true;
        }
        const In::UInt8 status = auxiliary ? 32U : 0U;
        if (Inject || (InjectAux && auxiliary))
        {
            Output.push_back(Byte{static_cast<In::UInt8>(auxiliary ? 0U : 32U), 0x1CU});
            Output.push_back(Byte{status, 0x1BU});
            Inject = false;
            if (auxiliary)
            {
                InjectAux = false;
            }
        }
        unsigned& resends = auxiliary ? AuxResends : Resends;
        if (resends != 0U)
        {
            --resends;
            Output.push_back(Byte{status, 0xFEU});
            return true;
        }
        Output.push_back(Byte{status, 0xFAU});
        Delay = Latency;
        if (!auxiliary)
        {
            if (KeyboardParameter)
            {
                KeyboardParameter = false;
                if (value == 0U)
                {
                    if (Unsolicited)
                    {
                        Output.push_back(Byte{0U, 0xFAU});
                        Output.push_back(Byte{0U, 0xAAU});
                    }
                    // Set-2 codes 01/02/03 through translation (F9, -, F5).
                    constexpr In::UInt8 translated[]{0x43U, 0x41U, 0x3FU};
                    Output.push_back(Byte{0U, (TranslatedAnswer && (ScanSet >= 1U) && (ScanSet <= 3U))
                                                  ? translated[ScanSet - 1U]
                                                  : ScanSet});
                }
                else
                {
                    ScanSet = value;
                }
            }
            else if (value == 0xF0U)
            {
                KeyboardParameter = true;
            }
            else if ((value != 0xF5U) && (value != 0xF4U))
            {
                Valid = false;
            }
        }
        else if (MouseParameter)
        {
            MouseParameter = false;
            constexpr In::UInt8 rates[]{200U, 100U, 80U};
            if ((RateAt >= 3U) || (value != rates[RateAt]))
            {
                Valid = false;
            }
            ++RateAt;
            Id = (Wheel && (RateAt == 3U)) ? 3U : 0U;
        }
        else if (value == 0xF3U)
        {
            MouseParameter = true;
        }
        else if (value == 0xF2U)
        {
            Output.push_back(Byte{32U, Id});
        }
        else if ((value != 0xF6U) && (value != 0xEAU) && (value != 0xF4U))
        {
            Valid = false;
        }
        return Valid;
    }
    static bool ReadPort(void* self, bool status, In::UInt8& value) noexcept
    {
        return static_cast<Model*>(self)->Read(status, value);
    }
    static bool WritePort(void* self, bool command, In::UInt8 value) noexcept
    {
        return static_cast<Model*>(self)->Write(command, value);
    }
    static bool Observe(void* self, In::UInt8 status, In::UInt8 value) noexcept
    {
        static_cast<Model*>(self)->Observed.push_back((static_cast<unsigned>(status) << 8U) | value);
        return true;
    }
    static unsigned long long Clock(void* self) noexcept
    {
        return static_cast<Model*>(self)->Ticks;
    }
    In::Ps2Transport Transport() noexcept
    {
        return In::Ps2Transport{this, ReadPort, WritePort, Observe, Clock};
    }
};
} // namespace Asteria::Test::Input
#endif
