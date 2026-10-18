// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_DEVICE_PCI_FUNCTION_CONTROL_HPP_INCLUDED
#define ASTERIA_KERNEL_DEVICE_PCI_FUNCTION_CONTROL_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Hal/Device/IPciConfigurationSpace.hpp"
#include "Hal/Device/IPciConfigurationWriter.hpp"
#include "Kernel/Device/PciFunction.hpp"
#include "Kernel/Protection/ProtectionDomain.hpp"
#include "Kernel/Device/IommuDomainPool.hpp"
#include "Kernel/Device/PciMessageSignaled.hpp"
#include "Kernel/Resource/ResourceDomainSubtree.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Device
{
enum class PciFunctionControlError : Foundation::UInt8
{
    None,
    NotConfigured,
    CapacityExceeded,
    /// The function has no assignment, or one this control did not make.
    NotAssigned,
    /// The caller's ProtectionDomain is not the function's assignee.
    NotAssignee,
    /// The Device Manager revoked bus mastering until a reassignment.
    Revoked,
    /// Bus mastering needs a Ready IommuDomain attached to the function.
    IommuRequired,
    AssignmentRejected,
    ConfigurationRejected,
    /// The function is attached to an IommuDomain already, or not to this one.
    AttachmentConflict,
    /// The IommuDomain refused the attachment or detachment; Iommu says why.
    IommuRejected,
    /// The function has no MSI capability.
    MessagesUnavailable,
    /// The function's MSI already serves an Interrupt, which must go first.
    MessagesInUse,
    /// A drain took the function from a dead driver; its Device Manager has
    /// not confirmed the device quiet yet.
    Quarantined,
    /// A confirmation for a function not quarantined, or for another
    /// assignment than the one the drain ended.
    QuarantineMismatch,
    DisplayReserved,
    BridgeReserved,
    ReservedMemory,
    /// A configuration access the USB legacy-support rule does not permit:
    /// another offset, a write that would claim firmware ownership or enable
    /// an SMI, or a function that is not a USB 1.1/2.0 host controller.
    AccessDenied,
    /// DMA storage still needs this requester for device-quiet confirmation.
    DmaPending,
};

/// Told of each parked Frame pin a confirmation releases (Source/User/System/AhciPort.hpp
/// evidence): the Frame's registry identity, first page and pages, as the pin drops. Called with
/// the control's rank-2 lock held: it takes no lock.
class IParkedPinObserver
{
public:
    virtual void Released(Object::ObjectId frame, Memory::PhysicalAddress physicalStart,
                          Memory::PageCount pages) noexcept = 0;

protected:
    constexpr IParkedPinObserver() noexcept = default;
    ~IParkedPinObserver() noexcept = default;
};

/// Turns address's bus mastering on or off: a read-modify-write of its
/// Command register that writes the Status half as zero, so it clears no
/// status bit. False when either access fails.
[[nodiscard]] bool SetPciBusMastering(Hal::Device::IPciConfigurationSpace& reader,
                                      Hal::Device::IPciConfigurationWriter& writer, PciFunctionAddress address,
                                      bool enabled) noexcept;

struct PciFunctionControlSnapshot
{
    Foundation::Size Assigned;
    Foundation::UInt64 Assignments;
    Foundation::UInt64 Releases;
    Foundation::UInt64 BusMasterEnables;
    Foundation::UInt64 BusMasterRefusals;
    Foundation::UInt64 BusMasterRevocations;
    /// Functions a drain released into quarantine, and quarantines their
    /// Device Manager lifted.
    Foundation::UInt64 Quarantines;
    Foundation::UInt64 Confirmations;
    /// Frame pins drained IommuDomains parked with quarantined functions: held now, parked in all,
    /// and released by confirmations.
    Foundation::Size ParkedPins;
    Foundation::UInt64 PinsParked;
    Foundation::UInt64 PinsReleased;
};

/// Who may make an assigned PCI function a bus master, and the kernel's writes of its Command
/// register. The Device Manager (Manage) assigns a function to a driver's ProtectionDomain with bus
/// mastering off, and may revoke it until the next assignment; the assignee (Invoke) may enable it
/// only behind an attached IommuDomain. Drivers never write configuration space. The assignee
/// attaches its function to an IommuDomain with bus mastering held off across the attachment; a
/// domain's destruction and the drain quiesce it through this control, which first stops the DMA of
/// every function attached to it. Every IommuDomain an entry names is live in its pool: its
/// destruction comes here first. A drain cannot prove a dead driver's DMA ended: a drained
/// IommuDomain's Frame pins and their DMA mapping policies are parked here, keyed by the functions
/// the drain quarantines and their drained assignments, and released only once the Device Manager
/// confirmed each of those functions quiet. The table holds every domain's mappings at once; a
/// domain whose mappings still did not fit would not be quiesced, so its drain would stay Busy and
/// the storage pinned. One lock (rank 2) covers the table; it calls IommuDomain (rank 3),
/// PciFunction (rank 4), the configuration accessor (rank 3) and, releasing parked pins, Frame and
/// the shared object lock, none of which calls back.
class PciFunctionControl final : public IIommuDomainQuiescer
{
public:
    /// The functions assigned at once: four xHCI controllers and the USB host group's four
    /// functions.
    static constexpr Foundation::Size Capacity{8U};
    /// The pins parked at once: every mapping of every IommuDomain the pool holds, so no drain
    /// stays Busy for want of room.
    static constexpr Foundation::Size ParkedCapacity{IommuDomainPool::Capacity * IommuDomain::MaximumMappings};
    static constexpr Foundation::UInt16 CommandRegister{0x04U};
    static constexpr Foundation::UInt32 BusMasterEnable{1U << 2U};
    static constexpr Foundation::UInt32 InterruptDisable{1U << 10U};
    static constexpr Foundation::UInt16 InterruptRegister{0x3CU};

    constexpr PciFunctionControl() noexcept = default;

    PciFunctionControl(const PciFunctionControl&) = delete;
    PciFunctionControl& operator=(const PciFunctionControl&) = delete;

    /// Once only; until then every operation is NotConfigured.
    [[nodiscard]] bool Configure(Hal::Device::IPciConfigurationSpace& reader,
                                 Hal::Device::IPciConfigurationWriter& writer) noexcept;

    /// The platform's observer of released parked pins, or null for none.
    void SetParkedPinObserver(IParkedPinObserver* observer) noexcept;

    /// Turns bus mastering off and assigns function to driver; a former
    /// assignment of this control is released first. Clears the revocation.
    [[nodiscard]] PciFunctionControlError Assign(PciFunction& function, Protection::ProtectionDomainId driver,
                                                 Protection::ProtectionDomain& driverStorage) noexcept;

    /// Turns bus mastering off and releases this control's assignment.
    [[nodiscard]] PciFunctionControlError Release(const PciFunction& function) noexcept;

    /// The assignee caller makes function a bus master.
    [[nodiscard]] PciFunctionControlError EnableBusMaster(const PciFunction& function,
                                                          Protection::ProtectionDomainId caller) noexcept;

    /// Turns bus mastering off: the assignee for itself, or the Device
    /// Manager (revoke), whose revocation stands until a reassignment.
    [[nodiscard]] PciFunctionControlError DisableBusMaster(const PciFunction& function,
                                                           Protection::ProtectionDomainId caller, bool revoke) noexcept;

    /// The assignee caller attaches function to domain, bus mastering off.
    [[nodiscard]] PciFunctionControlError Attach(PciFunction& function, Protection::ProtectionDomainId caller,
                                                 IommuDomain& domain) noexcept;

    /// The assignee caller detaches function from domain, bus mastering off.
    [[nodiscard]] PciFunctionControlError Detach(const PciFunction& function, Protection::ProtectionDomainId caller,
                                                 const IommuDomain& domain) noexcept;

    /// Turns off the bus mastering of every function attached to domain,
    /// forgets their attachments, and quiesces domain; true once it is
    /// quiesced.
    [[nodiscard]] bool QuiesceDomain(IommuDomain& domain) noexcept override;
    /// In a drain of subtree: the same, the domain's Frame pins parked with
    /// every attached function the drain quarantines (those whose assignee
    /// subtree charges) until ConfirmQuiesced; without such a function they
    /// are released as by QuiesceDomain.
    [[nodiscard]] bool QuiesceDrainedDomain(IommuDomain& domain,
                                            const Resource::IResourceDomainSubtree& subtree) noexcept override;

    /// The assignee caller has its function's MSI send data to address
    ///, INTx disabled; one Interrupt per function.
    [[nodiscard]] PciFunctionControlError EnableMessages(const PciFunction& function,
                                                         Protection::ProtectionDomainId caller,
                                                         Foundation::UInt64 address, Foundation::UInt16 data) noexcept;

    /// Turns the function's MSI off, INTx left disabled, so its Interrupt can
    /// end; the function may then be released.
    [[nodiscard]] PciFunctionControlError DisableMessages(const PciFunction& function) noexcept;

    /// The global system interrupt line the platform routes the function's
    /// INTx to (configuration byte 0x3C), for an assigned function with an
    /// interrupt pin; MessagesUnavailable for one without.
    [[nodiscard]] PciFunctionControlError GetInterruptLine(const PciFunction& function,
                                                           Foundation::UInt32& line) noexcept;

    /// The assignee caller lets its function assert INTx (clears INTx
    /// Disable); one Interrupt per function.
    [[nodiscard]] PciFunctionControlError EnableLine(const PciFunction& function,
                                                     Protection::ProtectionDomainId caller) noexcept;

    /// Disables the function's INTx again, so its Interrupt can end; the
    /// function may then be released.
    [[nodiscard]] PciFunctionControlError DisableLine(const PciFunction& function) noexcept;

    /// The assignee caller's narrow configuration access to its USB 1.1/2.0 host controller (Abi:
    /// DeviceReadConfiguration and Abi::DeviceWriteLegacySupport;
    /// ), at the naturally aligned dword offset. A
    /// read answers the identity (0x00), class (0x08), BAR 0 (0x10, whose low bits place a register
    /// block within its page) or interrupt (0x3C) dword, an EHCI's dwords from 0x40 on, where its
    /// EECP list lives (EHCI 1.0 section 2.1.7), or a UHCI's LEGSUP dword (0xC0, Intel UHCI design
    /// guide 1.1 section 5.2.1). A write only relinquishes firmware legacy support: at an EHCI USB
    /// Legacy Support capability (ID 1, outside the function's standard capability list, where ID 1
    /// is power management) it sets the OS Owned semaphore and nothing else, at the control/status
    /// dword after it it only acknowledges SMI status (bits 31:29) with every SMI enable zero, and
    /// at a UHCI's LEGSUP it only acknowledges status (bits 15, 11:8) and sets PIRQ (bit 13) with
    /// every trap and SMI enable zero. AccessDenied for anything else. Each write re-reads the
    /// register first, so firmware's own bits are never written back stale.
    [[nodiscard]] PciFunctionControlError
    AccessLegacySupport(const PciFunction& function, Protection::ProtectionDomainId caller, Foundation::UInt16 offset,
                        bool write, Foundation::UInt32 value, Foundation::UInt32& result) noexcept;

    /// The Device Manager's read (Manage, checked by the caller) of the same configuration dwords
    /// AccessLegacySupport reads, while no driver holds the function: after a drain released it
    /// into quarantine, so the manager's proof that the device is quiet can see whether firmware
    /// still owns it (an EHCI's USBLEGSUP, a UHCI's LEGSUP) and where its registers lie in their
    /// page. Read-only; NotAssignee while a driver is assigned, AccessDenied outside the dwords
    /// AccessLegacySupport reads.
    [[nodiscard]] PciFunctionControlError ReadReleasedConfiguration(const PciFunction& function,
                                                                    Foundation::UInt16 offset,
                                                                    Foundation::UInt32& result) noexcept;

    /// The drain's first device: every function whose
    /// assignee is charged to a domain of subtree loses bus mastering, which
    /// is revoked until a reassignment.
    [[nodiscard]] PciFunctionControlError DisableCharged(const Resource::IResourceDomainSubtree& subtree) noexcept;

    /// The drain's last device step: those functions are detached from any
    /// surviving IommuDomain and lose their assignment, before their
    /// ProtectionDomain drains, into quarantine: a transfer the dead driver
    /// started may still be running, so no driver gets them again until
    /// ConfirmQuiesced.
    [[nodiscard]] PciFunctionControlError ReleaseCharged(const Resource::IResourceDomainSubtree& subtree) noexcept;

    /// The Device Manager's confirmation (Manage, checked by the caller) that
    /// the device of function is quiet after the drain that ended its
    /// assignment of sequence: lifts that quarantine only, and releases each
    /// pin parked with it that no other quarantined function still holds.
    [[nodiscard]] PciFunctionControlError ConfirmQuiesced(PciFunction& function, Foundation::UInt64 sequence) noexcept;

    [[nodiscard]] PciFunctionControlSnapshot GetSnapshot() const noexcept;

    /// Whether caller is the assignee of a function the boot granted DMA without isolation
    /// (PciFunction::GrantTrustedDma): only such a driver may create an IommuDomain of the
    /// trusted-driver profile's identity backend.
    [[nodiscard]] bool HasTrustedAssignment(Protection::ProtectionDomainId caller) const noexcept;

private:
    /// One parked Frame pin: the pin, what releasing its DMA mapping policy
    /// takes, and the quarantined functions (and drained assignments) still
    /// to be confirmed before it goes.
    struct ParkedStorage
    {
        Object::ObjectPin Pin;
        Memory::Frame* FrameStorage{nullptr};
        Object::ObjectRegistry* Registry{nullptr};
        Object::ObjectId FrameObject{};
        Memory::PhysicalAddress PhysicalStart{};
        Memory::PageCount Pages{};
        Memory::PagePermissions Permissions{};
        const PciFunction* Functions[Capacity]{};
        Foundation::UInt64 Sequences[Capacity]{};
        Foundation::Size Pending{0U};
    };

    /// Parks a drained domain's mappings with the given functions; used with
    /// the control's lock held.
    class Parker final : public IIommuStorageParker
    {
    public:
        explicit Parker(PciFunctionControl& control) noexcept : m_control{control}
        {
        }

        void Add(const PciFunction& function, Foundation::UInt64 sequence) noexcept;
        [[nodiscard]] Foundation::Size GetCount() const noexcept
        {
            return m_count;
        }
        [[nodiscard]] bool Contains(const PciFunction& function) const noexcept;
        [[nodiscard]] bool CanPark(Foundation::Size count) noexcept override;
        void Park(const IommuParkedMapping& mapping) noexcept override;

    private:
        PciFunctionControl& m_control;
        const PciFunction* m_functions[Capacity]{};
        Foundation::UInt64 m_sequences[Capacity]{};
        Foundation::Size m_count{0U};
    };

    struct Entry
    {
        PciFunction* Function;
        PciFunctionAssignmentToken Assignment;
        Protection::ProtectionDomain* Driver;
        IommuDomain* Domain;
        IommuAttachmentToken Attachment;
        PciMessageCapability Messages;
        bool MessagesEnabled;
        /// The function may assert INTx for its Interrupt.
        bool LineEnabled;
        bool Revoked;
        bool DmaPending;
    };

    [[nodiscard]] Entry* FindLocked(const PciFunction& function) noexcept;
    [[nodiscard]] static bool IsChargedTo(const Entry& entry, const Resource::IResourceDomainSubtree& subtree) noexcept;
    [[nodiscard]] bool SetBusMasterLocked(const PciFunction& function, bool enabled) noexcept;
    [[nodiscard]] bool SetInterruptDisableLocked(const PciFunction& function, bool disabled) noexcept;
    [[nodiscard]] PciFunctionControlError ReleaseLocked(Entry& entry, bool quarantine = false) noexcept;
    /// Stops bus mastering on every function attached to domain, then
    /// quiesces it, with parker when one is given; true if both succeeded.
    [[nodiscard]] bool QuiesceLocked(IommuDomain& domain, Parker* parker) noexcept;
    /// Confirms function's drained assignment sequence on every parked pin,
    /// releasing those no other quarantined function holds.
    void ReleaseParkedLocked(const PciFunction& function, Foundation::UInt64 sequence) noexcept;
    [[nodiscard]] Foundation::Size ParkedCountLocked() const noexcept;

    Hal::Device::IPciConfigurationSpace* m_reader{nullptr};
    Hal::Device::IPciConfigurationWriter* m_writer{nullptr};
    mutable Platform::Synchronization::SpinLock m_synchronization;
    Entry m_entries[Capacity]{};
    Foundation::UInt64 m_assignments{0U};
    Foundation::UInt64 m_releases{0U};
    Foundation::UInt64 m_enables{0U};
    Foundation::UInt64 m_refusals{0U};
    Foundation::UInt64 m_revocations{0U};
    Foundation::UInt64 m_quarantines{0U};
    Foundation::UInt64 m_confirmations{0U};
    ParkedStorage m_parked[ParkedCapacity]{};

    static_assert(ParkedCapacity == IommuDomainPool::Capacity * IommuDomain::MaximumMappings);
    static_assert(ParkedCapacity == 256U);
    Foundation::UInt64 m_pinsParked{0U};
    Foundation::UInt64 m_pinsReleased{0U};
    IParkedPinObserver* m_observer{nullptr};
};
} // Namespace Asteria::Kernel::Device

#endif // ASTERIA_KERNEL_DEVICE_PCI_FUNCTION_CONTROL_HPP_INCLUDED
