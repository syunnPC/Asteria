// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_DEVICE_FIRMWARE_DMA_RESERVATIONS_HPP_INCLUDED
#define ASTERIA_PLATFORM_DEVICE_FIRMWARE_DMA_RESERVATIONS_HPP_INCLUDED

#include "Boot/Acpi/AcpiTopology.hpp"
#include "Kernel/Device/DmaReservations.hpp"
#include "Platform/Device/VtdTopology.hpp"

namespace Asteria::Platform::Device
{
/// The firmware DMA ranges of the boot's requesters, from the DMAR's RMRR scopes (VT-d) or the
/// IVRS's IVMD blocks (AMD-Vi), as the resolved topology plan assigns them: each captured function
/// with its mask of the retained ranges (VtdFunctionRouting::Regions), bridge scopes and the
/// conservative reservation of an ambiguous walk included. The plan is the one capture reserves
/// functions by, and it does not change when a function is released from firmware, so DMA reservations
/// remain enforced for released requesters too. Built once at boot before any IommuDomain exists; immutable and
/// lock-free afterwards.
class FirmwareDmaReservations final : public Kernel::Device::IDmaReservations
{
public:
    static constexpr Foundation::Size MaximumRanges{8U};
    static constexpr Foundation::Size MaximumRequesters{VtdUnit::MaximumRequesters};

    constexpr FirmwareDmaReservations() noexcept = default;
    FirmwareDmaReservations(const FirmwareDmaReservations&) = delete;
    FirmwareDmaReservations& operator=(const FirmwareDmaReservations&) = delete;

    /// Boot, once: the plan's functions with their masks over the DMAR's
    /// retained RMRRs. False when built twice; ranges past the retained
    /// count are ignored, as capture ignores them.
    [[nodiscard]] bool BuildFromDmar(const Boot::Acpi::AcpiTopology& topology, const VtdTopologyPlan& plan) noexcept;
    /// Boot, once: the plan's functions with their masks over the IVRS's
    /// retained IVMD blocks.
    [[nodiscard]] bool BuildFromIvrs(const Boot::Acpi::IvrsTopology& ivrs, const VtdTopologyPlan& plan) noexcept;

    [[nodiscard]] Foundation::Size GetRangeCount() const noexcept;
    [[nodiscard]] Foundation::Size GetRequesterCount() const noexcept;

    [[nodiscard]] bool Overlaps(Foundation::UInt32 requester, Foundation::UInt64 first,
                                Foundation::UInt64 last) const noexcept override;

private:
    struct Range
    {
        Foundation::UInt64 Base;
        Foundation::UInt64 Limit;
    };
    struct Requester
    {
        Foundation::UInt32 Identifier;
        Foundation::UInt8 Mask;
        Foundation::UInt8 Reserved[3];
    };

    void AddRequesters(const VtdTopologyPlan& plan) noexcept;

    Range m_ranges[MaximumRanges]{};
    Requester m_requesters[MaximumRequesters]{};
    Foundation::Size m_rangeCount{0U};
    Foundation::Size m_requesterCount{0U};
    bool m_built{false};
};
} // Namespace Asteria::Platform::Device

#endif // ASTERIA_PLATFORM_DEVICE_FIRMWARE_DMA_RESERVATIONS_HPP_INCLUDED
