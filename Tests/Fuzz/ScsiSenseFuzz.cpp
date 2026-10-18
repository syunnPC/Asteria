// SPDX-License-Identifier: Apache-2.0
#include "User/Usb/Scsi.hpp"

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
    M::Sense output{.Status = M::SenseStatus::MediumError, .Key = 0xA5U, .Asc = 0xA5U, .Ascq = 0xA5U};
    const M::Sense before = output;
    if (M::Scsi::DecodeSense(data, size, output))
    {
        Require(output.Key <= 15U && size >= 8U && data[7U] <= size - 8U);
        M::Sense again{};
        Require(M::Scsi::DecodeSense(data, size, again));
        Require(output.Status == again.Status && output.Key == again.Key && output.Asc == again.Asc &&
                output.Ascq == again.Ascq);
        // Current fixed and descriptor formats with equal fields classify equally.
        if ((data[0U] & 0x7FU) == 0x70U || (data[0U] & 0x7FU) == 0x72U)
        {
            std::uint8_t canonical[8U]{0x72U, output.Key, output.Asc, output.Ascq};
            M::Sense equivalent{};
            Require(M::Scsi::DecodeSense(canonical, 8U, equivalent));
            Require(equivalent.Status == output.Status);
        }
    }
    else
    {
        Require(std::memcmp(&before, &output, sizeof(output)) == 0);
    }
    return 0;
}
extern "C" void AsteriaFuzzSeeds(void (*emit)(const std::uint8_t*, std::size_t)) noexcept
{
    std::uint8_t fixed[18U]{0x70U, 0U, 2U, 0U, 0U, 0U, 0U, 10U, 0U, 0U, 0U, 0U, 4U, 1U};
    emit(fixed, 18U);
    fixed[0U] = 0x71U;
    emit(fixed, 18U);
    std::uint8_t descriptor[12U]{0x72U, 6U, 0x28U, 0U, 0U, 0U, 0U, 4U, 0U, 2U, 0U, 0U};
    emit(descriptor, 12U);
    descriptor[2U] = 0x29U;
    emit(descriptor, 12U);
    emit(fixed, 0U);
}
