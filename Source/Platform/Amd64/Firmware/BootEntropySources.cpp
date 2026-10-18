// SPDX-License-Identifier: Apache-2.0

#include "Platform/Firmware/BootEntropySources.hpp"

namespace Asteria::Platform::Firmware
{
namespace
{
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::UInt8;

constexpr UInt32 RdrandFeatureBit{1U << 30U}; // CPUID.01H:ECX
constexpr UInt32 RdseedFeatureBit{1U << 18U}; // CPUID.(EAX=07H,ECX=0):EBX
constexpr UInt32 StructuredFeatureLeaf{7U};
// RDSEED may run dry under load; RDRAND failing ten times in a row indicates
// a hardware fault (Intel DRNG software implementation guide).
constexpr Size RdseedAttempts{64U};
constexpr Size RdrandAttempts{10U};

struct CpuidResult
{
    UInt32 Eax;
    UInt32 Ebx;
    UInt32 Ecx;
    UInt32 Edx;
};

[[nodiscard]] CpuidResult Cpuid(const UInt32 leaf, const UInt32 subleaf) noexcept
{
    CpuidResult result{};
    __asm__ volatile("cpuid"
                     : "=a"(result.Eax), "=b"(result.Ebx), "=c"(result.Ecx), "=d"(result.Edx)
                     : "a"(leaf), "c"(subleaf));
    return result;
}

[[nodiscard]] bool TryRdseed(UInt64& value) noexcept
{
    UInt8 ready{0U};
    __asm__ volatile("rdseed %0\n\tsetc %1" : "=r"(value), "=qm"(ready) : : "cc");
    return ready != 0U;
}

[[nodiscard]] bool TryRdrand(UInt64& value) noexcept
{
    UInt8 ready{0U};
    __asm__ volatile("rdrand %0\n\tsetc %1" : "=r"(value), "=qm"(ready) : : "cc");
    return ready != 0U;
}

void Pause() noexcept
{
    __asm__ volatile("pause" : : : "memory");
}
} // namespace

UInt64 ReadCycleCounter() noexcept
{
    UInt32 low{0U};
    UInt32 high{0U};
    __asm__ volatile("lfence\n\trdtsc" : "=a"(low), "=d"(high) : : "memory");
    return (static_cast<UInt64>(high) << 32U) | low;
}

bool TryReadProcessorRandom(UInt64& value) noexcept
{
    if ((Cpuid(0U, 0U).Eax >= StructuredFeatureLeaf) &&
        ((Cpuid(StructuredFeatureLeaf, 0U).Ebx & RdseedFeatureBit) != 0U))
    {
        for (Size attempt{0U}; attempt < RdseedAttempts; ++attempt)
        {
            if (TryRdseed(value))
            {
                return true;
            }
            Pause();
        }
    }
    if ((Cpuid(1U, 0U).Ecx & RdrandFeatureBit) != 0U)
    {
        for (Size attempt{0U}; attempt < RdrandAttempts; ++attempt)
        {
            if (TryRdrand(value))
            {
                return true;
            }
        }
    }
    value = 0U;
    return false;
}
} // namespace Asteria::Platform::Firmware
