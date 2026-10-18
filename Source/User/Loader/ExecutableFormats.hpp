// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_LOADER_EXECUTABLE_FORMATS_HPP_INCLUDED
#define ASTERIA_USER_LOADER_EXECUTABLE_FORMATS_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "User/Loader/ExecutableImage.hpp"

namespace Asteria::User::Loader
{
/// One executable format the loader knows. Probe reads only what identifies an image of its format,
/// in the image's own byte order, bounds-checking each header it reads: None with identity set,
/// UnsupportedFormat when the image is not of its format (or a variant of it that is no
/// executable), Malformed when it is but its identifying headers are broken. Parse describes an
/// identified image under the native machine's rules, or says why it cannot; on failure the
/// description holds no segment.
struct FormatHandler
{
    ExecutableFormat Format;
    LoaderError (*Probe)(const Foundation::UInt8* image, Foundation::Size sizeBytes,
                         ExecutableIdentity& identity) noexcept;
    LoaderError (*Parse)(const Foundation::UInt8* image, Foundation::Size sizeBytes, const ExecutableIdentity& identity,
                         const MachineRules& rules, ExecutableImage& output) noexcept;
};

/// Probes every handler in order: the first that recognizes the image sets
/// identity. UnsupportedFormat when none does.
[[nodiscard]] LoaderError IdentifyExecutable(const Foundation::UInt8* image, Foundation::Size sizeBytes,
                                             ExecutableIdentity& identity) noexcept;

/// Identifies the image and, when it is for the native machine and
/// Asteria's ABI, describes it under rules:
/// ForeignMachineUnsupported for another machine (whose images go to an
/// interpreter), UnsupportedFormat for another ABI, or the handler's error.
/// On failure output describes no segment.
[[nodiscard]] LoaderError DescribeExecutable(const Foundation::UInt8* image, Foundation::Size sizeBytes,
                                             const MachineRules& rules, ExecutableImage& output) noexcept;
/// Staging writer: segment index and byte offset within its page allocation.
/// SegmentCount denotes the separate descriptor area. The caller owns and stages
/// all bytes, including zero tails, before applying relocations; callbacks consume
/// their byte view synchronously and are bounded,
/// non-allocating, and return false on a failed write. No executable mappings may exist.
struct RelocationWriter
{
    void* Context;
    bool (*Write)(void* context, Foundation::Size segment, Foundation::UInt64 offset, const Foundation::UInt8* bytes,
                  Foundation::Size count) noexcept;
    /// Runtime address and capacity of a separate IA-64 descriptor area, sealed
    /// read-only afterwards. Slot n belongs to the first FPTR relocation numbered n
    /// for a resolved entry (ordinary table first, then PLT). Aliases share pointers;
    /// capacity of 16 * total relocation count always suffices.
    Foundation::UInt64 DescriptorAddress{};
    Foundation::UInt64 DescriptorBytes{};
};

struct RelocationResult
{
    LoaderError Error{};
    /// Raw ELF relocation number on UnsupportedFormat, zero for other errors.
    Foundation::UInt32 Type{};
    Foundation::UInt64 Entry{};
    Foundation::UInt64 GlobalPointer{};
    Foundation::UInt64 EntryDescriptor{};
};

/// Applies all relocations eagerly at base (the load bias), then returns the
/// relocated entry and gp. image and description must be the immutable bytes and
/// successful description from DescribeExecutable. Input errors perform no writes;
/// a callback failure returns Denied and may leave partial staging, which must be
/// discarded. This library allocates nothing and never maps or seals memory.
[[nodiscard]] RelocationResult ApplyExecutableRelocations(const Foundation::UInt8* image, Foundation::Size sizeBytes,
                                                          const ExecutableImage& description, Foundation::UInt64 base,
                                                          const RelocationWriter& writer) noexcept;
} // Namespace Asteria::User::Loader

#endif // ASTERIA_USER_LOADER_EXECUTABLE_FORMATS_HPP_INCLUDED
