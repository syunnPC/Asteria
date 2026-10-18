// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <cstring>

#include "Boot/KernelElf.hpp"

namespace
{
// Hosted build-time adapter; bounded storage, using the production parser.
constinit Asteria::Foundation::UInt8 g_bytes[16U * 1024U * 1024U]{};
constinit Asteria::Boot::KernelElfPlan g_plan{};
} // namespace

int main(const int argc, const char* const* const argv)
{
    if (argc != 3)
    {
        return 1;
    }
    Asteria::Foundation::UInt32 architecture{0U};
    if (std::strcmp(argv[2], "Amd64") == 0)
    {
        architecture = Asteria::Boot::BootArchitectureAmd64;
    }
    else if (std::strcmp(argv[2], "Ia64") == 0)
    {
        architecture = Asteria::Boot::BootArchitectureIa64;
    }
    else
    {
        return 1;
    }
    std::FILE* const input = std::fopen(argv[1], "rb");
    if (input == nullptr)
    {
        return 1;
    }
    const std::size_t size = std::fread(g_bytes, 1U, sizeof(g_bytes), input);
    const int extra = std::fgetc(input);
    const bool read = std::ferror(input) == 0 && extra == EOF && size != 0U;
    const bool closed = std::fclose(input) == 0;
    if (!read || !closed)
    {
        return 1;
    }
    const auto error = Asteria::Boot::KernelElf::Parse({g_bytes, size}, architecture, g_plan);
    if (error != Asteria::Boot::KernelElfError::None)
    {
        std::fprintf(stderr, "installed KERNEL.ELF rejected: %u\n", static_cast<unsigned>(error));
        return 1;
    }
    return 0;
}
