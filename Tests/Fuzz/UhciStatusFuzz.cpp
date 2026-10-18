// SPDX-License-Identifier: Apache-2.0
#include "User/Usb/UhciStatus.hpp"

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
    if (size < 8U)
    {
        return 0;
    }
    const U::UInt32 status = U::Load32(data);
    const U::Size length = U::Load32(data + 4U);
    U::UhciTdResult result{
        .Active = false, .Status = Asteria::User::Input::TransferStatus::Stall, .ActualLength = 0xA5U};
    if (!U::DecodeUhciTd(status, length, result))
    {
        Require(!result.Active && result.Status == Asteria::User::Input::TransferStatus::Stall &&
                result.ActualLength == 0xA5U);
        return 0;
    }
    Require(length <= 64U && result.ActualLength <= length);
    Require(result.Active == ((status & (1U << 23U)) != 0U));
    Require(result.ActualLength == (result.Active ? 0U : ((status + 1U) & 0x7FFU)));
    U::UhciTdResult again{};
    // IOC, low speed, C_ERR and SPD cannot change a hardware completion.
    Require(U::DecodeUhciTd(status ^ 0x3F000000U, length, again));
    Require(again.Active == result.Active && again.Status == result.Status &&
            again.ActualLength == result.ActualLength);
    return 0;
}
extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t*, std::size_t)) noexcept
{
    std::uint8_t seed[8U]{};
    constexpr U::UInt32 patterns[]{0U, 1U, 2U, 4U, 8U, 16U, 32U, 64U, 48U, 127U};
    for (const U::UInt32 bits : patterns)
    {
        for (U::UInt32 errors{0U}; errors < 4U; ++errors)
        {
            U::Store32(seed, 0x7FFU | (bits << 17U) | (errors << 27U));
            U::Store32(seed + 4U, 8U);
            emit(seed, sizeof(seed));
        }
    }
    U::Store32(seed, 8U);
    U::Store32(seed + 4U, 8U);
    emit(seed, sizeof(seed));
    U::Store32(seed + 4U, 65U);
    emit(seed, sizeof(seed));
}
