// SPDX-License-Identifier: Apache-2.0

#include "Boot/BootManifest.hpp"
#include "Abi/DesktopSession.hpp"

#include "Abi/PagerBootstrap.hpp"
#include "Boot/BootInfo.hpp"

namespace Asteria::Boot
{
namespace
{
struct Text
{
    const Foundation::UInt8* Bytes;
    Foundation::Size Size;
    Foundation::Size Position;
};

bool Space(const Foundation::UInt8 byte) noexcept
{
    return byte == ' ' || byte == '\t' || byte == '\r';
}

void SkipSpace(Text& text) noexcept
{
    while (text.Position < text.Size && Space(text.Bytes[text.Position]))
    {
        ++text.Position;
    }
}

// A failed token trial must leave the grammar position intact.
[[nodiscard]] bool Consume(Text& text, const char* word) noexcept
{
    Foundation::Size position{text.Position};
    for (Foundation::Size index{0U}; word[index] != '\0'; ++index)
    {
        if (position == text.Size || text.Bytes[position] != word[index])
        {
            return false;
        }
        ++position;
    }
    text.Position = position;
    return true;
}

BootManifestError FileValue(Text& text, BootFileRecord& file) noexcept
{
    Foundation::Size length{0U};
    Foundation::Size stem{0U};
    Foundation::Size extension{0U};
    bool dot{false};
    while (text.Position < text.Size && !Space(text.Bytes[text.Position]))
    {
        const Foundation::UInt8 byte{text.Bytes[text.Position++]};
        if (length == 12U)
        {
            return BootManifestError::InvalidName;
        }
        if (byte == '.' && !dot && stem != 0U)
        {
            dot = true;
        }
        else if ((byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || byte == '_')
        {
            if (dot)
            {
                ++extension;
            }
            else
            {
                ++stem;
            }
        }
        else
        {
            return BootManifestError::InvalidName;
        }
        file.Name[length++] = static_cast<char>(byte);
    }
    if (stem == 0U || stem > 8U || !dot || extension == 0U || extension > 3U)
    {
        return BootManifestError::InvalidName;
    }
    SkipSpace(text);
    if (!Consume(text, "crc32c=0x"))
    {
        return BootManifestError::InvalidCrc;
    }
    Foundation::UInt32 crc{0U};
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        if (text.Position == text.Size)
        {
            return BootManifestError::InvalidCrc;
        }
        const Foundation::UInt8 byte{text.Bytes[text.Position++]};
        Foundation::UInt32 digit{0U};
        if (byte >= '0' && byte <= '9')
        {
            digit = byte - '0';
        }
        else if (byte >= 'a' && byte <= 'f')
        {
            digit = byte - 'a' + 10U;
        }
        else if (byte >= 'A' && byte <= 'F')
        {
            digit = byte - 'A' + 10U;
        }
        else
        {
            return BootManifestError::InvalidCrc;
        }
        crc = (crc << 4U) | digit;
    }
    SkipSpace(text);
    if (text.Position != text.Size)
    {
        return BootManifestError::InvalidCrc;
    }
    file.Crc32c = crc;
    return BootManifestError::None;
}

BootManifestError DisplayValue(Text& text, DisplayPolicy& policy) noexcept
{
    if (text.Position < text.Size && text.Bytes[text.Position] == 'n')
    {
        if (!Consume(text, "native"))
        {
            return BootManifestError::InvalidDisplay;
        }
    }
    else if (text.Position < text.Size && text.Bytes[text.Position] == 'k')
    {
        if (!Consume(text, "keep"))
        {
            return BootManifestError::InvalidDisplay;
        }
        policy.Kind = DisplayPolicyKind::Keep;
    }
    else
    {
        policy.Kind = DisplayPolicyKind::Resolution;
        for (Foundation::Size axis{0U}; axis < 2U; ++axis)
        {
            Foundation::UInt32 value{0U};
            Foundation::Size digits{0U};
            while (text.Position < text.Size && text.Bytes[text.Position] >= '0' && text.Bytes[text.Position] <= '9')
            {
                const Foundation::UInt32 digit{text.Bytes[text.Position++] - Foundation::UInt32{'0'}};
                if (value > (0xFFFF'FFFFU - digit) / 10U)
                {
                    return BootManifestError::InvalidDisplay;
                }
                value = value * 10U + digit;
                ++digits;
            }
            if (digits == 0U || value == 0U)
            {
                return BootManifestError::InvalidDisplay;
            }
            if (axis == 0U)
            {
                policy.Width = value;
                if (!Consume(text, "x"))
                {
                    return BootManifestError::InvalidDisplay;
                }
            }
            else
            {
                policy.Height = value;
            }
        }
    }
    SkipSpace(text);
    return text.Position == text.Size ? BootManifestError::None : BootManifestError::InvalidDisplay;
}

// What the validation keeps of a trust line: its selector, for duplicates,
// and where its module name is in the borrowed text, for the module lookup
// once every module line is known.
struct TrustKey
{
    const Foundation::UInt8* ModuleName;
    Foundation::UInt16 Segment;
    Foundation::UInt8 Bus;
    Foundation::UInt8 Device;
    Foundation::UInt8 Function;
    Foundation::UInt8 Selector;
    Foundation::UInt16 Vendor;
    Foundation::UInt16 Product;
    Foundation::UInt8 ModuleLength;
    BootTrustDriver Driver;
};

// A module's name where its line spells it in the manifest, which outlives
// The parse: four bytes a name, so sixteen fit Parse's frame.
struct ModuleName
{
    Foundation::UInt16 Offset;
    Foundation::UInt8 Length;
    Foundation::UInt8 Reserved;
};

static_assert(BootManifestMaximumBytes <= 0x1'0000U);
static_assert(BootModuleMaximumCount == 16U);

struct ParseState
{
    const Foundation::UInt8* Manifest;
    DisplayPolicy Display;
    Foundation::UInt64 LowMemory;
    Foundation::UInt64 WatchdogSeconds{Abi::WatchdogDefaultSeconds};
    Foundation::UInt64 SessionMode{Abi::SessionConsole};
    Foundation::UInt64 LoginEnabled{0U};
    Foundation::UInt32 Seen;
    Foundation::Size ModuleCount;
    ModuleName Names[BootModuleMaximumCount];
    Foundation::Size TrustCount;
    TrustKey Trust[BootTrustMaximumEntries];
};

// Whether module's name is exactly the length bytes at name.
bool SameModuleName(const ParseState& state, const ModuleName& module, const Foundation::UInt8* const name,
                    const Foundation::Size length) noexcept
{
    bool same{module.Length == length};
    for (Foundation::Size index{0U}; same && index < length; ++index)
    {
        same = state.Manifest[module.Offset + index] == name[index];
    }
    return same;
}

bool TokenEnds(const Text& text) noexcept
{
    return text.Position == text.Size || Space(text.Bytes[text.Position]);
}

// Exactly count hex digits, either case, most significant first.
bool HexDigits(Text& text, const Foundation::Size count, Foundation::UInt32& value) noexcept
{
    value = 0U;
    for (Foundation::Size index{0U}; index < count; ++index)
    {
        if (text.Position == text.Size)
        {
            return false;
        }
        const Foundation::UInt8 byte{text.Bytes[text.Position++]};
        Foundation::UInt32 digit{0U};
        if (byte >= '0' && byte <= '9')
        {
            digit = byte - Foundation::UInt32{'0'};
        }
        else if (byte >= 'a' && byte <= 'f')
        {
            digit = byte - Foundation::UInt32{'a'} + 10U;
        }
        else if (byte >= 'A' && byte <= 'F')
        {
            digit = byte - Foundation::UInt32{'A'} + 10U;
        }
        else
        {
            return false;
        }
        value = (value << 4U) | digit;
    }
    return true;
}

// Pci=SSSS:BB:DD.F (device at most 1f, function at most 7).
bool TrustAddress(Text& text, TrustKey& key) noexcept
{
    Foundation::UInt32 segment{0U};
    Foundation::UInt32 bus{0U};
    Foundation::UInt32 device{0U};
    Foundation::UInt32 function{0U};
    if (!HexDigits(text, 4U, segment) || !Consume(text, ":") || !HexDigits(text, 2U, bus) || !Consume(text, ":") ||
        !HexDigits(text, 2U, device) || !Consume(text, ".") || !HexDigits(text, 1U, function) || device > 0x1FU ||
        function > 7U || !TokenEnds(text))
    {
        return false;
    }
    key.Segment = static_cast<Foundation::UInt16>(segment);
    key.Bus = static_cast<Foundation::UInt8>(bus);
    key.Device = static_cast<Foundation::UInt8>(device);
    key.Function = static_cast<Foundation::UInt8>(function);
    key.Selector |= BootTrustSelectsAddress;
    return true;
}

// Id=VVVV:DDDD; vendor FFFF names no function.
bool TrustIdentifier(Text& text, TrustKey& key) noexcept
{
    Foundation::UInt32 vendor{0U};
    Foundation::UInt32 product{0U};
    if (!HexDigits(text, 4U, vendor) || !Consume(text, ":") || !HexDigits(text, 4U, product) || vendor == 0xFFFFU ||
        !TokenEnds(text))
    {
        return false;
    }
    key.Vendor = static_cast<Foundation::UInt16>(vendor);
    key.Product = static_cast<Foundation::UInt16>(product);
    key.Selector |= BootTrustSelectsIdentifier;
    return true;
}

BootTrustDriver TrustDriver(Text& text) noexcept
{
    constexpr BootTrustDriver drivers[]{BootTrustDriver::Ahci, BootTrustDriver::E1000e, BootTrustDriver::Xhci,
                                        BootTrustDriver::UsbHost};
    for (const BootTrustDriver driver : drivers)
    {
        Text attempt{text};
        if (Consume(attempt, BootTrustDriverName(driver)) && TokenEnds(attempt))
        {
            text = attempt;
            return driver;
        }
    }
    return BootTrustDriver::None;
}

bool SameAddress(const TrustKey& left, const TrustKey& right) noexcept
{
    return left.Segment == right.Segment && left.Bus == right.Bus && left.Device == right.Device &&
           left.Function == right.Function;
}

// A second entry for the same address, or a second address-less entry for
// the same vendor:device, would make the allowlist ambiguous as written.
bool DuplicatesTrust(const ParseState& state, const TrustKey& key) noexcept
{
    for (Foundation::Size index{0U}; index < state.TrustCount; ++index)
    {
        const TrustKey& other = state.Trust[index];
        const bool addresses = (key.Selector & BootTrustSelectsAddress) != 0U &&
                               (other.Selector & BootTrustSelectsAddress) != 0U && SameAddress(key, other);
        const bool identifiers = key.Selector == BootTrustSelectsIdentifier &&
                                 other.Selector == BootTrustSelectsIdentifier && key.Vendor == other.Vendor &&
                                 key.Product == other.Product;
        if (addresses || identifiers)
        {
            return true;
        }
    }
    return false;
}

// Out of line, so its digest scratch stays out of Parse's frame.
[[gnu::noinline]] BootManifestError TrustValue(Text& text, ParseState& state, BootTrustPolicy* const trust) noexcept
{
    if (state.TrustCount == BootTrustMaximumEntries)
    {
        return BootManifestError::TooManyTrust;
    }
    TrustKey key{};
    if (Consume(text, "pci="))
    {
        if (!TrustAddress(text, key))
        {
            return BootManifestError::InvalidTrust;
        }
        SkipSpace(text);
    }
    {
        Text attempt{text};
        if (Consume(attempt, "id="))
        {
            text = attempt;
            if (!TrustIdentifier(text, key))
            {
                return BootManifestError::InvalidTrust;
            }
            SkipSpace(text);
        }
    }
    if (key.Selector == 0U || !Consume(text, "driver="))
    {
        return BootManifestError::InvalidTrust;
    }
    const BootTrustDriver driver = TrustDriver(text);
    if (driver == BootTrustDriver::None)
    {
        return BootManifestError::InvalidTrust;
    }
    key.Driver = driver;
    SkipSpace(text);
    if (!Consume(text, "module="))
    {
        return BootManifestError::InvalidTrust;
    }
    key.ModuleName = text.Bytes + text.Position;
    while (!TokenEnds(text))
    {
        const Foundation::UInt8 byte{text.Bytes[text.Position++]};
        if (key.ModuleLength == 15U || !((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '-'))
        {
            return BootManifestError::InvalidTrust;
        }
        ++key.ModuleLength;
    }
    SkipSpace(text);
    if (key.ModuleLength == 0U || !Consume(text, "sha256="))
    {
        return BootManifestError::InvalidTrust;
    }
    Foundation::UInt8 digest[BootTrustDigestBytes]{};
    for (Foundation::UInt8& byte : digest)
    {
        Foundation::UInt32 value{0U};
        if (!HexDigits(text, 2U, value))
        {
            return BootManifestError::InvalidTrust;
        }
        byte = static_cast<Foundation::UInt8>(value);
    }
    SkipSpace(text);
    if (text.Position != text.Size)
    {
        return BootManifestError::InvalidTrust;
    }
    if (DuplicatesTrust(state, key))
    {
        return BootManifestError::Duplicate;
    }
    if (trust != nullptr)
    {
        BootTrustEntry& entry = trust->Entries[state.TrustCount];
        entry.Segment = key.Segment;
        entry.Bus = key.Bus;
        entry.Device = key.Device;
        entry.Function = key.Function;
        entry.Selector = key.Selector;
        entry.Vendor = key.Vendor;
        entry.Product = key.Product;
        entry.Driver = driver;
        entry.Verdict = BootTrustVerdict::Unchecked;
        for (Foundation::Size index{0U}; index < BootTrustDigestBytes; ++index)
        {
            entry.Digest[index] = digest[index];
        }
    }
    state.Trust[state.TrustCount++] = key;
    return BootManifestError::None;
}

// Every trust line names the module its driver runs from: the USB host driver's (Abi:
// PagerUsbHostModuleName) for the usb driver, the drivers' module (Abi::PagerDriverModuleName) for
// every other, so the digest pins the code a trusted driver runs, and a module line names it,
// wherever in the file that line is; with trust, the entry records the module's index.
BootManifestError ResolveTrustModules(const ParseState& state, BootTrustPolicy* const trust) noexcept
{
    for (Foundation::Size entry{0U}; entry < state.TrustCount; ++entry)
    {
        const TrustKey& key = state.Trust[entry];
        const bool usb = key.Driver == BootTrustDriver::UsbHost;
        const char* const required = usb ? Abi::PagerUsbHostModuleName : Abi::PagerDriverModuleName;
        bool drivers{(key.ModuleLength + 1U) ==
                     (usb ? sizeof(Abi::PagerUsbHostModuleName) : sizeof(Abi::PagerDriverModuleName))};
        for (Foundation::Size index{0U}; drivers && index < key.ModuleLength; ++index)
        {
            drivers = required[index] == static_cast<char>(key.ModuleName[index]);
        }
        if (!drivers)
        {
            return BootManifestError::TrustModule;
        }
        Foundation::Size found{BootModuleMaximumCount};
        for (Foundation::Size module{0U}; module < state.ModuleCount && found == BootModuleMaximumCount; ++module)
        {
            found = SameModuleName(state, state.Names[module], key.ModuleName, key.ModuleLength) ? module : found;
        }
        if (found == BootModuleMaximumCount)
        {
            return BootManifestError::TrustModule;
        }
        if (trust != nullptr)
        {
            trust->Entries[entry].Module = static_cast<Foundation::UInt8>(found);
        }
    }
    return BootManifestError::None;
}

BootManifestError ModuleValue(Text& text, ParseState& state, BootFileRegion* const region) noexcept
{
    if (state.ModuleCount == BootModuleMaximumCount)
    {
        return BootManifestError::TooManyModules;
    }
    BootModuleRecord record{};
    Foundation::Size length{0U};
    const Foundation::UInt8* const name = text.Bytes + text.Position;
    while (text.Position < text.Size && !Space(text.Bytes[text.Position]))
    {
        const Foundation::UInt8 byte{text.Bytes[text.Position++]};
        if (length == 15U || !((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '-'))
        {
            return BootManifestError::InvalidName;
        }
        record.Name[length++] = static_cast<char>(byte);
    }
    if (length == 0U)
    {
        return BootManifestError::InvalidName;
    }
    for (Foundation::Size index{0U}; index < state.ModuleCount; ++index)
    {
        if (SameModuleName(state, state.Names[index], name, length))
        {
            return BootManifestError::Duplicate;
        }
    }
    SkipSpace(text);
    const BootManifestError error = FileValue(text, record.File);
    if (error != BootManifestError::None)
    {
        return error;
    }
    // The manifest is at most BootManifestMaximumBytes, so the offset fits.
    state.Names[state.ModuleCount] = ModuleName{
        .Offset = static_cast<Foundation::UInt16>(name - state.Manifest),
        .Length = static_cast<Foundation::UInt8>(length),
        .Reserved = 0U,
    };
    if (region != nullptr)
    {
        region->Modules[state.ModuleCount] = record;
    }
    ++state.ModuleCount;
    return BootManifestError::None;
}

// `lowmemory = BLOCKS`: decimal, 1 to BootLowMemoryMaximumBlocks, the line's only value.
BootManifestError LowMemoryValue(Text& text, Foundation::UInt64& blocks) noexcept
{
    Foundation::UInt64 value{0U};
    Foundation::Size digits{0U};
    while (text.Position < text.Size && text.Bytes[text.Position] >= '0' && text.Bytes[text.Position] <= '9')
    {
        value = (value * 10U) + (text.Bytes[text.Position++] - Foundation::UInt64{'0'});
        if (++digits > 4U)
        {
            return BootManifestError::InvalidLowMemory;
        }
    }
    SkipSpace(text);
    if (digits == 0U || text.Position != text.Size || value == 0U || value > BootLowMemoryMaximumBlocks)
    {
        return BootManifestError::InvalidLowMemory;
    }
    blocks = value;
    return BootManifestError::None;
}

// Out of line, so Parse's frame holds only the parse state.
[[gnu::noinline]] BootManifestError Line(Text text, ParseState& state, BootFileRegion* const region,
                                         BootFileRecord* const kernel, BootTrustPolicy* const trust) noexcept
{
    SkipSpace(text);
    if (text.Position == text.Size)
    {
        return BootManifestError::None;
    }
    Foundation::UInt32& seen = state.Seen;
    Foundation::Size key{BootFileCount};
    const char* word{nullptr};
    switch (text.Bytes[text.Position])
    {
    case 'w':
        word = "watchdog";
        key = BootFileCount + 6U;
        break;
    case 'd':
        word = "display";
        key = BootFileCount + 3U;
        break;
    case 'v':
        word = "version";
        break;
    case 'i':
        word = "init";
        key = BootInitFile;
        break;
    case 'k':
        word = "kernel";
        key = BootFileCount + 2U;
        break;
    case 'l':
        if (text.Position + 2U < text.Size && text.Bytes[text.Position + 2U] == 'g')
        {
            word = "login";
            key = BootFileCount + 8U;
        }
        else
        {
            word = "lowmemory";
            key = BootFileCount + 5U;
        }
        break;
    case 'u':
        word = "user";
        key = BootUserFile;
        break;
    case 'm':
        word = "module";
        key = BootFileCount + 1U;
        break;
    case 's':
        if (text.Position + 1U < text.Size && text.Bytes[text.Position + 1U] == 'e')
        {
            word = "session";
            key = BootFileCount + 7U;
        }
        else
        {
            word = "spinner";
            key = BootSpinnerFile;
        }
        break;
    case 't':
        word = "trust";
        key = BootFileCount + 4U;
        break;
    default:
        return BootManifestError::UnknownKey;
    }
    if (!Consume(text, word))
    {
        return BootManifestError::UnknownKey;
    }
    SkipSpace(text);
    if (!Consume(text, "="))
    {
        return BootManifestError::Syntax;
    }
    SkipSpace(text);
    if (key == BootFileCount + 1U)
    {
        return seen == 0U ? BootManifestError::Version : ModuleValue(text, state, region);
    }
    if (key == BootFileCount + 4U)
    {
        return seen == 0U ? BootManifestError::Version : TrustValue(text, state, trust);
    }
    const Foundation::UInt32 bit{Foundation::UInt32{1U} << key};
    if ((seen & bit) != 0U)
    {
        return BootManifestError::Duplicate;
    }
    if (key == BootFileCount)
    {
        if (seen != 0U || !Consume(text, "1"))
        {
            return BootManifestError::Version;
        }
        SkipSpace(text);
        if (text.Position != text.Size)
        {
            return BootManifestError::Version;
        }
    }
    else if (key == BootFileCount + 3U)
    {
        if (seen == 0U)
        {
            return BootManifestError::Version;
        }
        const BootManifestError error = DisplayValue(text, state.Display);
        if (error != BootManifestError::None)
        {
            return error;
        }
    }
    else if (key == BootFileCount + 8U)
    {
        if (seen == 0U)
        {
            return BootManifestError::Version;
        }
        Text alternative{text};
        if (Consume(text, "on"))
        {
            state.LoginEnabled = 1U;
        }
        else if (Consume(alternative, "off"))
        {
            state.LoginEnabled = 0U;
            text = alternative;
        }
        else
        {
            return BootManifestError::InvalidLogin;
        }
        SkipSpace(text);
        if (text.Position != text.Size)
        {
            return BootManifestError::InvalidLogin;
        }
    }
    else if (key == BootFileCount + 7U)
    {
        if (seen == 0U)
        {
            return BootManifestError::Version;
        }
        if (Consume(text, "desktop"))
        {
            state.SessionMode = Abi::SessionDesktop;
        }
        else if (Consume(text, "console"))
        {
            state.SessionMode = Abi::SessionConsole;
        }
        else
        {
            return BootManifestError::InvalidSession;
        }
        SkipSpace(text);
        if (text.Position != text.Size)
        {
            return BootManifestError::InvalidSession;
        }
    }
    else if (key == BootFileCount + 6U)
    {
        if (seen == 0U)
        {
            return BootManifestError::Version;
        }
        Foundation::UInt64 seconds{0U};
        if (Consume(text, "off"))
        {
            SkipSpace(text);
            if (text.Position != text.Size)
            {
                return BootManifestError::InvalidWatchdog;
            }
        }
        else if (LowMemoryValue(text, seconds) != BootManifestError::None || seconds > Abi::WatchdogMaximumSeconds)
        {
            return BootManifestError::InvalidWatchdog;
        }
        state.WatchdogSeconds = seconds;
    }
    else if (key == BootFileCount + 5U)
    {
        if (seen == 0U)
        {
            return BootManifestError::Version;
        }
        const BootManifestError error = LowMemoryValue(text, state.LowMemory);
        if (error != BootManifestError::None)
        {
            return error;
        }
    }
    else
    {
        if (seen == 0U)
        {
            return BootManifestError::Version;
        }
        BootFileRecord file{};
        const BootManifestError error = FileValue(text, file);
        if (error != BootManifestError::None)
        {
            return error;
        }
        if (key == BootFileCount + 2U)
        {
            if (kernel != nullptr)
            {
                *kernel = file;
            }
        }
        else if (region != nullptr)
        {
            region->Files[key] = file;
        }
    }
    seen |= bit;
    return BootManifestError::None;
}
BootManifestError Parse(const Foundation::UInt8* const bytes, const Foundation::Size size, BootFileRegion* const output,
                        BootFileRecord* const kernel, DisplayPolicy* const display, BootTrustPolicy* const trust,
                        Foundation::UInt64* const lowMemory, Foundation::UInt64* const watchdogSeconds,
                        Foundation::UInt64* const sessionMode, Foundation::UInt64* const loginEnabled) noexcept
{
    ParseState state{};
    state.Manifest = bytes;
    Foundation::Size start{0U};
    while (start < size)
    {
        Foundation::Size end{start};
        while (end < size && bytes[end] != '\n' && bytes[end] != '#')
        {
            ++end;
        }
        const BootManifestError error = Line({bytes + start, end - start, 0U}, state, output, kernel, trust);
        if (error != BootManifestError::None)
        {
            return error;
        }
        while (end < size && bytes[end] != '\n')
        {
            if ((bytes[end] < 32U && !Space(bytes[end])) || bytes[end] > 126U)
            {
                return BootManifestError::Syntax;
            }
            ++end;
        }
        start = end < size ? end + 1U : end;
    }
    constexpr Foundation::UInt32 required{(1U << BootFileCount) | (1U << (BootFileCount + 2U)) | (1U << BootInitFile) |
                                          (1U << BootUserFile)};
    if ((state.Seen & required) != required)
    {
        return BootManifestError::MissingRequired;
    }
    const BootManifestError modules = ResolveTrustModules(state, trust);
    if (modules != BootManifestError::None)
    {
        return modules;
    }
    if (trust != nullptr)
    {
        trust->Count = state.TrustCount;
    }
    if (output != nullptr)
    {
        output->ModuleCount = state.ModuleCount;
        output->ManifestBytes = size;
    }
    if (display != nullptr)
    {
        *display = state.Display;
    }
    if (lowMemory != nullptr)
    {
        *lowMemory = state.LowMemory;
    }
    if (watchdogSeconds != nullptr)
    {
        *watchdogSeconds = state.WatchdogSeconds;
    }
    if (sessionMode != nullptr)
    {
        *sessionMode = state.SessionMode;
    }
    if (loginEnabled != nullptr)
    {
        *loginEnabled = state.LoginEnabled;
    }
    return BootManifestError::None;
}
} // Namespace

BootManifestError ParseBootManifest(const Foundation::UInt8* const bytes, const Foundation::Size size,
                                    BootFileRegion& output, BootFileRecord& kernel, DisplayPolicy* const display,
                                    BootTrustPolicy* const trust, Foundation::UInt64* const lowMemory,
                                    Foundation::UInt64* const watchdogSeconds, Foundation::UInt64* const sessionMode,
                                    Foundation::UInt64* const loginEnabled) noexcept
{
    if (size > BootManifestMaximumBytes)
    {
        return BootManifestError::TooLarge;
    }
    if (bytes == nullptr && size != 0U)
    {
        return BootManifestError::Syntax;
    }
    // Validate before writing: bounded scratch and failure atomicity without
    // placing the complete provenance record on the kernel stack.
    const BootManifestError error =
        Parse(bytes, size, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    if (error != BootManifestError::None)
    {
        return error;
    }
    output.PhysicalStart = 0U;
    output.PageCount = 0U;
    output.ManifestBytes = 0U;
    output.ModuleOffset = 0U;
    output.ModuleBytes = 0U;
    output.ModuleCount = 0U;
    for (BootFileRecord& file : output.Files)
    {
        file = BootFileRecord{};
    }
    for (BootModuleRecord& module : output.Modules)
    {
        module = BootModuleRecord{};
    }
    kernel = {};
    if (trust != nullptr)
    {
        trust->Count = 0U;
        for (BootTrustEntry& entry : trust->Entries)
        {
            entry = BootTrustEntry{};
        }
    }
    return Parse(bytes, size, &output, &kernel, display, trust, lowMemory, watchdogSeconds, sessionMode, loginEnabled);
}

const char* BootManifestReason(const BootManifestError error) noexcept
{
    switch (error)
    {
    case BootManifestError::None:
        return "OK";
    case BootManifestError::TooLarge:
        return "MANIFEST-TOO-LARGE";
    case BootManifestError::TooManyModules:
        return "MANIFEST-TOO-MANY-MODULES";
    case BootManifestError::Syntax:
        return "MANIFEST-SYNTAX";
    case BootManifestError::Version:
        return "MANIFEST-VERSION";
    case BootManifestError::UnknownKey:
        return "MANIFEST-UNKNOWN-KEY";
    case BootManifestError::Duplicate:
        return "MANIFEST-DUPLICATE";
    case BootManifestError::MissingRequired:
        return "MANIFEST-MISSING-REQUIRED";
    case BootManifestError::InvalidName:
        return "MANIFEST-INVALID-NAME";
    case BootManifestError::InvalidDisplay:
        return "MANIFEST-INVALID-DISPLAY";
    case BootManifestError::InvalidCrc:
        return "MANIFEST-INVALID-CRC";
    case BootManifestError::InvalidTrust:
        return "MANIFEST-INVALID-TRUST";
    case BootManifestError::TooManyTrust:
        return "MANIFEST-TOO-MANY-TRUST";
    case BootManifestError::TrustModule:
        return "MANIFEST-TRUST-MODULE";
    case BootManifestError::InvalidLogin:
        return "MANIFEST-LOGIN";
    case BootManifestError::InvalidSession:
        return "MANIFEST-INVALID-SESSION";
    case BootManifestError::InvalidWatchdog:
        return "MANIFEST-INVALID-WATCHDOG";
    case BootManifestError::InvalidLowMemory:
        return "MANIFEST-INVALID-LOWMEMORY";
    }
    return "MANIFEST-INVALID-ERROR";
}
} // Namespace Asteria::Boot
