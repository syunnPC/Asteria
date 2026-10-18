// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_DEVICE_VTD_HANDOFF_RECORD_HPP_INCLUDED
#define ASTERIA_PLATFORM_DEVICE_VTD_HANDOFF_RECORD_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Memory/PhysicalPageImportPolicy.hpp"

namespace Asteria::Platform::Device
{
/// Takeover is independent of later configuration failure. A record survives scratch-unit reuse and
/// is never reconstructed from the evidence's State. Registers are captured after the handoff step.
enum class FirmwareHandoffOutcome : Foundation::UInt8
{
    NotAddressed,
    TakenOver,
    Refused
};
struct VtdHandoffRecord
{
    Foundation::UInt64 RegisterBase{0U};
    FirmwareHandoffOutcome Outcome{FirmwareHandoffOutcome::NotAddressed};
    Foundation::UInt8 Reason{0U};
    Foundation::UInt8 Reserved[6]{};
    Foundation::UInt32 Version{0U};
    Foundation::UInt32 Status{0U};
    Foundation::UInt32 FaultStatus{0U};
    Foundation::UInt32 Kind{1U}; // AcpiIommuHandoff: 1 VT-d.
    Foundation::UInt64 QueueAddress{0U};
    Foundation::UInt64 QueueHead{0U};
    Foundation::UInt64 QueueTail{0U};
    Foundation::UInt64 RootAddress{0U};
    Foundation::UInt64 InterruptTable{0U};
    Foundation::UInt64 PageRequestQueue{0U};
    Foundation::UInt64 AdvancedFaultLog{0U};

    [[nodiscard]] constexpr bool AllowsBootServices() const noexcept
    {
        if (Outcome == FirmwareHandoffOutcome::TakenOver)
        {
            return true;
        }
        // Intel VT-d rev. 3.4 section 10.4.4.2: TES/IRES and QIES.
        return Outcome != FirmwareHandoffOutcome::NotAddressed &&
               (Status & ((Foundation::UInt32{1U} << 31U) | (Foundation::UInt32{1U} << 25U))) == 0U &&
               (((Status & (Foundation::UInt32{1U} << 26U)) == 0U) || QueueHead == QueueTail);
    }

    /// Enumerate every explicitly named extent, irrespective of enable bits.
    /// The caller excludes these from Conventional as well as BootServices.
    /// Taken-over units no longer refer to these firmware-owned structures.
    [[nodiscard]] constexpr Foundation::Size
    Exclusions(Kernel::Memory::PhysicalPageImportExclusion (&ranges)[5U]) const noexcept
    {
        // An all-ones version is an unreadable/absent register aperture,
        // not a set of physical base registers. Its status still denies BS.
        if (Outcome == FirmwareHandoffOutcome::TakenOver || Version == ~Foundation::UInt32{0U})
        {
            return 0U;
        }
        Foundation::Size count{0U};
        const Foundation::UInt64 bases[5U]{QueueAddress, RootAddress, InterruptTable, PageRequestQueue,
                                           AdvancedFaultLog};
        // VT-d rev. 3.4 sections 10.4.6, 10.4.15, 10.4.23, 10.4.30,
        // 10.4.33: IQA.QS, RTADDR, AFLOG.FLS[11:9], IRTA.S and PQA.PQS.
        const Foundation::UInt64 bytes[5U]{Foundation::UInt64{4096U} << (QueueAddress & 7U), 4096U,
                                           Foundation::UInt64{32U} << (InterruptTable & 15U),
                                           Foundation::UInt64{4096U} << (PageRequestQueue & 7U),
                                           Foundation::UInt64{4096U} << ((AdvancedFaultLog >> 9U) & 7U)};
        for (Foundation::Size index{0U}; index < 5U; ++index)
        {
            const Foundation::UInt64 base = bases[index] & ~Foundation::UInt64{4095U};
            if (base != 0U)
            {
                ranges[count++] = {base, bytes[index]};
            }
        }
        return count;
    }
};
static_assert(sizeof(VtdHandoffRecord) == 88U);
} // Namespace Asteria::Platform::Device
#endif
