// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_BOOT_KERNEL_INPUT_HPP_INCLUDED
#define ASTERIA_BOOT_BOOT_KERNEL_INPUT_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Boot
{
constexpr Foundation::UInt32 BootArchitectureAmd64{1U};
constexpr Foundation::UInt32 BootArchitectureIa64{2U};
constexpr Foundation::Size BootImageMaximumSections{7U};

/// Identity addresses and 4 KiB page counts. Purpose: 0 code, 1 read-only
/// data, 2 writable data. A section never combines write and execute.
struct BootImageSection
{
    Foundation::UInt64 PhysicalStart;
    Foundation::UInt64 PageCount;
    Foundation::UInt32 Purpose;
    Foundation::UInt32 Reserved;
};

struct BootImagePlacement
{
    Foundation::UInt64 PhysicalStart;
    Foundation::UInt64 SizeBytes;
    Foundation::UInt64 AlignmentBytes;
    Foundation::UInt64 Placement;
    Foundation::UInt32 SectionCount;
    Foundation::UInt32 Reserved;
    BootImageSection Sections[BootImageMaximumSections];
};

struct BootReservation
{
    Foundation::UInt64 PhysicalStart;
    Foundation::UInt64 PageCount;
    Foundation::UInt64 Attributes;
    Foundation::UInt32 FirmwareType;
    Foundation::UInt32 Reserved;
};

struct BootPciResource
{
    Foundation::UInt64 BaseAddress;
    Foundation::UInt64 LengthBytes;
    Foundation::UInt16 Segment;
    Foundation::UInt8 Bus;
    Foundation::UInt8 Device;
    Foundation::UInt8 Function;
    Foundation::UInt8 BarIndex;
    Foundation::UInt8 Reserved[2];
};

/// The validated SAL table facts, with Boolean facts encoded as 0 or 1.
struct BootSalInformation
{
    Foundation::UInt64 PalProcedurePhysicalAddress;
    Foundation::UInt64 SalProcedurePhysicalAddress;
    Foundation::UInt64 SalGlobalDataPhysicalAddress;
    Foundation::UInt64 TranslationRegisterVirtualAddress;
    Foundation::UInt64 TranslationRegisterEncodedPageSize;
    Foundation::UInt64 CoherenceDomainInformationPhysicalAddress;
    Foundation::UInt64 ApWakeVector;
    Foundation::UInt32 TotalTableLength;
    Foundation::UInt32 CoherenceDomainCount;
    Foundation::UInt16 SalRevision;
    Foundation::UInt16 EntryCount;
    Foundation::UInt16 SalAVersion;
    Foundation::UInt16 SalBVersion;
    Foundation::UInt16 MemoryDescriptorCount;
    Foundation::UInt8 PlatformFeatures;
    Foundation::UInt8 HasPlatformFeatures;
    Foundation::UInt8 HasTranslationRegister;
    Foundation::UInt8 HasPurgeCoherenceInformation;
    Foundation::UInt8 HasApWakeVector;
    Foundation::UInt8 Reserved8;
};

struct BootPagingCapabilities
{
    Foundation::UInt8 PhysicalAddressBits;
    Foundation::UInt8 FourLevelPagingEnabled;
    Foundation::UInt8 ExecuteDisableEnabled;
    Foundation::UInt8 SupervisorWriteProtectEnabled;
    Foundation::UInt8 GlobalPagesEnabled;
    Foundation::UInt8 PatLayoutConfigured;
    Foundation::UInt16 Reserved16;
    Foundation::UInt32 Reserved32;
    Foundation::UInt32 CaptureError;
};

/// Captured native diagnostic ports; zero names an absent sink.
struct BootDiagnosticSinks
{
    Foundation::UInt32 DebugPortPresent;
    Foundation::UInt16 SerialPort;
    Foundation::UInt16 Reserved;
};

/// All architecture inputs to preparation. Derived plans belong to the
/// kernel. Addresses are inert integers until the kernel validates their
/// extent and provenance against BootInfo's final map.
struct BootKernelInput
{
    BootImagePlacement Image;
    BootReservation Bootstrap;
    BootReservation RuntimePool;
    BootReservation Startup;
    BootReservation Ia64Runtime;
    BootPciResource Ahci;
    BootPciResource Device;
    BootPciResource Nic;
    BootSalInformation Sal;
    BootPagingCapabilities Paging;
    Foundation::UInt64 RsdpPhysicalAddress;
    Foundation::UInt64 TimerTicksPerMicrosecond;
    Foundation::UInt64 EvidencePhysicalAddress;
    BootDiagnosticSinks Diagnostics;
    Foundation::UInt32 Architecture;
    Foundation::UInt32 Reserved;
    /// AMD64's metadata arena: 2 MiB-aligned LoaderData below the fixed user ranges, of the pages
    /// KernelQuery::MetadataArenaPages answered. IA-64 leaves it zero.
    BootReservation MetadataArena;
};

static_assert(sizeof(BootImageSection) == 24U);
static_assert(sizeof(BootImagePlacement) == 208U);
static_assert(__builtin_offsetof(BootImagePlacement, SectionCount) == 32U);
static_assert(__builtin_offsetof(BootImagePlacement, Sections) == 40U);
static_assert(sizeof(BootReservation) == 32U);
static_assert(__builtin_offsetof(BootReservation, FirmwareType) == 24U);
static_assert(sizeof(BootPciResource) == 24U);
static_assert(__builtin_offsetof(BootPciResource, Segment) == 16U);
static_assert(sizeof(BootSalInformation) == 80U);
static_assert(__builtin_offsetof(BootSalInformation, TotalTableLength) == 56U);
static_assert(__builtin_offsetof(BootSalInformation, HasPlatformFeatures) == 75U);
static_assert(sizeof(BootPagingCapabilities) == 16U);
static_assert(__builtin_offsetof(BootPagingCapabilities, CaptureError) == 12U);
static_assert(sizeof(BootDiagnosticSinks) == 8U);
static_assert(sizeof(BootKernelInput) == 576U);
static_assert(alignof(BootKernelInput) == 8U);
static_assert(__builtin_offsetof(BootKernelInput, Bootstrap) == 208U);
static_assert(__builtin_offsetof(BootKernelInput, RuntimePool) == 240U);
static_assert(__builtin_offsetof(BootKernelInput, Startup) == 272U);
static_assert(__builtin_offsetof(BootKernelInput, Ia64Runtime) == 304U);
static_assert(__builtin_offsetof(BootKernelInput, Ahci) == 336U);
static_assert(__builtin_offsetof(BootKernelInput, Device) == 360U);
static_assert(__builtin_offsetof(BootKernelInput, Nic) == 384U);
static_assert(__builtin_offsetof(BootKernelInput, Sal) == 408U);
static_assert(__builtin_offsetof(BootKernelInput, Paging) == 488U);
static_assert(__builtin_offsetof(BootKernelInput, RsdpPhysicalAddress) == 504U);
static_assert(__builtin_offsetof(BootKernelInput, TimerTicksPerMicrosecond) == 512U);
static_assert(__builtin_offsetof(BootKernelInput, EvidencePhysicalAddress) == 520U);
static_assert(__builtin_offsetof(BootKernelInput, Diagnostics) == 528U);
static_assert(__builtin_offsetof(BootKernelInput, Architecture) == 536U);
static_assert(__builtin_offsetof(BootKernelInput, MetadataArena) == 544U);
} // Namespace Asteria::Boot

#endif // ASTERIA_BOOT_BOOT_KERNEL_INPUT_HPP_INCLUDED
