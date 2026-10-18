// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_ACPI_AML_FIXTURE_HPP_INCLUDED
#define ASTERIA_TESTS_ACPI_AML_FIXTURE_HPP_INCLUDED

#include "Acpi/Aml/Library.hpp"

#include <cstdio>
#include <vector>

namespace Asteria::TestSupport::Aml
{
inline bool Read(const char* const name, std::vector<Acpi::Aml::UInt8>& output) noexcept
{
    std::FILE* const file{std::fopen(name, "rb")};
    if (file == nullptr)
    {
        return false;
    }
    if (std::fseek(file, 0, SEEK_END) != 0)
    {
        static_cast<void>(std::fclose(file));
        return false;
    }
    const auto length{std::ftell(file)};
    if (length < 0 || static_cast<unsigned long long>(length) > Acpi::Aml::MaximumTableBytes ||
        std::fseek(file, 0, SEEK_SET) != 0)
    {
        static_cast<void>(std::fclose(file));
        return false;
    }
    output.resize(static_cast<std::size_t>(length));
    const bool good{std::fread(output.data(), 1U, output.size(), file) == output.size()};
    return std::fclose(file) == 0 && good;
}
inline bool Fixture(const char* const name, std::vector<Acpi::Aml::UInt8>& output) noexcept
{
    constexpr std::size_t maximumFixturePathBytes{2048U};
    char path[maximumFixturePathBytes]{};
    const int length{std::snprintf(path, sizeof(path), "%s/%s.aml", ASTERIA_AML_FIXTURE_DIR, name)};
    return length >= 0 && static_cast<std::size_t>(length) < sizeof(path) && Read(path, output);
}
inline void Checksum(std::vector<Acpi::Aml::UInt8>& table) noexcept
{
    const auto length{table.size()};
    for (std::size_t i{0U}; i < 4U; ++i)
    {
        table[4U + i] = static_cast<Acpi::Aml::UInt8>((length >> (i * 8U)) & 0xFFU);
    }
    table[9U] = 0U;
    Acpi::Aml::UInt32 sum{0U};
    for (const auto byte : table)
    {
        sum = (sum + byte) & 0xFFU;
    }
    table[9U] = static_cast<Acpi::Aml::UInt8>((256U - sum) & 0xFFU);
}
inline std::vector<Acpi::Aml::UInt8> Table(const std::vector<Acpi::Aml::UInt8>& body) noexcept
{
    std::vector<Acpi::Aml::UInt8> table(36U, 0U);
    table[0U] = 'D';
    table[1U] = 'S';
    table[2U] = 'D';
    table[3U] = 'T';
    table[8U] = 2U;
    table.insert(table.end(), body.begin(), body.end());
    Checksum(table);
    return table;
}
} // namespace Asteria::TestSupport::Aml

#endif
