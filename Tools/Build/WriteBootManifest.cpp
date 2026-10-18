// SPDX-License-Identifier: Apache-2.0
#include "Abi/ComponentImageTable.hpp"

#include <array>
#include <cstdio>
#include <cstring>

#include "User/Filesystem/Crc32c.hpp"

namespace
{
// Hosted build tool, explicit bounded scratch, using the target's CRC algorithm.
std::array<Asteria::Foundation::UInt8, 65536U> g_bytes{};

bool Entry(std::FILE* const output, const char* const key, const char* const name, const char* const path)
{
    std::FILE* const input = std::fopen(path, "rb");
    if (input == nullptr)
    {
        return false;
    }
    Asteria::Foundation::UInt32 crc{0U};
    std::size_t total{0U};
    for (;;)
    {
        const std::size_t count = std::fread(g_bytes.data(), 1U, g_bytes.size(), input);
        if (count > 16U * 1024U * 1024U - total)
        {
            static_cast<void>(std::fclose(input));
            return false;
        }
        total += count;
        crc = Asteria::User::Filesystem::Crc32c::Compute(g_bytes.data(), count, crc);
        if (count != g_bytes.size())
        {
            break;
        }
    }
    const bool read = std::ferror(input) == 0 && total != 0U && total <= 16U * 1024U * 1024U;
    const bool closed = std::fclose(input) == 0;
    if (!read || !closed)
    {
        return false;
    }
    return key == nullptr ? std::fprintf(output, "%s crc32c=0x%08x\n", name, crc) > 0
                          : std::fprintf(output, "%s = %s crc32c=0x%08x\n", key, name, crc) > 0;
}
} // namespace

int main(const int argc, const char* const* const argv)
{
    bool desktop{false};
    bool login{false};
    int entries{argc};
    while (entries > 1)
    {
        if (std::strcmp(argv[entries - 1], "--desktop") == 0)
        {
            desktop = true;
            --entries;
        }
        else if (std::strcmp(argv[entries - 1], "--login") == 0)
        {
            desktop = true;
            login = true;
            --entries;
        }
        else
        {
            break;
        }
    }
    const int first{entries > 5 && std::strcmp(argv[5], "--modules") == 0 ? 6 : 7};
    if (entries <= first || std::strcmp(argv[first - 1], "--modules") != 0 || (entries - first) % 3 != 0 ||
        static_cast<std::size_t>((entries - first) / 3) > Asteria::Abi::ComponentImageMaximumCount)
    {
        std::fputs("usage: WriteBootManifest BOOT.CFG KERNEL.ELF INIT.ELF USER.ELF [SPINNER.ELF]"
                   " --modules NAME FILE PATH [NAME FILE PATH ...] [--desktop]\n",
                   stderr);
        return 1;
    }
    std::FILE* const output = std::fopen(argv[1], "wb");
    if (output == nullptr)
    {
        return 1;
    }
    bool written = std::fputs("version = 1\n", output) >= 0 && Entry(output, "kernel", "KERNEL.ELF", argv[2]) &&
                   Entry(output, "init", "INIT.ELF", argv[3]) && Entry(output, "user", "USER.ELF", argv[4]) &&
                   (first == 6 || Entry(output, "spinner", "SPINNER.ELF", argv[5]));
    written = written &&
              (!desktop ||
               std::fputs(login ? "session = desktop\nlogin = on\n" : "session = desktop\nlogin = off\n", output) >= 0);
    for (int index{first}; written && index + 2 < entries; index += 3)
    {
        written = std::fprintf(output, "module = %s ", argv[index]) > 0 &&
                  Entry(output, nullptr, argv[index + 1], argv[index + 2]);
    }
    const bool closed = std::fclose(output) == 0;
    return written && closed ? 0 : 1;
}
