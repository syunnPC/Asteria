// SPDX-License-Identifier: Apache-2.0

#include "Platform/Device/VirtioIommuDomainBackend.hpp"

namespace Asteria::Platform::Device
{
namespace
{
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;
using Hal::Device::IommuBackendError;
using Hal::Device::IommuBackendOwnershipError;
using Hal::Device::MemoryMappedIoWidth;
using Synchronization::SpinLockGuard;
constexpr UInt64 MaximumAddress{~UInt64{0U}};
constexpr UInt64 MapUnmap{4U};
constexpr UInt64 BypassConfig{64U};

[[nodiscard]] IommuBackendError TransportError(const VirtioTransportError error) noexcept
{
    switch (error)
    {
    case VirtioTransportError::None:
        return IommuBackendError::None;
    case VirtioTransportError::TimedOut:
    case VirtioTransportError::Empty:
        return IommuBackendError::CommittedStateUnknown;
    case VirtioTransportError::MalformedDevice:
        return IommuBackendError::CorruptState;
    case VirtioTransportError::NotReady:
    case VirtioTransportError::InvalidArguments:
    case VirtioTransportError::UnsupportedFeatures:
    case VirtioTransportError::HardwareFailure:
        return IommuBackendError::HardwareFailure;
    }
    return IommuBackendError::HardwareFailure;
}
} // Namespace

VirtioTransportError VirtioIommuDevice::Initialize(const VirtioDmaPage (&requestQueue)[3U],
                                                   const VirtioDmaPage (&eventQueue)[3U], const VirtioDmaPage& request,
                                                   const VirtioDmaPage& events, const UInt32 polls,
                                                   const VirtioIommuCompletionPolicy completion) noexcept
{
    SpinLockGuard guard{m_lock};
    for (const Domain& domain : m_domains)
    {
        if (domain.Owner != nullptr)
        {
            return VirtioTransportError::NotReady;
        }
    }
    if ((m_initialized && !m_quiesced) ||
        ((completion != VirtioIommuCompletionPolicy::DrainsDma) &&
         (completion != VirtioIommuCompletionPolicy::TranslationOnly)) ||
        (polls < 2U) || (polls > VirtioPciTransport::MaximumPolls) || !VirtioPciTransport::PageIsValid(request) ||
        !VirtioPciTransport::PageIsValid(events) || (request.PhysicalAddress == events.PhysicalAddress) ||
        (request.Bytes == events.Bytes))
    {
        return VirtioTransportError::InvalidArguments;
    }
    for (Size index{0U}; index < 3U; ++index)
    {
        if ((request.PhysicalAddress == requestQueue[index].PhysicalAddress) ||
            (request.PhysicalAddress == eventQueue[index].PhysicalAddress) ||
            (events.PhysicalAddress == requestQueue[index].PhysicalAddress) ||
            (events.PhysicalAddress == eventQueue[index].PhysicalAddress) ||
            (request.Bytes == requestQueue[index].Bytes) || (request.Bytes == eventQueue[index].Bytes) ||
            (events.Bytes == requestQueue[index].Bytes) || (events.Bytes == eventQueue[index].Bytes))
        {
            return VirtioTransportError::InvalidArguments;
        }
    }
    if ((m_reservedRanges.Count > MaximumReservedRanges) ||
        ((m_reservedRanges.Count != 0U) && (m_reservedRanges.Data == nullptr)))
    {
        return VirtioTransportError::InvalidArguments;
    }
    for (Size index{0U}; index < m_reservedRanges.Count; ++index)
    {
        if (m_reservedRanges.Data[index].Start > m_reservedRanges.Data[index].End)
        {
            return VirtioTransportError::InvalidArguments;
        }
    }
    m_polls = polls;
    m_initialized = false;
    m_quiesced = false;
    m_bypassCleared = false;
    VirtioTransportError error = m_transport.Reset(polls);
    if (error != VirtioTransportError::None)
    {
        return FailInitialize(error);
    }
    error = m_transport.Negotiate(MapUnmap | BypassConfig, 3U);
    if (error != VirtioTransportError::None)
    {
        return FailInitialize(error);
    }
    error = ReadConfiguration();
    if (error != VirtioTransportError::None)
    {
        return FailInitialize(error);
    }
    error = m_transport.SetupQueue(0U, requestQueue, 2U);
    if (error != VirtioTransportError::None)
    {
        return FailInitialize(error);
    }
    error = m_transport.SetupQueue(1U, eventQueue, EventBuffers);
    if (error != VirtioTransportError::None)
    {
        return FailInitialize(error);
    }
    if (m_transport.GetQueueSize(0U) < 2U)
    {
        return FailInitialize(VirtioTransportError::MalformedDevice);
    }
    m_request = request;
    m_events = events;
    m_polls = polls;
    error = m_transport.Start();
    if (error != VirtioTransportError::None)
    {
        return FailInitialize(error);
    }
    for (UInt16 index{0U}; index < m_transport.GetQueueSize(1U); ++index)
    {
        error = m_transport.Submit(1U, index, 0U, 0U, events.PhysicalAddress + UInt64{index} * 24U, 24U);
        if (error != VirtioTransportError::None)
        {
            return FailInitialize(error);
        }
    }
    error = m_transport.Notify(1U);
    if (error != VirtioTransportError::None)
    {
        return FailInitialize(error);
    }
    m_completion = completion;
    m_initialized = true;
    return VirtioTransportError::None;
}

VirtioTransportError VirtioIommuDevice::FailInitialize(const VirtioTransportError error) noexcept
{
    // Reset may fail too: all exposed pages stay pinned until recovery succeeds.
    static_cast<void>(m_transport.Reset(m_polls));
    return error;
}

VirtioTransportError VirtioIommuDevice::ReadConfiguration() noexcept
{
    // Generation brackets every field, including both halves of each le64.
    for (UInt32 attempt{0U}; attempt < 8U; ++attempt)
    {
        UInt8 before{0U};
        UInt8 after{0U};
        if (m_transport.Generation(before) != VirtioTransportError::None)
        {
            return VirtioTransportError::HardwareFailure;
        }
        UInt64 values[10U]{};
        for (UInt32 index{0U}; index < 10U; ++index)
        {
            const bool bypass = index == 9U;
            const UInt32 offset = bypass ? 36U : index * 4U;
            const VirtioTransportError error = m_transport.ReadDevice(
                offset, bypass ? MemoryMappedIoWidth::Byte : MemoryMappedIoWidth::DoubleWord, values[index]);
            if (error != VirtioTransportError::None)
            {
                return error;
            }
        }
        if (m_transport.Generation(after) != VirtioTransportError::None)
        {
            return VirtioTransportError::HardwareFailure;
        }
        if (before != after)
        {
            continue;
        }
        const UInt64 features = m_transport.GetFeatures();
        m_configuration = VirtioIommuConfiguration{
            .PageSizeMask = values[0U] | (values[1U] << 32U),
            .InputStart = ((features & 1U) != 0U) ? values[2U] | (values[3U] << 32U) : 0U,
            .InputEnd = ((features & 1U) != 0U) ? values[4U] | (values[5U] << 32U) : MaximumAddress,
            .DomainStart = ((features & 2U) != 0U) ? static_cast<UInt32>(values[6U]) : 0U,
            .DomainEnd = ((features & 2U) != 0U) ? static_cast<UInt32>(values[7U]) : 0xFFFF'FFFFU,
            .ProbeSize = static_cast<UInt32>(values[8U]),
            .Bypass = static_cast<UInt8>(values[9U] & 1U),
        };
        if ((m_configuration.InputStart > m_configuration.InputEnd) ||
            (m_configuration.DomainStart > m_configuration.DomainEnd) || ((m_configuration.PageSizeMask & 4096U) == 0U))
        {
            return VirtioTransportError::MalformedDevice;
        }
        return VirtioTransportError::None;
    }
    return VirtioTransportError::TimedOut;
}

VirtioTransportError VirtioIommuDevice::ClearBypass() noexcept
{
    SpinLockGuard guard{m_lock};
    if (!m_initialized || m_bypassCleared || !m_transport.IsReady())
    {
        return VirtioTransportError::NotReady;
    }
    const VirtioTransportError error = m_transport.WriteDeviceByte(36U, 0U);
    if (error != VirtioTransportError::None)
    {
        return error;
    }
    UInt64 value{0U};
    if (m_transport.ReadDevice(36U, MemoryMappedIoWidth::Byte, value) != VirtioTransportError::None ||
        ((value & 1U) != 0U))
    {
        return VirtioTransportError::HardwareFailure;
    }
    m_configuration.Bypass = 0U;
    m_bypassCleared = true;
    return VirtioTransportError::None;
}

VirtioTransportError VirtioIommuDevice::QuiesceForRecovery() noexcept
{
    SpinLockGuard guard{m_lock};
    // Initialization may have failed with live queues. Recovery must still run.
    m_initialized = false;
    // Disable backend operations even if a later recovery step fails.
    m_quiesced = false;
    m_bypassCleared = false;
    VirtioTransportError error = m_transport.Reset(m_polls);
    if (error != VirtioTransportError::None)
    {
        return error;
    }
    error = m_transport.Negotiate(MapUnmap | BypassConfig, 3U);
    if (error != VirtioTransportError::None)
    {
        return error;
    }
    error = m_transport.WriteDeviceByte(36U, 0U);
    if (error != VirtioTransportError::None)
    {
        return error;
    }
    UInt64 bypass{0U};
    error = m_transport.ReadDevice(36U, MemoryMappedIoWidth::Byte, bypass);
    if (error != VirtioTransportError::None)
    {
        return error;
    }
    if ((bypass & 1U) != 0U)
    {
        return VirtioTransportError::HardwareFailure;
    }
    m_quiesced = true;
    return VirtioTransportError::None;
}

bool VirtioIommuDevice::IsReady() const noexcept
{
    return m_initialized && m_bypassCleared && m_transport.IsReady();
}

void VirtioIommuDevice::BeginRequest(const UInt8 type, const Size slot) noexcept
{
    for (Size index{0U}; index < 40U; ++index)
    {
        m_request.Bytes[index] = 0U;
    }
    m_request.Bytes[0U] = type;
    VirtioPciTransport::Store(m_request.Bytes, 4U, 4U, m_configuration.DomainStart + slot);
}

IommuBackendError VirtioIommuDevice::Request(const UInt32 readableBytes) noexcept
{
    m_request.Bytes[readableBytes] = 0xFFU; // Unwritten status is never success.
    VirtioTransportError error = m_transport.Submit(0U, 0U, m_request.PhysicalAddress, readableBytes,
                                                    m_request.PhysicalAddress + readableBytes, 4U);
    if (error != VirtioTransportError::None)
    {
        return TransportError(error);
    }
    error = m_transport.Notify(0U);
    if (error != VirtioTransportError::None)
    {
        return TransportError(error);
    }
    VirtioUsedBuffer used{};
    error = m_transport.Poll(0U, m_polls, used);
    if (error != VirtioTransportError::None)
    {
        return TransportError(error);
    }
    if ((used.Head != 0U) || (used.Length != 4U))
    {
        return IommuBackendError::CorruptState;
    }
    switch (m_request.Bytes[readableBytes])
    {
    case 0U:
        return IommuBackendError::None;
    case 4U: // INVAL
    case 5U: // RANGE
    case 6U: // NOENT
        return IommuBackendError::InvalidRequest;
    case 1U: // IOERR
    case 2U: // UNSUPP can follow a partial endpoint transition.
    case 3U: // DEVERR
    case 7U: // FAULT
    case 8U: // NOMEM has no specification guarantee of a no-write failure.
        return IommuBackendError::HardwareFailure;
    default:
        return IommuBackendError::CorruptState;
    }
}

IommuBackendError VirtioIommuDevice::EndpointRequest(const UInt8 type, const Size slot, const UInt16 endpoint) noexcept
{
    BeginRequest(type, slot);
    VirtioPciTransport::Store(m_request.Bytes, 8U, 4U, endpoint);
    return Request(20U);
}

IommuBackendError VirtioIommuDevice::MappingRequest(const UInt8 type, const Size slot, const Mapping& mapping) noexcept
{
    BeginRequest(type, slot);
    VirtioPciTransport::Store(m_request.Bytes, 8U, 8U, mapping.Start);
    VirtioPciTransport::Store(m_request.Bytes, 16U, 8U, mapping.End);
    if (type == 3U)
    {
        VirtioPciTransport::Store(m_request.Bytes, 24U, 8U, mapping.Physical);
        VirtioPciTransport::Store(m_request.Bytes, 32U, 4U, mapping.Flags);
    }
    return Request((type == 3U) ? 36U : 28U);
}

Hal::Device::IommuBackendFaults VirtioIommuDevice::GetFaults() noexcept
{
    Synchronization::SpinLockGuard guard{m_lock};
    if (IsReady())
    {
        DrainFaults();
    }
    return m_faults;
}

void VirtioIommuDevice::DrainFaults() noexcept
{
    for (UInt16 count{0U}; count < EventBuffers; ++count)
    {
        VirtioUsedBuffer used{};
        if (m_transport.TryPoll(1U, used) != VirtioTransportError::None)
        {
            return;
        }
        const Size offset = Size{used.Head} * 24U;
        if ((used.Length == 24U) && (VirtioPciTransport::Load(m_events.Bytes, offset + 1U, 3U) == 0U))
        {
            const UInt64 endpoint = VirtioPciTransport::Load(m_events.Bytes, offset + 8U, 4U);
            const UInt8 reason = m_events.Bytes[offset];
            if (endpoint <= 0xFFFFU)
            {
                if (m_faults.Count != MaximumAddress)
                {
                    ++m_faults.Count;
                }
                m_faults.LastSource = static_cast<UInt16>(endpoint);
                m_faults.LastReason = (reason <= 2U) ? reason : 0U;
                m_faults.LastAddress = ((VirtioPciTransport::Load(m_events.Bytes, offset + 4U, 4U) & 0x100U) != 0U)
                                           ? VirtioPciTransport::Load(m_events.Bytes, offset + 16U, 8U)
                                           : 0U;
                for (Domain& domain : m_domains)
                {
                    for (Size index{0U}; index < domain.AttachedCount; ++index)
                    {
                        if ((domain.Owner != nullptr) && (domain.Attached[index] == endpoint))
                        {
                            if (domain.Faults.Count != MaximumAddress)
                            {
                                ++domain.Faults.Count;
                            }
                            domain.Faults.LastSource = static_cast<UInt16>(endpoint);
                            domain.Faults.LastReason = (reason <= 2U) ? reason : 0U;
                            domain.Faults.LastAddress = m_faults.LastAddress;
                        }
                    }
                }
            }
        }
        if (m_transport.Submit(1U, used.Head, 0U, 0U, m_events.PhysicalAddress + offset, 24U) !=
                VirtioTransportError::None ||
            m_transport.Notify(1U) != VirtioTransportError::None)
        {
            return;
        }
    }
}

bool VirtioIommuDomainBackend::IsReady() const noexcept
{
    SpinLockGuard guard{m_device.m_lock};
    return m_device.IsReady();
}

IommuBackendOwnershipError VirtioIommuDomainBackend::Claim(const Kernel::Device::IommuDomain& owner,
                                                           const UInt8 inputAddressBits) noexcept
{
    SpinLockGuard guard{m_device.m_lock};
    if (m_slot != VirtioIommuDevice::MaximumDomains)
    {
        return IommuBackendOwnershipError::AlreadyOwned;
    }
    if (!m_device.IsReady())
    {
        return IommuBackendOwnershipError::NotReady;
    }
    const VirtioIommuConfiguration& config = m_device.m_configuration;
    if ((inputAddressBits < 12U) || (inputAddressBits > 64U) || (config.InputStart != 0U))
    {
        return IommuBackendOwnershipError::UnsupportedConfiguration;
    }
    for (Size slot{0U};
         (slot < VirtioIommuDevice::MaximumDomains) && (slot <= UInt64{config.DomainEnd} - config.DomainStart); ++slot)
    {
        VirtioIommuDevice::Domain& domain = m_device.m_domains[slot];
        if (domain.Owner == &owner)
        {
            return IommuBackendOwnershipError::AlreadyOwned;
        }
    }
    for (Size slot{0U};
         (slot < VirtioIommuDevice::MaximumDomains) && (slot <= UInt64{config.DomainEnd} - config.DomainStart); ++slot)
    {
        VirtioIommuDevice::Domain& domain = m_device.m_domains[slot];
        if (domain.Owner == nullptr)
        {
            domain.Owner = &owner;
            domain.InputBits = inputAddressBits;
            m_slot = slot;
            return IommuBackendOwnershipError::None;
        }
    }
    return IommuBackendOwnershipError::UnsupportedConfiguration;
}

IommuBackendOwnershipError VirtioIommuDomainBackend::Release(const Kernel::Device::IommuDomain& owner) noexcept
{
    SpinLockGuard guard{m_device.m_lock};
    if ((m_slot == VirtioIommuDevice::MaximumDomains) || (m_device.m_domains[m_slot].Owner != &owner))
    {
        return IommuBackendOwnershipError::InvalidOwner;
    }
    if (!m_device.IsReady() && !m_device.m_quiesced)
    {
        return IommuBackendOwnershipError::HardwareFailure;
    }
    m_device.DrainFaults();
    VirtioIommuDevice::Domain& domain = m_device.m_domains[m_slot];
    // Do not alter retained metadata on a failed Release. Successful DETACH
    // requests may have committed: re-ATTACH before DETACH makes retries safe.
    for (Size index{0U}; !m_device.m_quiesced && (index < domain.AttachedCount); ++index)
    {
        if (m_device.EndpointRequest(1U, m_slot, domain.Attached[index]) != IommuBackendError::None ||
            m_device.EndpointRequest(2U, m_slot, domain.Attached[index]) != IommuBackendError::None)
        {
            return IommuBackendOwnershipError::HardwareFailure;
        }
    }
    domain.Owner = nullptr;
    domain.AttachedCount = 0U;
    domain.InputBits = 0U;
    domain.Faults = Hal::Device::IommuBackendFaults{};
    for (VirtioIommuDevice::Mapping& mapping : domain.Mappings)
    {
        mapping = VirtioIommuDevice::Mapping{};
    }
    m_slot = VirtioIommuDevice::MaximumDomains;
    return IommuBackendOwnershipError::None;
}

bool VirtioIommuDomainBackend::RangeIsValid(const Hal::Device::IommuBackendMapRequest& request) const noexcept
{
    const UInt64 start = request.DmaStart.GetValue();
    const UInt64 pages = request.Pages.GetValue();
    const UInt8 bits = m_device.m_domains[m_slot].InputBits;
    // Intersect device width with the remapper's input range.
    const UInt64 deviceEnd = (bits == 64U) ? MaximumAddress : (UInt64{1U} << bits) - 1U;
    const UInt64 end = deviceEnd < m_device.m_configuration.InputEnd ? deviceEnd : m_device.m_configuration.InputEnd;
    return ((start & 4095U) == 0U) && (pages != 0U) && (pages <= MaximumAddress / 4096U) && (start <= end) &&
           (pages * 4096U - 1U <= end - start) && request.Permissions.HasOnlyKnownBits() &&
           !request.Permissions.IsEmpty();
}

IommuBackendError VirtioIommuDomainBackend::Map(const Hal::Device::IommuBackendMapRequest& request) noexcept
{
    SpinLockGuard guard{m_device.m_lock};
    if ((m_slot == VirtioIommuDevice::MaximumDomains) || !m_device.IsReady())
    {
        return IommuBackendError::NotReady;
    }
    const UInt64 physical = request.PhysicalStart.GetValue();
    if (!RangeIsValid(request) || ((physical & 4095U) != 0U) ||
        (request.Pages.GetValue() * 4096U - 1U > MaximumAddress - physical))
    {
        return IommuBackendError::InvalidRequest;
    }
    VirtioIommuDevice::Domain& domain = m_device.m_domains[m_slot];
    const VirtioIommuDevice::Mapping next{
        .Start = request.DmaStart.GetValue(),
        .End = request.DmaStart.GetValue() + (request.Pages.GetValue() * 4096U - 1U),
        .Physical = physical,
        .Flags = (request.Permissions.Contains(Kernel::Device::DmaPermission::Read) ? 1U : 0U) |
                 (request.Permissions.Contains(Kernel::Device::DmaPermission::Write) ? 2U : 0U),
        .Present = true,
    };
    for (Size index{0U}; index < m_device.m_reservedRanges.Count; ++index)
    {
        const VirtioIommuReservedRange& reserved = m_device.m_reservedRanges.Data[index];
        if ((next.Start <= reserved.End) && (reserved.Start <= next.End))
        {
            return IommuBackendError::UnsupportedConfiguration;
        }
    }
    VirtioIommuDevice::Mapping* free{nullptr};
    for (VirtioIommuDevice::Mapping& mapping : domain.Mappings)
    {
        if (mapping.Present && (next.Start <= mapping.End) && (mapping.Start <= next.End))
        {
            return IommuBackendError::InvalidRequest;
        }
        if (!mapping.Present)
        {
            free = &mapping;
        }
    }
    if (free == nullptr)
    {
        return IommuBackendError::CapacityExceeded;
    }
    const IommuBackendError error =
        domain.AttachedCount == 0U ? IommuBackendError::None : m_device.MappingRequest(3U, m_slot, next);
    if (error == IommuBackendError::None)
    {
        *free = next;
    }
    return error;
}

IommuBackendError VirtioIommuDomainBackend::Unmap(const Hal::Device::IommuBackendMapRequest& request) noexcept
{
    SpinLockGuard guard{m_device.m_lock};
    if ((m_slot == VirtioIommuDevice::MaximumDomains) || !m_device.IsReady())
    {
        return IommuBackendError::NotReady;
    }
    if (!RangeIsValid(request))
    {
        return IommuBackendError::InvalidRequest;
    }
    VirtioIommuDevice::Domain& domain = m_device.m_domains[m_slot];
    const VirtioIommuDevice::Mapping range{
        .Start = request.DmaStart.GetValue(),
        .End = request.DmaStart.GetValue() + (request.Pages.GetValue() * 4096U - 1U),
    };
    for (const VirtioIommuDevice::Mapping& mapping : domain.Mappings)
    {
        if (mapping.Present && (range.Start <= mapping.End) && (mapping.Start <= range.End) &&
            ((range.Start > mapping.Start) || (range.End < mapping.End)))
        {
            return IommuBackendError::UnsupportedConfiguration;
        }
    }
    const IommuBackendError error =
        domain.AttachedCount == 0U ? IommuBackendError::None : m_device.MappingRequest(4U, m_slot, range);
    if (error == IommuBackendError::None)
    {
        for (VirtioIommuDevice::Mapping& mapping : domain.Mappings)
        {
            if (mapping.Present && (mapping.Start >= range.Start) && (mapping.End <= range.End))
            {
                mapping.Present = false;
            }
        }
    }
    return error;
}

IommuBackendError VirtioIommuDomainBackend::Attach(Hal::Device::IIommuDevice& device) noexcept
{
    const UInt32 requester = device.GetRequesterId();
    SpinLockGuard guard{m_device.m_lock};
    if ((m_slot == VirtioIommuDevice::MaximumDomains) || !m_device.IsReady())
    {
        return IommuBackendError::NotReady;
    }
    // DMA isolation's device covers segment 0, bus 0 only. Platform discovery must additionally
    // Exclude the IOMMU's own function from endpoints.
    if ((requester > 0xFFU) || m_device.m_transport.IsOwnRequester(requester))
    {
        return IommuBackendError::UnsupportedConfiguration;
    }
    for (const VirtioIommuDevice::Domain& other : m_device.m_domains)
    {
        for (Size index{0U}; index < other.AttachedCount; ++index)
        {
            if (other.Attached[index] == requester)
            {
                return IommuBackendError::InvalidRequest;
            }
        }
    }
    VirtioIommuDevice::Domain& domain = m_device.m_domains[m_slot];
    if (domain.AttachedCount == VirtioIommuDevice::MaximumAttachments)
    {
        return IommuBackendError::CapacityExceeded;
    }
    m_device.DrainFaults();
    const IommuBackendError error = m_device.EndpointRequest(1U, m_slot, static_cast<UInt16>(requester));
    // Every device error can follow a detach from its previous domain.
    // Retain possible attachment for Release's idempotent ATTACH then DETACH.
    domain.Attached[domain.AttachedCount] = static_cast<UInt16>(requester);
    ++domain.AttachedCount;
    if (error != IommuBackendError::None)
    {
        return error;
    }
    if (domain.AttachedCount == 1U)
    {
        for (const VirtioIommuDevice::Mapping& mapping : domain.Mappings)
        {
            if (mapping.Present && (m_device.MappingRequest(3U, m_slot, mapping) != IommuBackendError::None))
            {
                return IommuBackendError::HardwareFailure;
            }
        }
    }
    return IommuBackendError::None;
}

IommuBackendError VirtioIommuDomainBackend::Detach(Hal::Device::IIommuDevice& device) noexcept
{
    const UInt32 requester = device.GetRequesterId();
    SpinLockGuard guard{m_device.m_lock};
    if ((m_slot == VirtioIommuDevice::MaximumDomains) || !m_device.IsReady())
    {
        return IommuBackendError::NotReady;
    }
    VirtioIommuDevice::Domain& domain = m_device.m_domains[m_slot];
    for (Size index{0U}; index < domain.AttachedCount; ++index)
    {
        if (domain.Attached[index] == requester)
        {
            m_device.DrainFaults();
            const IommuBackendError error = m_device.EndpointRequest(2U, m_slot, static_cast<UInt16>(requester));
            if (error == IommuBackendError::None)
            {
                --domain.AttachedCount;
                domain.Attached[index] = domain.Attached[domain.AttachedCount];
            }
            return error;
        }
    }
    return IommuBackendError::InvalidRequest;
}

Hal::Device::IommuBackendFaults VirtioIommuDomainBackend::GetFaults() noexcept
{
    SpinLockGuard guard{m_device.m_lock};
    m_device.DrainFaults();
    return (m_slot < VirtioIommuDevice::MaximumDomains) ? m_device.m_domains[m_slot].Faults
                                                        : Hal::Device::IommuBackendFaults{};
}

UInt32 VirtioIommuDomainBackend::GetDomainId() const noexcept
{
    SpinLockGuard guard{m_device.m_lock};
    return (m_slot < VirtioIommuDevice::MaximumDomains)
               ? static_cast<UInt32>(m_device.m_configuration.DomainStart + m_slot)
               : 0U;
}
} // Namespace Asteria::Platform::Device
