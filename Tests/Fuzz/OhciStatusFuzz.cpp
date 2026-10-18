// SPDX-License-Identifier: Apache-2.0
#include "User/Usb/OhciStatus.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace
{
namespace U = Asteria::User::Usb;
void Require(const bool good) noexcept
{
    if (!good)
    {
        std::abort();
    }
}
} // namespace
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size) noexcept
{
    if (size < 16U)
    {
        return 0;
    }
    const U::UInt32 flags = U::Load32(data);
    const U::UInt32 current = U::Load32(data + 4U);
    const U::UInt32 buffer = U::Load32(data + 8U);
    const U::Size length = U::Load32(data + 12U);
    U::OhciTdResult result{
        .Active = false, .Status = Asteria::User::Input::TransferStatus::Stall, .ActualLength = 0xA5U};
    if (!U::DecodeOhciTd(flags, current, buffer, length, result))
    {
        Require(!result.Active && result.Status == Asteria::User::Input::TransferStatus::Stall &&
                result.ActualLength == 0xA5U);
        return 0;
    }
    Require(length <= 1024U && result.ActualLength <= length);
    Require(result.Active == ((flags >> 28U) >= 14U));
    Require(result.ActualLength == (current == 0U ? length : U::Size{current - buffer}));
    // Interrupt delay/toggle/error-count metadata must not change the decode.
    U::OhciTdResult again{};
    Require(U::DecodeOhciTd(flags ^ 0x0FF00000U, current, buffer, length, again));
    Require(again.Active == result.Active && again.Status == result.Status &&
            again.ActualLength == result.ActualLength);
    return 0;
}
extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t*, std::size_t)) noexcept
{
    std::uint8_t seed[16U]{};
    for (U::UInt32 code{0U}; code < 16U; ++code)
    {
        constexpr U::UInt32 currents[]{0U, 0x10004U, 0x10009U};
        for (const U::UInt32 current : currents)
        {
            U::Store32(seed, (code << 28U) | (1U << 18U));
            U::Store32(seed + 4U, current);
            U::Store32(seed + 8U, 0x10000U);
            U::Store32(seed + 12U, 8U);
            emit(seed, sizeof(seed));
        }
    }
    U::Store32(seed + 12U, 1025U);
    emit(seed, sizeof(seed));
}
