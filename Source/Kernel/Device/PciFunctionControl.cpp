// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Device/PciFunctionControl.hpp"

#include "Kernel/Device/AhciPciBootstrapBinding.hpp"

// Target images define placement new once (Kernel/Boot/KernelEntry.cpp).
#if defined(ASTERIA_TARGET_IA64) || defined(ASTERIA_TARGET_AMD64)
void* operator new(Asteria::Foundation::Size, void*) noexcept;
#else
#include <new>
#endif

namespace Asteria::Kernel::Device
{
namespace
{
// PCI class 0C/03 programming interfaces (USB 1.1/2.0 host controllers).
enum class UsbHostKind : Foundation::UInt8
{
    None,
    Uhci,
    Ohci,
    Ehci,
};

[[nodiscard]] UsbHostKind KindOf(const PciFunctionDescriptor& descriptor) noexcept
{
    if ((descriptor.ClassCode != 0x0CU) || (descriptor.Subclass != 0x03U))
    {
        return UsbHostKind::None;
    }
    switch (descriptor.ProgrammingInterface)
    {
    case 0x00U:
        return UsbHostKind::Uhci;
    case 0x10U:
        return UsbHostKind::Ohci;
    case 0x20U:
        return UsbHostKind::Ehci;
    default:
        return UsbHostKind::None;
    }
}

// Whether offset heads a capability of the function's standard list (PCI
// 3.0 section 6.7): bounded, a malformed list counts as containing it.
[[nodiscard]] bool InStandardList(Hal::Device::IPciConfigurationSpace& reader, const PciFunctionAddress address,
                                  const Foundation::UInt16 offset) noexcept
{
    Foundation::UInt32 command{0U};
    Foundation::UInt32 pointer{0U};
    if ((reader.Read32(address, 0x04U, command) != Hal::Device::PciConfigurationReadError::None) ||
        (reader.Read32(address, 0x34U, pointer) != Hal::Device::PciConfigurationReadError::None))
    {
        return true;
    }
    if ((command & (Foundation::UInt32{1U} << 20U)) == 0U)
    {
        return false;
    }
    Foundation::UInt32 next = pointer & 0xFCU;
    for (Foundation::Size count{0U}; (next != 0U) && (count < 48U); ++count)
    {
        Foundation::UInt32 header{0U};
        if ((next < 0x40U) || (next == offset) ||
            (reader.Read32(address, static_cast<Foundation::UInt16>(next), header) !=
             Hal::Device::PciConfigurationReadError::None))
        {
            return true;
        }
        next = (header >> 8U) & 0xFCU;
    }
    return next != 0U;
}

// Whether an EHCI's dword at offset is a USB Legacy Support capability
// (EHCI 1.0 section 2.1.7, capability ID 1); header gets the dword.
[[nodiscard]] bool IsEhciLegacySupport(Hal::Device::IPciConfigurationSpace& reader, const PciFunctionAddress address,
                                       const Foundation::UInt16 offset, Foundation::UInt32& header) noexcept
{
    return (offset >= 0x40U) && (offset <= 0xF8U) && ((offset & 3U) == 0U) &&
           (reader.Read32(address, offset, header) == Hal::Device::PciConfigurationReadError::None) &&
           ((header & 0xFFU) == 1U) && !InStandardList(reader, address, offset);
}

// The configuration dwords of a USB host controller a read may answer: the
// identity (0x00), class (0x08), BAR 0 (0x10) and interrupt (0x3C) dwords, an
// EHCI's dwords from 0x40 on (its EECP list, EHCI 1.0 section 2.1.7) and a
// UHCI's LEGSUP (0xC0, Intel UHCI design guide 1.1 section 5.2.1).
[[nodiscard]] bool IsReadable(const UsbHostKind kind, const Foundation::UInt16 offset) noexcept
{
    constexpr Foundation::UInt16 UhciLegacySupport{0xC0U};
    if ((kind == UsbHostKind::None) || ((offset & 3U) != 0U) || (offset > 0xFCU))
    {
        return false;
    }
    return (offset == 0x00U) || (offset == 0x08U) || (offset == 0x10U) ||
           (offset == PciFunctionControl::InterruptRegister) || ((kind == UsbHostKind::Ehci) && (offset >= 0x40U)) ||
           ((kind == UsbHostKind::Uhci) && (offset == UhciLegacySupport));
}
} // Namespace

bool PciFunctionControl::Configure(Hal::Device::IPciConfigurationSpace& reader,
                                   Hal::Device::IPciConfigurationWriter& writer) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_writer != nullptr)
    {
        return false;
    }
    m_reader = &reader;
    m_writer = &writer;
    return true;
}

PciFunctionControlError PciFunctionControl::Assign(PciFunction& function, const Protection::ProtectionDomainId driver,
                                                   Protection::ProtectionDomain& driverStorage) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_writer == nullptr)
    {
        return PciFunctionControlError::NotConfigured;
    }
    Entry* entry = FindLocked(function);
    if ((entry != nullptr) && (entry->MessagesEnabled || entry->LineEnabled))
    {
        return PciFunctionControlError::MessagesInUse;
    }
    if (entry != nullptr)
    {
        const PciFunctionControlError released = ReleaseLocked(*entry);
        if (released != PciFunctionControlError::None)
        {
            return released;
        }
    }
    for (Entry& candidate : m_entries)
    {
        entry = ((entry == nullptr) && (candidate.Function == nullptr)) ? &candidate : entry;
    }
    if (entry == nullptr)
    {
        return PciFunctionControlError::CapacityExceeded;
    }
    const PciFunctionError restriction = function.GetAssignmentRestriction();
    if (restriction == PciFunctionError::BridgeReserved)
    {
        return PciFunctionControlError::BridgeReserved;
    }
    if (restriction == PciFunctionError::DisplayReserved)
    {
        return PciFunctionControlError::DisplayReserved;
    }
    if (restriction == PciFunctionError::ReservedMemory)
    {
        return PciFunctionControlError::ReservedMemory;
    }
    // A former driver's MSI and INTx are turned off before the next one gets
    // the function.
    const PciFunctionAddress address = function.GetSnapshot().Descriptor.Address;
    PciMessageCapability messages{};
    if (!SetBusMasterLocked(function, false) || !SetInterruptDisableLocked(function, true) ||
        (FindPciMessageCapability(*m_reader, address, messages) &&
         !DisablePciMessages(*m_reader, *m_writer, address, messages)))
    {
        return PciFunctionControlError::ConfigurationRejected;
    }
    const auto assigned = function.Assign(driver, driverStorage);
    if (!assigned.HasValue())
    {
        return (assigned.GetError().Error == PciFunctionError::Quarantined)
                   ? PciFunctionControlError::Quarantined
                   : PciFunctionControlError::AssignmentRejected;
    }
    *entry = Entry{
        .Function = &function,
        .Assignment = assigned.GetValue().Assignment,
        .Driver = &driverStorage,
        .Domain = nullptr,
        .Attachment = {},
        .Messages = {},
        .MessagesEnabled = false,
        .LineEnabled = false,
        .Revoked = false,
        .DmaPending = false,
    };
    ++m_assignments;
    return PciFunctionControlError::None;
}

PciFunctionControlError PciFunctionControl::Release(const PciFunction& function) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_writer == nullptr)
    {
        return PciFunctionControlError::NotConfigured;
    }
    Entry* const entry = FindLocked(function);
    return (entry != nullptr) ? ReleaseLocked(*entry) : PciFunctionControlError::NotAssigned;
}

PciFunctionControlError PciFunctionControl::EnableBusMaster(const PciFunction& function,
                                                            const Protection::ProtectionDomainId caller) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_writer == nullptr)
    {
        return PciFunctionControlError::NotConfigured;
    }
    const Entry* const entry = FindLocked(function);
    const PciFunctionSnapshot snapshot = function.GetSnapshot();
    PciFunctionControlError refused{PciFunctionControlError::None};
    if ((entry == nullptr) || (snapshot.State != PciFunctionState::Assigned))
    {
        refused = PciFunctionControlError::NotAssigned;
    }
    else if (!snapshot.Driver.IsEqualTo(caller))
    {
        refused = PciFunctionControlError::NotAssignee;
    }
    else if (entry->Revoked)
    {
        refused = PciFunctionControlError::Revoked;
    }
    else if (!snapshot.IommuClaimed || (entry->Domain == nullptr) ||
             (entry->Domain->GetSnapshot().State != IommuDomainState::Ready))
    {
        refused = PciFunctionControlError::IommuRequired;
    }
    if (refused != PciFunctionControlError::None)
    {
        ++m_refusals;
        return refused;
    }
    if (!SetBusMasterLocked(function, true))
    {
        return PciFunctionControlError::ConfigurationRejected;
    }
    ++m_enables;
    return PciFunctionControlError::None;
}

PciFunctionControlError PciFunctionControl::DisableBusMaster(const PciFunction& function,
                                                             const Protection::ProtectionDomainId caller,
                                                             const bool revoke) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_writer == nullptr)
    {
        return PciFunctionControlError::NotConfigured;
    }
    Entry* const entry = FindLocked(function);
    if (entry == nullptr)
    {
        return PciFunctionControlError::NotAssigned;
    }
    if (!revoke && !function.GetSnapshot().Driver.IsEqualTo(caller))
    {
        return PciFunctionControlError::NotAssignee;
    }
    if (!SetBusMasterLocked(function, false))
    {
        return PciFunctionControlError::ConfigurationRejected;
    }
    if (revoke)
    {
        entry->Revoked = true;
        ++m_revocations;
    }
    return PciFunctionControlError::None;
}

bool PciFunctionControl::HasTrustedAssignment(const Protection::ProtectionDomainId caller) const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    for (const Entry& entry : m_entries)
    {
        if ((entry.Function != nullptr) && entry.Function->HasTrustedDma() &&
            entry.Function->GetSnapshot().Driver.IsEqualTo(caller))
        {
            return true;
        }
    }
    return false;
}

PciFunctionControlError PciFunctionControl::Attach(PciFunction& function, const Protection::ProtectionDomainId caller,
                                                   IommuDomain& domain) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_writer == nullptr)
    {
        return PciFunctionControlError::NotConfigured;
    }
    Entry* const entry = FindLocked(function);
    if (entry == nullptr)
    {
        return PciFunctionControlError::NotAssigned;
    }
    if (!function.GetSnapshot().Driver.IsEqualTo(caller))
    {
        return PciFunctionControlError::NotAssignee;
    }
    if (entry->Revoked)
    {
        return PciFunctionControlError::Revoked;
    }
    if (entry->Domain != nullptr)
    {
        return PciFunctionControlError::AttachmentConflict;
    }
    if (!SetBusMasterLocked(function, false))
    {
        return PciFunctionControlError::ConfigurationRejected;
    }
    // An AHCI may cache addresses beyond an IOTLB drain on either ISA. Normal Unmap retains its
    // storage until reset too.
    const auto descriptor = function.GetSnapshot().Descriptor;
    if (AhciPciBootstrapBinding::IsAhciEndpoint(descriptor))
    {
        domain.RequireDmaQuiescence();
    }
    const auto beforeAttachment = domain.GetSnapshot().LastAttachmentSequence;
    const auto attached = domain.Attach(function);
    if (!attached.HasValue())
    {
        // An ambiguous native Attach already owns a conservative attachment. AHCI block I/O:
        // preserve this association for Quiesce/retry just as after an ambiguous Release. Ordinary
        // refusals publish none.
        const auto afterAttachment = domain.GetSnapshot().LastAttachmentSequence;
        if (afterAttachment != beforeAttachment)
        {
            entry->Domain = &domain;
            // No successful token was published; a Faulted domain permits
            // only Quiesce, which does not need a per-attachment token.
            entry->Attachment = IommuAttachmentToken{};
        }
        // A present mapping overlaps the function's firmware DMA reservation.
        return (attached.GetError().Error == IommuDomainError::ReservedDmaRange)
                   ? PciFunctionControlError::ReservedMemory
                   : PciFunctionControlError::IommuRejected;
    }
    entry->Domain = &domain;
    entry->Attachment = attached.GetValue().Attachment;
    return PciFunctionControlError::None;
}

PciFunctionControlError PciFunctionControl::Detach(const PciFunction& function,
                                                   const Protection::ProtectionDomainId caller,
                                                   const IommuDomain& domain) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_writer == nullptr)
    {
        return PciFunctionControlError::NotConfigured;
    }
    Entry* const entry = FindLocked(function);
    if (entry == nullptr)
    {
        return PciFunctionControlError::NotAssigned;
    }
    if (!function.GetSnapshot().Driver.IsEqualTo(caller))
    {
        return PciFunctionControlError::NotAssignee;
    }
    if (entry->Domain != &domain)
    {
        return PciFunctionControlError::AttachmentConflict;
    }
    if (!SetBusMasterLocked(function, false))
    {
        return PciFunctionControlError::ConfigurationRejected;
    }
    const IommuDomainError detached = entry->Domain->Detach(entry->Attachment).Error;
    if (detached != IommuDomainError::None)
    {
        return (detached == IommuDomainError::ParkingRefused) ? PciFunctionControlError::DmaPending
                                                              : PciFunctionControlError::IommuRejected;
    }
    entry->Domain = nullptr;
    entry->Attachment = IommuAttachmentToken{};
    return PciFunctionControlError::None;
}

PciFunctionControlError PciFunctionControl::EnableMessages(const PciFunction& function,
                                                           const Protection::ProtectionDomainId caller,
                                                           const Foundation::UInt64 address,
                                                           const Foundation::UInt16 data) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_writer == nullptr)
    {
        return PciFunctionControlError::NotConfigured;
    }
    Entry* const entry = FindLocked(function);
    if (entry == nullptr)
    {
        return PciFunctionControlError::NotAssigned;
    }
    if (!function.GetSnapshot().Driver.IsEqualTo(caller))
    {
        return PciFunctionControlError::NotAssignee;
    }
    if (entry->MessagesEnabled)
    {
        return PciFunctionControlError::MessagesInUse;
    }
    const PciFunctionAddress target = function.GetSnapshot().Descriptor.Address;
    PciMessageCapability messages{};
    if (!FindPciMessageCapability(*m_reader, target, messages))
    {
        return PciFunctionControlError::MessagesUnavailable;
    }
    if (!EnablePciMessages(*m_reader, *m_writer, target, messages, address, data))
    {
        return PciFunctionControlError::ConfigurationRejected;
    }
    entry->Messages = messages;
    entry->MessagesEnabled = true;
    return PciFunctionControlError::None;
}

PciFunctionControlError PciFunctionControl::DisableMessages(const PciFunction& function) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_writer == nullptr)
    {
        return PciFunctionControlError::NotConfigured;
    }
    Entry* const entry = FindLocked(function);
    if ((entry == nullptr) || !entry->MessagesEnabled)
    {
        return PciFunctionControlError::NotAssigned;
    }
    if (!DisablePciMessages(*m_reader, *m_writer, function.GetSnapshot().Descriptor.Address, entry->Messages))
    {
        return PciFunctionControlError::ConfigurationRejected;
    }
    entry->MessagesEnabled = false;
    return PciFunctionControlError::None;
}

PciFunctionControlError PciFunctionControl::GetInterruptLine(const PciFunction& function,
                                                             Foundation::UInt32& line) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_reader == nullptr)
    {
        return PciFunctionControlError::NotConfigured;
    }
    if (FindLocked(function) == nullptr)
    {
        return PciFunctionControlError::NotAssigned;
    }
    // The interrupt line in byte 0, the pin (1-4, 0 for none) in byte 1.
    Foundation::UInt32 interrupt{0U};
    if (m_reader->Read32(function.GetSnapshot().Descriptor.Address, InterruptRegister, interrupt) !=
        Hal::Device::PciConfigurationReadError::None)
    {
        return PciFunctionControlError::ConfigurationRejected;
    }
    if (((interrupt >> 8U) & 0xFFU) == 0U)
    {
        return PciFunctionControlError::MessagesUnavailable;
    }
    line = interrupt & 0xFFU;
    return PciFunctionControlError::None;
}

PciFunctionControlError PciFunctionControl::EnableLine(const PciFunction& function,
                                                       const Protection::ProtectionDomainId caller) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_writer == nullptr)
    {
        return PciFunctionControlError::NotConfigured;
    }
    Entry* const entry = FindLocked(function);
    if (entry == nullptr)
    {
        return PciFunctionControlError::NotAssigned;
    }
    if (!function.GetSnapshot().Driver.IsEqualTo(caller))
    {
        return PciFunctionControlError::NotAssignee;
    }
    if (entry->MessagesEnabled || entry->LineEnabled)
    {
        return PciFunctionControlError::MessagesInUse;
    }
    if (!SetInterruptDisableLocked(function, false))
    {
        return PciFunctionControlError::ConfigurationRejected;
    }
    entry->LineEnabled = true;
    return PciFunctionControlError::None;
}

PciFunctionControlError PciFunctionControl::DisableLine(const PciFunction& function) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_writer == nullptr)
    {
        return PciFunctionControlError::NotConfigured;
    }
    Entry* const entry = FindLocked(function);
    if ((entry == nullptr) || !entry->LineEnabled)
    {
        return PciFunctionControlError::NotAssigned;
    }
    if (!SetInterruptDisableLocked(function, true))
    {
        return PciFunctionControlError::ConfigurationRejected;
    }
    entry->LineEnabled = false;
    return PciFunctionControlError::None;
}

PciFunctionControlError PciFunctionControl::ReadReleasedConfiguration(const PciFunction& function,
                                                                      const Foundation::UInt16 offset,
                                                                      Foundation::UInt32& result) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    result = 0U;
    if (m_reader == nullptr)
    {
        return PciFunctionControlError::NotConfigured;
    }
    const PciFunctionSnapshot snapshot = function.GetSnapshot();
    if ((FindLocked(function) != nullptr) || (snapshot.State == PciFunctionState::Assigned))
    {
        return PciFunctionControlError::NotAssignee;
    }
    if (!IsReadable(KindOf(snapshot.Descriptor), offset))
    {
        return PciFunctionControlError::AccessDenied;
    }
    return (m_reader->Read32(snapshot.Descriptor.Address, offset, result) ==
            Hal::Device::PciConfigurationReadError::None)
               ? PciFunctionControlError::None
               : PciFunctionControlError::ConfigurationRejected;
}

PciFunctionControlError PciFunctionControl::AccessLegacySupport(const PciFunction& function,
                                                                const Protection::ProtectionDomainId caller,
                                                                const Foundation::UInt16 offset, const bool write,
                                                                const Foundation::UInt32 value,
                                                                Foundation::UInt32& result) noexcept
{
    constexpr Foundation::UInt32 OsOwned{Foundation::UInt32{1U} << 24U};
    constexpr Foundation::UInt32 EhciSmiEnables{0x0000'E03FU};
    constexpr Foundation::UInt32 EhciSmiStatus{0xE000'0000U};
    constexpr Foundation::UInt32 UhciRelinquish{0xAF00U};
    constexpr Foundation::UInt16 UhciLegacySupport{0xC0U};
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    result = 0U;
    if ((m_reader == nullptr) || (m_writer == nullptr))
    {
        return PciFunctionControlError::NotConfigured;
    }
    const PciFunctionSnapshot snapshot = function.GetSnapshot();
    if ((FindLocked(function) == nullptr) || (snapshot.State != PciFunctionState::Assigned))
    {
        return PciFunctionControlError::NotAssigned;
    }
    if (!snapshot.Driver.IsEqualTo(caller))
    {
        return PciFunctionControlError::NotAssignee;
    }
    const UsbHostKind kind = KindOf(snapshot.Descriptor);
    const PciFunctionAddress address = snapshot.Descriptor.Address;
    if ((kind == UsbHostKind::None) || ((offset & 3U) != 0U) || (offset > 0xFCU))
    {
        return PciFunctionControlError::AccessDenied;
    }
    if (!write)
    {
        if (!IsReadable(kind, offset))
        {
            return PciFunctionControlError::AccessDenied;
        }
        return (m_reader->Read32(address, offset, result) == Hal::Device::PciConfigurationReadError::None)
                   ? PciFunctionControlError::None
                   : PciFunctionControlError::ConfigurationRejected;
    }
    Foundation::UInt32 current{0U};
    Foundation::UInt32 written{0U};
    if ((kind == UsbHostKind::Uhci) && (offset == UhciLegacySupport) && ((value & ~UhciRelinquish) == 0U))
    {
        if (m_reader->Read32(address, offset, current) != Hal::Device::PciConfigurationReadError::None)
        {
            return PciFunctionControlError::ConfigurationRejected;
        }
        // LEGSUP is the low half; the reserved high half is written back.
        written = (current & 0xFFFF'0000U) | value;
    }
    else if ((kind == UsbHostKind::Ehci) && IsEhciLegacySupport(*m_reader, address, offset, current))
    {
        // Only the OS Owned semaphore, set: the BIOS semaphore and the
        // read-only ID and next pointer keep what the register holds now.
        if (((value & OsOwned) == 0U) || ((value & 0xFFFFU) != (current & 0xFFFFU)))
        {
            return PciFunctionControlError::AccessDenied;
        }
        written = current | OsOwned;
    }
    else if ((kind == UsbHostKind::Ehci) && (offset >= 0x44U) &&
             IsEhciLegacySupport(*m_reader, address, static_cast<Foundation::UInt16>(offset - 4U), current) &&
             ((value & EhciSmiEnables) == 0U))
    {
        // USBLEGCTLSTS: every SMI enable zero, status acknowledged as asked.
        written = value & EhciSmiStatus;
    }
    else
    {
        return PciFunctionControlError::AccessDenied;
    }
    return (m_writer->Write32(address, offset, written) == Hal::Device::PciConfigurationWriteError::None)
               ? PciFunctionControlError::None
               : PciFunctionControlError::ConfigurationRejected;
}

bool PciFunctionControl::QuiesceDomain(IommuDomain& domain) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    Parker parker{*this};
    if (domain.RequiresDmaQuiescence())
    {
        for (const Entry& entry : m_entries)
        {
            if (entry.Domain == &domain)
            {
                parker.Add(*entry.Function, entry.Assignment.GetSequence());
            }
        }
    }
    return QuiesceLocked(domain, (parker.GetCount() != 0U) ? &parker : nullptr);
}

bool PciFunctionControl::QuiesceDrainedDomain(IommuDomain& domain,
                                              const Resource::IResourceDomainSubtree& subtree) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    Parker parker{*this};
    for (const Entry& entry : m_entries)
    {
        if ((entry.Domain == &domain) && (domain.RequiresDmaQuiescence() || IsChargedTo(entry, subtree)))
        {
            parker.Add(*entry.Function, entry.Assignment.GetSequence());
        }
    }
    return QuiesceLocked(domain, (parker.GetCount() != 0U) ? &parker : nullptr);
}

// The domain is quiesced before its functions forget it: one that refuses
// (a parker without room) keeps its attachments for a retry.
bool PciFunctionControl::QuiesceLocked(IommuDomain& domain, Parker* const parker) noexcept
{
    bool stopped{true};
    for (const Entry& entry : m_entries)
    {
        // Without a configuration writer no entry exists.
        stopped = ((entry.Domain != &domain) || SetBusMasterLocked(*entry.Function, false)) && stopped;
    }
    // AHCI block I/O: each stage retains its retry association. Do not release translations while a
    // bus-master-off write failed.
    if (!stopped)
    {
        return false;
    }
    if ((parker != nullptr) && (domain.GetSnapshot().MappingCount != 0U))
    {
        for (Entry& entry : m_entries)
        {
            if ((entry.Domain == &domain) && parker->Contains(*entry.Function))
            {
                entry.DmaPending = true;
            }
        }
    }
    const IommuDomainError quiesced = domain.Quiesce(parker).Error;
    if ((quiesced != IommuDomainError::None) && (quiesced != IommuDomainError::AlreadyQuiesced))
    {
        return false;
    }
    for (Entry& entry : m_entries)
    {
        if (entry.Domain == &domain)
        {
            entry.Domain = nullptr;
            entry.Attachment = IommuAttachmentToken{};
        }
    }
    return true;
}

void PciFunctionControl::Parker::Add(const PciFunction& function, const Foundation::UInt64 sequence) noexcept
{
    if (m_count < Capacity)
    {
        m_functions[m_count] = &function;
        m_sequences[m_count] = sequence;
        ++m_count;
    }
}

bool PciFunctionControl::Parker::Contains(const PciFunction& function) const noexcept
{
    for (Foundation::Size index{0U}; index < m_count; ++index)
    {
        if (m_functions[index] == &function)
        {
            return true;
        }
    }
    return false;
}

bool PciFunctionControl::Parker::CanPark(const Foundation::Size count) noexcept
{
    Foundation::Size free{0U};
    for (const ParkedStorage& parked : m_control.m_parked)
    {
        free += (parked.Pending == 0U) ? 1U : 0U;
    }
    return count <= free;
}

void PciFunctionControl::Parker::Park(const IommuParkedMapping& mapping) noexcept
{
    for (ParkedStorage& parked : m_control.m_parked)
    {
        if (parked.Pending != 0U)
        {
            continue;
        }
        parked.Pin.~ObjectPin();
        ::new (static_cast<void*>(&parked.Pin)) Object::ObjectPin{static_cast<Object::ObjectPin&&>(*mapping.Pin)};
        parked.FrameStorage = mapping.FrameStorage;
        parked.Registry = mapping.Registry;
        parked.FrameObject = mapping.FrameObject;
        parked.PhysicalStart = mapping.PhysicalStart;
        parked.Pages = mapping.Pages;
        parked.Permissions = mapping.PolicyPermissions;
        for (Foundation::Size index{0U}; index < Capacity; ++index)
        {
            parked.Functions[index] = (index < m_count) ? m_functions[index] : nullptr;
            parked.Sequences[index] = (index < m_count) ? m_sequences[index] : 0U;
        }
        parked.Pending = m_count;
        ++m_control.m_pinsParked;
        return;
    }
}

Foundation::Size PciFunctionControl::ParkedCountLocked() const noexcept
{
    Foundation::Size parked{0U};
    for (const ParkedStorage& storage : m_parked)
    {
        parked += (storage.Pending != 0U) ? 1U : 0U;
    }
    return parked;
}

void PciFunctionControl::ReleaseParkedLocked(const PciFunction& function, const Foundation::UInt64 sequence) noexcept
{
    for (ParkedStorage& parked : m_parked)
    {
        if (parked.Pending == 0U)
        {
            continue;
        }
        for (Foundation::Size index{0U}; index < Capacity; ++index)
        {
            if ((parked.Functions[index] == &function) && (parked.Sequences[index] == sequence))
            {
                parked.Functions[index] = nullptr;
                parked.Sequences[index] = 0U;
                --parked.Pending;
            }
        }
        if (parked.Pending != 0U)
        {
            continue;
        }
        if (m_observer != nullptr)
        {
            m_observer->Released(parked.FrameObject, parked.PhysicalStart, parked.Pages);
        }
        IommuDomain::ReleaseParkedMapping(parked.Pin, *parked.FrameStorage, *parked.Registry, parked.FrameObject,
                                          parked.PhysicalStart, parked.Pages, parked.Permissions);
        parked.FrameStorage = nullptr;
        parked.Registry = nullptr;
        parked.FrameObject = Object::ObjectId{};
        parked.PhysicalStart = Memory::PhysicalAddress{};
        parked.Pages = Memory::PageCount{};
        parked.Permissions = Memory::PagePermissions{};
        ++m_pinsReleased;
    }
}

PciFunctionControlError PciFunctionControl::DisableCharged(const Resource::IResourceDomainSubtree& subtree) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    for (Entry& entry : m_entries)
    {
        if ((entry.Function == nullptr) || !IsChargedTo(entry, subtree))
        {
            continue;
        }
        if (!SetBusMasterLocked(*entry.Function, false))
        {
            return PciFunctionControlError::ConfigurationRejected;
        }
        entry.Revoked = true;
    }
    return PciFunctionControlError::None;
}

PciFunctionControlError PciFunctionControl::ReleaseCharged(const Resource::IResourceDomainSubtree& subtree) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    for (Entry& entry : m_entries)
    {
        if ((entry.Function == nullptr) || !IsChargedTo(entry, subtree))
        {
            continue;
        }
        const PciFunctionControlError released = ReleaseLocked(entry, true);
        if (released != PciFunctionControlError::None)
        {
            return released;
        }
        ++m_quarantines;
    }
    return PciFunctionControlError::None;
}

void PciFunctionControl::SetParkedPinObserver(IParkedPinObserver* const observer) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    m_observer = observer;
}

PciFunctionControlError PciFunctionControl::ConfirmQuiesced(PciFunction& function,
                                                            const Foundation::UInt64 sequence) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (function.LiftQuarantine(sequence).Error != PciFunctionError::None)
    {
        return PciFunctionControlError::QuarantineMismatch;
    }
    ++m_confirmations;
    ReleaseParkedLocked(function, sequence);
    return PciFunctionControlError::None;
}

PciFunctionControlSnapshot PciFunctionControl::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    Foundation::Size assigned{0U};
    for (const Entry& entry : m_entries)
    {
        assigned += (entry.Function != nullptr) ? 1U : 0U;
    }
    return PciFunctionControlSnapshot{
        .Assigned = assigned,
        .Assignments = m_assignments,
        .Releases = m_releases,
        .BusMasterEnables = m_enables,
        .BusMasterRefusals = m_refusals,
        .BusMasterRevocations = m_revocations,
        .Quarantines = m_quarantines,
        .Confirmations = m_confirmations,
        .ParkedPins = ParkedCountLocked(),
        .PinsParked = m_pinsParked,
        .PinsReleased = m_pinsReleased,
    };
}

// The assignment pins the driver, so its binding is stable here.
bool PciFunctionControl::IsChargedTo(const Entry& entry, const Resource::IResourceDomainSubtree& subtree) noexcept
{
    const auto binding = entry.Driver->GetBinding();
    return binding.HasValue() && subtree.Contains(binding.GetValue().ResourceDomainStorage);
}

PciFunctionControl::Entry* PciFunctionControl::FindLocked(const PciFunction& function) noexcept
{
    for (Entry& entry : m_entries)
    {
        if (entry.Function == &function)
        {
            return &entry;
        }
    }
    return nullptr;
}

// The Status half of the Command doubleword is written as zeros: its bits
// clear when written as ones.
bool PciFunctionControl::SetBusMasterLocked(const PciFunction& function, const bool enabled) noexcept
{
    return SetPciBusMastering(*m_reader, *m_writer, function.GetSnapshot().Descriptor.Address, enabled);
}

// The Command register's low half only: Status, above it, clears what a one
// Is written to.
bool PciFunctionControl::SetInterruptDisableLocked(const PciFunction& function, const bool disabled) noexcept
{
    const PciFunctionAddress address = function.GetSnapshot().Descriptor.Address;
    Foundation::UInt32 command{0U};
    if (m_reader->Read32(address, CommandRegister, command) != Hal::Device::PciConfigurationReadError::None)
    {
        return false;
    }
    command &= 0xFFFFU;
    command = disabled ? (command | InterruptDisable) : (command & ~InterruptDisable);
    return m_writer->Write32(address, CommandRegister, command) == Hal::Device::PciConfigurationWriteError::None;
}

bool SetPciBusMastering(Hal::Device::IPciConfigurationSpace& reader, Hal::Device::IPciConfigurationWriter& writer,
                        const PciFunctionAddress address, const bool enabled) noexcept
{
    Foundation::UInt32 command{0U};
    if (reader.Read32(address, PciFunctionControl::CommandRegister, command) !=
        Hal::Device::PciConfigurationReadError::None)
    {
        return false;
    }
    command &= 0xFFFFU;
    command =
        enabled ? (command | PciFunctionControl::BusMasterEnable) : (command & ~PciFunctionControl::BusMasterEnable);
    return writer.Write32(address, PciFunctionControl::CommandRegister, command) ==
           Hal::Device::PciConfigurationWriteError::None;
}

// A function still attached to a live IommuDomain is detached first: the
// function refuses to give up its assignment while a domain claims it.
PciFunctionControlError PciFunctionControl::ReleaseLocked(Entry& entry, const bool quarantine) noexcept
{
    if (entry.MessagesEnabled || entry.LineEnabled)
    {
        return PciFunctionControlError::MessagesInUse;
    }
    if (!SetBusMasterLocked(*entry.Function, false))
    {
        return PciFunctionControlError::ConfigurationRejected;
    }
    if (entry.Domain != nullptr)
    {
        if (entry.Domain->Detach(entry.Attachment).Error != IommuDomainError::None)
        {
            return PciFunctionControlError::IommuRejected;
        }
        entry.Domain = nullptr;
        entry.Attachment = IommuAttachmentToken{};
    }
    if (entry.Function->ReleaseAssignment(entry.Assignment, quarantine || entry.DmaPending).Error !=
        PciFunctionError::None)
    {
        return PciFunctionControlError::AssignmentRejected;
    }
    if (entry.DmaPending && !quarantine)
    {
        ++m_quarantines;
    }
    entry = Entry{};
    ++m_releases;
    return PciFunctionControlError::None;
}
} // Namespace Asteria::Kernel::Device
