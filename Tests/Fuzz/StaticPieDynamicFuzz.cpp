// SPDX-License-Identifier: Apache-2.0
// Fuzz the user format layer's bounded dynamic parser with complete ELF seeds.
// Also inject arbitrary input into a valid PT_DYNAMIC to reach tag handling
// when mutations would otherwise fail at the ELF header. Failure clears all
// load/relocation ranges; success is deterministic and all writes stay within
// the described staging buffers. The applier never reads destination memory.
#include "User/Loader/ExecutableFormats.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "Support/StaticPieImage.hpp"

namespace
{
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt64;
using Asteria::Foundation::UInt8;
using Asteria::TestSupport::PieRules;
using Asteria::TestSupport::StaticPieImage;
using Asteria::User::Loader::ApplyExecutableRelocations;
using Asteria::User::Loader::DescribeExecutable;
using Asteria::User::Loader::ExecutableImage;
using Asteria::User::Loader::LoaderError;
using Asteria::User::Loader::RelocationWriter;

void Require(const bool condition)
{
    if (!condition)
    {
        std::abort();
    }
}

struct WriteState
{
    const ExecutableImage* Image;
    Size Count{};
};

[[nodiscard]] bool Write(void* const context, const Size segment, const UInt64 offset, const UInt8*,
                         const Size count) noexcept
{
    auto& state = *static_cast<WriteState*>(context);
    ++state.Count;
    if (segment == state.Image->SegmentCount)
    {
        Require((offset <= 4096U) && (count <= 4096U - offset));
    }
    else
    {
        Require(segment < state.Image->SegmentCount);
        const UInt64 bytes = state.Image->Segments[segment].Pages * 4096U;
        Require((offset <= bytes) && (count <= bytes - offset));
    }
    return true;
}

void Check(const UInt8* const bytes, const Size count, const bool ia64)
{
    ExecutableImage first{};
    first.SegmentCount = 8U;
    first.Relocations.Bytes = 24U;
    first.Relro.Bytes = 4096U;
    const LoaderError error = DescribeExecutable(bytes, count, PieRules(ia64), first);
    ExecutableImage second{};
    Require(DescribeExecutable(bytes, count, PieRules(ia64), second) == error);
    if (error != LoaderError::None)
    {
        Require((first.SegmentCount == 0U) && (first.Relocations.Bytes == 0U) && (first.Relro.Bytes == 0U) &&
                (first.Span.Bytes == 0U));
        return;
    }
    Require((first.SegmentCount == second.SegmentCount) && (first.Entry == second.Entry) &&
            (first.GlobalPointer == second.GlobalPointer) && (first.Relocations.Bytes == second.Relocations.Bytes) &&
            (first.Span.Bytes == second.Span.Bytes) && (first.Alignment == second.Alignment));
    for (Size index{0U}; index < first.SegmentCount; ++index)
    {
        const auto& segment = first.Segments[index];
        Require((segment.FileOffset <= count) && (segment.FileBytes <= count - segment.FileOffset) &&
                (segment.FileBytes <= segment.MemoryBytes) && (segment.Pages <= 32U));
    }
    if (first.Relocations.Kind == 7U)
    {
        WriteState state{.Image = &first};
        const RelocationWriter writer{.Context = &state,
                                      .Write = Write,
                                      .DescriptorAddress = ia64 ? 0x2000'0000'0200'0000ULL : 0x5000'0000U,
                                      .DescriptorBytes = 4096U};
        const auto applied =
            ApplyExecutableRelocations(bytes, count, first, ia64 ? 0x2000'0000'0100'0000ULL : 0x4000'0000U, writer);
        Require((applied.Error == LoaderError::None) || (state.Count == 0U));
    }
}
} // namespace

extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t*, std::size_t))
{
    const StaticPieImage amd64;
    emit(amd64.Bytes, sizeof(amd64.Bytes));
    const StaticPieImage ia64{true};
    emit(ia64.Bytes, sizeof(ia64.Bytes));
    StaticPieImage symbols{true};
    symbols.Tag(3U, 0x6FFF'FFF9U, 0U);
    symbols.Rela(0U, 0x2B20U, (UInt64{1U} << 32U) | 0x47U, 0U);
    emit(symbols.Bytes, sizeof(symbols.Bytes));
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size)
{
    Check(data, size, false);
    Check(data, size, true);
    StaticPieImage tags{(size != 0U) && ((data[0U] & 1U) != 0U)};
    const Size count = (size < 0x100U) ? size : 0x100U;
    std::memcpy(tags.Bytes + 0x800U, data, count);
    Check(tags.Bytes, sizeof(tags.Bytes), tags.Bytes[18U] == 50U);
    return 0;
}
