// SPDX-License-Identifier: Apache-2.0
#include "Platform/Amd64/Time/Amd64TscClock.hpp"
#include "Platform/Amd64/Interrupt/Amd64LocalApic.hpp"

#include "Support/TestContext.hpp"

#include <cstring>

namespace
{
using Asteria::Foundation::UInt32;
using Asteria::Foundation::UInt64;
using TscCpuIdResult = Asteria::Platform::Amd64::Time::TscCpuIdResult;
// CPUID and MSR values as the Microsoft Hypervisor TLFS 6.0b states them:
// "Micr" "osof" "t Hv" at 0x40000000, "Hv#1" at 0x40000001, partition
// privilege bit 11 (AccessFrequencyRegs, section 4.2.2) in 0x40000003 EAX
// and "determining timer frequencies" in its EDX bit 8; the TSC frequency in
// Hz in MSR 40000022H (Appendix C). Any other leaf or MSR is unexpected.
class FrequencyRegisters final : public Asteria::Platform::Amd64::Time::ITscFrequencyRegisters
{
public:
    TscCpuIdResult CpuId(const UInt32 leaf) noexcept override
    {
        switch (leaf)
        {
        case 0U:
            return {BasicMaximum, 0U, 0U, 0U};
        case 1U:
            return {0U, 0U, HypervisorPresent ? 0x8000'0000U : 0U, 0U};
        case 0x15U:
            ++RatioReads;
            return Ratio;
        case 0x4000'0000U:
            return {HypervisorMaximum, Microsoft ? 0x7263'694DU : 0x7263'694EU, 0x666F'736FU, 0x7648'2074U};
        case 0x4000'0001U:
            return {Interface ? 0x3123'7648U : 0x3023'7648U, 0U, 0U, 0U};
        case 0x4000'0003U:
            return {Privilege ? 0x800U : 0x7FFU, 0xFFFF'FFFFU, 0U, Available ? 0x100U : 0xFFFF'FEFFU};
        default:
            Unexpected = true;
            return {};
        }
    }
    UInt64 ReadFrequencyMsr(const UInt32 number) noexcept override
    {
        ++MsrReads;
        Unexpected = Unexpected || number != 0x4000'0022U;
        return Frequency;
    }
    UInt32 BasicMaximum{1U};
    UInt32 HypervisorMaximum{0x4000'0005U};
    TscCpuIdResult Ratio{1U, 125U, 24'000'000U, 0U};
    UInt64 Frequency{2'400'000'000ULL};
    bool HypervisorPresent{true};
    bool Microsoft{true};
    bool Interface{true};
    bool Privilege{true};
    bool Available{true};
    bool Unexpected{false};
    UInt32 MsrReads{0U};
    UInt32 RatioReads{0U};
};

void TestFrequencySources(Asteria::TestSupport::TestContext& context) noexcept
{
    using Asteria::Platform::Amd64::Time::Amd64TscAttempts;
    using Asteria::Platform::Amd64::Time::Amd64TscSource;
    using Asteria::Platform::Amd64::Time::DiscoverTscFrequency;
    using Asteria::Platform::Amd64::Time::TscSourceName;
    constexpr UInt32 Ratio{1U << 1U};
    constexpr UInt32 Pit{1U << 5U};
    constexpr UInt32 Synthetic{1U << 7U};
    // Each of the seven gates (leaf 1 present, hypervisor bit, leaf bound,
    // vendor, interface, privilege, availability) is necessary: the MSR is
    // read only when all hold. Other sources' attempt bits are preserved.
    for (UInt32 gates{0U}; gates < 128U; ++gates)
    {
        FrequencyRegisters registers;
        registers.BasicMaximum = (gates & 1U) != 0U ? 1U : 0U;
        registers.HypervisorPresent = (gates & 2U) != 0U;
        registers.HypervisorMaximum = (gates & 4U) != 0U ? 0x4000'0003U : 0x4000'0002U;
        registers.Microsoft = (gates & 8U) != 0U;
        registers.Interface = (gates & 16U) != 0U;
        registers.Privilege = (gates & 32U) != 0U;
        registers.Available = (gates & 64U) != 0U;
        Amd64TscAttempts attempts{Pit, Pit};
        const auto result = DiscoverTscFrequency(registers, attempts);
        const bool accepted = gates == 127U;
        if (!ASTERIA_EXPECT(context, "only a fully advertised Microsoft frequency MSR is read", !registers.Unexpected,
                            registers.MsrReads == (accepted ? 1U : 0U),
                            result.Source == (accepted ? Amd64TscSource::HyperVMsr : Amd64TscSource::None),
                            result.FrequencyHz == (accepted ? 2'400'000'000ULL : 0U),
                            attempts.Present == (Pit | (accepted ? Synthetic : 0U)),
                            attempts.Tried == attempts.Present))
        {
            return;
        }
    }
    constexpr UInt64 frequencies[]{0U, 999'999U, 1'000'000U, 65'536'000'000ULL, 65'536'000'001ULL};
    for (const UInt64 hz : frequencies)
    {
        FrequencyRegisters registers;
        registers.Frequency = hz;
        Amd64TscAttempts attempts{};
        const auto result = DiscoverTscFrequency(registers, attempts);
        const bool usable = hz >= 1'000'000U && hz <= 65'536'000'000ULL;
        ASTERIA_EXPECT(context, "the synthetic frequency has the same plausibility bounds as other sources",
                       registers.MsrReads == 1U, result.FrequencyHz == (usable ? hz : 0U),
                       attempts.Present == Synthetic, attempts.Tried == Synthetic);
    }
    FrequencyRegisters both;
    both.BasicMaximum = 0x16U;
    Amd64TscAttempts attempts{};
    const auto synthetic = DiscoverTscFrequency(both, attempts);
    ASTERIA_EXPECT(context, "in an advertising Microsoft partition the hypervisor's TSC frequency precedes CPUID 15H",
                   synthetic.Source == Amd64TscSource::HyperVMsr, synthetic.FrequencyHz == 2'400'000'000ULL,
                   both.MsrReads == 1U, both.RatioReads == 0U, !both.Unexpected,
                   attempts.Present == (Ratio | Synthetic), attempts.Tried == Synthetic,
                   std::strcmp(TscSourceName(synthetic.Source), "HYPER-V-MSR") == 0);
    both.Frequency = 0U;
    attempts = {};
    const auto crystal = DiscoverTscFrequency(both, attempts);
    ASTERIA_EXPECT(context, "an implausible synthetic frequency falls through to the crystal ratio",
                   crystal.Source == Amd64TscSource::CpuIdRatio, crystal.FrequencyHz == 3'000'000'000ULL,
                   both.RatioReads == 1U, attempts.Present == (Ratio | Synthetic),
                   attempts.Tried == (Ratio | Synthetic), std::strcmp(TscSourceName(crystal.Source), "CPUID-15") == 0);
    FrequencyRegisters bare;
    bare.BasicMaximum = 0x16U;
    bare.HypervisorPresent = false;
    attempts = {};
    const auto architectural = DiscoverTscFrequency(bare, attempts);
    ASTERIA_EXPECT(context, "without a hypervisor only CPUID 15H is consulted, never 16H",
                   architectural.Source == Amd64TscSource::CpuIdRatio, bare.MsrReads == 0U, !bare.Unexpected,
                   attempts.Present == Ratio, attempts.Tried == Ratio);
    bare.Ratio.Ecx = 0U;
    attempts = {};
    const auto incomplete = DiscoverTscFrequency(bare, attempts);
    ASTERIA_EXPECT(context, "an incomplete crystal ratio leaves calibration to the reference counters",
                   incomplete.Source == Amd64TscSource::None, incomplete.FrequencyHz == 0U, !bare.Unexpected,
                   attempts.Tried == Ratio, std::strcmp(TscSourceName(Amd64TscSource::Hypervisor), "CPUID-HV") == 0);
}

class Reference final : public Asteria::Platform::Amd64::Time::ITscCalibrationReference
{
public:
    Asteria::Foundation::UInt64 ReadTimestamp() noexcept override
    {
        m_after = !m_after;
        if (m_after)
        {
            const Asteria::Foundation::UInt64 hz =
                BestOfThree ? 3'200'000'000ULL - (Reads / 6U) * 100'000'000ULL : 3'000'000'000ULL;
            m_ticks += hz / 100U;
            return m_ticks;
        }
        return m_ticks + (Wide || Reads <= NoisyReads ? 50'001U : 100U);
    }
    Asteria::Foundation::UInt32 ReadCounter() noexcept override
    {
        ++Reads;
        if (!Stuck)
        {
            Counter += 10'000U;
        }
        return Counter;
    }
    Asteria::Foundation::UInt32 Counter{0U};
    Asteria::Foundation::UInt32 Reads{0U};
    Asteria::Foundation::UInt32 NoisyReads{0U};
    bool Stuck{false};
    bool Wide{false};
    bool BestOfThree{false};

private:
    bool m_after{false};
    Asteria::Foundation::UInt64 m_ticks{0U};
};
} // namespace

int main()
{
    Asteria::TestSupport::TestContext context{"Amd64TscClockTests"};
    TestFrequencySources(context);
    using Asteria::Platform::Amd64::Time::TscRatioFrequency;
    ASTERIA_EXPECT(context, "crystal ratio uses full-width arithmetic",
                   TscRatioFrequency(2U, 250U, 24'000'000U) == 3'000'000'000ULL,
                   TscRatioFrequency(1U, 200U, 25'000'000U) == 5'000'000'000ULL);
    ASTERIA_EXPECT(context, "missing or implausible ratios fall through", TscRatioFrequency(0U, 1U, 1U) == 0U,
                   TscRatioFrequency(1U, 0U, 1U) == 0U, TscRatioFrequency(1U, 1U, 0U) == 0U,
                   TscRatioFrequency(1U, 0xFFFF'FFFFU, 0xFFFF'FFFFU) == 0U);
    using Asteria::Platform::Amd64::Interrupt::Amd64LocalApic;
    ASTERIA_EXPECT(context, "APIC register MSR translation", Amd64LocalApic::MsrFor(0x20U) == 0x802U,
                   Amd64LocalApic::MsrFor(0x300U) == 0x830U, Amd64LocalApic::MsrFor(0x390U) == 0x839U);
    alignas(4096U) Asteria::Foundation::UInt32 registers[1024U]{};
    Amd64LocalApic apic;
    ASTERIA_EXPECT(context, "MMIO backend refuses xAPIC broadcast destination",
                   apic.Initialize(reinterpret_cast<Asteria::Foundation::UIntPointer>(registers)),
                   !apic.Send(255U, 0x40U), apic.Send(254U, 0x40U), registers[0x310U / 4U] == 0xFE00'0000U);
    using Asteria::Platform::Amd64::Time::MeasureTscReference;
    constexpr Asteria::Foundation::UInt32 masks[]{0xFFFF'FFFFU, 0xFF'FFFFU};
    for (const Asteria::Foundation::UInt32 mask : masks)
    {
        Reference reference;
        reference.Counter = mask - 20'000U;
        ASTERIA_EXPECT(context, "50 ms reference windows handle wrap and retain exact frequency",
                       MeasureTscReference(reference, mask, 1'000'000U) == 3'000'000'000ULL, reference.Reads == 18U);
    }
    Reference noisy;
    noisy.NoisyReads = 2U;
    ASTERIA_EXPECT(context, "wide TSC brackets are retried",
                   MeasureTscReference(noisy, 0xFFFF'FFFFU, 1'000'000U) == 3'000'000'000ULL, noisy.Reads == 20U);
    Reference wide;
    wide.Wide = true;
    ASTERIA_EXPECT(context, "persistently wide references are abandoned after bounded retries",
                   MeasureTscReference(wide, 0xFFFF'FFFFU, 1'000'000U) == 0U, wide.Reads == 5U);
    Reference stuck;
    stuck.Stuck = true;
    ASTERIA_EXPECT(context, "a stuck counter is abandoned after ten thousand polls",
                   MeasureTscReference(stuck, 0xFFFF'FFFFU, 1'000'000U) == 0U, stuck.Reads == 10'001U);
    Reference best;
    best.BestOfThree = true;
    ASTERIA_EXPECT(context, "minimum of three measurements rejects upward delay bias",
                   MeasureTscReference(best, 0xFFFF'FFFFU, 1'000'000U) == 3'000'000'000ULL);
    return context.Finish();
}
