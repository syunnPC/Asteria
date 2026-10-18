// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_DEVICE_VIRTIO_PCI_TRANSPORT_HPP_INCLUDED
#define ASTERIA_PLATFORM_DEVICE_VIRTIO_PCI_TRANSPORT_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Hal/Device/IPciConfigurationSpace.hpp"
#include "Hal/Device/IMemoryMappedIo.hpp"
#include "Hal/Device/IMemoryMappedIoWriter.hpp"

namespace Asteria::Platform::Device
{
enum class VirtioTransportError : Foundation::UInt8
{
    None,
    NotReady,
    InvalidArguments,
    MalformedDevice,
    UnsupportedFeatures,
    HardwareFailure,
    TimedOut,
    Empty,
};

struct VirtioPciBar
{
    Foundation::UInt64 Base;
    Foundation::UInt64 Length;
};

/// Borrowed coherent WB RAM, mapped at Bytes, with a device-visible physical
/// address. Both addresses are page aligned; the owner pins all 4096 bytes
/// until a successful device reset, including after errors/timeouts.
struct VirtioDmaPage
{
    volatile Foundation::UInt8* Bytes;
    Foundation::UInt64 PhysicalAddress;
};

struct VirtioUsedBuffer
{
    Foundation::UInt16 Head;
    Foundation::UInt32 Length;
};

/// Modern PCI transport, split rings, two queues, no interrupt handlers.
/// Caller serializes every operation across all users, prevents same-CPU
/// reentry, and never calls from an ISR. Initialization borrows BAR authority
/// already sized/mapped by the platform; it never probes a live BAR or enables
/// bus mastering. MMIO must support naturally aligned byte/word/DWORD accesses
/// (IA-64 uses its bounded BAR adapter and uncached physical leaves).
/// Queue RAM is coherent and untranslated by this IOMMU. No allocation.
/// Only VERSION_1 and device-specific bits 0..23 are supported; indirect,
/// packed, event-index, notification-data and ACCESS_PLATFORM are not accepted.
/// DMA publication/consumption uses full ISA fences. UInt16 ring counters wrap
/// intentionally modulo 65536. A timeout poisons the transport: exposed buffers
/// cannot be reused, even if a late completion arrives, until Reset succeeds.
class VirtioPciTransport final
{
public:
    static constexpr Foundation::UInt16 MaximumQueues{2U};
    static constexpr Foundation::UInt16 MaximumQueueSize{128U};
    static constexpr Foundation::UInt32 MaximumPolls{100000U};
    static constexpr Foundation::UInt64 Version1{Foundation::UInt64{1U} << 32U};

    VirtioPciTransport(Hal::Device::IPciConfigurationSpace& pci, Hal::Device::IMemoryMappedIo& reader,
                       Hal::Device::IMemoryMappedIoWriter& writer) noexcept;
    VirtioPciTransport(const VirtioPciTransport&) = delete;
    VirtioPciTransport& operator=(const VirtioPciTransport&) = delete;

    [[nodiscard]] VirtioTransportError Discover(Kernel::Device::PciFunctionAddress function,
                                                const VirtioPciBar (&bars)[6U]) noexcept;
    [[nodiscard]] VirtioTransportError Reset(Foundation::UInt32 polls) noexcept;
    [[nodiscard]] VirtioTransportError Negotiate(Foundation::UInt64 required, Foundation::UInt64 optional) noexcept;
    [[nodiscard]] VirtioTransportError SetupQueue(Foundation::UInt16 queue, const VirtioDmaPage (&pages)[3U],
                                                  Foundation::UInt16 limit) noexcept;
    [[nodiscard]] VirtioTransportError Start() noexcept;
    [[nodiscard]] VirtioTransportError Submit(Foundation::UInt16 queue, Foundation::UInt16 head,
                                              Foundation::UInt64 readableAddress, Foundation::UInt32 readableBytes,
                                              Foundation::UInt64 writableAddress,
                                              Foundation::UInt32 writableBytes) noexcept;
    [[nodiscard]] VirtioTransportError Notify(Foundation::UInt16 queue) noexcept;
    [[nodiscard]] VirtioTransportError Poll(Foundation::UInt16 queue, Foundation::UInt32 polls,
                                            VirtioUsedBuffer& output) noexcept;
    /// One nonblocking completion check; Empty leaves the queue live.
    [[nodiscard]] VirtioTransportError TryPoll(Foundation::UInt16 queue, VirtioUsedBuffer& output) noexcept;
    [[nodiscard]] bool IsOwnRequester(Foundation::UInt32 requester) const noexcept
    {
        return requester == ((Foundation::UInt32{m_function.GetSegment()} << 16U) | m_function.GetRequesterId());
    }
    [[nodiscard]] VirtioTransportError ReadDevice(Foundation::UInt32 offset, Hal::Device::MemoryMappedIoWidth width,
                                                  Foundation::UInt64& output) noexcept;
    [[nodiscard]] VirtioTransportError WriteDeviceByte(Foundation::UInt32 offset, Foundation::UInt8 value) noexcept;
    [[nodiscard]] VirtioTransportError Generation(Foundation::UInt8& output) noexcept;
    [[nodiscard]] Foundation::UInt64 GetFeatures() const noexcept
    {
        return m_features;
    }
    [[nodiscard]] Foundation::UInt16 GetQueueSize(Foundation::UInt16 queue) const noexcept;
    [[nodiscard]] bool IsReady() const noexcept
    {
        return m_started && !m_failed;
    }
    [[nodiscard]] static bool PageIsValid(const VirtioDmaPage& page) noexcept;
    static void Store(volatile Foundation::UInt8* bytes, Foundation::Size offset, Foundation::Size width,
                      Foundation::UInt64 value) noexcept;
    [[nodiscard]] static Foundation::UInt64 Load(const volatile Foundation::UInt8* bytes, Foundation::Size offset,
                                                 Foundation::Size width) noexcept;

private:
    struct Window
    {
        Foundation::UInt64 Base{0U};
        Foundation::UInt64 Length{0U};
    };
    struct Queue
    {
        VirtioDmaPage Pages[3U]{};
        Foundation::UInt32 WritableBytes[MaximumQueueSize]{};
        Foundation::UInt8 Occupied[MaximumQueueSize]{};
        Foundation::UInt16 Size{0U};
        Foundation::UInt16 Available{0U};
        Foundation::UInt16 Used{0U};
        Foundation::UInt16 Outstanding{0U};
        Foundation::UInt64 NotifyAddress{0U};
    };
    [[nodiscard]] VirtioTransportError Capability(Foundation::UInt16 offset, Foundation::UInt32 header,
                                                  const VirtioPciBar (&bars)[6U]) noexcept;
    [[nodiscard]] VirtioTransportError Read(const Window& window, Foundation::UInt32 offset,
                                            Hal::Device::MemoryMappedIoWidth width,
                                            Foundation::UInt64& output) noexcept;
    [[nodiscard]] VirtioTransportError Write(const Window& window, Foundation::UInt32 offset,
                                             Hal::Device::MemoryMappedIoWidth width, Foundation::UInt64 value) noexcept;
    [[nodiscard]] VirtioTransportError Fail(VirtioTransportError error) noexcept;
    [[nodiscard]] VirtioTransportError CheckStatus() noexcept;
    void Descriptor(Queue& queue, Foundation::UInt16 index, Foundation::UInt64 address, Foundation::UInt32 length,
                    Foundation::UInt16 flags, Foundation::UInt16 next) noexcept;

    Hal::Device::IPciConfigurationSpace& m_pci;
    Hal::Device::IMemoryMappedIo& m_reader;
    Hal::Device::IMemoryMappedIoWriter& m_writer;
    Kernel::Device::PciFunctionAddress m_function{};
    Window m_windows[4U]{};
    Queue m_queues[MaximumQueues]{};
    Foundation::UInt32 m_notifyMultiplier{0U};
    Foundation::UInt64 m_features{0U};
    bool m_discovered{false};
    bool m_negotiated{false};
    bool m_reset{false};
    bool m_started{false};
    bool m_failed{false};
};
} // namespace Asteria::Platform::Device
#endif // ASTERIA_PLATFORM_DEVICE_VIRTIO_PCI_TRANSPORT_HPP_INCLUDED
