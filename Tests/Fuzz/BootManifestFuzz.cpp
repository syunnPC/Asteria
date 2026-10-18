// SPDX-License-Identifier: Apache-2.0
#include "Boot/BootManifest.hpp"

#include "Abi/PagerBootstrap.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
void Require(const bool condition) noexcept
{
    if (!condition)
    {
        std::abort();
    }
}
constinit char g_canonical[Asteria::Boot::BootManifestMaximumBytes]{};
constexpr char Seed[]{
    "version = 1\nkernel = KERNEL.ELF crc32c=0x87654321\ninit = INIT.ELF crc32c=0x1234abcd\nuser = USER.ELF crc32c=0x00000000\n"};
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size) noexcept
{
    Asteria::Boot::BootFileRegion output{};
    output.PhysicalStart = 0xA5U;
    Asteria::Boot::DisplayPolicy display{};
    display.Width = 123U;
    const auto beforeDisplay = display;
    Asteria::Boot::BootFileRecord kernel{};
    kernel.Crc32c = 0xA5U;
    const auto kernelBefore = kernel;
    const auto before = output;
    Asteria::Boot::BootTrustPolicy trust{};
    trust.Count = 0xA5U;
    trust.Entries[7].Digest[31] = 0xA5U;
    const auto trustBefore = trust;
    if (Asteria::Boot::ParseBootManifest(data, size, output, kernel, &display, &trust) !=
        Asteria::Boot::BootManifestError::None)
    {
        Require(std::memcmp(&before, &output, sizeof(output)) == 0);
        Require(std::memcmp(&kernelBefore, &kernel, sizeof(kernel)) == 0);
        Require(std::memcmp(&beforeDisplay, &display, sizeof(display)) == 0);
        Require(std::memcmp(&trustBefore, &trust, sizeof(trust)) == 0);
        return 0;
    }
    Require(trust.Count <= Asteria::Boot::BootTrustMaximumEntries);
    for (std::size_t index{0U}; index < Asteria::Boot::BootTrustMaximumEntries; ++index)
    {
        const auto& entry = trust.Entries[index];
        if (index >= trust.Count)
        {
            const Asteria::Boot::BootTrustEntry empty{};
            Require(std::memcmp(&entry, &empty, sizeof(entry)) == 0);
            continue;
        }
        Require(entry.Selector != 0U && (entry.Selector & ~3U) == 0U && entry.Module < output.ModuleCount &&
                std::strcmp(output.Modules[entry.Module].Name, Asteria::Abi::PagerDriverModuleName) == 0 &&
                entry.Driver != Asteria::Boot::BootTrustDriver::None &&
                entry.Verdict == Asteria::Boot::BootTrustVerdict::Unchecked && entry.Device <= 0x1FU &&
                entry.Function <= 7U);
    }
    Require(kernel.Name[0] != '\0');
    Require(output.Files[0].Name[0] != '\0' && output.Files[1].Name[0] != '\0');
    Require(output.ManifestBytes == size && size <= Asteria::Boot::BootManifestMaximumBytes);
    auto& canonical = g_canonical;
    std::size_t length{static_cast<std::size_t>(std::snprintf(
        canonical, sizeof(canonical), "version = 1\nkernel = %s crc32c=0x%08x\n", kernel.Name, kernel.Crc32c))};
    const int displayCount =
        display.Kind == Asteria::Boot::DisplayPolicyKind::Resolution
            ? std::snprintf(canonical + length, sizeof(canonical) - length, "display = %ux%u\n", display.Width,
                            display.Height)
            : std::snprintf(canonical + length, sizeof(canonical) - length, "display = %s\n",
                            display.Kind == Asteria::Boot::DisplayPolicyKind::Keep ? "keep" : "native");
    Require(displayCount > 0 && static_cast<std::size_t>(displayCount) < sizeof(canonical) - length);
    length += static_cast<std::size_t>(displayCount);
    constexpr const char* keys[]{"init", "user", "spinner"};
    for (std::size_t index{0U}; index < Asteria::Boot::BootFileCount; ++index)
    {
        if (output.Files[index].Name[0] != '\0')
        {
            const int count = std::snprintf(canonical + length, sizeof(canonical) - length, "%s = %s crc32c=0x%08x\n",
                                            keys[index], output.Files[index].Name, output.Files[index].Crc32c);
            Require(count > 0 && static_cast<std::size_t>(count) < sizeof(canonical) - length);
            length += static_cast<std::size_t>(count);
        }
    }
    for (std::size_t index{0U}; index < output.ModuleCount; ++index)
    {
        const auto& module = output.Modules[index];
        const int count =
            std::snprintf(canonical + length, sizeof(canonical) - length, "module = %s %s crc32c=0x%08x\n", module.Name,
                          module.File.Name, module.File.Crc32c);
        Require(count > 0 && static_cast<std::size_t>(count) < sizeof(canonical) - length);
        length += static_cast<std::size_t>(count);
    }
    for (std::size_t index{0U}; index < trust.Count; ++index)
    {
        const auto& entry = trust.Entries[index];
        int count{std::snprintf(canonical + length, sizeof(canonical) - length, "trust =")};
        Require(count > 0 && static_cast<std::size_t>(count) < sizeof(canonical) - length);
        length += static_cast<std::size_t>(count);
        if ((entry.Selector & Asteria::Boot::BootTrustSelectsAddress) != 0U)
        {
            count = std::snprintf(canonical + length, sizeof(canonical) - length, " pci=%04x:%02x:%02x.%x",
                                  entry.Segment, entry.Bus, entry.Device, entry.Function);
            Require(count > 0 && static_cast<std::size_t>(count) < sizeof(canonical) - length);
            length += static_cast<std::size_t>(count);
        }
        if ((entry.Selector & Asteria::Boot::BootTrustSelectsIdentifier) != 0U)
        {
            count = std::snprintf(canonical + length, sizeof(canonical) - length, " id=%04x:%04x", entry.Vendor,
                                  entry.Product);
            Require(count > 0 && static_cast<std::size_t>(count) < sizeof(canonical) - length);
            length += static_cast<std::size_t>(count);
        }
        count = std::snprintf(canonical + length, sizeof(canonical) - length,
                              " driver=%s module=%s sha256=", Asteria::Boot::BootTrustDriverName(entry.Driver),
                              output.Modules[entry.Module].Name);
        Require(count > 0 && static_cast<std::size_t>(count) < sizeof(canonical) - length);
        length += static_cast<std::size_t>(count);
        for (const std::uint8_t byte : entry.Digest)
        {
            count = std::snprintf(canonical + length, sizeof(canonical) - length, "%02x", byte);
            Require(count == 2 && static_cast<std::size_t>(count) < sizeof(canonical) - length);
            length += 2U;
        }
        Require(length + 1U < sizeof(canonical));
        canonical[length++] = '\n';
    }
    Asteria::Boot::BootFileRegion roundTrip{};
    Asteria::Boot::DisplayPolicy roundTripDisplay{};
    Asteria::Boot::BootFileRecord kernelRoundTrip{};
    Asteria::Boot::BootTrustPolicy trustRoundTrip{};
    Require(Asteria::Boot::ParseBootManifest(reinterpret_cast<const std::uint8_t*>(canonical), length, roundTrip,
                                             kernelRoundTrip, &roundTripDisplay,
                                             &trustRoundTrip) == Asteria::Boot::BootManifestError::None);
    Require(std::memcmp(&trust, &trustRoundTrip, sizeof(trust)) == 0);
    Require(std::memcmp(&kernel, &kernelRoundTrip, sizeof(kernel)) == 0);
    Require(std::memcmp(output.Files, roundTrip.Files, sizeof(output.Files)) == 0);
    Require(std::memcmp(&display, &roundTripDisplay, sizeof(display)) == 0);
    Require(output.ModuleCount == roundTrip.ModuleCount);
    Require(std::memcmp(output.Modules, roundTrip.Modules, sizeof(output.Modules)) == 0);
    return 0;
}

extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t*, std::size_t)) noexcept
{
    emit(reinterpret_cast<const std::uint8_t*>(Seed), sizeof(Seed) - 1U);
    constexpr char keep[]{
        "version=1\nkernel=KERNEL.ELF crc32c=0x00000000\ninit=INIT.ELF crc32c=0x00000000\nuser=USER.ELF crc32c=0x00000000\ndisplay=keep"};
    constexpr char panel[]{
        "version=1\nkernel=KERNEL.ELF crc32c=0x00000000\ninit=INIT.ELF crc32c=0x00000000\nuser=USER.ELF crc32c=0x00000000\ndisplay=1366x768"};
    emit(reinterpret_cast<const std::uint8_t*>(keep), sizeof(keep) - 1U);
    emit(reinterpret_cast<const std::uint8_t*>(panel), sizeof(panel) - 1U);
    constexpr char trusted[]{
        "version=1\nkernel=KERNEL.ELF crc32c=0x00000000\ninit=INIT.ELF crc32c=0x00000000\n"
        "user=USER.ELF crc32c=0x00000000\ntrust = pci=0000:00:1f.2 id=8086:2922 driver=ahci module=component "
        "sha256=00112233445566778899aabbccddeeff00112233445566778899AABBCCDDEEFF\n"
        "trust=id=1b36:000d driver=xhci module=component "
        "sha256=0000000000000000000000000000000000000000000000000000000000000000\n"
        "module = component COMP.ELF crc32c=0x12345678\n"};
    emit(reinterpret_cast<const std::uint8_t*>(trusted), sizeof(trusted) - 1U);
    emit(reinterpret_cast<const std::uint8_t*>(Seed), 3U);
    emit(reinterpret_cast<const std::uint8_t*>(Seed), 0U);
}
