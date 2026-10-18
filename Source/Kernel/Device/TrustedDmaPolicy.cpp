// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Device/TrustedDmaPolicy.hpp"

namespace Asteria::Kernel::Device
{
bool TrustEntryMatches(const Boot::BootTrustEntry& entry, const TrustedDmaFunction& function) noexcept
{
    const PciFunctionAddress& address = function.Address;
    const bool byAddress = (entry.Selector & Boot::BootTrustSelectsAddress) != 0U;
    const bool byIdentifier = (entry.Selector & Boot::BootTrustSelectsIdentifier) != 0U;
    const bool addressMatches =
        !byAddress || ((entry.Segment == address.GetSegment()) && (entry.Bus == address.GetBus()) &&
                       (entry.Device == address.GetDevice()) && (entry.Function == address.GetFunction()));
    const bool identifierMatches =
        !byIdentifier || ((entry.Vendor == function.Vendor) && (entry.Product == function.Product));
    return (byAddress || byIdentifier) && addressMatches && identifierMatches;
}

TrustedDmaDecision DecideTrustedDma(const Boot::BootTrustPolicy& policy, const TrustedDmaFunction& function,
                                    const IommuAvailability availability) noexcept
{
    const Foundation::Size count = (policy.Count < Boot::BootTrustMaximumEntries)
                                       ? static_cast<Foundation::Size>(policy.Count)
                                       : Boot::BootTrustMaximumEntries;
    Foundation::Size found{count};
    for (Foundation::Size index{0U}; index < count; ++index)
    {
        if (!TrustEntryMatches(policy.Entries[index], function))
        {
            continue;
        }
        if (found != count)
        {
            return {TrustedDmaOutcome::Ambiguous, 0U};
        }
        found = index;
    }
    if (found == count)
    {
        return {TrustedDmaOutcome::NoEntry, 0U};
    }
    const Boot::BootTrustEntry& entry = policy.Entries[found];
    const auto number = static_cast<Foundation::UInt8>(found + 1U);
    if ((entry.Driver != function.Driver) || (function.Driver == Boot::BootTrustDriver::None))
    {
        return {TrustedDmaOutcome::DriverMismatch, number};
    }
    if (entry.Verdict != Boot::BootTrustVerdict::Verified)
    {
        return {TrustedDmaOutcome::DigestMismatch, number};
    }
    switch (availability)
    {
    case IommuAvailability::Working:
        return {TrustedDmaOutcome::IommuConfines, number};
    case IommuAvailability::Absent:
    case IommuAvailability::Refused:
        return {TrustedDmaOutcome::Applied, number};
    }
    return {TrustedDmaOutcome::IommuConfines, number};
}
} // namespace Asteria::Kernel::Device
