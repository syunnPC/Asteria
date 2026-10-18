// SPDX-License-Identifier: Apache-2.0

// Native application runtime registry: a source only. No production header includes a facility
// list.
#include "User/Runtime/GenericStatus.hpp"
#include "User/Runtime/KernelStatus.hpp"
#include "User/Runtime/MemoryStatus.hpp"
#include "User/Runtime/TimeStatus.hpp"
#include "User/Runtime/ConsoleStatus.hpp"
#include "User/Runtime/FileStatus.hpp"
#include "User/Runtime/DatagramStatus.hpp"
#include "User/Runtime/IpcStatus.hpp"
#include "User/Runtime/ProcessStatus.hpp"

namespace Asteria::User::Runtime
{
namespace
{
enum class Allocation : Foundation::UInt32
{
    Generic,
    Asteria,
    Personality,
    Vendor,
};
struct Entry
{
    Facility Identity;
    Allocation Owner;
    Foundation::UInt32 LastCode;
};
constexpr Entry Entries[]{
    {.Identity = GenericFacility, .Owner = Allocation::Generic, .LastCode = LastGenericStatus},
    {.Identity = KernelFacility, .Owner = Allocation::Asteria, .LastCode = LastKernelStatus},
    {.Identity = MemoryFacility, .Owner = Allocation::Asteria, .LastCode = LastMemoryStatus},
    {.Identity = TimeFacility, .Owner = Allocation::Asteria, .LastCode = LastTimeStatus},
    {.Identity = ConsoleFacility, .Owner = Allocation::Asteria, .LastCode = LastConsoleStatus},
    {.Identity = FileFacility, .Owner = Allocation::Asteria, .LastCode = LastFileStatus},
    {.Identity = DatagramFacility, .Owner = Allocation::Asteria, .LastCode = LastDatagramStatus},
    {.Identity = IpcFacility, .Owner = Allocation::Asteria, .LastCode = LastIpcStatus},
    {.Identity = ProcessFacility, .Owner = Allocation::Asteria, .LastCode = LastProcessStatus},
};
[[nodiscard]] constexpr bool InRange(const Entry& entry) noexcept
{
    const Foundation::UInt32 number = entry.Identity.Number();
    switch (entry.Owner)
    {
    case Allocation::Generic:
        return number == 0U;
    case Allocation::Asteria:
        return (number >= 1U) && (number <= 63U);
    case Allocation::Personality:
        return (number >= 64U) && (number <= 95U);
    case Allocation::Vendor:
        return (number >= 96U) && (number <= 127U);
    }
    return false;
}
[[nodiscard]] constexpr bool Valid() noexcept
{
    constexpr Foundation::Size count{sizeof(Entries) / sizeof(Entries[0U])};
    for (Foundation::Size index{0U}; index < count; ++index)
    {
        if (!InRange(Entries[index]) || (Entries[index].LastCode > 0x00FF'FFFFU))
        {
            return false;
        }
        for (Foundation::Size other{index + 1U}; other < count; ++other)
        {
            if (Entries[index].Identity == Entries[other].Identity)
            {
                return false;
            }
        }
    }
    return true;
}
static_assert(Valid());
static_assert(InRange({.Identity = Facility{0U}, .Owner = Allocation::Generic, .LastCode = 0U}));
static_assert(!InRange({.Identity = Facility{1U}, .Owner = Allocation::Generic, .LastCode = 0U}));
static_assert(InRange({.Identity = Facility{63U}, .Owner = Allocation::Asteria, .LastCode = 0U}));
static_assert(!InRange({.Identity = Facility{64U}, .Owner = Allocation::Asteria, .LastCode = 0U}));
static_assert(InRange({.Identity = Facility{64U}, .Owner = Allocation::Personality, .LastCode = 0U}));
static_assert(InRange({.Identity = Facility{95U}, .Owner = Allocation::Personality, .LastCode = 0U}));
static_assert(!InRange({.Identity = Facility{96U}, .Owner = Allocation::Personality, .LastCode = 0U}));
static_assert(InRange({.Identity = Facility{96U}, .Owner = Allocation::Vendor, .LastCode = 0U}));
static_assert(InRange({.Identity = Facility{127U}, .Owner = Allocation::Vendor, .LastCode = 0U}));
static_assert(!InRange({.Identity = Facility{95U}, .Owner = Allocation::Vendor, .LastCode = 0U}));
} // Namespace

// The host test declares this link seam; there is deliberately no registry header.
[[nodiscard]] bool ValidateStatusRegistry() noexcept
{
    return Valid();
}
} // Namespace Asteria::User::Runtime
