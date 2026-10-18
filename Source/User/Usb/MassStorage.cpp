// SPDX-License-Identifier: Apache-2.0

#include "User/Usb/MassStorage.hpp"

namespace Asteria::User::Usb
{
bool MassStorage::Describe() noexcept
{
    if ((m_stage != Stage::Idle) || IsFaulted())
    {
        return false;
    }
    m_candidate = {};
    m_blocks = 0U;
    m_retries = 0U;
    m_retryAt = 0U;
    m_timing = false;
    m_cancelRequested = false;
    m_abortRefusals = 0U;
    Start(Stage::Lun);
    return true;
}
bool MassStorage::Range(bool write, UInt64 generation, UInt64 lba, UInt32 blocks, FrameBuffer buffer,
                        UInt32 maximumBytes) noexcept
{
    if ((m_stage != Stage::Idle) || IsFaulted() || !m_valid || (generation != m_generation) ||
        !m_description.MediumPresent || (write && m_description.ReadOnly) || (lba >= m_description.BlockCount) ||
        (blocks > m_description.BlockCount - lba) ||
        !Scsi::RangeCommand(write, lba, blocks, buffer, maximumBytes, m_command))
    {
        return false;
    }
    m_blocks = blocks;
    m_retries = 0U;
    m_retryAt = 0U;
    m_timing = false;
    m_cancelRequested = false;
    m_abortRefusals = 0U;
    Start(Stage::Io);
    return true;
}
bool MassStorage::Read(UInt64 generation, UInt64 lba, UInt32 blocks, FrameBuffer buffer, UInt32 maximumBytes) noexcept
{
    return Range(false, generation, lba, blocks, buffer, maximumBytes);
}
bool MassStorage::Write(UInt64 generation, UInt64 lba, UInt32 blocks, FrameBuffer buffer, UInt32 maximumBytes) noexcept
{
    return Range(true, generation, lba, blocks, buffer, maximumBytes);
}
bool MassStorage::Flush(UInt64 generation) noexcept
{
    if ((m_stage != Stage::Idle) || IsFaulted() || !m_valid || (generation != m_generation) ||
        !m_description.MediumPresent)
    {
        return false;
    }
    m_blocks = 0U;
    m_retries = 0U;
    m_retryAt = 0U;
    m_timing = false;
    m_cancelRequested = false;
    m_abortRefusals = 0U;
    Start(Stage::Flush);
    return true;
}
void MassStorage::Start(Stage stage) noexcept
{
    m_stage = stage;
    m_started = false;
}
void MassStorage::Finish(StorageStatus status) noexcept
{
    if ((status == StorageStatus::Ok) &&
        ((m_stage == Stage::Mode6) || (m_stage == Stage::Mode10) || (m_stage == Stage::Sense)))
    {
        m_description = m_candidate;
        m_valid = true;
    }
    m_result = {.Status = status,
                .Generation = m_generation,
                .Blocks = (status == StorageStatus::Ok) ? m_blocks : 0U,
                .Description = (status == StorageStatus::Ok) ? m_description : BlockDescription{}};
    m_stage = Stage::Done;
    m_started = false;
}
void MassStorage::Invalidate() noexcept
{
    m_valid = false;
    if (m_generation == ~UInt64{0U})
    {
        m_exhausted = true;
    }
    else
    {
        ++m_generation;
    }
}
void MassStorage::Step(IMassStoragePipe& pipe, UInt64 milliseconds) noexcept
{
    if (milliseconds < m_now)
    {
        return;
    }
    m_now = milliseconds;
    if ((m_stage == Stage::Idle) || (m_stage == Stage::Done))
    {
        return;
    }
    if (!m_timing)
    {
        m_operationAt = m_now;
        m_timing = true;
    }
    if (!m_cancelRequested && !m_deadlineFault && (m_now - m_operationAt >= m_operationMilliseconds))
    {
        // Consume any retained completion before racing it with an Abort.
        if (!m_started || m_bot.IsOutstanding())
        {
            if (Cancel(pipe))
            {
                m_cancelRequested = true;
            }
            else if (++m_abortRefusals == 3U)
            {
                m_deadlineFault = true;
            }
            return;
        }
    }
    if (m_now < m_retryAt)
    {
        return;
    }
    if (m_started && m_bot.TakeResult(m_botResult))
    {
        m_started = false;
        if (m_botResult.StateLost)
        {
            Invalidate();
        }
        if (m_botResult.Status == BotStatus::TransportError)
        {
            Finish(StorageStatus::TransportError);
        }
        else if (m_botResult.Status == BotStatus::Cancelled)
        {
            Finish(StorageStatus::Cancelled);
        }
        else if (m_botResult.Status == BotStatus::CommandFailed)
        {
            if (m_stage == Stage::Sense)
            {
                Finish(StorageStatus::IoError);
            }
            else
            {
                m_failedStage = m_stage;
                Start(Stage::Sense);
            }
        }
        else
        {
            Advance();
        }
        return;
    }
    if (!m_started)
    {
        switch (m_stage)
        {
        case Stage::Lun:
            m_started = m_bot.GetMaxLun();
            break;
        case Stage::Inquiry:
            m_command = Scsi::InquiryCommand();
            break;
        case Stage::Ready:
            m_command = Scsi::ReadyCommand();
            break;
        case Stage::Capacity10:
            m_command = Scsi::CapacityCommand(false);
            break;
        case Stage::Capacity16:
            m_command = Scsi::CapacityCommand(true);
            break;
        case Stage::Mode6:
            m_command = Scsi::ModeCommand(false);
            break;
        case Stage::Mode10:
            m_command = Scsi::ModeCommand(true);
            break;
        case Stage::Flush:
            m_command = Scsi::FlushCommand();
            break;
        case Stage::Sense:
            m_command = Scsi::SenseCommand();
            break;
        case Stage::Io:
            break;
        case Stage::Idle:
        case Stage::Done:
            return;
        }
        if (m_stage != Stage::Lun)
        {
            m_started = m_bot.Begin(m_command);
        }
        if (!m_started)
        {
            Finish(StorageStatus::TransportError);
            return;
        }
    }
    m_bot.Step(pipe);
}
void MassStorage::Advance() noexcept
{
    switch (m_stage)
    {
    case Stage::Lun:
        Start(Stage::Inquiry);
        break;
    case Stage::Inquiry:
    {
        Inquiry inquiry{};
        if (!Scsi::DecodeInquiry(m_botResult.Inline, m_botResult.RelevantLength, inquiry))
        {
            Finish(StorageStatus::IoError);
        }
        else if (inquiry.PeripheralType != 0U)
        {
            Finish(StorageStatus::Unsupported);
        }
        else
        {
            m_candidate.Removable = inquiry.Removable;
            for (Size i{0U}; i < 9U; ++i)
            {
                m_candidate.Vendor[i] = inquiry.Vendor[i];
            }
            for (Size i{0U}; i < 17U; ++i)
            {
                m_candidate.Product[i] = inquiry.Product[i];
            }
            Start(Stage::Ready);
        }
        break;
    }
    case Stage::Ready:
        Start(Stage::Capacity10);
        break;
    case Stage::Capacity10:
    case Stage::Capacity16:
    {
        Capacity capacity{};
        const bool good = (m_stage == Stage::Capacity10)
                              ? Scsi::Capacity10(m_botResult.Inline, m_botResult.RelevantLength, capacity)
                              : Scsi::Capacity16(m_botResult.Inline, m_botResult.RelevantLength, capacity);
        if (!good)
        {
            Finish(StorageStatus::IoError);
        }
        else if (capacity.Saturated)
        {
            Start(Stage::Capacity16);
        }
        else if (capacity.BlockSize != 512U)
        {
            Finish(StorageStatus::Unsupported);
        }
        else
        {
            m_candidate.BlockSize = capacity.BlockSize;
            m_candidate.BlockCount = capacity.BlockCount;
            m_candidate.MediumPresent = true;
            Start(Stage::Mode6);
        }
        break;
    }
    case Stage::Mode6:
    case Stage::Mode10:
        if (!Scsi::ModeSense(m_botResult.Inline, m_botResult.RelevantLength, m_stage == Stage::Mode10,
                             m_candidate.ReadOnly))
        {
            Finish(StorageStatus::IoError);
        }
        else
        {
            Finish(StorageStatus::Ok);
        }
        break;
    case Stage::Io:
        Finish((m_botResult.RelevantLength == m_blocks * 512U) ? StorageStatus::Ok : StorageStatus::IoError);
        break;
    case Stage::Flush:
        Finish(StorageStatus::Ok);
        break;
    case Stage::Sense:
        SenseResult();
        break;
    case Stage::Idle:
    case Stage::Done:
        break;
    }
}
void MassStorage::SenseResult() noexcept
{
    Sense sense{};
    if (!Scsi::DecodeSense(m_botResult.Inline, m_botResult.RelevantLength, sense))
    {
        Finish(StorageStatus::IoError);
        return;
    }
    switch (sense.Status)
    {
    case SenseStatus::MediumChanged:
    case SenseStatus::Reset:
        Invalidate();
        if ((m_failedStage == Stage::Ready) && (++m_retries < 4U) && !m_exhausted)
        {
            Start(Stage::Ready);
        }
        else
        {
            Finish(StorageStatus::MediumChanged);
        }
        break;
    case SenseStatus::BecomingReady:
        if ((m_failedStage == Stage::Ready) && (++m_retries < 4U) && (m_now <= ~UInt64{0U} - 100U))
        {
            m_retryAt = m_now + 100U;
            Start(Stage::Ready);
        }
        else
        {
            Finish(StorageStatus::NotReady);
        }
        break;
    case SenseStatus::NoMedium:
        m_valid = false;
        if (m_failedStage == Stage::Ready)
        {
            m_candidate.BlockSize = 512U;
            Finish(StorageStatus::Ok);
        }
        else
        {
            Finish(StorageStatus::NoMedium);
        }
        break;
    case SenseStatus::IllegalRequest:
        if (m_failedStage == Stage::Mode6)
        {
            Start(Stage::Mode10);
        }
        else if (m_failedStage == Stage::Flush)
        {
            // Avoid publishing a Describe candidate for a successful Flush sense.
            m_stage = Stage::Flush;
            Finish(StorageStatus::Ok);
        }
        else
        {
            Finish(StorageStatus::Unsupported);
        }
        break;
    case SenseStatus::WriteProtected:
        Finish(StorageStatus::ReadOnly);
        break;
    case SenseStatus::NotReady:
        Finish(StorageStatus::NotReady);
        break;
    case SenseStatus::Good:
    case SenseStatus::MediumError:
    case SenseStatus::Error:
        Finish(StorageStatus::IoError);
        break;
    }
}
bool MassStorage::Complete(const PipeCompletion& completion) noexcept
{
    return m_bot.Complete(completion);
}
bool MassStorage::Cancel(IMassStoragePipe& pipe) noexcept
{
    if ((m_stage == Stage::Idle) || (m_stage == Stage::Done))
    {
        return false;
    }
    if (m_started)
    {
        const bool accepted = m_bot.Cancel(pipe);
        if (accepted)
        {
            m_cancelRequested = true;
        }
        return accepted;
    }
    Finish(StorageStatus::Cancelled);
    return true;
}
bool MassStorage::TakeResult(StorageResult& output) noexcept
{
    if (m_stage != Stage::Done)
    {
        return false;
    }
    output = m_result;
    m_stage = Stage::Idle;
    return true;
}
}
