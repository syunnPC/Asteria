// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Boot/Acpi/I8042Evidence.hpp"
#include "Platform/Ia64/Interrupt/Ia64LocalSapicIpiController.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
struct ProcessorInterruptBlockWrite
{
    Foundation::UInt64 Address;
    Foundation::UInt64 Value;
};

ProcessorInterruptBlockWrite g_writes[Kernel::Scheduler::MaximumProcessorCount]{};
Foundation::Size g_writeCount{0U};
} // namespace Asteria::Test

extern "C" void AsteriaIa64WriteProcessorInterruptBlock(const Asteria::Foundation::UInt64 address,
                                                        const Asteria::Foundation::UInt64 value) noexcept
{
    if (Asteria::Test::g_writeCount < Asteria::Kernel::Scheduler::MaximumProcessorCount)
    {
        Asteria::Test::g_writes[Asteria::Test::g_writeCount] = Asteria::Test::ProcessorInterruptBlockWrite{
            .Address = address,
            .Value = value,
        };
    }
    ++Asteria::Test::g_writeCount;
}

namespace Asteria::Test
{
namespace
{
using Boot::Acpi::AcpiProcessorControllerKind;
using Boot::Acpi::AcpiProcessorTopology;
using Boot::Acpi::AcpiTopology;
using Platform::Ia64::Interrupt::Ia64LocalSapicIpiController;
using Platform::Ia64::Interrupt::Ia64LocalSapicIpiControllerInitializationError;
using Platform::Ia64::Interrupt::Ia64LocalSapicIpiControllerSnapshot;
using Platform::Ia64::Interrupt::Ia64LocalSapicIpiControllerState;
using Platform::Ia64::Interrupt::Ia64LocalSapicIpiSendError;
using Platform::Ia64::Interrupt::Ia64ProcessorInterruptBlockAccess;
using Platform::Ia64::Interrupt::Ia64ProcessorInterruptBlockBaseKind;

constexpr Foundation::UInt32 EnabledFlag{1U};
constexpr Foundation::UInt32 OnlineCapableFlag{1U << 1U};
constexpr Foundation::UInt64 PhysicalAddressUncachedBit{Foundation::UInt64{1U} << 63U};
constexpr Foundation::UInt64 DefaultPibPhysicalBase{0x0000'0000'FEE0'0000ULL};
constexpr Foundation::UInt64 DefaultPibBootstrapAccessBase{DefaultPibPhysicalBase | PhysicalAddressUncachedBit};
constexpr Foundation::UInt64 OneMiBBytes{1024U * 1024U};
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};

void ResetWrites() noexcept
{
    for (ProcessorInterruptBlockWrite& write : g_writes)
    {
        write = {};
    }
    g_writeCount = 0U;
}

[[nodiscard]] constexpr Foundation::UInt64 MakeCrLid(const Foundation::UInt8 hardwareId,
                                                     const Foundation::UInt8 hardwareEid) noexcept
{
    return (static_cast<Foundation::UInt64>(hardwareId) << 24U) | (static_cast<Foundation::UInt64>(hardwareEid) << 16U);
}

[[nodiscard]] constexpr Ia64ProcessorInterruptBlockAccess MakeAccess(
    const Foundation::UInt64 baseAddress = DefaultPibBootstrapAccessBase,
    const Ia64ProcessorInterruptBlockBaseKind kind = Ia64ProcessorInterruptBlockBaseKind::BootstrapPhysical) noexcept
{
    return Ia64ProcessorInterruptBlockAccess{
        .BaseAddress = baseAddress,
        .Kind = kind,
        .Reserved = {0U, 0U, 0U, 0U, 0U, 0U, 0U},
    };
}

[[nodiscard]] constexpr AcpiProcessorTopology
MakeProcessor(const Foundation::UInt32 acpiUid, const Foundation::UInt32 hardwareId,
              const Foundation::UInt32 hardwareEid, const Foundation::UInt32 flags = EnabledFlag,
              const AcpiProcessorControllerKind controllerKind = AcpiProcessorControllerKind::LocalSapic) noexcept
{
    return AcpiProcessorTopology{
        .AcpiUid = acpiUid,
        .HardwareId = hardwareId,
        .HardwareEid = hardwareEid,
        .Flags = flags,
        .ControllerKind = controllerKind,
        .Reserved = 0U,
    };
}

[[nodiscard]] AcpiTopology MakeOneProcessorTopology(const Foundation::UInt32 hardwareId = 1U,
                                                    const Foundation::UInt32 hardwareEid = 2U) noexcept
{
    AcpiTopology topology{};
    topology.LocalControllerPhysicalAddress = DefaultPibPhysicalBase;
    topology.ProcessorCount = 1U;
    topology.Processors[0U] = MakeProcessor(10U, hardwareId, hardwareEid);
    return topology;
}

[[nodiscard]] constexpr Foundation::UInt64 InterruptAddress(const Foundation::UInt64 baseAddress,
                                                            const Foundation::UInt8 hardwareId,
                                                            const Foundation::UInt8 hardwareEid) noexcept
{
    return baseAddress + (static_cast<Foundation::UInt64>(hardwareId) << 12U) +
           (static_cast<Foundation::UInt64>(hardwareEid) << 4U);
}

[[nodiscard]] bool ProcessorIs(const Platform::Ia64::Interrupt::Ia64LocalSapicLogicalProcessor& processor,
                               const Foundation::UInt32 acpiUid, const Foundation::UInt8 hardwareId,
                               const Foundation::UInt8 hardwareEid, const Foundation::UInt64 baseAddress) noexcept
{
    return (processor.AcpiUid == acpiUid) && (processor.HardwareId == hardwareId) &&
           (processor.HardwareEid == hardwareEid) &&
           (processor.InterruptAddress == InterruptAddress(baseAddress, hardwareId, hardwareEid)) &&
           (processor.Reserved[0U] == 0U) && (processor.Reserved[1U] == 0U);
}

[[nodiscard]] bool ProcessorIsZero(const Platform::Ia64::Interrupt::Ia64LocalSapicLogicalProcessor& processor) noexcept
{
    const Foundation::UInt8* const bytes = reinterpret_cast<const Foundation::UInt8*>(&processor);
    for (Foundation::Size index{0U}; index < sizeof(processor); ++index)
    {
        if (bytes[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool IsInitialSnapshot(const Ia64LocalSapicIpiControllerSnapshot& snapshot) noexcept
{
    bool processorsAreZero{true};
    for (Foundation::Size index{0U}; index < Kernel::Scheduler::MaximumProcessorCount; ++index)
    {
        processorsAreZero = processorsAreZero && ProcessorIsZero(snapshot.Processors[index]);
    }
    return processorsAreZero && (snapshot.PhysicalBaseAddress == 0U) && (snapshot.AccessBaseAddress == 0U) &&
           (snapshot.ProcessorCount == 0U) && (snapshot.EnabledProcessorMask == 0U) &&
           (snapshot.BaseKind == Ia64ProcessorInterruptBlockBaseKind::BootstrapPhysical) &&
           (snapshot.State == Ia64LocalSapicIpiControllerState::Uninitialized) && (snapshot.Reserved[0U] == 0U) &&
           (snapshot.Reserved[1U] == 0U) && (snapshot.Reserved[2U] == 0U) && (snapshot.Reserved[3U] == 0U);
}

[[nodiscard]] bool InitializationFailsAtomically(Ia64LocalSapicIpiController& controller, const AcpiTopology& topology,
                                                 const Foundation::UInt64 crLid,
                                                 const Ia64ProcessorInterruptBlockAccess& access,
                                                 const Ia64LocalSapicIpiControllerInitializationError expected) noexcept
{
    return (controller.Initialize(topology, crLid, access) == expected) &&
           IsInitialSnapshot(controller.GetSnapshot()) && (g_writeCount == 0U);
}

void TestOneProcessorAndSendValidation(TestContext& runner) noexcept
{
    ResetWrites();
    Ia64LocalSapicIpiController controller;
    ASTERIA_EXPECT(runner, "an uninitialized controller is inert",
                   controller.SendExternalInterrupt(16U, 1U) == Ia64LocalSapicIpiSendError::NotReady,
                   IsInitialSnapshot(controller.GetSnapshot()), g_writeCount == 0U);

    const AcpiTopology topology = MakeOneProcessorTopology(0x12U, 0x34U);
    const Ia64ProcessorInterruptBlockAccess access = MakeAccess();
    ASTERIA_EXPECT(runner, "one enabled Local SAPIC processor initializes",
                   controller.Initialize(topology, MakeCrLid(0x12U, 0x34U), access) ==
                       Ia64LocalSapicIpiControllerInitializationError::None);

    const Ia64LocalSapicIpiControllerSnapshot snapshot = controller.GetSnapshot();
    ASTERIA_EXPECT(runner, "one-CPU snapshot retains the exact ID/EID delivery address",
                   ProcessorIs(snapshot.Processors[0U], 10U, 0x12U, 0x34U, DefaultPibBootstrapAccessBase),
                   ProcessorIsZero(snapshot.Processors[1U]), ProcessorIsZero(snapshot.Processors[2U]),
                   ProcessorIsZero(snapshot.Processors[3U]), snapshot.PhysicalBaseAddress == DefaultPibPhysicalBase,
                   snapshot.AccessBaseAddress == DefaultPibBootstrapAccessBase, snapshot.ProcessorCount == 1U,
                   snapshot.EnabledProcessorMask == 1U,
                   snapshot.BaseKind == Ia64ProcessorInterruptBlockBaseKind::BootstrapPhysical,
                   snapshot.State == Ia64LocalSapicIpiControllerState::Ready);

    ResetWrites();
    ASTERIA_EXPECT(runner, "the minimum external vector is written exactly",
                   controller.SendExternalInterrupt(16U, 1U) == Ia64LocalSapicIpiSendError::None, g_writeCount == 1U,
                   g_writes[0U].Address == InterruptAddress(DefaultPibBootstrapAccessBase, 0x12U, 0x34U),
                   g_writes[0U].Value == 16U);

    ResetWrites();
    ASTERIA_EXPECT(runner, "the maximum external vector is written exactly",
                   controller.SendExternalInterrupt(255U, 1U) == Ia64LocalSapicIpiSendError::None, g_writeCount == 1U,
                   g_writes[0U].Value == 255U);

    ResetWrites();
    const bool invalidVectors =
        (controller.SendExternalInterrupt(0U, 1U) == Ia64LocalSapicIpiSendError::InvalidVector) &&
        (controller.SendExternalInterrupt(15U, 1U) == Ia64LocalSapicIpiSendError::InvalidVector) &&
        (controller.SendExternalInterrupt(256U, 1U) == Ia64LocalSapicIpiSendError::InvalidVector) &&
        (controller.SendExternalInterrupt(MaximumUInt64, 1U) == Ia64LocalSapicIpiSendError::InvalidVector);
    const bool invalidMasks =
        (controller.SendExternalInterrupt(16U, 0U) == Ia64LocalSapicIpiSendError::InvalidTargetMask) &&
        (controller.SendExternalInterrupt(16U, 0x02U) == Ia64LocalSapicIpiSendError::InvalidTargetMask) &&
        (controller.SendExternalInterrupt(16U, 0x11U) == Ia64LocalSapicIpiSendError::InvalidTargetMask);
    ASTERIA_EXPECT(runner, "invalid vectors and complete masks fail before any write", invalidVectors, invalidMasks,
                   g_writeCount == 0U);

    ASTERIA_EXPECT(runner, "successful initialization is single-use",
                   controller.Initialize(topology, MakeCrLid(0x12U, 0x34U), access) ==
                       Ia64LocalSapicIpiControllerInitializationError::AlreadyInitialized,
                   controller.GetSnapshot().ProcessorCount == 1U);
}

void TestTwoProcessorBspReordering(TestContext& runner) noexcept
{
    AcpiTopology topology{};
    topology.LocalControllerPhysicalAddress = DefaultPibPhysicalBase;
    topology.ProcessorCount = 2U;
    topology.Processors[0U] = MakeProcessor(20U, 0x12U, 0x34U);
    topology.Processors[1U] = MakeProcessor(21U, 0x56U, 0x78U);

    Ia64LocalSapicIpiController controller;
    ASTERIA_EXPECT(runner, "two-CPU topology initializes with a non-first BSP",
                   controller.Initialize(topology, MakeCrLid(0x56U, 0x78U), MakeAccess()) ==
                       Ia64LocalSapicIpiControllerInitializationError::None);

    const Ia64LocalSapicIpiControllerSnapshot snapshot = controller.GetSnapshot();
    ASTERIA_EXPECT(runner, "the BSP is CpuId 0 and the remaining CPU keeps firmware order",
                   snapshot.ProcessorCount == 2U, snapshot.EnabledProcessorMask == 0x03U,
                   ProcessorIs(snapshot.Processors[0U], 21U, 0x56U, 0x78U, DefaultPibBootstrapAccessBase),
                   ProcessorIs(snapshot.Processors[1U], 20U, 0x12U, 0x34U, DefaultPibBootstrapAccessBase));

    ResetWrites();
    ASTERIA_EXPECT(runner, "two targets are written in ascending logical CpuId order",
                   controller.SendExternalInterrupt(0xE8U, 0x03U) == Ia64LocalSapicIpiSendError::None,
                   g_writeCount == 2U,
                   g_writes[0U].Address == InterruptAddress(DefaultPibBootstrapAccessBase, 0x56U, 0x78U),
                   g_writes[1U].Address == InterruptAddress(DefaultPibBootstrapAccessBase, 0x12U, 0x34U),
                   g_writes[0U].Value == 0xE8U, g_writes[1U].Value == 0xE8U);
}

void TestFourProcessorOrderingAndVirtualAccess(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 VirtualPibBase{0xC000'0000'FEE0'0000ULL};
    AcpiTopology topology{};
    topology.LocalControllerPhysicalAddress = DefaultPibPhysicalBase;
    topology.ProcessorCount = 4U;
    topology.Processors[0U] = MakeProcessor(100U, 0x10U, 1U);
    topology.Processors[1U] = MakeProcessor(101U, 0x20U, 2U);
    topology.Processors[2U] = MakeProcessor(102U, 0x30U, 3U);
    topology.Processors[3U] = MakeProcessor(103U, 0x40U, 4U);

    Ia64LocalSapicIpiController controller;
    ASTERIA_EXPECT(
        runner, "a caller-proven pinned uncached access base is retained",
        controller.Initialize(topology, MakeCrLid(0x30U, 3U),
                              MakeAccess(VirtualPibBase, Ia64ProcessorInterruptBlockBaseKind::PinnedUncachedVirtual)) ==
            Ia64LocalSapicIpiControllerInitializationError::None);

    const Ia64LocalSapicIpiControllerSnapshot snapshot = controller.GetSnapshot();
    ASTERIA_EXPECT(runner, "four CPUs have a deterministic BSP-first logical mapping", snapshot.ProcessorCount == 4U,
                   snapshot.EnabledProcessorMask == 0x0FU, snapshot.PhysicalBaseAddress == DefaultPibPhysicalBase,
                   snapshot.AccessBaseAddress == VirtualPibBase,
                   snapshot.BaseKind == Ia64ProcessorInterruptBlockBaseKind::PinnedUncachedVirtual,
                   ProcessorIs(snapshot.Processors[0U], 102U, 0x30U, 3U, VirtualPibBase),
                   ProcessorIs(snapshot.Processors[1U], 100U, 0x10U, 1U, VirtualPibBase),
                   ProcessorIs(snapshot.Processors[2U], 101U, 0x20U, 2U, VirtualPibBase),
                   ProcessorIs(snapshot.Processors[3U], 103U, 0x40U, 4U, VirtualPibBase));

    ResetWrites();
    ASTERIA_EXPECT(runner, "a sparse four-CPU mask preserves logical target ordering",
                   controller.SendExternalInterrupt(0xFEU, 0x0DU) == Ia64LocalSapicIpiSendError::None,
                   g_writeCount == 3U, g_writes[0U].Address == InterruptAddress(VirtualPibBase, 0x30U, 3U),
                   g_writes[1U].Address == InterruptAddress(VirtualPibBase, 0x20U, 2U),
                   g_writes[2U].Address == InterruptAddress(VirtualPibBase, 0x40U, 4U));

    ResetWrites();
    ASTERIA_EXPECT(runner, "a valid prefix plus an invalid target bit cannot write partially",
                   controller.SendExternalInterrupt(15U, 0x0FU) == Ia64LocalSapicIpiSendError::InvalidVector,
                   controller.SendExternalInterrupt(0xFEU, 0x1FU) == Ia64LocalSapicIpiSendError::InvalidTargetMask,
                   g_writeCount == 0U);
}

void TestOnlineCapableProcessorsAreExcluded(TestContext& runner) noexcept
{
    AcpiTopology topology{};
    topology.LocalControllerPhysicalAddress = DefaultPibPhysicalBase;
    topology.ProcessorCount = 4U;
    topology.Processors[0U] = MakeProcessor(200U, 0x70U, 7U, OnlineCapableFlag);
    topology.Processors[1U] = MakeProcessor(201U, 0x21U, 2U);
    topology.Processors[2U] = MakeProcessor(202U, 0x80U, 8U, OnlineCapableFlag);
    topology.Processors[3U] = MakeProcessor(203U, 0x31U, 3U);

    Ia64LocalSapicIpiController controller;
    ASTERIA_EXPECT(runner, "online-capable-only records do not block enabled boot CPUs",
                   controller.Initialize(topology, MakeCrLid(0x31U, 3U), MakeAccess()) ==
                       Ia64LocalSapicIpiControllerInitializationError::None);

    const Ia64LocalSapicIpiControllerSnapshot snapshot = controller.GetSnapshot();
    ASTERIA_EXPECT(runner, "only enabled processors receive logical CpuIds", snapshot.ProcessorCount == 2U,
                   snapshot.EnabledProcessorMask == 0x03U,
                   ProcessorIs(snapshot.Processors[0U], 203U, 0x31U, 3U, DefaultPibBootstrapAccessBase),
                   ProcessorIs(snapshot.Processors[1U], 201U, 0x21U, 2U, DefaultPibBootstrapAccessBase),
                   ProcessorIsZero(snapshot.Processors[2U]), ProcessorIsZero(snapshot.Processors[3U]));

    ResetWrites();
    ASTERIA_EXPECT(runner, "an omitted online-capable processor cannot be targeted",
                   controller.SendExternalInterrupt(0xF0U, 0x04U) == Ia64LocalSapicIpiSendError::InvalidTargetMask,
                   g_writeCount == 0U);
}

void TestProcessorRecordValidation(TestContext& runner) noexcept
{
    ResetWrites();
    Ia64LocalSapicIpiController countController;
    AcpiTopology topology = MakeOneProcessorTopology();
    topology.ProcessorCount = 0U;
    const bool zeroRejected =
        InitializationFailsAtomically(countController, topology, MakeCrLid(1U, 2U), MakeAccess(),
                                      Ia64LocalSapicIpiControllerInitializationError::InvalidProcessorCount);
    topology = MakeOneProcessorTopology();
    topology.ProcessorCount = 5U;
    ASTERIA_EXPECT(
        runner, "zero and over-capacity processor counts are rejected atomically", zeroRejected,
        InitializationFailsAtomically(countController, topology, MakeCrLid(1U, 2U), MakeAccess(),
                                      Ia64LocalSapicIpiControllerInitializationError::InvalidProcessorCount));

    Ia64LocalSapicIpiController topologyController;
    topology = MakeOneProcessorTopology();
    topology.Reserved = 1U;
    const bool topologyReservedRejected =
        InitializationFailsAtomically(topologyController, topology, MakeCrLid(1U, 2U), MakeAccess(),
                                      Ia64LocalSapicIpiControllerInitializationError::InvalidTopology);
    topology = MakeOneProcessorTopology();
    topology.Processors[0U].Reserved = 1U;
    ASTERIA_EXPECT(runner, "invalid evidence flags and processor reserved fields are rejected",
                   topologyReservedRejected,
                   InitializationFailsAtomically(topologyController, topology, MakeCrLid(1U, 2U), MakeAccess(),
                                                 Ia64LocalSapicIpiControllerInitializationError::InvalidTopology));

    topology = MakeOneProcessorTopology();
    topology.Dsdt.PhysicalAddress = 0x10'0000U;
    topology.Dsdt.Length = 100U;
    Boot::Acpi::I8042Evidence::Record(topology, Abi::I8042KeyboardAml, 0x0303U, 7U, topology.Dsdt.PhysicalAddress,
                                      topology.Dsdt.Length);
    Ia64LocalSapicIpiController evidenceController;
    ASTERIA_EXPECT(runner, "BootInfo v14 bound keyboard evidence does not invalidate the local SAPIC topology",
                   evidenceController.Initialize(topology, MakeCrLid(1U, 2U), MakeAccess()) ==
                       Ia64LocalSapicIpiControllerInitializationError::None);

    Ia64LocalSapicIpiController flagsController;
    bool invalidFlagsRejected{true};
    constexpr Foundation::UInt32 InvalidFlags[]{0U, 3U, 4U};
    for (const Foundation::UInt32 flags : InvalidFlags)
    {
        topology = MakeOneProcessorTopology();
        topology.Processors[0U].Flags = flags;
        invalidFlagsRejected =
            invalidFlagsRejected &&
            InitializationFailsAtomically(flagsController, topology, MakeCrLid(1U, 2U), MakeAccess(),
                                          Ia64LocalSapicIpiControllerInitializationError::InvalidProcessorFlags);
    }
    ASTERIA_EXPECT(runner, "disabled, contradictory, and unknown processor flags are rejected", invalidFlagsRejected);

    Ia64LocalSapicIpiController noEnabledController;
    topology = MakeOneProcessorTopology();
    topology.Processors[0U].Flags = OnlineCapableFlag;
    ASTERIA_EXPECT(runner, "an online-capable-only topology has no boot processor",
                   InitializationFailsAtomically(noEnabledController, topology, MakeCrLid(1U, 2U), MakeAccess(),
                                                 Ia64LocalSapicIpiControllerInitializationError::NoEnabledProcessor));

    Ia64LocalSapicIpiController mixedController;
    topology = MakeOneProcessorTopology();
    topology.ProcessorCount = 2U;
    topology.Processors[1U] = MakeProcessor(11U, 3U, 4U, EnabledFlag, AcpiProcessorControllerKind::LocalApic);
    ASTERIA_EXPECT(
        runner, "a mixed local-controller model is rejected atomically",
        InitializationFailsAtomically(mixedController, topology, MakeCrLid(1U, 2U), MakeAccess(),
                                      Ia64LocalSapicIpiControllerInitializationError::UnsupportedProcessorController));

    Ia64LocalSapicIpiController identityController;
    topology = MakeOneProcessorTopology(256U, 2U);
    const bool wideIdRejected =
        InitializationFailsAtomically(identityController, topology, MakeCrLid(0U, 2U), MakeAccess(),
                                      Ia64LocalSapicIpiControllerInitializationError::InvalidProcessorIdentity);
    topology = MakeOneProcessorTopology(1U, 256U);
    ASTERIA_EXPECT(
        runner, "hardware IDs and EIDs must fit their eight-bit address fields", wideIdRejected,
        InitializationFailsAtomically(identityController, topology, MakeCrLid(1U, 0U), MakeAccess(),
                                      Ia64LocalSapicIpiControllerInitializationError::InvalidProcessorIdentity));

    Ia64LocalSapicIpiController duplicateController;
    topology = MakeOneProcessorTopology();
    topology.ProcessorCount = 2U;
    topology.Processors[1U] = MakeProcessor(11U, 1U, 2U);
    const bool hardwareDuplicateRejected =
        InitializationFailsAtomically(duplicateController, topology, MakeCrLid(1U, 2U), MakeAccess(),
                                      Ia64LocalSapicIpiControllerInitializationError::DuplicateProcessorIdentity);
    topology = MakeOneProcessorTopology();
    topology.ProcessorCount = 2U;
    topology.Processors[1U] = MakeProcessor(10U, 3U, 4U);
    ASTERIA_EXPECT(
        runner, "hardware identities and ACPI UIDs must both be distinct", hardwareDuplicateRejected,
        InitializationFailsAtomically(duplicateController, topology, MakeCrLid(1U, 2U), MakeAccess(),
                                      Ia64LocalSapicIpiControllerInitializationError::DuplicateProcessorIdentity));

    Ia64LocalSapicIpiController bspController;
    topology = MakeOneProcessorTopology();
    const bool absentBspRejected =
        InitializationFailsAtomically(bspController, topology, MakeCrLid(9U, 9U), MakeAccess(),
                                      Ia64LocalSapicIpiControllerInitializationError::BspNotEnabled);
    topology = MakeOneProcessorTopology();
    topology.ProcessorCount = 2U;
    topology.Processors[1U] = MakeProcessor(11U, 9U, 9U, OnlineCapableFlag);
    ASTERIA_EXPECT(runner, "cr.lid must identify an enabled rather than hotplug-only BSP", absentBspRejected,
                   InitializationFailsAtomically(bspController, topology, MakeCrLid(9U, 9U), MakeAccess(),
                                                 Ia64LocalSapicIpiControllerInitializationError::BspNotEnabled));

    Ia64LocalSapicIpiController hotplugValidationController;
    topology = MakeOneProcessorTopology();
    topology.ProcessorCount = 2U;
    topology.Processors[1U] = MakeProcessor(11U, 256U, 3U, OnlineCapableFlag);
    const bool invalidHotplugIdentityRejected =
        InitializationFailsAtomically(hotplugValidationController, topology, MakeCrLid(1U, 2U), MakeAccess(),
                                      Ia64LocalSapicIpiControllerInitializationError::InvalidProcessorIdentity);
    topology = MakeOneProcessorTopology();
    topology.ProcessorCount = 2U;
    topology.Processors[1U] = MakeProcessor(11U, 1U, 2U, OnlineCapableFlag);
    ASTERIA_EXPECT(
        runner, "excluded hotplug records still receive full identity validation", invalidHotplugIdentityRejected,
        InitializationFailsAtomically(hotplugValidationController, topology, MakeCrLid(1U, 2U), MakeAccess(),
                                      Ia64LocalSapicIpiControllerInitializationError::DuplicateProcessorIdentity));
}

void TestProcessorInterruptBlockBaseValidation(TestContext& runner) noexcept
{
    ResetWrites();
    Ia64LocalSapicIpiController physicalController;
    AcpiTopology topology = MakeOneProcessorTopology();
    topology.LocalControllerPhysicalAddress = 0U;
    const bool zeroRejected =
        InitializationFailsAtomically(physicalController, topology, MakeCrLid(1U, 2U), MakeAccess(0U),
                                      Ia64LocalSapicIpiControllerInitializationError::InvalidPhysicalBase);
    topology = MakeOneProcessorTopology();
    topology.LocalControllerPhysicalAddress = DefaultPibPhysicalBase + 0x1000U;
    const bool unalignedRejected = InitializationFailsAtomically(
        physicalController, topology, MakeCrLid(1U, 2U), MakeAccess(DefaultPibBootstrapAccessBase + 0x1000U),
        Ia64LocalSapicIpiControllerInitializationError::InvalidPhysicalBase);
    topology = MakeOneProcessorTopology();
    topology.LocalControllerPhysicalAddress = DefaultPibPhysicalBase + OneMiBBytes;
    ASTERIA_EXPECT(runner, "a one-MiB-only aligned physical PIB base is rejected",
                   InitializationFailsAtomically(physicalController, topology, MakeCrLid(1U, 2U),
                                                 MakeAccess(DefaultPibBootstrapAccessBase + OneMiBBytes),
                                                 Ia64LocalSapicIpiControllerInitializationError::InvalidPhysicalBase));
    topology = MakeOneProcessorTopology();
    topology.LocalControllerPhysicalAddress = 0x8000'0000'0000'0000ULL;
    ASTERIA_EXPECT(runner, "physical PIB bases require one aligned nonwrapping 2 MiB extent", zeroRejected,
                   unalignedRejected,
                   InitializationFailsAtomically(physicalController, topology, MakeCrLid(1U, 2U),
                                                 MakeAccess(0x8000'0000'0000'0000ULL),
                                                 Ia64LocalSapicIpiControllerInitializationError::InvalidPhysicalBase));

    Ia64LocalSapicIpiController accessController;
    topology = MakeOneProcessorTopology();
    Ia64ProcessorInterruptBlockAccess access = MakeAccess();
    access.Kind = static_cast<Ia64ProcessorInterruptBlockBaseKind>(2U);
    const bool invalidKindRejected =
        InitializationFailsAtomically(accessController, topology, MakeCrLid(1U, 2U), access,
                                      Ia64LocalSapicIpiControllerInitializationError::InvalidBaseKind);
    access = MakeAccess();
    access.Reserved[6U] = 1U;
    const bool reservedRejected =
        InitializationFailsAtomically(accessController, topology, MakeCrLid(1U, 2U), access,
                                      Ia64LocalSapicIpiControllerInitializationError::InvalidAccessBase);
    access = MakeAccess(DefaultPibBootstrapAccessBase + 0x1000U);
    const bool unalignedAccessRejected =
        InitializationFailsAtomically(accessController, topology, MakeCrLid(1U, 2U), access,
                                      Ia64LocalSapicIpiControllerInitializationError::InvalidAccessBase);
    access = MakeAccess(DefaultPibBootstrapAccessBase + OneMiBBytes,
                        Ia64ProcessorInterruptBlockBaseKind::PinnedUncachedVirtual);
    ASTERIA_EXPECT(runner, "a one-MiB-only aligned access base is rejected",
                   InitializationFailsAtomically(accessController, topology, MakeCrLid(1U, 2U), access,
                                                 Ia64LocalSapicIpiControllerInitializationError::InvalidAccessBase));
    access = MakeAccess(MaximumUInt64 - 7U);
    ASTERIA_EXPECT(runner, "access kind, reserved state, alignment, and overflow are checked", invalidKindRejected,
                   reservedRejected, unalignedAccessRejected,
                   InitializationFailsAtomically(accessController, topology, MakeCrLid(1U, 2U), access,
                                                 Ia64LocalSapicIpiControllerInitializationError::InvalidAccessBase));

    Ia64LocalSapicIpiController aliasController;
    topology = MakeOneProcessorTopology(0x12U, 0x34U);
    const bool writeBackAliasRejected = InitializationFailsAtomically(
        aliasController, topology, MakeCrLid(0x12U, 0x34U), MakeAccess(DefaultPibPhysicalBase),
        Ia64LocalSapicIpiControllerInitializationError::AccessBaseMismatch);
    const bool uncachedAliasAccepted = aliasController.Initialize(topology, MakeCrLid(0x12U, 0x34U), MakeAccess()) ==
                                       Ia64LocalSapicIpiControllerInitializationError::None;
    const Ia64LocalSapicIpiControllerSnapshot aliasSnapshot = aliasController.GetSnapshot();
    ResetWrites();
    ASTERIA_EXPECT(runner, "bootstrap access rejects WBL and writes through the exact UC alias", writeBackAliasRejected,
                   uncachedAliasAccepted, aliasSnapshot.PhysicalBaseAddress == DefaultPibPhysicalBase,
                   aliasSnapshot.AccessBaseAddress == DefaultPibBootstrapAccessBase,
                   aliasController.SendExternalInterrupt(0xE1U, 1U) == Ia64LocalSapicIpiSendError::None,
                   g_writeCount == 1U,
                   g_writes[0U].Address == InterruptAddress(DefaultPibBootstrapAccessBase, 0x12U, 0x34U),
                   g_writes[0U].Value == 0xE1U);

    ResetWrites();
    Ia64LocalSapicIpiController mismatchController;
    ASTERIA_EXPECT(runner, "bootstrap physical access cannot alias a different PIB",
                   InitializationFailsAtomically(mismatchController, topology, MakeCrLid(0x12U, 0x34U),
                                                 MakeAccess(DefaultPibBootstrapAccessBase + (2U * 1024U * 1024U)),
                                                 Ia64LocalSapicIpiControllerInitializationError::AccessBaseMismatch));

    constexpr Foundation::UInt64 HighestPhysicalPibBase{0x7FFF'FFFF'FFE0'0000ULL};
    constexpr Foundation::UInt64 HighestBootstrapAccessBase{HighestPhysicalPibBase | PhysicalAddressUncachedBit};
    Ia64LocalSapicIpiController edgeController;
    topology = MakeOneProcessorTopology(255U, 255U);
    topology.LocalControllerPhysicalAddress = HighestPhysicalPibBase;
    ASTERIA_EXPECT(
        runner, "the highest complete physical PIB and maximum ID/EID encode safely",
        edgeController.Initialize(topology, MakeCrLid(255U, 255U), MakeAccess(HighestBootstrapAccessBase)) ==
            Ia64LocalSapicIpiControllerInitializationError::None,
        edgeController.GetSnapshot().PhysicalBaseAddress == HighestPhysicalPibBase,
        edgeController.GetSnapshot().AccessBaseAddress == HighestBootstrapAccessBase,
        ProcessorIs(edgeController.GetSnapshot().Processors[0U], 10U, 255U, 255U, HighestBootstrapAccessBase));
}

void TestZeroIdentityWithLowVirtualAccess(TestContext& runner) noexcept
{
    ResetWrites();
    const AcpiTopology topology = MakeOneProcessorTopology(0U, 0U);
    const Ia64ProcessorInterruptBlockAccess access =
        MakeAccess(DefaultPibPhysicalBase, Ia64ProcessorInterruptBlockBaseKind::PinnedUncachedVirtual);
    Ia64LocalSapicIpiController controller;
    const bool initialized = controller.Initialize(topology, MakeCrLid(0U, 0U), access) ==
                             Ia64LocalSapicIpiControllerInitializationError::None;
    const Ia64LocalSapicIpiControllerSnapshot snapshot = controller.GetSnapshot();
    ResetWrites();
    const bool sent = initialized && (controller.SendExternalInterrupt(0xD0U, 1U) == Ia64LocalSapicIpiSendError::None);

    ASTERIA_EXPECT(runner, "zero ID/EID and a bit-63-clear pinned UC mapping are accepted", initialized,
                   snapshot.PhysicalBaseAddress == DefaultPibPhysicalBase,
                   snapshot.AccessBaseAddress == DefaultPibPhysicalBase,
                   snapshot.BaseKind == Ia64ProcessorInterruptBlockBaseKind::PinnedUncachedVirtual,
                   ProcessorIs(snapshot.Processors[0U], 10U, 0U, 0U, DefaultPibPhysicalBase), sent, g_writeCount == 1U,
                   g_writes[0U].Address == DefaultPibPhysicalBase, g_writes[0U].Value == 0xD0U);
}

void TestFailureRetryAndSinglePublication(TestContext& runner) noexcept
{
    ResetWrites();
    Ia64LocalSapicIpiController controller;
    AcpiTopology topology = MakeOneProcessorTopology();
    topology.Processors[0U].HardwareId = 256U;
    const bool failedAtomically =
        InitializationFailsAtomically(controller, topology, MakeCrLid(1U, 2U), MakeAccess(),
                                      Ia64LocalSapicIpiControllerInitializationError::InvalidProcessorIdentity);

    topology = MakeOneProcessorTopology(0xAAU, 0xBBU);
    const bool retried = controller.Initialize(topology, MakeCrLid(0xAAU, 0xBBU), MakeAccess()) ==
                         Ia64LocalSapicIpiControllerInitializationError::None;
    const Ia64LocalSapicIpiControllerSnapshot published = controller.GetSnapshot();
    topology.Processors[0U] = MakeProcessor(999U, 1U, 1U);
    const bool secondRejected = controller.Initialize(topology, MakeCrLid(1U, 1U), MakeAccess()) ==
                                Ia64LocalSapicIpiControllerInitializationError::AlreadyInitialized;
    const Ia64LocalSapicIpiControllerSnapshot afterSecond = controller.GetSnapshot();

    ASTERIA_EXPECT(
        runner, "failed admission is retryable but successful publication is final", failedAtomically, retried,
        secondRejected, ProcessorIs(published.Processors[0U], 10U, 0xAAU, 0xBBU, DefaultPibBootstrapAccessBase),
        ProcessorIs(afterSecond.Processors[0U], 10U, 0xAAU, 0xBBU, DefaultPibBootstrapAccessBase), g_writeCount == 0U);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64LocalSapicIpiControllerTests"};
    Asteria::Test::TestOneProcessorAndSendValidation(runner);
    Asteria::Test::TestTwoProcessorBspReordering(runner);
    Asteria::Test::TestFourProcessorOrderingAndVirtualAccess(runner);
    Asteria::Test::TestOnlineCapableProcessorsAreExcluded(runner);
    Asteria::Test::TestProcessorRecordValidation(runner);
    Asteria::Test::TestProcessorInterruptBlockBaseValidation(runner);
    Asteria::Test::TestZeroIdentityWithLowVirtualAccess(runner);
    Asteria::Test::TestFailureRetryAndSinglePublication(runner);
    return runner.Finish();
}
