// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_KERNEL_ELF_HPP_INCLUDED
#define ASTERIA_BOOT_KERNEL_ELF_HPP_INCLUDED

#include "Boot/BootKernelInput.hpp"
#include "Executable/Elf/Elf64Image.hpp"

namespace Asteria::Boot
{
constexpr Foundation::Size KernelElfReservationCount{3U};
constexpr Foundation::UInt32 KernelElfNoteType{0x4153'5401U};
constexpr Foundation::Size KernelElfNoteDescriptorBytes{360U};

struct KernelElfReservation
{
    Foundation::UInt64 PageCount;
    Foundation::UInt64 AlignmentPages;
    Foundation::UInt64 MaximumAddress;
    Foundation::UInt32 Kind;
    Foundation::UInt32 Flags;
};

/// Parsed source remains a borrow until placement completes. Addresses in
/// Image are preferred identity addresses; entry/GP/descriptor values are RVAs.
/// Reservations: kind 0 bootstrap, 1 runtime pool, 2 startup, 3 IA-64 runtime;
/// flag 1 is a bootstrap subview of runtime, flag 2 owns runtime-sized lower
/// alignment slack and one upper guard page. The loader owns allocations.
struct KernelElfPlan
{
    Executable::Elf::Elf64ByteView Source;
    BootImagePlacement Image;
    Foundation::UInt64 PrepareRva;
    Foundation::UInt64 EnterRva;
    Foundation::UInt64 QueryRva;
    Foundation::UInt64 GpRva;
    Foundation::UInt64 DescriptorRva;
    Foundation::UInt64 DescriptorBytes;
    KernelElfReservation Reservations[KernelElfReservationCount];
    Foundation::UInt64 NoteOffset;
    Foundation::UInt64 SectionTableOffset;
    Foundation::UInt64 ProgramTableOffset;
    Foundation::UInt16 SectionCount;
    Foundation::UInt16 ProgramCount;
    Foundation::UInt32 Architecture;
};

enum class KernelElfError : Foundation::UInt32
{
    None,
    InvalidSource,
    InvalidHeader,
    InvalidTables,
    InvalidNote,
    InvalidSegments,
    InvalidSections,
    InvalidReservations,
    InvalidSymbols,
    InvalidRelocations,
    InvalidDescriptors,
    InvalidDestination,
};

/// ELF64 LE closed ET_EXEC kernel profile. Every wire field is decoded from
/// bytes. Parse and Place allocate nothing and retain no hidden state; neither
/// accepts concurrent source mutation. Parse leaves output unchanged on failure.
/// Place reparses the source and validates all relocations before changing any
/// destination byte. Destination must name writable storage, at the allocated
/// identity address, of at least the declared extent, distinct from source and
/// plan. Permission publication and instruction coherence belong to the caller.
class KernelElf final
{
public:
    [[nodiscard]] static KernelElfError Parse(Executable::Elf::Elf64ByteView source, Foundation::UInt32 architecture,
                                              KernelElfPlan& output) noexcept;
    [[nodiscard]] static KernelElfError Place(const KernelElfPlan& plan, void* destination,
                                              Foundation::Size destinationBytes) noexcept;

private:
    KernelElf() = delete;
};

static_assert(sizeof(KernelElfReservation) == 32U);
static_assert(sizeof(KernelElfPlan) == 400U);
} // namespace Asteria::Boot

#endif // ASTERIA_BOOT_KERNEL_ELF_HPP_INCLUDED
