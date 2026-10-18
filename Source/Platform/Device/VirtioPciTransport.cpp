// SPDX-License-Identifier: Apache-2.0

#include "Platform/Device/VirtioPciTransport.hpp"

#include "Platform/Synchronization/DeviceMemoryBarrier.hpp"

namespace Asteria::Platform::Device
{
namespace
{
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;
using Hal::Device::MemoryMappedIoWidth;
using Hal::Device::PciConfigurationReadError;
constexpr UInt64 MaximumAddress{~UInt64{0U}};
constexpr UInt64 SupportedFeatures{VirtioPciTransport::Version1 | 0x00FF'FFFFU};

// Both target profiles are little endian. Naturally aligned ring indices
// require a single 16-bit access; byte accesses could tear at counter wrap.
[[nodiscard]] UInt16 RingIndex(const volatile UInt8* bytes) noexcept
{
    return *static_cast<const volatile UInt16*>(static_cast<const volatile void*>(bytes));
}
void RingIndex(volatile UInt8* bytes, const UInt16 value) noexcept
{
    *static_cast<volatile UInt16*>(static_cast<volatile void*>(bytes)) = value;
}
} // namespace

VirtioPciTransport::VirtioPciTransport(Hal::Device::IPciConfigurationSpace& pci, Hal::Device::IMemoryMappedIo& reader,
                                       Hal::Device::IMemoryMappedIoWriter& writer) noexcept
    : m_pci{pci}, m_reader{reader}, m_writer{writer}
{
}

bool VirtioPciTransport::PageIsValid(const VirtioDmaPage& page) noexcept
{
    return (page.Bytes != nullptr) && ((reinterpret_cast<Foundation::UIntPointer>(page.Bytes) & 4095U) == 0U) &&
           ((page.PhysicalAddress & 4095U) == 0U) && (page.PhysicalAddress <= MaximumAddress - 4095U);
}

void VirtioPciTransport::Store(volatile UInt8* const bytes, const Size offset, const Size width,
                               const UInt64 value) noexcept
{
    for (Size index{0U}; (index < width) && (index < 8U); ++index)
    {
        bytes[offset + index] = static_cast<UInt8>((value >> (8U * index)) & 0xFFU);
    }
}

UInt64 VirtioPciTransport::Load(const volatile UInt8* const bytes, const Size offset, const Size width) noexcept
{
    UInt64 value{0U};
    for (Size index{0U}; (index < width) && (index < 8U); ++index)
    {
        value |= UInt64{bytes[offset + index]} << (8U * index);
    }
    return value;
}

VirtioTransportError VirtioPciTransport::Discover(const Kernel::Device::PciFunctionAddress function,
                                                  const VirtioPciBar (&bars)[6U]) noexcept
{
    if (m_discovered || !function.IsValid())
    {
        return VirtioTransportError::InvalidArguments;
    }
    for (Window& window : m_windows)
    {
        window = Window{};
    }
    m_function = function;
    UInt32 value{0U};
    if (m_pci.Read32(function, 0U, value) != PciConfigurationReadError::None)
    {
        return VirtioTransportError::HardwareFailure;
    }
    if (((value & 0xFFFFU) != 0x1AF4U) || ((value >> 16U) < 0x1000U) || ((value >> 16U) > 0x107FU))
    {
        return VirtioTransportError::MalformedDevice;
    }
    if ((m_pci.Read32(function, 4U, value) != PciConfigurationReadError::None) || ((value & 0x0010'0000U) == 0U) ||
        (m_pci.Read32(function, 0x0CU, value) != PciConfigurationReadError::None) || ((value & 0x007F'0000U) != 0U) ||
        (m_pci.Read32(function, 0x34U, value) != PciConfigurationReadError::None))
    {
        return VirtioTransportError::MalformedDevice;
    }
    UInt16 offset = static_cast<UInt16>(value & 0xFFU);
    UInt64 visited{0U};
    for (UInt16 count{0U}; (offset != 0U) && (count < 48U); ++count)
    {
        if ((offset < 0x40U) || (offset > 0xFCU) || ((offset & 3U) != 0U) ||
            ((visited & (UInt64{1U} << (offset / 4U))) != 0U))
        {
            return VirtioTransportError::MalformedDevice;
        }
        visited |= UInt64{1U} << (offset / 4U);
        if (m_pci.Read32(function, offset, value) != PciConfigurationReadError::None)
        {
            return VirtioTransportError::HardwareFailure;
        }
        if ((value & 0xFFU) == 9U)
        {
            const VirtioTransportError error = Capability(offset, value, bars);
            if (error != VirtioTransportError::None)
            {
                return error;
            }
        }
        offset = static_cast<UInt16>((value >> 8U) & 0xFFU);
    }
    if ((offset != 0U) || (m_windows[0U].Length == 0U) || (m_windows[1U].Length == 0U) || (m_windows[2U].Length == 0U))
    {
        return VirtioTransportError::MalformedDevice;
    }
    m_discovered = true;
    return VirtioTransportError::None;
}

VirtioTransportError VirtioPciTransport::Capability(const UInt16 offset, const UInt32 header,
                                                    const VirtioPciBar (&bars)[6U]) noexcept
{
    const UInt32 length = (header >> 16U) & 0xFFU;
    const UInt32 type = header >> 24U;
    if ((type < 1U) || (type > 4U) || (m_windows[type - 1U].Length != 0U))
    {
        return VirtioTransportError::None;
    }
    UInt32 barField{0U};
    // Read only the BAR selector before deciding whether to ignore this cap.
    if (offset > 248U)
    {
        return VirtioTransportError::MalformedDevice;
    }
    if (m_pci.Read32(m_function, static_cast<UInt16>(offset + 4U), barField) != PciConfigurationReadError::None)
    {
        return VirtioTransportError::HardwareFailure;
    }
    const UInt32 bar = barField & 0xFFU;
    if (bar >= 6U)
    {
        return VirtioTransportError::None;
    }
    if ((length < 16U) || (length > 256U - offset))
    {
        return VirtioTransportError::MalformedDevice;
    }
    UInt32 start{0U};
    UInt32 bytes{0U};
    if ((m_pci.Read32(m_function, static_cast<UInt16>(offset + 8U), start) != PciConfigurationReadError::None) ||
        (m_pci.Read32(m_function, static_cast<UInt16>(offset + 12U), bytes) != PciConfigurationReadError::None))
    {
        return VirtioTransportError::HardwareFailure;
    }
    const UInt32 minimum = (type == 1U) ? 56U : ((type == 2U) ? 2U : 1U);
    if ((bytes < minimum) || (bars[bar].Length == 0U) || (start > bars[bar].Length) ||
        (bytes > bars[bar].Length - start) || (bars[bar].Base > MaximumAddress - bars[bar].Length) ||
        (((type <= 2U) || (type == 4U)) && ((start & 3U) != 0U)))
    {
        return VirtioTransportError::MalformedDevice;
    }
    // Validate that the selected slot is a memory BAR, not the high half of
    // a 64-bit BAR, and agrees with the platform's retained BAR authority.
    UInt64 base{0U};
    for (UInt32 slot{0U}; slot <= bar; ++slot)
    {
        UInt32 low{0U};
        if (m_pci.Read32(m_function, static_cast<UInt16>(0x10U + slot * 4U), low) != PciConfigurationReadError::None)
        {
            return VirtioTransportError::HardwareFailure;
        }
        if (slot == bar)
        {
            if (((low & 1U) != 0U) || (((low >> 1U) & 3U) == 1U) || (((low >> 1U) & 3U) == 3U))
            {
                return VirtioTransportError::MalformedDevice;
            }
            base = low & 0xFFFF'FFF0U;
        }
        if ((low & 7U) == 4U)
        {
            if ((slot == 5U) || (slot + 1U == bar))
            {
                return VirtioTransportError::MalformedDevice;
            }
            UInt32 high{0U};
            if (m_pci.Read32(m_function, static_cast<UInt16>(0x14U + slot * 4U), high) !=
                PciConfigurationReadError::None)
            {
                return VirtioTransportError::HardwareFailure;
            }
            if (slot == bar)
            {
                base |= UInt64{high} << 32U;
            }
            ++slot;
        }
    }
    if ((base != bars[bar].Base) || (base == 0U))
    {
        return VirtioTransportError::MalformedDevice;
    }
    UInt32 multiplier{0U};
    if ((type == 2U) && ((length < 20U) || (m_pci.Read32(m_function, static_cast<UInt16>(offset + 16U), multiplier) !=
                                            PciConfigurationReadError::None)))
    {
        return VirtioTransportError::MalformedDevice;
    }
    if (m_windows[type - 1U].Length == 0U)
    {
        for (const Window& other : m_windows)
        {
            if ((other.Length != 0U) && (base + start < other.Base + other.Length) &&
                (other.Base < base + start + bytes))
            {
                return VirtioTransportError::MalformedDevice;
            }
        }
        m_windows[type - 1U] = Window{.Base = base + start, .Length = bytes};
        if (type == 2U)
        {
            m_notifyMultiplier = multiplier;
        }
    }
    return VirtioTransportError::None;
}

VirtioTransportError VirtioPciTransport::Read(const Window& window, const UInt32 offset,
                                              const MemoryMappedIoWidth width, UInt64& output) noexcept
{
    const UInt32 bytes = static_cast<UInt32>(width);
    if ((offset > window.Length) || (bytes > window.Length - offset) || ((offset & (bytes - 1U)) != 0U))
    {
        return VirtioTransportError::MalformedDevice;
    }
    if (m_reader.Read(window.Base + offset, width, output) != Hal::Device::MemoryMappedIoReadError::None)
    {
        return VirtioTransportError::HardwareFailure;
    }
    if ((bytes < 8U) && (output >= (UInt64{1U} << (bytes * 8U))))
    {
        return VirtioTransportError::MalformedDevice;
    }
    return VirtioTransportError::None;
}

VirtioTransportError VirtioPciTransport::Write(const Window& window, const UInt32 offset,
                                               const MemoryMappedIoWidth width, const UInt64 value) noexcept
{
    const UInt32 bytes = static_cast<UInt32>(width);
    if ((offset > window.Length) || (bytes > window.Length - offset) || ((offset & (bytes - 1U)) != 0U))
    {
        return VirtioTransportError::MalformedDevice;
    }
    return (m_writer.Write(window.Base + offset, width, value) == Hal::Device::MemoryMappedIoWriteError::None)
               ? VirtioTransportError::None
               : VirtioTransportError::HardwareFailure;
}

VirtioTransportError VirtioPciTransport::Fail(const VirtioTransportError error) noexcept
{
    m_failed = true;
    return error;
}

VirtioTransportError VirtioPciTransport::Reset(const UInt32 polls) noexcept
{
    if (!m_discovered || (polls == 0U) || (polls > MaximumPolls))
    {
        return VirtioTransportError::InvalidArguments;
    }
    if (Write(m_windows[0U], 20U, MemoryMappedIoWidth::Byte, 0U) != VirtioTransportError::None)
    {
        return Fail(VirtioTransportError::HardwareFailure);
    }
    for (UInt32 poll{0U}; poll < polls; ++poll)
    {
        UInt64 status{0U};
        const VirtioTransportError error = Read(m_windows[0U], 20U, MemoryMappedIoWidth::Byte, status);
        if (error != VirtioTransportError::None)
        {
            return Fail(error);
        }
        if (status == 0U)
        {
            for (Queue& queue : m_queues)
            {
                for (Size index{0U}; index < MaximumQueueSize; ++index)
                {
                    queue.WritableBytes[index] = 0U;
                    queue.Occupied[index] = 0U;
                }
                for (VirtioDmaPage& page : queue.Pages)
                {
                    page = VirtioDmaPage{};
                }
                queue.Size = 0U;
                queue.Available = 0U;
                queue.Used = 0U;
                queue.Outstanding = 0U;
                queue.NotifyAddress = 0U;
            }
            m_features = 0U;
            m_started = false;
            m_negotiated = false;
            m_reset = true;
            m_failed = false;
            return VirtioTransportError::None;
        }
    }
    return Fail(VirtioTransportError::TimedOut);
}

VirtioTransportError VirtioPciTransport::Negotiate(const UInt64 required, const UInt64 optional) noexcept
{
    if (!m_discovered || !m_reset || m_failed || m_started || m_negotiated)
    {
        return VirtioTransportError::NotReady;
    }
    if (((required | optional) & ~SupportedFeatures) != 0U)
    {
        return VirtioTransportError::UnsupportedFeatures;
    }
    if (Write(m_windows[0U], 20U, MemoryMappedIoWidth::Byte, 1U) != VirtioTransportError::None ||
        Write(m_windows[0U], 20U, MemoryMappedIoWidth::Byte, 3U) != VirtioTransportError::None)
    {
        return Fail(VirtioTransportError::HardwareFailure);
    }
    UInt64 offered{0U};
    for (UInt32 half{0U}; half < 2U; ++half)
    {
        UInt64 value{0U};
        if (Write(m_windows[0U], 0U, MemoryMappedIoWidth::DoubleWord, half) != VirtioTransportError::None ||
            Read(m_windows[0U], 4U, MemoryMappedIoWidth::DoubleWord, value) != VirtioTransportError::None)
        {
            return Fail(VirtioTransportError::HardwareFailure);
        }
        offered |= value << (half * 32U);
    }
    const UInt64 needed = required | Version1;
    if ((offered & needed) != needed)
    {
        return Fail(VirtioTransportError::UnsupportedFeatures);
    }
    m_features = needed | (offered & optional);
    for (UInt32 half{0U}; half < 2U; ++half)
    {
        if (Write(m_windows[0U], 8U, MemoryMappedIoWidth::DoubleWord, half) != VirtioTransportError::None ||
            Write(m_windows[0U], 12U, MemoryMappedIoWidth::DoubleWord, (m_features >> (half * 32U)) & 0xFFFF'FFFFU) !=
                VirtioTransportError::None)
        {
            return Fail(VirtioTransportError::HardwareFailure);
        }
    }
    UInt64 status{0U};
    if (Write(m_windows[0U], 20U, MemoryMappedIoWidth::Byte, 11U) != VirtioTransportError::None ||
        Read(m_windows[0U], 20U, MemoryMappedIoWidth::Byte, status) != VirtioTransportError::None ||
        ((status & 11U) != 11U) || ((status & 0xC0U) != 0U))
    {
        return Fail(VirtioTransportError::UnsupportedFeatures);
    }
    m_negotiated = true;
    return VirtioTransportError::None;
}

VirtioTransportError VirtioPciTransport::SetupQueue(const UInt16 queueId, const VirtioDmaPage (&pages)[3U],
                                                    const UInt16 limit) noexcept
{
    if (!m_negotiated || m_started || m_failed || (queueId >= MaximumQueues) || (m_queues[queueId].Size != 0U))
    {
        return VirtioTransportError::NotReady;
    }
    if ((limit == 0U) || (limit > MaximumQueueSize))
    {
        return VirtioTransportError::InvalidArguments;
    }
    for (Size page{0U}; page < 3U; ++page)
    {
        if (!PageIsValid(pages[page]))
        {
            return VirtioTransportError::InvalidArguments;
        }
        for (Size previous{0U}; previous < page; ++previous)
        {
            if ((pages[page].PhysicalAddress == pages[previous].PhysicalAddress) ||
                (pages[page].Bytes == pages[previous].Bytes))
            {
                return VirtioTransportError::InvalidArguments;
            }
        }
        for (const Queue& queue : m_queues)
        {
            for (const VirtioDmaPage& other : queue.Pages)
            {
                if ((queue.Size != 0U) &&
                    ((other.PhysicalAddress == pages[page].PhysicalAddress) || (other.Bytes == pages[page].Bytes)))
                {
                    return VirtioTransportError::InvalidArguments;
                }
            }
        }
    }
    UInt64 count{0U};
    UInt64 size{0U};
    UInt64 enabled{0U};
    UInt64 notifyOffset{0U};
    if (Read(m_windows[0U], 18U, MemoryMappedIoWidth::Word, count) != VirtioTransportError::None ||
        (queueId >= count) ||
        Write(m_windows[0U], 22U, MemoryMappedIoWidth::Word, queueId) != VirtioTransportError::None ||
        Read(m_windows[0U], 24U, MemoryMappedIoWidth::Word, size) != VirtioTransportError::None ||
        Read(m_windows[0U], 28U, MemoryMappedIoWidth::Word, enabled) != VirtioTransportError::None ||
        Read(m_windows[0U], 30U, MemoryMappedIoWidth::Word, notifyOffset) != VirtioTransportError::None)
    {
        return Fail(VirtioTransportError::MalformedDevice);
    }
    if ((size == 0U) || (size > 32768U) || ((size & (size - 1U)) != 0U) || (enabled != 0U))
    {
        return Fail(VirtioTransportError::MalformedDevice);
    }
    UInt16 selected{1U};
    while ((UInt32{selected} * 2U <= limit) && (UInt32{selected} * 2U <= size))
    {
        selected = static_cast<UInt16>(selected * 2U);
    }
    const UInt64 notifyBytes = notifyOffset * m_notifyMultiplier;
    if ((notifyBytes > m_windows[1U].Length) || (2U > m_windows[1U].Length - notifyBytes) || ((notifyBytes & 1U) != 0U))
    {
        return Fail(VirtioTransportError::MalformedDevice);
    }
    Queue& queue = m_queues[queueId];
    for (Size page{0U}; page < 3U; ++page)
    {
        queue.Pages[page] = pages[page];
        for (Size byte{0U}; byte < 4096U; ++byte)
        {
            pages[page].Bytes[byte] = 0U;
        }
    }
    Store(pages[1U].Bytes, 0U, 2U, 1U); // VRING_AVAIL_F_NO_INTERRUPT.
    queue.Size = selected;
    queue.NotifyAddress = m_windows[1U].Base + notifyBytes;
    Synchronization::DeviceMemoryBarrier();
    if (Write(m_windows[0U], 24U, MemoryMappedIoWidth::Word, selected) != VirtioTransportError::None ||
        Write(m_windows[0U], 26U, MemoryMappedIoWidth::Word, 0xFFFFU) != VirtioTransportError::None)
    {
        return Fail(VirtioTransportError::HardwareFailure);
    }
    for (UInt32 page{0U}; page < 3U; ++page)
    {
        if (Write(m_windows[0U], 32U + page * 8U, MemoryMappedIoWidth::DoubleWord,
                  pages[page].PhysicalAddress & 0xFFFF'FFFFU) != VirtioTransportError::None ||
            Write(m_windows[0U], 36U + page * 8U, MemoryMappedIoWidth::DoubleWord,
                  pages[page].PhysicalAddress >> 32U) != VirtioTransportError::None)
        {
            return Fail(VirtioTransportError::HardwareFailure);
        }
    }
    if (Write(m_windows[0U], 28U, MemoryMappedIoWidth::Word, 1U) != VirtioTransportError::None)
    {
        return Fail(VirtioTransportError::HardwareFailure);
    }
    return VirtioTransportError::None;
}

VirtioTransportError VirtioPciTransport::Start() noexcept
{
    if (!m_negotiated || m_started || m_failed || (m_queues[0U].Size == 0U))
    {
        return VirtioTransportError::NotReady;
    }
    if (Write(m_windows[0U], 16U, MemoryMappedIoWidth::Word, 0xFFFFU) != VirtioTransportError::None ||
        Write(m_windows[0U], 20U, MemoryMappedIoWidth::Byte, 15U) != VirtioTransportError::None)
    {
        return Fail(VirtioTransportError::HardwareFailure);
    }
    m_started = true;
    return CheckStatus();
}

VirtioTransportError VirtioPciTransport::CheckStatus() noexcept
{
    UInt64 status{0U};
    if (Read(m_windows[0U], 20U, MemoryMappedIoWidth::Byte, status) != VirtioTransportError::None ||
        ((status & 15U) != 15U) || ((status & 0xC0U) != 0U))
    {
        return Fail(VirtioTransportError::HardwareFailure);
    }
    return VirtioTransportError::None;
}

void VirtioPciTransport::Descriptor(Queue& queue, const UInt16 index, const UInt64 address, const UInt32 length,
                                    const UInt16 flags, const UInt16 next) noexcept
{
    volatile UInt8* const bytes = queue.Pages[0U].Bytes;
    const Size offset = Size{index} * 16U;
    Store(bytes, offset, 8U, address);
    Store(bytes, offset + 8U, 4U, length);
    Store(bytes, offset + 12U, 2U, flags);
    Store(bytes, offset + 14U, 2U, next);
}

VirtioTransportError VirtioPciTransport::Submit(const UInt16 queueId, const UInt16 head, const UInt64 readableAddress,
                                                const UInt32 readableBytes, const UInt64 writableAddress,
                                                const UInt32 writableBytes) noexcept
{
    if (!IsReady() || (queueId >= MaximumQueues) || (m_queues[queueId].Size == 0U))
    {
        return VirtioTransportError::NotReady;
    }
    Queue& queue = m_queues[queueId];
    const bool chained = readableBytes != 0U;
    if ((head >= queue.Size) || (writableBytes == 0U) || (writableAddress > MaximumAddress - (writableBytes - 1U)) ||
        (chained && ((head + 1U >= queue.Size) || (readableAddress > MaximumAddress - (readableBytes - 1U)))) ||
        (queue.Occupied[head] != 0U) || (chained && (queue.Occupied[head + 1U] != 0U)))
    {
        return VirtioTransportError::InvalidArguments;
    }
    if (CheckStatus() != VirtioTransportError::None)
    {
        return VirtioTransportError::HardwareFailure;
    }
    if (chained)
    {
        Descriptor(queue, head, readableAddress, readableBytes, 1U, static_cast<UInt16>(head + 1U));
        Descriptor(queue, static_cast<UInt16>(head + 1U), writableAddress, writableBytes, 2U, 0U);
        queue.Occupied[head + 1U] = 2U;
    }
    else
    {
        Descriptor(queue, head, writableAddress, writableBytes, 2U, 0U);
    }
    queue.Occupied[head] = chained ? 3U : 1U;
    queue.WritableBytes[head] = writableBytes;
    Store(queue.Pages[1U].Bytes, 4U + (static_cast<Size>(queue.Available & (queue.Size - 1U)) * 2U), 2U, head);
    Synchronization::DeviceMemoryBarrier();
    queue.Available = static_cast<UInt16>((UInt32{queue.Available} + 1U) & 0xFFFFU);
    RingIndex(queue.Pages[1U].Bytes + 2U, queue.Available);
    ++queue.Outstanding;
    return VirtioTransportError::None;
}

VirtioTransportError VirtioPciTransport::Notify(const UInt16 queueId) noexcept
{
    if (!IsReady() || (queueId >= MaximumQueues) || (m_queues[queueId].Size == 0U))
    {
        return VirtioTransportError::NotReady;
    }
    Synchronization::DeviceMemoryBarrier();
    if (m_writer.Write(m_queues[queueId].NotifyAddress, MemoryMappedIoWidth::Word, queueId) !=
        Hal::Device::MemoryMappedIoWriteError::None)
    {
        return Fail(VirtioTransportError::HardwareFailure);
    }
    return VirtioTransportError::None;
}

VirtioTransportError VirtioPciTransport::Poll(const UInt16 queueId, const UInt32 polls,
                                              VirtioUsedBuffer& output) noexcept
{
    if (!IsReady() || (queueId >= MaximumQueues) || (m_queues[queueId].Size == 0U))
    {
        return VirtioTransportError::NotReady;
    }
    if ((polls == 0U) || (polls > MaximumPolls))
    {
        return VirtioTransportError::InvalidArguments;
    }
    for (UInt32 poll{0U}; poll < polls; ++poll)
    {
        const VirtioTransportError error = TryPoll(queueId, output);
        if (error != VirtioTransportError::Empty)
        {
            return error;
        }
    }
    return Fail(VirtioTransportError::TimedOut);
}

VirtioTransportError VirtioPciTransport::TryPoll(const UInt16 queueId, VirtioUsedBuffer& output) noexcept
{
    if (!IsReady() || (queueId >= MaximumQueues) || (m_queues[queueId].Size == 0U))
    {
        return VirtioTransportError::NotReady;
    }
    Queue& queue = m_queues[queueId];
    if (CheckStatus() != VirtioTransportError::None)
    {
        return VirtioTransportError::HardwareFailure;
    }
    const UInt16 deviceIndex = RingIndex(queue.Pages[2U].Bytes + 2U);
    const UInt16 pending = static_cast<UInt16>((UInt32{deviceIndex} + 65536U - queue.Used) & 0xFFFFU);
    if (pending > queue.Outstanding)
    {
        return Fail(VirtioTransportError::MalformedDevice);
    }
    if (pending == 0U)
    {
        return VirtioTransportError::Empty;
    }
    Synchronization::DeviceMemoryBarrier();
    const Size offset = 4U + static_cast<Size>(queue.Used & (queue.Size - 1U)) * 8U;
    const UInt64 id = Load(queue.Pages[2U].Bytes, offset, 4U);
    const UInt64 length = Load(queue.Pages[2U].Bytes, offset + 4U, 4U);
    if ((id >= queue.Size) || ((queue.Occupied[id] != 1U) && (queue.Occupied[id] != 3U)) ||
        (length > queue.WritableBytes[id]))
    {
        return Fail(VirtioTransportError::MalformedDevice);
    }
    if (queue.Occupied[id] == 3U)
    {
        queue.Occupied[id + 1U] = 0U;
    }
    queue.Occupied[id] = 0U;
    --queue.Outstanding;
    queue.Used = static_cast<UInt16>((UInt32{queue.Used} + 1U) & 0xFFFFU);
    output = VirtioUsedBuffer{.Head = static_cast<UInt16>(id), .Length = static_cast<UInt32>(length)};
    return VirtioTransportError::None;
}

VirtioTransportError VirtioPciTransport::ReadDevice(const UInt32 offset, const MemoryMappedIoWidth width,
                                                    UInt64& output) noexcept
{
    if (!m_negotiated || m_failed)
    {
        return VirtioTransportError::NotReady;
    }
    if ((width != MemoryMappedIoWidth::Byte) && (width != MemoryMappedIoWidth::Word) &&
        (width != MemoryMappedIoWidth::DoubleWord))
    {
        return VirtioTransportError::InvalidArguments;
    }
    return Read(m_windows[3U], offset, width, output);
}

VirtioTransportError VirtioPciTransport::WriteDeviceByte(const UInt32 offset, const UInt8 value) noexcept
{
    return (m_negotiated && !m_failed) ? Write(m_windows[3U], offset, MemoryMappedIoWidth::Byte, value)
                                       : VirtioTransportError::NotReady;
}

VirtioTransportError VirtioPciTransport::Generation(UInt8& output) noexcept
{
    UInt64 value{0U};
    const VirtioTransportError error = Read(m_windows[0U], 21U, MemoryMappedIoWidth::Byte, value);
    if (error == VirtioTransportError::None)
    {
        output = static_cast<UInt8>(value);
    }
    return error;
}

UInt16 VirtioPciTransport::GetQueueSize(const UInt16 queue) const noexcept
{
    return (queue < MaximumQueues) ? m_queues[queue].Size : 0U;
}
} // namespace Asteria::Platform::Device
