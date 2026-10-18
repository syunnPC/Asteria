// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_INTERRUPT_AMD64_INTERRUPT_LINES_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_INTERRUPT_AMD64_INTERRUPT_LINES_HPP_INCLUDED

#include "Boot/Acpi/AcpiTopology.hpp"
#include "Hal/Interrupt/IInterruptLines.hpp"
#include "Platform/Amd64/Interrupt/Amd64IoApic.hpp"
#include "Platform/Amd64/Interrupt/Amd64DeviceVectors.hpp"

namespace Asteria::Platform::Amd64::Interrupt
{
/// BSP-only two-phase router. No locks or allocation, including source ISR
/// operations. The publisher maps every register page UC in each kernel root.
class Amd64InterruptLines final : public Hal::Interrupt::IInterruptLines
{
public:
    constexpr Amd64InterruptLines() noexcept = default;
    [[nodiscard]] bool Configure(const Boot::Acpi::AcpiTopology& topology,
                                 const Platform::Device::IPhysicalMemoryAccess& access,
                                 Amd64DeviceVectors& vectors) noexcept;
    [[nodiscard]] bool CanRoute(Foundation::UInt32 gsi) const noexcept;
    [[nodiscard]] bool SetPolicy(Foundation::UInt32 gsi, bool level, bool activeLow) noexcept;
    [[nodiscard]] Hal::Interrupt::LineReservation Reserve(Foundation::UInt32 gsi) noexcept override;
    [[nodiscard]] bool Publish(Foundation::UInt64 vector, Kernel::Interrupt::Interrupt& interrupt) noexcept override;
    void Release(Foundation::UInt64 vector) noexcept override;

private:
    struct Entry
    {
        Amd64IoApicLine Source;
        Foundation::UInt32 Gsi;
        Foundation::UInt64 Vector;
    };
    struct Policy
    {
        Foundation::UInt32 Gsi;
        bool Level;
        bool ActiveLow;
    };
    Amd64IoApic m_controllers[Boot::Acpi::MaximumAcpiIoControllers]{};
    Foundation::Size m_count{0U};
    Amd64DeviceVectors* m_vectors{nullptr};
    Entry m_entries[Amd64DeviceVectors::VectorCount]{};
    Policy m_policies[Boot::Acpi::MaximumAcpiInterruptOverrides + 2U]{};
    Foundation::Size m_policyCount{0U};
};

/// IOAPIC pages are identity mapped, UC, supervisor-only in every kernel root.
class Amd64IoApicAccess final : public Platform::Device::IPhysicalMemoryAccess
{
public:
    constexpr Amd64IoApicAccess() noexcept = default;
    void AllowRegisterPage(Foundation::UInt64 page) noexcept;
    [[nodiscard]] Foundation::UInt8 GetMaximumPhysicalAddressBits() const noexcept override
    {
        return 52U;
    }
    [[nodiscard]] bool IsPhysicalRangeImplemented(Foundation::UInt64 start, Foundation::UInt64 length,
                                                  Foundation::UInt8 bits) const noexcept override;
    [[nodiscard]] Foundation::UInt64 ReadDoubleWord(Foundation::UInt64 address) const noexcept override;
    void WriteDoubleWord(Foundation::UInt64 address, Foundation::UInt32 value) const noexcept override;

private:
    Foundation::UInt64 m_pages[Boot::Acpi::MaximumAcpiIoControllers]{};
    Foundation::Size m_count{0U};
};
}
#endif
