// SPDX-License-Identifier: Apache-2.0
#include "Boot/BootManifest.hpp"

#include <cstddef>
#include <initializer_list>
#include <cstring>

#include "Support/TestContext.hpp"

namespace
{
using Asteria::Boot::BootManifestError;
constexpr char Valid[]{
    "# boot\r\nversion = 1\r\nkernel = KERNEL.ELF crc32c=0x87654321\r\ninit = INIT.ELF crc32c=0x1234abcd\nuser = USER.ELF crc32c=0x00000000"};
// The required lines and two modules, which trust lines name.
constexpr char Base[]{"version=1\nkernel=KERNEL.ELF crc32c=0x00000000\ninit=INIT.ELF crc32c=0x00000000\n"
                      "user=USER.ELF crc32c=0x00000000\nmodule=component COMP.ELF crc32c=0x00000000\n"
                      "module=dir DIR.ELF crc32c=0x00000000\n"};
constexpr char Digest[]{"00112233445566778899aabbccddeeff00112233445566778899AABBCCDDEEFF"};
char g_text[Asteria::Boot::BootManifestMaximumBytes]{};

// Text with every '@' replaced by Digest, after Base when base is set.
const char* Compose(const bool base, const char* text)
{
    std::size_t length{0U};
    for (const char* part = base ? Base : ""; *part != '\0'; ++part)
    {
        g_text[length++] = *part;
    }
    for (; *text != '\0'; ++text)
    {
        if (*text == '@')
        {
            for (const char digit : Digest)
            {
                if (digit != '\0')
                {
                    g_text[length++] = digit;
                }
            }
        }
        else
        {
            g_text[length++] = *text;
        }
    }
    g_text[length] = '\0';
    return g_text;
}
} // Namespace

int main()
{
    Asteria::TestSupport::TestContext context{"BootManifestTests"};
    Asteria::Boot::BootFileRegion output{};
    Asteria::Boot::DisplayPolicy display{};
    Asteria::Boot::BootFileRecord kernel{};
    Asteria::Foundation::UInt64 session{99U};
    const auto parse = [&output, &kernel, &display, &session](const char* const text)
    {
        return Asteria::Boot::ParseBootManifest(reinterpret_cast<const Asteria::Foundation::UInt8*>(text),
                                                std::strlen(text), output, kernel, &display, nullptr, nullptr, nullptr,
                                                &session);
    };
    ASTERIA_EXPECT(context, "valid required images without spinner", parse(Valid) == BootManifestError::None,
                   output.Files[0].Crc32c == 0x1234ABCDU, std::strcmp(output.Files[0].Name, "INIT.ELF") == 0,
                   kernel.Crc32c == 0x87654321U, std::strcmp(kernel.Name, "KERNEL.ELF") == 0,
                   output.Files[2].Name[0] == '\0');
    ASTERIA_EXPECT(context, "omitted session preserves console", session == 0U);
    ASTERIA_EXPECT(context, "desktop session", parse(Compose(true, "session = desktop\n")) == BootManifestError::None,
                   session == 1U);
    ASTERIA_EXPECT(context, "console session", parse(Compose(true, "session = console\n")) == BootManifestError::None,
                   session == 0U);
    session = 9U;
    ASTERIA_EXPECT(context, "invalid session leaves output alone",
                   parse(Compose(true, "session = gui\n")) == BootManifestError::InvalidSession, session == 9U);
    ASTERIA_EXPECT(context, "duplicate session",
                   parse(Compose(true, "session = desktop\nsession = console\n")) == BootManifestError::Duplicate);
    ASTERIA_EXPECT(
        context, "modules carry logical names, basenames and CRCs",
        parse(
            "version=1\nkernel=KERNEL.ELF crc32c=0x87654321\ninit=INIT.ELF crc32c=0x00000000\nuser=USER.ELF crc32c=0x00000000\n"
            "module = component COMP.ELF crc32c=0x12345678\nmodule = directory DIR.ELF crc32c=0xabcdef01") ==
            BootManifestError::None,
        output.ModuleCount == 2U, std::strcmp(output.Modules[0].Name, "component") == 0,
        std::strcmp(output.Modules[1].File.Name, "DIR.ELF") == 0, output.Modules[1].File.Crc32c == 0xABCDEF01U);
    ASTERIA_EXPECT(context, "display defaults to native", display.Kind == Asteria::Boot::DisplayPolicyKind::Native);
    Asteria::Boot::BootTrustPolicy trust{};
    const auto parseTrust = [&output, &kernel, &display, &trust](const char* const text)
    {
        return Asteria::Boot::ParseBootManifest(reinterpret_cast<const Asteria::Foundation::UInt8*>(text),
                                                std::strlen(text), output, kernel, &display, &trust);
    };
    ASTERIA_EXPECT(
        context, "trust entries by address, identifier or both, before or after their module",
        parseTrust(Compose(false, "version=1\ntrust = pci=0000:00:1f.2 id=8086:2922 driver=ahci module=component "
                                  "sha256=@ # SATA\ntrust=id=1b36:000d driver=xhci module=component sha256=@\n"
                                  "trust =\tpci=0001:02:1f.7\tdriver=e1000e  module=component sha256=@\r\n"
                                  "kernel=KERNEL.ELF crc32c=0x00000000\ninit=INIT.ELF crc32c=0x00000000\n"
                                  "user=USER.ELF crc32c=0x00000000\nmodule=dir DIR.ELF crc32c=0x00000000\n"
                                  "module=component COMP.ELF crc32c=0x00000000\n")) == BootManifestError::None,
        trust.Count == 3U,
        trust.Entries[0].Selector ==
            (Asteria::Boot::BootTrustSelectsAddress | Asteria::Boot::BootTrustSelectsIdentifier),
        trust.Entries[0].Segment == 0U, trust.Entries[0].Bus == 0U, trust.Entries[0].Device == 0x1FU,
        trust.Entries[0].Function == 2U, trust.Entries[0].Vendor == 0x8086U, trust.Entries[0].Product == 0x2922U,
        trust.Entries[0].Driver == Asteria::Boot::BootTrustDriver::Ahci, trust.Entries[0].Module == 1U,
        trust.Entries[0].Verdict == Asteria::Boot::BootTrustVerdict::Unchecked, trust.Entries[0].Digest[0] == 0x00U,
        trust.Entries[0].Digest[9] == 0x99U, trust.Entries[0].Digest[31] == 0xFFU, trust.Entries[0].Digest[26] == 0xAAU,
        trust.Entries[1].Selector == Asteria::Boot::BootTrustSelectsIdentifier, trust.Entries[1].Vendor == 0x1B36U,
        trust.Entries[1].Product == 0x000DU, trust.Entries[1].Driver == Asteria::Boot::BootTrustDriver::Xhci,
        trust.Entries[1].Module == 1U, trust.Entries[2].Selector == Asteria::Boot::BootTrustSelectsAddress,
        trust.Entries[2].Segment == 1U, trust.Entries[2].Bus == 2U, trust.Entries[2].Device == 0x1FU,
        trust.Entries[2].Function == 7U, trust.Entries[2].Driver == Asteria::Boot::BootTrustDriver::E1000e,
        trust.Entries[2].Module == 1U, trust.Entries[3].Selector == 0U);
    // The USB host driver runs from its own module, which its trust names.
    ASTERIA_EXPECT(context, "a usb trust entry names the usb module",
                   parseTrust(Compose(true, "module=usb USB.ELF crc32c=0x00000000\n"
                                            "trust=pci=0000:00:1d.7 driver=usb module=usb sha256=@\n"
                                            "trust=id=8086:2922 driver=ahci module=component sha256=@")) ==
                       BootManifestError::None,
                   trust.Count == 2U, trust.Entries[0].Driver == Asteria::Boot::BootTrustDriver::UsbHost,
                   trust.Entries[0].Device == 0x1DU, trust.Entries[0].Function == 7U, trust.Entries[0].Module == 2U,
                   trust.Entries[1].Driver == Asteria::Boot::BootTrustDriver::Ahci, trust.Entries[1].Module == 0U,
                   std::strcmp(Asteria::Boot::BootTrustDriverName(Asteria::Boot::BootTrustDriver::UsbHost), "usb") ==
                       0);
    ASTERIA_EXPECT(context, "a manifest without trust lines clears the policy",
                   parseTrust(Base) == BootManifestError::None, trust.Count == 0U, trust.Entries[0].Selector == 0U,
                   trust.Entries[0].Digest[31] == 0U);
    ASTERIA_EXPECT(
        context, "explicit panel size",
        parse(
            "version=1\nkernel=KERNEL.ELF crc32c=0x00000000\ninit=INIT.ELF crc32c=0x00000000\nuser=USER.ELF crc32c=0x00000000\n"
            "display = 1366x768 # panel") == BootManifestError::None,
        display.Kind == Asteria::Boot::DisplayPolicyKind::Resolution, display.Width == 1366U, display.Height == 768U);
    ASTERIA_EXPECT(
        context, "keep policy",
        parse(
            "version=1\nkernel=KERNEL.ELF crc32c=0x00000000\ninit=INIT.ELF crc32c=0x00000000\nuser=USER.ELF crc32c=0x00000000\ndisplay=keep") ==
            BootManifestError::None,
        display.Kind == Asteria::Boot::DisplayPolicyKind::Keep);
    ASTERIA_EXPECT(
        context, "explicit native policy",
        parse(
            "version=1\nkernel=KERNEL.ELF crc32c=0x00000000\ninit=INIT.ELF crc32c=0x00000000\nuser=USER.ELF crc32c=0x00000000\ndisplay=native") ==
            BootManifestError::None,
        display.Kind == Asteria::Boot::DisplayPolicyKind::Native);
    struct Case
    {
        const char* Text;
        BootManifestError Error;
    };
    constexpr Case cases[]{
        {"display=keep", BootManifestError::Version},
        {"version=1\ndisplay=keep\ndisplay=native", BootManifestError::Duplicate},
        {"version=1\ndisplay=auto", BootManifestError::InvalidDisplay},
        {"version=1\ndisplay=nativeX", BootManifestError::InvalidDisplay},
        {"version=1\ndisplay=keeper", BootManifestError::InvalidDisplay},
        {"version=1\ndisplay=0x768", BootManifestError::InvalidDisplay},
        {"version=1\ndisplay=1366x0", BootManifestError::InvalidDisplay},
        {"version=1\ndisplay=1366X768", BootManifestError::InvalidDisplay},
        {"version=1\ndisplay=4294967296x768", BootManifestError::InvalidDisplay},
        {"version=1\ndisplay=1366x4294967296", BootManifestError::InvalidDisplay},
        {"version=1\ndisplay=1366x768x1", BootManifestError::InvalidDisplay},
        {"version=1\ndisplay=1366x", BootManifestError::InvalidDisplay},
        {"version=1\nmodule=x X.ELF crc32c=0x00000000\nmodule=x Y.ELF crc32c=0x00000000", BootManifestError::Duplicate},
        {"version=1\nmodule=../../bad X.ELF crc32c=0x00000000", BootManifestError::InvalidName},
        // One module beyond BootModuleMaximumCount (sixteen).
        {"version=1\nmodule=x X.ELF crc32c=0x00000000\nmodule=y X.ELF crc32c=0x00000000\n"
         "module=z X.ELF crc32c=0x00000000\nmodule=a X.ELF crc32c=0x00000000\n"
         "module=b X.ELF crc32c=0x00000000\nmodule=c X.ELF crc32c=0x00000000\n"
         "module=d X.ELF crc32c=0x00000000\nmodule=e X.ELF crc32c=0x00000000\n"
         "module=f X.ELF crc32c=0x00000000\nmodule=g X.ELF crc32c=0x00000000\n"
         "module=h X.ELF crc32c=0x00000000\nmodule=i X.ELF crc32c=0x00000000\n"
         "module=j X.ELF crc32c=0x00000000\nmodule=k X.ELF crc32c=0x00000000\n"
         "module=l X.ELF crc32c=0x00000000\nmodule=m X.ELF crc32c=0x00000000\n"
         "module=n X.ELF crc32c=0x00000000",
         BootManifestError::TooManyModules},
        {"version=1\ninit=INIT.ELF crc32c=0x00000000\nuser=USER.ELF crc32c=0x00000000",
         BootManifestError::MissingRequired},
        {"version=1\nkernel=KERNEL.ELF crc32c=0x00000000\nkernel=OTHER.ELF crc32c=0x00000000",
         BootManifestError::Duplicate},
        {"version=1\nkernel=../BAD.ELF crc32c=0x00000000", BootManifestError::InvalidName},
        {"version=1\nkernel=KERNEL.ELF crc32c=0x1234567g", BootManifestError::InvalidCrc},
        {"", BootManifestError::MissingRequired},
        {"init = INIT.ELF crc32c=0x00000000", BootManifestError::Version},
        {"version=2", BootManifestError::Version},
        {"version=10", BootManifestError::Version},
        {"version=1\nversion=1", BootManifestError::Duplicate},
        {"version=1\nunknown=1", BootManifestError::UnknownKey},
        {"version=1\ninit=../X.ELF crc32c=0x00000000", BootManifestError::InvalidName},
        {"version=1\ninit=init.elf crc32c=0x00000000", BootManifestError::InvalidName},
        {"version=1\ninit=TOOLONGXX.ELF crc32c=0x00000000", BootManifestError::InvalidName},
        {"version=1\ninit=INIT.ELF crc32c=0x1234567", BootManifestError::InvalidCrc},
        {"version=1\ninit=INIT.ELF crc32c=0x123456789", BootManifestError::InvalidCrc},
        {"version=1\ninit=INIT.ELF crc32c=0x1234567g", BootManifestError::InvalidCrc},
        {"version 1", BootManifestError::Syntax},
        {"ver", BootManifestError::UnknownKey},
        {"version=1\ninit=INIT.ELF crc32c=0x00000000", BootManifestError::MissingRequired},
        {"version=1\ninit=INIT.ELF crc32c=0x00000000\ninit=INIT.ELF crc32c=0x00000000", BootManifestError::Duplicate},
    };
    struct TrustCase
    {
        bool WithBase;
        const char* Text;
        BootManifestError Error;
    };
    constexpr TrustCase trustCases[]{
        {false, "trust=pci=0000:00:1f.2 driver=ahci module=component sha256=@", BootManifestError::Version},
        {true, "trust=driver=ahci module=component sha256=@", BootManifestError::InvalidTrust},
        {true, "trust=id=8086:2922 pci=0000:00:1f.2 driver=ahci module=component sha256=@",
         BootManifestError::InvalidTrust},
        {true, "trust=pci=0000:00:20.0 driver=ahci module=component sha256=@", BootManifestError::InvalidTrust},
        {true, "trust=pci=0000:00:1f.8 driver=ahci module=component sha256=@", BootManifestError::InvalidTrust},
        {true, "trust=pci=00:1f.2 driver=ahci module=component sha256=@", BootManifestError::InvalidTrust},
        {true, "trust=pci=0000:00:1f.2x driver=ahci module=component sha256=@", BootManifestError::InvalidTrust},
        {true, "trust=id=ffff:2922 driver=ahci module=component sha256=@", BootManifestError::InvalidTrust},
        {true, "trust=id=8086:292 driver=ahci module=component sha256=@", BootManifestError::InvalidTrust},
        {true, "trust=id=8086:2922 driver=edu module=component sha256=@", BootManifestError::InvalidTrust},
        {true, "trust=id=8086:2922 driver=ahcix module=component sha256=@", BootManifestError::InvalidTrust},
        {true, "trust=id=8086:2922 driver=ahci module=Component sha256=@", BootManifestError::InvalidTrust},
        {true, "trust=id=8086:2922 driver=ahci module=component sha256=@0", BootManifestError::InvalidTrust},
        {true, "trust=id=8086:2922 driver=ahci module=component sha256=0011", BootManifestError::InvalidTrust},
        {true, "trust=id=8086:2922 driver=ahci module=component sha256=@ extra", BootManifestError::InvalidTrust},
        {true, "trust=id=8086:2922 driver=ahci sha256=@", BootManifestError::InvalidTrust},
        {true, "trust=id=8086:2922 driver=ahci module=spinner sha256=@", BootManifestError::TrustModule},
        {true, "trust=id=8086:2922 driver=ahci module=componen sha256=@", BootManifestError::TrustModule},
        {true, "trust=id=8086:2922 driver=ahci module=dir sha256=@", BootManifestError::TrustModule},
        {true, "trust=id=8086:2922 driver=ahci module=components sha256=@", BootManifestError::TrustModule},
        {true, "module=usb USB.ELF crc32c=0x00000000\ntrust=pci=0000:00:1d.7 driver=usb module=component sha256=@",
         BootManifestError::TrustModule},
        {true, "module=usb USB.ELF crc32c=0x00000000\ntrust=id=8086:2922 driver=ahci module=usb sha256=@",
         BootManifestError::TrustModule},
        {true, "module=usb USB.ELF crc32c=0x00000000\ntrust=pci=0000:00:1d.7 driver=usb module=us sha256=@",
         BootManifestError::TrustModule},
        {true, "trust=pci=0000:00:1d.7 driver=usb module=usb sha256=@", BootManifestError::TrustModule},
        {true,
         "trust=pci=0000:00:1f.2 driver=ahci module=component sha256=@\n"
         "trust=pci=0000:00:1f.2 id=8086:2922 driver=xhci module=component sha256=@",
         BootManifestError::Duplicate},
        {true,
         "trust=id=8086:2922 driver=ahci module=component sha256=@\n"
         "trust=id=8086:2922 driver=ahci module=component sha256=@",
         BootManifestError::Duplicate},
        {true,
         "trust=id=0001:0000 driver=ahci module=component sha256=@\n"
         "trust=id=0002:0000 driver=ahci module=component sha256=@\n"
         "trust=id=0003:0000 driver=ahci module=component sha256=@\n"
         "trust=id=0004:0000 driver=ahci module=component sha256=@\n"
         "trust=id=0005:0000 driver=ahci module=component sha256=@\n"
         "trust=id=0006:0000 driver=ahci module=component sha256=@\n"
         "trust=id=0007:0000 driver=ahci module=component sha256=@\n"
         "trust=id=0008:0000 driver=ahci module=component sha256=@\n"
         "trust=id=0009:0000 driver=ahci module=component sha256=@",
         BootManifestError::TooManyTrust},
    };
    // Every failure leaves a policy that a successful parse filled unchanged.
    ASTERIA_EXPECT(context, "a policy to keep",
                   parseTrust(Compose(true, "trust=pci=0000:00:1f.2 driver=ahci module=component sha256=@")) ==
                       BootManifestError::None,
                   trust.Count == 1U);
    for (const Case& test : cases)
    {
        const auto before = output;
        const auto beforeDisplay = display;
        const auto kernelBefore = kernel;
        const auto trustBefore = trust;
        ASTERIA_EXPECT(context, test.Text, parseTrust(test.Text) == test.Error,
                       std::memcmp(&before, &output, sizeof(output)) == 0,
                       std::memcmp(&kernelBefore, &kernel, sizeof(kernel)) == 0,
                       std::memcmp(&beforeDisplay, &display, sizeof(display)) == 0,
                       std::memcmp(&trustBefore, &trust, sizeof(trust)) == 0);
    }
    for (const TrustCase& test : trustCases)
    {
        const auto before = output;
        const auto beforeDisplay = display;
        const auto kernelBefore = kernel;
        const auto trustBefore = trust;
        const char* const text = Compose(test.WithBase, test.Text);
        ASTERIA_EXPECT(context, text, parseTrust(text) == test.Error,
                       std::memcmp(&before, &output, sizeof(output)) == 0,
                       std::memcmp(&kernelBefore, &kernel, sizeof(kernel)) == 0,
                       std::memcmp(&beforeDisplay, &display, sizeof(display)) == 0,
                       std::memcmp(&trustBefore, &trust, sizeof(trust)) == 0);
    }
    ASTERIA_EXPECT(
        context, "failure reasons name the trust key",
        std::strcmp(Asteria::Boot::BootManifestReason(BootManifestError::InvalidTrust), "MANIFEST-INVALID-TRUST") == 0,
        std::strcmp(Asteria::Boot::BootManifestReason(BootManifestError::TooManyTrust), "MANIFEST-TOO-MANY-TRUST") == 0,
        std::strcmp(Asteria::Boot::BootManifestReason(BootManifestError::TrustModule), "MANIFEST-TRUST-MODULE") == 0);
    // `lowmemory = BLOCKS`: 1 to 2,048 decimal blocks, zero when absent, and the value reaches the
    // caller only when the whole manifest parses.
    Asteria::Foundation::UInt64 lowMemory{77U};
    const auto parseLow = [&output, &kernel, &lowMemory](const char* const text)
    {
        return Asteria::Boot::ParseBootManifest(reinterpret_cast<const Asteria::Foundation::UInt8*>(text),
                                                std::strlen(text), output, kernel, nullptr, nullptr, &lowMemory);
    };
    const BootManifestError absent = parseLow(Base);
    const Asteria::Foundation::UInt64 absentValue = lowMemory;
    const BootManifestError some = parseLow(Compose(true, "lowmemory=64\n"));
    const Asteria::Foundation::UInt64 someValue = lowMemory;
    const BootManifestError most = parseLow(Compose(true, "lowmemory = 2048 # the most\r\n"));
    const Asteria::Foundation::UInt64 mostValue = lowMemory;
    const BootManifestError padded = parseLow(Compose(true, "lowmemory=\t0001"));
    ASTERIA_EXPECT(context, "lowmemory names 1 to 2,048 blocks and defaults to zero", absent == BootManifestError::None,
                   absentValue == 0U, some == BootManifestError::None, someValue == 64U,
                   most == BootManifestError::None, mostValue == 2048U, padded == BootManifestError::None,
                   lowMemory == 1U);
    struct LowMemoryCase
    {
        const char* Text;
        BootManifestError Error;
    };
    constexpr LowMemoryCase lowMemoryCases[]{
        {"lowmemory=0", BootManifestError::InvalidLowMemory},
        {"lowmemory=2049", BootManifestError::InvalidLowMemory},
        {"lowmemory=10000", BootManifestError::InvalidLowMemory},
        {"lowmemory=00064", BootManifestError::InvalidLowMemory},
        {"lowmemory=", BootManifestError::InvalidLowMemory},
        {"lowmemory=0x40", BootManifestError::InvalidLowMemory},
        {"lowmemory=64 128", BootManifestError::InvalidLowMemory},
        {"lowmemory=-1", BootManifestError::InvalidLowMemory},
        {"lowmemory=64\nlowmemory=64", BootManifestError::Duplicate},
    };
    for (const LowMemoryCase& test : lowMemoryCases)
    {
        lowMemory = 77U;
        ASTERIA_EXPECT(context, test.Text, parseLow(Compose(true, test.Text)) == test.Error, lowMemory == 77U);
    }
    ASTERIA_EXPECT(context, "lowmemory needs the version line first and names its failure",
                   parseLow("lowmemory=64\nversion=1") == BootManifestError::Version, lowMemory == 77U,
                   std::strcmp(Asteria::Boot::BootManifestReason(BootManifestError::InvalidLowMemory),
                               "MANIFEST-INVALID-LOWMEMORY") == 0);
    Asteria::Foundation::UInt64 seconds{77U};
    const auto parseWatchdog = [&](const char* text) noexcept
    {
        return Asteria::Boot::ParseBootManifest(reinterpret_cast<const Asteria::Foundation::UInt8*>(text),
                                                std::strlen(text), output, kernel, nullptr, nullptr, nullptr, &seconds);
    };
    ASTERIA_EXPECT(context, "watchdog absent defaults to five seconds", parseWatchdog(Base) == BootManifestError::None,
                   seconds == 5U);
    ASTERIA_EXPECT(context, "watchdog minimum",
                   parseWatchdog(Compose(true, "watchdog = 1 # enabled")) == BootManifestError::None, seconds == 1U);
    ASTERIA_EXPECT(context, "watchdog maximum",
                   parseWatchdog(Compose(true, "watchdog = 60")) == BootManifestError::None, seconds == 60U);
    ASTERIA_EXPECT(context, "watchdog off", parseWatchdog(Compose(true, "watchdog = off")) == BootManifestError::None,
                   seconds == 0U);
    for (const char* invalid :
         {"watchdog=o1", "watchdog=of1", "watchdog=o60", "watchdog=of60", "watchdog=0", "watchdog=61", "watchdog=-1",
          "watchdog=OFF", "watchdog=off trailing", "watchdog=5 5", "watchdog=99999999999999999999"})
    {
        seconds = 77U;
        ASTERIA_EXPECT(context, invalid, parseWatchdog(Compose(true, invalid)) == BootManifestError::InvalidWatchdog,
                       seconds == 77U);
    }
    ASTERIA_EXPECT(context, "watchdog duplicate",
                   parseWatchdog(Compose(true, "watchdog=5\nwatchdog=off")) == BootManifestError::Duplicate);
    ASTERIA_EXPECT(context, "watchdog requires version first",
                   parseWatchdog("watchdog=5\nversion=1") == BootManifestError::Version);
    ASTERIA_EXPECT(context, "oversized manifest",
                   Asteria::Boot::ParseBootManifest(nullptr, Asteria::Boot::BootManifestMaximumBytes + 1U, output,
                                                    kernel) == BootManifestError::TooLarge);
    ASTERIA_EXPECT(context, "null extent",
                   Asteria::Boot::ParseBootManifest(nullptr, 1U, output, kernel) == BootManifestError::Syntax);
    Asteria::Foundation::UInt64 login{77U};
    const auto parseLogin = [&](const char* text) noexcept
    {
        return Asteria::Boot::ParseBootManifest(reinterpret_cast<const Asteria::Foundation::UInt8*>(text),
                                                std::strlen(text), output, kernel, nullptr, nullptr, nullptr, nullptr,
                                                nullptr, &login);
    };
    ASTERIA_EXPECT(context, "login defaults off", parseLogin(Base) == BootManifestError::None, login == 0U);
    ASTERIA_EXPECT(context, "explicit synthetic login on",
                   parseLogin(Compose(true, "login = on")) == BootManifestError::None, login == 1U);
    ASTERIA_EXPECT(context, "explicit product login off",
                   parseLogin(Compose(true, "login = off")) == BootManifestError::None, login == 0U);
    for (const char* invalid : {"login=ON", "login=1", "login=", "login=on trailing"})
    {
        login = 77U;
        ASTERIA_EXPECT(context, invalid, parseLogin(Compose(true, invalid)) == BootManifestError::InvalidLogin,
                       login == 77U);
    }
    ASTERIA_EXPECT(context, "login duplicate refused",
                   parseLogin(Compose(true, "login=on\nlogin=off")) == BootManifestError::Duplicate);
    return context.Finish();
}
