// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_INTERRUPT_IA64_LOCAL_SAPIC_IPI_CONTROLLER_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_INTERRUPT_IA64_LOCAL_SAPIC_IPI_CONTROLLER_HPP_INCLUDED

#include "Boot/Acpi/AcpiTopology.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Scheduler/SchedulerTypes.hpp"

namespace Asteria::Platform::Ia64::Interrupt
{
enum class Ia64ProcessorInterruptBlockBaseKind : Foundation::UInt8
{
    BootstrapPhysical,
    PinnedUncachedVirtual,
};

struct Ia64ProcessorInterruptBlockAccess
{
    Foundation::UInt64 BaseAddress;
    Ia64ProcessorInterruptBlockBaseKind Kind;
    Foundation::UInt8 Reserved[7];
};

enum class Ia64LocalSapicIpiControllerState : Foundation::UInt8
{
    Uninitialized,
    Ready,
};

enum class Ia64LocalSapicIpiControllerInitializationError : Foundation::UInt8
{
    None,
    AlreadyInitialized,
    InvalidTopology,
    InvalidProcessorCount,
    InvalidProcessorFlags,
    UnsupportedProcessorController,
    InvalidProcessorIdentity,
    DuplicateProcessorIdentity,
    NoEnabledProcessor,
    BspNotEnabled,
    InvalidPhysicalBase,
    InvalidAccessBase,
    InvalidBaseKind,
    AccessBaseMismatch,
    InvalidInterruptAddress,
};

enum class Ia64LocalSapicIpiSendError : Foundation::UInt8
{
    None,
    NotReady,
    InvalidVector,
    InvalidTargetMask,
};

struct Ia64LocalSapicLogicalProcessor
{
    Foundation::UInt64 InterruptAddress;
    Foundation::UInt32 AcpiUid;
    Foundation::UInt8 HardwareId;
    Foundation::UInt8 HardwareEid;
    Foundation::UInt8 Reserved[2];
};

struct Ia64LocalSapicIpiControllerSnapshot
{
    Ia64LocalSapicLogicalProcessor Processors[Kernel::Scheduler::MaximumProcessorCount];
    Foundation::UInt64 PhysicalBaseAddress;
    Foundation::UInt64 AccessBaseAddress;
    Foundation::UInt8 ProcessorCount;
    Foundation::UInt8 EnabledProcessorMask;
    Ia64ProcessorInterruptBlockBaseKind BaseKind;
    Ia64LocalSapicIpiControllerState State;
    Foundation::UInt8 Reserved[4];
};

/// Fixed-capacity IA-64 local-SAPIC Processor Interrupt Block IPI sender. Initialization validates
/// the retained ACPI processor records, derives the current BSP identity from the explicitly
/// supplied cr.lid value, and builds one immutable logical CpuId map. CpuId 0 is the BSP; the
/// remaining enabled boot processors follow ACPI firmware order. Online-capable-only processors are
/// validated but not admitted; CPU hotplug is unsupported. No allocation.
/// Initialize and snapshot access require external serialization. After
/// Ready publication, callers may send concurrently if their platform ownership policy permits
/// interleaved Processor Interrupt Block stores.
class Ia64LocalSapicIpiController final
{
public:
    constexpr Ia64LocalSapicIpiController() noexcept = default;

    /// Binds the retained physical PIB and a currently usable access base.
    /// Preconditions not representable by values:
    /// - BootstrapPhysical is used only in IA-64 physical-address mode. Its
    /// access base must be the bit-63-set UC alias of the retained,
    /// bit-63-clear structural physical base.
    /// - PinnedUncachedVirtual names a caller-proven, boot-lifetime mapping of
    /// exactly topology.LocalControllerPhysicalAddress and its 2 MiB PIB.
    /// This controller neither creates nor verifies that translation.
    /// - currentBspCrLid was read on the non-migrating current BSP.
    [[nodiscard]] Ia64LocalSapicIpiControllerInitializationError
    Initialize(const Boot::Acpi::AcpiTopology& topology, Foundation::UInt64 currentBspCrLid,
               const Ia64ProcessorInterruptBlockAccess& access) noexcept;

    /// Sends one fixed INT-delivery write to each selected logical processor.
    /// Every argument is validated before the first write. Targets are visited
    /// in ascending logical CpuId order, so a rejected operation is inert and
    /// an accepted operation performs at most four stores.
    /// Preconditions not representable by values:
    /// - vector has a complete external-interrupt entry on every target.
    /// - Any ordinary-memory mailbox publication precedes this call.
    /// - The retained access mapping remains valid and uncached.
    [[nodiscard]] Ia64LocalSapicIpiSendError
    SendExternalInterrupt(Foundation::UInt64 vector, Foundation::UInt8 targetProcessorMask) const noexcept;

    [[nodiscard]] constexpr Ia64LocalSapicIpiControllerSnapshot GetSnapshot() const noexcept
    {
        return Ia64LocalSapicIpiControllerSnapshot{
            .Processors =
                {
                    m_processors[0U],
                    m_processors[1U],
                    m_processors[2U],
                    m_processors[3U],
                },
            .PhysicalBaseAddress = m_physicalBaseAddress,
            .AccessBaseAddress = m_accessBaseAddress,
            .ProcessorCount = m_processorCount,
            .EnabledProcessorMask = m_enabledProcessorMask,
            .BaseKind = m_baseKind,
            .State = m_state,
            .Reserved = {0U, 0U, 0U, 0U},
        };
    }

    ~Ia64LocalSapicIpiController() noexcept = default;

    Ia64LocalSapicIpiController(const Ia64LocalSapicIpiController&) = delete;
    Ia64LocalSapicIpiController& operator=(const Ia64LocalSapicIpiController&) = delete;
    Ia64LocalSapicIpiController(Ia64LocalSapicIpiController&&) = delete;
    Ia64LocalSapicIpiController& operator=(Ia64LocalSapicIpiController&&) = delete;

private:
    Ia64LocalSapicLogicalProcessor m_processors[Kernel::Scheduler::MaximumProcessorCount]{};
    Foundation::UInt64 m_physicalBaseAddress{0U};
    Foundation::UInt64 m_accessBaseAddress{0U};
    Foundation::UInt8 m_processorCount{0U};
    Foundation::UInt8 m_enabledProcessorMask{0U};
    Ia64ProcessorInterruptBlockBaseKind m_baseKind{Ia64ProcessorInterruptBlockBaseKind::BootstrapPhysical};
    Ia64LocalSapicIpiControllerState m_state{Ia64LocalSapicIpiControllerState::Uninitialized};
    [[maybe_unused]] Foundation::UInt8 m_reserved[4]{};
};

static_assert(sizeof(Ia64ProcessorInterruptBlockBaseKind) == 1U);
static_assert(sizeof(Ia64ProcessorInterruptBlockAccess) == 16U);
static_assert(alignof(Ia64ProcessorInterruptBlockAccess) == 8U);
static_assert(sizeof(Ia64LocalSapicIpiControllerState) == 1U);
static_assert(sizeof(Ia64LocalSapicIpiControllerInitializationError) == 1U);
static_assert(sizeof(Ia64LocalSapicIpiSendError) == 1U);
static_assert(sizeof(Ia64LocalSapicLogicalProcessor) == 16U);
static_assert(alignof(Ia64LocalSapicLogicalProcessor) == 8U);
static_assert(sizeof(Ia64LocalSapicIpiControllerSnapshot) == 88U);
static_assert(alignof(Ia64LocalSapicIpiControllerSnapshot) == 8U);
static_assert(sizeof(Ia64LocalSapicIpiController) == 88U);
static_assert(alignof(Ia64LocalSapicIpiController) == 8U);
} // Namespace Asteria::Platform::Ia64::Interrupt

#endif // ASTERIA_PLATFORM_IA64_INTERRUPT_IA64_LOCAL_SAPIC_IPI_CONTROLLER_HPP_INCLUDED
