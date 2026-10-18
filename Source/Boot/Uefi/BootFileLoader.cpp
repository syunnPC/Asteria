// SPDX-License-Identifier: Apache-2.0

#include "Boot/Uefi/BootFileLoader.hpp"

#include "Boot/BootManifest.hpp"
#include "Abi/PagerBootstrap.hpp"
#include "Boot/Uefi/AcpiTableDump.hpp"
#include "Kernel/Security/Sha256.hpp"
#include "User/Filesystem/Crc32c.hpp"

namespace Asteria::Boot::Uefi
{
namespace
{
constexpr Guid LoadedImageProtocolGuid{
    0x5B1B31A1U, 0x9562U, 0x11D2U, {0x8EU, 0x3FU, 0x00U, 0xA0U, 0xC9U, 0x69U, 0x72U, 0x3BU}};
constexpr Guid FileSystemGuid{0x964E5B22U, 0x6459U, 0x11D2U, {0x8EU, 0x39U, 0x00U, 0xA0U, 0xC9U, 0x69U, 0x72U, 0x3BU}};
constexpr Guid FileInfoGuid{0x09576E92U, 0x6D3FU, 0x11D2U, {0x8EU, 0x39U, 0x00U, 0xA0U, 0xC9U, 0x69U, 0x72U, 0x3BU}};
constexpr Foundation::UInt64 PageBytes{4096U};
// Explicit boot-lifetime scratch, outside the 512-byte function stack budget.
constinit Foundation::UInt8 g_manifest[BootManifestMaximumBytes]{};
// EFI_FILE_INFO's fixed prefix is 80 bytes; allow up to 255 UTF-16 name words.
alignas(8) constinit Foundation::UInt8 g_info[80U + 512U]{};
constinit BootFileRegion g_candidate{};
constinit DisplayPolicy g_candidateDisplay{};
constinit BootTrustPolicy g_candidateTrust{};
constinit Foundation::UInt64 g_candidateLowMemory{0U};
constinit Foundation::UInt64 g_candidateWatchdog{Abi::WatchdogDefaultSeconds};
constinit Foundation::UInt64 g_candidateSession{0U};
constinit Foundation::UInt64 g_candidateLogin{0U};
constinit KernelFileReceipt g_kernelCandidate{};
// Each module's digest is computed at most once, whatever entries name it.
constinit Foundation::UInt8 g_moduleDigests[BootModuleMaximumCount][BootTrustDigestBytes]{};
constinit bool g_moduleHashed[BootModuleMaximumCount]{};

Foundation::UInt64 Read64(const Foundation::UInt8* const bytes) noexcept
{
    Foundation::UInt64 value{0U};
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        value |= static_cast<Foundation::UInt64>(bytes[index]) << (index * 8U);
    }
    return value;
}

const char* SizeOf(File& file, const Foundation::UInt64 maximum, Foundation::UInt64& size) noexcept
{
    if (file.GetInfo == nullptr || file.Read == nullptr || file.Close == nullptr)
    {
        return "FILE-PROTOCOL";
    }
    Guid guid{FileInfoGuid};
    Foundation::Size bytes{sizeof(g_info)};
    if (file.GetInfo(&file, &guid, &bytes, g_info) != Success || bytes < 82U || bytes > sizeof(g_info))
    {
        return "FILE-INFO";
    }
    const Foundation::UInt64 declared{Read64(g_info)};
    if (declared < 82U || declared > bytes || (Read64(g_info + 72U) & 0x10U) != 0U)
    {
        return "FILE-INFO";
    }
    size = Read64(g_info + 8U);
    if (size == 0U || size > maximum)
    {
        return "FILE-SIZE";
    }
    return nullptr;
}

bool Whole(File& file, Foundation::UInt8* bytes, const Foundation::Size size) noexcept
{
    Foundation::Size done{0U};
    while (done < size)
    {
        Foundation::Size count{size - done};
        if (count > AcpiDumpWriteChunkBytes)
        {
            count = AcpiDumpWriteChunkBytes;
        }
        const Foundation::Size requested{count};
        if (file.Read(&file, &count, bytes + done) != Success || count == 0U || count > requested)
        {
            return false;
        }
        done += count;
    }
    // Detect growth after GetInfo; a short or oversized read never admits bytes.
    Foundation::UInt8 extra{0U};
    Foundation::Size count{1U};
    return file.Read(&file, &count, &extra) == Success && count == 0U;
}

const char* ReadManifest(File& root) noexcept
{
    Foundation::UInt16 path[]{'\\', 'E',  'F', 'I', '\\', 'A', 'S', 'T', 'E', 'R', 'I',
                              'A',  '\\', 'B', 'O', 'O',  'T', '.', 'C', 'F', 'G', 0U};
    File* file{nullptr};
    if (root.Open(&root, &file, path, 1U, 0U) != Success || file == nullptr)
    {
        return "MANIFEST-OPEN";
    }
    Foundation::UInt64 size{0U};
    const char* reason = SizeOf(*file, BootManifestMaximumBytes, size);
    if (reason == nullptr && !Whole(*file, g_manifest, static_cast<Foundation::Size>(size)))
    {
        reason = "MANIFEST-READ";
    }
    if (file->Close != nullptr)
    {
        if (file->Close(file) != Success && reason == nullptr)
        {
            reason = "FILE-CLOSE";
        }
    }
    if (reason != nullptr)
    {
        return reason;
    }
    const BootManifestError error = ParseBootManifest(
        g_manifest, static_cast<Foundation::Size>(size), g_candidate, g_kernelCandidate.File, &g_candidateDisplay,
        &g_candidateTrust, &g_candidateLowMemory, &g_candidateWatchdog, &g_candidateSession, &g_candidateLogin);
    return error == BootManifestError::None ? nullptr : BootManifestReason(error);
}

BootFileRecord& Record(const Foundation::Size index) noexcept
{
    return index < BootFileCount ? g_candidate.Files[index] : g_candidate.Modules[index - BootFileCount].File;
}

void Store64(Foundation::UInt8* const bytes, const Foundation::UInt64 value) noexcept
{
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>((value >> (index * 8U)) & 255U);
    }
}

// Opaque receipts need no simultaneous file handles. Keep UTF-16 path scratch in this bounded
// frame, used by both passes.
[[gnu::noinline]] const char* OpenImage(File& root, const Foundation::Size index, File*& file) noexcept
{
    const BootFileRecord& record = Record(index);
    Foundation::UInt16 path[35]{'\\', 'E', 'F', 'I', '\\', 'A', 'S', 'T', 'E', 'R', 'I', 'A', '\\'};
    Foundation::Size prefix{13U};
    if (index >= BootFileCount)
    {
        constexpr char directory[]{"MODULES\\"};
        for (Foundation::Size character{0U}; character < 8U; ++character)
        {
            path[prefix++] = static_cast<Foundation::UInt16>(directory[character]);
        }
    }
    for (Foundation::Size character{0U}; character < 13U; ++character)
    {
        path[prefix + character] = static_cast<Foundation::UInt16>(record.Name[character]);
    }
    file = nullptr;
    if (root.Open(&root, &file, path, 1U, 0U) != Success || file == nullptr)
    {
        file = nullptr;
        return "FILE-OPEN";
    }
    return nullptr;
}

const char* CloseImage(File* const file, const char* const reason) noexcept
{
    if (file != nullptr && file->Close != nullptr && file->Close(file) != Success && reason == nullptr)
    {
        return "FILE-CLOSE";
    }
    return reason;
}

const char* SizeImages(File& root, Foundation::UInt64& total) noexcept
{
    total = BootManifestMaximumBytes;
    for (Foundation::Size index{0U}; index < BootFileCount + g_candidate.ModuleCount; ++index)
    {
        BootFileRecord& record = Record(index);
        if (record.Name[0] == '\0')
        {
            continue;
        }
        if (index == BootFileCount)
        {
            // Isolate table and modules from every other file at page boundaries.
            total = (total + PageBytes - 1U) & ~(PageBytes - 1U);
            g_candidate.ModuleOffset = total;
            total += 16U + g_candidate.ModuleCount * 32U;
            total = (total + 15U) & ~Foundation::UInt64{15U};
        }
        File* file{nullptr};
        const char* reason = OpenImage(root, index, file);
        if (reason == nullptr)
        {
            reason =
                SizeOf(*file, index < BootFileCount ? BootFileMaximumBytes : BootModuleMaximumBytes, record.SizeBytes);
        }
        reason = CloseImage(file, reason);
        if (reason != nullptr)
        {
            return reason;
        }
        record.Offset = total;
        // Three files of at most 16 MiB, so neither rounding nor total wraps.
        total += (record.SizeBytes + 15U) & ~Foundation::UInt64{15U};
    }
    if (g_candidate.ModuleCount != 0U)
    {
        g_candidate.ModuleBytes = (total - g_candidate.ModuleOffset + PageBytes - 1U) & ~(PageBytes - 1U);
        if (g_candidate.ModuleBytes > Abi::PagerModuleMaximumBytes)
        {
            return "MODULE-TABLE-SIZE";
        }
        total = g_candidate.ModuleOffset + g_candidate.ModuleBytes;
    }
    return nullptr;
}

// SHA-256 (FIPS 180-4, Kernel/Security/Sha256) of every module a trust entry names, over the bytes
// just read and CRC-checked into the retained region, which nothing writes afterwards: the pager
// maps them read-only and the kernel never maps them writable.
void VerifyTrust(const Foundation::UInt8* const storage) noexcept
{
    for (bool& hashed : g_moduleHashed)
    {
        hashed = false;
    }
    for (Foundation::Size index{0U}; index < g_candidateTrust.Count; ++index)
    {
        BootTrustEntry& entry = g_candidateTrust.Entries[index];
        const Foundation::Size module{entry.Module};
        if (module >= g_candidate.ModuleCount)
        {
            entry.Verdict = BootTrustVerdict::DigestMismatch;
            continue;
        }
        if (!g_moduleHashed[module])
        {
            const BootFileRecord& file = g_candidate.Modules[module].File;
            Kernel::Security::Sha256 hash{};
            hash.Update(storage + file.Offset, static_cast<Foundation::Size>(file.SizeBytes));
            hash.Finish(g_moduleDigests[module]);
            g_moduleHashed[module] = true;
        }
        bool same{true};
        for (Foundation::Size byte{0U}; byte < BootTrustDigestBytes; ++byte)
        {
            same = same && entry.Digest[byte] == g_moduleDigests[module][byte];
        }
        entry.Verdict = same ? BootTrustVerdict::Verified : BootTrustVerdict::DigestMismatch;
    }
}

const char* ReadImages(File& root, BootServices& services, const Foundation::UInt64 total) noexcept
{
    if (services.AllocatePagesFunction == nullptr)
    {
        return "FILE-ALLOCATION";
    }
    Foundation::UInt64 address{0U};
    const Foundation::UInt64 pages{(total + PageBytes - 1U) / PageBytes};
    if (services.AllocatePagesFunction(AllocateType::AnyPages, MemoryType::LoaderData,
                                       static_cast<Foundation::Size>(pages), &address) != Success ||
        address == 0U || (address & (PageBytes - 1U)) != 0U || address > (~Foundation::UInt64{0U} - pages * PageBytes))
    {
        return "FILE-ALLOCATION";
    }
    g_candidate.PhysicalStart = address;
    g_candidate.PageCount = pages;
    // Firmware returned identity-addressable, page-aligned LoaderData storage.
    auto* const storage = reinterpret_cast<Foundation::UInt8*>(address);
    for (Foundation::UInt64 index{0U}; index < pages * PageBytes; ++index)
    {
        storage[index] = 0U;
    }
    for (Foundation::Size index{0U}; index < g_candidate.ManifestBytes; ++index)
    {
        storage[index] = g_manifest[index];
    }
    for (Foundation::Size index{0U}; index < BootFileCount + g_candidate.ModuleCount; ++index)
    {
        const BootFileRecord& record = Record(index);
        if (record.Name[0] == '\0')
        {
            continue;
        }
        Foundation::UInt8* const bytes = storage + record.Offset;
        File* file{nullptr};
        const char* reason = OpenImage(root, index, file);
        Foundation::UInt64 size{0U};
        if (reason == nullptr)
        {
            reason = SizeOf(*file, index < BootFileCount ? BootFileMaximumBytes : BootModuleMaximumBytes, size);
        }
        if (reason == nullptr && size != record.SizeBytes)
        {
            reason = "FILE-SIZE";
        }
        if (reason == nullptr && !Whole(*file, bytes, static_cast<Foundation::Size>(record.SizeBytes)))
        {
            reason = "FILE-READ";
        }
        reason = CloseImage(file, reason);
        if (reason != nullptr)
        {
            return reason;
        }
        if (User::Filesystem::Crc32c::Compute(bytes, static_cast<Foundation::Size>(record.SizeBytes)) != record.Crc32c)
        {
            return index < BootFileCount ? "FILE-CRC32C-MISMATCH" : "MODULE-CRC32C-MISMATCH";
        }
    }
    VerifyTrust(storage);
    if (g_candidate.ModuleCount != 0U)
    {
        Foundation::UInt8* const table = storage + g_candidate.ModuleOffset;
        Store64(table, g_candidate.ModuleCount);
        for (Foundation::Size index{0U}; index < g_candidate.ModuleCount; ++index)
        {
            const BootModuleRecord& module = g_candidate.Modules[index];
            Foundation::UInt8* const entry = table + 16U + index * 32U;
            for (Foundation::Size character{0U}; character < 16U; ++character)
            {
                entry[character] = static_cast<Foundation::UInt8>(module.Name[character]);
            }
            Store64(entry + 16U, module.File.Offset - g_candidate.ModuleOffset);
            Store64(entry + 24U, module.File.SizeBytes);
        }
    }
    return nullptr;
}

/// The file protocol owns the handle. This adapter owns the one explicit page
/// allocation until it transfers its receipt; all error paths close the handle.
/// Keep its path/allocation scratch out of LoadBootFiles' handle-array frame.
[[gnu::noinline]] const char* ReadKernel(File& root, BootServices& services) noexcept
{
    Foundation::UInt16 path[27]{'\\', 'E', 'F', 'I', '\\', 'A', 'S', 'T', 'E', 'R', 'I', 'A', '\\'};
    for (Foundation::Size index{0U}; index < 13U; ++index)
    {
        path[13U + index] = static_cast<Foundation::UInt16>(g_kernelCandidate.File.Name[index]);
    }
    File* file{nullptr};
    if (root.Open(&root, &file, path, 1U, 0U) != Success || file == nullptr)
    {
        return "KERNEL-OPEN";
    }
    const char* reason = SizeOf(*file, BootFileMaximumBytes, g_kernelCandidate.File.SizeBytes);
    if (reason == nullptr)
    {
        const Foundation::UInt64 pages = (g_kernelCandidate.File.SizeBytes + PageBytes - 1U) / PageBytes;
        Foundation::UInt64 address{0U};
        if (services.AllocatePagesFunction == nullptr ||
            services.AllocatePagesFunction(AllocateType::AnyPages, MemoryType::LoaderData,
                                           static_cast<Foundation::Size>(pages), &address) != Success ||
            address == 0U || (address & (PageBytes - 1U)) != 0U ||
            address > ~Foundation::UInt64{0U} - pages * PageBytes)
        {
            reason = "KERNEL-ALLOCATION";
        }
        else
        {
            g_kernelCandidate.PhysicalStart = address;
            g_kernelCandidate.PageCount = pages;
            // The accepted firmware allocation is page-aligned writable storage
            // in the loader's identity address space until its explicit release.
            auto* const bytes = reinterpret_cast<Foundation::UInt8*>(static_cast<Foundation::UIntPointer>(address));
            for (Foundation::UInt64 index{0U}; index < pages * PageBytes; ++index)
            {
                bytes[index] = 0U;
            }
            if (!Whole(*file, bytes, static_cast<Foundation::Size>(g_kernelCandidate.File.SizeBytes)))
            {
                reason = "KERNEL-READ";
            }
            else if (User::Filesystem::Crc32c::Compute(
                         bytes, static_cast<Foundation::Size>(g_kernelCandidate.File.SizeBytes)) !=
                     g_kernelCandidate.File.Crc32c)
            {
                reason = "KERNEL-CRC32C-MISMATCH";
            }
        }
    }
    if (file->Close != nullptr && file->Close(file) != Success && reason == nullptr)
    {
        reason = "FILE-CLOSE";
    }
    return reason;
}
} // Namespace

const char* LoadBootFiles(const Handle image, BootServices& services, BootFileRegion& output, KernelFileReceipt& kernel,
                          DisplayPolicy* const display, BootTrustPolicy* const trust,
                          Foundation::UInt64* const lowMemory, Foundation::UInt64* const watchdogSeconds,
                          Foundation::UInt64* const sessionMode, Foundation::UInt64* const loginEnabled) noexcept
{
    if (services.HandleProtocolFunction == nullptr)
    {
        return "FILE-VOLUME";
    }
    Guid guid{LoadedImageProtocolGuid};
    void* interface{nullptr};
    if (services.HandleProtocolFunction(image, &guid, &interface) != Success || interface == nullptr)
    {
        return "FILE-LOADED-IMAGE";
    }
    const Handle device = static_cast<LoadedImageProtocol*>(interface)->DeviceHandle;
    guid = FileSystemGuid;
    interface = nullptr;
    if (services.HandleProtocolFunction(device, &guid, &interface) != Success || interface == nullptr)
    {
        return "FILE-VOLUME";
    }
    auto& system = *static_cast<FileSystem*>(interface);
    File* root{nullptr};
    if (system.Open == nullptr || system.Open(&system, &root) != Success || root == nullptr)
    {
        return "FILE-VOLUME";
    }
    const char* reason = root->Open == nullptr || root->Close == nullptr ? "FILE-PROTOCOL" : ReadManifest(*root);
    Foundation::UInt64 total{0U};
    if (reason == nullptr)
    {
        reason = SizeImages(*root, total);
    }
    if (reason == nullptr)
    {
        reason = ReadImages(*root, services, total);
    }
    if (reason == nullptr)
    {
        reason = ReadKernel(*root, services);
    }
    if (root->Close != nullptr && root->Close(root) != Success && reason == nullptr)
    {
        reason = "FILE-CLOSE";
    }
    if (reason == nullptr)
    {
        if (watchdogSeconds != nullptr)
        {
            *watchdogSeconds = g_candidateWatchdog;
        }
        if (sessionMode != nullptr)
        {
            *sessionMode = g_candidateSession;
        }
        if (loginEnabled != nullptr)
        {
            *loginEnabled = g_candidateLogin;
        }
        output = g_candidate;
        if (display != nullptr)
        {
            *display = g_candidateDisplay;
        }
        if (trust != nullptr)
        {
            *trust = g_candidateTrust;
        }
        if (lowMemory != nullptr)
        {
            *lowMemory = g_candidateLowMemory;
        }
        kernel = g_kernelCandidate;
    }
    return reason;
}

namespace
{
void FormatBootFileLine(Inventory::ReportText& text, const BootFileRecord& record, const char* const prefix,
                        const char* const moduleName) noexcept
{
    if (record.SizeBytes == 0U)
    {
        return;
    }
    text.Text(prefix);
    if (moduleName != nullptr)
    {
        text.Text(moduleName);
        text.Text(" ");
    }
    text.Text(record.Name);
    text.Text(" size=");
    text.Decimal(record.SizeBytes);
    text.Text(" crc32c=0x");
    text.Hex(record.Crc32c, 8U);
    text.Text("\r\n");
}
} // Namespace

void FormatBootFile(Inventory::ReportText& text, const BootFileRecord& record, const char* const moduleName) noexcept
{
    FormatBootFileLine(text, record,
                       moduleName == nullptr ? "ASTERIA:STAGE:BOOT-FILE:" : "ASTERIA:STAGE:BOOT-MODULE:", moduleName);
}

void FormatBootFiles(Inventory::ReportText& text, const BootFileRegion& region) noexcept
{
    // Report lines, not stage events: the report also reaches the firmware
    // console, which is the diagnostic channel where an SPCR or HCDP console
    // names the UART, and each stage event is reported once.
    for (const BootFileRecord& file : region.Files)
    {
        FormatBootFileLine(text, file, "Boot file: ", nullptr);
    }
    for (Foundation::Size index{0U}; index < region.ModuleCount; ++index)
    {
        FormatBootFileLine(text, region.Modules[index].File, "Boot module: ", region.Modules[index].Name);
    }
}

void FormatBootTrust(Inventory::ReportText& text, const BootTrustEntry& entry, const Foundation::Size number,
                     const BootFileRegion& modules) noexcept
{
    text.Text("ASTERIA:BOOT-TRUST:");
    text.Decimal(number);
    if ((entry.Selector & BootTrustSelectsAddress) != 0U)
    {
        text.Text(" pci=");
        text.Hex(entry.Segment, 4U);
        text.Character(':');
        text.Hex(entry.Bus, 2U);
        text.Character(':');
        text.Hex(entry.Device, 2U);
        text.Character('.');
        text.Hex(entry.Function, 1U);
    }
    if ((entry.Selector & BootTrustSelectsIdentifier) != 0U)
    {
        text.Text(" id=");
        text.Hex(entry.Vendor, 4U);
        text.Character(':');
        text.Hex(entry.Product, 4U);
    }
    text.Text(" driver=");
    text.Text(BootTrustDriverName(entry.Driver));
    text.Text(" module=");
    text.Text(entry.Module < modules.ModuleCount ? modules.Modules[entry.Module].Name : "?");
    text.Text(" sha256=");
    for (const Foundation::UInt8 byte : entry.Digest)
    {
        text.Hex(byte, 2U);
    }
    text.Text(entry.Verdict == BootTrustVerdict::Verified
                  ? " VERIFIED"
                  : (entry.Verdict == BootTrustVerdict::DigestMismatch ? " DIGEST-MISMATCH" : " UNCHECKED"));
}
} // Namespace Asteria::Boot::Uefi
