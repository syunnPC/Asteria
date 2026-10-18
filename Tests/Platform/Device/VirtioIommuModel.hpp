// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_PLATFORM_DEVICE_VIRTIO_IOMMU_MODEL_HPP_INCLUDED
#define ASTERIA_TESTS_PLATFORM_DEVICE_VIRTIO_IOMMU_MODEL_HPP_INCLUDED

#include "Platform/Device/VirtioIommuDomainBackend.hpp"
#include "Kernel/Device/IommuDomain.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Support/ObjectRegistryFixture.hpp"

namespace Asteria::TestSupport
{
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;

// Independent field codec: intentionally does not use transport helpers.
[[nodiscard]] inline UInt64 Get(const volatile UInt8* bytes, Size offset, Size width) noexcept
{
    UInt64 value{0U};
    for (Size index{0U}; index < width; ++index)
    {
        value += UInt64{bytes[offset + index]} << (8U * index);
    }
    return value;
}
inline void Put(volatile UInt8* bytes, Size offset, Size width, UInt64 value) noexcept
{
    for (Size index{0U}; index < width; ++index)
    {
        bytes[offset + index] = static_cast<UInt8>(value & 255U);
        value >>= 8U;
    }
}

// Modern PCI + split-queue device contract from VIRTIO 1.3 sections 4.1 and
// 5.13. Re-ATTACH also follows QEMU's detach-first domain destruction.
// DMA addresses name a checked fake RAM aperture.
class VirtioIommuModel final : public Hal::Device::IPciConfigurationSpace,
                               public Hal::Device::IMemoryMappedIo,
                               public Hal::Device::IMemoryMappedIoWriter
{
public:
    static constexpr UInt64 BarBase{0x800000U};
    static constexpr UInt64 RamBase{0x100000U};
    struct Mapping
    {
        UInt64 Start{0U};
        UInt64 End{0U};
        UInt64 Physical{0U};
        UInt32 Flags{0U};
        bool Present{false};
    };
    struct Domain
    {
        bool Exists{false};
        UInt32 EndpointCount{0U};
        Mapping Mappings[64U]{};
    };
    struct Queue
    {
        UInt16 Size{128U};
        UInt16 Enabled{0U};
        UInt16 NotifyOffset{0U};
        UInt16 Consumed{0U};
        UInt16 Used{0U};
        UInt64 Addresses[3U]{};
    };

    VirtioIommuModel() noexcept
    {
        Put(Pci, 0U, 4U, 0x10571AF4U);
        Put(Pci, 4U, 4U, 0x00100000U);
        Put(Pci, 0x10U, 4U, BarBase);
        Put(Pci, 0x34U, 4U, 0x40U);
        Cap(0x40U, 0x50U, 16U, 1U, 0U, 56U);
        Cap(0x50U, 0x64U, 20U, 2U, 0x100U, 16U);
        Put(Pci, 0x60U, 4U, 2U);
        Cap(0x64U, 0x74U, 16U, 3U, 0x200U, 1U);
        Cap(0x74U, 0U, 16U, 4U, 0x300U, 40U);
        Put(Config, 0U, 8U, 4096U);
        Put(Config, 8U, 8U, 0U);
        Put(Config, 16U, 8U, 0xFFFF'FFFFU);
        Put(Config, 24U, 4U, 7U);
        Put(Config, 28U, 4U, 22U);
        Put(Config, 32U, 4U, 128U);
        Config[36U] = 1U;
        Queues[1U].NotifyOffset = 1U;
        for (UInt32& endpoint : Endpoints)
        {
            endpoint = 0xFFFF'FFFFU;
        }
    }

    void Cap(UInt16 at, UInt8 next, UInt8 length, UInt8 type, UInt32 start, UInt32 bytes) noexcept
    {
        Put(Pci, at, 4U, 9U | UInt32{next} << 8U | UInt32{length} << 16U | UInt32{type} << 24U);
        Put(Pci, at + 8U, 4U, start);
        Put(Pci, at + 12U, 4U, bytes);
    }
    [[nodiscard]] Hal::Device::PciConfigurationReadError Read32(Kernel::Device::PciFunctionAddress, UInt16 offset,
                                                                UInt32& output) noexcept override
    {
        if ((offset > 252U) || ((offset & 3U) != 0U) || PciFailure)
        {
            return Hal::Device::PciConfigurationReadError::HardwareFailure;
        }
        output = static_cast<UInt32>(Get(Pci, offset, 4U));
        return Hal::Device::PciConfigurationReadError::None;
    }
    [[nodiscard]] Hal::Device::MemoryMappedIoReadError Read(UInt64 address, Hal::Device::MemoryMappedIoWidth width,
                                                            UInt64& output) noexcept override
    {
        if (MmioFailure || (address < BarBase) || (address >= BarBase + 4096U))
        {
            return Hal::Device::MemoryMappedIoReadError::HardwareFailure;
        }
        const UInt64 offset = address - BarBase;
        const Size bytes = static_cast<Size>(width);
        output = 0U;
        if (offset < 56U)
        {
            Queue& queue = Queues[QueueSelect];
            switch (offset)
            {
            case 4U:
                output = (Features >> (FeatureSelect * 32U)) & 0xFFFF'FFFFU;
                break;
            case 18U:
                output = 2U;
                break;
            case 20U:
                output = Status;
                ++StatusReads;
                break;
            case 21U:
                output = UnstableConfig ? ++Generation : Generation;
                break;
            case 24U:
                output = queue.Size;
                break;
            case 28U:
                output = queue.Enabled;
                break;
            case 30U:
                output = queue.NotifyOffset;
                break;
            default:
                ++Violations;
                break;
            }
        }
        else if ((offset >= 0x300U) && (offset + bytes <= 0x328U))
        {
            output = Get(Config, static_cast<Size>(offset - 0x300U), bytes);
        }
        else
        {
            ++Violations;
        }
        if (OversizedRead)
        {
            output = 0x1'0000U;
        }
        return Hal::Device::MemoryMappedIoReadError::None;
    }
    [[nodiscard]] Hal::Device::MemoryMappedIoWriteError Write(UInt64 address, Hal::Device::MemoryMappedIoWidth width,
                                                              UInt64 value) noexcept override
    {
        if (MmioFailure || (address < BarBase) || (address >= BarBase + 4096U))
        {
            return Hal::Device::MemoryMappedIoWriteError::HardwareFailure;
        }
        const UInt64 offset = address - BarBase;
        if (offset < 56U)
        {
            switch (offset)
            {
            case 0U:
                FeatureSelect = static_cast<UInt32>(value);
                break;
            case 8U:
                DriverSelect = static_cast<UInt32>(value);
                break;
            case 12U:
                Accepted &= ~(UInt64{0xFFFF'FFFFU} << (DriverSelect * 32U));
                Accepted |= value << (DriverSelect * 32U);
                break;
            case 16U:
                break;
            case 20U:
                if ((width != Hal::Device::MemoryMappedIoWidth::Byte) || ((value & Status) != Status && value != 0U))
                {
                    ++Violations;
                }
                if (value == 0U)
                {
                    ++ResetWrites;
                    if (StuckReset)
                    {
                        Status = 1U;
                        break;
                    }
                    Status = 0U;
                    Accepted = 0U;
                    for (Domain& domain : Domains)
                    {
                        domain.Exists = false;
                        domain.EndpointCount = 0U;
                        for (Mapping& mapping : domain.Mappings)
                        {
                            mapping = Mapping{};
                        }
                    }
                    for (UInt32& endpoint : Endpoints)
                    {
                        endpoint = 0xFFFF'FFFFU;
                    }
                    for (Queue& queue : Queues)
                    {
                        queue.Enabled = 0U;
                        queue.Consumed = 0U;
                        queue.Used = 0U;
                    }
                }
                else
                {
                    Status = static_cast<UInt8>(value);
                    if (RejectFeatures && ((value & 8U) != 0U))
                    {
                        Status = static_cast<UInt8>(Status & ~8U);
                    }
                }
                break;
            case 22U:
                if (value >= 2U)
                {
                    ++Violations;
                    return Hal::Device::MemoryMappedIoWriteError::HardwareFailure;
                }
                QueueSelect = static_cast<UInt16>(value);
                break;
            case 24U:
                Queues[QueueSelect].Size = static_cast<UInt16>(value);
                break;
            case 26U:
                break;
            case 28U:
                Queues[QueueSelect].Enabled = static_cast<UInt16>(value);
                break;
            default:
                if ((offset >= 32U) && ((offset & 3U) == 0U))
                {
                    UInt64& target = Queues[QueueSelect].Addresses[(offset - 32U) / 8U];
                    const UInt64 shift = (offset & 4U) * 8U;
                    target = (target & ~(UInt64{0xFFFF'FFFFU} << shift)) | value << shift;
                }
                else
                {
                    ++Violations;
                }
                break;
            }
        }
        else if ((offset == 0x100U) || (offset == 0x102U))
        {
            ++Notifications;
            if ((value == 1U) && FailEventNotify)
            {
                PostedEventsAtFailure = Available(1U);
                StuckReset = FailCleanupReset;
                return Hal::Device::MemoryMappedIoWriteError::HardwareFailure;
            }
            if ((width != Hal::Device::MemoryMappedIoWidth::Word) || (value >= 2U) ||
                (offset != 0x100U + Queues[value].NotifyOffset * 2U) || Status != 15U)
            {
                ++Violations;
            }
            else if ((value == 0U) && !NeverComplete)
            {
                ProcessRequests();
            }
        }
        else if (offset == 0x324U && width == Hal::Device::MemoryMappedIoWidth::Byte && value <= 1U)
        {
            if (!IgnoreBypassWrite)
            {
                Config[36U] = static_cast<UInt8>(value);
            }
        }
        else
        {
            ++Violations;
        }
        return Hal::Device::MemoryMappedIoWriteError::None;
    }

    [[nodiscard]] volatile UInt8* Dma(UInt64 address, UInt64 length) noexcept
    {
        if ((address < RamBase) || (address - RamBase > sizeof(Ram)) || (length > sizeof(Ram) - (address - RamBase)))
        {
            ++Violations;
            return nullptr;
        }
        return Ram + (address - RamBase);
    }
    [[nodiscard]] UInt16 Available(UInt16 queue) noexcept
    {
        return static_cast<UInt16>(Get(Dma(Queues[queue].Addresses[1U], 4096U), 2U, 2U));
    }
    void Complete(UInt16 queueId, UInt16 head, UInt32 length) noexcept
    {
        Queue& queue = Queues[queueId];
        volatile UInt8* used = Dma(queue.Addresses[2U], 4096U);
        Put(used, 4U + (queue.Used % queue.Size) * 8U, 4U, BadUsedId ? queue.Size : head);
        Put(used, 8U + (queue.Used % queue.Size) * 8U, 4U, BadUsedLength ? 4096U : length);
        queue.Used = static_cast<UInt16>((UInt32{queue.Used} + 1U) & 0xFFFFU);
        Put(used, 2U, 2U, BadUsedAdvance ? UInt32{queue.Used} + 3U : queue.Used);
    }
    [[nodiscard]] UInt8 Execute(const volatile UInt8* request, UInt32 length) noexcept
    {
        const UInt8 type = request[0U];
        const UInt64 id = Get(request, 4U, 4U);
        if (Get(request, 1U, 3U) != 0U)
        {
            return 4U;
        }
        if ((id < 7U) || (id > 22U))
        {
            return 5U;
        }
        Domain& domain = Domains[id - 7U];
        if ((type == 1U) || (type == 2U))
        {
            if (length != 20U)
            {
                return 4U;
            }
            const UInt64 endpoint = Get(request, 8U, 4U);
            if (endpoint < 16U || endpoint > 255U)
            {
                return 6U;
            }
            if (Get(request, 12U, 8U) != 0U)
            {
                return 4U;
            }
            if (type == 1U)
            {
                if (Endpoints[endpoint] != 0xFFFF'FFFFU)
                {
                    ++Reattachments;
                    RemoveEndpoint(static_cast<UInt32>(endpoint));
                }
                if (AttachStatus != 0xFFU)
                {
                    return AttachStatus;
                }
                ++domain.EndpointCount;
                Endpoints[endpoint] = static_cast<UInt32>(id);
                domain.Exists = true;
            }
            else
            {
                if (!domain.Exists || Endpoints[endpoint] != id)
                {
                    return 4U;
                }
                RemoveEndpoint(static_cast<UInt32>(endpoint));
            }
            return 0U;
        }
        if (!domain.Exists)
        {
            return 6U;
        }
        if (type != 3U && type != 4U)
        {
            return 2U;
        }
        if (length != (type == 3U ? 36U : 28U))
        {
            return 4U;
        }
        const UInt64 start = Get(request, 8U, 8U);
        const UInt64 end = Get(request, 16U, 8U);
        if (end <= start || end > Get(Config, 16U, 8U))
        {
            return 5U;
        }
        if ((start & 4095U) != 0U || (end & 4095U) != 4095U)
        {
            return 4U;
        }
        if (type == 3U)
        {
            const UInt64 physical = Get(request, 24U, 8U);
            const UInt64 flags = Get(request, 32U, 4U);
            if ((physical & 4095U) != 0U || flags == 0U || flags > 3U)
            {
                return 4U;
            }
            Mapping* free{nullptr};
            for (Mapping& mapping : domain.Mappings)
            {
                if (mapping.Present && start <= mapping.End && mapping.Start <= end)
                {
                    return 4U;
                }
                if (!mapping.Present)
                {
                    free = &mapping;
                }
            }
            if (free == nullptr)
            {
                return 8U;
            }
            *free = Mapping{
                .Start = start, .End = end, .Physical = physical, .Flags = static_cast<UInt32>(flags), .Present = true};
        }
        else
        {
            if (Get(request, 24U, 4U) != 0U)
            {
                return 4U;
            }
            for (Mapping& mapping : domain.Mappings)
            {
                if (mapping.Present && start <= mapping.End && mapping.Start <= end &&
                    (start > mapping.Start || end < mapping.End))
                {
                    return 5U;
                }
            }
            for (Mapping& mapping : domain.Mappings)
            {
                if (mapping.Present && start <= mapping.Start && mapping.End <= end)
                {
                    mapping.Present = false;
                }
            }
        }
        return 0U;
    }
    void RemoveEndpoint(UInt32 endpoint) noexcept
    {
        Domain& previous = Domains[Endpoints[endpoint] - 7U];
        Endpoints[endpoint] = 0xFFFF'FFFFU;
        --previous.EndpointCount;
        if (previous.EndpointCount == 0U)
        {
            ++DestroyedDomains;
            previous.Exists = false;
            for (Mapping& mapping : previous.Mappings)
            {
                mapping = Mapping{};
            }
        }
    }
    void ProcessRequests() noexcept
    {
        Queue& queue = Queues[0U];
        for (UInt16 count{0U}; count < queue.Size && queue.Consumed != Available(0U); ++count)
        {
            volatile UInt8* available = Dma(queue.Addresses[1U], 4096U);
            const UInt16 head = static_cast<UInt16>(Get(available, 4U + (queue.Consumed % queue.Size) * 2U, 2U));
            volatile UInt8* descriptors = Dma(queue.Addresses[0U], 4096U);
            const Size at = Size{head} * 16U;
            const UInt64 readBytes = Get(descriptors, at + 8U, 4U);
            if (Get(descriptors, at + 12U, 2U) != 1U || head >= queue.Size)
            {
                ++Violations;
                return;
            }
            const UInt64 next = Get(descriptors, at + 14U, 2U);
            if (next >= queue.Size)
            {
                ++Violations;
                return;
            }
            const Size tailAt = static_cast<Size>(next) * 16U;
            if (Get(descriptors, tailAt + 12U, 2U) != 2U || Get(descriptors, tailAt + 8U, 4U) != 4U)
            {
                ++Violations;
                return;
            }
            volatile UInt8* request = Dma(Get(descriptors, at, 8U), readBytes);
            volatile UInt8* tail = Dma(Get(descriptors, tailAt, 8U), 4U);
            if (request == nullptr || tail == nullptr)
            {
                return;
            }
            const UInt8 status =
                (ForcedStatus != 0xFFU) ? ForcedStatus : Execute(request, static_cast<UInt32>(readBytes));
            if (!UnwrittenStatus)
            {
                Put(tail, 0U, 4U, status);
            }
            ++Requests;
            queue.Consumed = static_cast<UInt16>((UInt32{queue.Consumed} + 1U) & 0xFFFFU);
            Complete(0U, head, ZeroUsedLength ? 0U : 4U);
        }
    }
    [[nodiscard]] bool Emit(UInt32 endpoint, UInt8 reason, UInt8 reserved = 0U) noexcept
    {
        Queue& queue = Queues[1U];
        if (queue.Consumed == Available(1U))
        {
            return false;
        }
        volatile UInt8* available = Dma(queue.Addresses[1U], 4096U);
        const UInt16 head = static_cast<UInt16>(Get(available, 4U + (queue.Consumed % queue.Size) * 2U, 2U));
        volatile UInt8* descriptors = Dma(queue.Addresses[0U], 4096U);
        const Size at = Size{head} * 16U;
        if (Get(descriptors, at + 12U, 2U) != 2U || Get(descriptors, at + 8U, 4U) != 24U)
        {
            ++Violations;
            return false;
        }
        volatile UInt8* fault = Dma(Get(descriptors, at, 8U), 24U);
        Put(fault, 0U, 24U, 0U);
        fault[0U] = reason;
        fault[1U] = reserved;
        Put(fault, 4U, 4U, 0x101U);
        Put(fault, 8U, 4U, endpoint);
        Put(fault, 16U, 8U, 0x4000U);
        queue.Consumed = static_cast<UInt16>((UInt32{queue.Consumed} + 1U) & 0xFFFFU);
        Complete(1U, head, 24U);
        return true;
    }
    [[nodiscard]] Size MappingCount(UInt32 id) const noexcept
    {
        Size count{0U};
        for (const Mapping& mapping : Domains[id - 7U].Mappings)
        {
            count += mapping.Present ? 1U : 0U;
        }
        return count;
    }

    UInt8 Pci[256U]{};
    UInt8 Config[40U]{};
    alignas(4096U) UInt8 Ram[8U * 4096U]{};
    Queue Queues[2U]{};
    Domain Domains[16U]{};
    UInt32 Endpoints[256U]{};
    UInt64 Features{Platform::Device::VirtioPciTransport::Version1 | 0x57U};
    UInt64 Accepted{0U};
    UInt32 FeatureSelect{0U};
    UInt32 DriverSelect{0U};
    UInt16 QueueSelect{0U};
    UInt8 Status{0U};
    UInt8 Generation{0U};
    UInt8 ForcedStatus{0xFFU};
    UInt8 AttachStatus{0xFFU};
    UInt32 Requests{0U};
    UInt32 Reattachments{0U};
    UInt32 DestroyedDomains{0U};
    UInt32 ResetWrites{0U};
    UInt16 PostedEventsAtFailure{0U};
    UInt32 Notifications{0U};
    UInt32 StatusReads{0U};
    UInt32 Violations{0U};
    bool NeverComplete{false};
    bool FailEventNotify{false};
    bool FailCleanupReset{false};
    bool RejectFeatures{false};
    bool StuckReset{false};
    bool IgnoreBypassWrite{false};
    bool UnstableConfig{false};
    bool BadUsedAdvance{false};
    bool BadUsedId{false};
    bool BadUsedLength{false};
    bool UnwrittenStatus{false};
    bool ZeroUsedLength{false};
    bool OversizedRead{false};
    bool MmioFailure{false};
    bool PciFailure{false};
};

class VirtioEndpoint final : public Hal::Device::IIommuDevice
{
public:
    explicit VirtioEndpoint(UInt32 requester) noexcept : Requester{requester}
    {
    }
    [[nodiscard]] bool HasTrustedDma() const noexcept override
    {
        return false;
    }
    [[nodiscard]] UInt32 GetRequesterId() const noexcept override
    {
        return Requester;
    }
    [[nodiscard]] Hal::Device::IommuDeviceError Claim(const Kernel::Device::IommuDomain&) noexcept override
    {
        return Hal::Device::IommuDeviceError::None;
    }
    [[nodiscard]] Hal::Device::IommuDeviceError Release(const Kernel::Device::IommuDomain&) noexcept override
    {
        return Hal::Device::IommuDeviceError::None;
    }
    UInt32 Requester;
};

class VirtioFixture final
{
public:
    VirtioIommuModel Model;
    Platform::Device::VirtioPciTransport Transport{Model, Model, Model};
    Platform::Device::VirtioIommuReservedRange ReservedRanges[2U]{
        {.Start = 0xFEE00000U, .End = 0xFEEFFFFFU},
        {.Start = 0x900123U, .End = 0x900456U},
    };
    Platform::Device::VirtioIommuDevice Device{Transport, {.Data = ReservedRanges, .Count = 2U}};
    Platform::Device::VirtioIommuDomainBackend Backend{Device};
    Platform::Device::VirtioIommuDomainBackend OtherBackend{Device};
    Platform::Synchronization::SpinLock RegistryLock;
    Asteria::TestSupport::TestObjectRegistry Registry{RegistryLock};
    Kernel::Device::IommuDomain Owner{Registry, Backend};
    Kernel::Device::IommuDomain OtherOwner{Registry, OtherBackend};
    VirtioEndpoint Endpoint{0x10U};
    VirtioEndpoint OtherEndpoint{0x18U};
    Platform::Device::VirtioPciBar Bars[6U]{{.Base = VirtioIommuModel::BarBase, .Length = 4096U}};

    [[nodiscard]] Platform::Device::VirtioDmaPage Page(Size index) noexcept
    {
        return Platform::Device::VirtioDmaPage{.Bytes = Model.Ram + index * 4096U,
                                               .PhysicalAddress = VirtioIommuModel::RamBase + index * 4096U};
    }
    [[nodiscard]] Platform::Device::VirtioTransportError Discover() noexcept
    {
        return Transport.Discover(Kernel::Device::PciFunctionAddress{0U, 0U, 1U, 0U}, Bars);
    }
    [[nodiscard]] Platform::Device::VirtioTransportError
    Initialize(const Platform::Device::VirtioIommuCompletionPolicy completion =
                   Platform::Device::VirtioIommuCompletionPolicy::DrainsDma) noexcept
    {
        const Platform::Device::VirtioDmaPage requestQueue[3U]{Page(0U), Page(1U), Page(2U)};
        const Platform::Device::VirtioDmaPage eventQueue[3U]{Page(3U), Page(4U), Page(5U)};
        return Device.Initialize(requestQueue, eventQueue, Page(6U), Page(7U), 8U, completion);
    }
    [[nodiscard]] static Hal::Device::IommuBackendMapRequest Mapping(UInt64 start = 0x4000U, UInt64 pages = 1U,
                                                                     UInt8 flags = 3U) noexcept
    {
        Kernel::Device::DmaPermissions permissions{};
        if ((flags & 1U) != 0U)
        {
            permissions = permissions.With(Kernel::Device::DmaPermission::Read);
        }
        if ((flags & 2U) != 0U)
        {
            permissions = permissions.With(Kernel::Device::DmaPermission::Write);
        }
        return Hal::Device::IommuBackendMapRequest{.DmaStart = Kernel::Device::DmaAddress{start},
                                                   .PhysicalStart = Kernel::Memory::PhysicalAddress{0x200000U + start},
                                                   .Pages = Kernel::Memory::PageCount{pages},
                                                   .Permissions = permissions,
                                                   .Reserved = {}};
    }
};
} // namespace Asteria::TestSupport
#endif // ASTERIA_TESTS_PLATFORM_DEVICE_VIRTIO_IOMMU_MODEL_HPP_INCLUDED
