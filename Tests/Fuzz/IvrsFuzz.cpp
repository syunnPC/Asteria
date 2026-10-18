// SPDX-License-Identifier: Apache-2.0
#include "Boot/Acpi/Ivrs.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace
{
void Check(const bool valid) noexcept
{
    if (!valid)
    {
        std::abort();
    }
}

void Decode(const std::uint8_t* data, const std::size_t size) noexcept
{
    Asteria::Boot::Acpi::IvrsTopology first{};
    Asteria::Boot::Acpi::IvrsTopology second{};
    const bool accepted = Asteria::Boot::Acpi::ParseIvrs(data, size, first);
    Check(accepted == Asteria::Boot::Acpi::ParseIvrs(data, size, second));
    Check(std::memcmp(&first, &second, sizeof(first)) == 0);
    if (!accepted)
    {
        Check(first.UnitCount == 0U && first.DeviceCount == 0U && first.MemoryCount == 0U);
    }
    for (std::size_t index{0U}; (index < first.DeviceCount) && (index < 64U); ++index)
    {
        Check(first.Devices[index].First <= first.Devices[index].Last);
    }
    for (std::size_t index{0U}; (index < first.MemoryCount) && (index < 8U); ++index)
    {
        Check(first.Memory[index].Base <= first.Memory[index].Limit);
        Check((first.Memory[index].Base & 4095U) == 0U && (first.Memory[index].Limit & 4095U) == 4095U);
        Check(first.Memory[index].First <= first.Memory[index].Last);
    }
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size) noexcept
{
    Decode(data, size);
    if ((size >= 48U) && (size <= 4096U))
    {
        std::uint8_t repaired[4096U]{};
        std::memcpy(repaired, data, size);
        std::memcpy(repaired, "IVRS", 4U);
        for (std::size_t index{0U}; index < 4U; ++index)
        {
            repaired[4U + index] = static_cast<std::uint8_t>(size >> (index * 8U));
        }
        repaired[8U] = 2U;
        repaired[9U] = 0U;
        std::uint8_t sum{0U};
        for (std::size_t index{0U}; index < size; ++index)
        {
            sum = static_cast<std::uint8_t>(sum + repaired[index]);
        }
        repaired[9U] = static_cast<std::uint8_t>(0U - sum);
        Decode(repaired, size);
    }
    return 0;
}

extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t*, std::size_t)) noexcept
{
    std::uint8_t bytes[116U]{};
    bytes[48U] = 0x40U;
    bytes[50U] = 68U;
    bytes[56U] = 0U;
    bytes[57U] = 0U;
    bytes[58U] = 0xD8U;
    bytes[59U] = 0xFEU;
    bytes[88U] = 3U;
    bytes[89U] = 0x10U;
    bytes[92U] = 4U;
    bytes[93U] = 0x20U;
    bytes[96U] = 0x42U;
    bytes[97U] = 0x30U;
    bytes[101U] = 0x40U;
    bytes[104U] = 0x48U;
    bytes[111U] = 1U;
    bytes[112U] = 2U;
    emit(bytes, sizeof(bytes));
    emit(bytes, 0U);
}
