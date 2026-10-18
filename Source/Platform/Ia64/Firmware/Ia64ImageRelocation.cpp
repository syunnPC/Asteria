// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Firmware/Ia64ImageRelocation.hpp"

namespace Asteria::Platform::Ia64::Firmware
{
namespace
{
using RelocationResult = Foundation::Result<Ia64ImageRelocationResult, Ia64ImageRelocationError>;
constexpr Foundation::UInt64 SlotMask{(Foundation::UInt64{1U} << 41U) - 1U};

[[nodiscard]] Foundation::UInt64 Read(const Foundation::UInt8* bytes, const Foundation::Size count) noexcept
{
    Foundation::UInt64 value{0U};
    for (Foundation::Size index{0U}; index < count; ++index)
    {
        value |= static_cast<Foundation::UInt64>(bytes[index]) << (index * 8U);
    }
    return value;
}

void Write(Foundation::UInt8* bytes, Foundation::UInt64 value) noexcept
{
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value & 0xFFU);
        value >>= 8U;
    }
}

void FixImmediate(Foundation::UInt8* bundle, const Foundation::UInt64 delta) noexcept
{
    const Foundation::UInt64 low = Read(bundle, 8U);
    const Foundation::UInt64 high = Read(bundle + 8U, 8U);
    Foundation::UInt64 slot1 = ((low >> 46U) | (high << 18U)) & SlotMask;
    Foundation::UInt64 slot2 = high >> 23U;
    const Foundation::UInt64 value =
        (((slot2 >> 13U) & 0x7FU) | (((slot2 >> 27U) & 0x1FFU) << 7U) | (((slot2 >> 22U) & 0x1FU) << 16U) |
         (((slot2 >> 21U) & 1U) << 21U) | (slot1 << 22U) | (((slot2 >> 36U) & 1U) << 63U)) +
        delta;
    slot1 = (value >> 22U) & SlotMask;
    slot2 &=
        ~((Foundation::UInt64{1U} << 36U) | (Foundation::UInt64{0x1FFU} << 27U) | (Foundation::UInt64{0x1FU} << 22U) |
          (Foundation::UInt64{1U} << 21U) | (Foundation::UInt64{0x7FU} << 13U));
    slot2 |= (((value >> 63U) & 1U) << 36U) | (((value >> 7U) & 0x1FFU) << 27U) | (((value >> 16U) & 0x1FU) << 22U) |
             (((value >> 21U) & 1U) << 21U) | ((value & 0x7FU) << 13U);
    Write(bundle, (low & ((Foundation::UInt64{1U} << 46U) - 1U)) | (slot1 << 46U));
    Write(bundle + 8U, (slot1 >> 18U) | (slot2 << 23U));
}

/// Two passes over the immutable source: validate every footprint before any
/// copy, then apply. Ordered nonoverlapping sites exclude double application
/// and a fixup that could rewrite the directory being consumed.
[[nodiscard]] bool ProcessRelocations(const Foundation::UInt8* source, const Foundation::Size imageBytes,
                                      const Foundation::Size directory, const Foundation::Size directoryBytes,
                                      const Ia64LoadedImageLayout& layout, const Foundation::UInt32 descriptorRva,
                                      Foundation::UInt8* destination, const Foundation::UInt64 delta) noexcept
{
    Foundation::Size cursor{directory};
    Foundation::Size previousEnd{0U};
    Foundation::UInt64 previousPage{0U};
    const Foundation::Size end = directory + directoryBytes;
    bool entryCodeFixed{false};
    bool entryGpFixed{false};
    while (cursor < end)
    {
        if ((end - cursor) < 8U)
        {
            return false;
        }
        const Foundation::UInt64 page = Read(source + cursor, 4U);
        const Foundation::Size blockBytes = static_cast<Foundation::Size>(Read(source + cursor + 4U, 4U));
        if ((page == 0U) || ((page & 0xFFFU) != 0U) || (page <= previousPage) || (blockBytes < 8U) ||
            ((blockBytes & 3U) != 0U) || (blockBytes > (end - cursor)))
        {
            return false;
        }
        previousPage = page;
        for (Foundation::Size entry{cursor + 8U}; entry < (cursor + blockBytes); entry += 2U)
        {
            const Foundation::UInt64 encoded = Read(source + entry, 2U);
            const Foundation::UInt64 kind = encoded >> 12U;
            if (kind == 0U)
            {
                if ((encoded != 0U) || ((entry + 2U) != (cursor + blockBytes)))
                {
                    return false;
                }
                continue;
            }
            Foundation::UInt64 rva = page + (encoded & 0xFFFU);
            Foundation::Size footprint{8U};
            if (kind == 9U)
            {
                if ((rva & 0xFU) != 1U)
                {
                    return false;
                }
                rva &= ~Foundation::UInt64{0xFU};
                footprint = 16U;
            }
            else if ((kind != 10U) || ((rva & 7U) != 0U))
            {
                return false;
            }
            if ((rva < previousEnd) || (rva > imageBytes) || (footprint > (imageBytes - rva)) ||
                ((rva < end) && (directory < (rva + footprint))))
            {
                return false;
            }
            bool contained{false};
            for (Foundation::Size sectionIndex{0U}; sectionIndex < Ia64LoadedImageSectionCount; ++sectionIndex)
            {
                const Ia64LoadedImageSection& section = layout.Sections[sectionIndex];
                const Foundation::UInt64 start = section.VirtualStart - layout.ImageBase;
                const Foundation::UInt64 limit = start + section.PageCount * 4096U;
                const bool code = section.Purpose == Ia64LoadedImageSectionPurpose::Code;
                contained |= (rva >= start) && ((rva + footprint) <= limit) && ((kind == 9U) == code);
            }
            if (!contained)
            {
                return false;
            }
            entryCodeFixed |= (kind == 10U) && (rva == descriptorRva);
            entryGpFixed |= (kind == 10U) && (rva == (static_cast<Foundation::UInt64>(descriptorRva) + 8U));
            previousEnd = static_cast<Foundation::Size>(rva) + footprint;
            if ((kind == 9U) && (((source[rva] & 0x1EU) != 4U) || ((Read(source + rva + 8U, 8U) >> 60U) != 6U)))
            {
                return false;
            }
            if (destination != nullptr)
            {
                if (kind == 9U)
                {
                    FixImmediate(destination + rva, delta);
                }
                else
                {
                    Write(destination + rva, Read(source + rva, 8U) + delta);
                }
            }
        }
        cursor += blockBytes;
    }
    return (cursor == end) && entryCodeFixed && entryGpFixed;
}
} // namespace

Foundation::Result<Ia64ImageRelocationResult, Ia64ImageRelocationError>
Ia64ImageRelocation::RelocateToAlignment(const void* const image, const Foundation::Size imageBytes,
                                         void* const destination, const Foundation::Size destinationBytes,
                                         Foundation::UInt32 descriptorRva) noexcept
{
    const auto parsed = Ia64LoadedImageLayoutParser::Parse(image, imageBytes);
    if (!parsed.HasValue())
    {
        return RelocationResult::Failure(Ia64ImageRelocationError::InvalidImage);
    }
    const Foundation::UInt64 base = parsed.GetValue().ImageBase;
    const Foundation::UInt64 target = reinterpret_cast<Foundation::UIntPointer>(destination);
    if ((destination == nullptr) || (destinationBytes < imageBytes) ||
        ((target & (Ia64RelocatedImageAlignmentBytes - 1U)) != 0U) ||
        (target > (~Foundation::UInt64{0U} - imageBytes)) ||
        ((target < (base + imageBytes)) && (base < (target + imageBytes))))
    {
        return RelocationResult::Failure(Ia64ImageRelocationError::InvalidDestination);
    }
    const auto* const source = static_cast<const Foundation::UInt8*>(image);
    const Foundation::Size optional = static_cast<Foundation::Size>(Read(source + 60U, 4U)) + 24U;
    if (descriptorRva == 0U)
    {
        descriptorRva = static_cast<Foundation::UInt32>(Read(source + optional + 16U, 4U));
    }
    const Ia64LoadedImageSection& descriptors = parsed.GetValue().Sections[2U];
    if ((descriptorRva & 7U) != 0U || (descriptorRva < (descriptors.VirtualStart - base)) ||
        (descriptorRva > (descriptors.VirtualStart - base + descriptors.PageCount * 4096U - 16U)))
    {
        return RelocationResult::Failure(Ia64ImageRelocationError::InvalidEntryDescriptor);
    }
    const Foundation::UInt64 entryCode = Read(source + descriptorRva, 8U);
    const Foundation::UInt64 entryGp = Read(source + descriptorRva + 8U, 8U);
    const Ia64LoadedImageSection& code = parsed.GetValue().Sections[0U];
    if (((entryCode & 15U) != 0U) || (entryCode < code.VirtualStart) ||
        (entryCode >= (code.VirtualStart + code.PageCount * 4096U)) || ((entryGp & 7U) != 0U) || (entryGp < base) ||
        (entryGp >= (base + imageBytes)))
    {
        return RelocationResult::Failure(Ia64ImageRelocationError::InvalidEntryDescriptor);
    }
    const Foundation::Size directory = static_cast<Foundation::Size>(Read(source + optional + 152U, 4U));
    const Foundation::Size directoryBytes = static_cast<Foundation::Size>(Read(source + optional + 156U, 4U));
    const Ia64LoadedImageSection& relocations = parsed.GetValue().Sections[3U];
    if ((directoryBytes == 0U) || (directory != (relocations.VirtualStart - base)) ||
        (directoryBytes > relocations.PageCount * 4096U) ||
        !ProcessRelocations(source, imageBytes, directory, directoryBytes, parsed.GetValue(), descriptorRva, nullptr,
                            0U))
    {
        return RelocationResult::Failure(Ia64ImageRelocationError::InvalidRelocations);
    }
    auto* const output = static_cast<Foundation::UInt8*>(destination);
    for (Foundation::Size index{0U}; index < imageBytes; ++index)
    {
        output[index] = source[index];
    }
    // Unsigned subtraction is intentional: PE fixups are modulo 2^64.
    static_cast<void>(ProcessRelocations(source, imageBytes, directory, directoryBytes, parsed.GetValue(),
                                         descriptorRva, output, target - base));
    return RelocationResult::Success(Ia64ImageRelocationResult{target + descriptorRva});
}

#if !defined(ASTERIA_TARGET_IA64) || !defined(__ia64__)
void MakeIa64ImageInstructionCoherent(void*, Foundation::Size) noexcept
{
}
#else
extern "C" void AsteriaIa64MakeImageInstructionCoherent(void*, Foundation::Size) noexcept;
void MakeIa64ImageInstructionCoherent(void* const image, const Foundation::Size bytes) noexcept
{
    AsteriaIa64MakeImageInstructionCoherent(image, bytes);
}
#endif
} // namespace Asteria::Platform::Ia64::Firmware
