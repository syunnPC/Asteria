// SPDX-License-Identifier: Apache-2.0
#include <array>
#include <cstdio>
#include <cstring>

#include "Kernel/Security/Sha256.hpp"

// Writes each boot module's SHA-256 digest, as the boot application computes it over the module
// file it reads (Kernel/Security/Sha256, FIPS 180-4), one line per module: "<name> <FILE>
// sha256=<64 hex digits>". A BOOT.CFG trust entry names a module and this digest; the VM runner
// builds its cases' entries from the file, so no test hard-codes a digest of a module the build
// produces.
namespace
{
// Hosted build tool, explicit bounded scratch.
std::array<Asteria::Foundation::UInt8, 65536U> g_bytes{};
constexpr std::size_t MaximumModuleBytes{16U * 1024U * 1024U};

bool Digest(std::FILE* const output, const char* const name, const char* const file, const char* const path)
{
    std::FILE* const input = std::fopen(path, "rb");
    if (input == nullptr)
    {
        return false;
    }
    Asteria::Kernel::Security::Sha256 hash{};
    std::size_t total{0U};
    for (;;)
    {
        const std::size_t count = std::fread(g_bytes.data(), 1U, g_bytes.size(), input);
        if (count > MaximumModuleBytes - total)
        {
            static_cast<void>(std::fclose(input));
            return false;
        }
        total += count;
        hash.Update(g_bytes.data(), count);
        if (count != g_bytes.size())
        {
            break;
        }
    }
    const bool read = std::ferror(input) == 0 && total != 0U;
    const bool closed = std::fclose(input) == 0;
    if (!read || !closed)
    {
        return false;
    }
    Asteria::Foundation::UInt8 digest[Asteria::Kernel::Security::Sha256DigestBytes]{};
    hash.Finish(digest);
    bool written = std::fprintf(output, "%s %s sha256=", name, file) > 0;
    for (const Asteria::Foundation::UInt8 byte : digest)
    {
        written = written && std::fprintf(output, "%02x", byte) == 2;
    }
    return written && std::fputc('\n', output) != EOF;
}
} // Namespace

int main(const int argc, const char* const* const argv)
{
    if (argc < 5 || (argc - 2) % 3 != 0)
    {
        std::fputs("usage: WriteModuleDigests OUTPUT NAME FILE PATH [NAME FILE PATH ...]\n", stderr);
        return 1;
    }
    std::FILE* const output = std::fopen(argv[1], "wb");
    if (output == nullptr)
    {
        return 1;
    }
    bool written{true};
    for (int index{2}; written && index + 2 < argc; index += 3)
    {
        written = Digest(output, argv[index], argv[index + 1], argv[index + 2]);
    }
    const bool closed = std::fclose(output) == 0;
    return written && closed ? 0 : 1;
}
