// SPDX-License-Identifier: Apache-2.0

#include "Platform/Device/TrustedDmaRuntime.hpp"

#include "Abi/DeviceInventory.hpp"
#include "Kernel/Boot/BootInventory.hpp"
#include "Kernel/Device/PciFunction.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Platform/Firmware/Diagnostic.hpp"

namespace Asteria::Platform::Device
{
namespace
{
using Kernel::Device::IommuAvailability;
using Kernel::Device::TrustedDmaOutcome;

// A bounded diagnostic line; what does not fit is dropped, and the line keeps
// its terminator.
template <Foundation::Size TCapacity> class Line final
{
public:
    void Text(const char* text) noexcept
    {
        while ((text != nullptr) && (*text != '\0'))
        {
            Character(*text++);
        }
    }

    void Character(const char value) noexcept
    {
        if (m_length + 1U < TCapacity)
        {
            m_text[m_length++] = value;
        }
    }

    void Hex(const Foundation::UInt64 value, const Foundation::Size digits) noexcept
    {
        constexpr char Alphabet[]{"0123456789abcdef"};
        for (Foundation::Size index{digits}; (index != 0U) && (index <= 16U); --index)
        {
            Character(Alphabet[(value >> ((index - 1U) * 4U)) & 15U]);
        }
    }

    void Decimal(Foundation::UInt64 value) noexcept
    {
        char digits[20]{};
        Foundation::Size count{0U};
        do
        {
            digits[count++] = static_cast<char>('0' + static_cast<char>(value % 10U));
            value /= 10U;
        } while (value != 0U);
        while (count != 0U)
        {
            Character(digits[--count]);
        }
    }

    [[nodiscard]] const char* Get() const noexcept
    {
        return m_text;
    }

private:
    char m_text[TCapacity]{};
    Foundation::Size m_length{0U};
};

[[nodiscard]] Abi::InventoryTrustOutcome InventoryOutcome(const TrustedDmaOutcome outcome) noexcept
{
    switch (outcome)
    {
    case TrustedDmaOutcome::Applied:
        return Abi::InventoryTrustOutcome::Applied;
    case TrustedDmaOutcome::IommuConfines:
        return Abi::InventoryTrustOutcome::IommuConfines;
    case TrustedDmaOutcome::DigestMismatch:
        return Abi::InventoryTrustOutcome::DigestMismatch;
    case TrustedDmaOutcome::DriverMismatch:
        return Abi::InventoryTrustOutcome::DriverMismatch;
    case TrustedDmaOutcome::Ambiguous:
        return Abi::InventoryTrustOutcome::Ambiguous;
    case TrustedDmaOutcome::NoEntry:
        break;
    }
    return Abi::InventoryTrustOutcome::Pending;
}

[[nodiscard]] const char* DiagnosticOutcome(const TrustedDmaOutcome outcome) noexcept
{
    switch (outcome)
    {
    case TrustedDmaOutcome::Applied:
        return "TRUSTED-NO-DMA-ISOLATION";
    case TrustedDmaOutcome::IommuConfines:
        return "UNUSED-IOMMU-CONFINES";
    case TrustedDmaOutcome::DigestMismatch:
        return "REFUSED-DIGEST-MISMATCH";
    case TrustedDmaOutcome::DriverMismatch:
        return "REFUSED-DRIVER-MISMATCH";
    case TrustedDmaOutcome::Ambiguous:
        return "REFUSED-AMBIGUOUS";
    case TrustedDmaOutcome::NoEntry:
        break;
    }
    return "NO-ENTRY";
}

// "ASTERIA:DMA-TRUST:SSSS:BB:DD.F:<driver>:ENTRY-<n>:<outcome>"
[[gnu::noinline]] void ReportDecision(const Kernel::Device::TrustedDmaFunction& function,
                                      const Kernel::Device::TrustedDmaDecision decision,
                                      const char* const outcome) noexcept
{
    Line<112U> text{};
    text.Text("ASTERIA:DMA-TRUST:");
    text.Hex(function.Address.GetSegment(), 4U);
    text.Character(':');
    text.Hex(function.Address.GetBus(), 2U);
    text.Character(':');
    text.Hex(function.Address.GetDevice(), 2U);
    text.Character('.');
    text.Hex(function.Address.GetFunction(), 1U);
    text.Character(':');
    text.Text(Boot::BootTrustDriverName(function.Driver));
    text.Text(":ENTRY-");
    text.Decimal(decision.Entry);
    text.Character(':');
    text.Text(outcome);
    text.Text("\r\n");
    Platform::Firmware::WriteDiagnosticAscii(text.Get());
}

[[gnu::noinline]] void ReportEntry(const Foundation::Size number, const char* const outcome) noexcept
{
    Line<64U> text{};
    text.Text("ASTERIA:DMA-TRUST:ENTRY-");
    text.Decimal(number);
    text.Character(':');
    text.Text(outcome);
    text.Text("\r\n");
    Platform::Firmware::WriteDiagnosticAscii(text.Get());
}

// The function's identity and its driver, from the published object.
[[nodiscard]] bool Describe(Kernel::Device::PciFunction& function, const Boot::BootTrustDriver driver,
                            Kernel::Device::TrustedDmaFunction& output) noexcept
{
    if (!function.IsInitialized())
    {
        return false;
    }
    const Kernel::Device::PciFunctionDescriptor descriptor = function.GetSnapshot().Descriptor;
    output = Kernel::Device::TrustedDmaFunction{
        .Address = descriptor.Address,
        .Vendor = descriptor.VendorId,
        .Product = descriptor.DeviceId,
        .Driver = driver,
        .Reserved = 0U,
    };
    return true;
}
} // namespace

Hal::Device::IIommuDomainBackend* const (
    &TrustedDmaRuntime::GetBackends() noexcept)[Kernel::Device::IommuDomainPool::Capacity]
{
    for (Foundation::Size index{0U}; index < Kernel::Device::IommuDomainPool::Capacity; ++index)
    {
        m_backends[index] = &m_identity[index];
    }
    return m_backends;
}

Foundation::Size TrustedDmaRuntime::Apply(Kernel::Object::ObjectRegistry& registry, const Boot::BootTrustPolicy& policy,
                                          const Boot::BootFileRegion& files, const IommuAvailability availability,
                                          const TrustedDmaCandidate* const candidates,
                                          const Foundation::Size count) noexcept
{
    m_trusted = 0U;
    if ((policy.Count == 0U) || (policy.Count > Boot::BootTrustMaximumEntries))
    {
        return 0U;
    }
    Boot::Bootstrap::RecordBootInventoryTrust(policy, files);
    Platform::Firmware::WriteDiagnosticAscii((availability == IommuAvailability::Working)
                                                 ? "ASTERIA:DMA-TRUST:IOMMU:WORKING\r\n"
                                                 : ((availability == IommuAvailability::Refused)
                                                        ? "ASTERIA:DMA-TRUST:IOMMU:REFUSED\r\n"
                                                        : "ASTERIA:DMA-TRUST:IOMMU:ABSENT\r\n"));
    bool decided[Boot::BootTrustMaximumEntries]{};
    for (Foundation::Size index{0U}; (candidates != nullptr) && (index < count); ++index)
    {
        if (!candidates[index].Function.IsValid())
        {
            continue;
        }
        Kernel::Object::ObjectPin pin;
        if (registry.PinObject(candidates[index].Function, Kernel::Object::ObjectType::PciFunction, pin) !=
            Kernel::Object::ObjectPinError::Ok)
        {
            continue;
        }
        // The lookup checked the type, and only PciFunction instances report it.
        auto& function = static_cast<Kernel::Device::PciFunction&>(pin.GetObject());
        Kernel::Device::TrustedDmaFunction described{};
        if (!Describe(function, candidates[index].Driver, described))
        {
            continue;
        }
        const Kernel::Device::TrustedDmaDecision decision =
            Kernel::Device::DecideTrustedDma(policy, described, availability);
        if (decision.Outcome == TrustedDmaOutcome::NoEntry)
        {
            continue;
        }
        if (decision.Outcome == TrustedDmaOutcome::Ambiguous)
        {
            for (Foundation::Size entry{0U}; entry < policy.Count; ++entry)
            {
                if (Kernel::Device::TrustEntryMatches(policy.Entries[entry], described) && !decided[entry])
                {
                    decided[entry] = true;
                    Boot::Bootstrap::RecordBootInventoryTrustOutcome(entry + 1U, Abi::InventoryTrustOutcome::Ambiguous);
                }
            }
            ReportDecision(described, decision, DiagnosticOutcome(decision.Outcome));
            continue;
        }
        const Foundation::Size number{decision.Entry};
        if ((decision.Outcome == TrustedDmaOutcome::Applied) && !function.GrantTrustedDma())
        {
            // A bridge, a display or a function with firmware DMA, which
            // nothing may assign: the entry stays unapplied.
            ReportDecision(described, decision, "REFUSED-NOT-ASSIGNABLE");
            if (!decided[number - 1U])
            {
                decided[number - 1U] = true;
                Boot::Bootstrap::RecordBootInventoryTrustOutcome(number, Abi::InventoryTrustOutcome::NotAssignable);
            }
            continue;
        }
        if (decision.Outcome == TrustedDmaOutcome::Applied)
        {
            ++m_trusted;
            Boot::Bootstrap::RecordBootInventoryTrusted(described.Address.GetSegment(), described.Address.GetBus(),
                                                        described.Address.GetDevice(), described.Address.GetFunction(),
                                                        decision.Entry);
            // An applied entry reports Applied whatever else it matched.
            decided[number - 1U] = true;
            Boot::Bootstrap::RecordBootInventoryTrustOutcome(number, Abi::InventoryTrustOutcome::Applied);
        }
        else if (!decided[number - 1U])
        {
            decided[number - 1U] = true;
            Boot::Bootstrap::RecordBootInventoryTrustOutcome(number, InventoryOutcome(decision.Outcome));
        }
        ReportDecision(described, decision, DiagnosticOutcome(decision.Outcome));
    }
    for (Foundation::Size entry{0U}; entry < policy.Count; ++entry)
    {
        if (!decided[entry])
        {
            Boot::Bootstrap::RecordBootInventoryTrustOutcome(entry + 1U, Abi::InventoryTrustOutcome::NoFunction);
            ReportEntry(entry + 1U, "NO-FUNCTION");
        }
    }
    return m_trusted;
}
} // namespace Asteria::Platform::Device
