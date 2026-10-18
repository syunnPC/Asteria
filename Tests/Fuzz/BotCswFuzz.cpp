// SPDX-License-Identifier: Apache-2.0
#include "User/Usb/Bot.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace
{
namespace M = Asteria::User::Usb;
void Require(bool good) noexcept
{
    if (!good)
    {
        std::abort();
    }
}
} // namespace
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) noexcept
{
    M::Csw output{.Residue = 0xA5A5U, .Status = 0xA5U};
    const M::Csw before = output;
    const M::UInt32 transfer = (size > 0U) ? static_cast<M::UInt32>(data[0U]) * 512U : 0U;
    if (M::BotCodec::DecodeCsw(data, size, 0x12345678U, transfer, output))
    {
        Require(size == 13U && output.Status <= 2U);
        Require(output.Status == 2U || output.Residue <= transfer);
        std::uint8_t canonical[13U]{0x55U, 0x53U, 0x42U, 0x53U, 0x78U, 0x56U, 0x34U, 0x12U};
        for (M::Size i{0U}; i < 4U; ++i)
        {
            canonical[8U + i] = static_cast<std::uint8_t>((output.Residue >> (i * 8U)) & 255U);
        }
        canonical[12U] = output.Status;
        Require(std::memcmp(canonical, data, 13U) == 0);
        M::Csw again{};
        Require(M::BotCodec::DecodeCsw(canonical, 13U, 0x12345678U, transfer, again));
        Require(again.Residue == output.Residue && again.Status == output.Status);
    }
    else
    {
        Require(std::memcmp(&before, &output, sizeof(output)) == 0);
    }
    return 0;
}
extern "C" void AsteriaFuzzSeeds(void (*emit)(const std::uint8_t*, std::size_t)) noexcept
{
    std::uint8_t seed[13U]{0x55U, 0x53U, 0x42U, 0x53U, 0x78U, 0x56U, 0x34U, 0x12U};
    emit(seed, 13U);
    seed[12U] = 1U;
    seed[9U] = 2U;
    emit(seed, 13U);
    seed[12U] = 2U;
    std::memset(seed + 8U, 255, 4U);
    emit(seed, 13U);
    emit(seed, 0U);
}
