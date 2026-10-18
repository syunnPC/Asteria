// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <new>
#include <sys/mman.h>

#include "Platform/Amd64/Device/Amd64PhysicalMemoryAccess.hpp"
#include "Platform/Device/PciEcamRuntime.hpp"
#include "Platform/Device/UsbHostPublication.hpp"
#include "Platform/Firmware/Diagnostic.hpp"
#include "Platform/Ia64/Device/Ia64PhysicalMemoryAccess.hpp"

#include "Support/ManualClock.hpp"
#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
// The debug channel's bytes, for the USB host publication's lines.
char g_diagnostic[1024U]{};
Foundation::Size g_diagnosticLength{0U};
} // Namespace
} // Namespace Asteria::Test

namespace Asteria::Platform::Firmware
{
void WriteDiagnosticBytes(const Foundation::UInt8* const bytes, const Foundation::Size sizeBytes) noexcept
{
    for (Foundation::Size index{0U};
         (index < sizeBytes) && (Test::g_diagnosticLength + 1U < sizeof(Test::g_diagnostic)); ++index)
    {
        Test::g_diagnostic[Test::g_diagnosticLength++] = static_cast<char>(bytes[index]);
    }
}
} // Namespace Asteria::Platform::Firmware

namespace Asteria::Test
{
namespace
{
// A fixed key stands in for the boot-derived DeviceSpace key.
constexpr Kernel::Object::CapabilityValidationKey TestCapabilityValidationKey{
    .Word0 = 0xEC31'BBA4'DE82'27F1ULL,
    .Word1 = 0x4961'36E8'57C9'0A2DULL,
};

using Foundation::UInt8;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;
using Hal::Time::IMonotonicClock;
using Hal::Device::PciConfigurationReadError;
using Kernel::Device::AhciDmaPageWritableView;
using Kernel::Device::AhciIdentifyCommandError;
using Kernel::Device::AhciIdentifyCommandReceipt;
using Kernel::Device::AhciIdentifyDmaPageBuilder;
using Kernel::Device::AhciIdentifyPollingPolicy;
using Kernel::Device::AhciReadDmaExtCommandBuilder;
using Kernel::Device::AhciReadDmaExtCommandError;
using Kernel::Device::AhciReadDmaExtCommandPageWritableView;
using Kernel::Device::AhciReadDmaExtDataBufferSpan;
using Kernel::Device::AhciReadDmaExtExecutionError;
using Kernel::Device::AhciReadDmaExtExecutionReceipt;
using Kernel::Device::AhciReadDmaExtPollingPolicy;
using Kernel::Device::AhciReadDmaExtPortDisposition;
using Kernel::Device::DeviceManager;
using Kernel::Device::DeviceAccessWidth;
using Kernel::Device::DeviceRegionKind;
using Kernel::Device::DeviceRegionPermission;
using Kernel::Device::DeviceRegion;
using Kernel::Device::PciFunction;
using Kernel::Device::PciFunctionAddress;
using Kernel::Device::PciFunctionManagerError;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Resource::ResourceDomain;
using Kernel::Resource::ResourceDomainError;
using Kernel::Resource::ResourceDomainLedger;
using Kernel::Resource::ResourceDomainLimits;
using Kernel::Resource::ResourceDomainObjectId;
using Platform::Device::PciEcamAvailability;
using Platform::Device::PciEcamFunctionQueryError;
using Platform::Device::PciEcamRuntimeSummary;
using Platform::Device::PciEcamRuntime;
using Platform::Device::PciEcamRuntimeError;
using Platform::Device::PciEcamRuntimeState;
using Platform::Device::PciFunctionPublication;
using Platform::Device::AhciPciAvailability;
using Platform::Device::AhciPciRuntimeError;
using Platform::Device::AhciPciRuntimeQueryError;
using Platform::Device::AhciPciRuntimeResult;
using Platform::Device::AhciIdentifyRuntimeError;
using Platform::Device::AhciIdentifyRuntimeQueryError;
using Platform::Device::AhciIdentifyRuntimeState;
using Platform::Device::AhciReadDmaExtRuntimeError;
using Platform::Device::AhciReadDmaExtRuntimeQueryError;
using Platform::Device::AhciReadDmaExtRuntimeState;
using Platform::Firmware::AhciPciResource;
using Platform::Synchronization::SpinLock;
using TestSupport::ManualClock;

/// One ISA's physical access and the widest physical address it names. The
/// argv mode selects the profile before any scenario runs; the ECAM scenarios
/// prepare with that width, the AHCI ones with 32 bits.
struct AccessProfile
{
    const Platform::Device::IPhysicalMemoryAccess& Access;
    UInt8 PhysicalAddressBits;
};

const AccessProfile* g_profile{nullptr};

constexpr Foundation::Size EcamBusSizeBytes{Foundation::Size{1U} << 20U};
constexpr Foundation::Size EcamTestBusCount{2U};
constexpr Foundation::Size AbarSizeBytes{4096U};
constexpr Foundation::Size IdentifyMutationCaseCount{6U};
constexpr Foundation::Size ReadDmaStorageCount{12U};
constexpr UInt32 DmaEnabledPciCommand{0x06U};
struct alignas(AhciIdentifyDmaPageBuilder::PageAlignmentBytes) IdentifyDmaPageStorage
{
    UInt8 Bytes[AhciIdentifyDmaPageBuilder::PageSizeBytes];
};
IdentifyDmaPageStorage g_identifyDmaPage{};
IdentifyDmaPageStorage g_mutatedIdentifyDmaPages[IdentifyMutationCaseCount]{};
struct alignas(AhciReadDmaExtCommandBuilder::CommandPageAlignmentBytes) ReadDmaPageStorage
{
    UInt8 Bytes[AhciReadDmaExtCommandBuilder::CommandPageSizeBytes];
};
ReadDmaPageStorage g_readCommandPages[ReadDmaStorageCount]{};
ReadDmaPageStorage g_readDataPages[ReadDmaStorageCount]{};
UInt8* const g_ecam =
    static_cast<UInt8*>(::mmap(reinterpret_cast<void*>(0x6000'0000ULL), EcamBusSizeBytes* EcamTestBusCount,
                               PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0));
UInt8* const g_abar = static_cast<UInt8*>(
    ::mmap(nullptr, AbarSizeBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0));

static_assert(sizeof(IdentifyDmaPageStorage) == AhciIdentifyDmaPageBuilder::PageSizeBytes);
static_assert(alignof(IdentifyDmaPageStorage) == AhciIdentifyDmaPageBuilder::PageAlignmentBytes);
static_assert(sizeof(ReadDmaPageStorage) == AhciReadDmaExtCommandBuilder::CommandPageSizeBytes);
static_assert(alignof(ReadDmaPageStorage) == AhciReadDmaExtCommandBuilder::CommandPageAlignmentBytes);

class RuntimeFixture final
{
public:
    explicit RuntimeFixture(const UInt64 metadataLimit) noexcept
        : Registry{Synchronization}, Issuer{Registry}, Domain{Ledger}
    {
        const ResourceDomainLimits limits{
            .CommittedMemoryPages = 16U,
            .ResidentMemoryPages = 16U,
            .KernelMetadataBytes = metadataLimit,
            .CapabilityHandles = 512U,
            .IpcQueueBytes = 1024U,
            .Threads = 16U,
            .ChildDomains = 16U,
        };
        Ready = Domain.InitializeRoot(ResourceDomainId{93U}, limits).Error == ResourceDomainError::None;
        const auto registered = Issuer.CreateObject(Domain);
        if (registered.HasValue())
        {
            DomainIdentity = ResourceDomainObjectId{Registry, registered.GetValue()};
        }
        else
        {
            Ready = false;
        }
    }

    SpinLock Synchronization;
    Asteria::TestSupport::TestObjectRegistry Registry;
    RootCapabilityIssuer Issuer;
    ResourceDomainLedger Ledger;
    ResourceDomain Domain;
    ResourceDomainObjectId DomainIdentity{};
    bool Ready{true};
};

constexpr Foundation::Size RetainedRuntimeCount{56U};

struct alignas(RuntimeFixture) RuntimeFixtureStorage
{
    UInt8 Bytes[sizeof(RuntimeFixture)];
};

struct alignas(PciEcamRuntime) PciEcamRuntimeStorage
{
    UInt8 Bytes[sizeof(PciEcamRuntime)];
};

struct RetainedRuntime
{
    RuntimeFixture& Fixture;
    PciEcamRuntime& Runtime;
};

// Runtime and every referenced owner deliberately remain alive through process
// termination. This mirrors the target's non-destructible boot-lifetime owner.
RuntimeFixtureStorage g_runtimeFixtureStorage[RetainedRuntimeCount]{};
PciEcamRuntimeStorage g_pciEcamRuntimeStorage[RetainedRuntimeCount]{};

[[nodiscard]] RetainedRuntime ConstructRetainedRuntime(const Foundation::Size index,
                                                       const UInt64 metadataLimit) noexcept
{
    RuntimeFixture* const fixture =
        ::new (static_cast<void*>(g_runtimeFixtureStorage[index].Bytes)) RuntimeFixture{metadataLimit};
    PciEcamRuntime* const runtime = ::new (static_cast<void*>(g_pciEcamRuntimeStorage[index].Bytes)) PciEcamRuntime{
        fixture->Registry, fixture->Synchronization, fixture->Issuer, TestCapabilityValidationKey, g_profile->Access};
    return RetainedRuntime{*fixture, *runtime};
}

static_assert(sizeof(RuntimeFixtureStorage) == sizeof(RuntimeFixture));
static_assert(alignof(RuntimeFixtureStorage) == alignof(RuntimeFixture));
static_assert(sizeof(PciEcamRuntimeStorage) == sizeof(PciEcamRuntime));
static_assert(alignof(PciEcamRuntimeStorage) == alignof(PciEcamRuntime));

[[nodiscard]] Boot::BootInfo MakeBootInfo(const UInt64 baseAddress) noexcept
{
    Boot::BootInfo bootInfo{};
    bootInfo.Signature = Boot::BootInfoSignature;
    bootInfo.Version = Boot::BootInfoVersion;
    bootInfo.SizeBytes = sizeof(Boot::BootInfo);
    if (baseAddress != 0U)
    {
        bootInfo.Acpi.McfgPhysicalAddress = 0x1000U;
        bootInfo.Acpi.McfgRevision = 1U;
        bootInfo.Acpi.PciConfigurationRegionCount = 1U;
        bootInfo.Acpi.PciConfigurationRegions[0U] = Boot::Acpi::AcpiPciConfigurationRegion{
            .BaseAddress = baseAddress,
            .Segment = 7U,
            .FirstBus = 0U,
            .LastBus = 0U,
            .Reserved = 0U,
        };
    }
    return bootInfo;
}

[[nodiscard]] Boot::BootInfo MakeBoundaryBootInfo(const UInt64 physicalStart) noexcept
{
    constexpr UInt8 FirstBus{254U};
    constexpr UInt8 LastBus{255U};
    const UInt64 busZeroBase = physicalStart - (static_cast<UInt64>(FirstBus) << 20U);
    Boot::BootInfo bootInfo{};
    bootInfo.Signature = Boot::BootInfoSignature;
    bootInfo.Version = Boot::BootInfoVersion;
    bootInfo.SizeBytes = sizeof(Boot::BootInfo);
    bootInfo.Acpi.McfgPhysicalAddress = 0x1000U;
    bootInfo.Acpi.McfgRevision = 1U;
    bootInfo.Acpi.PciConfigurationRegionCount = 2U;
    bootInfo.Acpi.PciConfigurationRegions[0U] = Boot::Acpi::AcpiPciConfigurationRegion{
        .BaseAddress = busZeroBase,
        .Segment = 3U,
        .FirstBus = FirstBus,
        .LastBus = FirstBus,
        .Reserved = 0U,
    };
    bootInfo.Acpi.PciConfigurationRegions[1U] = Boot::Acpi::AcpiPciConfigurationRegion{
        .BaseAddress = busZeroBase,
        .Segment = 7U,
        .FirstBus = LastBus,
        .LastBus = LastBus,
        .Reserved = 0U,
    };
    return bootInfo;
}

void FillEcam(const UInt8 value) noexcept
{
    for (Foundation::Size index{0U}; index < (EcamBusSizeBytes * EcamTestBusCount); ++index)
    {
        g_ecam[index] = value;
    }
}

void FillAbar(const UInt8 value) noexcept
{
    for (Foundation::Size index{0U}; index < AbarSizeBytes; ++index)
    {
        g_abar[index] = value;
    }
}

void WriteAbarDoubleWord(const Foundation::Size registerOffset, const UInt32 value) noexcept
{
    g_abar[registerOffset] = static_cast<UInt8>(value & 0xFFU);
    g_abar[registerOffset + 1U] = static_cast<UInt8>((value >> 8U) & 0xFFU);
    g_abar[registerOffset + 2U] = static_cast<UInt8>((value >> 16U) & 0xFFU);
    g_abar[registerOffset + 3U] = static_cast<UInt8>((value >> 24U) & 0xFFU);
}

[[nodiscard]] UInt32 ReadAbarDoubleWord(const Foundation::Size registerOffset) noexcept
{
    return static_cast<UInt32>(g_abar[registerOffset]) | (static_cast<UInt32>(g_abar[registerOffset + 1U]) << 8U) |
           (static_cast<UInt32>(g_abar[registerOffset + 2U]) << 16U) |
           (static_cast<UInt32>(g_abar[registerOffset + 3U]) << 24U);
}

void WriteIdentifyWord(UInt8* const identifyData, const Foundation::Size word, const UInt16 value) noexcept
{
    identifyData[word * 2U] = static_cast<UInt8>(value & 0xFFU);
    identifyData[(word * 2U) + 1U] = static_cast<UInt8>((value >> 8U) & 0xFFU);
}

void WriteIdentifyString(UInt8* const identifyData, const Foundation::Size firstWord, const Foundation::Size wordCount,
                         const char* const value) noexcept
{
    const Foundation::Size characterCount = wordCount * 2U;
    Foundation::Size length{0U};
    while ((length < characterCount) && (value[length] != '\0'))
    {
        ++length;
    }
    for (Foundation::Size index{0U}; index < characterCount; ++index)
    {
        const UInt8 character = (index < length) ? static_cast<UInt8>(value[index]) : static_cast<UInt8>(' ');
        const Foundation::Size word = firstWord + (index / 2U);
        const Foundation::Size byte = ((index % 2U) == 0U) ? 1U : 0U;
        identifyData[(word * 2U) + byte] = character;
    }
}

[[nodiscard]] UInt32 ReadLittleEndianDoubleWord(const UInt8* const bytes) noexcept
{
    return static_cast<UInt32>(bytes[0U]) | (static_cast<UInt32>(bytes[1U]) << 8U) |
           (static_cast<UInt32>(bytes[2U]) << 16U) | (static_cast<UInt32>(bytes[3U]) << 24U);
}

void WriteLittleEndianDoubleWord(UInt8* const bytes, const UInt32 value) noexcept
{
    bytes[0U] = static_cast<UInt8>(value & 0xFFU);
    bytes[1U] = static_cast<UInt8>((value >> 8U) & 0xFFU);
    bytes[2U] = static_cast<UInt8>((value >> 16U) & 0xFFU);
    bytes[3U] = static_cast<UInt8>((value >> 24U) & 0xFFU);
}

[[nodiscard]] UInt64 ReadLittleEndianAddress(const UInt8* const bytes) noexcept
{
    return static_cast<UInt64>(ReadLittleEndianDoubleWord(bytes)) |
           (static_cast<UInt64>(ReadLittleEndianDoubleWord(bytes + 4U)) << 32U);
}

void WriteLittleEndianAddress(UInt8* const bytes, const UInt64 value) noexcept
{
    WriteLittleEndianDoubleWord(bytes, static_cast<UInt32>(value));
    WriteLittleEndianDoubleWord(bytes + 4U, static_cast<UInt32>(value >> 32U));
}

[[nodiscard]] UInt64 ReadAbarAddress(const Foundation::Size lowRegisterOffset) noexcept
{
    return static_cast<UInt64>(ReadAbarDoubleWord(lowRegisterOffset)) |
           (static_cast<UInt64>(ReadAbarDoubleWord(lowRegisterOffset + 4U)) << 32U);
}

void WriteAbarAddress(const Foundation::Size lowRegisterOffset, const UInt64 value) noexcept
{
    WriteAbarDoubleWord(lowRegisterOffset, static_cast<UInt32>(value));
    WriteAbarDoubleWord(lowRegisterOffset + 4U, static_cast<UInt32>(value >> 32U));
}

void PopulateIdentifyData(UInt8* const identifyData) noexcept
{
    WriteIdentifyString(identifyData, 10U, 10U, "RUNTIME-SERIAL");
    WriteIdentifyString(identifyData, 27U, 20U, "ASTERIA RUNTIME DISK");
    WriteIdentifyWord(identifyData, 49U, UInt16{1U} << 9U);
    WriteIdentifyWord(identifyData, 83U, 0x4400U);
    constexpr UInt64 SectorCount{0x0000'0001'2345'6789ULL};
    for (Foundation::Size index{0U}; index < 4U; ++index)
    {
        WriteIdentifyWord(identifyData, 100U + index, static_cast<UInt16>(SectorCount >> (index * 16U)));
    }
    WriteIdentifyWord(identifyData, 106U, 0x4000U);
    WriteIdentifyWord(identifyData, 117U, 0U);
    WriteIdentifyWord(identifyData, 118U, 0U);
}

enum class AhciCompletionMutation : UInt8
{
    None,
    CommandListAddress,
    ReceivedFisAddress,
    AtaCommand,
    PrdtAddress,
    PrdtByteCount,
};

enum class AhciCompletionValidationError : UInt8
{
    None,
    MultipleIssuedCommands,
    UnsupportedCommandSlot,
    InvalidCommandListAddress,
    InvalidReceivedFisAddress,
    InvalidCommandHeader,
    InvalidCommandTableAddress,
    InvalidCommandFis,
    InvalidPrdt,
    MutationFailed,
};

struct AhciDecodedIdentifyCommand
{
    UInt8* CommandHeader;
    UInt8* ReceivedFis;
    UInt8* IdentifyData;
    UInt8 Port;
    UInt8 Slot;
};

[[nodiscard]] constexpr Foundation::Size GetAhciPortRegisterOffset(const UInt8 port,
                                                                   const Foundation::Size registerOffset) noexcept
{
    return 0x100U + (static_cast<Foundation::Size>(port) * 0x80U) + registerOffset;
}

class AhciCompletionEmulatorClock final : public IMonotonicClock
{
public:
    explicit constexpr AhciCompletionEmulatorClock(
        const AhciDmaPageWritableView dmaPage,
        const AhciCompletionMutation mutation = AhciCompletionMutation::None) noexcept
        : m_dmaPage{dmaPage}, m_mutation{mutation}
    {
    }

    [[nodiscard]] UInt64 GetTicks() const noexcept override
    {
        ++m_ticks;
        if (!m_attempted)
        {
            ProcessIssuedCommand();
        }
        return m_ticks;
    }

    [[nodiscard]] constexpr bool WasCommandObserved() const noexcept
    {
        return m_commandObserved;
    }

    [[nodiscard]] constexpr bool WasMutationApplied() const noexcept
    {
        return m_mutationApplied;
    }

    [[nodiscard]] constexpr bool WasCompletionPublished() const noexcept
    {
        return m_completionPublished;
    }

    [[nodiscard]] constexpr AhciCompletionValidationError GetValidationError() const noexcept
    {
        return m_validationError;
    }

private:
    [[nodiscard]] bool ResolveDmaRange(const UInt64 address, const Foundation::Size sizeBytes,
                                       UInt8*& data) const noexcept
    {
        if ((m_dmaPage.Data == nullptr) || (address < m_dmaPage.PhysicalAddress))
        {
            return false;
        }
        const UInt64 offset = address - m_dmaPage.PhysicalAddress;
        if ((offset > m_dmaPage.SizeBytes) || (sizeBytes > (m_dmaPage.SizeBytes - offset)))
        {
            return false;
        }
        data = m_dmaPage.Data + static_cast<Foundation::Size>(offset);
        return true;
    }

    [[nodiscard]] bool ResolveCommandTableForMutation(const UInt8 port, const UInt8 slot,
                                                      UInt8*& commandTable) const noexcept
    {
        const Foundation::Size portOffset = GetAhciPortRegisterOffset(port, 0U);
        const UInt64 commandListAddress = ReadAbarAddress(portOffset);
        UInt8* commandList{nullptr};
        if (!ResolveDmaRange(commandListAddress, AhciIdentifyDmaPageBuilder::CommandListSizeBytes, commandList))
        {
            return false;
        }
        constexpr Foundation::Size CommandHeaderSizeBytes{32U};
        UInt8* const commandHeader = commandList + (static_cast<Foundation::Size>(slot) * CommandHeaderSizeBytes);
        const UInt64 commandTableAddress = ReadLittleEndianAddress(commandHeader + 8U);
        return ResolveDmaRange(commandTableAddress, AhciIdentifyDmaPageBuilder::CommandTableSizeBytes, commandTable);
    }

    [[nodiscard]] bool ApplyMutation(const UInt8 port, const UInt8 slot) const noexcept
    {
        const Foundation::Size portOffset = GetAhciPortRegisterOffset(port, 0U);
        const UInt64 outsideDmaPage = m_dmaPage.PhysicalAddress + m_dmaPage.SizeBytes;
        if (m_mutation == AhciCompletionMutation::None)
        {
            return true;
        }
        if (m_mutation == AhciCompletionMutation::CommandListAddress)
        {
            WriteAbarAddress(portOffset, outsideDmaPage);
        }
        else if (m_mutation == AhciCompletionMutation::ReceivedFisAddress)
        {
            WriteAbarAddress(portOffset + 8U, outsideDmaPage);
        }
        else
        {
            UInt8* commandTable{nullptr};
            if (!ResolveCommandTableForMutation(port, slot, commandTable))
            {
                return false;
            }
            UInt8* const prdt = commandTable + 128U;
            if (m_mutation == AhciCompletionMutation::AtaCommand)
            {
                commandTable[2U] = 0xE7U;
            }
            else if (m_mutation == AhciCompletionMutation::PrdtAddress)
            {
                WriteLittleEndianAddress(prdt, outsideDmaPage);
            }
            else
            {
                WriteLittleEndianDoubleWord(prdt + 12U, 510U);
            }
        }
        m_mutationApplied = true;
        return true;
    }

    [[nodiscard]] bool DecodeIssuedCommand(const UInt8 port, const UInt8 slot,
                                           AhciDecodedIdentifyCommand& decoded) const noexcept
    {
        const Foundation::Size portOffset = GetAhciPortRegisterOffset(port, 0U);
        const UInt64 commandListAddress = ReadAbarAddress(portOffset);
        UInt8* commandList{nullptr};
        if (((commandListAddress & 0x3FFU) != 0U) ||
            !ResolveDmaRange(commandListAddress, AhciIdentifyDmaPageBuilder::CommandListSizeBytes, commandList))
        {
            m_validationError = AhciCompletionValidationError::InvalidCommandListAddress;
            return false;
        }
        const UInt64 receivedFisAddress = ReadAbarAddress(portOffset + 8U);
        UInt8* receivedFis{nullptr};
        if (((receivedFisAddress & 0xFFU) != 0U) ||
            !ResolveDmaRange(receivedFisAddress, AhciIdentifyDmaPageBuilder::ReceivedFisSizeBytes, receivedFis))
        {
            m_validationError = AhciCompletionValidationError::InvalidReceivedFisAddress;
            return false;
        }

        constexpr Foundation::Size CommandHeaderSizeBytes{32U};
        UInt8* const commandHeader = commandList + (static_cast<Foundation::Size>(slot) * CommandHeaderSizeBytes);
        if ((ReadLittleEndianDoubleWord(commandHeader) != 0x0001'0005U) ||
            (ReadLittleEndianDoubleWord(commandHeader + 4U) != 0U))
        {
            m_validationError = AhciCompletionValidationError::InvalidCommandHeader;
            return false;
        }
        const UInt64 commandTableAddress = ReadLittleEndianAddress(commandHeader + 8U);
        UInt8* commandTable{nullptr};
        if (((commandTableAddress & 0x7FU) != 0U) ||
            !ResolveDmaRange(commandTableAddress, AhciIdentifyDmaPageBuilder::CommandTableSizeBytes, commandTable))
        {
            m_validationError = AhciCompletionValidationError::InvalidCommandTableAddress;
            return false;
        }
        if ((commandTable[0U] != 0x27U) || (commandTable[1U] != 0x80U) || (commandTable[2U] != 0xECU))
        {
            m_validationError = AhciCompletionValidationError::InvalidCommandFis;
            return false;
        }
        for (Foundation::Size index{3U}; index < 20U; ++index)
        {
            if (commandTable[index] != 0U)
            {
                m_validationError = AhciCompletionValidationError::InvalidCommandFis;
                return false;
            }
        }

        UInt8* const prdt = commandTable + 128U;
        const UInt64 identifyDataAddress = ReadLittleEndianAddress(prdt);
        UInt8* identifyData{nullptr};
        if (((identifyDataAddress & 1U) != 0U) || (ReadLittleEndianDoubleWord(prdt + 8U) != 0U) ||
            (ReadLittleEndianDoubleWord(prdt + 12U) != 511U) ||
            !ResolveDmaRange(identifyDataAddress, AhciIdentifyDmaPageBuilder::IdentifyDataSizeBytes, identifyData))
        {
            m_validationError = AhciCompletionValidationError::InvalidPrdt;
            return false;
        }
        decoded = AhciDecodedIdentifyCommand{
            .CommandHeader = commandHeader,
            .ReceivedFis = receivedFis,
            .IdentifyData = identifyData,
            .Port = port,
            .Slot = slot,
        };
        return true;
    }

    void PublishCompletion(const AhciDecodedIdentifyCommand& decoded) const noexcept
    {
        WriteLittleEndianDoubleWord(decoded.CommandHeader + 4U, 512U);
        UInt8* const deviceToHostFis = decoded.ReceivedFis + 0x40U;
        deviceToHostFis[0U] = 0x34U;
        deviceToHostFis[2U] = 0x50U;
        deviceToHostFis[3U] = 0U;
        PopulateIdentifyData(decoded.IdentifyData);

        const Foundation::Size portOffset = GetAhciPortRegisterOffset(decoded.Port, 0U);
        WriteAbarDoubleWord(portOffset + 0x10U, 1U);
        WriteAbarDoubleWord(portOffset + 0x20U, 0U);
        const UInt32 commandIssue = ReadAbarDoubleWord(portOffset + 0x38U);
        WriteAbarDoubleWord(portOffset + 0x38U, commandIssue & ~(UInt32{1U} << decoded.Slot));
        m_completionPublished = true;
    }

    void ProcessIssuedCommand() const noexcept
    {
        const UInt8 portCount = static_cast<UInt8>((ReadAbarDoubleWord(0U) & 0x1FU) + 1U);
        const UInt32 implementedPorts = ReadAbarDoubleWord(0x0CU);
        Foundation::Size issuedCount{0U};
        UInt8 selectedPort{0U};
        UInt8 selectedSlot{0U};
        for (UInt8 port{0U}; port < portCount; ++port)
        {
            if ((implementedPorts & (UInt32{1U} << port)) == 0U)
            {
                continue;
            }
            const UInt32 commandIssue = ReadAbarDoubleWord(GetAhciPortRegisterOffset(port, 0x38U));
            for (UInt8 slot{0U}; slot < 32U; ++slot)
            {
                if ((commandIssue & (UInt32{1U} << slot)) != 0U)
                {
                    ++issuedCount;
                    selectedPort = port;
                    selectedSlot = slot;
                }
            }
        }
        if (issuedCount == 0U)
        {
            return;
        }
        m_attempted = true;
        m_commandObserved = true;
        if (issuedCount != 1U)
        {
            m_validationError = AhciCompletionValidationError::MultipleIssuedCommands;
            return;
        }
        if (selectedSlot != 0U)
        {
            m_validationError = AhciCompletionValidationError::UnsupportedCommandSlot;
            return;
        }
        if (!ApplyMutation(selectedPort, selectedSlot))
        {
            m_validationError = AhciCompletionValidationError::MutationFailed;
            return;
        }
        AhciDecodedIdentifyCommand decoded{};
        if (DecodeIssuedCommand(selectedPort, selectedSlot, decoded))
        {
            PublishCompletion(decoded);
        }
    }

    AhciDmaPageWritableView m_dmaPage;
    AhciCompletionMutation m_mutation;
    mutable UInt64 m_ticks{100U};
    mutable AhciCompletionValidationError m_validationError{AhciCompletionValidationError::None};
    mutable bool m_attempted{false};
    mutable bool m_commandObserved{false};
    mutable bool m_mutationApplied{false};
    mutable bool m_completionPublished{false};
};

class AhciReadCompletionEmulatorClock final : public IMonotonicClock
{
public:
    constexpr AhciReadCompletionEmulatorClock(const AhciReadDmaExtCommandPageWritableView commandPage,
                                              const AhciReadDmaExtDataBufferSpan dataBuffer, UInt8* const data,
                                              const bool publishCompletion = true) noexcept
        : m_commandPage{commandPage}, m_dataBuffer{dataBuffer}, m_data{data}, m_publishCompletion{publishCompletion}
    {
    }

    [[nodiscard]] UInt64 GetTicks() const noexcept override
    {
        ++m_ticks;
        if (!m_attempted)
        {
            ProcessIssuedCommand();
        }
        return m_ticks;
    }

    [[nodiscard]] constexpr bool WasCommandObserved() const noexcept
    {
        return m_commandObserved;
    }

    [[nodiscard]] constexpr bool WasExactWireObserved() const noexcept
    {
        return m_exactWireObserved;
    }

    [[nodiscard]] constexpr bool WasCompletionPublished() const noexcept
    {
        return m_completionPublished;
    }

private:
    [[nodiscard]] bool ResolveCommandRange(const UInt64 address, const Foundation::Size sizeBytes,
                                           UInt8*& data) const noexcept
    {
        if ((m_commandPage.Data == nullptr) || (address < m_commandPage.PhysicalAddress))
        {
            return false;
        }
        const UInt64 offset = address - m_commandPage.PhysicalAddress;
        if ((offset > m_commandPage.SizeBytes) || (sizeBytes > (m_commandPage.SizeBytes - offset)))
        {
            return false;
        }
        data = m_commandPage.Data + static_cast<Foundation::Size>(offset);
        return true;
    }

    [[nodiscard]] static bool BytesAreZero(const UInt8* const bytes, const Foundation::Size begin,
                                           const Foundation::Size end) noexcept
    {
        for (Foundation::Size index{begin}; index < end; ++index)
        {
            if (bytes[index] != 0U)
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool DecodeExactCommand(const UInt8 port, const UInt8 slot, UInt8*& commandHeader,
                                          UInt8*& receivedFis) const noexcept
    {
        constexpr Foundation::Size CommandHeaderSizeBytes{32U};
        const Foundation::Size portOffset = GetAhciPortRegisterOffset(port, 0U);
        const UInt64 commandListAddress = ReadAbarAddress(portOffset);
        UInt8* commandList{nullptr};
        if ((slot != 0U) || (commandListAddress != m_commandPage.PhysicalAddress) ||
            !ResolveCommandRange(commandListAddress, AhciReadDmaExtCommandBuilder::CommandListSizeBytes, commandList))
        {
            return false;
        }
        const UInt64 receivedFisAddress = ReadAbarAddress(portOffset + 8U);
        if ((receivedFisAddress !=
             (m_commandPage.PhysicalAddress + AhciReadDmaExtCommandBuilder::ReceivedFisOffsetBytes)) ||
            !ResolveCommandRange(receivedFisAddress, AhciReadDmaExtCommandBuilder::ReceivedFisSizeBytes, receivedFis))
        {
            return false;
        }

        commandHeader = commandList + (static_cast<Foundation::Size>(slot) * CommandHeaderSizeBytes);
        const UInt64 commandTableAddress = ReadLittleEndianAddress(commandHeader + 8U);
        UInt8* commandTable{nullptr};
        if ((ReadLittleEndianDoubleWord(commandHeader) != 0x0001'0005U) ||
            (ReadLittleEndianDoubleWord(commandHeader + 4U) != 0U) ||
            (commandTableAddress !=
             (m_commandPage.PhysicalAddress + AhciReadDmaExtCommandBuilder::CommandTableOffsetBytes)) ||
            !BytesAreZero(commandHeader, 16U, CommandHeaderSizeBytes) ||
            !BytesAreZero(commandList, CommandHeaderSizeBytes, AhciReadDmaExtCommandBuilder::CommandListSizeBytes) ||
            !BytesAreZero(receivedFis, 0U, AhciReadDmaExtCommandBuilder::ReceivedFisSizeBytes) ||
            !ResolveCommandRange(commandTableAddress, AhciReadDmaExtCommandBuilder::CommandTableSizeBytes,
                                 commandTable))
        {
            return false;
        }
        if ((commandTable[0U] != 0x27U) || (commandTable[1U] != 0x80U) || (commandTable[2U] != 0x25U) ||
            (commandTable[3U] != 0U) || (commandTable[4U] != 8U) || (commandTable[5U] != 0U) ||
            (commandTable[6U] != 0U) || (commandTable[7U] != 0x40U) || !BytesAreZero(commandTable, 8U, 12U) ||
            (commandTable[12U] != 1U) || (commandTable[13U] != 0U) || !BytesAreZero(commandTable, 14U, 128U))
        {
            return false;
        }
        const UInt8* const prdt = commandTable + 128U;
        return (ReadLittleEndianAddress(prdt) == m_dataBuffer.PhysicalAddress) &&
               (ReadLittleEndianDoubleWord(prdt + 8U) == 0U) && (ReadLittleEndianDoubleWord(prdt + 12U) == 511U) &&
               (m_dataBuffer.SizeBytes == 512U);
    }

    void PublishCompletion(UInt8* const commandHeader, UInt8* const receivedFis, const UInt8 port,
                           const UInt8 slot) const noexcept
    {
        constexpr char Token[]{"ASTERIA-READ-DMA-EXT-SECTOR-0001"};
        static_assert((sizeof(Token) - 1U) == 32U);
        for (Foundation::Size index{0U}; index < 512U; ++index)
        {
            m_data[index] = static_cast<UInt8>(Token[index % (sizeof(Token) - 1U)]);
        }
        WriteLittleEndianDoubleWord(commandHeader + 4U, 512U);
        UInt8* const deviceToHostFis = receivedFis + 0x40U;
        deviceToHostFis[0U] = 0x34U;
        deviceToHostFis[2U] = 0x50U;
        deviceToHostFis[3U] = 0U;

        const Foundation::Size portOffset = GetAhciPortRegisterOffset(port, 0U);
        WriteAbarDoubleWord(portOffset + 0x10U, 1U);
        WriteAbarDoubleWord(portOffset + 0x20U, 0U);
        const UInt32 commandIssue = ReadAbarDoubleWord(portOffset + 0x38U);
        WriteAbarDoubleWord(portOffset + 0x38U, commandIssue & ~(UInt32{1U} << slot));
        m_completionPublished = true;
    }

    void ProcessIssuedCommand() const noexcept
    {
        const UInt8 portCount = static_cast<UInt8>((ReadAbarDoubleWord(0U) & 0x1FU) + 1U);
        const UInt32 implementedPorts = ReadAbarDoubleWord(0x0CU);
        Foundation::Size issuedCount{0U};
        UInt8 selectedPort{0U};
        UInt8 selectedSlot{0U};
        for (UInt8 port{0U}; port < portCount; ++port)
        {
            if ((implementedPorts & (UInt32{1U} << port)) == 0U)
            {
                continue;
            }
            const UInt32 commandIssue = ReadAbarDoubleWord(GetAhciPortRegisterOffset(port, 0x38U));
            for (UInt8 slot{0U}; slot < 32U; ++slot)
            {
                if ((commandIssue & (UInt32{1U} << slot)) != 0U)
                {
                    ++issuedCount;
                    selectedPort = port;
                    selectedSlot = slot;
                }
            }
        }
        if (issuedCount == 0U)
        {
            return;
        }
        m_attempted = true;
        m_commandObserved = true;
        UInt8* commandHeader{nullptr};
        UInt8* receivedFis{nullptr};
        if ((issuedCount != 1U) || !DecodeExactCommand(selectedPort, selectedSlot, commandHeader, receivedFis))
        {
            return;
        }
        m_exactWireObserved = true;
        if (m_publishCompletion)
        {
            PublishCompletion(commandHeader, receivedFis, selectedPort, selectedSlot);
        }
    }

    AhciReadDmaExtCommandPageWritableView m_commandPage;
    AhciReadDmaExtDataBufferSpan m_dataBuffer;
    UInt8* m_data;
    bool m_publishCompletion;
    mutable UInt64 m_ticks{100U};
    mutable bool m_attempted{false};
    mutable bool m_commandObserved{false};
    mutable bool m_exactWireObserved{false};
    mutable bool m_completionPublished{false};
};

[[nodiscard]] AhciPciResource MakeAhciResource(const UInt8 device, const UInt8 function,
                                               const UInt64 baseAddress = reinterpret_cast<UInt64>(&g_abar[0U]),
                                               const UInt64 lengthBytes = AbarSizeBytes) noexcept
{
    return AhciPciResource{
        .AbarBaseAddress = baseAddress,
        .AbarLengthBytes = lengthBytes,
        .Segment = 7U,
        .Bus = 0U,
        .Device = device,
        .Function = function,
        .BarIndex = 5U,
        .Reserved = {},
    };
}

void InitializeSuccessfulAbar() noexcept
{
    FillAbar(0U);
    WriteAbarDoubleWord(0x00U, 0xCC32'1F03U);
    WriteAbarDoubleWord(0x04U, 0x8000'0002U);
    WriteAbarDoubleWord(0x08U, 0x0000'0005U);
    WriteAbarDoubleWord(0x0CU, 0x0000'000BU);
    WriteAbarDoubleWord(0x10U, 0x0001'0301U);
}

void InitializeIdentifyAbar() noexcept
{
    InitializeSuccessfulAbar();
    WriteAbarDoubleWord(0x0CU, 1U);
    WriteAbarDoubleWord(0x124U, 0x0000'0101U);
    WriteAbarDoubleWord(0x128U, 0x0000'0103U);
}

[[nodiscard]] AhciDmaPageWritableView MakeIdentifyDmaPage() noexcept
{
    return AhciDmaPageWritableView{
        .Data = g_identifyDmaPage.Bytes,
        .SizeBytes = sizeof(g_identifyDmaPage.Bytes),
        .PhysicalAddress = reinterpret_cast<UInt64>(g_identifyDmaPage.Bytes),
    };
}

[[nodiscard]] AhciReadDmaExtCommandPageWritableView MakeReadCommandPage(const Foundation::Size index) noexcept
{
    return AhciReadDmaExtCommandPageWritableView{
        .Data = g_readCommandPages[index].Bytes,
        .SizeBytes = sizeof(g_readCommandPages[index].Bytes),
        .PhysicalAddress = reinterpret_cast<UInt64>(g_readCommandPages[index].Bytes),
    };
}

[[nodiscard]] AhciReadDmaExtDataBufferSpan MakeReadDataBuffer(const Foundation::Size index) noexcept
{
    return AhciReadDmaExtDataBufferSpan{
        .PhysicalAddress = reinterpret_cast<UInt64>(g_readDataPages[index].Bytes),
        .SizeBytes = 512U,
    };
}

void WriteEcamDoubleWord(const Foundation::Size busIndex, const UInt8 device, const UInt8 function,
                         const Foundation::UInt16 registerOffset, const UInt32 value) noexcept
{
    const Foundation::Size offset = (busIndex * EcamBusSizeBytes) + (static_cast<Foundation::Size>(device) << 15U) +
                                    (static_cast<Foundation::Size>(function) << 12U) + registerOffset;
    g_ecam[offset] = static_cast<UInt8>(value & 0xFFU);
    g_ecam[offset + 1U] = static_cast<UInt8>((value >> 8U) & 0xFFU);
    g_ecam[offset + 2U] = static_cast<UInt8>((value >> 16U) & 0xFFU);
    g_ecam[offset + 3U] = static_cast<UInt8>((value >> 24U) & 0xFFU);
}

[[nodiscard]] UInt32 ReadEcamDoubleWord(const Foundation::Size busIndex, const UInt8 device, const UInt8 function,
                                        const Foundation::UInt16 registerOffset) noexcept
{
    const Foundation::Size offset = (busIndex * EcamBusSizeBytes) + (static_cast<Foundation::Size>(device) << 15U) +
                                    (static_cast<Foundation::Size>(function) << 12U) + registerOffset;
    return static_cast<UInt32>(g_ecam[offset]) | (static_cast<UInt32>(g_ecam[offset + 1U]) << 8U) |
           (static_cast<UInt32>(g_ecam[offset + 2U]) << 16U) | (static_cast<UInt32>(g_ecam[offset + 3U]) << 24U);
}

void AddEcamFunction(const Foundation::Size busIndex, const UInt8 device, const UInt8 function,
                     const Foundation::UInt16 vendorId, const Foundation::UInt16 deviceId, const UInt8 revisionId,
                     const UInt8 programmingInterface, const UInt8 subclass, const UInt8 classCode,
                     const UInt8 headerLayout, const bool multifunction) noexcept
{
    WriteEcamDoubleWord(busIndex, device, function, 0x00U,
                        static_cast<UInt32>(vendorId) | (static_cast<UInt32>(deviceId) << 16U));
    WriteEcamDoubleWord(busIndex, device, function, 0x08U,
                        static_cast<UInt32>(revisionId) | (static_cast<UInt32>(programmingInterface) << 8U) |
                            (static_cast<UInt32>(subclass) << 16U) | (static_cast<UInt32>(classCode) << 24U));
    const UInt8 headerType = static_cast<UInt8>(headerLayout | (multifunction ? 0x80U : 0U));
    WriteEcamDoubleWord(busIndex, device, function, 0x0CU, static_cast<UInt32>(headerType) << 16U);
}

void AddAhciEcamFunction(const UInt8 device, const UInt8 function, const UInt32 bar5,
                         const UInt32 command = DmaEnabledPciCommand) noexcept
{
    AddEcamFunction(0U, device, function, 0x1234U, 0x5678U, 1U, 1U, 6U, 1U, 0U, function != 0U);
    WriteEcamDoubleWord(0U, device, function, 0x04U, command);
    WriteEcamDoubleWord(0U, device, function, 0x24U, bar5);
}

[[nodiscard]] bool PrepareIdentifiedRuntime(RetainedRuntime& retained, const UInt8 device,
                                            AhciIdentifyCommandReceipt& receipt) noexcept
{
    FillEcam(0xFFU);
    InitializeIdentifyAbar();
    const UInt64 abarBase = reinterpret_cast<UInt64>(&g_abar[0U]);
    AddAhciEcamFunction(device, 0U, static_cast<UInt32>(abarBase));
    const auto prepared =
        retained.Runtime.Prepare(MakeBootInfo(reinterpret_cast<UInt64>(&g_ecam[0U])), MakeAhciResource(device, 0U), 32U,
                                 retained.Fixture.DomainIdentity, retained.Fixture.Domain);
    const AhciDmaPageWritableView dmaPage = MakeIdentifyDmaPage();
    AhciCompletionEmulatorClock completingClock{dmaPage};
    const auto identify = retained.Runtime.ExecuteAhciIdentify(completingClock, dmaPage,
                                                               AhciIdentifyPollingPolicy{
                                                                   .DeadlineTicks = 10'000U,
                                                                   .MaximumPollCount = 16U,
                                                                   .MaximumRecoveryPollCount = 16U,
                                                               });
    return prepared.HasValue() && (identify.Error == AhciIdentifyRuntimeError::None) &&
           completingClock.WasCommandObserved() &&
           (completingClock.GetValidationError() == AhciCompletionValidationError::None) &&
           completingClock.WasCompletionPublished() &&
           (retained.Runtime.GetAhciIdentifyReceipt(receipt) == AhciIdentifyRuntimeQueryError::None);
}

void TestMultifunctionDiscoveryAndPublication(TestContext& runner) noexcept
{
    FillEcam(0xFFU);
    constexpr UInt8 NetworkDevice{2U};
    constexpr UInt8 MultifunctionDevice{5U};
    constexpr UInt8 AhciFunction{3U};
    constexpr UInt32 InitialVendorDevice{0x100E'8086U};
    constexpr UInt32 AhciVendorDevice{0x2922'8086U};
    AddEcamFunction(0U, NetworkDevice, 0U, 0x8086U, 0x100EU, 2U, 0U, 0U, 2U, 0U, false);
    AddEcamFunction(0U, NetworkDevice, 1U, 0xDEADU, 0xBEEFU, 0U, 1U, 6U, 1U, 0U, false);
    AddEcamFunction(0U, MultifunctionDevice, 0U, 0x1234U, 0x1111U, 3U, 0x30U, 3U, 0x0CU, 0U, true);
    AddEcamFunction(0U, MultifunctionDevice, AhciFunction, 0x8086U, 0x2922U, 4U, 1U, 6U, 1U, 0U, false);
    const UInt64 baseAddress = reinterpret_cast<UInt64>(&g_ecam[0U]);
    RetainedRuntime retained = ConstructRetainedRuntime(0U, 1024U * 1024U);
    RuntimeFixture& fixture = retained.Fixture;
    PciEcamRuntime& runtime = retained.Runtime;
    UInt32 unpreparedOutput{0xA5A5'5A5AU};
    const auto unpreparedRead = runtime.Read32(PciFunctionAddress{7U, 0U, NetworkDevice, 0U}, 0U, unpreparedOutput);
    PciFunctionPublication untouchedPublication{};
    untouchedPublication.Descriptor.Address = PciFunctionAddress{0x55AAU, 0xA5U, 7U, 1U};
    untouchedPublication.Descriptor.VendorId = 0x1234U;
    untouchedPublication.Capability = Kernel::Object::CapabilityHandle{0x77U, 0x88U};
    const PciEcamFunctionQueryError unpreparedQuery = runtime.GetPublishedFunction(0U, untouchedPublication);
    ASTERIA_EXPECT(runner, "unprepared reads and publication queries leave outputs unchanged",
                   unpreparedRead == PciConfigurationReadError::NotReady, unpreparedOutput == 0xA5A5'5A5AU,
                   unpreparedQuery == PciEcamFunctionQueryError::NotReady,
                   untouchedPublication.Descriptor.VendorId == 0x1234U,
                   untouchedPublication.Capability.GetSelector() == 0x77U);
    const Boot::BootInfo bootInfo = MakeBootInfo(baseAddress);
    const auto prepared =
        runtime.Prepare(bootInfo, {}, g_profile->PhysicalAddressBits, fixture.DomainIdentity, fixture.Domain);
    UInt32 output{0U};
    const auto read = runtime.Read32(
        PciFunctionAddress{
            7U,
            0U,
            NetworkDevice,
            0U,
        },
        0U, output);
    PciEcamRuntimeSummary queriedSummary{};
    const auto summaryQuery = runtime.GetSummary(queriedSummary);
    PciFunctionPublication first{};
    PciFunctionPublication second{};
    PciFunctionPublication ahci{};
    const auto firstQuery = runtime.GetPublishedFunction(0U, first);
    const auto secondQuery = runtime.GetPublishedFunction(1U, second);
    const auto ahciQuery = runtime.GetPublishedFunction(2U, ahci);
    ASTERIA_EXPECT(runner, "the catalog publishes all visible functions and exact AHCI evidence", fixture.Ready,
                   prepared.HasValue(), prepared.GetValue().Availability == PciEcamAvailability::Ready,
                   prepared.GetValue().InitialFunctionPresent, prepared.GetValue().AhciFunctionPresent,
                   prepared.GetValue().BusCount == 1U, prepared.GetValue().DiscoveredFunctionCount == 3U,
                   prepared.GetValue().PublishedFunctionCount == 3U, prepared.GetValue().ConfigurationReadCount == 45U,
                   prepared.GetValue().InitialFunction.GetSegment() == 7U,
                   prepared.GetValue().InitialFunction.GetDevice() == NetworkDevice,
                   prepared.GetValue().InitialVendorDevice == InitialVendorDevice,
                   prepared.GetValue().AhciFunction.Address.IsEqualTo(PciFunctionAddress{
                       7U,
                       0U,
                       MultifunctionDevice,
                       AhciFunction,
                   }),
                   runtime.GetState() == PciEcamRuntimeState::Ready, read == PciConfigurationReadError::None,
                   output == InitialVendorDevice, summaryQuery == PciEcamFunctionQueryError::None,
                   queriedSummary.PublishedFunctionCount == 3U);

    const bool commonPublicationState =
        (firstQuery == PciEcamFunctionQueryError::None) && (secondQuery == PciEcamFunctionQueryError::None) &&
        (ahciQuery == PciEcamFunctionQueryError::None) && first.Function.IsValid() && second.Function.IsValid() &&
        ahci.Function.IsValid() && first.Capability.IsValid() && second.Capability.IsValid() &&
        ahci.Capability.IsValid() && first.CapabilitySpace.IsValid() &&
        (first.CapabilitySpace.GetValue() == second.CapabilitySpace.GetValue()) &&
        (second.CapabilitySpace.GetValue() == ahci.CapabilitySpace.GetValue());
    ASTERIA_EXPECT(
        runner, "published identities and internal-space handles match catalog order", commonPublicationState,
        first.Descriptor.Address.IsEqualTo(PciFunctionAddress{7U, 0U, NetworkDevice, 0U}),
        second.Descriptor.Multifunction,
        second.Descriptor.Address.IsEqualTo(PciFunctionAddress{7U, 0U, MultifunctionDevice, 0U}),
        ahci.Descriptor.Address.IsEqualTo(PciFunctionAddress{
            7U,
            0U,
            MultifunctionDevice,
            AhciFunction,
        }),
        ahci.Descriptor.VendorId == 0x8086U, ahci.Descriptor.DeviceId == 0x2922U, ahci.Descriptor.ClassCode == 1U,
        ahci.Descriptor.Subclass == 6U, ahci.Descriptor.ProgrammingInterface == 1U,
        (static_cast<UInt32>(ahci.Descriptor.VendorId) | (static_cast<UInt32>(ahci.Descriptor.DeviceId) << 16U)) ==
            AhciVendorDevice);

    const PciFunctionPublication beforeInvalid = untouchedPublication;
    const auto hiddenQuery = runtime.GetPublishedFunction(3U, untouchedPublication);
    ASTERIA_EXPECT(runner, "hidden non-multifunction functions stay absent and query is atomic",
                   hiddenQuery == PciEcamFunctionQueryError::IndexOutOfRange,
                   untouchedPublication.Descriptor.VendorId == beforeInvalid.Descriptor.VendorId,
                   untouchedPublication.Capability.GetSelector() == beforeInvalid.Capability.GetSelector());
}

void TestNoFunctionAndAbsentTopology(TestContext& runner) noexcept
{
    FillEcam(0xFFU);
    const UInt64 baseAddress = reinterpret_cast<UInt64>(&g_ecam[0U]);
    RetainedRuntime emptyRetained = ConstructRetainedRuntime(1U, 1024U * 1024U);
    RuntimeFixture& emptyFixture = emptyRetained.Fixture;
    PciEcamRuntime& emptyRuntime = emptyRetained.Runtime;
    const Boot::BootInfo emptyBootInfo = MakeBootInfo(baseAddress);
    const auto empty = emptyRuntime.Prepare(emptyBootInfo, {}, g_profile->PhysicalAddressBits,
                                            emptyFixture.DomainIdentity, emptyFixture.Domain);
    UInt32 output{0x1234'5678U};
    const auto invalid = emptyRuntime.Read32(PciFunctionAddress{7U, 0U, 0U, 0U}, 2U, output);
    PciFunctionPublication noFunctionOutput{};
    noFunctionOutput.Descriptor.VendorId = 0x5678U;
    noFunctionOutput.Capability = Kernel::Object::CapabilityHandle{0x99U, 0xAAU};
    const auto noFunctionQuery = emptyRuntime.GetPublishedFunction(0U, noFunctionOutput);
    ASTERIA_EXPECT(runner, "no function is ready and failed reads and queries preserve output", emptyFixture.Ready,
                   empty.HasValue(), empty.GetValue().Availability == PciEcamAvailability::NoFunctionPresent,
                   !empty.GetValue().InitialFunctionPresent, !empty.GetValue().AhciFunctionPresent,
                   empty.GetValue().BusCount == 1U, empty.GetValue().DiscoveredFunctionCount == 0U,
                   empty.GetValue().PublishedFunctionCount == 0U, empty.GetValue().ConfigurationReadCount == 32U,
                   empty.GetValue().InitialVendorDevice == 0xFFFF'FFFFU,
                   emptyRuntime.GetState() == PciEcamRuntimeState::Ready,
                   invalid == PciConfigurationReadError::InvalidAddress, output == 0x1234'5678U,
                   noFunctionQuery == PciEcamFunctionQueryError::IndexOutOfRange,
                   noFunctionOutput.Descriptor.VendorId == 0x5678U, noFunctionOutput.Capability.GetSelector() == 0x99U);

    RetainedRuntime absentRetained = ConstructRetainedRuntime(2U, 1024U * 1024U);
    RuntimeFixture& absentFixture = absentRetained.Fixture;
    PciEcamRuntime& absentRuntime = absentRetained.Runtime;
    const Boot::BootInfo absentBootInfo = MakeBootInfo(0U);
    const auto absent = absentRuntime.Prepare(absentBootInfo, {}, g_profile->PhysicalAddressBits,
                                              absentFixture.DomainIdentity, absentFixture.Domain);
    const auto unsupported = absentRuntime.Read32(PciFunctionAddress{0U, 0U, 0U, 0U}, 0U, output);
    PciEcamRuntimeSummary absentSummary{};
    const auto absentSummaryQuery = absentRuntime.GetSummary(absentSummary);
    const auto absentFunctionQuery = absentRuntime.GetPublishedFunction(0U, noFunctionOutput);
    ASTERIA_EXPECT(
        runner, "absent optional topology has an immutable empty summary", absentFixture.Ready, absent.HasValue(),
        absent.GetValue().Availability == PciEcamAvailability::Absent,
        absentRuntime.GetState() == PciEcamRuntimeState::Unavailable,
        unsupported == PciConfigurationReadError::Unsupported, output == 0x1234'5678U,
        absentSummaryQuery == PciEcamFunctionQueryError::None,
        absentSummary.Availability == PciEcamAvailability::Absent, absentSummary.DiscoveredFunctionCount == 0U,
        absentFunctionQuery == PciEcamFunctionQueryError::NotReady, noFunctionOutput.Descriptor.VendorId == 0x5678U);
    const auto absentScanout =
        absentRuntime.PublishScanoutRegion(reinterpret_cast<UInt64>(&g_abar[0U]), 4096U, g_profile->PhysicalAddressBits,
                                           absentFixture.DomainIdentity, absentFixture.Domain);
    ASTERIA_EXPECT(runner, "a machine without ECAM still publishes its scanout", absentScanout.HasValue());
}

void TestProbeRangeAndBusBoundary(TestContext& runner) noexcept
{
    FillEcam(0xFFU);
    constexpr UInt8 PresentDevice{PciFunctionAddress::MaximumDevice};
    constexpr UInt32 VendorDevice{0x1234'ABCDU};
    AddEcamFunction(1U, PresentDevice, 0U, 0xABCDU, 0x1234U, 1U, 0U, 0U, 2U, 0U, false);
    const UInt64 physicalStart = reinterpret_cast<UInt64>(&g_ecam[0U]);
    const Boot::BootInfo bootInfo = MakeBoundaryBootInfo(physicalStart);

    RetainedRuntime presentRetained = ConstructRetainedRuntime(3U, 1024U * 1024U);
    RuntimeFixture& presentFixture = presentRetained.Fixture;
    PciEcamRuntime& presentRuntime = presentRetained.Runtime;
    const auto present = presentRuntime.Prepare(bootInfo, {}, g_profile->PhysicalAddressBits,
                                                presentFixture.DomainIdentity, presentFixture.Domain);
    ASTERIA_EXPECT(
        runner, "discovery advances across ranges to device 31 on bus 255", presentFixture.Ready, present.HasValue(),
        present.GetValue().Availability == PciEcamAvailability::Ready, present.GetValue().RegionCount == 2U,
        present.GetValue().BusCount == 2U, present.GetValue().DiscoveredFunctionCount == 1U,
        present.GetValue().PublishedFunctionCount == 1U, present.GetValue().ConfigurationReadCount == 66U,
        present.GetValue().InitialFunction.GetSegment() == 7U, present.GetValue().InitialFunction.GetBus() == 255U,
        present.GetValue().InitialFunction.GetDevice() == PresentDevice,
        present.GetValue().InitialFunction.GetFunction() == 0U, present.GetValue().InitialVendorDevice == VendorDevice);

    FillEcam(0xFFU);
    RetainedRuntime emptyRetained = ConstructRetainedRuntime(4U, 1024U * 1024U);
    RuntimeFixture& emptyFixture = emptyRetained.Fixture;
    PciEcamRuntime& emptyRuntime = emptyRetained.Runtime;
    const auto empty = emptyRuntime.Prepare(bootInfo, {}, g_profile->PhysicalAddressBits, emptyFixture.DomainIdentity,
                                            emptyFixture.Domain);
    ASTERIA_EXPECT(runner, "discovery terminates after the last device of bus 255", emptyFixture.Ready,
                   empty.HasValue(), empty.GetValue().Availability == PciEcamAvailability::NoFunctionPresent,
                   emptyRuntime.GetState() == PciEcamRuntimeState::Ready);
}

// The runtime takes its physical-address limit from the ISA access: one bit
// wider than the access names leaves every ECAM aperture unimplemented, so
// the scan never starts and the AHCI binding is not evaluated.
void TestWidthBeyondAccessIsUnsupported(TestContext& runner) noexcept
{
    FillEcam(0xFFU);
    AddEcamFunction(0U, 0U, 0U, 0xABCDU, 0x1234U, 1U, 0U, 0U, 2U, 0U, false);
    RetainedRuntime retained = ConstructRetainedRuntime(41U, 1024U * 1024U);
    const auto prepared = retained.Runtime.Prepare(MakeBootInfo(reinterpret_cast<UInt64>(&g_ecam[0U])), {},
                                                   static_cast<UInt8>(g_profile->PhysicalAddressBits + 1U),
                                                   retained.Fixture.DomainIdentity, retained.Fixture.Domain);
    ASTERIA_EXPECT(runner, "a physical-address width beyond the ISA access leaves ECAM unsupported",
                   retained.Fixture.Ready, prepared.HasValue(),
                   prepared.GetValue().Availability == PciEcamAvailability::UnsupportedPhysicalRange,
                   prepared.GetValue().ConfigurationReadCount == 0U,
                   retained.Runtime.GetState() == PciEcamRuntimeState::Unavailable,
                   retained.Runtime.GetAhciAvailability() == AhciPciAvailability::EcamUnavailable);
}

void TestFunctionCapacityPreflight(TestContext& runner) noexcept
{
    FillEcam(0xFFU);
    for (UInt8 device{0U}; device <= PciFunctionAddress::MaximumDevice; ++device)
    {
        for (UInt8 function{0U}; function <= PciFunctionAddress::MaximumFunction; ++function)
        {
            AddEcamFunction(0U, device, function, 0x1000U, 0x2000U, 2U, 0U, 0U, 2U, 0U, function == 0U);
        }
    }
    RetainedRuntime retained = ConstructRetainedRuntime(5U, 4U * 1024U * 1024U);
    const auto prepared = retained.Runtime.Prepare(MakeBootInfo(reinterpret_cast<UInt64>(&g_ecam[0U])), {},
                                                   g_profile->PhysicalAddressBits, retained.Fixture.DomainIdentity,
                                                   retained.Fixture.Domain);
    PciFunctionPublication last{};
    ASTERIA_EXPECT(runner, "all 256 catalog functions fit manager, registry, capabilities and metadata budget",
                   retained.Fixture.Ready, prepared.HasValue(), prepared.GetValue().PublishedFunctionCount == 256U,
                   retained.Runtime.GetPublishedFunction(255U, last) == PciEcamFunctionQueryError::None,
                   last.Function.IsValid(), last.Capability.IsValid(),
                   retained.Fixture.Registry.GetActiveCount() == 258U);
    const auto scanout = retained.Runtime.PublishScanoutRegion(
        reinterpret_cast<UInt64>(&g_abar[0U]), 4096U, g_profile->PhysicalAddressBits, retained.Fixture.DomainIdentity,
        retained.Fixture.Domain);
    ASTERIA_EXPECT(runner, "region publication keeps headroom when every PCI capability slot is occupied",
                   scanout.HasValue(), retained.Fixture.Registry.GetActiveCount() == 259U);
    Kernel::Object::ObjectPin scanoutPin;
    ASTERIA_EXPECT(runner, "scanout publication prefers WC and permits UC fallback",
                   retained.Fixture.Registry.PinObject(scanout.GetValue(), Kernel::Object::ObjectType::DeviceRegion,
                                                       scanoutPin) == Kernel::Object::ObjectPinError::Ok,
                   static_cast<Kernel::Device::DeviceRegion&>(scanoutPin.GetObject()).GetMappingCachePolicy() ==
                       Kernel::Memory::MemoryCachePolicy::WriteCombining);
    ASTERIA_EXPECT(runner, "scanout publication refuses RAM cache policies",
                   !retained.Runtime
                        .PublishScanoutRegion(reinterpret_cast<UInt64>(&g_abar[0U]), 4096U,
                                              g_profile->PhysicalAddressBits, retained.Fixture.DomainIdentity,
                                              retained.Fixture.Domain, Kernel::Memory::MemoryCachePolicy::WriteBack)
                        .HasValue());
}

void TestPartialPublicationIsNeverReady(TestContext& runner) noexcept
{
    FillEcam(0xFFU);
    AddEcamFunction(0U, 0U, 0U, 0x1111U, 0x2222U, 1U, 0U, 0U, 2U, 0U, false);
    AddEcamFunction(0U, 1U, 0U, 0x3333U, 0x4444U, 2U, 0U, 0U, 2U, 0U, false);

    constexpr UInt64 metadataLimit{sizeof(DeviceRegion) + sizeof(PciFunction)};
    const UInt64 baseAddress = reinterpret_cast<UInt64>(&g_ecam[0U]);
    RetainedRuntime retained = ConstructRetainedRuntime(6U, metadataLimit);
    RuntimeFixture& fixture = retained.Fixture;
    PciEcamRuntime& runtime = retained.Runtime;
    const Boot::BootInfo bootInfo = MakeBootInfo(baseAddress);
    const auto prepared =
        runtime.Prepare(bootInfo, {}, g_profile->PhysicalAddressBits, fixture.DomainIdentity, fixture.Domain);
    const auto failure = runtime.GetFailure();
    const auto domain = fixture.Domain.GetSnapshot();
    PciFunctionPublication output{};
    output.Descriptor.VendorId = 0xBEEFU;
    output.Capability = Kernel::Object::CapabilityHandle{0x56U, 0x78U};
    const auto query = runtime.GetPublishedFunction(0U, output);
    UInt32 readOutput{0x9876'5432U};
    const auto read = runtime.Read32(PciFunctionAddress{7U, 0U, 0U, 0U}, 0U, readOutput);
    ASTERIA_EXPECT(runner, "a later manager failure retains ownership without publishing Ready", fixture.Ready,
                   !prepared.HasValue(), prepared.GetError() == PciEcamRuntimeError::FunctionPublicationRejected,
                   runtime.GetState() == PciEcamRuntimeState::Failed, failure.PublishedFunctionCount == 1U,
                   failure.PciFunctionManager.Error == PciFunctionManagerError::FunctionRejected,
                   failure.PciFunctionManager.FunctionFailure.Error ==
                       Kernel::Device::PciFunctionError::ResourceDomainRejected,
                   failure.PciFunctionManager.FunctionFailure.Resource == ResourceDomainError::LimitExceeded,
                   fixture.Registry.GetActiveCount() == 3U, domain.Usage.KernelMetadataBytes == metadataLimit,
                   query == PciEcamFunctionQueryError::NotReady, output.Descriptor.VendorId == 0xBEEFU,
                   output.Capability.GetSelector() == 0x56U, read == PciConfigurationReadError::NotReady,
                   readOutput == 0x9876'5432U);
}

void TestTerminalOwnershipFailure(TestContext& runner) noexcept
{
    FillEcam(0xFFU);
    const UInt64 baseAddress = reinterpret_cast<UInt64>(&g_ecam[0U]);
    RetainedRuntime retained = ConstructRetainedRuntime(7U, 1U);
    RuntimeFixture& fixture = retained.Fixture;
    PciEcamRuntime& runtime = retained.Runtime;
    const Boot::BootInfo bootInfo = MakeBootInfo(baseAddress);
    const auto prepared =
        runtime.Prepare(bootInfo, {}, g_profile->PhysicalAddressBits, fixture.DomainIdentity, fixture.Domain);
    UInt32 output{0x89AB'CDEFU};
    const auto read = runtime.Read32(PciFunctionAddress{7U, 0U, 0U, 0U}, 0U, output);
    ASTERIA_EXPECT(runner, "ownership failure is terminal and leaves later read output unchanged", fixture.Ready,
                   !prepared.HasValue(), prepared.GetError() == PciEcamRuntimeError::RegionRejected,
                   runtime.GetState() == PciEcamRuntimeState::Failed, read == PciConfigurationReadError::NotReady,
                   output == 0x89AB'CDEFU);
}

void TestAhciAuthoritativePublicationAndBackendSeparation(TestContext& runner) noexcept
{
    FillEcam(0xFFU);
    InitializeSuccessfulAbar();
    constexpr UInt8 AhciDevice{6U};
    const UInt64 abarBase = reinterpret_cast<UInt64>(&g_abar[0U]);
    AddAhciEcamFunction(AhciDevice, 0U, static_cast<UInt32>(abarBase), DmaEnabledPciCommand);

    RetainedRuntime retained = ConstructRetainedRuntime(8U, 1024U * 1024U);
    RuntimeFixture& fixture = retained.Fixture;
    PciEcamRuntime& runtime = retained.Runtime;
    AhciPciRuntimeResult result{};
    result.Region.BaseAddress = 0xA5A5'A5A5U;
    const auto beforePrepare = runtime.GetAhciResult(result);
    const auto prepared =
        runtime.Prepare(MakeBootInfo(reinterpret_cast<UInt64>(&g_ecam[0U])), MakeAhciResource(AhciDevice, 0U), 32U,
                        fixture.DomainIdentity, fixture.Domain);
    const auto query = runtime.GetAhciResult(result);
    UInt32 ecamIdentifier{0U};
    const auto ecamRead = runtime.Read32(PciFunctionAddress{7U, 0U, AhciDevice, 0U}, 0x00U, ecamIdentifier);
    const auto domain = fixture.Domain.GetSnapshot();

    ASTERIA_EXPECT(runner, "a unique captured AHCI endpoint publishes independently after ECAM becomes ready",
                   abarBase <= 0xFFFF'FFFFU, g_abar != MAP_FAILED, (abarBase & (AbarSizeBytes - 1U)) == 0U,
                   beforePrepare == AhciPciRuntimeQueryError::NotReady, result.Region.BaseAddress != 0xA5A5'A5A5U,
                   prepared.HasValue(), runtime.GetState() == PciEcamRuntimeState::Ready,
                   runtime.GetAhciAvailability() == AhciPciAvailability::Ready,
                   query == AhciPciRuntimeQueryError::None);
    ASTERIA_EXPECT(runner, "the binding cross-checks BDF, exact BAR5 geometry, and the ABAR snapshot",
                   result.Binding.Function.Address.IsEqualTo(PciFunctionAddress{7U, 0U, AhciDevice, 0U}),
                   result.Binding.Abar.BaseAddress == abarBase, result.Binding.Abar.ExtentBytes == AbarSizeBytes,
                   result.Binding.Abar.BarIndex == 5U, result.Binding.Abar.RegisterCount == 1U,
                   !result.Binding.Abar.Prefetchable, result.Binding.Snapshot.Capabilities == 0xCC32'1F03U,
                   result.Binding.Snapshot.GlobalHostControl == 0x8000'0002U,
                   result.Binding.Snapshot.InterruptStatus == 5U, result.Binding.Snapshot.PortsImplemented == 0xBU,
                   result.Binding.Snapshot.Version == 0x0001'0301U);
    ASTERIA_EXPECT(runner, "ABAR authority is one exact read-write DWORD-only uncached DeviceRegion",
                   result.Region.BaseAddress == abarBase, result.Region.LengthBytes == AbarSizeBytes,
                   result.Region.Kind == DeviceRegionKind::MemoryMappedIo,
                   result.Region.Permissions.Contains(DeviceRegionPermission::Read),
                   result.Region.Permissions.Contains(DeviceRegionPermission::Write),
                   result.Region.Widths.Contains(DeviceAccessWidth::DoubleWord),
                   !result.Region.Widths.Contains(DeviceAccessWidth::Byte),
                   !result.Region.Widths.Contains(DeviceAccessWidth::Word),
                   result.Region.CachePolicy == Kernel::Memory::MemoryCachePolicy::Uncached,
                   domain.Usage.KernelMetadataBytes == ((2U * sizeof(DeviceRegion)) + sizeof(PciFunction)));
    ASTERIA_EXPECT(
        runner, "the ECAM backend remains readable and distinct from the one-region ABAR backend",
        ecamRead == PciConfigurationReadError::None, ecamIdentifier == 0x5678'1234U,
        runtime.GetFailure().MemoryMappedIo == Platform::Device::PhysicalMemoryMappedIoInitializationError::None,
        runtime.GetAhciFailure().MemoryMappedIo == Platform::Device::PhysicalMemoryMappedIoInitializationError::None);

    AhciPciRuntimeResult immutable{};
    const auto repeated = runtime.Prepare(MakeBootInfo(reinterpret_cast<UInt64>(&g_ecam[0U])), {}, 32U,
                                          fixture.DomainIdentity, fixture.Domain);
    const auto immutableQuery = runtime.GetAhciResult(immutable);
    ASTERIA_EXPECT(runner, "successful AHCI publication is immutable across a repeated Prepare", !repeated.HasValue(),
                   repeated.GetError() == PciEcamRuntimeError::AlreadyPrepared,
                   immutableQuery == AhciPciRuntimeQueryError::None, immutable.Region.BaseAddress == abarBase,
                   immutable.Binding.Snapshot.Version == 0x0001'0301U);
}

// An assignable device's captured BAR becomes a read-write uncached DeviceRegion once it decodes to
// exactly that range on a published function that decodes memory; anything else is refused.
void TestAssignableDevicePublication(TestContext& runner) noexcept
{
    FillEcam(0xFFU);
    InitializeSuccessfulAbar();
    constexpr UInt8 DeviceNumber{6U};
    const UInt64 abarBase = reinterpret_cast<UInt64>(&g_abar[0U]);
    AddAhciEcamFunction(DeviceNumber, 0U, static_cast<UInt32>(abarBase), DmaEnabledPciCommand);
    RetainedRuntime retained = ConstructRetainedRuntime(8U, 1024U * 1024U);
    RuntimeFixture& fixture = retained.Fixture;
    PciEcamRuntime& runtime = retained.Runtime;
    const auto unprepared = runtime.PublishAssignableDevice(MakeAhciResource(DeviceNumber, 0U), 32U,
                                                            fixture.DomainIdentity, fixture.Domain);
    const auto prepared = runtime.Prepare(MakeBootInfo(reinterpret_cast<UInt64>(&g_ecam[0U])), {}, 32U,
                                          fixture.DomainIdentity, fixture.Domain);
    const auto before = fixture.Domain.GetSnapshot().Usage.KernelMetadataBytes;
    const auto published = runtime.PublishAssignableDevice(MakeAhciResource(DeviceNumber, 0U), 32U,
                                                           fixture.DomainIdentity, fixture.Domain);
    const auto moved = runtime.PublishAssignableDevice(MakeAhciResource(DeviceNumber, 0U, abarBase + 0x1000U), 32U,
                                                       fixture.DomainIdentity, fixture.Domain);
    const auto absent = runtime.PublishAssignableDevice(MakeAhciResource(DeviceNumber + 1U, 0U), 32U,
                                                        fixture.DomainIdentity, fixture.Domain);
    PciFunctionPublication publication{};
    const bool listed = runtime.GetPublishedFunction(0U, publication) == PciEcamFunctionQueryError::None;
    ASTERIA_EXPECT(runner, "an assignable device's BAR becomes a DeviceRegion beside its published function",
                   !unprepared.HasValue() && (unprepared.GetError() == PciEcamRuntimeError::InvalidConfiguration),
                   prepared.HasValue(), published.HasValue(), listed,
                   published.HasValue() &&
                       (published.GetValue().Function.GetValue() == publication.Function.GetObjectId().GetValue()),
                   published.HasValue() && published.GetValue().Region.IsValid(),
                   fixture.Domain.GetSnapshot().Usage.KernelMetadataBytes == (before + sizeof(DeviceRegion)),
                   !moved.HasValue() && (moved.GetError() == PciEcamRuntimeError::DeviceRejected),
                   !absent.HasValue() && (absent.GetError() == PciEcamRuntimeError::DeviceRejected));
}

// Firmware may leave a 64-bit Intel xHCI BAR decoding above 4 GiB and
// bus mastering enabled. Publication consumes the authoritative captured
// extent without probing the running controller or truncating its address.
void TestWideXhciPublication(TestContext& runner) noexcept
{
    FillEcam(0xFFU);
    constexpr UInt8 device{20U};
    constexpr UInt64 base{0x30'142E'0000ULL};
    constexpr UInt64 bytes{0x10000U};
    AddEcamFunction(0U, device, 0U, 0x8086U, 0xA87DU, 1U, 0x30U, 3U, 12U, 0U, false);
    WriteEcamDoubleWord(0U, device, 0U, 4U, DmaEnabledPciCommand);
    WriteEcamDoubleWord(0U, device, 0U, 0x10U, static_cast<UInt32>(base & 0xFFFF'FFFFU) | 4U);
    WriteEcamDoubleWord(0U, device, 0U, 0x14U, static_cast<UInt32>(base >> 32U));
    RetainedRuntime retained = ConstructRetainedRuntime(8U, 1024U * 1024U);
    const auto prepared = retained.Runtime.Prepare(MakeBootInfo(reinterpret_cast<UInt64>(&g_ecam[0U])), {},
                                                   g_profile->PhysicalAddressBits, retained.Fixture.DomainIdentity,
                                                   retained.Fixture.Domain);
    const AhciPciResource resource{base, bytes, 7U, 0U, device, 0U, 0U, {0U, 0U}};
    const auto published = retained.Runtime.PublishAssignableDevice(
        resource, g_profile->PhysicalAddressBits, retained.Fixture.DomainIdentity, retained.Fixture.Domain);
    Kernel::Object::ObjectPin pin;
    const bool pinned =
        published.HasValue() &&
        retained.Fixture.Registry.PinObject(published.GetValue().Region, Kernel::Object::ObjectType::DeviceRegion,
                                            pin) == Kernel::Object::ObjectPinError::Ok;
    ASTERIA_EXPECT(runner, "Intel high BAR is published without disturbing firmware command state", prepared.HasValue(),
                   published.HasValue(), pinned, ReadEcamDoubleWord(0U, device, 0U, 4U) == DmaEnabledPciCommand,
                   ReadEcamDoubleWord(0U, device, 0U, 0x14U) == 0x30U);
    if (pinned)
    {
        const auto region = static_cast<DeviceRegion&>(pin.GetObject()).GetSnapshot();
        ASTERIA_EXPECT(runner, "uncached device aperture retains the full 64-bit address",
                       region.Descriptor.BaseAddress == base, region.Descriptor.LengthBytes == bytes,
                       static_cast<DeviceRegion&>(pin.GetObject()).GetMappingCachePolicy() ==
                           Kernel::Memory::MemoryCachePolicy::Uncached);
    }
    const auto narrow = retained.Runtime.PublishAssignableDevice(resource, 32U, retained.Fixture.DomainIdentity,
                                                                 retained.Fixture.Domain);
    ASTERIA_EXPECT(runner, "physical width cannot silently truncate a high aperture", !narrow.HasValue());
}

// Where no firmware reports an assignable device's BAR, the runtime finds the published function by
// Its vendor and device, sizes the BAR through configuration space (this ECAM is plain memory,
// which reads all ones back: a 16-byte BAR) and disables its INTx.
void TestAssignableResourceCapture(TestContext& runner) noexcept
{
    FillEcam(0xFFU);
    InitializeSuccessfulAbar();
    constexpr UInt8 DeviceNumber{6U};
    const UInt64 abarBase = reinterpret_cast<UInt64>(&g_abar[0U]);
    AddAhciEcamFunction(DeviceNumber, 0U, static_cast<UInt32>(abarBase), DmaEnabledPciCommand);
    RetainedRuntime retained = ConstructRetainedRuntime(8U, 1024U * 1024U);
    RuntimeFixture& fixture = retained.Fixture;
    PciEcamRuntime& runtime = retained.Runtime;
    const auto unprepared = runtime.CaptureAssignableResource(0x5678'1234U, 5U);
    const auto prepared = runtime.Prepare(MakeBootInfo(reinterpret_cast<UInt64>(&g_ecam[0U])), {}, 32U,
                                          fixture.DomainIdentity, fixture.Domain);
    const auto captured = runtime.CaptureAssignableResource(0x5678'1234U, 5U);
    const auto absent = runtime.CaptureAssignableResource(0x11E8'1234U, 0U);
    const auto unsized = runtime.CaptureAssignableResource(0x5678'1234U, 0U);
    const UInt64 command = ReadEcamDoubleWord(0U, DeviceNumber, 0U, 0x04U);
    const UInt64 bar = ReadEcamDoubleWord(0U, DeviceNumber, 0U, 0x24U);
    ASTERIA_EXPECT(runner, "an assignable device's BAR is found and sized through configuration space",
                   !unprepared.HasValue() && (unprepared.GetError() == PciEcamRuntimeError::InvalidConfiguration),
                   prepared.HasValue(), captured.HasValue(),
                   captured.HasValue() && (captured.GetValue().AbarBaseAddress == static_cast<UInt32>(abarBase)) &&
                       (captured.GetValue().AbarLengthBytes == 16U) && (captured.GetValue().Device == DeviceNumber) &&
                       (captured.GetValue().Bus == 0U) && (captured.GetValue().BarIndex == 5U),
                   !absent.HasValue() && (absent.GetError() == PciEcamRuntimeError::DeviceRejected),
                   !unsized.HasValue() && (unsized.GetError() == PciEcamRuntimeError::DeviceRejected),
                   command == (DmaEnabledPciCommand | 0x400U), bar == static_cast<UInt32>(abarBase));
}

// The device's INTx line is its own once every other function routed to the same line has INTx
// Disable; a function on another line, or one with no pin, is left alone.
void TestInterruptLineIsolation(TestContext& runner) noexcept
{
    FillEcam(0xFFU);
    InitializeSuccessfulAbar();
    const UInt64 abarBase = reinterpret_cast<UInt64>(&g_abar[0U]);
    AddAhciEcamFunction(0U, 0U, static_cast<UInt32>(abarBase), DmaEnabledPciCommand);
    // Three more functions of no class the runtime binds.
    constexpr UInt8 Others[]{4U, 5U, 6U};
    for (const UInt8 device : Others)
    {
        AddEcamFunction(0U, device, 0U, 0x1000U, 0x0012U, 1U, 0U, 0U, 0xFFU, 0U, false);
        WriteEcamDoubleWord(0U, device, 0U, 0x04U, DmaEnabledPciCommand);
    }
    // Pin A on line 16 for the device and a sharer, line 17 for another, no
    // pin on the fourth.
    WriteEcamDoubleWord(0U, 0U, 0U, 0x3CU, 0x0000'0110U);
    WriteEcamDoubleWord(0U, 4U, 0U, 0x3CU, 0x0000'0110U);
    WriteEcamDoubleWord(0U, 5U, 0U, 0x3CU, 0x0000'0111U);
    WriteEcamDoubleWord(0U, 6U, 0U, 0x3CU, 0x0000'0010U);
    RetainedRuntime retained = ConstructRetainedRuntime(8U, 1024U * 1024U);
    RuntimeFixture& fixture = retained.Fixture;
    PciEcamRuntime& runtime = retained.Runtime;
    const auto prepared = runtime.Prepare(MakeBootInfo(reinterpret_cast<UInt64>(&g_ecam[0U])), {}, 32U,
                                          fixture.DomainIdentity, fixture.Domain);
    // The published functions' addresses carry the region's first bus.
    Kernel::Device::PciFunctionAddress device{};
    Kernel::Device::PciFunctionAddress unpinned{};
    for (Foundation::Size index{0U}; index < 4U; ++index)
    {
        PciFunctionPublication publication{};
        if (runtime.GetPublishedFunction(index, publication) == PciEcamFunctionQueryError::None)
        {
            device = (publication.Descriptor.Address.GetDevice() == 0U) ? publication.Descriptor.Address : device;
            unpinned = (publication.Descriptor.Address.GetDevice() == 6U) ? publication.Descriptor.Address : unpinned;
        }
    }
    const auto isolated = runtime.IsolateInterruptLine(device);
    const auto pinless = runtime.IsolateInterruptLine(unpinned);

    ASTERIA_EXPECT(runner, "the device's line is its own: a sharer's INTx is disabled, others are left alone",
                   prepared.HasValue(), isolated.HasValue() && (isolated.GetValue() == 16U),
                   ReadEcamDoubleWord(0U, 0U, 0U, 0x04U) == DmaEnabledPciCommand,
                   ReadEcamDoubleWord(0U, 4U, 0U, 0x04U) == (DmaEnabledPciCommand | 0x400U),
                   ReadEcamDoubleWord(0U, 5U, 0U, 0x04U) == DmaEnabledPciCommand,
                   ReadEcamDoubleWord(0U, 6U, 0U, 0x04U) == DmaEnabledPciCommand,
                   !pinless.HasValue() && (pinless.GetError() == PciEcamRuntimeError::DeviceRejected));
}

/// A USB host controller function of class 0C/03 with interface on bus 0,
/// decoding as command says, with value in the BAR register at bar.
void AddUsbFunction(const UInt8 device, const UInt8 function, const UInt8 interface, const UInt32 command,
                    const UInt16 bar, const UInt32 value, const bool multifunction = false) noexcept
{
    AddEcamFunction(0U, device, function, 0x8086U, static_cast<UInt16>(0x3A30U + interface), 1U, interface, 3U, 0x0CU,
                    0U, multifunction);
    WriteEcamDoubleWord(0U, device, function, 0x04U, command);
    WriteEcamDoubleWord(0U, device, function, bar, value);
}

/// The descriptor of the DeviceRegion region names; zero when it names none.
[[nodiscard]] Kernel::Device::DeviceRegionDescriptor RegionOf(RuntimeFixture& fixture,
                                                              const Kernel::Object::ObjectId region) noexcept
{
    Kernel::Object::ObjectPin pin;
    if (fixture.Registry.PinObject(region, Kernel::Object::ObjectType::DeviceRegion, pin) !=
        Kernel::Object::ObjectPinError::Ok)
    {
        return {};
    }
    return static_cast<DeviceRegion&>(pin.GetObject()).GetSnapshot().Descriptor;
}

// UsbHostDriver.md: a USB 1.1/2.0 host controller's registers become a
// DeviceRegion beside its function. An EHCI's 1 KiB memory BAR 0 at a page
// offset is published as its whole page unless another function's memory BAR
// starts in that page; an OHCI's 4 KiB BAR as decoded; a UHCI's I/O BAR 4 as
// 32 ports once the function decodes I/O. Publication disables INTx.
void TestUsbHostDevicePublication(TestContext& runner) noexcept
{
    FillEcam(0xFFU);
    constexpr UInt8 Ehci{8U};
    constexpr UInt8 Ohci{9U};
    constexpr UInt8 Uhci{10U};
    constexpr UInt8 Smbus{11U};
    constexpr UInt8 Bridge{12U};
    constexpr UInt8 Xhci{13U};
    constexpr UInt32 IoCommand{0x05U};
    constexpr UInt32 EveryDecodeCommand{0x07U};
    AddUsbFunction(Ehci, 0U, 0x20U, EveryDecodeCommand, 0x10U, 0xFEB0'0400U);
    AddUsbFunction(Ohci, 0U, 0x10U, DmaEnabledPciCommand, 0x10U, 0xFEB1'0000U);
    AddUsbFunction(Uhci, 0U, 0x00U, DmaEnabledPciCommand, 0x20U, 0x0000'E001U);
    AddUsbFunction(Xhci, 0U, 0x30U, DmaEnabledPciCommand, 0x10U, 0xFEB3'0000U);
    // Neighbours whose registers may start in the EHCI's page: an SMBus
    // controller (class 0C/05, its I/O BAR 4 decoded as a UHCI's would be)
    // and a PCI-to-PCI bridge (header type 1).
    AddEcamFunction(0U, Smbus, 0U, 0x8086U, 0x3A60U, 0U, 0U, 5U, 0x0CU, 0U, false);
    WriteEcamDoubleWord(0U, Smbus, 0U, 0x04U, IoCommand);
    WriteEcamDoubleWord(0U, Smbus, 0U, 0x20U, 0x0000'EFA1U);
    AddEcamFunction(0U, Bridge, 0U, 0x8086U, 0x3A70U, 0U, 0U, 4U, 6U, 1U, false);
    RetainedRuntime retained = ConstructRetainedRuntime(44U, 1024U * 1024U);
    RuntimeFixture& fixture = retained.Fixture;
    PciEcamRuntime& runtime = retained.Runtime;
    const UInt8 bits = g_profile->PhysicalAddressBits;
    const PciFunctionAddress ehci{7U, 0U, Ehci, 0U};
    const AhciPciResource ehciBar{0xFEB0'0400U, 0x400U, 7U, 0U, Ehci, 0U, 0U, {0U, 0U}};
    const AhciPciResource ohciBar{0xFEB1'0000U, 0x1000U, 7U, 0U, Ohci, 0U, 0U, {0U, 0U}};
    const auto publish = [&](const UInt8 device, const AhciPciResource* const bar) noexcept
    {
        return runtime.PublishUsbHostDevice(PciFunctionAddress{7U, 0U, device, 0U}, bar, bits, fixture.DomainIdentity,
                                            fixture.Domain);
    };
    const auto rejected =
        [](const Foundation::Result<Platform::Device::AssignableDevice, PciEcamRuntimeError>& result) noexcept
    {
        return !result.HasValue() && (result.GetError() == PciEcamRuntimeError::DeviceRejected);
    };
    // Whether the EHCI is refused while value is at offset of device's
    // Configuration space, which reads all ones again afterwards.
    const auto refusedWith = [&](const UInt8 device, const UInt16 offset, const UInt32 value) noexcept
    {
        WriteEcamDoubleWord(0U, device, 0U, offset, value);
        const bool refused = rejected(publish(Ehci, &ehciBar));
        WriteEcamDoubleWord(0U, device, 0U, offset, 0xFFFF'FFFFU);
        return refused;
    };
    const auto unprepared = publish(Ehci, &ehciBar);
    const auto prepared = runtime.Prepare(MakeBootInfo(reinterpret_cast<UInt64>(&g_ecam[0U])), {}, bits,
                                          fixture.DomainIdentity, fixture.Domain);
    ASTERIA_EXPECT(runner, "a memory BAR of another function starting in the EHCI's page refuses it",
                   !unprepared.HasValue() && (unprepared.GetError() == PciEcamRuntimeError::InvalidConfiguration),
                   prepared.HasValue(), refusedWith(Smbus, 0x10U, 0xFEB0'0000U),
                   refusedWith(Smbus, 0x24U, 0xFEB0'0FF4U), refusedWith(Bridge, 0x14U, 0xFEB0'0A00U),
                   ReadEcamDoubleWord(0U, Ehci, 0U, 0x04U) == EveryDecodeCommand);
    // Starting in the page is what counts, and only for a memory BAR: not an
    // I/O BAR, a BAR at the next page, a 64-bit BAR whose high dword moves it
    // above 4 GiB, the CardBus CIS pointer after BAR 5, nor a bridge's bus
    // numbers after its two BARs. (A 64-bit claim in BAR 5 above has no high
    // dword to move it.)
    WriteEcamDoubleWord(0U, Smbus, 0U, 0x10U, 0xFEB0'0F01U);
    WriteEcamDoubleWord(0U, Smbus, 0U, 0x14U, 0xFEB0'1000U);
    WriteEcamDoubleWord(0U, Smbus, 0U, 0x18U, 0xFEB0'0804U);
    WriteEcamDoubleWord(0U, Smbus, 0U, 0x1CU, 0xFEB0'0000U);
    WriteEcamDoubleWord(0U, Smbus, 0U, 0x28U, 0xFEB0'0D00U);
    WriteEcamDoubleWord(0U, Bridge, 0U, 0x10U, 0xFEB0'0B04U);
    WriteEcamDoubleWord(0U, Bridge, 0U, 0x14U, 0x0000'0001U);
    WriteEcamDoubleWord(0U, Bridge, 0U, 0x18U, 0xFEB0'0900U);
    const auto whole = publish(Ehci, &ehciBar);
    const Kernel::Device::DeviceRegionDescriptor page =
        RegionOf(fixture, whole.HasValue() ? whole.GetValue().Region : Kernel::Object::ObjectId{});
    ASTERIA_EXPECT(runner, "an EHCI's sub-page BAR is its whole page, with INTx disabled", whole.HasValue(),
                   page.BaseAddress == 0xFEB0'0000U, page.LengthBytes == 0x1000U,
                   page.Kind == DeviceRegionKind::MemoryMappedIo,
                   ReadEcamDoubleWord(0U, Ehci, 0U, 0x04U) == (EveryDecodeCommand | 0x400U));

    const auto decoded = publish(Ohci, &ohciBar);
    const Kernel::Device::DeviceRegionDescriptor registers =
        RegionOf(fixture, decoded.HasValue() ? decoded.GetValue().Region : Kernel::Object::ObjectId{});
    // A UHCI's BAR 4 must be an I/O BAR with a base in the 64 KiB port space,
    // decoded by the function.
    const auto uhciWith = [&](const UInt32 command, const UInt32 bar) noexcept
    {
        WriteEcamDoubleWord(0U, Uhci, 0U, 0x04U, command);
        WriteEcamDoubleWord(0U, Uhci, 0U, 0x20U, bar);
        return publish(Uhci, nullptr);
    };
    const bool refusedPorts =
        rejected(uhciWith(DmaEnabledPciCommand, 0x0000'E001U)) && rejected(uhciWith(IoCommand, 0x0000'E000U)) &&
        rejected(uhciWith(IoCommand, 0x0000'0001U)) && rejected(uhciWith(IoCommand, 0x0001'E001U));
    const auto ports = uhciWith(IoCommand, 0x0000'E001U);
    const Kernel::Device::DeviceRegionDescriptor io =
        RegionOf(fixture, ports.HasValue() ? ports.GetValue().Region : Kernel::Object::ObjectId{});
    ASTERIA_EXPECT(runner, "an OHCI page BAR is published as is and a UHCI's I/O BAR 4 as 32 ports", decoded.HasValue(),
                   registers.BaseAddress == 0xFEB1'0000U, registers.LengthBytes == 0x1000U,
                   registers.Kind == DeviceRegionKind::MemoryMappedIo,
                   ReadEcamDoubleWord(0U, Ohci, 0U, 0x04U) == (DmaEnabledPciCommand | 0x400U), refusedPorts,
                   ports.HasValue(), io.Kind == DeviceRegionKind::PortIo, io.BaseAddress == 0xE000U,
                   io.LengthBytes == 32U, io.Widths.Contains(DeviceAccessWidth::Byte),
                   ReadEcamDoubleWord(0U, Uhci, 0U, 0x04U) == (IoCommand | 0x400U));
    ASTERIA_EXPECT(runner, "an xHCI, another class 0C function or an absent one is no USB 1.1/2.0 host controller",
                   rejected(publish(Xhci, nullptr)), rejected(publish(Smbus, nullptr)),
                   rejected(publish(30U, nullptr)));
}

// The boot's side of PublishUsbHosts: one function's firmware BAR extent, one
// withheld device, and every outcome in the order it is recorded.
class UsbHostSite final : public Platform::Device::IUsbHostPublicationSite
{
public:
    struct Outcome
    {
        PciFunctionAddress Function;
        UInt8 Index;
        UInt8 Step;
        UInt8 Error;
    };

    [[nodiscard]] bool FirmwareBar(const PciFunctionAddress function, AhciPciResource& resource) noexcept override
    {
        if (!function.IsEqualTo(Firmware.Address))
        {
            return false;
        }
        resource = AhciPciResource{Firmware.Base,
                                   Firmware.Bytes,
                                   function.GetSegment(),
                                   function.GetBus(),
                                   function.GetDevice(),
                                   function.GetFunction(),
                                   0U,
                                   {0U, 0U}};
        return true;
    }

    [[nodiscard]] bool Permits(const Kernel::Device::PciFunctionDescriptor& function) noexcept override
    {
        return function.Address.GetDevice() != WithheldDevice;
    }

    void Record(const PciFunctionAddress function, const UInt8 index, const UInt8 step,
                const UInt8 error) noexcept override
    {
        if (Count < 16U)
        {
            Records[Count] = Outcome{function, index, step, error};
        }
        ++Count;
    }

    struct Extent
    {
        PciFunctionAddress Address;
        UInt64 Base;
        UInt64 Bytes;
    };

    Extent Firmware{};
    UInt8 WithheldDevice{0U};
    Outcome Records[16U]{};
    Foundation::Size Count{0U};
};

// UsbHostDriver.md: the USB host table takes whole PCI devices in catalog
// order. Device 2 (two UHCIs and their EHCI) fills three entries; device 3's
// two functions exceed the last and are skipped whole (step 3); device 4 is
// withheld (step 4); device 5 is reserved for firmware DMA (step 5); device
// 6 decodes no memory (step 1); device 7's EHCI, beside an SMBus controller
// (class 0C/05), takes the last entry with a BAR sized through configuration
// space.
void TestUsbHostsPublication(TestContext& runner) noexcept
{
    FillEcam(0xFFU);
    AddUsbFunction(2U, 0U, 0x00U, 0x05U, 0x20U, 0x0000'E001U, true);
    AddUsbFunction(2U, 1U, 0x00U, 0x05U, 0x20U, 0x0000'E021U);
    AddUsbFunction(2U, 7U, 0x20U, DmaEnabledPciCommand, 0x10U, 0xFEC0'0000U);
    AddUsbFunction(3U, 0U, 0x10U, DmaEnabledPciCommand, 0x10U, 0xFEC1'0000U, true);
    AddUsbFunction(3U, 1U, 0x20U, DmaEnabledPciCommand, 0x10U, 0xFEC2'0000U);
    AddUsbFunction(4U, 0U, 0x10U, DmaEnabledPciCommand, 0x10U, 0xFEC3'0000U);
    AddUsbFunction(5U, 0U, 0x00U, 0x05U, 0x20U, 0x0000'E041U);
    AddUsbFunction(6U, 0U, 0x10U, 0x04U, 0x10U, 0xFEC4'0000U);
    AddUsbFunction(7U, 0U, 0x20U, DmaEnabledPciCommand, 0x10U, 0xFEC5'0000U, true);
    AddEcamFunction(0U, 7U, 1U, 0x8086U, 0x3A60U, 0U, 0U, 5U, 0x0CU, 0U, false);
    RetainedRuntime unprepared = ConstructRetainedRuntime(46U, 1024U * 1024U);
    RetainedRuntime retained = ConstructRetainedRuntime(45U, 1024U * 1024U);
    RuntimeFixture& fixture = retained.Fixture;
    PciEcamRuntime& runtime = retained.Runtime;
    const auto prepared = runtime.Prepare(MakeBootInfo(reinterpret_cast<UInt64>(&g_ecam[0U])), {},
                                          g_profile->PhysicalAddressBits, fixture.DomainIdentity, fixture.Domain);
    const bool reserved = runtime.ReserveFirmwareDma(PciFunctionAddress{7U, 0U, 5U, 0U});
    UsbHostSite site{};
    site.Firmware = UsbHostSite::Extent{PciFunctionAddress{7U, 0U, 2U, 7U}, 0xFEC0'0000U, 0x400U};
    site.WithheldDevice = 4U;
    Platform::Device::AssignableDevice output[Abi::PagerUsbHostControllers]{};
    const Platform::Device::UsbHostPublicationInput unusable{.Runtime = &runtime,
                                                             .Site = nullptr,
                                                             .PhysicalAddressBits = g_profile->PhysicalAddressBits,
                                                             .ResourceIdentity = fixture.DomainIdentity,
                                                             .Resources = &fixture.Domain};
    const Foundation::Size none = Platform::Device::PublishUsbHosts(unusable, output);
    Platform::Device::UsbHostPublicationInput input = unusable;
    input.Site = &site;
    input.Runtime = &unprepared.Runtime;
    const Foundation::Size early = Platform::Device::PublishUsbHosts(input, output);
    input.Runtime = &runtime;
    g_diagnosticLength = 0U;
    const Foundation::Size published = Platform::Device::PublishUsbHosts(input, output);
    g_diagnostic[g_diagnosticLength] = '\0';
    const auto outcome = [&site](const Foundation::Size record, const UInt8 device, const UInt8 function,
                                 const UInt8 index, const UInt8 step, const PciEcamRuntimeError error) noexcept
    {
        return (record < site.Count) &&
               site.Records[record].Function.IsEqualTo(PciFunctionAddress{7U, 0U, device, function}) &&
               (site.Records[record].Index == index) && (site.Records[record].Step == step) &&
               (site.Records[record].Error == static_cast<UInt8>(error));
    };
    // The catalog's first function is device 2's first UHCI, its ninth device 7's EHCI.
    PciFunctionPublication first{};
    PciFunctionPublication last{};
    const bool listed = (runtime.GetPublishedFunction(0U, first) == PciEcamFunctionQueryError::None) &&
                        (runtime.GetPublishedFunction(8U, last) == PciEcamFunctionQueryError::None);
    constexpr PciEcamRuntimeError NoError{PciEcamRuntimeError::None};
    ASTERIA_EXPECT(
        runner, "USB host controllers are published by whole devices in catalog order", prepared.HasValue(), reserved,
        none == 0U, early == 0U, published == 4U, site.Count == 9U, outcome(0U, 2U, 0U, 1U, 0U, NoError),
        outcome(1U, 2U, 1U, 2U, 0U, NoError), outcome(2U, 2U, 7U, 3U, 0U, NoError),
        outcome(3U, 3U, 0U, 0U, 3U, PciEcamRuntimeError::FunctionCapacityExceeded),
        outcome(4U, 3U, 1U, 0U, 3U, PciEcamRuntimeError::FunctionCapacityExceeded),
        outcome(5U, 4U, 0U, 0U, 4U, NoError), outcome(6U, 5U, 0U, 0U, 5U, NoError),
        outcome(7U, 6U, 0U, 0U, 1U, PciEcamRuntimeError::DeviceRejected), outcome(8U, 7U, 0U, 4U, 0U, NoError));
    ASTERIA_EXPECT(runner, "the table holds each published function and region, and the channel names each outcome",
                   listed, first.Descriptor.Address.IsEqualTo(PciFunctionAddress{7U, 0U, 2U, 0U}),
                   output[0U].Function.GetValue() == first.Function.GetObjectId().GetValue(),
                   last.Descriptor.Address.IsEqualTo(PciFunctionAddress{7U, 0U, 7U, 0U}),
                   output[3U].Function.GetValue() == last.Function.GetObjectId().GetValue(),
                   RegionOf(fixture, output[2U].Region).BaseAddress == 0xFEC0'0000U,
                   RegionOf(fixture, output[2U].Region).LengthBytes == 0x1000U,
                   RegionOf(fixture, output[3U].Region).BaseAddress == 0xFEC5'0000U,
                   RegionOf(fixture, output[3U].Region).LengthBytes == 0x1000U,
                   std::strstr(g_diagnostic, "ASTERIA:USB:PUBLISHED:00020\r\nASTERIA:USB:PUBLISHED:00021\r\n") !=
                       nullptr,
                   std::strstr(g_diagnostic, "ASTERIA:USB:SKIPPED:00030:3:") != nullptr,
                   std::strstr(g_diagnostic, "ASTERIA:USB:SKIPPED:00050:5:00\r\n") != nullptr,
                   std::strstr(g_diagnostic, "ASTERIA:USB:PUBLISHED:00070\r\n") != nullptr);
}

void TestOptionalCaptureFailurePreservesEcam(TestContext& runner) noexcept
{
    FillEcam(0xFFU);
    constexpr UInt8 AhciDevice{4U};
    AddAhciEcamFunction(AhciDevice, 0U, 0U);
    RetainedRuntime retained = ConstructRetainedRuntime(9U, 1024U * 1024U);
    RuntimeFixture& fixture = retained.Fixture;
    PciEcamRuntime& runtime = retained.Runtime;
    const auto prepared = runtime.Prepare(MakeBootInfo(reinterpret_cast<UInt64>(&g_ecam[0U])), {}, 32U,
                                          fixture.DomainIdentity, fixture.Domain);
    AhciPciRuntimeResult output{};
    output.Region.BaseAddress = 0x55AAU;
    const auto query = runtime.GetAhciResult(output);
    UInt32 identifier{0U};
    const auto read = runtime.Read32(PciFunctionAddress{7U, 0U, AhciDevice, 0U}, 0U, identifier);
    ASTERIA_EXPECT(
        runner, "a default record from optional capture failure leaves ECAM usable and AHCI atomically failed",
        prepared.HasValue(), prepared.GetValue().Availability == PciEcamAvailability::Ready,
        runtime.GetState() == PciEcamRuntimeState::Ready, runtime.GetAhciAvailability() == AhciPciAvailability::Failed,
        runtime.GetAhciFailure().Error == AhciPciRuntimeError::InvalidFirmwareResource,
        query == AhciPciRuntimeQueryError::Failed, output.Region.BaseAddress == 0x55AAU,
        read == PciConfigurationReadError::None, identifier == 0x5678'1234U);
}

void TestAhciCatalogCrossMatchFailures(TestContext& runner) noexcept
{
    const UInt64 ecamBase = reinterpret_cast<UInt64>(&g_ecam[0U]);
    const UInt64 abarBase = reinterpret_cast<UInt64>(&g_abar[0U]);

    FillEcam(0xFFU);
    AddEcamFunction(0U, 1U, 0U, 1U, 2U, 0U, 0U, 0U, 2U, 0U, false);
    RetainedRuntime noneRetained = ConstructRetainedRuntime(10U, 1024U * 1024U);
    const auto none = noneRetained.Runtime.Prepare(MakeBootInfo(ecamBase), {}, 32U, noneRetained.Fixture.DomainIdentity,
                                                   noneRetained.Fixture.Domain);
    ASTERIA_EXPECT(runner, "zero catalog AHCI matches publish independent typed absence", none.HasValue(),
                   noneRetained.Runtime.GetAhciAvailability() == AhciPciAvailability::Absent,
                   noneRetained.Runtime.GetAhciFailure().Error == AhciPciRuntimeError::NoCatalogAhciController);

    FillEcam(0xFFU);
    AddAhciEcamFunction(1U, 0U, static_cast<UInt32>(abarBase));
    AddAhciEcamFunction(2U, 0U, static_cast<UInt32>(abarBase));
    RetainedRuntime ambiguousRetained = ConstructRetainedRuntime(11U, 1024U * 1024U);
    const auto ambiguous =
        ambiguousRetained.Runtime.Prepare(MakeBootInfo(ecamBase), MakeAhciResource(1U, 0U), 32U,
                                          ambiguousRetained.Fixture.DomainIdentity, ambiguousRetained.Fixture.Domain);
    ASTERIA_EXPECT(runner, "multiple catalog AHCI matches fail before command, BAR, or region creation",
                   ambiguous.HasValue(), ambiguousRetained.Runtime.GetAhciAvailability() == AhciPciAvailability::Failed,
                   ambiguousRetained.Runtime.GetAhciFailure().Error ==
                       AhciPciRuntimeError::AmbiguousCatalogAhciController,
                   ambiguousRetained.Runtime.GetAhciFailure().MatchingFunctionCount == 2U);

    FillEcam(0xFFU);
    AddAhciEcamFunction(1U, 0U, static_cast<UInt32>(abarBase));
    RetainedRuntime mismatchRetained = ConstructRetainedRuntime(12U, 1024U * 1024U);
    const auto mismatch =
        mismatchRetained.Runtime.Prepare(MakeBootInfo(ecamBase), MakeAhciResource(2U, 0U), 32U,
                                         mismatchRetained.Fixture.DomainIdentity, mismatchRetained.Fixture.Domain);
    ASTERIA_EXPECT(runner, "captured and catalog BDF mismatch is rejected before ABAR publication", mismatch.HasValue(),
                   mismatchRetained.Runtime.GetAhciFailure().Error == AhciPciRuntimeError::FirmwareBdfMismatch,
                   mismatchRetained.Fixture.Registry.GetActiveCount() == 3U);
}

void TestAhciRangeCommandAndBarPreflightFailures(TestContext& runner) noexcept
{
    const UInt64 ecamBase = reinterpret_cast<UInt64>(&g_ecam[0U]);
    const UInt64 abarBase = reinterpret_cast<UInt64>(&g_abar[0U]);
    Foundation::Size runtimeIndex{13U};

    struct Case
    {
        AhciPciResource Resource;
        UInt32 Bar5;
        UInt32 Command;
        AhciPciRuntimeError Expected;
    };
    Case cases[11U]{
        {MakeAhciResource(32U, 0U), static_cast<UInt32>(abarBase), DmaEnabledPciCommand,
         AhciPciRuntimeError::InvalidFirmwareResource},
        {MakeAhciResource(3U, 0U, 0xFFFF'F000U, 0x2000U), 0xFFFF'F000U, DmaEnabledPciCommand,
         AhciPciRuntimeError::PhysicalRangeRejected},
        {MakeAhciResource(3U, 0U, ~UInt64{0U} - 15U, 32U), static_cast<UInt32>(abarBase), DmaEnabledPciCommand,
         AhciPciRuntimeError::PhysicalRangeRejected},
        {MakeAhciResource(3U, 0U, abarBase, 0U), static_cast<UInt32>(abarBase), DmaEnabledPciCommand,
         AhciPciRuntimeError::InvalidFirmwareResource},
        {MakeAhciResource(3U, 0U, abarBase, 24U), static_cast<UInt32>(abarBase), DmaEnabledPciCommand,
         AhciPciRuntimeError::AbarPreflightRejected},
        {MakeAhciResource(3U, 0U), static_cast<UInt32>(abarBase), 0U, AhciPciRuntimeError::MemorySpaceDisabled},
        {MakeAhciResource(3U, 0U), static_cast<UInt32>(abarBase), 0x02U, AhciPciRuntimeError::BusMasterDisabled},
        {MakeAhciResource(3U, 0U), 0U, DmaEnabledPciCommand, AhciPciRuntimeError::AbarPreflightRejected},
        {MakeAhciResource(3U, 0U), 0x0000'C001U, DmaEnabledPciCommand, AhciPciRuntimeError::AbarKindRejected},
        {MakeAhciResource(3U, 0U), static_cast<UInt32>(abarBase) | 8U, DmaEnabledPciCommand,
         AhciPciRuntimeError::AbarPrefetchable},
        {MakeAhciResource(3U, 0U), static_cast<UInt32>(abarBase + AbarSizeBytes), DmaEnabledPciCommand,
         AhciPciRuntimeError::AbarBaseMismatch},
    };
    cases[0U].Resource.Device = 32U;

    for (const Case& testCase : cases)
    {
        FillEcam(0xFFU);
        AddAhciEcamFunction(3U, 0U, testCase.Bar5, testCase.Command);
        RetainedRuntime retained = ConstructRetainedRuntime(runtimeIndex, 1024U * 1024U);
        ++runtimeIndex;
        const auto prepared = retained.Runtime.Prepare(MakeBootInfo(ecamBase), testCase.Resource, 32U,
                                                       retained.Fixture.DomainIdentity, retained.Fixture.Domain);
        AhciPciRuntimeResult output{};
        output.Region.BaseAddress = 0xAA55U;
        const auto failure = retained.Runtime.GetAhciFailure();
        ASTERIA_EXPECT(
            runner,
            "invalid BDF, range, DMA command state, and BAR geometry fail atomically before DeviceRegion creation",
            prepared.HasValue(), retained.Runtime.GetState() == PciEcamRuntimeState::Ready,
            retained.Runtime.GetAhciAvailability() == AhciPciAvailability::Failed, failure.Error == testCase.Expected,
            (testCase.Expected != AhciPciRuntimeError::BusMasterDisabled) ||
                (failure.CommandRegister == testCase.Command),
            retained.Runtime.GetAhciResult(output) == AhciPciRuntimeQueryError::Failed,
            output.Region.BaseAddress == 0xAA55U, retained.Fixture.Registry.GetActiveCount() == 3U);
    }
}

void TestAhciCapacityAndLateFailureRetention(TestContext& runner) noexcept
{
    const UInt64 ecamBase = reinterpret_cast<UInt64>(&g_ecam[0U]);
    const UInt64 abarBase = reinterpret_cast<UInt64>(&g_abar[0U]);
    FillEcam(0xFFU);
    AddAhciEcamFunction(7U, 0U, static_cast<UInt32>(abarBase));
    constexpr UInt64 MetadataBeforeAbar{sizeof(DeviceRegion) + sizeof(PciFunction)};
    RetainedRuntime capacityRetained = ConstructRetainedRuntime(23U, MetadataBeforeAbar);
    const auto capacity =
        capacityRetained.Runtime.Prepare(MakeBootInfo(ecamBase), MakeAhciResource(7U, 0U), 32U,
                                         capacityRetained.Fixture.DomainIdentity, capacityRetained.Fixture.Domain);
    ASTERIA_EXPECT(runner, "ABAR DeviceRegion capacity rejection occurs only after complete preflight",
                   capacity.HasValue(), capacityRetained.Runtime.GetState() == PciEcamRuntimeState::Ready,
                   capacityRetained.Runtime.GetAhciFailure().Error == AhciPciRuntimeError::RegionRejected,
                   capacityRetained.Runtime.GetAhciFailure().DeviceManager.Error ==
                       Kernel::Device::DeviceManagerError::RegionRejected,
                   capacityRetained.Fixture.Registry.GetActiveCount() == 3U);

    FillEcam(0xFFU);
    AddAhciEcamFunction(7U, 0U, static_cast<UInt32>(abarBase));
    RetainedRuntime lateRetained = ConstructRetainedRuntime(24U, 1024U * 1024U);
    const auto late = lateRetained.Runtime.Prepare(MakeBootInfo(ecamBase), MakeAhciResource(7U, 0U, abarBase, 16U), 32U,
                                                   lateRetained.Fixture.DomainIdentity, lateRetained.Fixture.Domain);
    const auto lateDomain = lateRetained.Fixture.Domain.GetSnapshot();
    AhciPciRuntimeResult output{};
    output.Region.BaseAddress = 0x1122U;
    ASTERIA_EXPECT(runner,
                   "a post-creation binding failure retains the private ABAR region without partial publication",
                   late.HasValue(), lateRetained.Runtime.GetAhciFailure().Error == AhciPciRuntimeError::BindingRejected,
                   lateRetained.Runtime.GetAhciFailure().Binding.Error ==
                       Kernel::Device::AhciPciBootstrapBindingError::SnapshotReadRejected,
                   lateRetained.Runtime.GetAhciFailure().Binding.Snapshot.Error ==
                       Kernel::Device::AhciHbaSnapshotError::InvalidRange,
                   lateRetained.Fixture.Registry.GetActiveCount() == 4U,
                   lateDomain.Usage.KernelMetadataBytes == ((2U * sizeof(DeviceRegion)) + sizeof(PciFunction)),
                   lateRetained.Runtime.GetAhciResult(output) == AhciPciRuntimeQueryError::Failed,
                   output.Region.BaseAddress == 0x1122U);
}

void TestAhciIdentifyOneShotBoundary(TestContext& runner) noexcept
{
    ManualClock clock{101U};
    clock.SetAdvanceOnRead(true);
    const AhciIdentifyPollingPolicy polling{
        .DeadlineTicks = 10'000U,
        .MaximumPollCount = 16U,
        .MaximumRecoveryPollCount = 16U,
    };
    const AhciDmaPageWritableView dmaPage{
        .Data = g_identifyDmaPage.Bytes,
        .SizeBytes = sizeof(g_identifyDmaPage.Bytes),
        .PhysicalAddress = reinterpret_cast<UInt64>(g_identifyDmaPage.Bytes),
    };

    RetainedRuntime unavailable = ConstructRetainedRuntime(25U, 1024U * 1024U);
    AhciIdentifyCommandReceipt untouched{};
    untouched.Device.SectorCount = 0x55AAU;
    const auto beforeQuery = unavailable.Runtime.GetAhciIdentifyReceipt(untouched);
    const auto unavailableFailure = unavailable.Runtime.ExecuteAhciIdentify(clock, dmaPage, polling);
    const auto unavailableRepeat = unavailable.Runtime.ExecuteAhciIdentify(clock, dmaPage, polling);
    ASTERIA_EXPECT(runner, "IDENTIFY rejects unavailable AHCI authority and consumes its one-shot boundary",
                   beforeQuery == AhciIdentifyRuntimeQueryError::NotReady, untouched.Device.SectorCount == 0x55AAU,
                   unavailableFailure.Error == AhciIdentifyRuntimeError::AhciNotReady,
                   unavailable.Runtime.GetAhciIdentifyState() == AhciIdentifyRuntimeState::Failed,
                   unavailableRepeat.Error == AhciIdentifyRuntimeError::AlreadyAttempted);

    FillEcam(0xFFU);
    InitializeSuccessfulAbar();
    constexpr UInt8 AhciDevice{7U};
    const UInt64 abarBase = reinterpret_cast<UInt64>(&g_abar[0U]);
    AddAhciEcamFunction(AhciDevice, 0U, static_cast<UInt32>(abarBase));
    RetainedRuntime retained = ConstructRetainedRuntime(26U, 1024U * 1024U);
    const auto prepared =
        retained.Runtime.Prepare(MakeBootInfo(reinterpret_cast<UInt64>(&g_ecam[0U])), MakeAhciResource(AhciDevice, 0U),
                                 32U, retained.Fixture.DomainIdentity, retained.Fixture.Domain);
    const auto first = retained.Runtime.ExecuteAhciIdentify(clock, dmaPage, polling);
    AhciIdentifyCommandReceipt rejectedOutput{};
    rejectedOutput.Device.SectorCount = 0xAA55U;
    const auto rejectedQuery = retained.Runtime.GetAhciIdentifyReceipt(rejectedOutput);
    const auto stored = retained.Runtime.GetAhciIdentifyFailure();
    const auto repeated = retained.Runtime.ExecuteAhciIdentify(clock, dmaPage, polling);
    ASTERIA_EXPECT(
        runner, "IDENTIFY derives the bound ABAR, atomically retains failure, and rejects replay", prepared.HasValue(),
        retained.Runtime.GetAhciAvailability() == AhciPciAvailability::Ready,
        first.Error == AhciIdentifyRuntimeError::CommandRejected,
        first.Command.Error == AhciIdentifyCommandError::NoActiveDiskPort, !first.Command.DmaPageMustRemainLive,
        retained.Runtime.GetAhciIdentifyState() == AhciIdentifyRuntimeState::Failed,
        rejectedQuery == AhciIdentifyRuntimeQueryError::Failed, rejectedOutput.Device.SectorCount == 0xAA55U,
        stored.Error == first.Error, stored.Command.Error == first.Command.Error,
        repeated.Error == AhciIdentifyRuntimeError::AlreadyAttempted, repeated.Command.Error == first.Command.Error);

    FillEcam(0xFFU);
    InitializeIdentifyAbar();
    constexpr UInt8 SuccessfulAhciDevice{8U};
    AddAhciEcamFunction(SuccessfulAhciDevice, 0U, static_cast<UInt32>(abarBase));
    RetainedRuntime successful = ConstructRetainedRuntime(27U, 1024U * 1024U);
    const auto successfulPreparation = successful.Runtime.Prepare(
        MakeBootInfo(reinterpret_cast<UInt64>(&g_ecam[0U])), MakeAhciResource(SuccessfulAhciDevice, 0U), 32U,
        successful.Fixture.DomainIdentity, successful.Fixture.Domain);
    AhciCompletionEmulatorClock completingClock{dmaPage};
    const auto completed = successful.Runtime.ExecuteAhciIdentify(completingClock, dmaPage, polling);
    AhciIdentifyCommandReceipt receipt{};
    const auto receiptQuery = successful.Runtime.GetAhciIdentifyReceipt(receipt);
    const auto completedRepeat = successful.Runtime.ExecuteAhciIdentify(completingClock, dmaPage, polling);
    ASTERIA_EXPECT(runner, "IDENTIFY publishes a completed transaction atomically", successfulPreparation.HasValue(),
                   completingClock.WasCommandObserved(), completingClock.WasCompletionPublished(),
                   !completingClock.WasMutationApplied(),
                   completingClock.GetValidationError() == AhciCompletionValidationError::None,
                   completed.Error == AhciIdentifyRuntimeError::None,
                   completed.Command.Error == AhciIdentifyCommandError::None,
                   successful.Runtime.GetAhciIdentifyState() == AhciIdentifyRuntimeState::Succeeded,
                   receiptQuery == AhciIdentifyRuntimeQueryError::None);
    ASTERIA_EXPECT(runner, "IDENTIFY retains its parsed receipt and rejects successful replay",
                   receipt.PortNumber == 0U, receipt.Device.SectorCount == 0x0000'0001'2345'6789ULL,
                   receipt.Device.ModelNumberLength == 20U, receipt.DeviceToHostFisStatus == 0x50U,
                   completedRepeat.Error == AhciIdentifyRuntimeError::AlreadyAttempted,
                   completedRepeat.Command.Error == AhciIdentifyCommandError::None);
}

void TestAhciIdentifyCompletionEmulatorRejectsMalformedCommands(TestContext& runner) noexcept
{
    struct Case
    {
        AhciCompletionMutation Mutation;
        AhciCompletionValidationError ExpectedValidationError;
        bool OffsetRequestDmaAddress;
        bool ExpectedMutationApplied;
        const char* Description;
    };
    constexpr Case Cases[]{
        {AhciCompletionMutation::None, AhciCompletionValidationError::InvalidCommandListAddress, true, false,
         "IDENTIFY rejects a caller-provided DMA address outside the controller mapping"},
        {AhciCompletionMutation::CommandListAddress, AhciCompletionValidationError::InvalidCommandListAddress, false,
         true, "IDENTIFY emulator rejects a mutated PxCLB address"},
        {AhciCompletionMutation::ReceivedFisAddress, AhciCompletionValidationError::InvalidReceivedFisAddress, false,
         true, "IDENTIFY emulator rejects a mutated PxFB address"},
        {AhciCompletionMutation::AtaCommand, AhciCompletionValidationError::InvalidCommandFis, false, true,
         "IDENTIFY emulator rejects a non-IDENTIFY ATA command"},
        {AhciCompletionMutation::PrdtAddress, AhciCompletionValidationError::InvalidPrdt, false, true,
         "IDENTIFY emulator rejects a PRDT address outside the DMA mapping"},
        {AhciCompletionMutation::PrdtByteCount, AhciCompletionValidationError::InvalidPrdt, false, true,
         "IDENTIFY emulator rejects a malformed PRDT transfer bound"},
    };
    static_assert((sizeof(Cases) / sizeof(Cases[0U])) == IdentifyMutationCaseCount);
    const AhciIdentifyPollingPolicy polling{
        .DeadlineTicks = 10'000U,
        .MaximumPollCount = 4U,
        .MaximumRecoveryPollCount = 4U,
    };
    const UInt64 ecamBase = reinterpret_cast<UInt64>(&g_ecam[0U]);
    const UInt64 abarBase = reinterpret_cast<UInt64>(&g_abar[0U]);

    Foundation::Size caseIndex{0U};
    for (const Case& testCase : Cases)
    {
        const AhciDmaPageWritableView controllerDmaPage{
            .Data = g_mutatedIdentifyDmaPages[caseIndex].Bytes,
            .SizeBytes = sizeof(g_mutatedIdentifyDmaPages[caseIndex].Bytes),
            .PhysicalAddress = reinterpret_cast<UInt64>(g_mutatedIdentifyDmaPages[caseIndex].Bytes),
        };
        FillEcam(0xFFU);
        InitializeIdentifyAbar();
        const UInt8 device = static_cast<UInt8>(9U + caseIndex);
        AddAhciEcamFunction(device, 0U, static_cast<UInt32>(abarBase));
        RetainedRuntime retained = ConstructRetainedRuntime(28U + caseIndex, 1024U * 1024U);
        const auto prepared = retained.Runtime.Prepare(MakeBootInfo(ecamBase), MakeAhciResource(device, 0U), 32U,
                                                       retained.Fixture.DomainIdentity, retained.Fixture.Domain);
        AhciDmaPageWritableView requestDmaPage = controllerDmaPage;
        if (testCase.OffsetRequestDmaAddress)
        {
            requestDmaPage.PhysicalAddress += requestDmaPage.SizeBytes;
        }
        AhciCompletionEmulatorClock emulatorClock{controllerDmaPage, testCase.Mutation};
        const auto failure = retained.Runtime.ExecuteAhciIdentify(emulatorClock, requestDmaPage, polling);
        AhciIdentifyCommandReceipt receipt{};
        receipt.Device.SectorCount = 0xAA55U;
        const auto receiptQuery = retained.Runtime.GetAhciIdentifyReceipt(receipt);
        const auto storedFailure = retained.Runtime.GetAhciIdentifyFailure();
        ASTERIA_EXPECT(runner, testCase.Description, prepared.HasValue(), emulatorClock.WasCommandObserved(),
                       !emulatorClock.WasCompletionPublished(),
                       emulatorClock.WasMutationApplied() == testCase.ExpectedMutationApplied,
                       emulatorClock.GetValidationError() == testCase.ExpectedValidationError,
                       failure.Error == AhciIdentifyRuntimeError::CommandRejected,
                       failure.Command.Error == AhciIdentifyCommandError::CommandTimedOut,
                       failure.Command.DmaPageMustRemainLive,
                       retained.Runtime.GetAhciIdentifyState() == AhciIdentifyRuntimeState::Failed,
                       receiptQuery == AhciIdentifyRuntimeQueryError::Failed, receipt.Device.SectorCount == 0xAA55U,
                       storedFailure.Error == failure.Error, storedFailure.Command.Error == failure.Command.Error);
        ++caseIndex;
    }
}

void TestAhciReadPrerequisiteBoundaries(TestContext& runner) noexcept
{
    const AhciReadDmaExtPollingPolicy polling{
        .DeadlineTicks = 10'000U,
        .MaximumPollCount = 16U,
        .MaximumRecoveryPollCount = 16U,
    };
    const AhciReadDmaExtCommandPageWritableView commandPage = MakeReadCommandPage(0U);
    const AhciReadDmaExtDataBufferSpan dataBuffer = MakeReadDataBuffer(0U);

    RetainedRuntime beforeIdentify = ConstructRetainedRuntime(34U, 1024U * 1024U);
    AhciReadDmaExtExecutionReceipt untouched{};
    untouched.StartingLba = 0xAA55U;
    const auto beforeQuery = beforeIdentify.Runtime.GetAhciReadDmaExtReceipt(untouched);
    ManualClock beforeClock{301U};
    beforeClock.SetAdvanceOnRead(true);
    const auto first =
        beforeIdentify.Runtime.ExecuteAhciReadDmaExt(beforeClock, commandPage, dataBuffer, 8U, 1U, polling);
    const auto failedQuery = beforeIdentify.Runtime.GetAhciReadDmaExtReceipt(untouched);
    const auto stored = beforeIdentify.Runtime.GetAhciReadDmaExtFailure();
    const auto repeated = beforeIdentify.Runtime.ExecuteAhciReadDmaExt(
        beforeClock, AhciReadDmaExtCommandPageWritableView{}, AhciReadDmaExtDataBufferSpan{}, 0U, 0U,
        AhciReadDmaExtPollingPolicy{});
    const auto afterRepeat = beforeIdentify.Runtime.GetAhciReadDmaExtFailure();
    ASTERIA_EXPECT(
        runner, "READ rejects execution before IDENTIFY, retains the first failure, and leaves queries atomic",
        beforeQuery == AhciReadDmaExtRuntimeQueryError::NotReady,
        failedQuery == AhciReadDmaExtRuntimeQueryError::Failed, untouched.StartingLba == 0xAA55U,
        first.Error == AhciReadDmaExtRuntimeError::IdentifyNotSucceeded, stored.Error == first.Error,
        beforeIdentify.Runtime.GetAhciReadDmaExtState() == AhciReadDmaExtRuntimeState::Failed,
        repeated.Error == AhciReadDmaExtRuntimeError::AlreadyAttempted, repeated.Command.Error == first.Command.Error,
        !repeated.Command.CommandPageMustRemainLive, !repeated.Command.DataBufferMustRemainLive,
        afterRepeat.Error == AhciReadDmaExtRuntimeError::IdentifyNotSucceeded,
        afterRepeat.Command.Error == first.Command.Error, beforeClock.GetReadCount() == 0U);

    FillEcam(0xFFU);
    InitializeSuccessfulAbar();
    constexpr UInt8 AhciDevice{18U};
    const UInt64 abarBase = reinterpret_cast<UInt64>(&g_abar[0U]);
    AddAhciEcamFunction(AhciDevice, 0U, static_cast<UInt32>(abarBase));
    RetainedRuntime noDisk = ConstructRetainedRuntime(35U, 1024U * 1024U);
    const auto prepared =
        noDisk.Runtime.Prepare(MakeBootInfo(reinterpret_cast<UInt64>(&g_ecam[0U])), MakeAhciResource(AhciDevice, 0U),
                               32U, noDisk.Fixture.DomainIdentity, noDisk.Fixture.Domain);
    ManualClock identifyClock{101U};
    identifyClock.SetAdvanceOnRead(true);
    const auto identify = noDisk.Runtime.ExecuteAhciIdentify(identifyClock, MakeIdentifyDmaPage(),
                                                             AhciIdentifyPollingPolicy{
                                                                 .DeadlineTicks = 10'000U,
                                                                 .MaximumPollCount = 16U,
                                                                 .MaximumRecoveryPollCount = 16U,
                                                             });
    ManualClock readClock{501U};
    readClock.SetAdvanceOnRead(true);
    const auto read = noDisk.Runtime.ExecuteAhciReadDmaExt(readClock, commandPage, dataBuffer, 8U, 1U, polling);
    ASTERIA_EXPECT(runner, "READ after no-disk IDENTIFY fails before invoking its executor", prepared.HasValue(),
                   identify.Error == AhciIdentifyRuntimeError::CommandRejected,
                   identify.Command.Error == AhciIdentifyCommandError::NoActiveDiskPort,
                   read.Error == AhciReadDmaExtRuntimeError::IdentifyNotSucceeded, readClock.GetReadCount() == 0U,
                   !read.Command.CommandPageMustRemainLive, !read.Command.DataBufferMustRemainLive);
}

void TestAhciReadRejectsMalformedSpans(TestContext& runner) noexcept
{
    struct Case
    {
        Foundation::Size RuntimeIndex;
        Foundation::Size StorageIndex;
        AhciReadDmaExtCommandError Expected;
        Foundation::Size CommandSizeBytes;
        UInt64 CommandPhysicalAdjustment;
        UInt64 DataSizeBytes;
        const char* Description;
    };
    constexpr Case Cases[]{
        {36U, 1U, AhciReadDmaExtCommandError::InvalidCommandPageSize,
         AhciReadDmaExtCommandBuilder::CommandPageSizeBytes - 1U, 0U, 512U,
         "READ rejects a non-page-sized command span atomically"},
        {37U, 2U, AhciReadDmaExtCommandError::InvalidCommandPagePhysicalAlignment,
         AhciReadDmaExtCommandBuilder::CommandPageSizeBytes, 1U, 512U,
         "READ rejects a misaligned controller-visible command span atomically"},
        {38U, 3U, AhciReadDmaExtCommandError::InvalidDataBufferSize, AhciReadDmaExtCommandBuilder::CommandPageSizeBytes,
         0U, 510U, "READ rejects a destination span that is not the derived transfer size"},
    };
    const AhciReadDmaExtPollingPolicy polling{
        .DeadlineTicks = 10'000U,
        .MaximumPollCount = 16U,
        .MaximumRecoveryPollCount = 16U,
    };
    for (const Case& testCase : Cases)
    {
        RetainedRuntime retained = ConstructRetainedRuntime(testCase.RuntimeIndex, 1024U * 1024U);
        AhciIdentifyCommandReceipt identify{};
        const bool identified =
            PrepareIdentifiedRuntime(retained, static_cast<UInt8>(testCase.RuntimeIndex - 17U), identify);
        AhciReadDmaExtCommandPageWritableView commandPage = MakeReadCommandPage(testCase.StorageIndex);
        commandPage.SizeBytes = testCase.CommandSizeBytes;
        commandPage.PhysicalAddress += testCase.CommandPhysicalAdjustment;
        AhciReadDmaExtDataBufferSpan dataBuffer = MakeReadDataBuffer(testCase.StorageIndex);
        dataBuffer.SizeBytes = testCase.DataSizeBytes;
        ManualClock clock{101U};
        clock.SetAdvanceOnRead(true);
        const auto failure = retained.Runtime.ExecuteAhciReadDmaExt(clock, commandPage, dataBuffer, 8U, 1U, polling);
        AhciReadDmaExtExecutionReceipt output{};
        output.TransferSizeBytes = 0xAA55U;
        const auto query = retained.Runtime.GetAhciReadDmaExtReceipt(output);
        ASTERIA_EXPECT(runner, testCase.Description, identified, identify.PortNumber == 0U,
                       failure.Error == AhciReadDmaExtRuntimeError::CommandRejected,
                       failure.Command.Error == AhciReadDmaExtExecutionError::CommandRejected,
                       failure.Command.Command == testCase.Expected, !failure.Command.CommandPageMustRemainLive,
                       !failure.Command.DataBufferMustRemainLive,
                       retained.Runtime.GetAhciReadDmaExtState() == AhciReadDmaExtRuntimeState::Failed,
                       query == AhciReadDmaExtRuntimeQueryError::Failed, output.TransferSizeBytes == 0xAA55U);
    }
}

void TestAhciReadTimeoutLifetimeAndReplay(TestContext& runner) noexcept
{
    constexpr Foundation::Size StorageIndex{4U};
    RetainedRuntime retained = ConstructRetainedRuntime(39U, 1024U * 1024U);
    AhciIdentifyCommandReceipt identify{};
    const bool identified = PrepareIdentifiedRuntime(retained, 22U, identify);
    const AhciReadDmaExtCommandPageWritableView commandPage = MakeReadCommandPage(StorageIndex);
    const AhciReadDmaExtDataBufferSpan dataBuffer = MakeReadDataBuffer(StorageIndex);
    for (Foundation::Size index{0U}; index < sizeof(g_readCommandPages[StorageIndex].Bytes); ++index)
    {
        g_readCommandPages[StorageIndex].Bytes[index] = 0x6DU;
    }
    AhciReadCompletionEmulatorClock timeoutClock{commandPage, dataBuffer, g_readDataPages[StorageIndex].Bytes, false};
    const auto first = retained.Runtime.ExecuteAhciReadDmaExt(timeoutClock, commandPage, dataBuffer, 8U, 1U,
                                                              AhciReadDmaExtPollingPolicy{
                                                                  .DeadlineTicks = 10'000U,
                                                                  .MaximumPollCount = 2U,
                                                                  .MaximumRecoveryPollCount = 2U,
                                                              });
    AhciReadDmaExtExecutionReceipt untouched{};
    untouched.StartingLba = 0x1122U;
    const auto query = retained.Runtime.GetAhciReadDmaExtReceipt(untouched);
    const auto stored = retained.Runtime.GetAhciReadDmaExtFailure();
    ManualClock replayClock{701U};
    replayClock.SetAdvanceOnRead(true);
    const auto repeated =
        retained.Runtime.ExecuteAhciReadDmaExt(replayClock, MakeReadCommandPage(5U), MakeReadDataBuffer(5U), 9U, 2U,
                                               AhciReadDmaExtPollingPolicy{
                                                   .DeadlineTicks = 20'000U,
                                                   .MaximumPollCount = 8U,
                                                   .MaximumRecoveryPollCount = 8U,
                                               });
    const auto afterRepeat = retained.Runtime.GetAhciReadDmaExtFailure();
    ASTERIA_EXPECT(
        runner, "READ timeout retains exact command/data lifetime evidence and replay cannot overwrite it", identified,
        timeoutClock.WasCommandObserved(), timeoutClock.WasExactWireObserved(), !timeoutClock.WasCompletionPublished(),
        first.Error == AhciReadDmaExtRuntimeError::CommandRejected,
        first.Command.Error == AhciReadDmaExtExecutionError::CommandTimedOut, first.Command.CommandPageMustRemainLive,
        first.Command.DataBufferMustRemainLive, query == AhciReadDmaExtRuntimeQueryError::Failed,
        untouched.StartingLba == 0x1122U, stored.Command.Error == first.Command.Error,
        stored.Command.RecoveryError == first.Command.RecoveryError,
        stored.Command.CommandPageMustRemainLive == first.Command.CommandPageMustRemainLive,
        stored.Command.DataBufferMustRemainLive == first.Command.DataBufferMustRemainLive,
        repeated.Error == AhciReadDmaExtRuntimeError::AlreadyAttempted, repeated.Command.Error == first.Command.Error,
        repeated.Command.RecoveryError == first.Command.RecoveryError, repeated.Command.CommandPageMustRemainLive,
        repeated.Command.DataBufferMustRemainLive, afterRepeat.Error == AhciReadDmaExtRuntimeError::CommandRejected,
        afterRepeat.Command.Error == first.Command.Error,
        afterRepeat.Command.RecoveryError == first.Command.RecoveryError, afterRepeat.Command.CommandPageMustRemainLive,
        afterRepeat.Command.DataBufferMustRemainLive, replayClock.GetReadCount() == 0U);
}

void TestAhciReadSuccessfulExecutionAndQuery(TestContext& runner) noexcept
{
    constexpr Foundation::Size StorageIndex{6U};
    RetainedRuntime retained = ConstructRetainedRuntime(40U, 1024U * 1024U);
    AhciIdentifyCommandReceipt identify{};
    const bool identified = PrepareIdentifiedRuntime(retained, 23U, identify);
    const AhciReadDmaExtCommandPageWritableView commandPage = MakeReadCommandPage(StorageIndex);
    const AhciReadDmaExtDataBufferSpan dataBuffer = MakeReadDataBuffer(StorageIndex);
    for (Foundation::Size index{0U}; index < sizeof(g_readCommandPages[StorageIndex].Bytes); ++index)
    {
        g_readCommandPages[StorageIndex].Bytes[index] = 0x6DU;
    }
    for (Foundation::Size index{0U}; index < 512U; ++index)
    {
        g_readDataPages[StorageIndex].Bytes[index] = 0xA5U;
    }
    AhciReadDmaExtExecutionReceipt untouched{};
    untouched.StartingLba = 0x55AAU;
    const auto beforeQuery = retained.Runtime.GetAhciReadDmaExtReceipt(untouched);
    AhciReadCompletionEmulatorClock completingClock{commandPage, dataBuffer, g_readDataPages[StorageIndex].Bytes};
    const auto completed = retained.Runtime.ExecuteAhciReadDmaExt(completingClock, commandPage, dataBuffer, 8U, 1U,
                                                                  AhciReadDmaExtPollingPolicy{
                                                                      .DeadlineTicks = 10'000U,
                                                                      .MaximumPollCount = 16U,
                                                                      .MaximumRecoveryPollCount = 16U,
                                                                  });
    AhciReadDmaExtExecutionReceipt receipt{};
    const auto query = retained.Runtime.GetAhciReadDmaExtReceipt(receipt);
    const auto repeated = retained.Runtime.ExecuteAhciReadDmaExt(completingClock, commandPage, dataBuffer, 8U, 1U,
                                                                 AhciReadDmaExtPollingPolicy{
                                                                     .DeadlineTicks = 10'000U,
                                                                     .MaximumPollCount = 16U,
                                                                     .MaximumRecoveryPollCount = 16U,
                                                                 });
    const auto afterRepeat = retained.Runtime.GetAhciReadDmaExtFailure();
    constexpr char Token[]{"ASTERIA-READ-DMA-EXT-SECTOR-0001"};
    bool contentMatches{true};
    for (Foundation::Size index{0U}; index < 512U; ++index)
    {
        if (g_readDataPages[StorageIndex].Bytes[index] != static_cast<UInt8>(Token[index % (sizeof(Token) - 1U)]))
        {
            contentMatches = false;
        }
    }
    bool inactiveCommandBytesPreserved{true};
    for (Foundation::Size index{0x590U}; index < sizeof(g_readCommandPages[StorageIndex].Bytes); ++index)
    {
        if (g_readCommandPages[StorageIndex].Bytes[index] != 0x6DU)
        {
            inactiveCommandBytesPreserved = false;
        }
    }
    ASTERIA_EXPECT(runner, "successful IDENTIFY plus READ publishes one immutable receipt", identified,
                   beforeQuery == AhciReadDmaExtRuntimeQueryError::NotReady, untouched.StartingLba == 0x55AAU,
                   completingClock.WasCommandObserved(), completingClock.WasExactWireObserved(),
                   completingClock.WasCompletionPublished(), completed.Error == AhciReadDmaExtRuntimeError::None,
                   completed.Command.Error == AhciReadDmaExtExecutionError::None,
                   !completed.Command.CommandPageMustRemainLive, !completed.Command.DataBufferMustRemainLive,
                   retained.Runtime.GetAhciReadDmaExtState() == AhciReadDmaExtRuntimeState::Succeeded,
                   query == AhciReadDmaExtRuntimeQueryError::None);
    ASTERIA_EXPECT(runner, "READ receipt, exact command wire/PRDT, DMA content, and completion evidence agree",
                   receipt.StartTicks != 0U, receipt.CompletionTicks >= receipt.StartTicks,
                   receipt.DeviceSectorCount == identify.Device.SectorCount, receipt.StartingLba == 8U,
                   receipt.LogicalSectorSizeBytes == identify.Device.LogicalSectorSizeBytes, receipt.SectorCount == 1U,
                   receipt.TransferSizeBytes == 512U, receipt.Capabilities == identify.Capabilities,
                   receipt.PortsImplemented == identify.PortsImplemented,
                   receipt.PortSignature == identify.PortSignature, receipt.CompletionInterruptStatus == 1U,
                   receipt.CompletionTaskFileData == 0U, receipt.PortNumber == identify.PortNumber,
                   receipt.PortDisposition == AhciReadDmaExtPortDisposition::Restored,
                   receipt.DeviceToHostFisStatus == 0x50U, receipt.DeviceToHostFisError == 0U, contentMatches,
                   inactiveCommandBytesPreserved);
    ASTERIA_EXPECT(runner, "successful READ replay preserves the original zero failure outcome",
                   repeated.Error == AhciReadDmaExtRuntimeError::AlreadyAttempted,
                   repeated.Command.Error == AhciReadDmaExtExecutionError::None,
                   !repeated.Command.CommandPageMustRemainLive, !repeated.Command.DataBufferMustRemainLive,
                   afterRepeat.Error == AhciReadDmaExtRuntimeError::None,
                   afterRepeat.Command.Error == AhciReadDmaExtExecutionError::None,
                   !afterRepeat.Command.CommandPageMustRemainLive, !afterRepeat.Command.DataBufferMustRemainLive);
}
} // namespace
} // namespace Asteria::Test

int main(const int argumentCount, char** const arguments)
{
    constexpr Asteria::Platform::Ia64::Device::Ia64PhysicalMemoryAccess ia64Access{};
    constexpr Asteria::Platform::Amd64::Device::Amd64PhysicalMemoryAccess amd64Access{};
    const Asteria::Test::AccessProfile ia64{ia64Access, 63U};
    const Asteria::Test::AccessProfile amd64{amd64Access, 52U};
    const char* const mode = (argumentCount == 2) ? arguments[1] : "";
    if (std::strcmp(mode, "ia64") == 0)
    {
        Asteria::Test::g_profile = &ia64;
    }
    else if (std::strcmp(mode, "amd64") == 0)
    {
        Asteria::Test::g_profile = &amd64;
    }
    else
    {
        static_cast<void>(std::fprintf(stderr, "usage: PciEcamRuntimeTests ia64|amd64\n"));
        return 2;
    }
    if ((Asteria::Test::g_ecam == nullptr) || (Asteria::Test::g_ecam == MAP_FAILED) ||
        (Asteria::Test::g_abar == nullptr) || (Asteria::Test::g_abar == MAP_FAILED))
    {
        static_cast<void>(std::fprintf(stderr, "FAIL: low-address hosted MMIO test mappings are unavailable\n"));
        return 1;
    }
    TestContext runner{"PciEcamRuntimeTests"};
    Asteria::Test::TestMultifunctionDiscoveryAndPublication(runner);
    Asteria::Test::TestNoFunctionAndAbsentTopology(runner);
    Asteria::Test::TestProbeRangeAndBusBoundary(runner);
    Asteria::Test::TestWidthBeyondAccessIsUnsupported(runner);
    Asteria::Test::TestFunctionCapacityPreflight(runner);
    Asteria::Test::TestPartialPublicationIsNeverReady(runner);
    Asteria::Test::TestTerminalOwnershipFailure(runner);
    Asteria::Test::TestAhciAuthoritativePublicationAndBackendSeparation(runner);
    Asteria::Test::TestOptionalCaptureFailurePreservesEcam(runner);
    Asteria::Test::TestAssignableDevicePublication(runner);
    Asteria::Test::TestWideXhciPublication(runner);
    Asteria::Test::TestAssignableResourceCapture(runner);
    Asteria::Test::TestInterruptLineIsolation(runner);
    Asteria::Test::TestUsbHostDevicePublication(runner);
    Asteria::Test::TestUsbHostsPublication(runner);
    Asteria::Test::TestAhciCatalogCrossMatchFailures(runner);
    Asteria::Test::TestAhciRangeCommandAndBarPreflightFailures(runner);
    Asteria::Test::TestAhciCapacityAndLateFailureRetention(runner);
    Asteria::Test::TestAhciIdentifyOneShotBoundary(runner);
    Asteria::Test::TestAhciIdentifyCompletionEmulatorRejectsMalformedCommands(runner);
    Asteria::Test::TestAhciReadPrerequisiteBoundaries(runner);
    Asteria::Test::TestAhciReadRejectsMalformedSpans(runner);
    Asteria::Test::TestAhciReadTimeoutLifetimeAndReplay(runner);
    Asteria::Test::TestAhciReadSuccessfulExecutionAndQuery(runner);
    return runner.Finish(mode);
}
