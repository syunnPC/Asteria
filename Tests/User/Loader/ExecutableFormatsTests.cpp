// SPDX-License-Identifier: Apache-2.0
#include "User/Loader/ExecutableFormats.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt64;
using Foundation::UInt8;
using User::Loader::DescribeExecutable;
using User::Loader::ExecutableAbi;
using User::Loader::ExecutableByteOrder;
using User::Loader::ExecutableFormat;
using User::Loader::ExecutableIdentity;
using User::Loader::ExecutableImage;
using User::Loader::ExecutableKind;
using User::Loader::ExecutableMachine;
using User::Loader::IdentifyExecutable;
using User::Loader::LoaderError;
using User::Loader::MachineRules;

// AMD64's rules: the canonical lower half above page zero, a code entry.
constexpr MachineRules Rules{.Machine = ExecutableMachine::Amd64,
                             .UserStart = 0x1000U,
                             .UserLimit = 0x0000'8000'0000'0000ULL,
                             .EntryDescriptor = false,
                             .MaximumSegments = 3U,
                             .MaximumSegmentPages = 16U,
                             .WritableFileBytes = false};
// IA-64's: region 1, an entry descriptor.
constexpr MachineRules DescriptorRules{.Machine = ExecutableMachine::Ia64,
                                       .UserStart = 0x2000'0000'0000'1000ULL,
                                       .UserLimit = 0x2000'0000'8000'0000ULL,
                                       .EntryDescriptor = true,
                                       .MaximumSegments = 3U,
                                       .MaximumSegmentPages = 16U,
                                       .WritableFileBytes = false};
constexpr UInt64 TextStart{0x2000'0000'0100'0000ULL};
constexpr UInt64 DataStart{0x2000'0000'0101'0000ULL};

void Store(UInt8* const bytes, const UInt64 value, const Size width) noexcept
{
    for (Size index{0U}; index < width; ++index)
    {
        bytes[index] = static_cast<UInt8>(value >> (index * 8U));
    }
}

// A two-segment AMD64 image like the linker script makes: text (R+X) at
// 0x40000000 from file offset 0x1000, read-only data at 0x40002000 from
// 0x3000.
struct Image
{
    UInt8 Bytes[0x3100U]{};

    Image() noexcept
    {
        Bytes[0U] = 0x7FU;
        Bytes[1U] = 'E';
        Bytes[2U] = 'L';
        Bytes[3U] = 'F';
        Bytes[4U] = 2U;
        Bytes[5U] = 1U;
        Bytes[6U] = 1U;
        Store(Bytes + 16U, 2U, 2U);
        Store(Bytes + 18U, 62U, 2U);
        Store(Bytes + 20U, 1U, 4U);
        Store(Bytes + 24U, 0x4000'0010U, 8U);
        Store(Bytes + 32U, 64U, 8U);
        Store(Bytes + 54U, 56U, 2U);
        Store(Bytes + 56U, 3U, 2U);
        Segment(0U, 1U, 5U, 0x1000U, 0x4000'0000U, 0x1100U, 0x1100U);
        Segment(1U, 1U, 4U, 0x3000U, 0x4000'2000U, 0x58U, 0x58U);
        Segment(2U, 0x6474'E551U, 6U, 0U, 0U, 0U, 0U);
    }

    void Segment(const Size index, const UInt64 type, const UInt64 flags, const UInt64 offset, const UInt64 address,
                 const UInt64 fileBytes, const UInt64 memoryBytes) noexcept
    {
        UInt8* const header = Bytes + 64U + (index * 56U);
        Store(header, type, 4U);
        Store(header + 4U, flags, 4U);
        Store(header + 8U, offset, 8U);
        Store(header + 16U, address, 8U);
        Store(header + 32U, fileBytes, 8U);
        Store(header + 40U, memoryBytes, 8U);
    }
};

// An IA-64 image of the LP64 ABI: text at TextStart, read-only data at
// DataStart whose file bytes hold the entry descriptor {ip, gp} the ELF
// entry names.
struct DescriptorImage : Image
{
    DescriptorImage() noexcept
    {
        Store(Bytes + 18U, 50U, 2U);
        Store(Bytes + 48U, 0x10U, 4U);
        Store(Bytes + 24U, DataStart + 0x10U, 8U);
        Segment(0U, 1U, 5U, 0x1000U, TextStart, 0x1100U, 0x1100U);
        Segment(1U, 1U, 4U, 0x3000U, DataStart, 0x58U, 0x58U);
        Store(Bytes + 0x3010U, TextStart + 0x40U, 8U);
        Store(Bytes + 0x3018U, DataStart + 0x800U, 8U);
    }
};

// A PE image: the MZ stub naming the PE header at 0x80, a COFF header for
// machine and an optional header of magic and subsystem.
struct PeImage
{
    UInt8 Bytes[0x200U]{};

    explicit PeImage(const UInt64 machine, const UInt64 magic = 0x20BU) noexcept
    {
        Bytes[0U] = 'M';
        Bytes[1U] = 'Z';
        Store(Bytes + 0x3CU, 0x80U, 4U);
        Bytes[0x80U] = 'P';
        Bytes[0x81U] = 'E';
        Store(Bytes + 0x84U, machine, 2U);
        Store(Bytes + 0x84U + 16U, 0xF0U, 2U);
        Store(Bytes + 0x98U, magic, 2U);
        Store(Bytes + 0x98U + 68U, 3U, 2U);
    }
};

[[nodiscard]] LoaderError Describe(const UInt8* const bytes, const Size size, const MachineRules& rules,
                                   ExecutableImage& image) noexcept
{
    return DescribeExecutable(bytes, size, rules, image);
}

void TestLinkedLayout(TestContext& runner) noexcept
{
    const Image image;
    ExecutableImage layout{};
    const LoaderError error = Describe(image.Bytes, sizeof(image.Bytes), Rules, layout);
    ASTERIA_EXPECT(
        runner, "a linked component's PT_LOAD segments become page-granular loads", error == LoaderError::None,
        layout.Identity.Format == ExecutableFormat::Elf64, layout.Identity.Machine == ExecutableMachine::Amd64,
        layout.Identity.Abi == ExecutableAbi::Asteria, layout.Identity.Kind == ExecutableKind::Fixed,
        layout.Entry == 0x4000'0010U, layout.SegmentCount == 2U, layout.Segments[0U].VirtualStart == 0x4000'0000U,
        layout.Segments[0U].Pages == 2U, layout.Segments[0U].FileOffset == 0x1000U,
        layout.Segments[0U].FileBytes == 0x1100U, layout.Segments[0U].Executable, !layout.Segments[0U].Writable,
        layout.Segments[1U].VirtualStart == 0x4000'2000U, layout.Segments[1U].Pages == 1U,
        !layout.Segments[1U].Executable, layout.Segments[0U].PageOffset == 0U);
}

void TestRejections(TestContext& runner) noexcept
{
    ExecutableImage layout{};
    const auto refuses =
        [&](const Image& image, const LoaderError expected, const Size size = sizeof(Image::Bytes)) noexcept
    {
        return (Describe(image.Bytes, size, Rules, layout) == expected) && (layout.SegmentCount == 0U);
    };
    Image notElf;
    notElf.Bytes[1U] = 'X';
    Image beyondFile;
    beyondFile.Segment(1U, 1U, 4U, 0x3000U, 0x4000'2000U, 0x200U, 0x200U);
    Image unreadable;
    unreadable.Segment(1U, 1U, 0U, 0x3000U, 0x4000'2000U, 0x58U, 0x58U);
    Image overlapping;
    overlapping.Segment(1U, 1U, 4U, 0x3000U, 0x4000'1000U, 0x58U, 0x58U);
    Image tooLarge;
    tooLarge.Segment(1U, 1U, 4U, 0x3000U, 0x4000'2000U, 0x58U, 17U * 4096U);
    Image kernelHalf;
    kernelHalf.Segment(1U, 1U, 4U, 0x3000U, 0xFFFF'8000'0000'0000ULL, 0x58U, 0x58U);
    Image misaligned;
    misaligned.Segment(1U, 1U, 4U, 0x3008U, 0x4000'2000U, 0x58U, 0x58U);
    // A fourth PT_LOAD is one more than a component may have.
    Image fourth;
    Store(fourth.Bytes + 56U, 4U, 2U);
    fourth.Segment(2U, 1U, 6U, 0x3100U, 0x4000'4000U, 0U, 0x100U);
    fourth.Segment(3U, 1U, 4U, 0x3000U, 0x4000'6000U, 0x58U, 0x58U);
    Image entryInData;
    Store(entryInData.Bytes + 24U, 0x4000'2010U, 8U);
    Image zeroPage;
    zeroPage.Segment(1U, 1U, 4U, 0x0000U, 0x0U, 0x58U, 0x58U);
    ASTERIA_EXPECT(runner, "an image the root service cannot load is refused, says why and describes no segment",
                   refuses(notElf, LoaderError::UnsupportedFormat), refuses(Image{}, LoaderError::Malformed, 63U),
                   refuses(beyondFile, LoaderError::Malformed), refuses(unreadable, LoaderError::UnsupportedFormat),
                   refuses(overlapping, LoaderError::Malformed), refuses(tooLarge, LoaderError::ImageTooLarge),
                   refuses(kernelHalf, LoaderError::Malformed), refuses(misaligned, LoaderError::Malformed),
                   refuses(fourth, LoaderError::ImageTooLarge), refuses(entryInData, LoaderError::Malformed),
                   refuses(zeroPage, LoaderError::Malformed));
}

// A dynamically linked or relocatable image, one of another version, or one for another system is
// refused before anything is mapped; one for another machine is left to an interpreter.
void TestIdentityRefusals(TestContext& runner) noexcept
{
    ExecutableImage layout{};
    const auto refuses = [&](const Image& image, const LoaderError expected) noexcept
    {
        return (Describe(image.Bytes, sizeof(image.Bytes), Rules, layout) == expected) && (layout.SegmentCount == 0U);
    };
    Image interpreted;
    interpreted.Segment(2U, 3U, 4U, 0x3000U, 0x4000'2000U, 0x10U, 0x10U);
    Image dynamic;
    dynamic.Segment(2U, 2U, 6U, 0x3000U, 0x4000'2000U, 0x10U, 0x10U);
    Image shared;
    Store(shared.Bytes + 16U, 3U, 2U);
    Image object;
    Store(object.Bytes + 16U, 1U, 2U);
    Image oldVersion;
    oldVersion.Bytes[6U] = 0U;
    Image fileVersion;
    Store(fileVersion.Bytes + 20U, 2U, 4U);
    Image linux;
    linux.Bytes[7U] = 3U;
    Image ia64;
    Store(ia64.Bytes + 18U, 50U, 2U);
    const bool foreign = refuses(ia64, LoaderError::ForeignMachineUnsupported) &&
                         (layout.Identity.Machine == ExecutableMachine::Ia64) && (layout.Identity.MachineCode == 50U);
    ASTERIA_EXPECT(runner, "dynamic, relocatable, versioned and foreign-system images are refused",
                   refuses(interpreted, LoaderError::UnsupportedFormat),
                   refuses(dynamic, LoaderError::UnsupportedFormat), refuses(shared, LoaderError::UnsupportedFormat),
                   refuses(object, LoaderError::UnsupportedFormat), refuses(oldVersion, LoaderError::Malformed),
                   refuses(fileVersion, LoaderError::Malformed), refuses(linux, LoaderError::UnsupportedFormat));
    ASTERIA_EXPECT(runner, "an image for another machine is identified and left to an interpreter", foreign);

    DescriptorImage lp32;
    Store(lp32.Bytes + 48U, 0U, 4U);
    ExecutableImage ia64Layout{};
    ASTERIA_EXPECT(runner, "an IA-64 image of another ABI than LP64 is refused",
                   Describe(lp32.Bytes, sizeof(lp32.Bytes), DescriptorRules, ia64Layout) ==
                       LoaderError::UnsupportedFormat);
}

// Probes read the identifying fields in the image's own byte order and
// recognize what the loader cannot load: big-endian and 32-bit ELF, PE.
void TestProbes(TestContext& runner) noexcept
{
    ExecutableIdentity identity{};
    Image big;
    big.Bytes[5U] = 2U;
    Store(big.Bytes + 16U, 0x0200U, 2U);
    Store(big.Bytes + 18U, 0x3200U, 2U);
    Store(big.Bytes + 20U, 0x0100'0000U, 4U);
    const bool bigIdentified = (IdentifyExecutable(big.Bytes, sizeof(big.Bytes), identity) == LoaderError::None) &&
                               (identity.ByteOrder == ExecutableByteOrder::Big) &&
                               (identity.Machine == ExecutableMachine::Ia64);
    Image elf32;
    elf32.Bytes[4U] = 1U;
    Store(elf32.Bytes + 18U, 3U, 2U);
    const bool elf32Identified =
        (IdentifyExecutable(elf32.Bytes, sizeof(elf32.Bytes), identity) == LoaderError::None) &&
        (identity.Format == ExecutableFormat::Elf32) && (identity.Machine == ExecutableMachine::Ia32);
    const PeImage pe{0x8664U};
    const bool peIdentified = (IdentifyExecutable(pe.Bytes, sizeof(pe.Bytes), identity) == LoaderError::None) &&
                              (identity.Format == ExecutableFormat::Pe32Plus) &&
                              (identity.Machine == ExecutableMachine::Amd64) &&
                              (identity.Abi == ExecutableAbi::Foreign) && (identity.AbiCode == 3U);
    const PeImage pe32{0x14CU, 0x10BU};
    const bool pe32Identified = (IdentifyExecutable(pe32.Bytes, sizeof(pe32.Bytes), identity) == LoaderError::None) &&
                                (identity.Format == ExecutableFormat::Pe32) &&
                                (identity.Machine == ExecutableMachine::Ia32);
    PeImage outside{0x8664U};
    Store(outside.Bytes + 0x3CU, 0x1F0U, 4U);
    PeImage badMagic{0x8664U, 0x333U};
    UInt8 unknown[64U]{};
    ASTERIA_EXPECT(runner, "probes identify big-endian and 32-bit ELF and PE32+ and PE32 images", bigIdentified,
                   elf32Identified, peIdentified, pe32Identified,
                   IdentifyExecutable(outside.Bytes, sizeof(outside.Bytes), identity) == LoaderError::UnsupportedFormat,
                   IdentifyExecutable(badMagic.Bytes, sizeof(badMagic.Bytes), identity) == LoaderError::Malformed,
                   IdentifyExecutable(unknown, sizeof(unknown), identity) == LoaderError::UnsupportedFormat,
                   IdentifyExecutable(nullptr, 0U, identity) == LoaderError::Malformed);
}

// precedence: a PE for the native machine is refused as an
// unsupported format; one for another machine as a foreign machine.
void TestPrecedence(TestContext& runner) noexcept
{
    ExecutableImage layout{};
    const PeImage native{0x8664U};
    const PeImage foreign{0x200U};
    Image big;
    big.Bytes[5U] = 2U;
    Store(big.Bytes + 16U, 0x0200U, 2U);
    Store(big.Bytes + 18U, 0x3E00U, 2U);
    Store(big.Bytes + 20U, 0x0100'0000U, 4U);
    ASTERIA_EXPECT(runner, "a native PE is an unsupported format, a foreign one a foreign machine",
                   Describe(native.Bytes, sizeof(native.Bytes), Rules, layout) == LoaderError::UnsupportedFormat,
                   Describe(foreign.Bytes, sizeof(foreign.Bytes), Rules, layout) ==
                       LoaderError::ForeignMachineUnsupported,
                   layout.Identity.Format == ExecutableFormat::Pe32Plus,
                   Describe(big.Bytes, sizeof(big.Bytes), Rules, layout) == LoaderError::UnsupportedFormat,
                   layout.SegmentCount == 0U);
}

void TestEntryDescriptor(TestContext& runner) noexcept
{
    const DescriptorImage image;
    ExecutableImage layout{};
    const LoaderError error = Describe(image.Bytes, sizeof(image.Bytes), DescriptorRules, layout);
    ExecutableImage amd64{};
    const LoaderError asAmd64 = Describe(image.Bytes, sizeof(image.Bytes), Rules, amd64);
    ASTERIA_EXPECT(runner, "an IA-64 entry descriptor yields its ip and gp; on AMD64 the image is foreign",
                   error == LoaderError::None, layout.Entry == (TextStart + 0x40U),
                   layout.GlobalPointer == (DataStart + 0x800U), layout.SegmentCount == 2U,
                   asAmd64 == LoaderError::ForeignMachineUnsupported);

    ExecutableImage refused{};
    const auto rejects = [&](const Image& bad) noexcept
    {
        return (Describe(bad.Bytes, sizeof(bad.Bytes), DescriptorRules, refused) != LoaderError::None) &&
               (refused.SegmentCount == 0U);
    };
    DescriptorImage misalignedDescriptor;
    Store(misalignedDescriptor.Bytes + 24U, DataStart + 0x14U, 8U);
    DescriptorImage beyondFileBytes;
    Store(beyondFileBytes.Bytes + 24U, DataStart + 0x50U, 8U);
    DescriptorImage outsideSegments;
    Store(outsideSegments.Bytes + 24U, DataStart + 0x2000U, 8U);
    DescriptorImage ipInData;
    Store(ipInData.Bytes + 0x3010U, DataStart + 0x40U, 8U);
    DescriptorImage ipNotBundle;
    Store(ipNotBundle.Bytes + 0x3010U, TextStart + 0x48U, 8U);
    DescriptorImage regionZero;
    regionZero.Segment(1U, 1U, 4U, 0x3000U, 0x4000'2000U, 0x58U, 0x58U);
    ASTERIA_EXPECT(runner, "a descriptor outside the file bytes, misaligned, or naming no bundle of text is refused",
                   rejects(misalignedDescriptor), rejects(beyondFileBytes), rejects(outsideSegments), rejects(ipInData),
                   rejects(ipNotBundle), rejects(regionZero));
}

// Service IPC: a third, writable segment with no file bytes (the image's.bss) is admitted wherever
// Its file offset lies; a writable segment with file bytes (unless the rules admit it), a writable
// executable one or a second writable one is refused, and an empty PT_LOAD is skipped.
void TestBssSegment(TestContext& runner) noexcept
{
    ExecutableImage layout{};
    Image bss;
    bss.Segment(2U, 1U, 6U, 0x9'0000U, 0x4000'4000U, 0U, 0x2100U);
    const LoaderError error = Describe(bss.Bytes, sizeof(bss.Bytes), Rules, layout);
    ASTERIA_EXPECT(runner, "a writable segment of zero-filled pages loads past the file's end",
                   error == LoaderError::None, layout.SegmentCount == 3U,
                   layout.Segments[2U].VirtualStart == 0x4000'4000U, layout.Segments[2U].Pages == 3U,
                   layout.Segments[2U].FileBytes == 0U, layout.Segments[2U].FileOffset == 0U,
                   layout.Segments[2U].Writable, !layout.Segments[2U].Executable);

    ExecutableImage refused{};
    const auto rejects = [&](const Image& image) noexcept
    {
        return Describe(image.Bytes, sizeof(image.Bytes), Rules, refused) == LoaderError::UnsupportedFormat;
    };
    Image data;
    data.Segment(2U, 1U, 6U, 0x3000U, 0x4000'4000U, 0x58U, 0x100U);
    Image writableText;
    writableText.Segment(2U, 1U, 7U, 0x9'0000U, 0x4000'4000U, 0U, 0x100U);
    Image twoWritable;
    twoWritable.Segment(1U, 1U, 6U, 0x9'0000U, 0x4000'2000U, 0U, 0x100U);
    twoWritable.Segment(2U, 1U, 6U, 0x9'0000U, 0x4000'4000U, 0U, 0x100U);
    MachineRules dataRules = Rules;
    dataRules.WritableFileBytes = true;
    ExecutableImage withData{};
    ASTERIA_EXPECT(runner, "a writable segment with file bytes or code, or a second writable one, is refused",
                   rejects(data), rejects(writableText), rejects(twoWritable), refused.SegmentCount == 0U,
                   Describe(data.Bytes, sizeof(data.Bytes), dataRules, withData) == LoaderError::None,
                   withData.SegmentCount == 3U, withData.Segments[2U].FileBytes == 0x58U);

    Image empty;
    empty.Segment(2U, 1U, 6U, 0x9'0000U, 0x4000'4000U, 0U, 0U);
    ExecutableImage skipped{};
    ASTERIA_EXPECT(runner, "an empty PT_LOAD is skipped",
                   Describe(empty.Bytes, sizeof(empty.Bytes), Rules, skipped) == LoaderError::None,
                   skipped.SegmentCount == 2U);
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"ExecutableFormatsTests"};
    Asteria::Test::TestLinkedLayout(runner);
    Asteria::Test::TestRejections(runner);
    Asteria::Test::TestIdentityRefusals(runner);
    Asteria::Test::TestProbes(runner);
    Asteria::Test::TestPrecedence(runner);
    Asteria::Test::TestEntryDescriptor(runner);
    Asteria::Test::TestBssSegment(runner);
    return runner.Finish();
}
