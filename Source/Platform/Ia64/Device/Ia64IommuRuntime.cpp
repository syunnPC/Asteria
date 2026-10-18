// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Device/Ia64IommuRuntime.hpp"

#if defined(ASTERIA_TARGET_IA64) || defined(ASTERIA_TARGET_AMD64)
void* operator new(Asteria::Foundation::Size, void*) noexcept;
#else
#include <new>
#endif

#include "Kernel/Device/PciFunctionControl.hpp"
#include "Kernel/System/ObjectManager.hpp"
#include "Platform/Device/PciEcamRuntime.hpp"
#include "Platform/Firmware/Diagnostic.hpp"
#include "Platform/Ia64/Diagnostics/Ia64EvidenceRecord.hpp"

namespace Asteria::Platform::Ia64::Device
{
namespace
{
constexpr Platform::Device::VirtioIommuReservedRange ReservedRanges[]{{0xFEE0'0000U, 0xFEEF'FFFFU}};
using ReadError = Hal::Device::PciConfigurationReadError;
using WriteError = Hal::Device::PciConfigurationWriteError;
using Width = Hal::Device::MemoryMappedIoWidth;
} // Namespace

bool Ia64VirtioMmio::Discover(Hal::Device::IPciConfigurationSpace& pci, Hal::Device::IPciConfigurationWriter& writer,
                              const Kernel::Device::PciFunctionAddress function,
                              const Foundation::UInt8 physicalBits) noexcept
{
    // The caller has disabled memory decode, bus mastering and INTx. Probe
    // both halves of 64-bit BARs, restoring each before any MMIO access.
    for (Foundation::UInt8 index{0U}; index < 6U; ++index)
    {
        const Foundation::UInt16 offset = static_cast<Foundation::UInt16>(0x10U + (4U * index));
        Foundation::UInt32 low{0U};
        Foundation::UInt32 high{0U};
        Foundation::UInt32 maskLow{0U};
        Foundation::UInt32 maskHigh{0U};
        if (pci.Read32(function, offset, low) != ReadError::None)
        {
            return false;
        }
        if ((low & 1U) != 0U)
        {
            continue;
        }
        const bool wide = (low & 6U) == 4U;
        if (((low & 6U) != 0U) && !wide)
        {
            return false;
        }
        if (wide && ((index == 5U) || (pci.Read32(function, offset + 4U, high) != ReadError::None)))
        {
            return false;
        }
        bool probed = writer.Write32(function, offset, 0xFFFF'FFFFU) == WriteError::None;
        if (wide)
        {
            probed = (writer.Write32(function, offset + 4U, 0xFFFF'FFFFU) == WriteError::None) && probed;
        }
        probed = (pci.Read32(function, offset, maskLow) == ReadError::None) && probed;
        if (wide)
        {
            probed = (pci.Read32(function, offset + 4U, maskHigh) == ReadError::None) && probed;
        }
        const bool restoredLow = writer.Write32(function, offset, low) == WriteError::None;
        const bool restoredHigh = !wide || (writer.Write32(function, offset + 4U, high) == WriteError::None);
        if (!probed || !restoredLow || !restoredHigh)
        {
            return false;
        }
        const Foundation::UInt64 base = (Foundation::UInt64{high} << 32U) | (low & ~Foundation::UInt32{15U});
        const Foundation::UInt64 mask = (Foundation::UInt64{maskHigh} << 32U) | (maskLow & ~Foundation::UInt32{15U});
        const Foundation::UInt64 length =
            wide ? (~mask + 1U)
                 : Foundation::UInt64{static_cast<Foundation::UInt32>(~static_cast<Foundation::UInt32>(mask) + 1U)};
        if ((mask != 0U) && (base != 0U))
        {
            if ((length == 0U) || ((length & (length - 1U)) != 0U) || ((base & (length - 1U)) != 0U) ||
                !m_access.IsPhysicalRangeImplemented(base, length, physicalBits))
            {
                return false;
            }
            m_bars[index] = {base, length};
        }
        else if (mask != 0U)
        {
            return false;
        }
        if (wide)
        {
            ++index;
        }
    }
    return true;
}

bool Ia64VirtioMmio::Contains(const Foundation::UInt64 address, const Foundation::UInt64 bytes) const noexcept
{
    if ((bytes == 0U) || ((address & (bytes - 1U)) != 0U))
    {
        return false;
    }
    for (const auto& bar : m_bars)
    {
        if ((address >= bar.Base) && (bytes <= bar.Length) && ((address - bar.Base) <= (bar.Length - bytes)))
        {
            return true;
        }
    }
    return false;
}

Hal::Device::MemoryMappedIoReadError Ia64VirtioMmio::Read(const Foundation::UInt64 address, const Width width,
                                                          Foundation::UInt64& output) noexcept
{
    if ((width != Width::Byte) && (width != Width::Word) && (width != Width::DoubleWord))
    {
        return Hal::Device::MemoryMappedIoReadError::UnsupportedWidth;
    }
    if (!Contains(address, static_cast<Foundation::UInt64>(width)))
    {
        return Hal::Device::MemoryMappedIoReadError::InvalidAddress;
    }
    if (width == Width::Byte)
    {
        output = m_access.ReadByte(address);
    }
    else if (width == Width::Word)
    {
        output = m_access.ReadWord(address);
    }
    else
    {
        output = m_access.ReadDoubleWord(address);
    }
    return Hal::Device::MemoryMappedIoReadError::None;
}

Hal::Device::MemoryMappedIoWriteError Ia64VirtioMmio::Write(const Foundation::UInt64 address, const Width width,
                                                            const Foundation::UInt64 value) noexcept
{
    if ((width != Width::Byte) && (width != Width::Word) && (width != Width::DoubleWord))
    {
        return Hal::Device::MemoryMappedIoWriteError::UnsupportedWidth;
    }
    if (!Contains(address, static_cast<Foundation::UInt64>(width)))
    {
        return Hal::Device::MemoryMappedIoWriteError::InvalidAddress;
    }
    const Foundation::UInt64 maximum = (Foundation::UInt64{1U} << (8U * static_cast<Foundation::UInt64>(width))) - 1U;
    if (value > maximum)
    {
        return Hal::Device::MemoryMappedIoWriteError::ValueOutOfRange;
    }
    if (width == Width::Byte)
    {
        m_access.WriteByte(address, static_cast<Foundation::UInt8>(value));
    }
    else if (width == Width::Word)
    {
        m_access.WriteWord(address, static_cast<Foundation::UInt16>(value));
    }
    else
    {
        m_access.WriteDoubleWord(address, static_cast<Foundation::UInt32>(value));
    }
    return Hal::Device::MemoryMappedIoWriteError::None;
}

Ia64IommuRuntime::Ia64IommuRuntime(Kernel::Device::PciEcamConfigurationSpace& pci,
                                   const Ia64PhysicalMemoryAccess& access) noexcept
    : m_pci{pci}, m_mmio{access}, m_transport{pci, m_mmio, m_mmio}, m_device{m_transport, {ReservedRanges, 1U}},
      m_vtd{pci, access}
{
}

Platform::Device::VirtioDmaPage Ia64IommuRuntime::Page(const Foundation::Size index) noexcept
{
    return {m_pages[index], reinterpret_cast<Foundation::UIntPointer>(m_pages[index])};
}

bool Ia64IommuRuntime::Start(const Kernel::Device::PciFunctionAddress function,
                             const Foundation::UInt8 physicalBits) noexcept
{
    using Error = Platform::Device::VirtioTransportError;
    Foundation::UInt32 command{0U};
    if ((m_pci.Read32(function, 4U, command) != ReadError::None) ||
        (m_pci.Write32(function, 4U, (command & 0xFFFFU & ~7U) | 0x400U) != WriteError::None) ||
        !m_mmio.Discover(m_pci, m_pci, function, physicalBits) ||
        (m_pci.Write32(function, 4U, (command & 0xFFFFU & ~5U) | 0x402U) != WriteError::None) ||
        (m_transport.Discover(function, m_mmio.GetBars()) != Error::None))
    {
        return false;
    }
    const Platform::Device::VirtioDmaPage requestQueue[]{Page(0U), Page(1U), Page(2U)};
    const Platform::Device::VirtioDmaPage eventQueue[]{Page(3U), Page(4U), Page(5U)};
    // AHCI block I/O / DMA isolation: virtio completion removes translations but cannot attest
    // physical DMA completion. The core retains normal-Unmap pins too, until the device-quiet
    // confirmation.
    if (m_device.Initialize(requestQueue, eventQueue, Page(6U), Page(7U),
                            Platform::Device::VirtioPciTransport::MaximumPolls,
                            Platform::Device::VirtioIommuCompletionPolicy::TranslationOnly) != Error::None)
    {
        return false;
    }
    if (m_device.ClearBypass() != Error::None)
    {
        return false;
    }
    // Bypass is now verified clear with every published bus master off.
    // Only the IOMMU's own untranslated queue DMA resumes here.
    if (m_pci.Write32(function, 4U, (command & 0xFFFFU & ~1U) | 0x406U) != WriteError::None)
    {
        return false;
    }
    auto& evidence = Diagnostics::GetIa64EvidenceRecord().Iommu;
    evidence.Bypass = m_device.GetConfiguration().Bypass;
    evidence.Ready = m_device.IsReady() ? 1U : 0U;
    evidence.QueuePhysical = Page(0U).PhysicalAddress;
    evidence.QueuePages = 8U;
    for (Foundation::Size slot{0U}; slot < Kernel::Device::IommuDomainPool::Capacity; ++slot)
    {
        m_backends[slot] = new (m_backendStorage[slot]) Platform::Device::VirtioIommuDomainBackend{m_device};
    }
    return m_device.IsReady();
}

Ia64VtdHandoff Ia64IommuRuntime::HandOff(Platform::Device::PciEcamRuntime& runtime, const Ia64VtdInput& input,
                                         const Kernel::Device::PciFunctionAddress* const bootStorage) noexcept
{
    if ((input.BootInformation == nullptr) || !Ia64VtdRuntime::IsDescribed(input.BootInformation->Acpi))
    {
        return Ia64VtdHandoff::Ready;
    }
    return m_vtd.HandOff(runtime, input, bootStorage);
}

bool Ia64IommuRuntime::Initialize(Platform::Device::PciEcamRuntime& runtime, const Ia64VtdInput& input) noexcept
{
    auto& backend = Diagnostics::GetIa64EvidenceRecord().IommuBackend;
    if ((input.BootInformation != nullptr) && Ia64VtdRuntime::IsDescribed(input.BootInformation->Acpi))
    {
        m_vtdSelected = true;
        const bool initialized = m_vtd.Initialize(runtime, input);
        Diagnostics::PublishIa64Evidence(Diagnostics::GetIa64EvidenceRecord().Iommu.Published);
        Diagnostics::PublishIa64Evidence(backend.Published);
        return initialized;
    }
    const bool initialized = InitializeVirtio(runtime, input.PhysicalAddressBits);
    backend.Backend = m_present ? 2U : 0U;
    Platform::Firmware::WriteDiagnosticAscii(m_present ? "ASTERIA:IA64-IOMMU:VIRTIO\r\n"
                                                       : "ASTERIA:IA64-IOMMU:NONE\r\n");
    Diagnostics::PublishIa64Evidence(backend.Published);
    return initialized;
}

bool Ia64IommuRuntime::InitializeVirtio(Platform::Device::PciEcamRuntime& runtime,
                                        const Foundation::UInt8 physicalBits) noexcept
{
    auto& evidence = Diagnostics::GetIa64EvidenceRecord().Iommu;
    Platform::Device::PciEcamRuntimeSummary summary{};
    if (runtime.GetSummary(summary) != Platform::Device::PciEcamFunctionQueryError::None)
    {
        Diagnostics::PublishIa64Evidence(evidence.Published);
        return true;
    }
    Kernel::Device::PciFunctionAddress own{};
    for (Foundation::Size index{0U}; index < summary.PublishedFunctionCount; ++index)
    {
        Platform::Device::PciFunctionPublication publication{};
        if (runtime.GetPublishedFunction(index, publication) != Platform::Device::PciEcamFunctionQueryError::None)
        {
            return false;
        }
        const auto& function = publication.Descriptor;
        if ((function.VendorId == 0x1AF4U) && (function.DeviceId == 0x1057U) && (function.Address.GetSegment() == 0U) &&
            (function.Address.GetBus() == 0U))
        {
            if (m_present)
            {
                return false;
            }
            m_present = true;
            if (function.HeaderLayout != 0U)
            {
                return false;
            }
            own = function.Address;
            m_queueFunction = own;
            evidence.Present = 1U;
            evidence.Requester = own.GetRequesterId();
        }
    }
    if (!m_present)
    {
        Diagnostics::PublishIa64Evidence(evidence.Published);
        return true;
    }
    // After the boot storage diagnostic, fail closed if even one published
    // function cannot be stopped. The IOMMU alone resumes its untranslated DMA.
    for (Foundation::Size index{0U}; index < summary.PublishedFunctionCount; ++index)
    {
        Platform::Device::PciFunctionPublication publication{};
        if ((runtime.GetPublishedFunction(index, publication) != Platform::Device::PciEcamFunctionQueryError::None) ||
            !Kernel::Device::SetPciBusMastering(m_pci, m_pci, publication.Descriptor.Address, false))
        {
            return false;
        }
        ++evidence.SweptFunctions;
    }
    const bool started = Start(own, physicalBits);
    Diagnostics::PublishIa64Evidence(evidence.Published);
    return started;
}

void Ia64IommuRuntime::Record(const Kernel::System::ObjectManager& objects) noexcept
{
    auto& evidence = Diagnostics::GetIa64EvidenceRecord().Iommu;
    const auto domains = objects.GetIommuDomainSnapshot();
    if (m_vtdSelected)
    {
        evidence.DomainsCreated = domains.Created;
        evidence.DomainsReclaimed = domains.Reclaimed;
        m_vtd.Record();
        return;
    }
    const auto faults = m_device.GetFaults();
    evidence.Ready = m_device.IsReady() ? 1U : 0U;
    evidence.DomainsCreated = domains.Created;
    evidence.DomainsReclaimed = domains.Reclaimed;
    evidence.Faults = faults.Count;
    evidence.LastFaultSource = faults.LastSource;
    evidence.LastFaultReason = faults.LastReason;
    evidence.LastFaultAddress = faults.LastAddress;
    Diagnostics::PublishIa64Evidence(evidence.Published);
}

Kernel::Device::IommuAvailability Ia64IommuRuntime::GetAvailability() const noexcept
{
    if (m_vtdSelected)
    {
        return m_vtd.IsReady() ? Kernel::Device::IommuAvailability::Working
                               : Kernel::Device::IommuAvailability::Refused;
    }
    if (m_present)
    {
        return m_device.IsReady() ? Kernel::Device::IommuAvailability::Working
                                  : Kernel::Device::IommuAvailability::Refused;
    }
    return Kernel::Device::IommuAvailability::Absent;
}

void Ia64IommuRuntime::ApplyTrustedDma(Kernel::Object::ObjectRegistry& registry, const Boot::BootInfo& information,
                                       const Platform::Device::TrustedDmaCandidate* const candidates,
                                       const Foundation::Size count) noexcept
{
    static_cast<void>(
        m_trusted.Apply(registry, information.Trust, information.Files, GetAvailability(), candidates, count));
}

bool Ia64IommuRuntime::Configure(Kernel::System::ObjectManager& objects) noexcept
{
    if (GetAvailability() == Kernel::Device::IommuAvailability::Working)
    {
        return m_vtdSelected ? objects.ConfigureIommu(m_slots, m_vtd.GetBackends())
                             : objects.ConfigureIommu(m_slots, m_backends);
    }
    // A refused or absent IOMMU configures no domains: bus mastering stays refused, unless a
    // BOOT.CFG trust entry granted a function trusted DMA; then the identity backends serve that
    // function's driver and no other.
    return (m_trusted.GetTrustedCount() == 0U) ||
           objects.ConfigureIommu(m_slots, m_trusted.GetBackends(), Kernel::System::IommuAdmission::TrustedAssignees);
}
} // Namespace Asteria::Platform::Ia64::Device
