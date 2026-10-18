// SPDX-License-Identifier: Apache-2.0
#include <cstring>

#include "Boot/Uefi/AcpiTableDump.hpp"
#include "Support/TestContext.hpp"
#include "Support/Random.hpp"

namespace
{
using Asteria::Boot::Uefi::File;
using Asteria::Boot::Uefi::Status;
using Asteria::Boot::Uefi::Success;
using Asteria::Boot::Uefi::NotFound;
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt8;
using Asteria::Foundation::UInt16;
using Asteria::Foundation::UInt64;
constexpr Size Capacity{1'048'576U};
constinit UInt8 g_input[Capacity]{};
constinit UInt8 g_output[Capacity]{};
File g_file{};
Size g_size{0U};
Size g_position{0U};
Size g_calls{0U};
Size g_closes{0U};
Size g_deletes{0U};
Size g_shortWrite{0U};
Size g_failCall{0U};
bool g_zeroWrite{false};
bool g_exists{false};
bool g_validChunks{true};
bool g_failFlush{false};
bool g_failDelete{false};

Status ASTERIA_UEFI_API Close(File*)
{
    ++g_closes;
    return Success;
}

Status ASTERIA_UEFI_API Delete(File*)
{
    ++g_deletes;
    g_exists = false;
    g_size = 0U;
    return g_failDelete ? NotFound : Success;
}

Status ASTERIA_UEFI_API Flush(File*)
{
    return g_failFlush ? NotFound : Success;
}

Status ASTERIA_UEFI_API Write(File*, Size* length, const void* bytes)
{
    ++g_calls;
    g_validChunks = g_validChunks && *length <= Asteria::Boot::Uefi::AcpiDumpWriteChunkBytes;
    if (g_calls == g_failCall)
    {
        return NotFound;
    }
    if (g_zeroWrite)
    {
        *length = 0U;
    }
    else if (g_shortWrite != 0U && *length > g_shortWrite)
    {
        *length = g_shortWrite;
    }
    std::memcpy(g_output + g_position, bytes, *length);
    g_position += *length;
    g_size = g_position;
    return Success;
}

Status ASTERIA_UEFI_API Open(File*, File** output, UInt16*, UInt64 mode, UInt64)
{
    if (mode == 3U && !g_exists)
    {
        return NotFound;
    }
    *output = &g_file;
    g_exists = true;
    g_position = 0U;
    return Success;
}

Status ASTERIA_UEFI_API SetPosition(File*, UInt64 position)
{
    if (position != ~UInt64{0U})
    {
        return NotFound;
    }
    g_position = g_size;
    return Success;
}

void Reset()
{
    g_file = {};
    g_file.Open = Open;
    g_file.Close = Close;
    g_file.Delete = Delete;
    g_file.Write = Write;
    g_file.Flush = Flush;
    g_file.SetPosition = SetPosition;
    g_size = 0U;
    g_position = 0U;
    g_calls = 0U;
    g_closes = 0U;
    g_deletes = 0U;
    g_shortWrite = 0U;
    g_failCall = 0U;
    g_zeroWrite = false;
    g_exists = false;
    g_validChunks = true;
    g_failFlush = false;
    g_failDelete = false;
}

bool NameEquals(const UInt16* actual, const char* expected)
{
    for (Size index{0U}; expected[index] != '\0'; ++index)
    {
        if (actual[index] != static_cast<UInt16>(expected[index]))
        {
            return false;
        }
    }
    return actual[std::strlen(expected)] == 0U;
}
} // namespace

int main()
{
    Asteria::TestSupport::TestContext context{"AcpiTableDumpTests"};
    UInt16 name[Asteria::Boot::Uefi::AcpiDumpNameCharacters]{};
    constexpr const char* Names[]{"SSDT1.AML", "SSDT2.AML", "SSDT10.AML", "SSDT2049.AML"};
    constexpr Asteria::Foundation::UInt32 Ordinals[]{1U, 2U, 10U, 2049U};
    for (Size index{0U}; index < 4U; ++index)
    {
        ASTERIA_EXPECT(context, "repeated signatures have stable 8.3 names",
                       Asteria::Boot::Uefi::AcpiDumpName("SSDT", Ordinals[index], name),
                       NameEquals(name, Names[index]));
    }
    ASTERIA_EXPECT(
        context, "unique names and unsafe signature rejection", Asteria::Boot::Uefi::AcpiDumpName("DSDT", 0U, name),
        NameEquals(name, "DSDT.AML"), Asteria::Boot::Uefi::AcpiDumpName("ASF!", 0U, name), NameEquals(name, "ASF!.AML"),
        !Asteria::Boot::Uefi::AcpiDumpName("../X", 0U, name), !Asteria::Boot::Uefi::AcpiDumpName("SSDT", 2050U, name));
    static_cast<void>(Asteria::Boot::Uefi::AcpiDumpName("DSDT", 0U, name));
    for (Size index{0U}; index < Capacity; ++index)
    {
        g_input[index] = static_cast<UInt8>((index * 37U) & 255U);
    }
    // Seeded lengths, partial writes, overwrites and injected protocol failures
    // compare the saved file and handle counts against the byte-array model.
    const auto seeds = Asteria::TestSupport::GetSeedRange(0xAC91U, 1U);
    Asteria::TestSupport::Random random{seeds.First};
    for (Size step{0U}; step < 80U; ++step)
    {
        Reset();
        const Size failuresBefore{context.GetFailureCount()};
        const Size length = step == 0U ? Capacity : 1U + static_cast<Size>(random.Below(Capacity));
        g_shortWrite = step % 3U == 0U ? 4093U : 0U;
        g_exists = step % 2U == 0U;
        const bool existed = g_exists;
        const Size fault = step % 8U;
        g_failCall = fault == 1U ? 2U : 0U;
        g_zeroWrite = fault == 2U;
        g_failFlush = fault == 3U;
        g_failDelete = fault == 4U && existed;
        const bool expected =
            !(g_zeroWrite || g_failFlush || g_failDelete ||
              (g_failCall != 0U &&
               length > (g_shortWrite != 0U ? g_shortWrite : Asteria::Boot::Uefi::AcpiDumpWriteChunkBytes)));
        const bool saved = Asteria::Boot::Uefi::SaveAcpiFile(g_file, name, g_input, length);
        ASTERIA_EXPECT(context, "chunked writer matches file model and closes after failures", saved == expected,
                       g_validChunks, g_deletes == (existed ? 1U : 0U), g_closes == (g_failDelete ? 0U : 1U),
                       !saved || (g_size == length && std::memcmp(g_input, g_output, length) == 0));
        if (saved)
        {
            g_calls = 0U;
            g_shortWrite = 0U;
            g_failCall = 0U;
            ASTERIA_EXPECT(context, "shorter overwrite removes stale suffix",
                           Asteria::Boot::Uefi::SaveAcpiFile(g_file, name, g_input, 37U), g_size == 37U);
            ASTERIA_EXPECT(context, "report outcome appends without replacing earlier bytes",
                           Asteria::Boot::Uefi::AppendAcpiFile(g_file, name, g_input + 37U, 29U), g_size == 66U,
                           std::memcmp(g_input, g_output, 66U) == 0);
        }
        if (context.GetFailureCount() != failuresBefore)
        {
            static_cast<void>(std::fprintf(stderr, "seed=%llu step=%zu op=save/overwrite/append length=%zu fault=%zu\n",
                                           static_cast<unsigned long long>(seeds.First), step, length, fault));
        }
    }
    return context.Finish();
}
