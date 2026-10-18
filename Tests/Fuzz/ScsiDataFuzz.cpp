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
void Put(std::uint8_t* bytes, M::UInt64 value, std::size_t count) noexcept
{
    for (std::size_t i{0U}; i < count; ++i)
    {
        bytes[count - i - 1U] = static_cast<std::uint8_t>((value >> (i * 8U)) & 255U);
    }
}
} // namespace
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) noexcept
{
    M::Inquiry inquiry{};
    inquiry.PeripheralType = 0xA5U;
    const M::Inquiry beforeInquiry = inquiry;
    if (M::Scsi::DecodeInquiry(data, size, inquiry))
    {
        Require(size >= 36U && inquiry.PeripheralType <= 31U);
        Require(inquiry.Vendor[8U] == '\0' && inquiry.Product[16U] == '\0');
        for (std::size_t i{0U}; i < 8U; ++i)
        {
            Require(inquiry.Vendor[i] >= 32 && inquiry.Vendor[i] <= 126);
        }
        for (std::size_t i{0U}; i < 16U; ++i)
        {
            Require(inquiry.Product[i] >= 32 && inquiry.Product[i] <= 126);
        }
    }
    else
    {
        Require(std::memcmp(&beforeInquiry, &inquiry, sizeof(inquiry)) == 0);
    }
    for (unsigned variant{0U}; variant < 2U; ++variant)
    {
        M::Capacity capacity{.BlockCount = 0xA5U, .BlockSize = 0xA5U, .Saturated = true};
        const M::Capacity before = capacity;
        const bool good =
            (variant == 0U) ? M::Scsi::Capacity10(data, size, capacity) : M::Scsi::Capacity16(data, size, capacity);
        if (good)
        {
            Require(capacity.BlockCount > 0U && capacity.BlockSize > 0U);
            std::uint8_t canonical[32U]{};
            Put(canonical, capacity.BlockCount - 1U, (variant == 0U) ? 4U : 8U);
            Put(canonical + ((variant == 0U) ? 4U : 8U), capacity.BlockSize, 4U);
            M::Capacity again{};
            Require((variant == 0U) ? M::Scsi::Capacity10(canonical, 8U, again)
                                    : M::Scsi::Capacity16(canonical, 32U, again));
            Require(capacity.BlockCount == again.BlockCount && capacity.BlockSize == again.BlockSize &&
                    capacity.Saturated == again.Saturated);
        }
        else
        {
            Require(std::memcmp(&before, &capacity, sizeof(capacity)) == 0);
        }
        bool readOnly{true};
        if (M::Scsi::ModeSense(data, size, variant != 0U, readOnly))
        {
            Require(size >= ((variant == 0U) ? 4U : 8U));
            Require(readOnly == ((data[(variant == 0U) ? 2U : 3U] & 128U) != 0U));
        }
        else
        {
            Require(readOnly);
        }
    }
    return 0;
}
extern "C" void AsteriaFuzzSeeds(void (*emit)(const std::uint8_t*, std::size_t)) noexcept
{
    std::uint8_t inquiry[36U]{0U, 0x80U, 6U, 2U, 31U};
    std::memcpy(inquiry + 8U, "ASTERIA TEST USB DISK    ", 24U);
    emit(inquiry, 36U);
    std::uint8_t capacity[32U]{};
    Put(capacity, 255U, 4U);
    Put(capacity + 4U, 512U, 4U);
    emit(capacity, 8U);
    Put(capacity, 0xFFFFFFFFU, 4U);
    emit(capacity, 8U);
    Put(capacity, 0x100000000ULL, 8U);
    Put(capacity + 8U, 512U, 4U);
    emit(capacity, 32U);
    std::uint8_t mode[8U]{3U, 0U, 128U};
    emit(mode, 4U);
    mode[0U] = 0U;
    mode[1U] = 6U;
    mode[3U] = 128U;
    emit(mode, 8U);
    emit(mode, 0U);
}
