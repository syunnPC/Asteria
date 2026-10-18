// SPDX-License-Identifier: Apache-2.0
#include "User/Font/Outline.hpp"
#include "User/Font/Internal.hpp"
#include "User/Font/Fixture.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
using Asteria::Foundation::UInt64;
namespace
{
using Asteria::User::Font::Bytes;
using Asteria::User::Font::Command;
namespace Detail = Asteria::User::Font::Detail;
using Asteria::User::Font::Error;
using Asteria::User::Font::Index;
using Asteria::User::Font::InterpretType2;
using Asteria::User::Font::MaximumCoordinate;
using Asteria::User::Font::MaximumOperands;
using Asteria::User::Font::MaximumProgramBytes;
using Asteria::User::Font::MaximumSubroutineDepth;
using Asteria::User::Font::One;
using Asteria::User::Font::Outline;
using Asteria::User::Font::Size;
using Asteria::User::Font::Type2Workspace;
using Asteria::User::Font::UInt32;
using Asteria::User::Font::UInt8;
using Asteria::TestSupport::FontFixture::Buffer;

using Asteria::TestSupport::FontFixture::IndexOf;
using Asteria::TestSupport::FontFixture::Number;
using Asteria::TestSupport::FontFixture::RectangleProgram;
static Type2Workspace workspace{};
struct Guarded
{
    UInt64 Before{0x12345678U};
    Command Commands[512U]{};
    UInt64 After{0x87654321U};
};
static Guarded guarded{};
void Check(const Bytes program, const Index& local = {}) noexcept
{
    Outline outline{guarded.Commands, 512U, 0U, 0};
    const Error e{InterpretType2(program, local, local, 600 * One, 0, workspace, outline)};
    if (guarded.Before != 0x12345678U || guarded.After != 0x87654321U || outline.Count > 512U ||
        workspace.Count > MaximumOperands || workspace.Depth > MaximumSubroutineDepth)
    {
        std::abort();
    }
    if (e == Error::None &&
        (outline.CharstringWidth < -MaximumCoordinate || outline.CharstringWidth > MaximumCoordinate))
    {
        std::abort();
    }
}
} // namespace
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size) noexcept
{
    Check({data, size});
    if (size > MaximumProgramBytes)
    {
        return 0;
    }
    const auto encoded = IndexOf({Buffer(data, data + size)});
    Index index{};
    Size at{};
    if (Detail::ReadIndex({encoded.data(), encoded.size()}, at, index) == Error::None)
    {
        const UInt8 call[]{32U, 10U, 14U};
        Check({call, sizeof(call)}, index);
    }
    return 0;
}
extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t*, std::size_t)) noexcept
{
    const auto rectangle = RectangleProgram();
    emit(rectangle.data(), rectangle.size());
    const UInt8 call[]{32U, 10U, 11U};
    const UInt8 hints[]{139U, 149U, 1U, 19U, 0x80U, 139U, 139U, 21U, 14U};
    const UInt8 arithmetic[]{140U, 141U, 12U, 10U, 139U, 21U, 14U};
    emit(call, sizeof(call));
    emit(hints, sizeof(hints));
    emit(arithmetic, sizeof(arithmetic));
    emit(call, 0U);
    for (const UInt32 op : {34U, 35U, 36U, 37U})
    {
        Buffer p{139U, 139U, 21U};
        const UInt32 count{op == 34U ? 7U : (op == 35U ? 13U : (op == 36U ? 9U : 11U))};
        for (UInt32 i{}; i < count; ++i)
        {
            Number(p, 10);
        }
        p.push_back(12U);
        p.push_back(static_cast<UInt8>(op));
        p.push_back(14U);
        emit(p.data(), p.size());
    }
}
