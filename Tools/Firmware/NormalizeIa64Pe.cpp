// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

namespace Asteria::Tools::Firmware
{
namespace
{
static_assert(CHAR_BIT == 8);
static_assert(sizeof(std::uint16_t) == 2U);
static_assert(sizeof(std::uint32_t) == 4U);
static_assert(sizeof(std::uint64_t) == 8U);
static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));

constexpr std::uint16_t DosSignature{0x5A4DU};
constexpr std::uint32_t PeSignature{0x0000'4550U};
constexpr std::uint16_t Ia64Machine{0x0200U};
constexpr std::uint16_t Pe32PlusMagic{0x020BU};
constexpr std::uint16_t EfiApplicationSubsystem{10U};
constexpr std::uint16_t CanonicalCharacteristics{0x022EU};
constexpr std::uint16_t CanonicalDllCharacteristics{0x0160U};
constexpr std::uint16_t MinimumOptionalHeaderBytes{160U};
constexpr std::uint32_t MinimumDataDirectoryCount{6U};
constexpr std::uint32_t PeSectionHeaderBytes{40U};
constexpr std::uint32_t PeDirectoryBytes{8U};
constexpr std::uint32_t PeDirectoryTableOffset{112U};
constexpr std::uint32_t PeBaseRelocationDirectoryIndex{5U};
constexpr std::uint32_t PeSectionMemoryMask{0xE000'0000U};
constexpr std::uint32_t PeMinimumFileAlignment{0x0200U};
constexpr std::uint32_t PeMaximumFileAlignment{0x1'0000U};
constexpr std::uint32_t PePageBytes{0x1000U};
constexpr std::uint64_t PeImageBaseAlignment{0x1'0000U};
constexpr std::uint64_t PeAddressLimit{static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1U};
constexpr std::uint16_t PeRelocationAbsolute{0U};
constexpr std::uint16_t PeRelocationIa64Imm64{9U};
constexpr std::uint16_t PeRelocationDir64{10U};
constexpr std::uint32_t RelocationPageBytes{0x1000U};

constexpr std::uint32_t ElfHeaderBytes{64U};
constexpr std::uint32_t ElfProgramHeaderBytes{56U};
constexpr std::uint32_t ElfSectionHeaderBytes{64U};
constexpr std::uint32_t ElfRelaBytes{24U};
constexpr std::uint32_t ElfSymbolBytes{24U};
constexpr std::uint16_t ElfExecutableType{2U};
constexpr std::uint16_t ElfIa64Machine{50U};
constexpr std::uint32_t ElfCurrentVersion{1U};
constexpr std::uint32_t ElfIa64Abi64Flag{0x10U};
constexpr std::uint16_t ElfExtendedIndex{0xFFFFU};
constexpr std::uint16_t ElfUndefinedIndex{0U};
constexpr std::uint16_t ElfAbsoluteIndex{0xFFF1U};
constexpr std::uint16_t ElfCommonIndex{0xFFF2U};
constexpr std::uint32_t ElfProgramLoad{1U};
constexpr std::uint32_t ElfProgramDynamic{2U};
constexpr std::uint32_t ElfProgramInterpreter{3U};
constexpr std::uint32_t ElfProgramTls{7U};
constexpr std::uint32_t ElfSectionNull{0U};
constexpr std::uint32_t ElfSectionProgramBits{1U};
constexpr std::uint32_t ElfSectionSymbolTable{2U};
constexpr std::uint32_t ElfSectionStringTable{3U};
constexpr std::uint32_t ElfSectionRela{4U};
constexpr std::uint32_t ElfSectionHash{5U};
constexpr std::uint32_t ElfSectionDynamic{6U};
constexpr std::uint32_t ElfSectionNoBits{8U};
constexpr std::uint32_t ElfSectionRel{9U};
constexpr std::uint32_t ElfSectionDynamicSymbols{11U};
constexpr std::uint32_t ElfSectionRelr{19U};
constexpr std::uint32_t ElfSectionGnuHash{0x6FFF'FFF6U};
constexpr std::uint32_t ElfSectionGnuVersionDefinition{0x6FFF'FFFDU};
constexpr std::uint32_t ElfSectionGnuVersionNeeds{0x6FFF'FFFEU};
constexpr std::uint32_t ElfSectionGnuSymbolVersions{0x6FFF'FFFFU};
constexpr std::uint64_t ElfSectionAlloc{0x2U};
constexpr std::uint64_t ElfSectionExecute{0x4U};
constexpr std::uint64_t ElfSectionTls{0x400U};

constexpr std::uint32_t Ia64RelocationImm64{0x23U};
constexpr std::uint32_t Ia64RelocationDir64Lsb{0x27U};
constexpr std::uint32_t Ia64RelocationGpRel22{0x2AU};
constexpr std::uint32_t Ia64RelocationGpRel64I{0x2BU};
constexpr std::uint32_t Ia64RelocationFptr64Lsb{0x47U};
constexpr std::uint32_t Ia64RelocationPcRel21B{0x49U};
constexpr std::uint32_t Ia64RelocationIpltLsb{0x81U};

struct PeSection
{
    unsigned char Name[8U];
    std::uint32_t VirtualSize;
    std::uint32_t VirtualAddress;
    std::uint32_t RawSize;
    std::uint32_t RawOffset;
    std::uint32_t Characteristics;
};

struct PeImageInfo
{
    std::uint64_t FileSize;
    std::uint64_t PeOffset;
    std::uint64_t OptionalHeaderOffset;
    std::uint64_t RelocationDirectoryOffset;
    std::uint64_t ImageBase;
    std::uint32_t SectionAlignment;
    std::uint32_t FileAlignment;
    std::uint32_t SizeOfImage;
    std::uint32_t SizeOfHeaders;
    PeSection RelocationSection;
    std::vector<PeSection> Sections;
};

struct ElfSection
{
    std::uint32_t NameOffset;
    std::uint32_t Type;
    std::uint64_t Flags;
    std::uint64_t Address;
    std::uint64_t Offset;
    std::uint64_t Size;
    std::uint32_t Link;
    std::uint32_t Info;
    std::uint64_t AddressAlignment;
    std::uint64_t EntrySize;
};

struct ElfImageInfo
{
    std::uint64_t FileSize;
    std::uint64_t SectionHeaderOffset;
    std::uint64_t SectionCount;
    std::uint64_t SectionNameOffset;
    std::uint64_t SectionNameSize;
    bool HasOpdSection;
    ElfSection OpdSection;
};

struct SectionNamePrefix
{
    unsigned char Bytes[32U];
};

struct ElfAllocatedRange
{
    std::uint64_t Start;
    std::uint64_t End;
};

struct SourceRelocation
{
    std::uint32_t Rva;
    std::uint32_t Type;
};

struct BaseRelocation
{
    std::uint32_t Rva;
    std::uint16_t Type;
};

[[nodiscard]] bool ReportFailure(const char* const message) noexcept
{
    static_cast<void>(std::fprintf(stderr, "NormalizeIa64Pe: %s\n", message));
    return false;
}

[[nodiscard]] bool ReportUnsupportedRelocation(const std::uint32_t type) noexcept
{
    static_cast<void>(std::fprintf(stderr, "NormalizeIa64Pe: unsupported allocated IA-64 relocation 0x%08x\n",
                                   static_cast<unsigned int>(type)));
    return false;
}

[[nodiscard]] bool StringEquals(const char* const left, const char* const right) noexcept
{
    if ((left == nullptr) || (right == nullptr))
    {
        return false;
    }
    std::size_t index{0U};
    while ((left[index] != '\0') && (right[index] != '\0'))
    {
        if (left[index] != right[index])
        {
            return false;
        }
        ++index;
    }
    return left[index] == right[index];
}

[[nodiscard]] std::size_t StringLength(const char* const text) noexcept
{
    std::size_t length{0U};
    while (text[length] != '\0')
    {
        ++length;
    }
    return length;
}

[[nodiscard]] bool TryAdd(const std::uint64_t left, const std::uint64_t right, std::uint64_t& result) noexcept
{
    if (left > (std::numeric_limits<std::uint64_t>::max() - right))
    {
        return false;
    }
    result = left + right;
    return true;
}

[[nodiscard]] bool TryMultiply(const std::uint64_t left, const std::uint64_t right, std::uint64_t& result) noexcept
{
    if ((left != 0U) && (right > (std::numeric_limits<std::uint64_t>::max() / left)))
    {
        return false;
    }
    result = left * right;
    return true;
}

[[nodiscard]] bool IsPowerOfTwo(const std::uint64_t value) noexcept
{
    return (value != 0U) && ((value & (value - 1U)) == 0U);
}

[[nodiscard]] bool TryAlignUp(const std::uint64_t value, const std::uint64_t alignment, std::uint64_t& result) noexcept
{
    if (!IsPowerOfTwo(alignment))
    {
        return false;
    }
    const std::uint64_t mask = alignment - 1U;
    std::uint64_t adjusted{0U};
    if (!TryAdd(value, mask, adjusted))
    {
        return false;
    }
    result = adjusted & ~mask;
    return true;
}

[[nodiscard]] bool IsRangeInFile(const std::uint64_t offset, const std::uint64_t byteCount,
                                 const std::uint64_t fileSize) noexcept
{
    return (offset <= fileSize) && (byteCount <= (fileSize - offset));
}

[[nodiscard]] bool GetFileSize(std::FILE* const file, std::uint64_t& fileSize) noexcept
{
    if (std::fseek(file, 0L, SEEK_END) != 0)
    {
        return false;
    }
    const long position = std::ftell(file);
    if ((position < 0L) || (std::fseek(file, 0L, SEEK_SET) != 0))
    {
        return false;
    }
    fileSize = static_cast<std::uint64_t>(position);
    return true;
}

[[nodiscard]] bool ReadBytes(std::FILE* const file, const std::uint64_t fileSize, const std::uint64_t offset,
                             unsigned char* const bytes, const std::size_t byteCount) noexcept
{
    const std::uint64_t count = static_cast<std::uint64_t>(byteCount);
    if (!IsRangeInFile(offset, count, fileSize) ||
        (offset > static_cast<std::uint64_t>(std::numeric_limits<long>::max())))
    {
        return false;
    }
    if (byteCount == 0U)
    {
        return true;
    }
    return (std::fseek(file, static_cast<long>(offset), SEEK_SET) == 0) &&
           (std::fread(bytes, 1U, byteCount, file) == byteCount);
}

[[nodiscard]] bool WriteBytes(std::FILE* const file, const std::uint64_t fileSize, const std::uint64_t offset,
                              const unsigned char* const bytes, const std::size_t byteCount) noexcept
{
    const std::uint64_t count = static_cast<std::uint64_t>(byteCount);
    if (!IsRangeInFile(offset, count, fileSize) ||
        (offset > static_cast<std::uint64_t>(std::numeric_limits<long>::max())))
    {
        return false;
    }
    if (byteCount == 0U)
    {
        return true;
    }
    return (std::fseek(file, static_cast<long>(offset), SEEK_SET) == 0) &&
           (std::fwrite(bytes, 1U, byteCount, file) == byteCount);
}

[[nodiscard]] std::uint16_t LoadUInt16(const unsigned char* const bytes) noexcept
{
    return static_cast<std::uint16_t>(bytes[0U]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1U]) << 8U);
}

[[nodiscard]] std::uint32_t LoadUInt32(const unsigned char* const bytes) noexcept
{
    return static_cast<std::uint32_t>(bytes[0U]) | (static_cast<std::uint32_t>(bytes[1U]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2U]) << 16U) | (static_cast<std::uint32_t>(bytes[3U]) << 24U);
}

[[nodiscard]] std::uint64_t LoadUInt64(const unsigned char* const bytes) noexcept
{
    return static_cast<std::uint64_t>(bytes[0U]) | (static_cast<std::uint64_t>(bytes[1U]) << 8U) |
           (static_cast<std::uint64_t>(bytes[2U]) << 16U) | (static_cast<std::uint64_t>(bytes[3U]) << 24U) |
           (static_cast<std::uint64_t>(bytes[4U]) << 32U) | (static_cast<std::uint64_t>(bytes[5U]) << 40U) |
           (static_cast<std::uint64_t>(bytes[6U]) << 48U) | (static_cast<std::uint64_t>(bytes[7U]) << 56U);
}

void StoreUInt16(unsigned char* const bytes, const std::uint16_t value) noexcept
{
    bytes[0U] = static_cast<unsigned char>(value & 0xFFU);
    bytes[1U] = static_cast<unsigned char>((value >> 8U) & 0xFFU);
}

void StoreUInt32(unsigned char* const bytes, const std::uint32_t value) noexcept
{
    bytes[0U] = static_cast<unsigned char>(value & 0xFFU);
    bytes[1U] = static_cast<unsigned char>((value >> 8U) & 0xFFU);
    bytes[2U] = static_cast<unsigned char>((value >> 16U) & 0xFFU);
    bytes[3U] = static_cast<unsigned char>((value >> 24U) & 0xFFU);
}

[[nodiscard]] bool PeNameEquals(const unsigned char* const name, const char* const expected) noexcept
{
    const std::size_t expectedLength = StringLength(expected);
    if (expectedLength > 8U)
    {
        return false;
    }
    for (std::size_t index{0U}; index < expectedLength; ++index)
    {
        if (name[index] != static_cast<unsigned char>(expected[index]))
        {
            return false;
        }
    }
    for (std::size_t index{expectedLength}; index < 8U; ++index)
    {
        if (name[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool IsPeDynamicArtifact(const unsigned char* const name) noexcept
{
    constexpr const char* artifactNames[]{
        ".edata", ".idata", ".didat", ".got", ".plt", ".iplt", ".tls", ".cormeta", ".loadcfg", ".CRT",
    };
    for (const char* const artifactName : artifactNames)
    {
        const std::size_t length = StringLength(artifactName);
        if (length > 8U)
        {
            continue;
        }
        bool matches{true};
        for (std::size_t index{0U}; index < length; ++index)
        {
            if (name[index] != static_cast<unsigned char>(artifactName[index]))
            {
                matches = false;
                break;
            }
        }
        if (matches && (length == 8U))
        {
            return true;
        }
        if (matches && ((name[length] == 0U) || (name[length] == static_cast<unsigned char>('.')) ||
                        (name[length] == static_cast<unsigned char>('$'))))
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::uint64_t GetPeVirtualSize(const PeSection& section) noexcept
{
    return section.VirtualSize;
}

[[nodiscard]] bool IsPeSectionLoaded(const PeSection& section) noexcept
{
    return (section.Characteristics & PeSectionMemoryMask) != 0U;
}

[[nodiscard]] bool ParsePeImage(std::FILE* const file, PeImageInfo& image)
{
    image.Sections.clear();
    image.PeOffset = 0U;
    image.OptionalHeaderOffset = 0U;
    image.RelocationDirectoryOffset = 0U;
    image.ImageBase = 0U;
    image.SectionAlignment = 0U;
    image.FileAlignment = 0U;
    image.SizeOfImage = 0U;
    image.SizeOfHeaders = 0U;
    image.RelocationSection = PeSection{};

    if (!GetFileSize(file, image.FileSize))
    {
        return ReportFailure("cannot determine PE image length");
    }
    unsigned char dosHeader[64U]{};
    if (!ReadBytes(file, image.FileSize, 0U, dosHeader, sizeof(dosHeader)) || (LoadUInt16(dosHeader) != DosSignature))
    {
        return ReportFailure("invalid PE DOS header");
    }

    image.PeOffset = LoadUInt32(dosHeader + 0x3CU);
    unsigned char coffHeader[24U]{};
    if (!ReadBytes(file, image.FileSize, image.PeOffset, coffHeader, sizeof(coffHeader)) ||
        (LoadUInt32(coffHeader) != PeSignature) || (LoadUInt16(coffHeader + 4U) != Ia64Machine))
    {
        return ReportFailure("invalid IA-64 PE header");
    }
    const std::uint16_t sectionCount = LoadUInt16(coffHeader + 6U);
    const std::uint16_t optionalHeaderBytes = LoadUInt16(coffHeader + 20U);
    if ((sectionCount == 0U) || (optionalHeaderBytes < MinimumOptionalHeaderBytes))
    {
        return ReportFailure("invalid PE header sizes");
    }
    if (!TryAdd(image.PeOffset, 24U, image.OptionalHeaderOffset))
    {
        return ReportFailure("overflow in PE optional-header offset");
    }
    std::vector<unsigned char> optionalHeader(optionalHeaderBytes);
    if (!ReadBytes(file, image.FileSize, image.OptionalHeaderOffset, optionalHeader.data(), optionalHeader.size()) ||
        (LoadUInt16(optionalHeader.data()) != Pe32PlusMagic) ||
        (LoadUInt16(optionalHeader.data() + 68U) != EfiApplicationSubsystem))
    {
        return ReportFailure("invalid PE32+ optional header");
    }

    const std::uint32_t directoryCount = LoadUInt32(optionalHeader.data() + 108U);
    image.SectionAlignment = LoadUInt32(optionalHeader.data() + 32U);
    image.FileAlignment = LoadUInt32(optionalHeader.data() + 36U);
    image.SizeOfImage = LoadUInt32(optionalHeader.data() + 56U);
    image.SizeOfHeaders = LoadUInt32(optionalHeader.data() + 60U);
    const std::uint64_t availableDirectories =
        (static_cast<std::uint64_t>(optionalHeaderBytes) - PeDirectoryTableOffset) / PeDirectoryBytes;
    if ((directoryCount < MinimumDataDirectoryCount) ||
        (static_cast<std::uint64_t>(directoryCount) > availableDirectories))
    {
        return ReportFailure("invalid PE data-directory table");
    }
    if (!IsPowerOfTwo(image.SectionAlignment) || !IsPowerOfTwo(image.FileAlignment) ||
        (image.FileAlignment < PeMinimumFileAlignment) || (image.FileAlignment > PeMaximumFileAlignment) ||
        (image.SectionAlignment < image.FileAlignment) ||
        ((image.SectionAlignment < PePageBytes) && (image.SectionAlignment != image.FileAlignment)))
    {
        return ReportFailure("invalid PE section or file alignment");
    }
    image.ImageBase = LoadUInt64(optionalHeader.data() + 24U);
    std::uint64_t imageEnd{0U};
    if (((image.ImageBase % PeImageBaseAlignment) != 0U) || !TryAdd(image.ImageBase, image.SizeOfImage, imageEnd))
    {
        return ReportFailure("invalid PE image base range");
    }
    const std::uint64_t relocationDirectoryInOptional =
        PeDirectoryTableOffset + (PeBaseRelocationDirectoryIndex * PeDirectoryBytes);
    if (!TryAdd(image.OptionalHeaderOffset, relocationDirectoryInOptional, image.RelocationDirectoryOffset))
    {
        return ReportFailure("overflow in PE relocation-directory offset");
    }

    constexpr std::uint32_t dynamicDirectoryIndices[]{
        0U, 1U, 7U, 9U, 10U, 11U, 12U, 13U, 14U, 15U,
    };
    for (const std::uint32_t index : dynamicDirectoryIndices)
    {
        if (index >= directoryCount)
        {
            continue;
        }
        const std::size_t offset = static_cast<std::size_t>(PeDirectoryTableOffset + (index * PeDirectoryBytes));
        if ((LoadUInt32(optionalHeader.data() + offset) != 0U) ||
            (LoadUInt32(optionalHeader.data() + offset + 4U) != 0U))
        {
            return ReportFailure("unexpected PE import/dynamic directory");
        }
    }

    std::uint64_t sectionTableOffset{0U};
    std::uint64_t sectionTableBytes{0U};
    std::uint64_t sectionTableEnd{0U};
    std::uint64_t expectedHeadersSize{0U};
    if (!TryAdd(image.OptionalHeaderOffset, optionalHeaderBytes, sectionTableOffset) ||
        !TryMultiply(sectionCount, PeSectionHeaderBytes, sectionTableBytes) ||
        !TryAdd(sectionTableOffset, sectionTableBytes, sectionTableEnd) ||
        !TryAlignUp(sectionTableEnd, image.FileAlignment, expectedHeadersSize) ||
        !IsRangeInFile(sectionTableOffset, sectionTableBytes, image.FileSize) ||
        (expectedHeadersSize > std::numeric_limits<std::uint32_t>::max()) ||
        (image.SizeOfHeaders != expectedHeadersSize) || (image.SizeOfHeaders > image.FileSize))
    {
        return ReportFailure("invalid PE section table");
    }

    bool hasRelocationSection{false};
    image.Sections.reserve(sectionCount);
    for (std::uint32_t index{0U}; index < sectionCount; ++index)
    {
        std::uint64_t entryDelta{0U};
        std::uint64_t entryOffset{0U};
        if (!TryMultiply(index, PeSectionHeaderBytes, entryDelta) ||
            !TryAdd(sectionTableOffset, entryDelta, entryOffset))
        {
            return ReportFailure("overflow in PE section-header offset");
        }
        unsigned char header[PeSectionHeaderBytes]{};
        if (!ReadBytes(file, image.FileSize, entryOffset, header, sizeof(header)))
        {
            return ReportFailure("cannot read PE section header");
        }
        PeSection section{};
        for (std::size_t nameIndex{0U}; nameIndex < 8U; ++nameIndex)
        {
            section.Name[nameIndex] = header[nameIndex];
        }
        section.VirtualSize = LoadUInt32(header + 8U);
        section.VirtualAddress = LoadUInt32(header + 12U);
        section.RawSize = LoadUInt32(header + 16U);
        section.RawOffset = LoadUInt32(header + 20U);
        section.Characteristics = LoadUInt32(header + 36U);
        std::uint64_t rawEnd{0U};
        std::uint64_t maximumRawSize{0U};
        if ((section.RawSize != 0U) &&
            (((section.RawOffset % image.FileAlignment) != 0U) || ((section.RawSize % image.FileAlignment) != 0U) ||
             (section.RawOffset < image.SizeOfHeaders) ||
             !TryAlignUp(section.VirtualSize, image.FileAlignment, maximumRawSize) ||
             (section.RawSize > maximumRawSize) || !TryAdd(section.RawOffset, section.RawSize, rawEnd) ||
             (rawEnd > PeAddressLimit) || !IsRangeInFile(section.RawOffset, section.RawSize, image.FileSize)))
        {
            return ReportFailure("invalid PE raw section range");
        }
        if ((section.VirtualAddress % image.SectionAlignment) != 0U)
        {
            return ReportFailure("misaligned PE virtual section");
        }
        if (IsPeSectionLoaded(section) && IsPeDynamicArtifact(section.Name))
        {
            return ReportFailure("unexpected loaded PE import/PLT/GOT section");
        }
        if (PeNameEquals(section.Name, ".reloc"))
        {
            if (hasRelocationSection)
            {
                return ReportFailure("duplicate PE .reloc section");
            }
            hasRelocationSection = true;
            image.RelocationSection = section;
        }
        image.Sections.push_back(section);
    }
    if (!hasRelocationSection || !IsPeSectionLoaded(image.RelocationSection) ||
        (image.RelocationSection.VirtualSize == 0U) || (image.RelocationSection.RawSize == 0U))
    {
        return ReportFailure("missing loaded raw-backed PE .reloc section");
    }

    std::sort(image.Sections.begin(), image.Sections.end(),
              [](const PeSection& left, const PeSection& right)
              {
                  return left.VirtualAddress < right.VirtualAddress;
              });
    std::uint64_t headersVirtualEnd{0U};
    if (!TryAlignUp(image.SizeOfHeaders, image.SectionAlignment, headersVirtualEnd))
    {
        return ReportFailure("overflow in PE header virtual range");
    }
    std::uint64_t expectedImageSize = headersVirtualEnd;
    std::uint64_t previousVirtualEnd = headersVirtualEnd;
    bool hasPreviousVirtualRange{false};
    for (const PeSection& section : image.Sections)
    {
        const std::uint64_t virtualSize = GetPeVirtualSize(section);
        if (virtualSize == 0U)
        {
            continue;
        }
        const std::uint64_t start = section.VirtualAddress;
        std::uint64_t end{0U};
        std::uint64_t alignedEnd{0U};
        if ((start < headersVirtualEnd) || !TryAdd(start, virtualSize, end) || (end > PeAddressLimit) ||
            !TryAlignUp(end, image.SectionAlignment, alignedEnd) || (alignedEnd > PeAddressLimit) ||
            (hasPreviousVirtualRange && (start < previousVirtualEnd)))
        {
            return ReportFailure("invalid or overlapping PE virtual sections");
        }
        previousVirtualEnd = alignedEnd;
        expectedImageSize = alignedEnd;
        hasPreviousVirtualRange = true;
    }
    if ((expectedImageSize > std::numeric_limits<std::uint32_t>::max()) || (image.SizeOfImage != expectedImageSize) ||
        ((image.SizeOfImage % image.SectionAlignment) != 0U))
    {
        return ReportFailure("invalid PE image size");
    }

    std::vector<PeSection> rawSections = image.Sections;
    std::sort(rawSections.begin(), rawSections.end(),
              [](const PeSection& left, const PeSection& right)
              {
                  return left.RawOffset < right.RawOffset;
              });
    std::uint64_t previousRawEnd{0U};
    bool hasPreviousRawRange{false};
    for (const PeSection& section : rawSections)
    {
        if (section.RawSize == 0U)
        {
            continue;
        }
        const std::uint64_t start = section.RawOffset;
        std::uint64_t end{0U};
        if (!TryAdd(start, section.RawSize, end) || (hasPreviousRawRange && (start < previousRawEnd)))
        {
            return ReportFailure("overlapping PE raw sections");
        }
        previousRawEnd = end;
        hasPreviousRawRange = true;
    }
    return true;
}

[[nodiscard]] ElfSection DecodeElfSection(const unsigned char* const header) noexcept
{
    return ElfSection{
        .NameOffset = LoadUInt32(header),
        .Type = LoadUInt32(header + 4U),
        .Flags = LoadUInt64(header + 8U),
        .Address = LoadUInt64(header + 16U),
        .Offset = LoadUInt64(header + 24U),
        .Size = LoadUInt64(header + 32U),
        .Link = LoadUInt32(header + 40U),
        .Info = LoadUInt32(header + 44U),
        .AddressAlignment = LoadUInt64(header + 48U),
        .EntrySize = LoadUInt64(header + 56U),
    };
}

[[nodiscard]] bool ReadElfSectionAt(std::FILE* const file, const std::uint64_t fileSize,
                                    const std::uint64_t sectionHeaderOffset, const std::uint64_t index,
                                    ElfSection& section) noexcept
{
    std::uint64_t offsetDelta{0U};
    std::uint64_t offset{0U};
    if (!TryMultiply(index, ElfSectionHeaderBytes, offsetDelta) || !TryAdd(sectionHeaderOffset, offsetDelta, offset))
    {
        return false;
    }
    unsigned char header[ElfSectionHeaderBytes]{};
    if (!ReadBytes(file, fileSize, offset, header, sizeof(header)))
    {
        return false;
    }
    section = DecodeElfSection(header);
    return true;
}

[[nodiscard]] bool ReadElfSection(std::FILE* const file, const ElfImageInfo& image, const std::uint64_t index,
                                  ElfSection& section) noexcept
{
    if (index >= image.SectionCount)
    {
        return false;
    }
    return ReadElfSectionAt(file, image.FileSize, image.SectionHeaderOffset, index, section);
}

[[nodiscard]] bool ReadSectionNamePrefix(std::FILE* const file, const ElfImageInfo& image, const ElfSection& section,
                                         SectionNamePrefix& name) noexcept
{
    name = SectionNamePrefix{};
    if (section.NameOffset >= image.SectionNameSize)
    {
        return false;
    }
    const std::uint64_t available = image.SectionNameSize - section.NameOffset;
    const std::uint64_t bufferSize = sizeof(name.Bytes);
    const std::uint64_t count = available < bufferSize ? available : bufferSize;
    std::uint64_t offset{0U};
    if (!TryAdd(image.SectionNameOffset, section.NameOffset, offset))
    {
        return false;
    }
    return ReadBytes(file, image.FileSize, offset, name.Bytes, static_cast<std::size_t>(count));
}

[[nodiscard]] bool SectionNameEquals(const SectionNamePrefix& name, const char* const expected) noexcept
{
    const std::size_t length = StringLength(expected);
    if (length >= sizeof(name.Bytes))
    {
        return false;
    }
    for (std::size_t index{0U}; index < length; ++index)
    {
        if (name.Bytes[index] != static_cast<unsigned char>(expected[index]))
        {
            return false;
        }
    }
    return name.Bytes[length] == 0U;
}

[[nodiscard]] bool SectionNameIsOrHasDotSuffix(const SectionNamePrefix& name, const char* const expected) noexcept
{
    const std::size_t length = StringLength(expected);
    if (length >= sizeof(name.Bytes))
    {
        return false;
    }
    for (std::size_t index{0U}; index < length; ++index)
    {
        if (name.Bytes[index] != static_cast<unsigned char>(expected[index]))
        {
            return false;
        }
    }
    return (name.Bytes[length] == 0U) || (name.Bytes[length] == static_cast<unsigned char>('.'));
}

[[nodiscard]] bool IsElfDynamicArtifact(const ElfSection& section, const SectionNamePrefix& name) noexcept
{
    return (section.Type == ElfSectionDynamic) || (section.Type == ElfSectionDynamicSymbols) ||
           (section.Type == ElfSectionHash) || (section.Type == ElfSectionRelr) ||
           (section.Type == ElfSectionGnuHash) || (section.Type == ElfSectionGnuVersionDefinition) ||
           (section.Type == ElfSectionGnuVersionNeeds) || (section.Type == ElfSectionGnuSymbolVersions) ||
           SectionNameIsOrHasDotSuffix(name, ".dynamic") || SectionNameIsOrHasDotSuffix(name, ".dynsym") ||
           SectionNameIsOrHasDotSuffix(name, ".dynstr") || SectionNameIsOrHasDotSuffix(name, ".interp") ||
           SectionNameIsOrHasDotSuffix(name, ".plt") || SectionNameIsOrHasDotSuffix(name, ".got") ||
           SectionNameIsOrHasDotSuffix(name, ".iplt") || SectionNameIsOrHasDotSuffix(name, ".hash") ||
           SectionNameIsOrHasDotSuffix(name, ".gnu.hash") || SectionNameIsOrHasDotSuffix(name, ".gnu.version") ||
           SectionNameIsOrHasDotSuffix(name, ".gnu.version_d") || SectionNameIsOrHasDotSuffix(name, ".gnu.version_r") ||
           SectionNameIsOrHasDotSuffix(name, ".rela.dyn") || SectionNameIsOrHasDotSuffix(name, ".rela.plt") ||
           SectionNameIsOrHasDotSuffix(name, ".rel.dyn") || SectionNameIsOrHasDotSuffix(name, ".rel.plt") ||
           SectionNameIsOrHasDotSuffix(name, ".IA_64.pltoff");
}

[[nodiscard]] bool ParseElfImage(std::FILE* const file, ElfImageInfo& image)
{
    image.SectionHeaderOffset = 0U;
    image.SectionCount = 0U;
    image.SectionNameOffset = 0U;
    image.SectionNameSize = 0U;
    image.HasOpdSection = false;
    image.OpdSection = ElfSection{};
    if (!GetFileSize(file, image.FileSize))
    {
        return ReportFailure("cannot determine ELF image length");
    }

    unsigned char header[ElfHeaderBytes]{};
    if (!ReadBytes(file, image.FileSize, 0U, header, sizeof(header)) || (header[0U] != 0x7FU) ||
        (header[1U] != static_cast<unsigned char>('E')) || (header[2U] != static_cast<unsigned char>('L')) ||
        (header[3U] != static_cast<unsigned char>('F')) || (header[4U] != 2U) || (header[5U] != 1U) ||
        (header[6U] != 1U) || (LoadUInt16(header + 16U) != ElfExecutableType) ||
        (LoadUInt16(header + 18U) != ElfIa64Machine) || (LoadUInt32(header + 20U) != ElfCurrentVersion) ||
        ((LoadUInt32(header + 48U) & ElfIa64Abi64Flag) == 0U) || (LoadUInt16(header + 52U) != ElfHeaderBytes) ||
        (LoadUInt16(header + 58U) != ElfSectionHeaderBytes))
    {
        return ReportFailure("invalid little-endian ELF64 IA-64 header");
    }

    const std::uint64_t programHeaderOffset = LoadUInt64(header + 32U);
    image.SectionHeaderOffset = LoadUInt64(header + 40U);
    const std::uint16_t programHeaderEntryBytes = LoadUInt16(header + 54U);
    const std::uint16_t rawProgramCount = LoadUInt16(header + 56U);
    const std::uint16_t rawSectionCount = LoadUInt16(header + 60U);
    const std::uint16_t rawSectionNameIndex = LoadUInt16(header + 62U);
    if (image.SectionHeaderOffset == 0U)
    {
        return ReportFailure("ELF section table is missing");
    }

    ElfSection sectionZero{};
    if (!ReadElfSectionAt(file, image.FileSize, image.SectionHeaderOffset, 0U, sectionZero) ||
        (sectionZero.Type != ElfSectionNull))
    {
        return ReportFailure("invalid ELF section zero");
    }
    image.SectionCount = rawSectionCount == 0U ? sectionZero.Size : rawSectionCount;
    const std::uint64_t sectionNameIndex =
        rawSectionNameIndex == ElfExtendedIndex ? sectionZero.Link : rawSectionNameIndex;
    const std::uint64_t programCount = rawProgramCount == ElfExtendedIndex ? sectionZero.Info : rawProgramCount;
    if ((image.SectionCount == 0U) || (image.SectionCount > std::numeric_limits<std::uint32_t>::max()) ||
        (sectionNameIndex == 0U) || (sectionNameIndex >= image.SectionCount))
    {
        return ReportFailure("invalid ELF extended section metadata");
    }
    std::uint64_t sectionTableBytes{0U};
    if (!TryMultiply(image.SectionCount, ElfSectionHeaderBytes, sectionTableBytes) ||
        !IsRangeInFile(image.SectionHeaderOffset, sectionTableBytes, image.FileSize))
    {
        return ReportFailure("invalid ELF section-table range");
    }

    if (programCount != 0U)
    {
        std::uint64_t programTableBytes{0U};
        if ((programHeaderEntryBytes != ElfProgramHeaderBytes) ||
            !TryMultiply(programCount, ElfProgramHeaderBytes, programTableBytes) ||
            !IsRangeInFile(programHeaderOffset, programTableBytes, image.FileSize))
        {
            return ReportFailure("invalid ELF program-header table");
        }
        for (std::uint64_t index{0U}; index < programCount; ++index)
        {
            std::uint64_t offsetDelta{0U};
            std::uint64_t offset{0U};
            if (!TryMultiply(index, ElfProgramHeaderBytes, offsetDelta) ||
                !TryAdd(programHeaderOffset, offsetDelta, offset))
            {
                return ReportFailure("overflow in ELF program-header offset");
            }
            unsigned char programHeader[ElfProgramHeaderBytes]{};
            if (!ReadBytes(file, image.FileSize, offset, programHeader, sizeof(programHeader)))
            {
                return ReportFailure("cannot read ELF program header");
            }
            const std::uint32_t type = LoadUInt32(programHeader);
            const std::uint64_t fileOffset = LoadUInt64(programHeader + 8U);
            const std::uint64_t virtualAddress = LoadUInt64(programHeader + 16U);
            const std::uint64_t fileBytes = LoadUInt64(programHeader + 32U);
            const std::uint64_t memoryBytes = LoadUInt64(programHeader + 40U);
            const std::uint64_t alignment = LoadUInt64(programHeader + 48U);
            if ((type == ElfProgramDynamic) || (type == ElfProgramInterpreter) || (type == ElfProgramTls))
            {
                return ReportFailure("unexpected dynamic or TLS ELF program header");
            }
            std::uint64_t virtualEnd{0U};
            if ((fileBytes > memoryBytes) || !IsRangeInFile(fileOffset, fileBytes, image.FileSize) ||
                ((alignment > 1U) && !IsPowerOfTwo(alignment)) || !TryAdd(virtualAddress, memoryBytes, virtualEnd))
            {
                return ReportFailure("invalid ELF program-header range");
            }
            if ((type == ElfProgramLoad) && (alignment > 1U) &&
                ((virtualAddress % alignment) != (fileOffset % alignment)))
            {
                return ReportFailure("misaligned ELF load segment");
            }
        }
    }

    ElfSection sectionNameSection{};
    if (!ReadElfSection(file, image, sectionNameIndex, sectionNameSection) ||
        (sectionNameSection.Type != ElfSectionStringTable) || (sectionNameSection.Size == 0U) ||
        !IsRangeInFile(sectionNameSection.Offset, sectionNameSection.Size, image.FileSize))
    {
        return ReportFailure("invalid ELF section-name table");
    }
    image.SectionNameOffset = sectionNameSection.Offset;
    image.SectionNameSize = sectionNameSection.Size;
    unsigned char boundaryBytes[2U]{};
    if (!ReadBytes(file, image.FileSize, image.SectionNameOffset, boundaryBytes, 1U) ||
        !ReadBytes(file, image.FileSize, image.SectionNameOffset + image.SectionNameSize - 1U, boundaryBytes + 1U,
                   1U) ||
        (boundaryBytes[0U] != 0U) || (boundaryBytes[1U] != 0U))
    {
        return ReportFailure("unterminated ELF section-name table");
    }

    std::vector<ElfAllocatedRange> allocatedRanges;
    for (std::uint64_t index{0U}; index < image.SectionCount; ++index)
    {
        ElfSection section{};
        SectionNamePrefix name{};
        if (!ReadElfSection(file, image, index, section) || !ReadSectionNamePrefix(file, image, section, name))
        {
            return ReportFailure("invalid ELF section header or name");
        }
        if ((section.AddressAlignment != 0U) && !IsPowerOfTwo(section.AddressAlignment))
        {
            return ReportFailure("invalid ELF section alignment");
        }
        if ((section.Type != ElfSectionNull) && (section.Type != ElfSectionNoBits) &&
            !IsRangeInFile(section.Offset, section.Size, image.FileSize))
        {
            return ReportFailure("invalid ELF section file range");
        }
        if ((section.Flags & ElfSectionTls) != 0U)
        {
            return ReportFailure("unexpected ELF TLS section");
        }
        if (IsElfDynamicArtifact(section, name))
        {
            return ReportFailure("unexpected ELF dynamic/PLT/GOT artifact");
        }
        if ((section.Flags & ElfSectionAlloc) == 0U)
        {
            continue;
        }
        if ((section.AddressAlignment > 1U) && ((section.Address % section.AddressAlignment) != 0U))
        {
            return ReportFailure("misaligned allocated ELF section address");
        }
        std::uint64_t sectionEnd{0U};
        if (!TryAdd(section.Address, section.Size, sectionEnd))
        {
            return ReportFailure("overflow in allocated ELF section range");
        }
        if (section.Size != 0U)
        {
            allocatedRanges.push_back(ElfAllocatedRange{
                .Start = section.Address,
                .End = sectionEnd,
            });
        }
        if ((section.Type == ElfSectionRela) || (section.Type == ElfSectionRel))
        {
            return ReportFailure("unexpected allocated ELF relocation section");
        }
        if (SectionNameEquals(name, ".opd"))
        {
            if (image.HasOpdSection || (section.Type != ElfSectionProgramBits))
            {
                return ReportFailure("invalid allocated ELF .opd section");
            }
            image.HasOpdSection = true;
            image.OpdSection = section;
        }
    }
    std::sort(allocatedRanges.begin(), allocatedRanges.end(),
              [](const ElfAllocatedRange& left, const ElfAllocatedRange& right)
              {
                  return left.Start < right.Start;
              });
    for (std::size_t index{1U}; index < allocatedRanges.size(); ++index)
    {
        if (allocatedRanges[index].Start < allocatedRanges[index - 1U].End)
        {
            return ReportFailure("overlapping allocated ELF sections");
        }
    }
    return true;
}

[[nodiscard]] bool TryAddressToRva(const std::uint64_t address, const std::uint64_t imageBase,
                                   std::uint32_t& rva) noexcept
{
    if (address < imageBase)
    {
        return false;
    }
    const std::uint64_t difference = address - imageBase;
    if (difference > std::numeric_limits<std::uint32_t>::max())
    {
        return false;
    }
    rva = static_cast<std::uint32_t>(difference);
    return true;
}

[[nodiscard]] bool IsRangeInElfSection(const ElfSection& section, const std::uint64_t address,
                                       const std::uint64_t byteCount) noexcept
{
    if (address < section.Address)
    {
        return false;
    }
    const std::uint64_t offset = address - section.Address;
    return (offset <= section.Size) && (byteCount <= (section.Size - offset));
}

[[nodiscard]] bool IsRangeInLoadedPeSection(const PeImageInfo& image, const std::uint32_t rva,
                                            const std::uint32_t byteCount) noexcept
{
    const std::uint64_t rangeStart = rva;
    std::uint64_t rangeEnd{0U};
    if (!TryAdd(rangeStart, byteCount, rangeEnd) || (rangeEnd > PeAddressLimit))
    {
        return false;
    }
    for (const PeSection& section : image.Sections)
    {
        if (!IsPeSectionLoaded(section) || (section.RawSize == 0U))
        {
            continue;
        }
        const std::uint64_t sectionStart = section.VirtualAddress;
        std::uint64_t loadedEnd{0U};
        std::uint64_t rawEnd{0U};
        if (!TryAdd(sectionStart, GetPeVirtualSize(section), loadedEnd) ||
            !TryAdd(sectionStart, section.RawSize, rawEnd))
        {
            return false;
        }
        if ((rangeStart >= sectionStart) && (rangeEnd <= loadedEnd) && (rangeEnd <= rawEnd))
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool DoesRangeOverlapRelocationReservation(const PeImageInfo& image, const std::uint32_t rva,
                                                         const std::uint32_t byteCount) noexcept
{
    const std::uint64_t rangeStart = rva;
    const std::uint64_t reservationStart = image.RelocationSection.VirtualAddress;
    std::uint64_t rangeEnd{0U};
    std::uint64_t reservationEnd{0U};
    if (!TryAdd(rangeStart, byteCount, rangeEnd) ||
        !TryAdd(reservationStart, image.RelocationSection.VirtualSize, reservationEnd))
    {
        return true;
    }
    return (rangeStart < reservationEnd) && (reservationStart < rangeEnd);
}

[[nodiscard]] bool IsValidPeRelocationTargetRange(const PeImageInfo& image, const std::uint32_t rva,
                                                  const std::uint32_t byteCount) noexcept
{
    return IsRangeInLoadedPeSection(image, rva, byteCount) &&
           !DoesRangeOverlapRelocationReservation(image, rva, byteCount);
}

[[nodiscard]] bool ValidateCellSite(const ElfSection& target, const std::uint64_t address,
                                    const std::uint32_t byteCount, const PeImageInfo& peImage,
                                    std::uint32_t& rva) noexcept
{
    if ((target.Type == ElfSectionNoBits) || !IsRangeInElfSection(target, address, byteCount) ||
        !TryAddressToRva(address, peImage.ImageBase, rva) || !IsValidPeRelocationTargetRange(peImage, rva, byteCount))
    {
        return false;
    }
    return true;
}

[[nodiscard]] bool ValidateInstructionSite(const ElfSection& target, const std::uint64_t address,
                                           const PeImageInfo& peImage, std::uint32_t& rva) noexcept
{
    // IA-64 relocation offsets retain the bundle slot in the low nibble.
    const std::uint64_t slot = address & 0x0FU;
    if ((slot > 2U) || ((target.Flags & ElfSectionExecute) == 0U) || (target.Type == ElfSectionNoBits))
    {
        return false;
    }
    const std::uint64_t bundleAddress = address - slot;
    std::uint32_t bundleRva{0U};
    if (!IsRangeInElfSection(target, bundleAddress, 16U) || !TryAddressToRva(address, peImage.ImageBase, rva) ||
        !TryAddressToRva(bundleAddress, peImage.ImageBase, bundleRva) ||
        !IsValidPeRelocationTargetRange(peImage, bundleRva, 16U))
    {
        return false;
    }
    return true;
}

[[nodiscard]] bool ValidateImm64InstructionSite(const ElfSection& target, const std::uint64_t address,
                                                const PeImageInfo& peImage, std::uint32_t& rva) noexcept
{
    return ((address & 0x0FU) == 1U) && ValidateInstructionSite(target, address, peImage, rva);
}

[[nodiscard]] bool ReadElfCell(std::FILE* const elfFile, const ElfImageInfo& elfImage, const ElfSection& section,
                               const std::uint64_t address, std::uint64_t& value) noexcept
{
    if ((section.Type == ElfSectionNoBits) || !IsRangeInElfSection(section, address, 8U))
    {
        return false;
    }
    const std::uint64_t sectionDelta = address - section.Address;
    std::uint64_t fileOffset{0U};
    if (!TryAdd(section.Offset, sectionDelta, fileOffset))
    {
        return false;
    }
    unsigned char bytes[8U]{};
    if (!ReadBytes(elfFile, elfImage.FileSize, fileOffset, bytes, sizeof(bytes)))
    {
        return false;
    }
    value = LoadUInt64(bytes);
    return true;
}

[[nodiscard]] bool IsTerminatedElfString(std::FILE* const elfFile, const ElfImageInfo& elfImage,
                                         const ElfSection& stringTable, const std::uint32_t stringOffset) noexcept
{
    if ((stringTable.Type != ElfSectionStringTable) || (stringOffset >= stringTable.Size))
    {
        return false;
    }
    unsigned char bytes[256U]{};
    std::uint64_t consumed{0U};
    const std::uint64_t available = stringTable.Size - stringOffset;
    while (consumed < available)
    {
        const std::uint64_t remaining = available - consumed;
        const std::size_t count = remaining < sizeof(bytes) ? static_cast<std::size_t>(remaining) : sizeof(bytes);
        std::uint64_t tablePosition{0U};
        std::uint64_t fileOffset{0U};
        if (!TryAdd(stringOffset, consumed, tablePosition) || !TryAdd(stringTable.Offset, tablePosition, fileOffset) ||
            !ReadBytes(elfFile, elfImage.FileSize, fileOffset, bytes, count))
        {
            return false;
        }
        for (std::size_t index{0U}; index < count; ++index)
        {
            if (bytes[index] == 0U)
            {
                return true;
            }
        }
        consumed += static_cast<std::uint64_t>(count);
    }
    return false;
}

[[nodiscard]] bool ValidateRelocationSymbol(std::FILE* const elfFile, const ElfImageInfo& elfImage,
                                            const ElfSection& symbolTable, const ElfSection& symbolNames,
                                            const std::uint32_t symbolIndex) noexcept
{
    const std::uint64_t symbolCount = symbolTable.Size / ElfSymbolBytes;
    if (symbolIndex >= symbolCount)
    {
        return false;
    }
    std::uint64_t symbolDelta{0U};
    std::uint64_t symbolOffset{0U};
    if (!TryMultiply(symbolIndex, ElfSymbolBytes, symbolDelta) ||
        !TryAdd(symbolTable.Offset, symbolDelta, symbolOffset))
    {
        return false;
    }
    unsigned char symbol[ElfSymbolBytes]{};
    if (!ReadBytes(elfFile, elfImage.FileSize, symbolOffset, symbol, sizeof(symbol)))
    {
        return false;
    }
    const std::uint32_t nameOffset = LoadUInt32(symbol);
    const std::uint16_t sectionIndex = LoadUInt16(symbol + 6U);
    if ((sectionIndex == ElfUndefinedIndex) || (sectionIndex == ElfCommonIndex) || (sectionIndex == ElfExtendedIndex) ||
        !IsTerminatedElfString(elfFile, elfImage, symbolNames, nameOffset))
    {
        return false;
    }
    return (sectionIndex == ElfAbsoluteIndex) || (sectionIndex < elfImage.SectionCount);
}

void AddSourceRelocation(std::vector<SourceRelocation>& relocations, const std::uint32_t rva, const std::uint32_t type)
{
    relocations.push_back(SourceRelocation{
        .Rva = rva,
        .Type = type,
    });
}

void AddBaseRelocation(std::vector<BaseRelocation>& relocations, const std::uint32_t rva, const std::uint16_t type)
{
    relocations.push_back(BaseRelocation{
        .Rva = rva,
        .Type = type,
    });
}

[[nodiscard]] bool TryGetInstructionSlotMask(const SourceRelocation& relocation, std::uint32_t& slotMask) noexcept
{
    switch (relocation.Type)
    {
    case Ia64RelocationImm64:
    case Ia64RelocationGpRel64I:
        slotMask = 0x06U;
        return true;

    case Ia64RelocationGpRel22:
    case Ia64RelocationPcRel21B:
    {
        const std::uint32_t slot = relocation.Rva & 0x0FU;
        if (slot > 2U)
        {
            return false;
        }
        slotMask = std::uint32_t{1U} << slot;
        return true;
    }

    default:
        slotMask = 0U;
        return true;
    }
}

[[nodiscard]] bool ProcessAllocatedRelocation(std::FILE* const elfFile, const ElfImageInfo& elfImage,
                                              const PeImageInfo& peImage, const ElfSection& target,
                                              const std::uint64_t address, const std::uint32_t type,
                                              std::vector<SourceRelocation>& sourceRelocations,
                                              std::vector<BaseRelocation>& baseRelocations)
{
    std::uint32_t rva{0U};
    switch (type)
    {
    case Ia64RelocationPcRel21B:
    case Ia64RelocationGpRel22:
        if (!ValidateInstructionSite(target, address, peImage, rva))
        {
            return ReportFailure("invalid IA-64 invariant instruction site");
        }
        AddSourceRelocation(sourceRelocations, rva, type);
        return true;

    case Ia64RelocationGpRel64I:
        if (!ValidateImm64InstructionSite(target, address, peImage, rva))
        {
            return ReportFailure("invalid IA-64 GPREL64I instruction site");
        }
        AddSourceRelocation(sourceRelocations, rva, type);
        return true;

    case Ia64RelocationDir64Lsb:
        if (!ValidateCellSite(target, address, 8U, peImage, rva))
        {
            return ReportFailure("invalid IA-64 DIR64 cell");
        }
        AddSourceRelocation(sourceRelocations, rva, type);
        AddBaseRelocation(baseRelocations, rva, PeRelocationDir64);
        return true;

    case Ia64RelocationIpltLsb:
    {
        // The inline descriptor site P retains its ABI-required alignment.
        if (((address & 0x07U) != 0U) || !ValidateCellSite(target, address, 16U, peImage, rva) ||
            (rva > (std::numeric_limits<std::uint32_t>::max() - 8U)))
        {
            return ReportFailure("invalid IA-64 inline IPLT descriptor");
        }
        AddSourceRelocation(sourceRelocations, rva, type);
        AddBaseRelocation(baseRelocations, rva, PeRelocationDir64);
        AddBaseRelocation(baseRelocations, rva + 8U, PeRelocationDir64);
        return true;
    }

    case Ia64RelocationImm64:
        if (!ValidateImm64InstructionSite(target, address, peImage, rva))
        {
            return ReportFailure("invalid IA-64 IMM64 instruction site");
        }
        AddSourceRelocation(sourceRelocations, rva, type);
        AddBaseRelocation(baseRelocations, rva, PeRelocationIa64Imm64);
        return true;

    case Ia64RelocationFptr64Lsb:
    {
        std::uint64_t descriptorAddress{0U};
        if (!ValidateCellSite(target, address, 8U, peImage, rva) ||
            !ReadElfCell(elfFile, elfImage, target, address, descriptorAddress) || !elfImage.HasOpdSection ||
            ((descriptorAddress & 0x07U) != 0U) || !IsRangeInElfSection(elfImage.OpdSection, descriptorAddress, 16U))
        {
            return ReportFailure("invalid IA-64 function descriptor reference");
        }
        std::uint32_t descriptorRva{0U};
        if (!TryAddressToRva(descriptorAddress, peImage.ImageBase, descriptorRva) ||
            !IsValidPeRelocationTargetRange(peImage, descriptorRva, 16U) ||
            (descriptorRva > (std::numeric_limits<std::uint32_t>::max() - 8U)))
        {
            return ReportFailure("IA-64 function descriptor is outside the PE image");
        }
        AddSourceRelocation(sourceRelocations, rva, type);
        AddBaseRelocation(baseRelocations, rva, PeRelocationDir64);
        AddBaseRelocation(baseRelocations, descriptorRva, PeRelocationDir64);
        AddBaseRelocation(baseRelocations, descriptorRva + 8U, PeRelocationDir64);
        return true;
    }

    default:
        return ReportUnsupportedRelocation(type);
    }
}

[[nodiscard]] bool CollectElfRelocations(std::FILE* const elfFile, const ElfImageInfo& elfImage,
                                         const PeImageInfo& peImage, std::vector<BaseRelocation>& baseRelocations)
{
    std::vector<SourceRelocation> sourceRelocations;
    for (std::uint64_t sectionIndex{0U}; sectionIndex < elfImage.SectionCount; ++sectionIndex)
    {
        ElfSection relocationSection{};
        if (!ReadElfSection(elfFile, elfImage, sectionIndex, relocationSection))
        {
            return ReportFailure("cannot read ELF relocation section");
        }
        if ((relocationSection.Type != ElfSectionRela) && (relocationSection.Type != ElfSectionRel))
        {
            continue;
        }
        if (relocationSection.Info >= elfImage.SectionCount)
        {
            return ReportFailure("ELF relocation target index is invalid");
        }
        ElfSection target{};
        if (!ReadElfSection(elfFile, elfImage, relocationSection.Info, target))
        {
            return ReportFailure("cannot read ELF relocation target");
        }
        if ((target.Flags & ElfSectionAlloc) == 0U)
        {
            continue;
        }
        if ((relocationSection.Type != ElfSectionRela) || (relocationSection.EntrySize != ElfRelaBytes) ||
            ((relocationSection.Size % ElfRelaBytes) != 0U) || (relocationSection.Link >= elfImage.SectionCount))
        {
            return ReportFailure("invalid allocated-section ELF relocation table");
        }

        ElfSection symbolTable{};
        if (!ReadElfSection(elfFile, elfImage, relocationSection.Link, symbolTable) ||
            (symbolTable.Type != ElfSectionSymbolTable) || (symbolTable.EntrySize != ElfSymbolBytes) ||
            ((symbolTable.Size % ElfSymbolBytes) != 0U) || (symbolTable.Info > (symbolTable.Size / ElfSymbolBytes)) ||
            (symbolTable.Link >= elfImage.SectionCount))
        {
            return ReportFailure("invalid static ELF symbol table for relocation");
        }
        ElfSection symbolNames{};
        if (!ReadElfSection(elfFile, elfImage, symbolTable.Link, symbolNames) ||
            (symbolNames.Type != ElfSectionStringTable))
        {
            return ReportFailure("invalid ELF symbol-name table");
        }

        const std::uint64_t relocationCount = relocationSection.Size / ElfRelaBytes;
        for (std::uint64_t index{0U}; index < relocationCount; ++index)
        {
            std::uint64_t entryDelta{0U};
            std::uint64_t entryOffset{0U};
            if (!TryMultiply(index, ElfRelaBytes, entryDelta) ||
                !TryAdd(relocationSection.Offset, entryDelta, entryOffset))
            {
                return ReportFailure("overflow in ELF relocation offset");
            }
            unsigned char relocation[ElfRelaBytes]{};
            if (!ReadBytes(elfFile, elfImage.FileSize, entryOffset, relocation, sizeof(relocation)))
            {
                return ReportFailure("cannot read ELF relocation entry");
            }
            const std::uint64_t address = LoadUInt64(relocation);
            const std::uint64_t relocationInfo = LoadUInt64(relocation + 8U);
            const std::uint32_t symbolIndex = static_cast<std::uint32_t>(relocationInfo >> 32U);
            const std::uint32_t type = static_cast<std::uint32_t>(relocationInfo & 0xFFFF'FFFFU);
            if (!ValidateRelocationSymbol(elfFile, elfImage, symbolTable, symbolNames, symbolIndex))
            {
                return ReportFailure("invalid or unresolved ELF relocation symbol");
            }
            if (!ProcessAllocatedRelocation(elfFile, elfImage, peImage, target, address, type, sourceRelocations,
                                            baseRelocations))
            {
                return false;
            }
        }
    }

    std::sort(sourceRelocations.begin(), sourceRelocations.end(),
              [](const SourceRelocation& left, const SourceRelocation& right)
              {
                  if (left.Rva != right.Rva)
                  {
                      return left.Rva < right.Rva;
                  }
                  return left.Type < right.Type;
              });
    for (std::size_t index{1U}; index < sourceRelocations.size(); ++index)
    {
        if ((sourceRelocations[index].Rva == sourceRelocations[index - 1U].Rva) &&
            (sourceRelocations[index].Type != sourceRelocations[index - 1U].Type))
        {
            return ReportFailure("contradictory IA-64 relocations at one RVA");
        }
    }

    const auto uniqueEnd = std::unique(sourceRelocations.begin(), sourceRelocations.end(),
                                       [](const SourceRelocation& left, const SourceRelocation& right)
                                       {
                                           return (left.Rva == right.Rva) && (left.Type == right.Type);
                                       });
    sourceRelocations.erase(uniqueEnd, sourceRelocations.end());

    for (std::size_t firstIndex{0U}; firstIndex < sourceRelocations.size(); ++firstIndex)
    {
        const SourceRelocation& first = sourceRelocations[firstIndex];
        std::uint32_t firstSlotMask{0U};
        if (!TryGetInstructionSlotMask(first, firstSlotMask))
        {
            return ReportFailure("invalid IA-64 instruction relocation slot");
        }
        if (firstSlotMask == 0U)
        {
            continue;
        }

        const std::uint32_t bundleRva = first.Rva & ~0x0FU;
        for (std::size_t secondIndex{firstIndex + 1U}; secondIndex < sourceRelocations.size(); ++secondIndex)
        {
            const SourceRelocation& second = sourceRelocations[secondIndex];
            if ((second.Rva & ~0x0FU) != bundleRva)
            {
                break;
            }
            std::uint32_t secondSlotMask{0U};
            if (!TryGetInstructionSlotMask(second, secondSlotMask))
            {
                return ReportFailure("invalid IA-64 instruction relocation slot");
            }
            if ((firstSlotMask & secondSlotMask) != 0U)
            {
                return ReportFailure("overlapping IA-64 instruction relocation footprints");
            }
        }
    }
    return true;
}

[[nodiscard]] bool PrepareBaseRelocations(std::vector<BaseRelocation>& relocations, const PeImageInfo& peImage,
                                          std::vector<unsigned char>& encoded)
{
    std::sort(relocations.begin(), relocations.end(),
              [](const BaseRelocation& left, const BaseRelocation& right)
              {
                  if (left.Rva != right.Rva)
                  {
                      return left.Rva < right.Rva;
                  }
                  return left.Type < right.Type;
              });
    for (std::size_t index{1U}; index < relocations.size(); ++index)
    {
        if ((relocations[index].Rva == relocations[index - 1U].Rva) &&
            (relocations[index].Type != relocations[index - 1U].Type))
        {
            return ReportFailure("contradictory PE relocation types at one RVA");
        }
    }
    const auto uniqueEnd = std::unique(relocations.begin(), relocations.end(),
                                       [](const BaseRelocation& left, const BaseRelocation& right)
                                       {
                                           return (left.Rva == right.Rva) && (left.Type == right.Type);
                                       });
    relocations.erase(uniqueEnd, relocations.end());

    std::uint32_t previousImm64Bundle{0U};
    bool hasPreviousImm64Bundle{false};
    for (const BaseRelocation& relocation : relocations)
    {
        if (relocation.Type != PeRelocationIa64Imm64)
        {
            continue;
        }
        const std::uint32_t bundle = relocation.Rva & ~0x0FU;
        if (hasPreviousImm64Bundle && (bundle == previousImm64Bundle))
        {
            return ReportFailure("duplicate IA-64 IMM64 relocation bundle");
        }
        previousImm64Bundle = bundle;
        hasPreviousImm64Bundle = true;
    }

    std::uint64_t encodedSize{0U};
    std::size_t index{0U};
    while (index < relocations.size())
    {
        const std::uint32_t page = relocations[index].Rva & ~(RelocationPageBytes - 1U);
        std::size_t endIndex = index;
        while ((endIndex < relocations.size()) && ((relocations[endIndex].Rva & ~(RelocationPageBytes - 1U)) == page))
        {
            ++endIndex;
        }
        const std::uint64_t entryCount = static_cast<std::uint64_t>(endIndex - index);
        std::uint64_t entriesBytes{0U};
        std::uint64_t blockBytes{0U};
        if (!TryMultiply(entryCount, 2U, entriesBytes) || !TryAdd(8U, entriesBytes, blockBytes))
        {
            return ReportFailure("overflow while sizing PE relocations");
        }
        if ((blockBytes & 3U) != 0U)
        {
            if (!TryAdd(blockBytes, 2U, blockBytes))
            {
                return ReportFailure("overflow while padding PE relocations");
            }
        }
        if (!TryAdd(encodedSize, blockBytes, encodedSize))
        {
            return ReportFailure("overflow in PE relocation-directory size");
        }
        index = endIndex;
    }

    const std::uint64_t relocationCapacity = peImage.RelocationSection.VirtualSize < peImage.RelocationSection.RawSize
                                                 ? peImage.RelocationSection.VirtualSize
                                                 : peImage.RelocationSection.RawSize;
    if ((encodedSize > relocationCapacity) || (encodedSize > std::numeric_limits<std::uint32_t>::max()) ||
        (encodedSize > std::numeric_limits<std::size_t>::max()))
    {
        return ReportFailure("PE .reloc reservation is too small");
    }

    encoded.clear();
    encoded.reserve(static_cast<std::size_t>(encodedSize));
    index = 0U;
    while (index < relocations.size())
    {
        const std::uint32_t page = relocations[index].Rva & ~(RelocationPageBytes - 1U);
        std::size_t endIndex = index;
        while ((endIndex < relocations.size()) && ((relocations[endIndex].Rva & ~(RelocationPageBytes - 1U)) == page))
        {
            ++endIndex;
        }
        const std::size_t entryCount = endIndex - index;
        const std::size_t blockSize = 8U + (entryCount * 2U) + ((entryCount & 1U) != 0U ? 2U : 0U);
        const std::uint32_t blockBytes = static_cast<std::uint32_t>(blockSize);
        unsigned char header[8U]{};
        StoreUInt32(header, page);
        StoreUInt32(header + 4U, blockBytes);
        encoded.insert(encoded.end(), header, header + sizeof(header));
        for (std::size_t entryIndex{index}; entryIndex < endIndex; ++entryIndex)
        {
            const std::uint16_t offset =
                static_cast<std::uint16_t>(relocations[entryIndex].Rva & (RelocationPageBytes - 1U));
            const std::uint16_t value =
                static_cast<std::uint16_t>(static_cast<std::uint16_t>(relocations[entryIndex].Type << 12U) | offset);
            unsigned char entry[2U]{};
            StoreUInt16(entry, value);
            encoded.insert(encoded.end(), entry, entry + sizeof(entry));
        }
        if ((entryCount & 1U) != 0U)
        {
            unsigned char padding[2U]{};
            StoreUInt16(padding, PeRelocationAbsolute);
            encoded.insert(encoded.end(), padding, padding + sizeof(padding));
        }
        index = endIndex;
    }
    return encoded.size() == static_cast<std::size_t>(encodedSize);
}

[[nodiscard]] bool WriteZeroBytes(std::FILE* const file, const std::uint64_t fileSize, const std::uint64_t offset,
                                  const std::uint64_t byteCount) noexcept
{
    unsigned char zeros[4096U]{};
    std::uint64_t written{0U};
    while (written < byteCount)
    {
        const std::uint64_t remaining = byteCount - written;
        const std::size_t chunk = remaining < sizeof(zeros) ? static_cast<std::size_t>(remaining) : sizeof(zeros);
        std::uint64_t writeOffset{0U};
        if (!TryAdd(offset, written, writeOffset) || !WriteBytes(file, fileSize, writeOffset, zeros, chunk))
        {
            return false;
        }
        written += static_cast<std::uint64_t>(chunk);
    }
    return true;
}

[[nodiscard]] bool WriteFinalizedPeImage(std::FILE* const file, const PeImageInfo& image,
                                         const std::vector<unsigned char>& relocations) noexcept
{
    const std::uint64_t rawOffset = image.RelocationSection.RawOffset;
    if (!WriteZeroBytes(file, image.FileSize, rawOffset, image.RelocationSection.RawSize) ||
        !WriteBytes(file, image.FileSize, rawOffset, relocations.data(), relocations.size()))
    {
        return ReportFailure("cannot write reserved PE .reloc section");
    }

    unsigned char characteristics[2U]{};
    StoreUInt16(characteristics, CanonicalCharacteristics);
    unsigned char checksum[4U]{};
    StoreUInt32(checksum, 0U);
    unsigned char dllCharacteristics[2U]{};
    StoreUInt16(dllCharacteristics, CanonicalDllCharacteristics);
    unsigned char directory[8U]{};
    StoreUInt32(directory, image.RelocationSection.VirtualAddress);
    StoreUInt32(directory + 4U, static_cast<std::uint32_t>(relocations.size()));

    std::uint64_t characteristicsOffset{0U};
    std::uint64_t checksumOffset{0U};
    std::uint64_t dllCharacteristicsOffset{0U};
    if (!TryAdd(image.PeOffset, 22U, characteristicsOffset) ||
        !TryAdd(image.OptionalHeaderOffset, 64U, checksumOffset) ||
        !TryAdd(image.OptionalHeaderOffset, 70U, dllCharacteristicsOffset) ||
        !WriteBytes(file, image.FileSize, characteristicsOffset, characteristics, sizeof(characteristics)) ||
        !WriteBytes(file, image.FileSize, checksumOffset, checksum, sizeof(checksum)) ||
        !WriteBytes(file, image.FileSize, dllCharacteristicsOffset, dllCharacteristics, sizeof(dllCharacteristics)) ||
        !WriteBytes(file, image.FileSize, image.RelocationDirectoryOffset, directory, sizeof(directory)) ||
        (std::fflush(file) != 0))
    {
        return ReportFailure("cannot patch normalized PE headers");
    }
    return true;
}

[[nodiscard]] bool FinalizeIa64Image(std::FILE* const elfFile, std::FILE* const peFile)
{
    ElfImageInfo elfImage{};
    PeImageInfo peImage{};
    if (!ParseElfImage(elfFile, elfImage) || !ParsePeImage(peFile, peImage))
    {
        return false;
    }

    std::vector<BaseRelocation> baseRelocations;
    std::vector<unsigned char> encodedRelocations;
    if (!CollectElfRelocations(elfFile, elfImage, peImage, baseRelocations) ||
        !PrepareBaseRelocations(baseRelocations, peImage, encodedRelocations))
    {
        return false;
    }
    return WriteFinalizedPeImage(peFile, peImage, encodedRelocations);
}
} // namespace

int Run(const int argumentCount, char** const arguments)
{
    if ((argumentCount != 4) || !StringEquals(arguments[1U], "--normalize-ia64"))
    {
        static_cast<void>(std::fprintf(stderr, "Usage: %s --normalize-ia64 <linked-elf> <pe-image>\n", arguments[0U]));
        return 2;
    }

    std::FILE* const elfFile = std::fopen(arguments[2U], "rb");
    if (elfFile == nullptr)
    {
        static_cast<void>(std::fprintf(stderr, "Cannot open linked ELF %s\n", arguments[2U]));
        return 1;
    }
    std::FILE* const peFile = std::fopen(arguments[3U], "r+b");
    if (peFile == nullptr)
    {
        static_cast<void>(std::fprintf(stderr, "Cannot open PE image %s\n", arguments[3U]));
        static_cast<void>(std::fclose(elfFile));
        return 1;
    }

    const bool finalized = FinalizeIa64Image(elfFile, peFile);
    const int peCloseResult = std::fclose(peFile);
    const int elfCloseResult = std::fclose(elfFile);
    if (!finalized || (peCloseResult != 0) || (elfCloseResult != 0))
    {
        if (finalized)
        {
            static_cast<void>(std::fprintf(stderr, "NormalizeIa64Pe: cannot close input or output file\n"));
        }
        return 1;
    }
    return 0;
}
} // namespace Asteria::Tools::Firmware

int main(const int argumentCount, char** const arguments)
{
    return Asteria::Tools::Firmware::Run(argumentCount, arguments);
}
