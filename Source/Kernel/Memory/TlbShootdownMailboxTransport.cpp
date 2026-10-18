// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Memory/TlbShootdownMailboxTransport.hpp"

namespace Asteria::Kernel::Memory
{
namespace
{
constexpr Foundation::UInt64 ReservedSlotState{Foundation::UInt64{1U} << 63U};
constexpr Foundation::ProcessorMask ValidProcessorMask{
    static_cast<Foundation::ProcessorMask>((Foundation::ProcessorMask{1U} << Scheduler::MaximumProcessorCount) - 1U)};
constexpr Foundation::UInt64 ValidPublishedState{ValidProcessorMask};
constexpr Foundation::ProcessorMask SlotIndexMask{
    static_cast<Foundation::ProcessorMask>(TlbShootdownMailboxSlotCount - 1U)};

[[nodiscard]] constexpr bool ConfigurationIsValid(const Foundation::ProcessorMask onlineProcessorMask) noexcept
{
    return (onlineProcessorMask != 0U) &&
           ((onlineProcessorMask & static_cast<Foundation::ProcessorMask>(~ValidProcessorMask)) == 0U);
}

[[nodiscard]] Hal::Memory::TlbShootdownTransportError
ValidateBatch(const TlbShootdownBatch& batch, const Foundation::ProcessorMask onlineProcessorMask,
              Foundation::ProcessorMask& targetProcessorMask) noexcept
{
    const Foundation::UInt8 count = batch.GetCount();
    if ((count == 0U) || (count > Scheduler::MaximumProcessorCount))
    {
        return Hal::Memory::TlbShootdownTransportError::InvalidTarget;
    }

    Foundation::ProcessorMask targetMask{0U};
    for (Foundation::UInt8 index{0U}; index < count; ++index)
    {
        const Scheduler::CpuId target = batch.GetWork(index).GetTargetProcessor();
        if (!target.IsValid())
        {
            return Hal::Memory::TlbShootdownTransportError::InvalidTarget;
        }
        const Foundation::ProcessorMask targetBit =
            static_cast<Foundation::ProcessorMask>(Foundation::ProcessorMask{1U} << target.GetValue());
        if (((onlineProcessorMask & targetBit) == 0U) || ((targetMask & targetBit) != 0U))
        {
            return Hal::Memory::TlbShootdownTransportError::InvalidTarget;
        }
        targetMask = static_cast<Foundation::ProcessorMask>(targetMask | targetBit);
    }
    targetProcessorMask = targetMask;
    return Hal::Memory::TlbShootdownTransportError::None;
}

[[nodiscard]] constexpr bool SlotStateIsValid(const Foundation::UInt64 state) noexcept
{
    return (state == 0U) || (state == ReservedSlotState) || ((state & ~ValidPublishedState) == 0U);
}
} // namespace

Hal::Memory::TlbShootdownTransportError TlbShootdownMailboxTransport::Publish(const TlbShootdownBatch& batch) noexcept
{
    const auto online = GetOnlineProcessorMask();
    if (!ConfigurationIsValid(online))
    {
        return Hal::Memory::TlbShootdownTransportError::Unavailable;
    }

    Foundation::ProcessorMask targetProcessorMask{0U};
    const Hal::Memory::TlbShootdownTransportError validationError = ValidateBatch(batch, online, targetProcessorMask);
    if (validationError != Hal::Memory::TlbShootdownTransportError::None)
    {
        return validationError;
    }

    Slot* reservedSlot{nullptr};
    for (Foundation::UInt8 index{0U}; index < TlbShootdownMailboxSlotCount; ++index)
    {
        Foundation::UInt64 state = m_slots[index].State.LoadAcquire();
        if (!SlotStateIsValid(state))
        {
            return Hal::Memory::TlbShootdownTransportError::Unavailable;
        }
        if (state != 0U)
        {
            continue;
        }
        if (m_slots[index].State.CompareExchangeStrongAcquireRelease(state, ReservedSlotState))
        {
            reservedSlot = &m_slots[index];
            break;
        }
    }
    if (reservedSlot == nullptr)
    {
        return Hal::Memory::TlbShootdownTransportError::QueueFull;
    }

    reservedSlot->Batch = batch;
    reservedSlot->State.StoreRelease(targetProcessorMask);
    m_notifier->Notify(targetProcessorMask);
    return Hal::Memory::TlbShootdownTransportError::None;
}

bool TlbShootdownMailboxTransport::HasPendingWork(const Scheduler::CpuId processor) const noexcept
{
    if (!processor.IsValid())
    {
        return true;
    }
    const Foundation::ProcessorMask bit = Foundation::ProcessorMask{1U} << processor.GetValue();
    for (const Slot& slot : m_slots)
    {
        const Foundation::UInt64 state = slot.State.LoadAcquire();
        if (!SlotStateIsValid(state) || ((state & bit) != 0U))
        {
            return true;
        }
    }
    return false;
}

TlbShootdownReceiveError TlbShootdownMailboxTransport::TryReceive(const Scheduler::CpuId processor,
                                                                  TlbShootdownWork& output) noexcept
{
    const auto online = GetOnlineProcessorMask();
    if (!ConfigurationIsValid(online) || !processor.IsValid())
    {
        return TlbShootdownReceiveError::InvalidProcessor;
    }
    const Foundation::ProcessorMask processorBit =
        static_cast<Foundation::ProcessorMask>(Foundation::ProcessorMask{1U} << processor.GetValue());
    if ((online & processorBit) == 0U)
    {
        return TlbShootdownReceiveError::InvalidProcessor;
    }

    const Foundation::UInt8 firstSlot = m_nextSlot[processor.GetValue()];
    for (Foundation::UInt8 offset{0U}; offset < TlbShootdownMailboxSlotCount; ++offset)
    {
        const Foundation::UInt8 slotIndex =
            static_cast<Foundation::ProcessorMask>((firstSlot + offset) & SlotIndexMask);
        Slot& slot = m_slots[slotIndex];

        for (Foundation::UInt8 attempt{0U}; attempt < Scheduler::MaximumProcessorCount; ++attempt)
        {
            const Foundation::UInt64 state = slot.State.LoadAcquire();
            if (!SlotStateIsValid(state))
            {
                return TlbShootdownReceiveError::CorruptState;
            }
            if ((state == 0U) || (state == ReservedSlotState) || ((state & processorBit) == 0U))
            {
                break;
            }

            const Foundation::UInt8 count = slot.Batch.GetCount();
            if ((count == 0U) || (count > Scheduler::MaximumProcessorCount))
            {
                return TlbShootdownReceiveError::CorruptState;
            }

            const TlbShootdownWork* selectedWork{nullptr};
            Foundation::UInt8 matchingWorkCount{0U};
            for (Foundation::UInt8 workIndex{0U}; workIndex < count; ++workIndex)
            {
                const TlbShootdownWork& work = slot.Batch.GetWork(workIndex);
                if (work.GetTargetProcessor().GetValue() == processor.GetValue())
                {
                    selectedWork = &work;
                    ++matchingWorkCount;
                }
            }
            if ((matchingWorkCount != 1U) || (selectedWork == nullptr))
            {
                return TlbShootdownReceiveError::CorruptState;
            }

            const TlbShootdownWork retainedWork = *selectedWork;
            Foundation::UInt64 expected = state;
            const Foundation::UInt64 desired = state & static_cast<Foundation::UInt64>(~processorBit);
            if (slot.State.CompareExchangeStrongAcquireRelease(expected, desired))
            {
                m_nextSlot[processor.GetValue()] =
                    static_cast<Foundation::ProcessorMask>((slotIndex + 1U) & SlotIndexMask);
                output = retainedWork;
                return TlbShootdownReceiveError::None;
            }
        }
    }
    return TlbShootdownReceiveError::Empty;
}
} // namespace Asteria::Kernel::Memory
