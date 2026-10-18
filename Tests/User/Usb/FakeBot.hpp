// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_USER_USB_FAKEBOT_HPP_INCLUDED
#define ASTERIA_TESTS_USER_USB_FAKEBOT_HPP_INCLUDED

#include "User/Usb/MassStorage.hpp"

#include <cstring>

namespace Asteria::TestSupport
{
namespace Msc = User::Usb;
// Device model owns its wire state, disk and Frame table independently of Bot.
class FakeBot final : public Msc::IMassStoragePipe
{
public:
    Msc::PipeRequest Pending{};
    Msc::UInt8 Frame[65536U]{};
    Msc::UInt8 Disk[131072U]{};
    Msc::UInt8 Response[64U]{};
    Msc::UInt8 Cdb[16U]{};
    Msc::UInt32 Commands[256U]{};
    Msc::UInt32 Clears[3U]{};
    Msc::UInt32 Resets{0U};
    Msc::UInt32 Aborts{0U};
    Msc::UInt32 MaxLive{0U};
    Msc::UInt32 Live{0U};
    Msc::UInt32 Refuse{0U};
    Msc::UInt32 NotReady{0U};
    Msc::UInt8 Attention{0U};
    Msc::UInt8 FaultOpcode{0x28U};
    Msc::UInt8 FailOpcode{0xFFU};
    Msc::UInt8 FailKey{3U};
    Msc::UInt8 FailAsc{0U};
    Msc::UInt8 MaxLun{0U};
    Msc::UInt32 BlockSize{512U};
    Msc::UInt64 BlockCount{256U};
    Msc::UInt32 ShortBy{0U};
    Msc::UInt32 ExtraResidue{0U};
    Msc::UInt8 StallCbw{0U};
    Msc::UInt8 StallData{0U};
    Msc::UInt8 StallCsw{0U};
    Msc::UInt8 ErrorStage{0U};
    bool PhaseError{false};
    bool BadSignature{false};
    bool BadTag{false};
    bool ShortCsw{false};
    bool LunStall{false};
    bool ReadOnly{false};
    bool Mode6Refused{false};
    bool FlushRefused{false};
    bool DescriptorSense{false};
    bool NoMedium{false};
    bool ResetFails{false};
    bool ClearFails{false};
    bool AbortRefused{false};
    bool Valid{true};
    bool Held{false};

    [[nodiscard]] bool Submit(const Msc::PipeRequest& request) noexcept override
    {
        if (Refuse != 0U)
        {
            --Refuse;
            return false;
        }
        if (Held)
        {
            Valid = false;
            return false;
        }
        Pending = request;
        Held = true;
        ++Live;
        if (Live > MaxLive)
        {
            MaxLive = Live;
        }
        return true;
    }
    [[nodiscard]] bool Abort(Msc::UInt16 pipe, Msc::UInt64 token) noexcept override
    {
        if (AbortRefused)
        {
            return false;
        }
        Valid = Valid && Held && (pipe == Pending.Pipe) && (token == Pending.Token);
        ++Aborts;
        m_cancel = true;
        return true;
    }
    [[nodiscard]] Msc::PipeCompletion Reply() noexcept
    {
        Msc::PipeCompletion c{.Token = Pending.Token,
                              .Status = Msc::PipeStatus::Ok,
                              .ActualLength = Pending.Length,
                              .Inline = Response,
                              .InlineLength = 64U};
        Valid = Valid && Held;
        Held = false;
        --Live;
        std::memset(Response, 0, sizeof(Response));
        if (m_cancel)
        {
            m_cancel = false;
            c.Status = Msc::PipeStatus::Cancelled;
            c.ActualLength = 0U;
            return c;
        }
        switch (Pending.Operation)
        {
        case Msc::PipeOperation::ClassControl:
            Valid = Valid && (Pending.Pipe == 0U) && (Pending.Setup[4U] == 3U) && (Pending.Setup[2U] == 0U) &&
                    (Pending.Setup[3U] == 0U) && (Pending.Setup[5U] == 0U) && (Pending.Setup[7U] == 0U);
            if (Pending.Setup[1U] == 0xFEU)
            {
                Valid = Valid && (Pending.Setup[0U] == 0xA1U) && (Pending.Setup[6U] == 1U);
                Response[0U] = MaxLun;
                if (LunStall)
                {
                    c.Status = Msc::PipeStatus::Stall;
                    c.ActualLength = 0U;
                }
            }
            else
            {
                Valid =
                    Valid && (Pending.Setup[1U] == 0xFFU) && (Pending.Setup[0U] == 0x21U) && (Pending.Setup[6U] == 0U);
                ++Resets;
                m_recovery = 1U;
                m_stage = 0U;
                if (ResetFails)
                {
                    c.Status = Msc::PipeStatus::Error;
                }
            }
            break;
        case Msc::PipeOperation::ClearHalt:
            Valid = Valid && (Pending.Pipe < 3U) && (Pending.Length == 0U);
            if (Pending.Pipe < 3U)
            {
                ++Clears[Pending.Pipe];
            }
            if (m_recovery != 0U)
            {
                Valid = Valid && (Pending.Pipe == m_recovery);
                m_recovery = (m_recovery == 1U) ? 2U : 0U;
            }
            if (ClearFails)
            {
                c.Status = Msc::PipeStatus::Error;
            }
            break;
        case Msc::PipeOperation::BulkIn:
        case Msc::PipeOperation::BulkOut:
            Bulk(c);
            break;
        }
        return c;
    }

private:
    void Fail(Msc::UInt8 key, Msc::UInt8 asc, Msc::UInt8 ascq = 0U) noexcept
    {
        m_status = 1U;
        m_key = key;
        m_asc = asc;
        m_ascq = ascq;
    }
    static Msc::UInt32 Little(const Msc::UInt8* bytes) noexcept
    {
        return static_cast<Msc::UInt32>(bytes[0U]) | (static_cast<Msc::UInt32>(bytes[1U]) << 8U) |
               (static_cast<Msc::UInt32>(bytes[2U]) << 16U) | (static_cast<Msc::UInt32>(bytes[3U]) << 24U);
    }
    static void Put(Msc::UInt8* bytes, Msc::UInt64 value, Msc::Size count, bool big) noexcept
    {
        for (Msc::Size i{0U}; i < count; ++i)
        {
            bytes[big ? count - 1U - i : i] = static_cast<Msc::UInt8>((value >> (i * 8U)) & 255U);
        }
    }
    void Bulk(Msc::PipeCompletion& c) noexcept
    {
        if (m_stage == 0U)
        {
            Valid = Valid && (Pending.Pipe == 2U) && (Pending.Operation == Msc::PipeOperation::BulkOut) &&
                    (Pending.Length == 31U) && !Pending.UseFrame && (Little(Pending.Inline) == 0x43425355U) &&
                    (Pending.Inline[13U] == 0U) && (Pending.Inline[14U] > 0U) && (Pending.Inline[14U] <= 16U);
            std::memcpy(Cdb, Pending.Inline + 15U, 16U);
            ++Commands[Cdb[0U]];
            m_tag = Little(Pending.Inline + 4U);
            m_length = Little(Pending.Inline + 8U);
            m_in = Pending.Inline[12U] == 0x80U;
            m_status = 0U;
            m_residue = 0U;
            if ((Cdb[0U] == 0U) && (NotReady != 0U))
            {
                --NotReady;
                Fail(2U, 4U, 1U);
            }
            else if ((Cdb[0U] == 0U) && NoMedium)
            {
                Fail(2U, 0x3AU);
            }
            else if ((Cdb[0U] == 0U) && (Attention != 0U))
            {
                Fail(6U, Attention);
                Attention = 0U;
            }
            if ((Cdb[0U] == 0x1AU) && Mode6Refused)
            {
                Fail(5U, 0x20U);
            }
            if ((Cdb[0U] == 0x35U) && FlushRefused)
            {
                Fail(5U, 0x20U);
            }
            if (Cdb[0U] == FailOpcode)
            {
                Fail(FailKey, FailAsc);
            }
            if ((Cdb[0U] == 0x2AU) && ReadOnly)
            {
                Fail(7U, 0x27U);
            }
            m_stage = (m_length == 0U) ? 2U : 1U;
            if ((Cdb[0U] == FaultOpcode) && (StallCbw != 0U))
            {
                --StallCbw;
                c.Status = Msc::PipeStatus::Stall;
                c.ActualLength = 0U;
            }
            if ((Cdb[0U] == FaultOpcode) && (ErrorStage == 1U))
            {
                c.Status = Msc::PipeStatus::Error;
                c.ActualLength = 0U;
            }
            return;
        }
        if (m_stage == 1U)
        {
            Valid = Valid && (Pending.Pipe == (m_in ? 1U : 2U)) && (Pending.Length == m_length) &&
                    (Pending.Operation == (m_in ? Msc::PipeOperation::BulkIn : Msc::PipeOperation::BulkOut));
            Data(c);
            m_stage = 2U;
            if (Cdb[0U] == FaultOpcode)
            {
                if (ShortBy <= c.ActualLength)
                {
                    c.ActualLength -= ShortBy;
                    m_residue += ShortBy;
                }
                m_residue += ExtraResidue;
                if (StallData != 0U)
                {
                    --StallData;
                    c.Status = Msc::PipeStatus::Stall;
                }
                else if (ShortBy != 0U)
                {
                    c.Status = Msc::PipeStatus::Short;
                }
                if (ErrorStage == 2U)
                {
                    c.Status = Msc::PipeStatus::Error;
                }
            }
            return;
        }
        Valid = Valid && (Pending.Pipe == 1U) && (Pending.Operation == Msc::PipeOperation::BulkIn) &&
                (Pending.Length == 13U) && !Pending.UseFrame;
        if ((Cdb[0U] == FaultOpcode) && (StallCsw != 0U))
        {
            --StallCsw;
            c.Status = Msc::PipeStatus::Stall;
            c.ActualLength = 0U;
            return;
        }
        Put(Response, 0x53425355U, 4U, false);
        Put(Response + 4U, m_tag, 4U, false);
        Put(Response + 8U, m_residue, 4U, false);
        Response[12U] = m_status;
        if (Cdb[0U] == FaultOpcode)
        {
            if (PhaseError)
            {
                Response[12U] = 2U;
            }
            if (BadSignature)
            {
                Response[0U] ^= 1U;
            }
            if (BadTag)
            {
                Response[4U] ^= 1U;
            }
            if (ShortCsw)
            {
                c.ActualLength = 12U;
                c.Status = Msc::PipeStatus::Short;
            }
            if (ErrorStage == 3U)
            {
                c.Status = Msc::PipeStatus::Error;
            }
        }
        m_stage = 0U;
    }
    void Data(Msc::PipeCompletion& c) noexcept
    {
        if (m_status != 0U)
        {
            c.ActualLength = 0U;
            c.Status = Msc::PipeStatus::Short;
            m_residue = m_length;
            return;
        }
        switch (Cdb[0U])
        {
        case 0x12U:
            Response[1U] = 0x80U;
            Response[2U] = 6U;
            Response[3U] = 2U;
            Response[4U] = 31U;
            std::memcpy(Response + 8U, "ASTERIA TEST USB DISK    ", 24U);
            c.ActualLength = 36U;
            break;
        case 3U:
            if (DescriptorSense)
            {
                Response[0U] = 0x72U;
                Response[1U] = m_key;
                Response[2U] = m_asc;
                Response[3U] = m_ascq;
                c.ActualLength = 8U;
            }
            else
            {
                Response[0U] = 0x70U;
                Response[2U] = m_key;
                Response[7U] = 10U;
                Response[12U] = m_asc;
                Response[13U] = m_ascq;
                c.ActualLength = 18U;
            }
            break;
        case 0x25U:
            Put(Response, (BlockCount > 0xFFFFFFFFULL) ? 0xFFFFFFFFULL : BlockCount - 1U, 4U, true);
            Put(Response + 4U, BlockSize, 4U, true);
            c.ActualLength = 8U;
            break;
        case 0x9EU:
            Valid = Valid && (Cdb[1U] == 0x10U) && (Cdb[13U] == 32U);
            Put(Response, BlockCount - 1U, 8U, true);
            Put(Response + 8U, BlockSize, 4U, true);
            c.ActualLength = 32U;
            break;
        case 0x1AU:
            Response[0U] = 3U;
            Response[2U] = ReadOnly ? 0x80U : 0U;
            c.ActualLength = 4U;
            break;
        case 0x5AU:
            Response[1U] = 6U;
            Response[3U] = ReadOnly ? 0x80U : 0U;
            c.ActualLength = 8U;
            break;
        case 0x28U:
        case 0x2AU:
        {
            Msc::UInt32 lba{0U};
            for (Msc::Size i{2U}; i < 6U; ++i)
            {
                lba = (lba << 8U) | Cdb[i];
            }
            const Msc::UInt32 count = (static_cast<Msc::UInt32>(Cdb[7U]) << 8U) | Cdb[8U];
            Valid = Valid && Pending.UseFrame && (Pending.Buffer.Handle.Selector == 7U) &&
                    (Pending.Buffer.Handle.Authenticator == 9U) && (Pending.Buffer.Length == count * 512U) &&
                    (Pending.Buffer.Offset <= sizeof(Frame)) &&
                    (Pending.Length <= sizeof(Frame) - Pending.Buffer.Offset) && (lba < 256U) &&
                    (Pending.Length <= sizeof(Disk) - lba * 512U);
            if (Valid)
            {
                const Msc::UInt32 transfer = (Cdb[0U] == FaultOpcode) ? Pending.Length - ShortBy : Pending.Length;
                if (Cdb[0U] == 0x28U)
                {
                    std::memcpy(Frame + Pending.Buffer.Offset, Disk + lba * 512U, transfer);
                }
                else
                {
                    std::memcpy(Disk + lba * 512U, Frame + Pending.Buffer.Offset, transfer);
                }
            }
            break;
        }
        default:
            break;
        }
        m_residue = m_length - c.ActualLength;
        if (c.ActualLength < m_length)
        {
            c.Status = Msc::PipeStatus::Short;
        }
    }
    Msc::UInt32 m_tag{0U};
    Msc::UInt32 m_length{0U};
    Msc::UInt32 m_residue{0U};
    Msc::UInt8 m_stage{0U};
    Msc::UInt8 m_status{0U};
    Msc::UInt8 m_key{0U};
    Msc::UInt8 m_asc{0U};
    Msc::UInt8 m_ascq{0U};
    Msc::UInt8 m_recovery{0U};
    bool m_in{true};
    bool m_cancel{false};
};
} // namespace Asteria::TestSupport

#endif
