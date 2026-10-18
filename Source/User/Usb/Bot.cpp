// SPDX-License-Identifier: Apache-2.0

#include "User/Usb/Bot.hpp"

namespace Asteria::User::Usb
{
namespace
{
void Store(UInt8* bytes, UInt32 value) noexcept
{
    for (Size i{0U}; i < 4U; ++i)
    {
        bytes[i] = static_cast<UInt8>((value >> (i * 8U)) & 255U);
    }
}
UInt32 Load(const UInt8* bytes) noexcept
{
    UInt32 value{0U};
    for (Size i{0U}; i < 4U; ++i)
    {
        value |= static_cast<UInt32>(bytes[i]) << (i * 8U);
    }
    return value;
}
bool Valid(const ScsiCommand& c) noexcept
{
    return (c.Length > 0U) && (c.Length <= 16U) && (c.TransferLength <= 65536U) &&
           (c.UseFrame ? ((c.Buffer.Length >= c.TransferLength) && (c.Buffer.Offset <= ~UInt64{0U} - c.TransferLength))
                       : (c.TransferLength <= 64U));
}
}
bool BotCodec::Cbw(const ScsiCommand& command, UInt32 tag, UInt8* bytes, Size length) noexcept
{
    if ((bytes == nullptr) || (length < 31U) || !Valid(command))
    {
        return false;
    }
    for (Size i{0U}; i < 31U; ++i)
    {
        bytes[i] = 0U;
    }
    Store(bytes, 0x43425355U);
    Store(bytes + 4U, tag);
    Store(bytes + 8U, command.TransferLength);
    bytes[12U] = command.In ? 0x80U : 0U;
    bytes[14U] = command.Length;
    for (Size i{0U}; i < command.Length; ++i)
    {
        bytes[15U + i] = command.Bytes[i];
    }
    return true;
}
bool BotCodec::DecodeCsw(const UInt8* bytes, Size length, UInt32 tag, UInt32 transferLength, Csw& output) noexcept
{
    if ((bytes == nullptr) || (length != 13U) || (Load(bytes) != 0x53425355U) || (Load(bytes + 4U) != tag) ||
        (bytes[12U] > 2U))
    {
        return false;
    }
    const UInt32 residue = Load(bytes + 8U);
    // Phase error makes residue undefined (BOT 6.7); recovery ignores it.
    if ((bytes[12U] != 2U) && (residue > transferLength))
    {
        return false;
    }
    output = {.Residue = residue, .Status = bytes[12U]};
    return true;
}
bool Bot::GetMaxLun() noexcept
{
    if ((m_phase != Phase::Idle) || m_faulted || (m_binding.InPipe == m_binding.OutPipe) ||
        (m_binding.InPipe == m_binding.ControlPipe) || (m_binding.OutPipe == m_binding.ControlPipe))
    {
        return false;
    }
    m_result = {};
    m_cancelled = false;
    m_phase = Phase::Lun;
    return true;
}
bool Bot::Begin(const ScsiCommand& command) noexcept
{
    if ((m_phase != Phase::Idle) || m_faulted || (m_binding.InPipe == m_binding.OutPipe) ||
        (m_binding.InPipe == m_binding.ControlPipe) || (m_binding.OutPipe == m_binding.ControlPipe) ||
        !Valid(command) || (m_tag == ~UInt32{0U}))
    {
        return false;
    }
    ++m_tag;
    m_command = command;
    m_result = {};
    m_actual = 0U;
    m_cancelled = false;
    m_retryCsw = false;
    m_phase = Phase::Cbw;
    return true;
}
void Bot::Finish(BotStatus status) noexcept
{
    m_result.Status = status;
    m_phase = Phase::Done;
}
void Bot::Recover(BotStatus status) noexcept
{
    m_result.StateLost = true;
    m_result.Status = status;
    m_phase = Phase::Reset;
}
void Bot::Build() noexcept
{
    m_request = {};
    m_request.Token = m_token;
    m_request.Pipe = m_binding.InPipe;
    switch (m_phase)
    {
    case Phase::Lun:
    case Phase::Reset:
        m_request.Operation = PipeOperation::ClassControl;
        m_request.Pipe = m_binding.ControlPipe;
        m_request.Setup[0U] = (m_phase == Phase::Lun) ? 0xA1U : 0x21U;
        m_request.Setup[1U] = (m_phase == Phase::Lun) ? 0xFEU : 0xFFU;
        m_request.Setup[4U] = m_binding.Interface;
        m_request.Setup[6U] = (m_phase == Phase::Lun) ? 1U : 0U;
        m_request.Length = m_request.Setup[6U];
        break;
    case Phase::Cbw:
        m_request.Operation = PipeOperation::BulkOut;
        m_request.Pipe = m_binding.OutPipe;
        m_request.Length = 31U;
        static_cast<void>(BotCodec::Cbw(m_command, m_tag, m_request.Inline, 64U));
        break;
    case Phase::Data:
        m_request.Operation = m_command.In ? PipeOperation::BulkIn : PipeOperation::BulkOut;
        m_request.Pipe = m_command.In ? m_binding.InPipe : m_binding.OutPipe;
        m_request.UseFrame = m_command.UseFrame;
        m_request.Buffer = m_command.Buffer;
        m_request.Buffer.Length = m_command.TransferLength;
        m_request.Length = m_command.TransferLength;
        for (Size i{0U}; i < 64U; ++i)
        {
            m_request.Inline[i] = m_command.Inline[i];
        }
        break;
    case Phase::Csw:
        m_request.Operation = PipeOperation::BulkIn;
        m_request.Length = 13U;
        break;
    case Phase::ClearData:
    case Phase::ClearCsw:
    case Phase::ClearIn:
    case Phase::ClearOut:
        m_request.Operation = PipeOperation::ClearHalt;
        if ((m_phase == Phase::ClearOut) || ((m_phase == Phase::ClearData) && !m_command.In))
        {
            m_request.Pipe = m_binding.OutPipe;
        }
        break;
    case Phase::Idle:
    case Phase::Done:
        break;
    }
}
void Bot::Step(IMassStoragePipe& pipe) noexcept
{
    if (m_outstanding || (m_phase == Phase::Idle) || (m_phase == Phase::Done))
    {
        return;
    }
    if (m_token == ~UInt64{0U})
    {
        m_faulted = true;
        Finish(BotStatus::TransportError);
        return;
    }
    ++m_token;
    Build();
    if (pipe.Submit(m_request))
    {
        m_outstanding = true;
        m_refusals = 0U;
    }
    else if (++m_refusals == 3U)
    {
        m_refusals = 0U;
        // No request owns a buffer. A partially issued command needs recovery.
        if ((m_phase == Phase::Lun) || (m_phase == Phase::Cbw))
        {
            Finish(BotStatus::TransportError);
        }
        else if ((m_phase == Phase::Reset) || (m_phase == Phase::ClearIn) || (m_phase == Phase::ClearOut))
        {
            m_faulted = true;
            Finish(BotStatus::TransportError);
        }
        else
        {
            Recover(BotStatus::TransportError);
        }
    }
}
bool Bot::Complete(const PipeCompletion& c) noexcept
{
    if (!m_outstanding || (c.Token != m_request.Token))
    {
        return false;
    }
    m_outstanding = false;
    if (m_cancelled || (c.Status == PipeStatus::Cancelled))
    {
        m_cancelled = false;
        if ((m_phase == Phase::Reset) || (m_phase == Phase::ClearIn) || (m_phase == Phase::ClearOut))
        {
            // Cancelled recovery is terminal: retrying it can loop forever on detach.
            m_faulted = true;
            Finish(BotStatus::Cancelled);
        }
        else if (m_phase == Phase::Lun)
        {
            Finish(BotStatus::Cancelled);
        }
        else
        {
            Recover(BotStatus::Cancelled);
        }
        return true;
    }
    const bool good = (c.Status == PipeStatus::Ok) || (c.Status == PipeStatus::Short);
    const bool inlineIn =
        !m_request.UseFrame && ((m_request.Operation == PipeOperation::BulkIn) || ((m_phase == Phase::Lun) && good));
    if ((c.ActualLength > m_request.Length) ||
        (inlineIn && (good || (c.Status == PipeStatus::Stall)) && (c.ActualLength != 0U) &&
         ((c.Inline == nullptr) || (c.InlineLength < c.ActualLength))))
    {
        if ((m_phase == Phase::Reset) || (m_phase == Phase::ClearIn) || (m_phase == Phase::ClearOut))
        {
            m_faulted = true;
            Finish(BotStatus::TransportError);
        }
        else
        {
            Recover(BotStatus::TransportError);
        }
        return true;
    }
    switch (m_phase)
    {
    case Phase::Lun:
        if (c.Status == PipeStatus::Stall)
        {
            Finish(BotStatus::Passed);
        }
        else if (good && (c.ActualLength == 1U) && (c.Inline[0U] <= 15U))
        {
            m_result.MaxLun = c.Inline[0U];
            Finish(BotStatus::Passed);
        }
        else
        {
            Finish(BotStatus::TransportError);
        }
        break;
    case Phase::Cbw:
        if (good && (c.ActualLength == 31U))
        {
            m_phase = (m_command.TransferLength == 0U) ? Phase::Csw : Phase::Data;
        }
        else
        {
            Recover(BotStatus::TransportError);
        }
        break;
    case Phase::Data:
        m_actual = c.ActualLength;
        if (inlineIn && (good || (c.Status == PipeStatus::Stall)))
        {
            for (Size i{0U}; i < c.ActualLength; ++i)
            {
                m_result.Inline[i] = c.Inline[i];
            }
        }
        if (good)
        {
            m_phase = Phase::Csw;
        }
        else if (c.Status == PipeStatus::Stall)
        {
            m_phase = Phase::ClearData;
        }
        else
        {
            Recover(BotStatus::TransportError);
        }
        break;
    case Phase::Csw:
    {
        Csw csw{};
        if ((c.Status == PipeStatus::Stall) && !m_retryCsw)
        {
            m_retryCsw = true;
            m_phase = Phase::ClearCsw;
        }
        else if (!good || !BotCodec::DecodeCsw(c.Inline, c.ActualLength, m_tag, m_command.TransferLength, csw) ||
                 (csw.Status == 2U))
        {
            Recover(BotStatus::TransportError);
        }
        else if (m_command.TransferLength - csw.Residue > m_actual)
        {
            Recover(BotStatus::TransportError);
        }
        else
        {
            m_result.RelevantLength = m_command.TransferLength - csw.Residue;
            Finish((csw.Status == 0U) ? BotStatus::Passed : BotStatus::CommandFailed);
        }
        break;
    }
    case Phase::ClearData:
    case Phase::ClearCsw:
        if (good)
        {
            m_phase = Phase::Csw;
        }
        else
        {
            Recover(BotStatus::TransportError);
        }
        break;
    case Phase::Reset:
    case Phase::ClearIn:
    case Phase::ClearOut:
        if (!good)
        {
            m_faulted = true;
            Finish(BotStatus::TransportError);
        }
        else if (m_phase == Phase::Reset)
        {
            m_phase = Phase::ClearIn;
        }
        else if (m_phase == Phase::ClearIn)
        {
            m_phase = Phase::ClearOut;
        }
        else
        {
            Finish(m_result.Status);
        }
        break;
    case Phase::Idle:
    case Phase::Done:
        return false;
    }
    return true;
}
bool Bot::Cancel(IMassStoragePipe& pipe) noexcept
{
    if ((m_phase == Phase::Idle) || (m_phase == Phase::Done) || m_cancelled)
    {
        return false;
    }
    if (m_outstanding)
    {
        if (!pipe.Abort(m_request.Pipe, m_request.Token))
        {
            return false;
        }
        m_cancelled = true;
    }
    else if ((m_phase == Phase::Lun) || (m_phase == Phase::Cbw))
    {
        Finish(BotStatus::Cancelled);
    }
    else if ((m_phase == Phase::Reset) || (m_phase == Phase::ClearIn) || (m_phase == Phase::ClearOut))
    {
        m_faulted = true;
        Finish(BotStatus::Cancelled);
    }
    else
    {
        Recover(BotStatus::Cancelled);
    }
    return true;
}
bool Bot::TakeResult(BotResult& output) noexcept
{
    if (m_phase != Phase::Done)
    {
        return false;
    }
    output = m_result;
    m_phase = Phase::Idle;
    return true;
}
}
