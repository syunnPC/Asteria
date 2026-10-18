// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Time/Amd64TscClock.hpp"

#include "Platform/Synchronization/ProcessorContext.hpp"

namespace Asteria::Platform::Amd64::Time
{
namespace
{
constinit Foundation::UInt64 g_frequencyHz{0U};
constinit Amd64TscAttempts g_attempts{};
constexpr Foundation::Size MaximumPolls{10'000'000U};
constexpr Foundation::Size MaximumUnchangedPolls{10'000U};
constexpr Foundation::UInt64 MaximumReadCycles{50'000U};
[[nodiscard]] bool ValidFrequency(const Foundation::UInt64 hz) noexcept
{
    return hz >= 1'000'000U && hz <= 65'536'000'000ULL;
}
[[nodiscard]] constexpr Foundation::UInt32 SourceBit(const Amd64TscSource source) noexcept
{
    return 1U << static_cast<Foundation::UInt32>(source);
}
struct ReferenceSample
{
    Foundation::UInt64 Tsc;
    Foundation::UInt32 Counter;
};
[[nodiscard]] bool Sample(ITscCalibrationReference& reference, ReferenceSample& sample) noexcept
{
    for (Foundation::Size retry{0U}; retry < 5U; ++retry)
    {
        const Foundation::UInt64 before = reference.ReadTimestamp();
        const Foundation::UInt32 counter = reference.ReadCounter();
        const Foundation::UInt64 after = reference.ReadTimestamp();
        if (after >= before && after - before <= MaximumReadCycles)
        {
            sample = {before + (after - before) / 2U, counter};
            return true;
        }
    }
    return false;
}
[[nodiscard]] Foundation::UInt64 MeasureRun(ITscCalibrationReference& reference, const Foundation::UInt32 mask,
                                            const Foundation::UInt64 counterHz) noexcept
{
    if (counterHz < 20U || counterHz / 20U > mask)
    {
        return 0U;
    }
    const auto goal = static_cast<Foundation::UInt32>(counterHz / 20U);
    ReferenceSample start{};
    if (!Sample(reference, start))
    {
        return 0U;
    }
    Foundation::UInt32 previous = start.Counter & mask;
    Foundation::Size unchanged{0U};
    for (Foundation::Size poll{0U}; poll < MaximumPolls; ++poll)
    {
        ReferenceSample end{};
        if (!Sample(reference, end))
        {
            return 0U;
        }
        const Foundation::UInt32 current = end.Counter & mask;
        unchanged = current == previous ? unchanged + 1U : 0U;
        if (unchanged >= MaximumUnchangedPolls)
        {
            return 0U;
        }
        previous = current;
        // Intentional modulo subtraction handles a 24/32-bit counter wrap.
        const Foundation::UInt32 elapsed = (current - start.Counter) & mask;
        if (elapsed >= goal)
        {
            if (end.Tsc <= start.Tsc ||
                static_cast<Foundation::UInt64>(elapsed) > static_cast<Foundation::UInt64>(goal) * 10U)
            {
                return 0U;
            }
            const Foundation::UInt64 ticks = end.Tsc - start.Tsc;
            if (ticks > ~Foundation::UInt64{0U} / counterHz)
            {
                return 0U;
            }
            const Foundation::UInt64 hz = ticks * counterHz / elapsed;
            return ValidFrequency(hz) ? hz : 0U;
        }
    }
    return 0U;
}
#if defined(ASTERIA_TARGET_AMD64)
class FrequencyRegisters final : public ITscFrequencyRegisters
{
public:
    TscCpuIdResult CpuId(const Foundation::UInt32 leaf) noexcept override
    {
        TscCpuIdResult result{};
        __asm__ __volatile__("cpuid"
                             : "=a"(result.Eax), "=b"(result.Ebx), "=c"(result.Ecx), "=d"(result.Edx)
                             : "a"(leaf), "c"(0U));
        return result;
    }
    Foundation::UInt64 ReadFrequencyMsr(const Foundation::UInt32 number) noexcept override
    {
        Foundation::UInt32 low{0U};
        Foundation::UInt32 high{0U};
        __asm__ __volatile__("rdmsr" : "=a"(low), "=d"(high) : "c"(number));
        return (static_cast<Foundation::UInt64>(high) << 32U) | low;
    }
};

class InterruptGuard final
{
public:
    InterruptGuard() noexcept
    {
        __asm__ __volatile__("pushfq; popq %0; cli" : "=r"(m_flags) : : "memory");
    }
    ~InterruptGuard() noexcept
    {
        if ((m_flags & 0x200U) != 0U)
        {
            __asm__ __volatile__("sti" : : : "memory");
        }
    }
    InterruptGuard(const InterruptGuard&) = delete;
    InterruptGuard& operator=(const InterruptGuard&) = delete;

private:
    Foundation::UInt64 m_flags{0U};
};
void Out(const Foundation::UInt16 port, const Foundation::UInt8 value) noexcept
{
    __asm__ __volatile__("outb %0,%1" : : "a"(value), "Nd"(port) : "memory");
}
[[nodiscard]] Foundation::UInt8 In(const Foundation::UInt16 port) noexcept
{
    Foundation::UInt8 value{0U};
    __asm__ __volatile__("inb %1,%0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}
class CounterReference final : public ITscCalibrationReference
{
public:
    CounterReference(const Foundation::UInt64 address, const Foundation::UInt8 space) noexcept
        : m_address{address}, m_space{space}
    {
    }
    Foundation::UInt64 ReadTimestamp() noexcept override
    {
        return ReadTsc();
    }
    Foundation::UInt32 ReadCounter() noexcept override
    {
        if (m_space == 1U)
        {
            Foundation::UInt32 value{0U};
            const auto port = static_cast<Foundation::UInt16>(m_address);
            __asm__ __volatile__("inl %1,%0" : "=a"(value) : "Nd"(port) : "memory");
            return value;
        }
        // Firmware identity mapping, naturally aligned 32-bit register.
        return *reinterpret_cast<volatile Foundation::UInt32*>(static_cast<Foundation::UIntPointer>(m_address));
    }

private:
    Foundation::UInt64 m_address;
    Foundation::UInt8 m_space;
};
class PitReference final : public ITscCalibrationReference
{
public:
    Foundation::UInt64 ReadTimestamp() noexcept override
    {
        return ReadTsc();
    }
    Foundation::UInt32 ReadCounter() noexcept override
    {
        Out(0x43U, 0x80U); // Channel 2 counter latch; read low then high.
        const Foundation::UInt32 low = In(0x42U);
        const Foundation::UInt32 high = In(0x42U);
        return (0x1'0000U - ((high << 8U) | low)) & 0xFFFFU;
    }
};
[[nodiscard]] Foundation::UInt64 MeasurePit() noexcept
{
    // Mode 2 gives a continuously advancing reference for all three windows.
    const Foundation::UInt8 saved = In(0x61U);
    Out(0x61U, static_cast<Foundation::UInt8>(saved & 0xFCU));
    Out(0x43U, 0xB4U);
    Out(0x42U, 0U);
    Out(0x42U, 0U); // 65,536 clocks, a 54.9 ms modulus.
    Out(0x61U, static_cast<Foundation::UInt8>((saved & 0xFCU) | 1U));
    PitReference reference;
    const Foundation::UInt64 best = MeasureTscReference(reference, 0xFFFFU, 1'193'182U);
    Out(0x61U, saved);
    return best;
}
#endif
} // Namespace

Foundation::UInt64 MeasureTscReference(ITscCalibrationReference& reference, const Foundation::UInt32 mask,
                                       const Foundation::UInt64 counterHz) noexcept
{
    Foundation::UInt64 best{0U};
    for (Foundation::Size run{0U}; run < 3U; ++run)
    {
        const Foundation::UInt64 hz = MeasureRun(reference, mask, counterHz);
        if (hz == 0U)
        {
            return 0U;
        }
        if (best == 0U || hz < best)
        {
            best = hz;
        }
    }
    return best;
}

Amd64TscAttempts GetTscCalibrationAttempts() noexcept
{
    return g_attempts;
}

Amd64TscCalibration DiscoverTscFrequency(ITscFrequencyRegisters& registers, Amd64TscAttempts& attempts) noexcept
{
    const TscCpuIdResult basic = registers.CpuId(0U);
    const bool ratioPresent = basic.Eax >= 0x15U;
    bool syntheticPresent{false};
    // Microsoft Hypervisor Top-Level Functional Specification 6.0b:
    // section 2.2 (CPUID.01H:ECX[31], hypervisor present), section 2.4
    // "Hypervisor CPUID Leaf Range - 0x40000000" (maximum leaf, "Microsoft Hv"
    // signature), "Hypervisor Vendor-Neutral Interface Identification -
    // 0x40000001" ("Hv#1", which section 2.3 makes the basis for interpreting
    // leaves 0x40000002 and up) and "Hypervisor Feature Identification -
    // 0x40000003": EAX is the low half of the partition privilege mask, whose
    // bit 11 is AccessFrequencyRegs (section 4.2.2), and EDX bit 8 reports
    // that timer frequencies can be determined. Every gate must hold before
    // the synthetic MSR is read.
    if (basic.Eax >= 1U && (registers.CpuId(1U).Ecx & 0x8000'0000U) != 0U)
    {
        const TscCpuIdResult vendor = registers.CpuId(0x4000'0000U);
        if (vendor.Eax >= 0x4000'0003U && vendor.Ebx == 0x7263'694DU && vendor.Ecx == 0x666F'736FU &&
            vendor.Edx == 0x7648'2074U && registers.CpuId(0x4000'0001U).Eax == 0x3123'7648U)
        {
            const TscCpuIdResult features = registers.CpuId(0x4000'0003U);
            syntheticPresent = (features.Eax & (1U << 11U)) != 0U && (features.Edx & (1U << 8U)) != 0U;
        }
    }
    if (syntheticPresent)
    {
        attempts.Present |= SourceBit(Amd64TscSource::HyperVMsr);
    }
    if (ratioPresent)
    {
        attempts.Present |= SourceBit(Amd64TscSource::CpuIdRatio);
    }
    if (syntheticPresent)
    {
        // TLFS 6.0b Appendix C (section 19), Hypervisor Synthetic MSRs:
        // 40000022H HV_X64_MSR_TSC_FREQUENCY is read-only under
        // AccessFrequencyRegs and specifies the frequency, in Hz, of the TSC
        // as reported by the hypervisor. That hypervisor presents the
        // partition's TSC, so its report is taken before the architectural
        // Leaf. Hyper-V generation 2 firmware can omit CPUID 15H, the PM timer and HPET, and expose
        // a PIT that does not advance. In that configuration this MSR provides the usable frequency
        // source.
        attempts.Tried |= SourceBit(Amd64TscSource::HyperVMsr);
        const Foundation::UInt64 hz = registers.ReadFrequencyMsr(0x4000'0022U);
        if (ValidFrequency(hz))
        {
            return {hz, Amd64TscSource::HyperVMsr};
        }
    }
    // Intel SDM Vol. 3B section 20.7.3 and Vol. 2 CPUID leaf 15H: the TSC
    // frequency is the crystal frequency times the TSC/crystal ratio. Leaf 16H
    // reports nominal core frequency, which the SDM does not equate with the
    // TSC, so it is not a source. Hypervisor leaf 0x40000010 is also excluded.
    if (ratioPresent)
    {
        attempts.Tried |= SourceBit(Amd64TscSource::CpuIdRatio);
        const TscCpuIdResult ratio = registers.CpuId(0x15U);
        const Foundation::UInt64 hz = TscRatioFrequency(ratio.Eax, ratio.Ebx, ratio.Ecx);
        if (hz != 0U)
        {
            return {hz, Amd64TscSource::CpuIdRatio};
        }
    }
    return {0U, Amd64TscSource::None};
}

Foundation::UInt64 ReadTsc() noexcept
{
#if defined(ASTERIA_TARGET_AMD64)
    Foundation::UInt32 low{0U};
    Foundation::UInt32 high{0U};
    __asm__ __volatile__("lfence; rdtsc; lfence" : "=a"(low), "=d"(high) : : "memory");
    return (static_cast<Foundation::UInt64>(high) << 32U) | low;
#else
    return 1U;
#endif
}

Amd64TscCalibration CalibrateTsc(const ::Asteria::Boot::Acpi::AcpiTopology& topology) noexcept
{
    g_attempts = {};
#if defined(ASTERIA_TARGET_AMD64)
    g_attempts.Present = SourceBit(Amd64TscSource::Pit);
    if (topology.PmTimerAddress != 0U)
    {
        g_attempts.Present |= SourceBit(Amd64TscSource::AcpiPm);
    }
    if (topology.HpetAddress != 0U)
    {
        g_attempts.Present |= SourceBit(Amd64TscSource::Hpet);
    }
    FrequencyRegisters frequencyRegisters;
    const Amd64TscCalibration discovered = DiscoverTscFrequency(frequencyRegisters, g_attempts);
    if (discovered.FrequencyHz != 0U)
    {
        return discovered;
    }
    const InterruptGuard interrupts;
    if (topology.PmTimerAddress != 0U)
    {
        g_attempts.Tried |= SourceBit(Amd64TscSource::AcpiPm);
        CounterReference reference{topology.PmTimerAddress, topology.PmTimerSpace};
        const Foundation::UInt32 mask = topology.PmTimerBits == 32U ? 0xFFFF'FFFFU : 0xFF'FFFFU;
        const Foundation::UInt64 hz = MeasureTscReference(reference, mask, 3'579'545U);
        if (hz != 0U)
        {
            return {hz, Amd64TscSource::AcpiPm};
        }
    }
    if (topology.HpetAddress != 0U)
    {
        g_attempts.Tried |= SourceBit(Amd64TscSource::Hpet);
        // HPET registers use 32-bit accesses, including capabilities high
        // (femtosecond period), configuration low, and main counter low.
        auto* const registers =
            reinterpret_cast<volatile Foundation::UInt32*>(static_cast<Foundation::UIntPointer>(topology.HpetAddress));
        const Foundation::UInt32 capabilities = registers[0U];
        const Foundation::UInt64 period = registers[1U];
        if (period != 0U && period <= 100'000'000U && (capabilities & 0xFFU) != 0U)
        {
            const Foundation::UInt32 saved = registers[0x10U / 4U];
            registers[0x10U / 4U] = saved | 1U;
            CounterReference reference{topology.HpetAddress + 0xF0U, 0U};
            const Foundation::UInt64 hz =
                MeasureTscReference(reference, 0xFFFF'FFFFU, 1'000'000'000'000'000ULL / period);
            registers[0x10U / 4U] = saved;
            if (hz != 0U)
            {
                return {hz, Amd64TscSource::Hpet};
            }
        }
    }
    g_attempts.Tried |= SourceBit(Amd64TscSource::Pit);
    const Foundation::UInt64 hz = MeasurePit();
    if (hz != 0U)
    {
        return {hz, Amd64TscSource::Pit};
    }
#else
    static_cast<void>(topology);
#endif
    return {0U, Amd64TscSource::None};
}

void SetTscFrequency(const Foundation::UInt64 frequencyHz) noexcept
{
    g_frequencyHz = frequencyHz <= 65'536'000'000ULL ? frequencyHz : 0U;
}
Foundation::UInt64 GetTscFrequency() noexcept
{
    return g_frequencyHz;
}

bool DelayTscMicroseconds(const Foundation::UInt32 microseconds) noexcept
{
    if (g_frequencyHz == 0U || microseconds == 0U || microseconds > 50'000U)
    {
        return false;
    }
#if defined(ASTERIA_TARGET_AMD64)
    const Foundation::UInt64 ticks = (g_frequencyHz * microseconds + 999'999U) / 1'000'000U;
    const Foundation::UInt64 start = ReadTsc();
    for (Foundation::Size poll{0U}; poll < 100'000'000U; ++poll)
    {
        if (ReadTsc() - start >= ticks)
        {
            return true;
        }
        Platform::Synchronization::ProcessorWaitProgress();
    }
#endif
    return false;
}

} // Namespace Asteria::Platform::Amd64::Time
