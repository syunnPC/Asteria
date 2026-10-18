// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_TIME_AMD64_TSC_CLOCK_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_TIME_AMD64_TSC_CLOCK_HPP_INCLUDED

#include "Boot/Acpi/AcpiTopology.hpp"

namespace Asteria::Platform::Amd64::Time
{
/// Values are evidence ABI. CpuIdBase and Hypervisor are no longer produced
/// and stay reserved so recorded evidence keeps its meaning.
enum class Amd64TscSource : Foundation::UInt32
{
    None,
    CpuIdRatio,
    CpuIdBase,
    AcpiPm,
    Hpet,
    Pit,
    Hypervisor,
    HyperVMsr,
};
struct Amd64TscCalibration
{
    Foundation::UInt64 FrequencyHz;
    Amd64TscSource Source;
};
struct Amd64TscAttempts
{
    Foundation::UInt32 Present;
    Foundation::UInt32 Tried;
};

struct TscCpuIdResult
{
    Foundation::UInt32 Eax;
    Foundation::UInt32 Ebx;
    Foundation::UInt32 Ecx;
    Foundation::UInt32 Edx;
};

/// Architectural/vendor discovery seam. ReadFrequencyMsr is called only
/// after the hypervisor-present bit, the leaf bound, the "Microsoft Hv" and
/// "Hv#1" signatures and both TLFS feature gates.
class ITscFrequencyRegisters
{
public:
    [[nodiscard]] virtual TscCpuIdResult CpuId(Foundation::UInt32 leaf) noexcept = 0;
    [[nodiscard]] virtual Foundation::UInt64 ReadFrequencyMsr(Foundation::UInt32 number) noexcept = 0;

protected:
    ~ITscFrequencyRegisters() noexcept = default;
};

/// Allocation-free discovery; preserves other sources' attempt bits.
/// The guarded Hyper-V frequency MSR precedes a complete CPUID 15H ratio.
[[nodiscard]] Amd64TscCalibration DiscoverTscFrequency(ITscFrequencyRegisters& registers,
                                                       Amd64TscAttempts& attempts) noexcept;
/// Bit positions match the source enumeration; PIT presence means a legacy
/// candidate, whose existence is established only by a successful measurement.
[[nodiscard]] Amd64TscAttempts GetTscCalibrationAttempts() noexcept;

/// Bounded reference adapter. Counter is an increasing modulo counter;
/// reads are serialized by the caller with interrupts and migration disabled.
class ITscCalibrationReference
{
public:
    [[nodiscard]] virtual Foundation::UInt64 ReadTimestamp() noexcept = 0;
    [[nodiscard]] virtual Foundation::UInt32 ReadCounter() noexcept = 0;

protected:
    /// Reference storage is caller-owned and is never deleted through this interface.
    ~ITscCalibrationReference() noexcept = default;
};
/// Three 50 ms measurements, selecting the minimum valid frequency to reject
/// upward delay bias. Each reference read is bracketed; wide reads are retried.
/// A counter stuck for 10,000 reads abandons the source. No allocation.
[[nodiscard]] Foundation::UInt64 MeasureTscReference(ITscCalibrationReference& reference, Foundation::UInt32 mask,
                                                     Foundation::UInt64 counterHz) noexcept;

/// BSP-only, before firmware exit under its identity map. Does not allocate.
/// Bounded polling rejects absent/stuck counters. HPET enable is restored;
/// PM timer is read-only; PIT channel 2 is the last resort.
[[nodiscard]] Amd64TscCalibration CalibrateTsc(const ::Asteria::Boot::Acpi::AcpiTopology& topology) noexcept;
[[nodiscard]] Foundation::UInt64 ReadTsc() noexcept;
/// Publish once before AP startup. Later calls use only TSC, on any CPU.
void SetTscFrequency(Foundation::UInt64 frequencyHz) noexcept;
[[nodiscard]] Foundation::UInt64 GetTscFrequency() noexcept;
[[nodiscard]] bool DelayTscMicroseconds(Foundation::UInt32 microseconds) noexcept;
[[nodiscard]] constexpr const char* TscSourceName(const Amd64TscSource source) noexcept
{
    switch (source)
    {
    case Amd64TscSource::None:
        return "NONE";
    case Amd64TscSource::CpuIdRatio:
        return "CPUID-15";
    case Amd64TscSource::CpuIdBase:
        return "CPUID-16";
    case Amd64TscSource::AcpiPm:
        return "ACPI-PM";
    case Amd64TscSource::Hpet:
        return "HPET";
    case Amd64TscSource::Pit:
        return "PIT";
    case Amd64TscSource::Hypervisor:
        return "CPUID-HV";
    case Amd64TscSource::HyperVMsr:
        return "HYPER-V-MSR";
    }
    return "NONE";
}
/// Exact crystal ratio, checked before multiplication; zero means unusable.
[[nodiscard]] constexpr Foundation::UInt64 TscRatioFrequency(const Foundation::UInt32 denominator,
                                                             const Foundation::UInt32 numerator,
                                                             const Foundation::UInt32 crystalHz) noexcept
{
    if (denominator == 0U || numerator == 0U || crystalHz == 0U)
    {
        return 0U;
    }
    const Foundation::UInt64 hz = static_cast<Foundation::UInt64>(crystalHz) * numerator / denominator;
    return hz >= 1'000'000U && hz <= 65'536'000'000ULL ? hz : 0U;
}
} // namespace Asteria::Platform::Amd64::Time
#endif
