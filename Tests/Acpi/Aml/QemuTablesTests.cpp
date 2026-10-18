// SPDX-License-Identifier: Apache-2.0
#include "Acpi/Aml/Facts.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <string>
#include <vector>

#include "Acpi/Aml/Fixture.hpp"
#include "Support/TestContext.hpp"

namespace
{
namespace Aml = Asteria::Acpi::Aml;
namespace Fixture = Asteria::TestSupport::Aml;
using Asteria::TestSupport::TestContext;
bool Irq(const Aml::DeviceFacts& facts, const Aml::UInt32 number) noexcept
{
    for (Aml::Size i{0U}; i < facts.Crs.Count; ++i)
    {
        if (facts.Crs.Items[i].Kind == Aml::ResourceKind::Irq &&
            (facts.Crs.Items[i].Interrupt & (Aml::UInt32{1U} << number)) != 0U)
        {
            return true;
        }
    }
    return false;
}
bool Port(const Aml::DeviceFacts& facts, const Aml::UInt64 port) noexcept
{
    for (Aml::Size i{0U}; i < facts.Crs.Count; ++i)
    {
        const auto& resource{facts.Crs.Items[i]};
        if ((resource.Kind == Aml::ResourceKind::Io || resource.Kind == Aml::ResourceKind::FixedIo) &&
            resource.Minimum == port && resource.Length == 1U)
        {
            return true;
        }
    }
    return false;
}
void Run(TestContext& context, Aml::Library& library, const char* const base, const char* const machine) noexcept
{
    const std::string directory{std::string{base} + "/" + machine};
    std::vector<std::vector<Aml::UInt8>> tables;
    tables.emplace_back();
    if (!ASTERIA_EXPECT(context, "external QEMU DSDT read",
                        Fixture::Read((directory + "/DSDT").c_str(), tables.back())))
    {
        return;
    }
    // Sort only unqualified baseline SSDTs; variant-suffixed files describe
    // different machines and must not mix with the unqualified DSDT.
    constexpr std::size_t maximumFilenameBytes{256U};
    std::vector<std::array<char, maximumFilenameBytes>> names;
    DIR* const scan{opendir(directory.c_str())};
    if (!ASTERIA_EXPECT(context, "external QEMU table directory", scan != nullptr))
    {
        return;
    }
    {
        dirent* entry{readdir(scan)};
        while (entry != nullptr)
        {
            const char* const name{entry->d_name};
            bool match{std::strncmp(name, "SSDT", 4U) == 0};
            for (std::size_t i{4U}; match && name[i] != '\0'; ++i)
            {
                match = name[i] >= '0' && name[i] <= '9';
            }
            if (match)
            {
                std::array<char, maximumFilenameBytes> copy{};
                static_cast<void>(std::snprintf(copy.data(), copy.size(), "%s", name));
                names.push_back(copy);
            }
            entry = readdir(scan);
        }
        static_cast<void>(closedir(scan));
    }
    std::sort(names.begin(), names.end(),
              [](const auto& left, const auto& right) noexcept
              {
                  return std::strcmp(left.data(), right.data()) < 0;
              });
    for (const auto& name : names)
    {
        tables.emplace_back();
        ASTERIA_EXPECT(context, "external QEMU SSDT read",
                       Fixture::Read((directory + "/" + name.data()).c_str(), tables.back()));
    }
    library.Reset();
    for (const auto& table : tables)
    {
        ASTERIA_EXPECT(context, "QEMU grammar", library.Load({table.data(), table.size()}));
    }
    ASTERIA_EXPECT(context, "QEMU method grammar", library.FinishLoading());
    Aml::Facts facts{library};
    const auto output{std::make_unique<Aml::DeviceFacts>()};
    ASTERIA_EXPECT(context, "QEMU PIC", facts.Prepare() == Aml::Reason::None);
    bool root{false};
    bool keyboard{false};
    bool mouse{false};
    for (Aml::Id node{facts.NextDevice()}; node != Aml::NoId; node = facts.NextDevice(node))
    {
        facts.ReadDevice(node, *output);
        if (output->PciRoot)
        {
            root = true;
            ASTERIA_EXPECT(context, "QEMU root routes", output->Sta == Aml::Presence::Present,
                           output->PrtError == Aml::Reason::None, output->PrtCount == 128U);
            static_cast<void>(std::printf("%s PCI root: %zu routes\n", machine, output->PrtCount));
        }
        if (facts.HasId(output->Hid, "PNP0303"))
        {
            keyboard = true;
            ASTERIA_EXPECT(context, "QEMU shared controller ports and keyboard IRQ",
                           output->Sta == Aml::Presence::Present, output->Crs.Error == Aml::Reason::None,
                           Port(*output, 0x60U), Port(*output, 0x64U), Irq(*output, 1U));
        }
        if (facts.HasId(output->Hid, "PNP0F13"))
        {
            mouse = true;
            // These baseline tables assign the shared 0x60/0x64 ports to KBD;
            // MOU declares only IRQ 12. Do not manufacture mouse IO resources.
            ASTERIA_EXPECT(context, "QEMU mouse IRQ", output->Sta == Aml::Presence::Present,
                           output->Crs.Error == Aml::Reason::None, Irq(*output, 12U));
        }
    }
    const auto sleep{facts.Sleep()};
    ASTERIA_EXPECT(context, "QEMU known facts", root, keyboard, mouse, sleep.Error == Aml::Reason::None,
                   sleep.TypeA == 0U, sleep.TypeB == 0U);
    static_cast<void>(std::printf("%s keyboard=PNP0303 ports=0x60,0x64 IRQ=1 mouse=PNP0F13 IRQ=12 S5=%u,%u\n", machine,
                                  static_cast<unsigned int>(sleep.TypeA), static_cast<unsigned int>(sleep.TypeB)));
}
} // namespace
int main()
{
    TestContext context{"External QEMU AML tables"};
    const char* const base{std::getenv("ASTERIA_QEMU_ACPI_DIRECTORY")};
    if (base == nullptr || base[0U] == '\0')
    {
        static_cast<void>(std::printf("SKIP: set ASTERIA_QEMU_ACPI_DIRECTORY to QEMU's tests/data/acpi/x86\n"));
        return 0;
    }
    const auto arena{std::make_unique<Aml::Arena>()};
    Aml::Library library{arena->Get()};
    Run(context, library, base, "q35");
    Run(context, library, base, "pc");
    return context.Finish();
}
