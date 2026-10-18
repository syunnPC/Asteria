// SPDX-License-Identifier: Apache-2.0
#if !defined(ASTERIA_TARGET_AMD64)
#error "BootApplicationIntegrationTests requires the AMD64 UEFI ABI profile."
#elif ASTERIA_TARGET_AMD64 != 1
#error "BootApplicationIntegrationTests requires ASTERIA_TARGET_AMD64=1."
#endif

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <sys/prctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "Boot/Uefi/AcpiTableDump.hpp"
#include "Boot/BootInfoValidation.hpp"
#include "Boot/Uefi/KernelLoader.hpp"
#include "User/Filesystem/Crc32c.hpp"
#include "Boot/Uefi/TableValidation.hpp"
#include "Boot/Uefi/UefiTypes.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Memory/InitialMappingPlan.hpp"
#include "Platform/Amd64/Firmware/Amd64LoadedImageLayout.hpp"
#include "Platform/Amd64/Firmware/FirmwarePortIo.hpp"
#include "Platform/Amd64/Boot/Amd64BootRuntime.hpp"
#include "Platform/Amd64/Boot/Amd64MetadataArena.hpp"
#include "Platform/Amd64/Time/Amd64TscClock.hpp"
#include "Platform/Amd64/Diagnostics/Amd64EvidenceRecord.hpp"
#include "Platform/Amd64/Memory/BootstrapMappingPlan.hpp"
#include "Platform/Amd64/Memory/InitialAddressSpacePreparation.hpp"
#include "Platform/Firmware/BootEntropySources.hpp"
#include "Platform/Firmware/Diagnostic.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

extern "C" Asteria::Boot::Uefi::Status ASTERIA_UEFI_API EfiMain(Asteria::Boot::Uefi::Handle imageHandle,
                                                                Asteria::Boot::Uefi::SystemTable* systemTable) noexcept;
extern "C" [[noreturn]] void AsteriaAmd64FailStopInterrupt() noexcept;

namespace Asteria::Boot::Bootstrap
{
extern Platform::Amd64::Firmware::Amd64LoadedImageLayout g_amd64LoadedImageLayout;
extern Kernel::Memory::InitialMappingPlan g_amd64InitialMappingPlan;
extern Platform::Amd64::Memory::BootstrapMappingPlanSummary g_amd64BootstrapMappingPlanSummary;
extern Kernel::Memory::PhysicalPageRange g_amd64BootstrapReservation;
extern Platform::Amd64::Memory::PagingCapabilities g_amd64InitialPagingCapabilities;
extern Platform::Amd64::Memory::PageTableStorage g_amd64PageTableStorage;
extern Platform::Amd64::Memory::WritableInitialMemoryStackView g_amd64InitialMemoryStackView;
extern Platform::Amd64::Memory::WritableInitialDescriptorTableView g_amd64InitialDescriptorTableView;
extern Platform::Amd64::Memory::InitialProcessorStateReport g_amd64InitialProcessorStateReport;
extern Platform::Amd64::Memory::InitialAddressSpacePreparation g_amd64InitialAddressSpacePreparation;
extern BootInfo g_bootInfo;
} // Namespace Asteria::Boot::Bootstrap

namespace Asteria::Test
{
namespace
{
constexpr Foundation::Size DescriptorSize{40U};
constexpr Foundation::Size MemoryDescriptorCount{8U};
constexpr Foundation::Size ReservationDescriptorIndex{2U};
constexpr Foundation::Size ImageBytes{0xA000U};
constexpr Foundation::Size ReservedPageCount{23U};
constexpr Foundation::Size ReservedBytes{ReservedPageCount * 4096U};
constexpr Foundation::Size ConventionalPageCount{64U};
constexpr Foundation::Size ConventionalBytes{ConventionalPageCount * 4096U};
constexpr Foundation::Size AcpiBytes{4096U};
// The fixtures stand in for identity-mapped physical memory. Test-chosen
// addresses keep the firmware memory map and the generated page tables
// independent of address-space layout randomization: each fixture starts on
// its own 2 MiB boundary in the second GiB, so the unmapped guard page at the
// start of the reservation shares its page table with the stack that follows.
constexpr Foundation::UInt64 ImageAddress{0x4000'0000U};
constexpr Foundation::UInt64 ReservedAddress{0x4020'0000U};
constexpr Foundation::UInt64 ConventionalAddress{0x4040'0000U};
constexpr Foundation::UInt64 AcpiAddress{0x4060'0000U};
constexpr Foundation::Size PageTablePageOffset{5U};
constexpr Foundation::Size PageTablePageCount{16U};
constexpr Foundation::Size PageTableEntryCount{PageTablePageCount * 512U};
constexpr Foundation::Size DescriptorTablePageOffset{21U};
constexpr Foundation::Size DescriptorTablePageCount{2U};
constexpr Foundation::UInt64 PageAddressMask{0x000F'FFFF'FFFF'F000ULL};
constexpr Foundation::UInt64 PresentBit{1U};
constexpr Foundation::UInt64 WritableBit{1U << 1U};
constexpr Foundation::UInt64 GlobalBit{1U << 8U};
constexpr Foundation::UInt64 ExecuteDisableBit{Foundation::UInt64{1U} << 63U};
constexpr Foundation::UInt64 NonLeafFlags{PresentBit | WritableBit};
constexpr Foundation::Size PeOffset{0x0080U};
constexpr Foundation::Size OptionalOffset{PeOffset + 24U};
constexpr Foundation::Size SectionTableOffset{OptionalOffset + 240U};
constexpr Foundation::UInt64 WriteBackAttribute{1U << 3U};
constexpr Foundation::UInt64 ExecuteProtectAttribute{Foundation::UInt64{1U} << 14U};
constexpr Foundation::UInt8 ReservedByteSentinel{0xA5U};
constexpr int CallbackAfterExitStatus{20};
constexpr int InvalidPostExitStateStatus{21};
constexpr int EfiMainReturnedStatus{22};
constexpr int ChildSetupFailedStatus{23};
constexpr int ExitBeforePreparationStatus{24};
constexpr int InvalidActivationStateStatus{25};
constexpr int FailStopInterruptStatus{26};
constexpr time_t ChildWaitTimeoutSeconds{4};
constexpr long ChildWaitPollNanoseconds{1'000'000L};
constexpr char BootInfoStageRecord[]{"ASTERIA:STAGE:BOOT-INFO\r\n"};
constexpr char PageAllocatorStageRecord[]{"ASTERIA:STAGE:PAGE-ALLOCATOR\r\n"};
constexpr char InitialMappingPlanStageRecord[]{"ASTERIA:STAGE:AMD64-INITIAL-MAPPING-PLAN\r\n"};
constexpr char InitialPageTableStageRecord[]{"ASTERIA:STAGE:AMD64-INITIAL-PAGE-TABLE\r\n"};
constexpr char InitialAddressSpaceStageRecord[]{"ASTERIA:STAGE:AMD64-INITIAL-ADDRESS-SPACE\r\n"};
constexpr char EvidenceFactPrefix[]{"ASTERIA:EVIDENCE:"};
constexpr Foundation::Size PreExitTranscriptCapacity{8192U};
constexpr char ExitBootServicesStageRecord[]{"ASTERIA:STAGE:EXIT-BOOT-SERVICES\r\n"};
constexpr char FinalMemoryMapFailurePrefix[]{"ASTERIA:AMD64-FINAL-MEMORY-MAP:"};
constexpr char ReservationFailureRecord[]{"ASTERIA:AMD64-FINAL-MEMORY-MAP:FAIL:RESERVATION\r\n"};
constexpr char RetryFailureRecord[]{"ASTERIA:AMD64-FINAL-MEMORY-MAP:FAIL:RETRY\r\n"};
constexpr char ExitBootServicesRecordPrefix[]{"ASTERIA:EXIT-BOOT-SERVICES:"};
constexpr char ExitBootServicesFailureRecord[]{"ASTERIA:EXIT-BOOT-SERVICES:FAIL\r\n"};
constexpr char ExitBootServicesPreparationDetailRecord[]{"ASTERIA:EXIT-BOOT-SERVICES:DETAIL:PREPARATION\r\n"};
// The fixture offers no RNG protocol, no processor generator, and no GetTime
// or monotonic count, so only the cycle-counter jitter source is credited.
constexpr char BootEntropySourcesRecord[]{"ASTERIA:BOOT-ENTROPY:SOURCES:0x0000000000000004\r\n"};
constexpr Foundation::UInt32 GraphicsOutputProtocolGuidData1{0x9042A9DEU};

enum class ChildWaitResult : Foundation::UInt8
{
    Completed,
    TimedOut,
    Failed,
};

enum class IntegrationScenario : Foundation::UInt8
{
    Success,
    AllocationFailure,
    ZeroAddressAllocation,
    RetryTypeChanged,
    RetryAttributesChanged,
    RetryStartChanged,
    RetryCountChanged,
    ApicStateRejected,
    TscCalibrationRejected,
    KernelFileCrcMismatch,
    BootFileCrcMismatch,
    BootFileOversized,
    BootFileMissing,
    BootManifestInvalid,
    BootFileTruncated,
    BootFileGrown,
};

constexpr IntegrationScenario IntegrationScenarios[]{
    IntegrationScenario::Success,
    IntegrationScenario::KernelFileCrcMismatch,
    IntegrationScenario::BootFileCrcMismatch,
    IntegrationScenario::BootFileOversized,
    IntegrationScenario::BootFileMissing,
    IntegrationScenario::BootManifestInvalid,
    IntegrationScenario::BootFileTruncated,
    IntegrationScenario::BootFileGrown,
    IntegrationScenario::AllocationFailure,
    IntegrationScenario::ZeroAddressAllocation,
    IntegrationScenario::RetryTypeChanged,
    IntegrationScenario::RetryAttributesChanged,
    IntegrationScenario::RetryStartChanged,
    IntegrationScenario::RetryCountChanged,
    IntegrationScenario::ApicStateRejected,
    IntegrationScenario::TscCalibrationRejected,
};

[[nodiscard]] constexpr const char* GetExpectedFinalMemoryMapFailureRecord(const IntegrationScenario scenario) noexcept
{
    switch (scenario)
    {
    case IntegrationScenario::RetryTypeChanged:
    case IntegrationScenario::RetryStartChanged:
    case IntegrationScenario::RetryCountChanged:
        return ReservationFailureRecord;
    case IntegrationScenario::RetryAttributesChanged:
        return RetryFailureRecord;
    case IntegrationScenario::KernelFileCrcMismatch:
    case IntegrationScenario::BootFileCrcMismatch:
    case IntegrationScenario::BootFileOversized:
    case IntegrationScenario::BootFileMissing:
    case IntegrationScenario::BootManifestInvalid:
    case IntegrationScenario::BootFileTruncated:
    case IntegrationScenario::BootFileGrown:
    case IntegrationScenario::Success:
    case IntegrationScenario::AllocationFailure:
    case IntegrationScenario::ApicStateRejected:
    case IntegrationScenario::TscCalibrationRejected:
    case IntegrationScenario::ZeroAddressAllocation:
        return nullptr;
    }
    return nullptr;
}

struct IntegrationState
{
    Foundation::UInt32 GetMemoryMapCalls;
    Foundation::UInt32 ExitBootServicesCalls;
    Foundation::UInt32 AllocatePagesCalls;
    Foundation::UInt32 ReportOpenCalls;
    Foundation::UInt32 FailureStallCalls;
    Foundation::UInt32 WatchdogCalls;
    Foundation::UInt32 DesktopCalls;
    bool TextOrderingFailed;
    Foundation::UInt32 FreePagesCalls;
    Foundation::UInt32 RuntimePrepareCalls;
    Foundation::UInt32 BootInfoStageCount;
    Foundation::UInt32 PageAllocatorStageCount;
    Foundation::UInt32 InitialMappingPlanStageCount;
    Foundation::UInt32 InitialPageTableStageCount;
    Foundation::UInt32 InitialAddressSpaceStageCount;
    Foundation::UInt32 ActivationCalls;
    Foundation::UInt32 ExitBootServicesStageCount;
    Foundation::UInt32 FinalMemoryMapFailureCount;
    Foundation::UInt32 ExitBootServicesFailureCount;
    Foundation::UInt32 ExitBootServicesPreparationDetailCount;
    Foundation::UInt32 UnexpectedPreExitFailureRecordCount;
    Foundation::UInt32 UnexpectedPostExitRecordCount;
    bool BootServicesExited;
    bool PreparationObservedBeforeExit;
    bool ActivationArgumentsExact;
    bool EntropyStableAcrossPreparations;
    Foundation::UInt64 CycleCounter;
    Foundation::UInt64 CycleCounterReadings;
    Boot::BootEntropy FirstPreparedEntropy;
};

IntegrationState g_state{};
IntegrationScenario g_scenario{IntegrationScenario::Success};
// Every diagnostic text written before ExitBootServices, in order; the last
// byte stays zero, and text beyond the capacity sets the overflow flag.
char g_preExitTranscript[PreExitTranscriptCapacity]{};
char g_consoleTranscript[PreExitTranscriptCapacity]{};
Foundation::Size g_consoleTranscriptBytes{0U};
Foundation::Size g_preExitTranscriptBytes{0U};
bool g_preExitTranscriptOverflow{false};
Boot::Uefi::SimpleTextOutputMode g_textMode{};
Boot::Uefi::GraphicsOutputModeInformation g_graphicsInformation{};
Boot::Uefi::GraphicsOutputProtocolMode g_graphicsMode{};
Boot::Uefi::GraphicsOutputProtocol g_graphics{};
Boot::Uefi::LoadedImageProtocol g_loadedImage{};
Boot::Uefi::BootServices g_bootServices{};
Boot::Uefi::TableHeader g_runtimeServices{};
Boot::Uefi::ConfigurationTable g_configurationTable{};
Boot::Uefi::SimpleTextOutputProtocol g_console{};
Boot::Uefi::SystemTable g_systemTable{};
Foundation::UInt8* g_loadedImageBytes{nullptr};
Foundation::UInt8* g_reservedBytes{nullptr};
Foundation::UInt8* g_conventionalBytes{nullptr};
Foundation::UInt8* g_acpiBytes{nullptr};
Foundation::UInt64 g_pageTableSnapshot[PageTableEntryCount]{};
alignas(4096) Foundation::UInt8 g_filePages[12288]{};
alignas(4096) Foundation::UInt8 g_kernelFilePages[4096]{};

[[nodiscard]] bool IsReservedStoragePrepared() noexcept;
[[nodiscard]] bool IsPublishedAmd64BootstrapStateValid() noexcept;
[[nodiscard]] Foundation::UInt64* GetPageTableEntries() noexcept;

[[noreturn]] void ExitChild(const int status) noexcept
{
    _exit(status);
}

void ObserveFirmwareCallback() noexcept
{
    if (g_state.BootServicesExited)
    {
        ExitChild(CallbackAfterExitStatus);
    }
}

void Store32(Foundation::UInt8* const bytes, const Foundation::UInt32 value) noexcept
{
    for (Foundation::UInt32 index{0U}; index < 4U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> (index * 8U));
    }
}

void Store16(Foundation::UInt8* const bytes, const Foundation::UInt16 value) noexcept
{
    bytes[0U] = static_cast<Foundation::UInt8>(value);
    bytes[1U] = static_cast<Foundation::UInt8>(value >> 8U);
}

void Store64(Foundation::UInt8* const bytes, const Foundation::UInt64 value) noexcept
{
    for (Foundation::UInt32 index{0U}; index < 8U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> (index * 8U));
    }
}

void WriteMemoryDescriptor(Foundation::UInt8* const bytes, const Foundation::UInt32 type,
                           const Foundation::UInt64 physicalStart, const Foundation::UInt64 pageCount,
                           const Foundation::UInt64 attributes) noexcept
{
    for (Foundation::Size index{0U}; index < DescriptorSize; ++index)
    {
        bytes[index] = 0U;
    }
    Store32(bytes, type);
    Store64(bytes + 8U, physicalStart);
    Store64(bytes + 24U, pageCount);
    Store64(bytes + 32U, attributes);
}

void WriteMemoryMap(void* const memoryMap) noexcept
{
    auto* const bytes = static_cast<Foundation::UInt8*>(memoryMap);
    const Foundation::UInt64 imageStart =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(g_loadedImageBytes));
    const Foundation::UInt64 reservedStart =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(g_reservedBytes));
    const Foundation::UInt64 conventionalStart =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(g_conventionalBytes));
    const Foundation::UInt64 acpiStart =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(g_acpiBytes));
    struct DescriptorRecord
    {
        Foundation::UInt32 Type;
        Foundation::UInt64 PhysicalStart;
        Foundation::UInt64 PageCount;
        Foundation::UInt64 Attributes;
        bool Written;
    };
    DescriptorRecord records[MemoryDescriptorCount]{
        DescriptorRecord{0U, 0U, 1U, 0U, false},
        DescriptorRecord{2U, imageStart, ImageBytes / 4096U, WriteBackAttribute, false},
        DescriptorRecord{g_state.AllocatePagesCalls <= 2U ? 7U : 2U, reservedStart, ReservedPageCount,
                         WriteBackAttribute, false},
        DescriptorRecord{7U, conventionalStart, ConventionalPageCount, WriteBackAttribute, false},
        DescriptorRecord{9U, acpiStart, 1U, 0U, false},
        DescriptorRecord{2U, 0x1000000U, Platform::Amd64::Bootstrap::RuntimePoolPages, WriteBackAttribute, false},
        DescriptorRecord{2U, 0x90000U, 2U, WriteBackAttribute, false},
        DescriptorRecord{2U, reinterpret_cast<Foundation::UIntPointer>(g_filePages), 3U, WriteBackAttribute, false},
    };
    if (g_state.GetMemoryMapCalls >= 4U)
    {
        switch (g_scenario)
        {
        case IntegrationScenario::RetryTypeChanged:
            records[ReservationDescriptorIndex].Type = 7U;
            break;
        case IntegrationScenario::RetryAttributesChanged:
            records[ReservationDescriptorIndex].Attributes = WriteBackAttribute | ExecuteProtectAttribute;
            break;
        case IntegrationScenario::RetryStartChanged:
            records[ReservationDescriptorIndex].PhysicalStart += 4096U;
            --records[ReservationDescriptorIndex].PageCount;
            break;
        case IntegrationScenario::RetryCountChanged:
            --records[ReservationDescriptorIndex].PageCount;
            break;
        case IntegrationScenario::KernelFileCrcMismatch:
        case IntegrationScenario::BootFileCrcMismatch:
        case IntegrationScenario::BootFileOversized:
        case IntegrationScenario::BootFileMissing:
        case IntegrationScenario::BootManifestInvalid:
        case IntegrationScenario::BootFileTruncated:
        case IntegrationScenario::BootFileGrown:
        case IntegrationScenario::Success:
        case IntegrationScenario::AllocationFailure:
        case IntegrationScenario::ApicStateRejected:
        case IntegrationScenario::TscCalibrationRejected:
        case IntegrationScenario::ZeroAddressAllocation:
            break;
        }
    }
    for (Foundation::Size outputIndex{0U}; outputIndex < MemoryDescriptorCount; ++outputIndex)
    {
        Foundation::Size selected{MemoryDescriptorCount};
        for (Foundation::Size candidate{0U}; candidate < MemoryDescriptorCount; ++candidate)
        {
            if (!records[candidate].Written && ((selected == MemoryDescriptorCount) ||
                                                (records[candidate].PhysicalStart < records[selected].PhysicalStart)))
            {
                selected = candidate;
            }
        }
        WriteMemoryDescriptor(bytes + (outputIndex * DescriptorSize), records[selected].Type,
                              records[selected].PhysicalStart, records[selected].PageCount,
                              records[selected].Attributes);
        records[selected].Written = true;
    }
}

void WriteName(Foundation::UInt8* const bytes, const char* const name, const Foundation::Size length) noexcept
{
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        bytes[index] = index < length ? static_cast<Foundation::UInt8>(name[index]) : Foundation::UInt8{0U};
    }
}

void WriteSection(const Foundation::Size index, const char* const name, const Foundation::Size nameLength,
                  const Foundation::UInt32 virtualSize, const Foundation::UInt32 virtualAddress,
                  const Foundation::UInt32 rawSize, const Foundation::UInt32 rawOffset,
                  const Foundation::UInt32 characteristics) noexcept
{
    Foundation::UInt8* const section = g_loadedImageBytes + SectionTableOffset + (index * 40U);
    WriteName(section, name, nameLength);
    Store32(section + 8U, virtualSize);
    Store32(section + 12U, virtualAddress);
    Store32(section + 16U, rawSize);
    Store32(section + 20U, rawOffset);
    Store32(section + 36U, characteristics);
}

void SetUpLoadedImageBytes() noexcept
{
    for (Foundation::Size index{0U}; index < ImageBytes; ++index)
    {
        g_loadedImageBytes[index] = 0U;
    }
    g_loadedImageBytes[0U] = static_cast<Foundation::UInt8>('M');
    g_loadedImageBytes[1U] = static_cast<Foundation::UInt8>('Z');
    Store32(g_loadedImageBytes + 60U, PeOffset);
    g_loadedImageBytes[PeOffset] = static_cast<Foundation::UInt8>('P');
    g_loadedImageBytes[PeOffset + 1U] = static_cast<Foundation::UInt8>('E');
    Store16(g_loadedImageBytes + PeOffset + 4U, 0x8664U);
    Store16(g_loadedImageBytes + PeOffset + 6U, 7U);
    Store16(g_loadedImageBytes + PeOffset + 20U, 240U);
    Store16(g_loadedImageBytes + PeOffset + 22U, 0x022EU);

    Store16(g_loadedImageBytes + OptionalOffset, 0x020BU);
    Store32(g_loadedImageBytes + OptionalOffset + 4U, 0x1800U);
    Store32(g_loadedImageBytes + OptionalOffset + 16U, 0x1170U);
    Store32(g_loadedImageBytes + OptionalOffset + 20U, 0x1000U);
    Store64(g_loadedImageBytes + OptionalOffset + 24U, 0x0400'0000U);
    Store32(g_loadedImageBytes + OptionalOffset + 32U, 0x1000U);
    Store32(g_loadedImageBytes + OptionalOffset + 36U, 0x0200U);
    Store32(g_loadedImageBytes + OptionalOffset + 56U, ImageBytes);
    Store32(g_loadedImageBytes + OptionalOffset + 60U, 0x0400U);
    Store16(g_loadedImageBytes + OptionalOffset + 68U, 10U);
    Store16(g_loadedImageBytes + OptionalOffset + 70U, 0x0160U);
    Store32(g_loadedImageBytes + OptionalOffset + 108U, 16U);
    Store32(g_loadedImageBytes + OptionalOffset + 120U, 0x7000U);
    Store32(g_loadedImageBytes + OptionalOffset + 124U, 24U);
    Store32(g_loadedImageBytes + OptionalOffset + 152U, 0x8000U);
    Store32(g_loadedImageBytes + OptionalOffset + 156U, 0x011CU);

    WriteSection(0U, ".text", 5U, 0x1800U, 0x1000U, 0x1800U, 0x0400U, 0x6000'0020U);
    WriteSection(1U, ".rdata", 6U, 32U, 0x3000U, 0x0200U, 0x1C00U, 0x4000'0040U);
    WriteSection(2U, ".rodata", 7U, 0x0400U, 0x4000U, 0x0400U, 0x1E00U, 0x4000'0040U);
    WriteSection(3U, ".bss", 4U, 0x2000U, 0x5000U, 0U, 0U, 0xC000'0080U);
    WriteSection(4U, ".idata", 6U, 24U, 0x7000U, 0x0200U, 0x2200U, 0x4000'0040U);
    WriteSection(5U, ".reloc", 6U, 0x0180U, 0x8000U, 0x0200U, 0x2400U, 0x4200'0040U);
    WriteSection(6U, ".data", 5U, 16U, 0x9000U, 0x0200U, 0x2600U, 0xC000'0040U);
}

void WriteAcpiSignature(Foundation::UInt8* const bytes, const char* const signature,
                        const Foundation::Size byteCount) noexcept
{
    for (Foundation::Size index{0U}; index < byteCount; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(signature[index]);
    }
}

void RepairAcpiChecksum(Foundation::UInt8* const bytes, const Foundation::Size byteCount,
                        const Foundation::Size checksumOffset) noexcept
{
    bytes[checksumOffset] = 0U;
    Foundation::UInt8 sum{0U};
    for (Foundation::Size index{0U}; index < byteCount; ++index)
    {
        sum = static_cast<Foundation::UInt8>(sum + bytes[index]);
    }
    bytes[checksumOffset] = static_cast<Foundation::UInt8>(0U - sum);
}

void SetUpAcpiTables() noexcept
{
    constexpr Foundation::Size XsdtOffset{64U};
    constexpr Foundation::Size MadtOffset{256U};
    for (Foundation::Size index{0U}; index < AcpiBytes; ++index)
    {
        g_acpiBytes[index] = 0U;
    }

    Foundation::UInt8* const madt = g_acpiBytes + MadtOffset;
    WriteAcpiSignature(madt, "APIC", 4U);
    Store32(madt + 4U, 64U);
    madt[8U] = 6U;
    Store32(madt + 36U, 0xFEE0'0000U);
    Store32(madt + 40U, 1U);
    madt[44U] = 0U;
    madt[45U] = 8U;
    madt[46U] = 0U;
    madt[47U] = 0U;
    Store32(madt + 48U, 1U);
    madt[52U] = 1U;
    madt[53U] = 12U;
    madt[54U] = 0U;
    Store32(madt + 56U, 0xFEC0'0000U);
    Store32(madt + 60U, 0U);
    RepairAcpiChecksum(madt, 64U, 9U);

    Foundation::UInt8* const xsdt = g_acpiBytes + XsdtOffset;
    WriteAcpiSignature(xsdt, "XSDT", 4U);
    Store32(xsdt + 4U, 44U);
    xsdt[8U] = 1U;
    Store64(xsdt + 36U, static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(madt)));
    RepairAcpiChecksum(xsdt, 44U, 9U);

    Foundation::UInt8* const rsdp = g_acpiBytes;
    WriteAcpiSignature(rsdp, "RSD PTR ", 8U);
    WriteAcpiSignature(rsdp + 9U, "ASTERI", 6U);
    rsdp[15U] = 2U;
    Store32(rsdp + 20U, 36U);
    Store64(rsdp + 24U, static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(xsdt)));
    RepairAcpiChecksum(rsdp, 20U, 8U);
    RepairAcpiChecksum(rsdp, 36U, 32U);
}

void RecordUnexpectedPostExitDiagnostic() noexcept
{
    if (g_state.UnexpectedPostExitRecordCount != ~Foundation::UInt32{0U})
    {
        ++g_state.UnexpectedPostExitRecordCount;
    }
}

void RecordExpectedPostExitDiagnostic(Foundation::UInt32& count) noexcept
{
    if (count == ~Foundation::UInt32{0U})
    {
        RecordUnexpectedPostExitDiagnostic();
        return;
    }
    ++count;
}

[[nodiscard]] bool IsExactRecord(const char* const record, const Foundation::Size recordLength,
                                 const char* const expected) noexcept
{
    const Foundation::Size expectedLength = std::strlen(expected);
    return (recordLength == expectedLength) && (std::memcmp(record, expected, recordLength) == 0);
}

void AppendPreExitTranscript(const char* const text) noexcept
{
    const Foundation::Size length = std::strlen(text);
    if (length >= (PreExitTranscriptCapacity - g_preExitTranscriptBytes))
    {
        g_preExitTranscriptOverflow = true;
        return;
    }
    std::memcpy(g_preExitTranscript + g_preExitTranscriptBytes, text, length);
    g_preExitTranscriptBytes += length;
}

void ObserveDiagnosticRecords(const char* const text) noexcept
{
    if (!g_state.BootServicesExited)
    {
        if (text == nullptr)
        {
            return;
        }
        AppendPreExitTranscript(text);
        const char* const expected = GetExpectedFinalMemoryMapFailureRecord(g_scenario);
        if ((expected != nullptr) && (std::strcmp(text, expected) == 0))
        {
            ++g_state.FinalMemoryMapFailureCount;
        }
        else if (std::strcmp(text, ExitBootServicesFailureRecord) == 0)
        {
            ++g_state.ExitBootServicesFailureCount;
        }
        else if (std::strcmp(text, ExitBootServicesPreparationDetailRecord) == 0)
        {
            ++g_state.ExitBootServicesPreparationDetailCount;
        }
        else if ((std::strncmp(text, FinalMemoryMapFailurePrefix, sizeof(FinalMemoryMapFailurePrefix) - 1U) == 0) ||
                 (std::strncmp(text, ExitBootServicesRecordPrefix, sizeof(ExitBootServicesRecordPrefix) - 1U) == 0))
        {
            ++g_state.UnexpectedPreExitFailureRecordCount;
        }
        return;
    }
    if (text == nullptr)
    {
        RecordUnexpectedPostExitDiagnostic();
        return;
    }

    const char* cursor = text;
    while (*cursor != '\0')
    {
        const char* const record = cursor;
        Foundation::Size recordLength{0U};
        while ((*cursor != '\0') && !((cursor[0] == '\r') && (cursor[1] == '\n')))
        {
            ++cursor;
            ++recordLength;
        }
        if (*cursor == '\0')
        {
            RecordUnexpectedPostExitDiagnostic();
            return;
        }

        cursor += 2U;
        recordLength += 2U;
        if (std::strncmp(record, "ASTERIA:BOOT-SERVICES:ABSENT system-table=0x", 41U) == 0)
        {
            char expected[128]{};
            const int length = std::snprintf(
                expected, sizeof(expected), "ASTERIA:BOOT-SERVICES:ABSENT system-table=0x%016llx\r\n",
                static_cast<unsigned long long>(reinterpret_cast<Foundation::UIntPointer>(&g_systemTable)));
            if (length < 0 || recordLength != static_cast<Foundation::Size>(length) ||
                std::memcmp(record, expected, recordLength) != 0 || g_systemTable.BootServicesTable != nullptr ||
                !Boot::Uefi::IsTableCrcValid(&g_systemTable, g_systemTable.Header))
            {
                std::fprintf(stderr, "expired-service fact: got [%.*s], expected [%s], CRC %u\n",
                             static_cast<int>(recordLength), record, expected,
                             Boot::Uefi::IsTableCrcValid(&g_systemTable, g_systemTable.Header) ? 1U : 0U);
                RecordUnexpectedPostExitDiagnostic();
            }
        }
        else if (IsExactRecord(record, recordLength, BootInfoStageRecord))
        {
            RecordExpectedPostExitDiagnostic(g_state.BootInfoStageCount);
        }
        else if (IsExactRecord(record, recordLength, PageAllocatorStageRecord))
        {
            RecordExpectedPostExitDiagnostic(g_state.PageAllocatorStageCount);
        }
        else if (IsExactRecord(record, recordLength, InitialMappingPlanStageRecord))
        {
            RecordExpectedPostExitDiagnostic(g_state.InitialMappingPlanStageCount);
        }
        else if (IsExactRecord(record, recordLength, InitialPageTableStageRecord))
        {
            RecordExpectedPostExitDiagnostic(g_state.InitialPageTableStageCount);
        }
        else if (IsExactRecord(record, recordLength, InitialAddressSpaceStageRecord))
        {
            RecordExpectedPostExitDiagnostic(g_state.InitialAddressSpaceStageCount);
        }
        else if (IsExactRecord(record, recordLength, ExitBootServicesStageRecord))
        {
            RecordExpectedPostExitDiagnostic(g_state.ExitBootServicesStageCount);
        }
        else
        {
            RecordUnexpectedPostExitDiagnostic();
        }
    }
}

Boot::Uefi::Status ASTERIA_UEFI_API Watchdog(const Foundation::Size timeout, const Foundation::UInt64 code,
                                             const Foundation::Size size, Foundation::UInt16* data) noexcept
{
    ObserveFirmwareCallback();
    ++g_state.WatchdogCalls;
    g_state.TextOrderingFailed = g_state.TextOrderingFailed || timeout != 0U || code <= 0xFFFFU || size != 0U ||
                                 data != nullptr || g_consoleTranscriptBytes != 0U;
    return Boot::Uefi::Success;
}

Boot::Uefi::Status ASTERIA_UEFI_API OutputString(Boot::Uefi::SimpleTextOutputProtocol*,
                                                 Foundation::UInt16* const text) noexcept
{
    ObserveFirmwareCallback();
    g_state.TextOrderingFailed =
        g_state.TextOrderingFailed || g_state.WatchdogCalls != 1U || g_state.DesktopCalls == 0U;
    for (Foundation::Size index{0U}; text[index] != 0U && g_consoleTranscriptBytes < PreExitTranscriptCapacity - 1U;
         ++index)
    {
        g_consoleTranscript[g_consoleTranscriptBytes++] = static_cast<char>(text[index]);
    }
    g_consoleTranscript[g_consoleTranscriptBytes] = '\0';
    return Boot::Uefi::Success;
}

Boot::Uefi::Status ASTERIA_UEFI_API QueryMode(Boot::Uefi::SimpleTextOutputProtocol*, Foundation::Size,
                                              Foundation::Size* const columns, Foundation::Size* const rows) noexcept
{
    ObserveFirmwareCallback();
    *columns = 80U;
    *rows = 25U;
    return Boot::Uefi::Success;
}

Boot::Uefi::Status ASTERIA_UEFI_API SetAttribute(Boot::Uefi::SimpleTextOutputProtocol*, Foundation::Size) noexcept
{
    ObserveFirmwareCallback();
    return Boot::Uefi::Success;
}

Boot::Uefi::Status ASTERIA_UEFI_API SetCursorPosition(Boot::Uefi::SimpleTextOutputProtocol*, Foundation::Size,
                                                      Foundation::Size) noexcept
{
    ObserveFirmwareCallback();
    return Boot::Uefi::Success;
}

Boot::Uefi::Status ASTERIA_UEFI_API GraphicsBlt(Boot::Uefi::GraphicsOutputProtocol*,
                                                Boot::Uefi::GraphicsOutputBltPixel*,
                                                Boot::Uefi::GraphicsOutputBltOperation, Foundation::Size,
                                                Foundation::Size, Foundation::Size, Foundation::Size, Foundation::Size,
                                                Foundation::Size, Foundation::Size) noexcept
{
    ObserveFirmwareCallback();
    ++g_state.DesktopCalls;
    if (g_consoleTranscriptBytes != 0U)
    {
        g_state.TextOrderingFailed = true;
    }
    return Boot::Uefi::Success;
}

Boot::Uefi::Status ASTERIA_UEFI_API LocateProtocol(Boot::Uefi::Guid* const protocol, void*,
                                                   void** const interface) noexcept
{
    ObserveFirmwareCallback();
    if (protocol->Data1 != GraphicsOutputProtocolGuidData1)
    {
        *interface = nullptr;
        return Boot::Uefi::NotFound;
    }
    *interface = &g_graphics;
    return Boot::Uefi::Success;
}

constexpr Foundation::UInt8 FileImage[]{0x7FU, 0x45U, 0x4CU, 0x46U};
char g_manifestBytes[384]{};
Boot::Uefi::File g_files[5]{};
Boot::Uefi::File g_root{};
Foundation::Size g_positions[5]{};
bool g_fileOpen[5]{};

Boot::Uefi::Status ASTERIA_UEFI_API FileClose(Boot::Uefi::File* const file) noexcept
{
    ObserveFirmwareCallback();
    for (Foundation::Size index{0U}; index < 5U; ++index)
    {
        if (file == &g_files[index])
        {
            g_fileOpen[index] = false;
        }
    }
    return Boot::Uefi::Success;
}

Boot::Uefi::Status ASTERIA_UEFI_API FileRead(Boot::Uefi::File* const file, Foundation::Size* const count,
                                             void* const bytes) noexcept
{
    ObserveFirmwareCallback();
    const auto index = static_cast<Foundation::Size>(file - g_files);
    const Foundation::Size size = index == 0U ? std::strlen(g_manifestBytes) : sizeof(FileImage);
    if (index == 1U && g_scenario == IntegrationScenario::BootFileTruncated && g_positions[index] >= 2U)
    {
        *count = 0U;
        return Boot::Uefi::Success;
    }
    if (index == 1U && g_scenario == IntegrationScenario::BootFileGrown && g_positions[index] == size)
    {
        *count = 1U;
        *static_cast<Foundation::UInt8*>(bytes) = 0xA5U;
        return Boot::Uefi::Success;
    }
    const Foundation::Size remaining = size - g_positions[index];
    if (*count > remaining)
    {
        *count = remaining;
    }
    // Short successful reads exercise the firmware adapter's whole-file loop.
    if (*count > 2U)
    {
        *count = 2U;
    }
    const void* const source = index == 0U ? static_cast<const void*>(g_manifestBytes + g_positions[index])
                                           : static_cast<const void*>(FileImage + g_positions[index]);
    std::memcpy(bytes, source, *count);
    g_positions[index] += *count;
    return Boot::Uefi::Success;
}

Boot::Uefi::Status ASTERIA_UEFI_API FileInfo(Boot::Uefi::File* const file, Boot::Uefi::Guid*,
                                             Foundation::Size* const count, void* const bytes) noexcept
{
    ObserveFirmwareCallback();
    if (*count < 82U)
    {
        return Boot::Uefi::BufferTooSmall;
    }
    *count = 82U;
    auto* const data = static_cast<Foundation::UInt8*>(bytes);
    std::memset(data, 0, *count);
    Store64(data, 82U);
    Foundation::UInt64 size{sizeof(FileImage)};
    if (file == &g_files[0])
    {
        size = std::strlen(g_manifestBytes);
    }
    else if (g_scenario == IntegrationScenario::BootFileOversized)
    {
        size = Boot::BootFileMaximumBytes + 1U;
    }
    Store64(data + 8U, size);
    return Boot::Uefi::Success;
}

Boot::Uefi::Status ASTERIA_UEFI_API FileOpen(Boot::Uefi::File*, Boot::Uefi::File** const result,
                                             Foundation::UInt16* const path, const Foundation::UInt64 mode,
                                             Foundation::UInt64) noexcept
{
    ObserveFirmwareCallback();
    if (mode != 1U)
    {
        ++g_state.ReportOpenCalls;
        return Boot::Uefi::DeviceError;
    }
    constexpr const char* names[]{"\\EFI\\ASTERIA\\BOOT.CFG", "\\EFI\\ASTERIA\\INIT.ELF", "\\EFI\\ASTERIA\\USER.ELF",
                                  "\\EFI\\ASTERIA\\MODULES\\COMP.ELF", "\\EFI\\ASTERIA\\KERNEL.ELF"};
    for (Foundation::Size index{0U}; index < 5U; ++index)
    {
        Foundation::Size character{0U};
        while (names[index][character] != '\0' && path[character] == names[index][character])
        {
            ++character;
        }
        if (names[index][character] == '\0' && path[character] == 0U)
        {
            // Root plus one payload handle must suffice even after adding data modules to a full
            // module table.
            bool busy{false};
            for (const bool open : g_fileOpen)
            {
                busy = busy || open;
            }
            if (busy)
            {
                return Boot::Uefi::DeviceError;
            }
            if (index == 1U && g_scenario == IntegrationScenario::BootFileMissing)
            {
                return Boot::Uefi::DeviceError;
            }
            g_files[index] = {};
            g_files[index].Read = FileRead;
            g_files[index].GetInfo = FileInfo;
            g_files[index].Close = FileClose;
            g_positions[index] = 0U;
            g_fileOpen[index] = true;
            *result = &g_files[index];
            return Boot::Uefi::Success;
        }
    }
    return Boot::Uefi::DeviceError;
}

Boot::Uefi::Status ASTERIA_UEFI_API OpenVolume(Boot::Uefi::FileSystem*, Boot::Uefi::File** const root) noexcept
{
    ObserveFirmwareCallback();
    for (bool& open : g_fileOpen)
    {
        open = false;
    }
    constexpr Foundation::UInt32 actual = User::Filesystem::Crc32c::Compute(FileImage, sizeof(FileImage));
    const Foundation::UInt32 crc = g_scenario == IntegrationScenario::BootFileCrcMismatch ? actual ^ 1U : actual;
    const Foundation::UInt32 kernelCrc =
        g_scenario == IntegrationScenario::KernelFileCrcMismatch ? actual ^ 1U : actual;
    static_cast<void>(std::snprintf(
        g_manifestBytes, sizeof(g_manifestBytes),
        "version = 1\nkernel = KERNEL.ELF crc32c=0x%08x\ninit = INIT.ELF crc32c=0x%08x\nuser = USER.ELF crc32c=0x%08x\nmodule = component COMP.ELF crc32c=0x%08x\n",
        kernelCrc, crc, crc, actual));
    if (g_scenario == IntegrationScenario::BootManifestInvalid)
    {
        g_manifestBytes[10] = '2';
    }
    g_root = {};
    g_root.Open = FileOpen;
    g_root.Close = FileClose;
    *root = &g_root;
    return Boot::Uefi::Success;
}

Boot::Uefi::FileSystem g_fileSystem{0U, OpenVolume};

Boot::Uefi::Status ASTERIA_UEFI_API HandleProtocol(Boot::Uefi::Handle, Boot::Uefi::Guid* const protocol,
                                                   void** const interface) noexcept
{
    ObserveFirmwareCallback();
    // Expose only the actual loaded-image and filesystem interfaces; another
    // GUID must never fabricate a Block I/O or storage capability.
    if (protocol == nullptr)
    {
        *interface = nullptr;
        return Boot::Uefi::NotFound;
    }
    if (protocol->Data1 == 0x964E5B22U)
    {
        *interface = &g_fileSystem;
    }
    else if (protocol->Data1 == 0x5B1B31A1U)
    {
        *interface = &g_loadedImage;
    }
    else
    {
        *interface = nullptr;
        return Boot::Uefi::NotFound;
    }
    return Boot::Uefi::Success;
}

Boot::Uefi::Status ASTERIA_UEFI_API Stall(const Foundation::Size microseconds) noexcept
{
    ObserveFirmwareCallback();
    if (microseconds == 30'000'000U)
    {
        ++g_state.FailureStallCalls;
    }
    return Boot::Uefi::Success;
}

Boot::Uefi::Status ASTERIA_UEFI_API AllocatePages(const Boot::Uefi::AllocateType allocateType,
                                                  const Boot::Uefi::MemoryType memoryType,
                                                  const Foundation::Size pageCount,
                                                  Foundation::UInt64* const physicalStart) noexcept
{
    ObserveFirmwareCallback();
    ++g_state.AllocatePagesCalls;
    if (g_state.AllocatePagesCalls == 1U)
    {
        if (allocateType != Boot::Uefi::AllocateType::AnyPages || memoryType != Boot::Uefi::MemoryType::LoaderData ||
            pageCount != 3U || physicalStart == nullptr)
        {
            return Boot::Uefi::InvalidParameter;
        }
        *physicalStart = reinterpret_cast<Foundation::UInt64>(g_filePages);
        return Boot::Uefi::Success;
    }

    if (g_state.AllocatePagesCalls == 2U)
    {
        if (allocateType != Boot::Uefi::AllocateType::AnyPages || memoryType != Boot::Uefi::MemoryType::LoaderData ||
            pageCount != 1U || physicalStart == nullptr)
        {
            return Boot::Uefi::InvalidParameter;
        }
        *physicalStart = reinterpret_cast<Foundation::UInt64>(g_kernelFilePages);
        return Boot::Uefi::Success;
    }
    if ((g_state.AllocatePagesCalls == 4U) || (g_state.AllocatePagesCalls == 5U))
    {
        const bool pool = g_state.AllocatePagesCalls == 4U;
        if ((allocateType != Boot::Uefi::AllocateType::MaximumAddress) ||
            (memoryType != Boot::Uefi::MemoryType::LoaderData) ||
            (pageCount != (pool ? Platform::Amd64::Bootstrap::RuntimePoolPages : 2U)) || (physicalStart == nullptr) ||
            (*physicalStart != (pool ? 0xFFFF'FFFFU : 0xFFFFFU)))
        {
            return Boot::Uefi::InvalidParameter;
        }
        *physicalStart = pool ? 0x1000000U : 0x90000U;
        return Boot::Uefi::Success;
    }
    if ((allocateType != Boot::Uefi::AllocateType::AnyPages) || (memoryType != Boot::Uefi::MemoryType::LoaderData) ||
        (pageCount != ReservedPageCount) || (physicalStart == nullptr) || (g_state.AllocatePagesCalls != 3U))
    {
        return Boot::Uefi::InvalidParameter;
    }
    if (g_scenario == IntegrationScenario::AllocationFailure)
    {
        return Boot::Uefi::DeviceError;
    }
    *physicalStart = g_scenario == IntegrationScenario::ZeroAddressAllocation
                         ? 0U
                         : static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(g_reservedBytes));
    return Boot::Uefi::Success;
}

Boot::Uefi::Status ASTERIA_UEFI_API FreePages(const Foundation::UInt64 physicalStart,
                                              const Foundation::Size pageCount) noexcept
{
    ObserveFirmwareCallback();
    ++g_state.FreePagesCalls;
    if (physicalStart == reinterpret_cast<Foundation::UInt64>(g_kernelFilePages) && pageCount == 1U &&
        g_state.FreePagesCalls == 1U)
    {
        return Boot::Uefi::Success;
    }
    const Foundation::UInt64 expected =
        g_scenario == IntegrationScenario::ZeroAddressAllocation
            ? 0U
            : static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(g_reservedBytes));
    return ((physicalStart == expected) && (pageCount == ReservedPageCount) && (g_state.FreePagesCalls == 2U))
               ? Boot::Uefi::Success
               : Boot::Uefi::InvalidParameter;
}

Boot::Uefi::Status ASTERIA_UEFI_API GetMemoryMap(Foundation::Size* const memoryMapSize, void* const memoryMap,
                                                 Foundation::Size* const memoryMapKey,
                                                 Foundation::Size* const descriptorSize,
                                                 Foundation::UInt32* const descriptorVersion) noexcept
{
    ObserveFirmwareCallback();
    ++g_state.GetMemoryMapCalls;
    if ((memoryMapSize == nullptr) || (memoryMap == nullptr) ||
        (*memoryMapSize < (DescriptorSize * MemoryDescriptorCount)) || (memoryMapKey == nullptr) ||
        (descriptorSize == nullptr) || (descriptorVersion == nullptr))
    {
        return Boot::Uefi::InvalidParameter;
    }

    WriteMemoryMap(memoryMap);
    *memoryMapSize = DescriptorSize * MemoryDescriptorCount;
    *memoryMapKey = 0x1234U;
    *descriptorSize = DescriptorSize;
    *descriptorVersion = Boot::Uefi::MemoryDescriptorVersion;
    return Boot::Uefi::Success;
}

Boot::Uefi::Status ASTERIA_UEFI_API ExitBootServices(Boot::Uefi::Handle, Foundation::Size) noexcept
{
    ObserveFirmwareCallback();
    ++g_state.ExitBootServicesCalls;
    if (!Boot::ValidateBootInfo(Boot::Bootstrap::g_bootInfo) ||
        Boot::Bootstrap::g_bootInfo.Kernel.TimerTicksPerMicrosecond != 1000U || !IsReservedStoragePrepared() ||
        !IsPublishedAmd64BootstrapStateValid())
    {
        ExitChild(ExitBeforePreparationStatus);
    }
    g_state.PreparationObservedBeforeExit = true;
    const Foundation::UInt64* const entries = GetPageTableEntries();
    if (g_state.ExitBootServicesCalls == 1U)
    {
        for (Foundation::Size index{0U}; index < PageTableEntryCount; ++index)
        {
            g_pageTableSnapshot[index] = entries[index];
        }
        return Boot::Uefi::InvalidParameter;
    }
    for (Foundation::Size index{0U}; index < PageTableEntryCount; ++index)
    {
        if (entries[index] != g_pageTableSnapshot[index])
        {
            ExitChild(ExitBeforePreparationStatus);
        }
    }
    g_state.BootServicesExited = true;
    return Boot::Uefi::Success;
}

void SetTableCrc(Boot::Uefi::TableHeader& header, const void* const table) noexcept
{
    header.Crc32 = 0U;
    header.Crc32 = Boot::Uefi::ComputeCrc32(table, header.HeaderSize);
}

[[nodiscard]] Foundation::UInt8* MapIdentityFixture(const Foundation::UInt64 address,
                                                    const Foundation::Size sizeBytes) noexcept
{
    void* const requested = reinterpret_cast<void*>(static_cast<Foundation::UIntPointer>(address));
    void* const mapped =
        mmap(requested, sizeBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    return (mapped == requested) ? static_cast<Foundation::UInt8*>(mapped) : nullptr;
}

[[nodiscard]] bool MapIdentityFixtures() noexcept
{
    g_loadedImageBytes = MapIdentityFixture(ImageAddress, ImageBytes);
    g_reservedBytes = MapIdentityFixture(ReservedAddress, ReservedBytes);
    g_conventionalBytes = MapIdentityFixture(ConventionalAddress, ConventionalBytes);
    g_acpiBytes = MapIdentityFixture(AcpiAddress, AcpiBytes);
    return (g_loadedImageBytes != nullptr) && (g_reservedBytes != nullptr) && (g_conventionalBytes != nullptr) &&
           (g_acpiBytes != nullptr);
}

[[nodiscard]] bool SetUpFirmwareTables() noexcept
{
    if (!MapIdentityFixtures())
    {
        return false;
    }
    g_state = {};
    g_consoleTranscriptBytes = 0U;
    g_consoleTranscript[0U] = '\0';
    SetUpLoadedImageBytes();
    SetUpAcpiTables();
    for (Foundation::Size index{0U}; index < ReservedBytes; ++index)
    {
        g_reservedBytes[index] = ReservedByteSentinel;
    }

    g_textMode = {};
    g_textMode.Mode = 0;
    g_console = {};
    g_console.OutputString = OutputString;
    g_console.QueryMode = QueryMode;
    g_console.SetAttribute = SetAttribute;
    g_console.SetCursorPosition = SetCursorPosition;
    g_console.Mode = &g_textMode;

    g_graphicsInformation = {};
    g_graphicsInformation.HorizontalResolution = 1024U;
    g_graphicsInformation.VerticalResolution = 768U;
    g_graphicsMode = {};
    g_graphicsMode.MaximumMode = 1U;
    g_graphicsMode.Mode = 0U;
    g_graphicsMode.Information = &g_graphicsInformation;
    g_graphicsMode.InformationSize = sizeof(g_graphicsInformation);
    g_graphics = {};
    g_graphics.Blt = GraphicsBlt;
    g_graphics.Mode = &g_graphicsMode;

    g_bootServices = {};
    g_bootServices.Header.Signature = Boot::Uefi::BootServicesSignature;
    g_bootServices.Header.Revision = Boot::Uefi::MinimumSupportedTableRevision;
    g_bootServices.Header.HeaderSize = sizeof(g_bootServices);
    g_bootServices.AllocatePagesFunction = AllocatePages;
    g_bootServices.FreePagesFunction = FreePages;
    g_bootServices.GetMemoryMapFunction = GetMemoryMap;
    g_bootServices.HandleProtocolFunction = HandleProtocol;
    g_bootServices.ExitBootServicesFunction = ExitBootServices;
    g_bootServices.StallFunction = Stall;
    g_bootServices.SetWatchdogTimerFunction = Watchdog;
    g_bootServices.LocateProtocolFunction = LocateProtocol;
    SetTableCrc(g_bootServices.Header, &g_bootServices);

    g_runtimeServices = {};
    g_runtimeServices.Signature = Boot::Uefi::RuntimeServicesSignature;
    g_runtimeServices.Revision = Boot::Uefi::MinimumSupportedTableRevision;
    g_runtimeServices.HeaderSize = sizeof(g_runtimeServices);
    SetTableCrc(g_runtimeServices, &g_runtimeServices);

    g_configurationTable = {
        .VendorGuid =
            Boot::Uefi::Guid{0x8868E871U, 0xE4F1U, 0x11D3U, {0xBCU, 0x22U, 0x00U, 0x80U, 0xC7U, 0x3CU, 0x88U, 0x81U}},
        .VendorTable = g_acpiBytes,
    };

    g_systemTable = {};
    g_systemTable.Header.Signature = Boot::Uefi::SystemTableSignature;
    g_systemTable.Header.Revision = Boot::Uefi::MinimumSupportedTableRevision;
    g_systemTable.Header.HeaderSize = sizeof(g_systemTable);
    g_systemTable.ConsoleOut = &g_console;
    g_systemTable.RuntimeServices = &g_runtimeServices;
    g_systemTable.BootServicesTable = &g_bootServices;
    g_systemTable.NumberOfTableEntries = 1U;
    g_systemTable.ConfigurationTables = &g_configurationTable;
    SetTableCrc(g_systemTable.Header, &g_systemTable);

    g_loadedImage = {};
    g_loadedImage.ImageSystemTable = &g_systemTable;
    g_loadedImage.DeviceHandle = nullptr;
    g_loadedImage.ImageBase = g_loadedImageBytes;
    g_loadedImage.ImageSize = ImageBytes;
    return true;
}

[[nodiscard]] bool TryGetChildTablePage(const Foundation::UInt64 entry, const Foundation::UInt64 storagePhysicalStart,
                                        const Foundation::Size usedTablePages, Foundation::Size& page) noexcept
{
    if ((entry & ~PageAddressMask) != NonLeafFlags)
    {
        return false;
    }
    const Foundation::UInt64 physicalAddress = entry & PageAddressMask;
    if (physicalAddress < storagePhysicalStart)
    {
        return false;
    }
    const Foundation::UInt64 offset = physicalAddress - storagePhysicalStart;
    if ((offset & 0xFFFU) != 0U)
    {
        return false;
    }
    page = static_cast<Foundation::Size>(offset >> 12U);
    return page < usedTablePages;
}

[[nodiscard]] Foundation::UInt64* GetPageTableEntries() noexcept
{
    const Foundation::UIntPointer address = reinterpret_cast<Foundation::UIntPointer>(g_reservedBytes) +
                                            static_cast<Foundation::UIntPointer>(PageTablePageOffset * 4096U);
    return reinterpret_cast<Foundation::UInt64*>(address);
}

[[nodiscard]] bool TryReadLeaf(const Foundation::UInt64* const entries, const Foundation::UInt64 storagePhysicalStart,
                               const Foundation::Size usedTablePages, const Foundation::UInt64 virtualAddress,
                               Foundation::UInt64& leaf) noexcept
{
    const Foundation::Size indices[4U]{
        static_cast<Foundation::Size>((virtualAddress >> 39U) & 0x1FFU),
        static_cast<Foundation::Size>((virtualAddress >> 30U) & 0x1FFU),
        static_cast<Foundation::Size>((virtualAddress >> 21U) & 0x1FFU),
        static_cast<Foundation::Size>((virtualAddress >> 12U) & 0x1FFU),
    };
    Foundation::Size page{0U};
    for (Foundation::Size level{0U}; level < 3U; ++level)
    {
        const Foundation::UInt64 entry = entries[(page * 512U) + indices[level]];
        if (!TryGetChildTablePage(entry, storagePhysicalStart, usedTablePages, page))
        {
            return false;
        }
    }
    leaf = entries[(page * 512U) + indices[3U]];
    return true;
}

[[nodiscard]] bool IsGeneratedPageTableExact() noexcept
{
    using Kernel::Memory::MappingPermission;

    const auto preparationResult = Boot::Bootstrap::g_amd64InitialAddressSpacePreparation.TryGetPageTablePreparation();
    if (!preparationResult.HasValue())
    {
        return false;
    }
    const auto& preparation = *preparationResult.GetValue();
    const auto summaryResult = preparation.TryGetSummary();
    const auto retainedPlanResult = preparation.TryGetMappingPlan();
    if (!summaryResult.HasValue() || !retainedPlanResult.HasValue())
    {
        return false;
    }
    const auto& summary = *summaryResult.GetValue();
    const auto& retainedPlan = *retainedPlanResult.GetValue();
    const Foundation::UInt64 reservedStart =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(g_reservedBytes));
    const Foundation::UInt64 storagePhysicalStart = reservedStart + (PageTablePageOffset * 4096U);
    const Foundation::UInt64* const entries = GetPageTableEntries();
    if (!Boot::Bootstrap::g_amd64InitialAddressSpacePreparation.IsPrepared() || !retainedPlan.IsSealed() ||
        (retainedPlan.GetMappingCount() != 10U) ||
        (summary.Storage.RootPhysicalAddress.GetValue() != storagePhysicalStart) ||
        (summary.Storage.Entries != entries) || (summary.Storage.PageCapacity != PageTablePageCount) ||
        (summary.Storage.UsedTablePages == 0U) || (summary.Storage.UsedTablePages > 13U) ||
        (summary.MappedPages != 31U) || (summary.MappingCount != 10U) || (summary.Reserved16 != 0U) ||
        (summary.Reserved32 != 0U))
    {
        return false;
    }

    Foundation::UInt64 mappedPages{0U};
    for (Foundation::Size mappingIndex{0U}; mappingIndex < retainedPlan.GetMappingCount(); ++mappingIndex)
    {
        const auto mappingResult = retainedPlan.GetMapping(mappingIndex);
        if (!mappingResult.HasValue())
        {
            return false;
        }
        const Kernel::Memory::InitialMapping mapping = mappingResult.GetValue();
        for (Foundation::UInt64 page{0U}; page < mapping.Pages.GetValue(); ++page)
        {
            const Foundation::UInt64 virtualAddress = mapping.VirtualStart.GetValue() + (page * 4096U);
            const Foundation::UInt64 physicalAddress = mapping.PhysicalStart.GetValue() + (page * 4096U);
            Foundation::UInt64 leaf{0U};
            if (!TryReadLeaf(entries, storagePhysicalStart, summary.Storage.UsedTablePages, virtualAddress, leaf))
            {
                return false;
            }
            Foundation::UInt64 expected = physicalAddress | PresentBit;
            if (mapping.Permissions.Contains(MappingPermission::Write))
            {
                expected |= WritableBit;
            }
            if (mapping.Permissions.Contains(MappingPermission::Global))
            {
                expected |= GlobalBit;
            }
            if (!mapping.Permissions.Contains(MappingPermission::Execute))
            {
                expected |= ExecuteDisableBit;
            }
            if (leaf != expected)
            {
                return false;
            }
            ++mappedPages;
        }
    }

    Foundation::Size nonzeroEntries{0U};
    for (Foundation::Size index{0U}; index < PageTableEntryCount; ++index)
    {
        if (entries[index] != 0U)
        {
            ++nonzeroEntries;
        }
        if ((index >= (summary.Storage.UsedTablePages * 512U)) && (entries[index] != 0U))
        {
            return false;
        }
    }
    Foundation::UInt64 guardLeaf{1U};
    if (!TryReadLeaf(entries, storagePhysicalStart, summary.Storage.UsedTablePages, reservedStart, guardLeaf))
    {
        return false;
    }
    return (guardLeaf == 0U) && (mappedPages == summary.MappedPages) &&
           (nonzeroEntries == (summary.Storage.UsedTablePages - 1U) + static_cast<Foundation::Size>(mappedPages));
}

[[nodiscard]] bool DescriptorTablesAreExact(const Foundation::UInt16 codeSelector,
                                            const Foundation::UInt16 stackSelector) noexcept
{
    const Foundation::UIntPointer descriptorAddress =
        reinterpret_cast<Foundation::UIntPointer>(g_reservedBytes) +
        static_cast<Foundation::UIntPointer>(DescriptorTablePageOffset * 4096U);
    const auto* const descriptors = reinterpret_cast<const Foundation::UInt64*>(descriptorAddress);
    const Foundation::Size codeIndex = codeSelector >> 3U;
    const Foundation::Size stackIndex = stackSelector >> 3U;
    constexpr Foundation::UInt64 codeDescriptor{0x00AF'9A00'0000'FFFFULL};
    constexpr Foundation::UInt64 dataDescriptor{0x00CF'9200'0000'FFFFULL};
    for (Foundation::Size index{0U}; index < 512U; ++index)
    {
        const Foundation::UInt64 expected =
            index == codeIndex ? codeDescriptor : (index == stackIndex ? dataDescriptor : 0U);
        if (descriptors[index] != expected)
        {
            return false;
        }
    }
    const Foundation::UInt64 handler =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(&AsteriaAmd64FailStopInterrupt));
    const Foundation::UInt64 gateLow = (handler & 0xFFFFU) | (static_cast<Foundation::UInt64>(codeSelector) << 16U) |
                                       (Foundation::UInt64{0x8EU} << 40U) | (((handler >> 16U) & 0xFFFFU) << 48U);
    const Foundation::UInt64 gateHigh = handler >> 32U;
    for (Foundation::Size index{0U}; index < 256U; ++index)
    {
        if ((descriptors[512U + (index * 2U)] != gateLow) || (descriptors[513U + (index * 2U)] != gateHigh))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool IsReservedStoragePrepared() noexcept
{
    for (Foundation::Size index{0U}; index < 4096U; ++index)
    {
        if (g_reservedBytes[index] != ReservedByteSentinel)
        {
            return false;
        }
    }
    for (Foundation::Size index{4096U}; index < (PageTablePageOffset * 4096U); ++index)
    {
        if (g_reservedBytes[index] != 0U)
        {
            return false;
        }
    }
    return IsGeneratedPageTableExact() &&
           DescriptorTablesAreExact(Boot::Bootstrap::g_amd64InitialProcessorStateReport.CodeSelector,
                                    Boot::Bootstrap::g_amd64InitialProcessorStateReport.StackSelector);
}

/// The pre-exit transcript names the AMD64 evidence record once, as
/// `ASTERIA:EVIDENCE:0x<16 upper-case hex digits>`, and the record it names
/// carries the common header of the AMD64 schema.
[[nodiscard]] bool EvidenceRecordIsReported() noexcept
{
    const auto& record = Platform::Amd64::Diagnostics::GetAmd64EvidenceRecord();
    char expected[64]{};
    const int length =
        std::snprintf(expected, sizeof(expected), "%s0x%016llX\r\n", EvidenceFactPrefix,
                      static_cast<unsigned long long>(reinterpret_cast<Foundation::UIntPointer>(&record)));
    const char* const first = std::strstr(g_preExitTranscript, EvidenceFactPrefix);
    return !g_preExitTranscriptOverflow && (length > 0) && (first != nullptr) &&
           (std::strncmp(first, expected, static_cast<Foundation::Size>(length)) == 0) &&
           (std::strstr(first + 1, EvidenceFactPrefix) == nullptr) &&
           (record.Header.Magic == Kernel::Diagnostics::EvidenceRecordMagic) &&
           (record.Header.Schema == Platform::Amd64::Diagnostics::Amd64EvidenceRecordSchema) &&
           (record.Header.SizeBytes == sizeof(record));
}

/// The handoff carries the published seed of the fixture's only credited
/// Source, and the pre-exit transcript reports that source once.
[[nodiscard]] bool IsPublishedEntropyValid() noexcept
{
    const Boot::BootEntropy& entropy = Boot::Bootstrap::g_bootInfo.Entropy;
    const char* const first = std::strstr(g_preExitTranscript, BootEntropySourcesRecord);
    return !g_preExitTranscriptOverflow && (first != nullptr) &&
           (std::strstr(first + 1, "ASTERIA:BOOT-ENTROPY:") == nullptr) &&
           (Boot::GetBootEntropyState(entropy) == Boot::BootEntropyState::Published) &&
           (entropy.Sources == Boot::GetBootEntropySourceMask(Boot::BootEntropySource::CycleCounterJitter)) &&
           g_state.EntropyStableAcrossPreparations;
}

[[nodiscard]] bool GuardAndStackRemainExact() noexcept
{
    for (Foundation::Size index{0U}; index < 4096U; ++index)
    {
        if (g_reservedBytes[index] != ReservedByteSentinel)
        {
            return false;
        }
    }
    for (Foundation::Size index{4096U}; index < (PageTablePageOffset * 4096U); ++index)
    {
        if (g_reservedBytes[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool IsPublishedAmd64BootstrapStateValid() noexcept
{
    const char* const acpi{std::strstr(g_consoleTranscript, "ASTERIA:STAGE:ACPI\r\n")};
    const char* const graphics{std::strstr(g_consoleTranscript, "ASTERIA:STAGE:GRAPHICS\r\n")};
    const char* const files{std::strstr(g_consoleTranscript, "ASTERIA:STAGE:BOOT-FILE:")};
    if (acpi == nullptr || graphics == nullptr || files == nullptr || !(acpi < graphics && graphics < files))
    {
        return false;
    }
    const Foundation::UInt64 imageStart =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(g_loadedImageBytes));
    const Foundation::UInt64 reservedStart =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(g_reservedBytes));
    const auto& summary = Boot::Bootstrap::g_amd64BootstrapMappingPlanSummary;
    return (Boot::Bootstrap::g_amd64LoadedImageLayout.ImageBase == imageStart) &&
           (Boot::Bootstrap::g_amd64LoadedImageLayout.ImageSize == ImageBytes) &&
           Boot::Bootstrap::g_amd64InitialMappingPlan.IsSealed() &&
           (Boot::Bootstrap::g_amd64InitialMappingPlan.GetMappingCount() == 10U) &&
           (summary.ReservedPhysicalStart == reservedStart) &&
           (summary.StackPhysicalStart == (reservedStart + 4096U)) &&
           (summary.PageTableStoragePhysicalStart == (reservedStart + (5U * 4096U))) &&
           (summary.DescriptorTableStoragePhysicalStart == (reservedStart + (21U * 4096U))) &&
           (summary.MappingCount == 10U) && (summary.Reserved16 == 0U) && (summary.Reserved32 == 0U) &&
           (Boot::Bootstrap::g_amd64BootstrapReservation.PhysicalStart == reservedStart) &&
           (Boot::Bootstrap::g_amd64BootstrapReservation.PageCount == ReservedPageCount) &&
           (Boot::Bootstrap::g_amd64BootstrapReservation.Attributes == WriteBackAttribute) &&
           (Boot::Bootstrap::g_amd64BootstrapReservation.FirmwareType == 2U) &&
           (Boot::Bootstrap::g_amd64BootstrapReservation.Reserved == 0U) &&
           (Boot::Bootstrap::g_amd64InitialPagingCapabilities.PhysicalAddressBits >= 32U) &&
           (Boot::Bootstrap::g_amd64InitialPagingCapabilities.PhysicalAddressBits <= 52U) &&
           Boot::Bootstrap::g_amd64InitialPagingCapabilities.FourLevelPagingEnabled &&
           Boot::Bootstrap::g_amd64InitialPagingCapabilities.ExecuteDisableEnabled &&
           Boot::Bootstrap::g_amd64InitialPagingCapabilities.SupervisorWriteProtectEnabled &&
           Boot::Bootstrap::g_amd64InitialPagingCapabilities.GlobalPagesEnabled &&
           Boot::Bootstrap::g_amd64InitialPagingCapabilities.PatLayoutConfigured &&
           (Boot::Bootstrap::g_amd64InitialPagingCapabilities.Reserved16 == 0U) &&
           (Boot::Bootstrap::g_amd64InitialPagingCapabilities.Reserved == 0U) &&
           (Boot::Bootstrap::g_amd64PageTableStorage.PhysicalStart.GetValue() ==
            summary.PageTableStoragePhysicalStart) &&
           (Boot::Bootstrap::g_amd64PageTableStorage.Entries == GetPageTableEntries()) &&
           (Boot::Bootstrap::g_amd64PageTableStorage.PageCapacity == PageTablePageCount) &&
           (Boot::Bootstrap::g_amd64PageTableStorage.Reserved == 0U) &&
           (Boot::Bootstrap::g_amd64InitialMemoryStackView.Data == (g_reservedBytes + 4096U)) &&
           (Boot::Bootstrap::g_amd64InitialMemoryStackView.SizeBytes == 4U * 4096U) &&
           (Boot::Bootstrap::g_amd64InitialMemoryStackView.PhysicalStart == summary.StackPhysicalStart) &&
           (reinterpret_cast<Foundation::UIntPointer>(Boot::Bootstrap::g_amd64InitialDescriptorTableView.Data) ==
            (reinterpret_cast<Foundation::UIntPointer>(g_reservedBytes) + (DescriptorTablePageOffset * 4096U))) &&
           (Boot::Bootstrap::g_amd64InitialDescriptorTableView.SizeBytes == DescriptorTablePageCount * 4096U) &&
           (Boot::Bootstrap::g_amd64InitialDescriptorTableView.PhysicalStart ==
            summary.DescriptorTableStoragePhysicalStart) &&
           (Boot::Bootstrap::g_amd64InitialProcessorStateReport.Cr0 == 0x8001'0033U) &&
           (Boot::Bootstrap::g_amd64InitialProcessorStateReport.Cr3 == 0x0010'0000U) &&
           (Boot::Bootstrap::g_amd64InitialProcessorStateReport.Cr4 == 0x0000'0668U) &&
           (Boot::Bootstrap::g_amd64InitialProcessorStateReport.Efer == 0x0000'0D00U) &&
           Boot::Bootstrap::g_amd64InitialAddressSpacePreparation.IsPrepared() &&
           (Boot::Bootstrap::g_amd64InitialAddressSpacePreparation.ValidatePreparedState() ==
            Platform::Amd64::Memory::InitialAddressSpacePreparationError::None);
}

[[nodiscard]] bool IsPublishedAcpiTopologyValid() noexcept
{
    const Foundation::UInt64 acpiStart =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(g_acpiBytes));
    const auto& topology = Boot::Bootstrap::g_bootInfo.Acpi;
    return (topology.RsdpPhysicalAddress == acpiStart) && (topology.RootTablePhysicalAddress == (acpiStart + 64U)) &&
           (topology.MadtPhysicalAddress == (acpiStart + 256U)) &&
           (topology.LocalControllerPhysicalAddress == 0xFEE0'0000U) &&
           (topology.RootKind == Boot::Acpi::AcpiRootTableKind::Xsdt) && (topology.ProcessorCount == 1U) &&
           (topology.Processors[0U].AcpiUid == 0U) && (topology.Processors[0U].HardwareId == 0U) &&
           (topology.Processors[0U].Flags == 1U) &&
           (topology.Processors[0U].ControllerKind == Boot::Acpi::AcpiProcessorControllerKind::LocalApic) &&
           (topology.IoControllerCount == 1U) && (topology.IoControllers[0U].ControllerId == 0U) &&
           (topology.IoControllers[0U].PhysicalAddress == 0xFEC0'0000U) &&
           (topology.IoControllers[0U].GlobalSystemInterruptBase == 0U) &&
           (topology.IoControllers[0U].ControllerKind == Boot::Acpi::AcpiIoControllerKind::IoApic) &&
           (topology.InterruptOverrideCount == 0U);
}

[[nodiscard]] bool PageTableMatchesFirstExitSnapshot() noexcept
{
    const Foundation::UInt64* const entries = GetPageTableEntries();
    for (Foundation::Size index{0U}; index < PageTableEntryCount; ++index)
    {
        if (entries[index] != g_pageTableSnapshot[index])
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] Foundation::UInt16 ReadDescriptorRegisterLimit(const Foundation::UInt64 address) noexcept
{
    const auto* const bytes = reinterpret_cast<const Foundation::UInt8*>(static_cast<Foundation::UIntPointer>(address));
    return static_cast<Foundation::UInt16>(bytes[0U]) |
           static_cast<Foundation::UInt16>(static_cast<Foundation::UInt16>(bytes[1U]) << 8U);
}

[[nodiscard]] Foundation::UInt64 ReadDescriptorRegisterBase(const Foundation::UInt64 address) noexcept
{
    const auto* const bytes = reinterpret_cast<const Foundation::UInt8*>(static_cast<Foundation::UIntPointer>(address));
    Foundation::UInt64 value{0U};
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        value |= static_cast<Foundation::UInt64>(bytes[index + 2U]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] bool ObserveActivation(const Foundation::UInt64 rootPhysicalAddress,
                                     const Foundation::UInt64 initialStackPointer,
                                     const Foundation::UInt64 globalDescriptorRegisterAddress,
                                     const Foundation::UInt64 interruptDescriptorRegisterAddress,
                                     const Foundation::UInt64 currentRootPhysicalAddress,
                                     const Foundation::UInt64 packedSelectors) noexcept
{
    ++g_state.ActivationCalls;
    const auto& summary = Boot::Bootstrap::g_amd64BootstrapMappingPlanSummary;
    const Foundation::UInt16 codeSelector = static_cast<Foundation::UInt16>(packedSelectors >> 16U);
    const Foundation::UInt16 stackSelector = static_cast<Foundation::UInt16>(packedSelectors);
    g_state.ActivationArgumentsExact =
        g_state.BootServicesExited && g_state.PreparationObservedBeforeExit &&
        g_systemTable.BootServicesTable == nullptr &&
        Boot::Uefi::IsTableCrcValid(&g_systemTable, g_systemTable.Header) &&
        (rootPhysicalAddress == summary.PageTableStoragePhysicalStart) &&
        (initialStackPointer == summary.StackPhysicalStart + (4U * 4096U)) &&
        (currentRootPhysicalAddress == 0x0010'0000U) &&
        (codeSelector == Boot::Bootstrap::g_amd64InitialProcessorStateReport.CodeSelector) &&
        (stackSelector == Boot::Bootstrap::g_amd64InitialProcessorStateReport.StackSelector) &&
        (ReadDescriptorRegisterLimit(globalDescriptorRegisterAddress) == 63U) &&
        (ReadDescriptorRegisterBase(globalDescriptorRegisterAddress) == summary.DescriptorTableStoragePhysicalStart) &&
        (ReadDescriptorRegisterLimit(interruptDescriptorRegisterAddress) == 4095U) &&
        (ReadDescriptorRegisterBase(interruptDescriptorRegisterAddress) ==
         summary.DescriptorTableStoragePhysicalStart + 4096U) &&
        GuardAndStackRemainExact() && DescriptorTablesAreExact(codeSelector, stackSelector) &&
        PageTableMatchesFirstExitSnapshot();
    return g_state.ActivationArgumentsExact;
}

[[nodiscard]] bool IsBootFileFailureStateValid(const Boot::Uefi::Status result) noexcept
{
    const char* reason{nullptr};
    Foundation::UInt32 allocations{0U};
    switch (g_scenario)
    {
    case IntegrationScenario::KernelFileCrcMismatch:
        reason = "KERNEL-CRC32C-MISMATCH";
        allocations = 2U;
        break;
    case IntegrationScenario::BootFileCrcMismatch:
        reason = "FILE-CRC32C-MISMATCH";
        allocations = 1U;
        break;
    case IntegrationScenario::BootFileOversized:
        reason = "FILE-SIZE";
        break;
    case IntegrationScenario::BootFileMissing:
        reason = "FILE-OPEN";
        break;
    case IntegrationScenario::BootManifestInvalid:
        reason = "MANIFEST-VERSION";
        break;
    case IntegrationScenario::BootFileTruncated:
    case IntegrationScenario::BootFileGrown:
        reason = "FILE-READ";
        allocations = 1U;
        break;
    case IntegrationScenario::Success:
    case IntegrationScenario::AllocationFailure:
    case IntegrationScenario::ZeroAddressAllocation:
    case IntegrationScenario::RetryTypeChanged:
    case IntegrationScenario::RetryAttributesChanged:
    case IntegrationScenario::RetryStartChanged:
    case IntegrationScenario::RetryCountChanged:
    case IntegrationScenario::ApicStateRejected:
    case IntegrationScenario::TscCalibrationRejected:
        return false;
    }
    return !g_state.TextOrderingFailed && g_state.WatchdogCalls == 1U && result == Boot::Uefi::DeviceError &&
           g_state.ExitBootServicesCalls == 0U && g_state.AllocatePagesCalls == allocations &&
           g_state.FreePagesCalls == 0U && g_state.ReportOpenCalls == 0U && g_state.FailureStallCalls == 1U &&
           std::strstr(g_consoleTranscript, reason) != nullptr && std::strstr(g_preExitTranscript, reason) != nullptr &&
           std::strstr(g_consoleTranscript, "Returning to the firmware in 30 seconds") != nullptr;
}

[[nodiscard]] bool IsAllocationFailureStateValid() noexcept
{
    const Foundation::UInt32 expectedFreePagesCalls =
        g_scenario == IntegrationScenario::ZeroAddressAllocation ? 2U : 1U;
    if ((g_state.GetMemoryMapCalls != 2U) || (g_state.ExitBootServicesCalls != 0U) ||
        (g_state.AllocatePagesCalls != 3U) || (g_state.FreePagesCalls != expectedFreePagesCalls) ||
        g_state.BootServicesExited || g_state.PreparationObservedBeforeExit || (g_state.BootInfoStageCount != 0U) ||
        (g_state.PageAllocatorStageCount != 0U) || (g_state.InitialMappingPlanStageCount != 0U) ||
        (g_state.InitialPageTableStageCount != 0U) || (g_state.InitialAddressSpaceStageCount != 0U) ||
        (g_state.ActivationCalls != 0U) || g_state.ActivationArgumentsExact ||
        (g_state.ExitBootServicesStageCount != 0U) || (g_state.FinalMemoryMapFailureCount != 0U) ||
        (g_state.ExitBootServicesFailureCount != 0U) || (g_state.ExitBootServicesPreparationDetailCount != 0U) ||
        (g_state.UnexpectedPreExitFailureRecordCount != 0U) || (g_state.UnexpectedPostExitRecordCount != 0U) ||
        (Boot::Bootstrap::g_amd64BootstrapReservation.PhysicalStart != 0U) ||
        (Boot::Bootstrap::g_amd64InitialMappingPlan.GetMappingCount() != 0U) ||
        Boot::Bootstrap::g_amd64InitialMappingPlan.IsSealed() ||
        Boot::Bootstrap::g_amd64InitialAddressSpacePreparation.IsPrepared() ||
        Boot::Bootstrap::g_amd64InitialAddressSpacePreparation.HasFailed())
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < ReservedBytes; ++index)
    {
        if (g_reservedBytes[index] != ReservedByteSentinel)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool IsRuntimeRejectionStateValid() noexcept
{
    const char* const expected = g_scenario == IntegrationScenario::ApicStateRejected
                                     ? "ASTERIA:AMD64-RUNTIME-PREPARE:FAIL:APIC-STATE"
                                     : "ASTERIA:AMD64-RUNTIME-PREPARE:FAIL:TSC-CALIBRATION";
    return !g_state.BootServicesExited && g_state.ExitBootServicesCalls == 0U && g_state.ActivationCalls == 0U &&
           g_state.RuntimePrepareCalls == 1U && std::strstr(g_consoleTranscript, expected) != nullptr &&
           (g_scenario != IntegrationScenario::TscCalibrationRejected ||
            std::strstr(g_consoleTranscript, "ASTERIA:AMD64-TSC:FAIL:PRESENT=ACPI-PM,HPET,PIT,"
                                             ":TRIED=ACPI-PM,HPET,PIT,") != nullptr) &&
           std::strstr(g_consoleTranscript, "ASTERIA:EXIT-BOOT-SERVICES:FAIL") != nullptr;
}

[[nodiscard]] bool IsRetryRejectionStateValid() noexcept
{
    return !g_state.BootServicesExited && (g_state.GetMemoryMapCalls == 4U) && (g_state.ExitBootServicesCalls == 1U) &&
           (g_state.AllocatePagesCalls == 5U) && (g_state.FreePagesCalls == 1U) &&
           g_state.PreparationObservedBeforeExit && (g_state.BootInfoStageCount == 0U) &&
           (g_state.PageAllocatorStageCount == 0U) && (g_state.InitialMappingPlanStageCount == 0U) &&
           (g_state.InitialPageTableStageCount == 0U) && (g_state.InitialAddressSpaceStageCount == 0U) &&
           (g_state.ActivationCalls == 0U) && !g_state.ActivationArgumentsExact &&
           (g_state.ExitBootServicesStageCount == 0U) && (g_state.FinalMemoryMapFailureCount == 1U) &&
           (g_state.ExitBootServicesFailureCount == 1U) && (g_state.ExitBootServicesPreparationDetailCount == 1U) &&
           (g_state.UnexpectedPreExitFailureRecordCount == 0U) && (g_state.UnexpectedPostExitRecordCount == 0U) &&
           IsReservedStoragePrepared() && IsPublishedAmd64BootstrapStateValid() && PageTableMatchesFirstExitSnapshot();
}

[[nodiscard]] bool KillAndReapChild(const pid_t child) noexcept
{
    if ((kill(child, SIGKILL) != 0) && (errno != ESRCH))
    {
        return false;
    }

    while (true)
    {
        const pid_t result = waitpid(child, nullptr, 0);
        if (result == child)
        {
            return true;
        }
        if ((result == -1) && (errno == EINTR))
        {
            continue;
        }
        return (result == -1) && (errno == ECHILD);
    }
}

[[nodiscard]] ChildWaitResult WaitForChild(const pid_t child, int& status) noexcept
{
    timespec deadline{};
    if ((clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) ||
        (deadline.tv_sec > (std::numeric_limits<time_t>::max() - ChildWaitTimeoutSeconds)))
    {
        static_cast<void>(KillAndReapChild(child));
        return ChildWaitResult::Failed;
    }
    deadline.tv_sec += ChildWaitTimeoutSeconds;

    while (true)
    {
        const pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child)
        {
            return ChildWaitResult::Completed;
        }
        if (result == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return ChildWaitResult::Failed;
        }

        timespec now{};
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        {
            static_cast<void>(KillAndReapChild(child));
            return ChildWaitResult::Failed;
        }
        if ((now.tv_sec > deadline.tv_sec) || ((now.tv_sec == deadline.tv_sec) && (now.tv_nsec >= deadline.tv_nsec)))
        {
            break;
        }

        const timespec delay{
            .tv_sec = 0,
            .tv_nsec = ChildWaitPollNanoseconds,
        };
        if ((nanosleep(&delay, nullptr) != 0) && (errno != EINTR))
        {
            static_cast<void>(KillAndReapChild(child));
            return ChildWaitResult::Failed;
        }
    }

    return KillAndReapChild(child) ? ChildWaitResult::TimedOut : ChildWaitResult::Failed;
}
} // namespace
} // namespace Asteria::Test

extern "C" Asteria::Foundation::UInt64
AsteriaAmd64ActivateInitialAddressSpace(const Asteria::Foundation::UInt64 rootPhysicalAddress,
                                        const Asteria::Foundation::UInt64 initialStackPointer,
                                        const Asteria::Foundation::UInt64 globalDescriptorRegisterAddress,
                                        const Asteria::Foundation::UInt64 interruptDescriptorRegisterAddress,
                                        const Asteria::Foundation::UInt64 currentRootPhysicalAddress,
                                        const Asteria::Foundation::UInt64 packedSelectors) noexcept
{
    if (!Asteria::Test::ObserveActivation(rootPhysicalAddress, initialStackPointer, globalDescriptorRegisterAddress,
                                          interruptDescriptorRegisterAddress, currentRootPhysicalAddress,
                                          packedSelectors))
    {
        Asteria::Test::ExitChild(Asteria::Test::InvalidActivationStateStatus);
    }
    Asteria::Platform::Firmware::WriteDiagnosticAscii(Asteria::Test::InitialAddressSpaceStageRecord);
    Asteria::Platform::Firmware::HaltAfterFirmwareExit();
}

extern "C" [[noreturn]] void AsteriaAmd64FailStopInterrupt() noexcept
{
    Asteria::Test::ExitChild(Asteria::Test::FailStopInterruptStatus);
}

namespace Asteria::Boot::Uefi
{
void SelectLoaderDiagnosticSinks(const void*) noexcept
{
}
BootDiagnosticSinks GetLoaderDiagnosticSinks() noexcept
{
    return {};
}
BootDebugPort GetLoaderDebugPort() noexcept
{
    return {};
}
void WriteLoaderDiagnosticAscii(const char* const text) noexcept
{
    Asteria::Test::ObserveDiagnosticRecords(text);
}
void WriteLoaderMirroredAscii(const char* const text) noexcept
{
    Asteria::Test::ObserveDiagnosticRecords(text);
}
} // namespace Asteria::Boot::Uefi
namespace Asteria::Platform::Amd64::Firmware
{
void InstallDiagnosticSinks(const Boot::BootDiagnosticSinks&) noexcept
{
}
// The fixture has no PCH: a port read finds nothing.
Foundation::UInt32 ReadFirmwarePort32(Foundation::UInt16) noexcept
{
    return 0xFFFF'FFFFU;
}
} // Namespace Asteria::Platform::Amd64::Firmware

namespace Asteria::Platform::Firmware
{
void WriteDiagnosticAscii(const char* const text) noexcept
{
    Asteria::Test::ObserveDiagnosticRecords(text);
}

// A cycle counter whose deltas pass the jitter health tests; no processor
// random-number instruction.
Foundation::UInt64 ReadCycleCounter() noexcept
{
    auto& state = Asteria::Test::g_state;
    ++state.CycleCounterReadings;
    state.CycleCounter += 100U + ((state.CycleCounterReadings * 7919U) % 997U);
    return state.CycleCounter;
}

bool TryReadProcessorRandom(Foundation::UInt64& value) noexcept
{
    value = 0U;
    return false;
}

[[noreturn]] void HaltAfterFirmwareExit() noexcept
{
    const bool valid =
        Asteria::Test::g_scenario == Asteria::Test::IntegrationScenario::Success
            ? Asteria::Test::g_state.BootServicesExited && (Asteria::Test::g_state.GetMemoryMapCalls == 4U) &&
                  (Asteria::Test::g_state.ExitBootServicesCalls == 2U) &&
                  (Asteria::Test::g_state.AllocatePagesCalls == 5U) &&
                  (Asteria::Test::g_state.RuntimePrepareCalls == 2U) && (Asteria::Test::g_state.FreePagesCalls == 1U) &&
                  Asteria::Test::g_state.PreparationObservedBeforeExit &&
                  (Asteria::Test::g_state.BootInfoStageCount == 1U) &&
                  (Asteria::Test::g_state.PageAllocatorStageCount == 1U) &&
                  (Asteria::Test::g_state.InitialMappingPlanStageCount == 1U) &&
                  (Asteria::Test::g_state.InitialPageTableStageCount == 1U) &&
                  (Asteria::Test::g_state.InitialAddressSpaceStageCount == 1U) &&
                  (Asteria::Test::g_state.ExitBootServicesStageCount == 1U) &&
                  (Asteria::Test::g_state.ActivationCalls == 1U) && Asteria::Test::g_state.ActivationArgumentsExact &&
                  Asteria::Test::EvidenceRecordIsReported() && Asteria::Test::IsPublishedEntropyValid() &&
                  Asteria::Test::IsPublishedAcpiTopologyValid() &&
                  (Asteria::Test::g_state.FinalMemoryMapFailureCount == 0U) &&
                  (Asteria::Test::g_state.ExitBootServicesFailureCount == 0U) &&
                  (Asteria::Test::g_state.ExitBootServicesPreparationDetailCount == 0U) &&
                  (Asteria::Test::g_state.UnexpectedPreExitFailureRecordCount == 0U) &&
                  (Asteria::Test::g_state.UnexpectedPostExitRecordCount == 0U) &&
                  Asteria::Test::GuardAndStackRemainExact() &&
                  Asteria::Test::DescriptorTablesAreExact(
                      Asteria::Boot::Bootstrap::g_amd64InitialProcessorStateReport.CodeSelector,
                      Asteria::Boot::Bootstrap::g_amd64InitialProcessorStateReport.StackSelector) &&
                  Asteria::Test::PageTableMatchesFirstExitSnapshot()
            : Asteria::Test::IsRetryRejectionStateValid();
    if (!valid)
    {
        static_cast<void>(std::fprintf(
            stderr,
            "FAIL: halted with get-map=%u exit=%u allocate=%u free=%u "
            "exited=%u prepared-before-exit=%u markers=%u/%u/%u/%u/%u/%u "
            "activation=%u/%u "
            "pre-failure=%u/%u detail=%u pre-unexpected=%u "
            "post-unexpected=%u "
            "storage=%u publication=%u snapshot=%u evidence=%u entropy=%u\n",
            Asteria::Test::g_state.GetMemoryMapCalls, Asteria::Test::g_state.ExitBootServicesCalls,
            Asteria::Test::g_state.AllocatePagesCalls, Asteria::Test::g_state.FreePagesCalls,
            Asteria::Test::g_state.BootServicesExited ? 1U : 0U,
            Asteria::Test::g_state.PreparationObservedBeforeExit ? 1U : 0U, Asteria::Test::g_state.BootInfoStageCount,
            Asteria::Test::g_state.PageAllocatorStageCount, Asteria::Test::g_state.InitialMappingPlanStageCount,
            Asteria::Test::g_state.InitialPageTableStageCount, Asteria::Test::g_state.InitialAddressSpaceStageCount,
            Asteria::Test::g_state.ExitBootServicesStageCount, Asteria::Test::g_state.ActivationCalls,
            Asteria::Test::g_state.ActivationArgumentsExact ? 1U : 0U,
            Asteria::Test::g_state.FinalMemoryMapFailureCount, Asteria::Test::g_state.ExitBootServicesFailureCount,
            Asteria::Test::g_state.ExitBootServicesPreparationDetailCount,
            Asteria::Test::g_state.UnexpectedPreExitFailureRecordCount,
            Asteria::Test::g_state.UnexpectedPostExitRecordCount, Asteria::Test::GuardAndStackRemainExact() ? 1U : 0U,
            Asteria::Test::g_state.ActivationArgumentsExact ? 1U : 0U,
            Asteria::Test::PageTableMatchesFirstExitSnapshot() ? 1U : 0U,
            Asteria::Test::EvidenceRecordIsReported() ? 1U : 0U, Asteria::Test::IsPublishedEntropyValid() ? 1U : 0U));
    }
    Asteria::Test::ExitChild(valid ? 0 : Asteria::Test::InvalidPostExitStateStatus);
}
} // Namespace Asteria::Platform::Firmware

namespace Asteria::Test
{
// KernelQuery::MetadataArenaPages, through the kernel's own query entry: the layout's arithmetic
// for every index, never above the cap, and asking it changes no answer of any selector.
void TestMetadataArenaQuery(TestContext& runner) noexcept
{
    using Boot::KernelQuery;
    using Foundation::UInt32;
    using Foundation::UInt64;
    using Platform::Amd64::Bootstrap::GetMetadataArenaPages;
    using Platform::Amd64::Bootstrap::MetadataArenaMaximumPages;
    constexpr UInt32 Selector{static_cast<UInt32>(KernelQuery::MetadataArenaPages)};
    constexpr UInt32 Indices{4U};
    UInt64 before[Selector + 2U][Indices]{};
    for (UInt32 selector{0U}; selector < Selector + 2U; ++selector)
    {
        for (UInt32 index{0U}; index < Indices; ++index)
        {
            before[selector][index] = AsteriaKernelQuery(selector, index);
        }
    }
    bool exact{true};
    bool bounded{true};
    UInt64 previous{0U};
    constexpr UInt32 Probes[]{0U, 1U, 2U, 8U, 64U, 100U, 1000U, 0x7FFF'FFFFU, 0xFFFF'FFFFU};
    for (UInt32 index{0U}; index < 4096U; ++index)
    {
        const UInt64 pages = AsteriaKernelQuery(Selector, index);
        exact = exact && (pages == GetMetadataArenaPages(index));
        bounded = bounded && (pages != 0U) && (pages <= MetadataArenaMaximumPages) && (pages >= previous);
        previous = pages;
    }
    for (const UInt32 index : Probes)
    {
        const UInt64 pages = AsteriaKernelQuery(Selector, index);
        exact = exact && (pages == GetMetadataArenaPages(index));
        bounded = bounded && (pages <= MetadataArenaMaximumPages);
    }
    bool unchanged{true};
    for (UInt32 selector{0U}; selector < Selector + 2U; ++selector)
    {
        for (UInt32 index{0U}; index < Indices; ++index)
        {
            unchanged = unchanged && (before[selector][index] == AsteriaKernelQuery(selector, index));
        }
    }
    ASTERIA_EXPECT(runner, "the metadata-arena query answers the layout's bounded arithmetic and changes nothing",
                   exact, bounded, unchanged, AsteriaKernelQuery(Selector, 0xFFFF'FFFFU) == MetadataArenaMaximumPages,
                   AsteriaKernelQuery(Selector + 1U, 0U) == 0U);
}
} // namespace Asteria::Test

int main()
{
    TestContext runner{"BootApplicationIntegrationTests"};
    Asteria::Test::TestMetadataArenaQuery(runner);
    for (const Asteria::Test::IntegrationScenario scenario : Asteria::Test::IntegrationScenarios)
    {
        Asteria::Test::g_scenario = scenario;
        const pid_t parent = getpid();
        const pid_t child = fork();
        if (child == -1)
        {
            ASTERIA_FAIL(runner, "fork failed");
            break;
        }
        if (child == 0)
        {
            if ((prctl(PR_SET_PDEATHSIG, SIGKILL) != 0) || (getppid() != parent))
            {
                Asteria::Test::ExitChild(Asteria::Test::ChildSetupFailedStatus);
            }
            if (!Asteria::Test::SetUpFirmwareTables())
            {
                Asteria::Test::ExitChild(Asteria::Test::ChildSetupFailedStatus);
            }
            const Asteria::Boot::Uefi::Status result =
                EfiMain(&Asteria::Test::g_loadedImage, &Asteria::Test::g_systemTable);
            if (Asteria::Test::IsBootFileFailureStateValid(result))
            {
                Asteria::Test::ExitChild(0);
            }
            if (((scenario == Asteria::Test::IntegrationScenario::AllocationFailure) ||
                 (scenario == Asteria::Test::IntegrationScenario::ZeroAddressAllocation)) &&
                (result == Asteria::Boot::Uefi::DeviceError) && Asteria::Test::IsAllocationFailureStateValid())
            {
                Asteria::Test::ExitChild(0);
            }
            if ((scenario == Asteria::Test::IntegrationScenario::ApicStateRejected ||
                 scenario == Asteria::Test::IntegrationScenario::TscCalibrationRejected) &&
                result == Asteria::Boot::Uefi::DeviceError && Asteria::Test::IsRuntimeRejectionStateValid())
            {
                Asteria::Test::ExitChild(0);
            }
            Asteria::Test::ExitChild(Asteria::Test::EfiMainReturnedStatus);
        }

        int status{0};
        const Asteria::Test::ChildWaitResult waitResult = Asteria::Test::WaitForChild(child, status);
        const bool completed = waitResult == Asteria::Test::ChildWaitResult::Completed;
        const bool exited = completed && WIFEXITED(status);
        const bool succeeded = exited && (WEXITSTATUS(status) == 0);
        if (!ASTERIA_EXPECT(runner, "production EfiMain scenario child completes and exits with success", completed,
                            exited, succeeded))
        {
            static_cast<void>(std::fprintf(
                stderr, "  scenario=%u wait=%s status=%d\n", static_cast<unsigned int>(scenario),
                waitResult == Asteria::Test::ChildWaitResult::TimedOut ? "timed-out-and-reaped"
                                                                       : (completed ? "completed" : "failed"),
                status));
        }
    }
    return runner.Finish();
}

namespace Asteria::Platform::Amd64::Time
{
Amd64TscAttempts GetTscCalibrationAttempts() noexcept
{
    if (Test::g_scenario == Test::IntegrationScenario::TscCalibrationRejected)
    {
        return {(1U << 3U) | (1U << 4U) | (1U << 5U), (1U << 3U) | (1U << 4U) | (1U << 5U)};
    }
    return {};
}
} // namespace Asteria::Platform::Amd64::Time
namespace Asteria::Platform::Amd64::Bootstrap
{
Foundation::UInt32 GetAmd64FirmwareApicId() noexcept
{
    return 0U;
}
Foundation::UInt64 GetAmd64BootTscTicksPerMicrosecond() noexcept
{
    return 1000U;
}

const char* GetAmd64BootRuntimeRejection() noexcept
{
    if (Test::g_scenario == Test::IntegrationScenario::ApicStateRejected)
    {
        return "APIC-STATE";
    }
    if (Test::g_scenario == Test::IntegrationScenario::TscCalibrationRejected)
    {
        return "TSC-CALIBRATION";
    }
    return nullptr;
}

bool PrepareAmd64BootRuntime(const Amd64BootRuntimeInput& input) noexcept
{
    // Every preparation sees the published seed, the same one on each attempt.
    ++Test::g_state.RuntimePrepareCalls;
    if (GetAmd64BootRuntimeRejection() != nullptr)
    {
        return false;
    }
    const auto& entropy = input.BootInformation->Entropy;
    if (Test::g_state.RuntimePrepareCalls == 1U)
    {
        Test::g_state.FirstPreparedEntropy = entropy;
        Test::g_state.EntropyStableAcrossPreparations =
            ::Asteria::Boot::GetBootEntropyState(entropy) == ::Asteria::Boot::BootEntropyState::Published;
    }
    else
    {
        Test::g_state.EntropyStableAcrossPreparations =
            Test::g_state.EntropyStableAcrossPreparations &&
            (std::memcmp(&Test::g_state.FirstPreparedEntropy, &entropy, sizeof(entropy)) == 0);
    }
    return !Test::g_state.TextOrderingFailed && Test::g_state.WatchdogCalls == 1U &&
           !Test::g_state.BootServicesExited && input.BootInformation == &::Asteria::Boot::Bootstrap::g_bootInfo &&
           input.InitialMappings == &::Asteria::Boot::Bootstrap::g_amd64InitialMappingPlan &&
           input.Capabilities == &::Asteria::Boot::Bootstrap::g_amd64InitialPagingCapabilities &&
           input.InitialMappings->IsSealed() && input.PoolPhysicalStart == 0x1000000U &&
           input.StartupPhysicalStart == 0x90000U && input.UserImage.Data != nullptr && input.UserImage.SizeBytes == 4U;
}
} // namespace Asteria::Platform::Amd64::Bootstrap

// Ordinary loader-entry link unit for the firmware transaction scenarios.
// The pure ELF parser and placer have independent byte-level coverage; these
// scenarios exercise the retained preparation/console/EBS ordering.
namespace Asteria::Boot::Uefi
{
namespace
{
BootImagePlacement g_testPlacedImage{};
BootReservation g_testReservations[3U]{};
} // namespace

KernelElfError LoadKernelImage(BootServices&, Executable::Elf::Elf64ByteView) noexcept
{
    Platform::Amd64::Firmware::Amd64LoadedImageLayout layout{};
    const auto error = Platform::Amd64::Firmware::Amd64LoadedImageLayoutParser::Parse(Test::g_loadedImageBytes,
                                                                                      Test::ImageBytes, layout);
    if (error != Platform::Amd64::Firmware::Amd64LoadedImageLayoutError::None)
    {
        return KernelElfError::InvalidSegments;
    }
    g_testPlacedImage = {layout.ImageBase, layout.ImageSize, 4096U, 0U, 7U, 0U, {}};
    for (Foundation::Size index{0U}; index < 7U; ++index)
    {
        const auto& section = layout.Sections[index];
        g_testPlacedImage.Sections[index] = {section.VirtualStart, section.PageCount,
                                             static_cast<Foundation::UInt32>(section.Purpose), 0U};
    }
    return KernelElfError::None;
}

bool ReserveLoadedKernelMemory(BootServices& services, const Foundation::UInt64 spanGiB) noexcept
{
    // The fake kernel asks for no metadata arena: the scripted firmware
    // allocates only the note's reservations.
    static_cast<void>(spanGiB);
    Foundation::UInt64 start{0U};
    if (services.AllocatePagesFunction(AllocateType::AnyPages, MemoryType::LoaderData, Test::ReservedPageCount,
                                       &start) != Success)
    {
        return false;
    }
    g_testReservations[0U] = {start, Test::ReservedPageCount, 0U, 2U, 0U};
    if (start == 0U || (start & 0xFFFU) != 0U || start > ~Foundation::UInt64{0U} - Test::ReservedPageCount * 4096U)
    {
        static_cast<void>(services.FreePagesFunction(start, Test::ReservedPageCount));
        g_testReservations[0U] = {};
        return false;
    }
    for (Foundation::Size index{1U}; index < 3U; ++index)
    {
        const Foundation::UInt64 pages = index == 1U ? Platform::Amd64::Bootstrap::RuntimePoolPages : 2U;
        start = index == 1U ? 0xFFFFFFFFU : 0xFFFFFU;
        if (services.AllocatePagesFunction(AllocateType::MaximumAddress, MemoryType::LoaderData, pages, &start) !=
            Success)
        {
            static_cast<void>(ReleaseKernelBootReservations(services));
            return false;
        }
        g_testReservations[index] = {start, pages, 0U, 2U, 0U};
    }
    return true;
}

const BootImagePlacement& GetPlacedKernelImage() noexcept
{
    return g_testPlacedImage;
}

BootReservation GetKernelBootReservation(const Foundation::UInt32 kind) noexcept
{
    return kind < 3U ? g_testReservations[kind] : BootReservation{};
}

BootReservation GetKernelMetadataArena() noexcept
{
    return BootReservation{};
}

bool ReleaseKernelBootReservations(BootServices& services) noexcept
{
    for (Foundation::Size index{3U}; index > 0U; --index)
    {
        auto& reservation = g_testReservations[index - 1U];
        if (reservation.PageCount != 0U)
        {
            if (services.FreePagesFunction(reservation.PhysicalStart, reservation.PageCount) != Success)
            {
                return false;
            }
            reservation = {};
        }
    }
    return true;
}

Foundation::UInt64 ReadKernelQuery(const KernelQuery query, const Foundation::UInt32 index) noexcept
{
    return AsteriaKernelQuery(static_cast<Foundation::UInt32>(query), index);
}

const char* ReadKernelDiagnosticString(const KernelQuery query) noexcept
{
    return reinterpret_cast<const char*>(static_cast<Foundation::UIntPointer>(ReadKernelQuery(query, 0U)));
}

bool PrepareLoadedKernel(BootInfo& info) noexcept
{
    return AsteriaKernelPrepare(&info) == 1U;
}

[[noreturn]] void EnterLoadedKernel(const BootInfo& info) noexcept
{
    AsteriaKernelEnter(&info);
}
} // Namespace Asteria::Boot::Uefi
