// SPDX-License-Identifier: Apache-2.0
#include "Kernel/Device/TrustedDmaPolicy.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Boot::BootTrustDriver;
using Boot::BootTrustEntry;
using Boot::BootTrustPolicy;
using Boot::BootTrustVerdict;
using Kernel::Device::DecideTrustedDma;
using Kernel::Device::IommuAvailability;
using Kernel::Device::PciFunctionAddress;
using Kernel::Device::TrustedDmaDecision;
using Kernel::Device::TrustedDmaFunction;
using Kernel::Device::TrustedDmaOutcome;

// The q35 AHCI controller and xHCI the VM cases trust.
constexpr TrustedDmaFunction Ahci{.Address = PciFunctionAddress{0U, 0U, 0x1FU, 2U},
                                  .Vendor = 0x8086U,
                                  .Product = 0x2922U,
                                  .Driver = BootTrustDriver::Ahci,
                                  .Reserved = 0U};
constexpr TrustedDmaFunction Xhci{.Address = PciFunctionAddress{0U, 0U, 8U, 0U},
                                  .Vendor = 0x1B36U,
                                  .Product = 0x000DU,
                                  .Driver = BootTrustDriver::Xhci,
                                  .Reserved = 0U};

[[nodiscard]] constexpr BootTrustEntry Entry(const Foundation::UInt8 selector, const TrustedDmaFunction& function,
                                             const BootTrustDriver driver,
                                             const BootTrustVerdict verdict = BootTrustVerdict::Verified) noexcept
{
    return BootTrustEntry{.Segment = function.Address.GetSegment(),
                          .Bus = function.Address.GetBus(),
                          .Device = function.Address.GetDevice(),
                          .Function = function.Address.GetFunction(),
                          .Selector = selector,
                          .Vendor = function.Vendor,
                          .Product = function.Product,
                          .Driver = driver,
                          .Verdict = verdict,
                          .Module = 0U,
                          .Reserved = {},
                          .Digest = {}};
}

[[nodiscard]] bool Is(const TrustedDmaDecision decision, const TrustedDmaOutcome outcome,
                      const Foundation::UInt8 entry) noexcept
{
    return (decision.Outcome == outcome) && (decision.Entry == entry);
}

// The decision for every pairing of an entry (absent, applicable) and an
// IOMMU (absent, refused, working): only an entry where no IOMMU works
// applies; without one nothing changes, whatever the IOMMU.
void TestDecisionTable(TestContext& runner) noexcept
{
    constexpr IommuAvailability availabilities[]{IommuAvailability::Absent, IommuAvailability::Refused,
                                                 IommuAvailability::Working};
    BootTrustPolicy none{};
    BootTrustPolicy one{};
    one.Count = 1U;
    one.Entries[0U] = Entry(Boot::BootTrustSelectsAddress, Ahci, BootTrustDriver::Ahci);
    for (const IommuAvailability availability : availabilities)
    {
        const bool works = availability == IommuAvailability::Working;
        ASTERIA_EXPECT(runner,
                       works ? "an IOMMU that works: confined as today, the entry unused"
                             : "no usable IOMMU: only an entry applies",
                       Is(DecideTrustedDma(none, Ahci, availability), TrustedDmaOutcome::NoEntry, 0U),
                       Is(DecideTrustedDma(one, Ahci, availability),
                          works ? TrustedDmaOutcome::IommuConfines : TrustedDmaOutcome::Applied, 1U),
                       Is(DecideTrustedDma(one, Xhci, availability), TrustedDmaOutcome::NoEntry, 0U));
    }
}

// Which functions an entry names, and why a matching entry does not apply.
void TestMatchingAndRefusals(TestContext& runner) noexcept
{
    TrustedDmaFunction moved{Ahci};
    moved.Address = PciFunctionAddress{0U, 0U, 0x1FU, 3U};
    TrustedDmaFunction other{Ahci};
    other.Product = 0x2923U;
    const BootTrustEntry address = Entry(Boot::BootTrustSelectsAddress, Ahci, BootTrustDriver::Ahci);
    const BootTrustEntry identifier = Entry(Boot::BootTrustSelectsIdentifier, Ahci, BootTrustDriver::Ahci);
    const BootTrustEntry both =
        Entry(Boot::BootTrustSelectsAddress | Boot::BootTrustSelectsIdentifier, Ahci, BootTrustDriver::Ahci);
    BootTrustEntry neither{both};
    neither.Selector = 0U;
    ASTERIA_EXPECT(runner, "an entry names a function by address, by identifier, or by both",
                   Kernel::Device::TrustEntryMatches(address, Ahci), !Kernel::Device::TrustEntryMatches(address, moved),
                   Kernel::Device::TrustEntryMatches(address, other),
                   Kernel::Device::TrustEntryMatches(identifier, moved),
                   !Kernel::Device::TrustEntryMatches(identifier, other), Kernel::Device::TrustEntryMatches(both, Ahci),
                   !Kernel::Device::TrustEntryMatches(both, moved), !Kernel::Device::TrustEntryMatches(both, other),
                   !Kernel::Device::TrustEntryMatches(neither, Ahci));
    BootTrustPolicy policy{};
    policy.Count = 2U;
    policy.Entries[0U] = Entry(Boot::BootTrustSelectsAddress, Xhci, BootTrustDriver::Xhci);
    policy.Entries[1U] = Entry(Boot::BootTrustSelectsIdentifier, Ahci, BootTrustDriver::Xhci);
    ASTERIA_EXPECT(
        runner, "the entry's number; another driver than the function's is a mismatch",
        Is(DecideTrustedDma(policy, Xhci, IommuAvailability::Absent), TrustedDmaOutcome::Applied, 1U),
        Is(DecideTrustedDma(policy, Ahci, IommuAvailability::Absent), TrustedDmaOutcome::DriverMismatch, 2U),
        Is(DecideTrustedDma(policy, Ahci, IommuAvailability::Working), TrustedDmaOutcome::DriverMismatch, 2U));
    TrustedDmaFunction driverless{Xhci};
    driverless.Driver = BootTrustDriver::None;
    policy.Entries[1U] =
        Entry(Boot::BootTrustSelectsIdentifier, Ahci, BootTrustDriver::Ahci, BootTrustVerdict::DigestMismatch);
    BootTrustPolicy unchecked{};
    unchecked.Count = 1U;
    unchecked.Entries[0U] =
        Entry(Boot::BootTrustSelectsAddress, Ahci, BootTrustDriver::Ahci, BootTrustVerdict::Unchecked);
    ASTERIA_EXPECT(
        runner, "only a verified digest applies; a function without a driver never does",
        Is(DecideTrustedDma(policy, Ahci, IommuAvailability::Refused), TrustedDmaOutcome::DigestMismatch, 2U),
        Is(DecideTrustedDma(unchecked, Ahci, IommuAvailability::Absent), TrustedDmaOutcome::DigestMismatch, 1U),
        Is(DecideTrustedDma(policy, driverless, IommuAvailability::Absent), TrustedDmaOutcome::DriverMismatch, 1U));
    policy.Entries[1U] = Entry(Boot::BootTrustSelectsIdentifier, Xhci, BootTrustDriver::Xhci);
    BootTrustPolicy beyond{policy};
    beyond.Count = 1U;
    BootTrustPolicy oversized{policy};
    oversized.Count = Boot::BootTrustMaximumEntries + 5U;
    ASTERIA_EXPECT(runner, "a function two entries name is ambiguous; entries beyond the count do not exist",
                   Is(DecideTrustedDma(policy, Xhci, IommuAvailability::Absent), TrustedDmaOutcome::Ambiguous, 0U),
                   Is(DecideTrustedDma(beyond, Xhci, IommuAvailability::Absent), TrustedDmaOutcome::Applied, 1U),
                   Is(DecideTrustedDma(oversized, Xhci, IommuAvailability::Absent), TrustedDmaOutcome::Ambiguous, 0U));
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"TrustedDmaPolicyTests"};
    Asteria::Test::TestDecisionTable(runner);
    Asteria::Test::TestMatchingAndRefusals(runner);
    return runner.Finish();
}
