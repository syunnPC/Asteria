// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_DEVICE_AMD_VI_HANDOFF_RECORD_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_DEVICE_AMD_VI_HANDOFF_RECORD_HPP_INCLUDED
#include "Platform/Device/VtdHandoffRecord.hpp"
namespace Asteria::Platform::Amd64::Device
{
/// AMD I/O Virtualization Technology (IOMMU) Specification 48882 rev.
/// 2.62 section 3.4's named memory base registers. Command and event pointer offsets are also used
/// by AmdViUnit's queues.
struct AmdViHandoffRecord
{
    static constexpr Foundation::Size BaseCount{15U};
    static constexpr Foundation::Size SecondaryPointerCount{8U};
    static constexpr Foundation::Size PointerCount{4U + SecondaryPointerCount};
    Foundation::UInt64 RegisterBase{0U};
    Platform::Device::FirmwareHandoffOutcome Outcome{Platform::Device::FirmwareHandoffOutcome::NotAddressed};
    Foundation::UInt8 Reason{0U};
    Foundation::UInt8 Reserved[6]{};
    Foundation::UInt64 Control{0U};
    Foundation::UInt64 Status{0U};
    Foundation::UInt64 Bases[BaseCount]{};
    Foundation::UInt64 CommandHead{0U};
    Foundation::UInt64 CommandTail{0U};
    Foundation::UInt64 EventHead{0U};
    Foundation::UInt64 EventTail{0U};
    // PPR, guest virtual APIC, PPR B, event B head/tail pairs. AMD IOMMU Specification 48882 rev.
    // 3.09 sections 3.4.17–20;
    Foundation::UInt64 SecondaryPointers[SecondaryPointerCount]{};
    [[nodiscard]] bool AllowsBootServices() const noexcept
    {
        return Outcome == Platform::Device::FirmwareHandoffOutcome::TakenOver;
    }
    [[nodiscard]] Foundation::Size
    Exclusions(Kernel::Memory::PhysicalPageImportExclusion (&ranges)[BaseCount]) const noexcept
    {
        if (Outcome == Platform::Device::FirmwareHandoffOutcome::TakenOver)
        {
            return 0U;
        }
        Foundation::Size count{0U};
        for (Foundation::Size index{0U}; index < BaseCount; ++index)
        {
            const auto value = Bases[index];
            const auto base = value & 0x000F'FFFF'FFFF'F000ULL;
            if (base == 0U)
            {
                continue;
            }
            // Device table/segments: DevTblSize+1 pages. Rings: 2^Len
            // sixteen-byte entries; the GA log uses eight-byte entries.
            const auto bytes = index < 8U     ? (1U + (value & 0x1FFU)) * 4096U
                               : index == 12U ? 4096U
                                              : (Foundation::UInt64{index == 11U ? 8U : 16U} << ((value >> 56U) & 15U));
            ranges[count++] = {base, bytes};
        }
        return count;
    }
};
static_assert(sizeof(AmdViHandoffRecord) == 248U);
} // Namespace Asteria::Platform::Amd64::Device
#endif
