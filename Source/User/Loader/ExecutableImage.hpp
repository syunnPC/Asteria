// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_LOADER_EXECUTABLE_IMAGE_HPP_INCLUDED
#define ASTERIA_USER_LOADER_EXECUTABLE_IMAGE_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::User::Loader
{
/// What an executable is, independent of its format.
enum class ExecutableFormat : Foundation::UInt8
{
    Unknown,
    Elf64,
    Elf32,
    Pe32Plus,
    Pe32,
};

/// The machine an executable's instructions are for, mapped from ELF
/// e_machine or PE Machine.
enum class ExecutableMachine : Foundation::UInt8
{
    Unknown,
    Amd64,
    Ia64,
    Ia32,
    AArch64,
};

enum class ExecutableByteOrder : Foundation::UInt8
{
    Little,
    Big,
};

/// The system interface the executable expects: Asteria's own, or another
/// operating system's (an ELF OS ABI other than System V, any PE
/// subsystem), whose system calls only an interpreter can translate.
enum class ExecutableAbi : Foundation::UInt8
{
    Asteria,
    Foreign,
};

/// Whether the executable runs only at the addresses it names, or can be
/// relocated (ELF ET_DYN, PE with base relocations).
enum class ExecutableKind : Foundation::UInt8
{
    Fixed,
    Relocatable,
};

struct ExecutableIdentity
{
    ExecutableFormat Format;
    ExecutableMachine Machine;
    ExecutableByteOrder ByteOrder;
    ExecutableAbi Abi;
    ExecutableKind Kind;
    /// The raw ELF e_machine or PE Machine, and ELF EI_OSABI or PE Subsystem.
    Foundation::UInt16 MachineCode;
    Foundation::UInt16 AbiCode;
};

/// One loadable segment, page-granular: its pages start at VirtualStart
/// (page aligned); FileBytes bytes of the image from FileOffset land at
/// VirtualStart + PageOffset and the rest of its pages are zero.
struct ExecutableSegment
{
    Foundation::UInt64 VirtualStart;
    Foundation::UInt64 Pages;
    Foundation::UInt64 PageOffset;
    Foundation::UInt64 FileOffset;
    Foundation::UInt64 FileBytes;
    bool Writable;
    bool Executable;
    /// Exact PT_LOAD memory extent, excluding page padding.
    Foundation::UInt64 MemoryBytes{};
};

/// The segments a description can hold; a machine's rules admit fewer.
inline constexpr Foundation::Size MaximumExecutableSegments{8U};

/// File-backed ELF64 RELA tables; Kind is DT_RELA (7), zero for fixed images.
struct ExecutableRelocations
{
    Foundation::UInt64 FileOffset;
    Foundation::UInt64 Bytes;
    Foundation::UInt64 Kind;
    Foundation::UInt64 RelativeCount{};
    Foundation::UInt64 PltFileOffset{};
    Foundation::UInt64 PltBytes{};
    Foundation::UInt64 SymbolAddress{};
    Foundation::UInt64 StringAddress{};
    Foundation::UInt64 StringBytes{};
};

struct ExecutableRange
{
    Foundation::UInt64 Start{};
    Foundation::UInt64 Bytes{};
};

/// A described executable: Entry is its first instruction (a bundle on
/// IA-64) and GlobalPointer the gp its entry descriptor names (zero
/// without one).
struct ExecutableImage
{
    ExecutableIdentity Identity;
    Foundation::UInt64 Entry;
    Foundation::UInt64 GlobalPointer;
    ExecutableSegment Segments[MaximumExecutableSegments];
    Foundation::Size SegmentCount;
    ExecutableRelocations Relocations;
    /// Link-time span [Start, Start + Bytes); base is a load bias, not the span start.
    ExecutableRange Span{};
    Foundation::UInt64 Alignment{};
    /// Exact range to seal after relocation; the caller must enforce page permissions.
    ExecutableRange Relro{};
    Foundation::UInt64 EntryDescriptor{};
    Foundation::UInt64 UserStart{};
    Foundation::UInt64 UserLimit{};
    /// Preserved on a description's UnsupportedFormat relocation failure.
    Foundation::UInt32 RejectedRelocationType{};
};

/// Why an executable cannot be loaded.
enum class LoaderError : Foundation::UInt8
{
    None,
    /// Its headers are inconsistent or lie outside the image.
    Malformed,
    /// No handler knows it, or it uses what the loader does not support
    /// (imports, unknown relocations, another ABI, forbidden data with file bytes).
    UnsupportedFormat,
    /// It is for another machine and no interpreter is registered for it.
    ForeignMachineUnsupported,
    /// A segment or the segment count exceeds the machine's rules.
    ImageTooLarge,
    /// The interpreter registered for it is not itself native.
    InterpreterNotNative,
    /// A registry entry refuses it.
    Denied,
};

/// The native machine's loading policy: every segment lies in
/// [UserStart, UserLimit), UserStart at least one page so page zero stays
/// unmapped; at most MaximumSegments segments of at most MaximumSegmentPages
/// pages; for ET_DYN this range is enforced at the chosen base; a writable segment carries no file bytes unless WritableFileBytes;
/// and with EntryDescriptor the entry names a {ip, gp} descriptor in a
/// segment's file bytes (IA-64), not the code itself (AMD64).
struct MachineRules
{
    ExecutableMachine Machine;
    Foundation::UInt64 UserStart;
    Foundation::UInt64 UserLimit;
    bool EntryDescriptor;
    Foundation::Size MaximumSegments;
    Foundation::UInt64 MaximumSegmentPages;
    bool WritableFileBytes;
    /// ET_DYN admission is opt-in; offsets are checked before choosing a load bias.
    bool PositionIndependentImages{false};
};
} // Namespace Asteria::User::Loader

#endif // ASTERIA_USER_LOADER_EXECUTABLE_IMAGE_HPP_INCLUDED
