// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_DEVICE_PCI_FUNCTION_HPP_INCLUDED
#define ASTERIA_KERNEL_DEVICE_PCI_FUNCTION_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Hal/Device/IIommuDevice.hpp"
#include "Kernel/Device/PciTypes.hpp"
#include "Kernel/Object/IKernelObject.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Kernel/Protection/ProtectionDomain.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Object
{
class ObjectRegistry;
}

namespace Asteria::Kernel::Device
{
class DeviceManager;
class PciFunction;

/// Identifies one PciFunction generation within one ObjectRegistry.
/// The originating registry outlives every copy and use of this value.
class PciFunctionId final
{
public:
    constexpr PciFunctionId() noexcept = default;

    constexpr PciFunctionId(const Object::ObjectRegistry& registry, const Object::ObjectId object) noexcept
        : m_registry{&registry}, m_object{object}
    {
    }

    [[nodiscard]] constexpr Foundation::UInt64 GetValue() const noexcept
    {
        return m_object.GetValue();
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return (m_registry != nullptr) && m_object.IsValid();
    }

    [[nodiscard]] constexpr Object::ObjectId GetObjectId() const noexcept
    {
        return m_object;
    }

    [[nodiscard]] constexpr bool IsFromRegistry(const Object::ObjectRegistry& registry) const noexcept
    {
        return m_registry == &registry;
    }

    [[nodiscard]] constexpr bool IsEqualTo(const PciFunctionId other) const noexcept
    {
        return (m_registry == other.m_registry) && (m_object.GetValue() == other.m_object.GetValue());
    }

private:
    const Object::ObjectRegistry* m_registry{nullptr};
    Object::ObjectId m_object{};
};

/// Names one exact driver assignment. The originating PciFunction and its
/// storage outlive every copy and use of this kernel-only token.
class PciFunctionAssignmentToken final
{
public:
    constexpr PciFunctionAssignmentToken() noexcept = default;

    [[nodiscard]] constexpr Foundation::UInt64 GetSequence() const noexcept
    {
        return m_sequence;
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return (m_function != nullptr) && (m_sequence != 0U);
    }

    [[nodiscard]] constexpr bool IsFromFunction(const PciFunction& function) const noexcept
    {
        return m_function == &function;
    }

private:
    friend class PciFunction;

    constexpr PciFunctionAssignmentToken(const PciFunction& function, const Foundation::UInt64 sequence) noexcept
        : m_function{&function}, m_sequence{sequence}
    {
    }

    const PciFunction* m_function{nullptr};
    Foundation::UInt64 m_sequence{0U};
};

class PciFunctionAssignmentSequencePolicy final
{
public:
    static constexpr Foundation::UInt64 Maximum{~Foundation::UInt64{0U}};

    PciFunctionAssignmentSequencePolicy() = delete;

    [[nodiscard]] static constexpr bool TryAdvance(const Foundation::UInt64 current, Foundation::UInt64& next) noexcept
    {
        if (current == Maximum)
        {
            return false;
        }
        next = current + 1U;
        return true;
    }
};

enum class PciFunctionState : Foundation::UInt8
{
    Uninitialized,
    Available,
    Assigned,
    Quiesced,
    Retiring,
};

enum class PciFunctionError : Foundation::UInt8
{
    None,
    InvalidConfiguration,
    InvalidDescriptor,
    AlreadyInitialized,
    ResourceDomainRegistryMismatch,
    ResourceDomainPinRejected,
    ResourceDomainInstanceMismatch,
    ResourceDomainRejected,
    NotReady,
    Quiesced,
    InvalidDriver,
    DriverRegistryMismatch,
    DriverPinRejected,
    DriverInstanceMismatch,
    DriverStateRejected,
    AlreadyAssigned,
    AssignmentSequenceExhausted,
    InvalidAssignment,
    StaleAssignment,
    IommuStillClaimed,
    AssignmentBusy,
    CorruptState,
    RetirementPending,
    ManagerOwned,
    /// A drain took the function from its driver, and its Device Manager has not confirmed the
    /// device quiet since.
    Quarantined,
    NotQuarantined,
    DisplayReserved,
    BridgeReserved,
    ReservedMemory,
};

struct PciFunctionFailure
{
    PciFunctionError Error;
    Object::ObjectPinError Pin;
    Resource::ResourceDomainError Resource;
    Protection::ProtectionDomainError Driver;
    Foundation::UInt8 Reserved[4];
};

struct PciFunctionAssignmentReceipt
{
    PciFunctionAssignmentToken Assignment;
    Protection::ProtectionDomainId Driver;
};

struct PciFunctionSnapshot
{
    PciFunctionDescriptor Descriptor;
    Protection::ProtectionDomainId Driver;
    Foundation::UInt64 LastAssignmentSequence;
    PciFunctionState State;
    bool IommuClaimed;
    /// The last assignment ended in quarantine, not yet lifted.
    bool Quarantined;
    Foundation::UInt8 Reserved[5];
};

/// Owns one discovered PCI function authority and its exact DMA endpoint. Assign pins one
/// already-authorized ProtectionDomain generation. Only an assigned function can be claimed by an
/// IOMMU domain, and the IOMMU claim must be released before the driver assignment. The trusted
/// device owner keeps bus mastering disabled while changing either relationship and drains device
/// DMA before IOMMU detach. This object changes no PCI register. Operations may spin on this
/// object, ObjectRegistry, ProtectionDomain, and ResourceDomain and are forbidden from ISRs and
/// same-CPU reentry. Each collaborator lock is nested below this object's lock; no collaborator may
/// call back into this object.
class PciFunction final : public Object::IKernelObject, public Hal::Device::IIommuDevice
{
public:
    constexpr PciFunction() noexcept = default;

    /// Boot-only, before any assignment: permanently reserve firmware DMA.
    [[nodiscard]] bool ReserveFirmwareDma() noexcept;
    /// Boot-only, before any assignment or IOMMU claim: the function may DMA without isolation
    /// under a BOOT.CFG trust entry, attached to an identity IommuDomain. False for a bridge, a
    /// display or a function with firmware DMA reservations, which are never assignable.
    [[nodiscard]] bool GrantTrustedDma() noexcept;
    /// None, BridgeReserved, DisplayReserved or ReservedMemory; allocation-free, normal context.
    [[nodiscard]] PciFunctionError GetAssignmentRestriction() const noexcept;

    /// Preconditions:
    /// - State is Uninitialized or Quiesced;
    /// - no assignment, IOMMU claim, object pin, or concurrent call remains;
    /// - registry and every retained object outlive this storage.
    ~PciFunction() noexcept = default;

    PciFunction(const PciFunction&) = delete;
    PciFunction& operator=(const PciFunction&) = delete;
    PciFunction(PciFunction&&) = delete;
    PciFunction& operator=(PciFunction&&) = delete;

    [[nodiscard]] static bool DescriptorIsValid(const PciFunctionDescriptor& descriptor) noexcept;

    [[nodiscard]] PciFunctionFailure Initialize(Object::ObjectRegistry& registry,
                                                Resource::ResourceDomainObjectId resourceDomainIdentity,
                                                Resource::ResourceDomain& resourceDomain,
                                                const PciFunctionDescriptor& descriptor) noexcept;

    /// Pins the exact driver generation and retains its Ready assignment use.
    /// The caller has already authorized Manage on this function. The supplied
    /// storage must be the exact instance named by driver.
    [[nodiscard]] Foundation::Result<PciFunctionAssignmentReceipt, PciFunctionFailure>
    Assign(Protection::ProtectionDomainId driver, Protection::ProtectionDomain& driverStorage) noexcept;

    /// Releases the exact assignment after device access is revoked, bus
    /// mastering is disabled, DMA is drained, and the IOMMU is detached. With
    /// quarantine (a drain took it from a dead driver), the function then
    /// refuses every assignment until LiftQuarantine names this assignment's
    /// sequence: a transfer the dead driver started may still be running.
    [[nodiscard]] PciFunctionFailure ReleaseAssignment(PciFunctionAssignmentToken assignment,
                                                       bool quarantine = false) noexcept;

    /// Lifts the quarantine of the assignment of sequence, once its Device
    /// Manager confirmed the device quiet; a stale sequence lifts nothing.
    [[nodiscard]] PciFunctionFailure LiftQuarantine(Foundation::UInt64 sequence) noexcept;

    /// Releases metadata only when no driver or IOMMU relationship remains.
    [[nodiscard]] PciFunctionFailure Quiesce() noexcept;

    [[nodiscard]] bool IsInitialized() const noexcept;
    [[nodiscard]] PciFunctionSnapshot GetSnapshot() const noexcept;

    [[nodiscard]] Hal::Device::IommuDeviceError Claim(const IommuDomain& owner) noexcept override;
    [[nodiscard]] Hal::Device::IommuDeviceError Release(const IommuDomain& owner) noexcept override;
    [[nodiscard]] Foundation::UInt32 GetRequesterId() const noexcept override;
    [[nodiscard]] bool HasTrustedDma() const noexcept override;

    [[nodiscard]] Object::ObjectType GetObjectType() const noexcept override;
    [[nodiscard]] Object::ResourceDomainId GetChargeOwner() const noexcept override;

private:
    friend class DeviceManager;

    [[nodiscard]] static PciFunctionFailure
    MakeFailure(PciFunctionError error, Object::ObjectPinError pin = Object::ObjectPinError::Ok,
                Resource::ResourceDomainError resource = Resource::ResourceDomainError::None,
                Protection::ProtectionDomainError driver = Protection::ProtectionDomainError::None) noexcept;
    [[nodiscard]] static constexpr Resource::ResourceReservation GetResourceReservation() noexcept;
    [[nodiscard]] PciFunctionFailure InitializeForDeviceManager(Object::ObjectRegistry& registry,
                                                                Resource::ResourceDomainObjectId resourceDomainIdentity,
                                                                Resource::ResourceDomain& resourceDomain,
                                                                const PciFunctionDescriptor& descriptor) noexcept;
    [[nodiscard]] PciFunctionFailure InitializeInternal(Object::ObjectRegistry& registry,
                                                        Resource::ResourceDomainObjectId resourceDomainIdentity,
                                                        Resource::ResourceDomain& resourceDomain,
                                                        const PciFunctionDescriptor& descriptor,
                                                        bool deviceManagerOwned) noexcept;
    [[nodiscard]] PciFunctionFailure QuiesceAvailableForDeviceManager() noexcept;
    [[nodiscard]] PciFunctionFailure QuiesceRetiringForDeviceManager() noexcept;
    [[nodiscard]] PciFunctionFailure QuiesceLocked(PciFunctionState expectedState) noexcept;
    [[nodiscard]] PciFunctionError BeginDeviceManagerRetirement() noexcept;
    [[nodiscard]] PciFunctionError CancelDeviceManagerRetirement() noexcept;
    [[nodiscard]] PciFunctionError ResetForDeviceManagerReuse() noexcept;

    mutable Platform::Synchronization::SpinLock m_synchronization;
    Platform::Synchronization::AtomicUInt64 m_chargeOwnerValue;
    Object::ObjectRegistry* m_registry{nullptr};
    Object::ObjectPin m_resourceDomainPin;
    Object::ObjectPin m_driverPin;
    Resource::ResourceDomain* m_resourceDomain{nullptr};
    Protection::ProtectionDomain* m_driverStorage{nullptr};
    const IommuDomain* m_iommuOwner{nullptr};
    PciFunctionDescriptor m_descriptor{};
    Resource::ResourceDomainObjectId m_resourceDomainIdentity{};
    Object::ResourceDomainId m_resourceDomainIdentifier{};
    Protection::ProtectionDomainId m_driver{};
    Foundation::UInt64 m_lastAssignmentSequence{0U};
    bool m_quarantined{false};
    bool m_firmwareDma{false};
    bool m_trustedDma{false};
    bool m_deviceManagerOwned{false};
    PciFunctionState m_state{PciFunctionState::Uninitialized};
};

static_assert(sizeof(PciFunctionId) == 16U);
static_assert(alignof(PciFunctionId) == 8U);
static_assert(sizeof(PciFunctionAssignmentToken) == 16U);
static_assert(alignof(PciFunctionAssignmentToken) == 8U);
static_assert(sizeof(PciFunctionState) == 1U);
static_assert(sizeof(PciFunctionError) == 1U);
static_assert(sizeof(PciFunctionFailure) == 8U);
static_assert(alignof(PciFunctionFailure) == 1U);
static_assert(sizeof(PciFunctionAssignmentReceipt) == 32U);
static_assert(alignof(PciFunctionAssignmentReceipt) == 8U);
static_assert(sizeof(PciFunctionSnapshot) == 48U);
static_assert(alignof(PciFunctionSnapshot) == 8U);
} // Namespace Asteria::Kernel::Device

#endif // ASTERIA_KERNEL_DEVICE_PCI_FUNCTION_HPP_INCLUDED
