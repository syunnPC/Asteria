// SPDX-License-Identifier: Apache-2.0
#include "Acpi/Aml/Report.hpp"

#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace Asteria::Tools::Acpi
{
namespace
{
namespace Aml = Asteria::Acpi::Aml;
class FileSink final : public Aml::IReportSink
{
public:
    [[nodiscard]] bool Write(const char* bytes, const Aml::Size length) noexcept override
    {
        return std::fwrite(bytes, 1U, length, stdout) == length;
    }
};
bool Read(const char* const name, std::vector<Aml::UInt8>& bytes) noexcept
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
    if (length < 0 || static_cast<unsigned long long>(length) > Aml::MaximumTableBytes ||
        std::fseek(file, 0, SEEK_SET) != 0)
    {
        static_cast<void>(std::fclose(file));
        return false;
    }
    bytes.resize(static_cast<Aml::Size>(length));
    const bool good{std::fread(bytes.data(), 1U, bytes.size(), file) == bytes.size()};
    return std::fclose(file) == 0 && good;
}
} // namespace
template <Aml::Size Nodes> int Print(const int argc, char** const argv, const int first) noexcept
{
    // The hosted tool is a memory owner; freestanding production never allocates.
    const auto arena{std::make_unique<Aml::BoundedArena<Nodes>>()};
    const auto storage{std::make_unique<Aml::ReportStorage>()};
    Aml::Library library{arena->Get()};
    std::vector<std::vector<Aml::UInt8>> tables(static_cast<Aml::Size>(argc - first));
    bool good{true};
    for (int i{first}; i < argc; ++i)
    {
        auto& bytes{tables[static_cast<Aml::Size>(i - first)]};
        if (!Read(argv[i], bytes))
        {
            static_cast<void>(std::fprintf(stderr, "Cannot read table: %s\n", argv[i]));
            return 2;
        }
        good = library.Load({bytes.data(), bytes.size()}) && good;
    }
    good = library.FinishLoading() && good;
    Aml::Facts facts{library};
    FileSink sink{};
    good = Aml::Report::Print(library, facts, *storage, sink) && good;
    return good ? 0 : 1;
}
int Run(const int argc, char** const argv) noexcept
{
    const bool reduced{argc >= 2 && std::strcmp(argv[1], "--reduced-arena") == 0};
    const int first{reduced ? 2 : 1};
    if (argc <= first || static_cast<unsigned int>(argc - first) > Aml::MaximumTables)
    {
        static_cast<void>(std::fprintf(stderr, "Usage: AmlFacts [--reduced-arena] DSDT.AML [SSDT1.AML ...]\n"));
        return 2;
    }
    return reduced ? Print<Aml::ReducedNodes>(argc, argv, first) : Print<Aml::MaximumNodes>(argc, argv, first);
}
} // namespace Asteria::Tools::Acpi

int main(const int argc, char** const argv)
{
    return Asteria::Tools::Acpi::Run(argc, argv);
}
