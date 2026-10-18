// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_BOOT_FILES_HPP_INCLUDED
#define ASTERIA_BOOT_BOOT_FILES_HPP_INCLUDED

#include "Abi/ComponentImageTable.hpp"
#include "Foundation/Types.hpp"

namespace Asteria::Boot
{
using Abi::ComponentImageMaximumCount;
constexpr Foundation::Size BootFileCount{3U};
/// Module table capacity; each module is bounded by BootModuleMaximumBytes.
constexpr Foundation::Size BootModuleMaximumCount{ComponentImageMaximumCount};
constexpr Foundation::UInt64 BootModuleMaximumBytes{2U * 1024U * 1024U};
constexpr Foundation::Size BootManifestMaximumBytes{4096U};
constexpr Foundation::UInt64 BootFileMaximumBytes{16U * 1024U * 1024U};
constexpr Foundation::Size BootInitFile{0U};
constexpr Foundation::Size BootUserFile{1U};
constexpr Foundation::Size BootSpinnerFile{2U};

/// Offsets are relative to the retained LoaderData region. SizeBytes zero
/// denotes the absent optional spinner. Names are uppercase 8.3 basenames.
struct BootFileRecord
{
    Foundation::UInt64 Offset;
    Foundation::UInt64 SizeBytes;
    Foundation::UInt32 Crc32c;
    char Name[13];
    Foundation::UInt8 Reserved[7];
};

struct BootModuleRecord
{
    BootFileRecord File;
    char Name[16];
};

/// Identity-addressed before translation takeover; never freed. The manifest
/// occupies the first ManifestBytes bytes, followed by aligned whole files.
struct BootFileRegion
{
    Foundation::UInt64 PhysicalStart;
    Foundation::UInt64 PageCount;
    Foundation::UInt64 ManifestBytes;
    BootFileRecord Files[BootFileCount];
    Foundation::UInt64 ModuleOffset;
    Foundation::UInt64 ModuleBytes;
    Foundation::UInt64 ModuleCount;
    BootModuleRecord Modules[BootModuleMaximumCount];
};

static_assert(__builtin_offsetof(BootFileRecord, Offset) == 0U);
static_assert(__builtin_offsetof(BootFileRecord, SizeBytes) == 8U);
static_assert(__builtin_offsetof(BootFileRecord, Crc32c) == 16U);
static_assert(__builtin_offsetof(BootFileRecord, Name) == 20U);
static_assert(__builtin_offsetof(BootFileRecord, Reserved) == 33U);
static_assert(__builtin_offsetof(BootFileRegion, PhysicalStart) == 0U);
static_assert(__builtin_offsetof(BootFileRegion, PageCount) == 8U);
static_assert(__builtin_offsetof(BootFileRegion, ManifestBytes) == 16U);
static_assert(__builtin_offsetof(BootFileRegion, Files) == 24U);
static_assert(__builtin_offsetof(BootModuleRecord, File) == 0U);
static_assert(__builtin_offsetof(BootModuleRecord, Name) == 40U);
static_assert(sizeof(BootModuleRecord) == 56U);
static_assert(__builtin_offsetof(BootFileRegion, ModuleOffset) == 144U);
static_assert(__builtin_offsetof(BootFileRegion, ModuleBytes) == 152U);
static_assert(__builtin_offsetof(BootFileRegion, ModuleCount) == 160U);
static_assert(__builtin_offsetof(BootFileRegion, Modules) == 168U);
static_assert(sizeof(BootFileRecord) == 40U);
static_assert(sizeof(BootFileRegion) == 168U + (BootModuleMaximumCount * sizeof(BootModuleRecord)));
static_assert(sizeof(BootFileRegion) == 1064U);
} // Namespace Asteria::Boot

#endif // ASTERIA_BOOT_BOOT_FILES_HPP_INCLUDED
