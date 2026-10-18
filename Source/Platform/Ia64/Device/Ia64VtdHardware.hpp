// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_DEVICE_IA64_VTD_HARDWARE_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_DEVICE_IA64_VTD_HARDWARE_HPP_INCLUDED

#include "Boot/BootInfo.hpp"
#include "Platform/Device/VtdTopology.hpp"
#include "Platform/Device/VtdUnit.hpp"
#include "Platform/Ia64/Device/Ia64PhysicalMemoryAccess.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"

namespace Asteria::Platform::Ia64::Device
{
/// The fc stride. Intel IA-64 Architecture Software Developer's Manual,
/// Volume 3, revision 1.1 (245319-002), "fc": "The line size affected is at
/// least 32-bytes (aligned on a 32-byte boundary)." Every 32-byte line of a
/// table range is therefore flushed; an implementation that flushes more per
/// fc only repeats work. Ia64VtdCacheMaintenance.S takes both values from the
/// generated layout header.
inline constexpr Foundation::Size Ia64VtdFlushLineShift{5U};
inline constexpr Foundation::Size Ia64VtdFlushLineBytes{Foundation::Size{1U} << Ia64VtdFlushLineShift};

/// The VT-d register adapter of IA-64. Registers are reached through the existing uncached physical
/// corridor (bit-63 alias, PSR.dt off, mf before and mf.a after every access), so a register set
/// needs no translation register. VT-d revision 1.3 section 10.2: 32-bit registers are accessed as
/// aligned doublewords, and every 64-bit register (CAP, ECAP, RTADDR, CCMD, IOTLB, IQH/IQT/IQA,
/// fault records) as one aligned quadword, never split, since a command's busy bit lives in the
/// upper half. A quadword access to a 32-bit register would also write its neighbour (GSTS beside
/// GCMD, FEDATA beside FECTL, the write-1-to-clear FSTS), so the widths are kept apart. BSP boot
/// configuration binds it once; later calls come from the unit under its lock in normal context.
class Ia64VtdRegisters final : public Platform::Device::IVtdRegisters
{
public:
    explicit Ia64VtdRegisters(const Ia64PhysicalMemoryAccess& access) noexcept : m_access{access}
    {
    }

    /// Once, before the first access: the register set's physical base and
    /// the PAL physical-address width that bounds every queue access.
    void Bind(Foundation::UInt64 base, Foundation::UInt8 physicalBits) noexcept;
    /// Borrowed for the BSP handoff only.
    void SetFirmwareMemoryPolicy(const Boot::BootInfo& info,
                                 const Platform::Device::VtdFirmwareMemoryOwnership& ownership) noexcept;

    [[nodiscard]] Foundation::UInt32 Read32(Foundation::Size offset) const noexcept override;
    [[nodiscard]] Foundation::UInt64 Read64(Foundation::Size offset) const noexcept override;
    void Write32(Foundation::Size offset, Foundation::UInt32 value) noexcept override;
    void Write64(Foundation::Size offset, Foundation::UInt64 value) noexcept override;

    [[nodiscard]] bool ReadQueueWord(Foundation::UInt64 address, Foundation::UInt32& value) const noexcept override;
    [[nodiscard]] bool WriteQueueWord(Foundation::UInt64 address, Foundation::UInt32 value) noexcept override;
    [[nodiscard]] bool CanWriteFirmwareMemory(Foundation::UInt64 address,
                                              Foundation::UInt64 bytes) const noexcept override;
    /// The firmware's GSTS, IQA, IQH, IQT and FSTS on the diagnostic channel,
    /// before any write (`VT-D handoff BASE=...`).
    void ReportHandoff(const Platform::Device::VtdFirmwareState& state) noexcept override;
    void ReportRefusal(Platform::Device::VtdHandoffFailure failure) noexcept override;

private:
    const Ia64PhysicalMemoryAccess& m_access;
    Foundation::UInt64 m_base{0U};
    Foundation::UInt8 m_physicalBits{0U};
    const Boot::BootInfo* m_bootInfo{nullptr};
    Platform::Device::VtdFirmwareMemoryOwnership m_ownership{};
};

/// Fc, sync.i, srlz.i and mf (Ia64VtdCacheMaintenance.S). One instance per unit, called only under
/// that unit's lock; its count is read without it.
class Ia64VtdCacheMaintenance final : public Platform::Device::IVtdCacheMaintenance
{
public:
    constexpr Ia64VtdCacheMaintenance() noexcept = default;

    void Flush(const volatile void* line) noexcept override;
    /// Fc on every 32-byte line of the range, then; sync.i; srlz.i; mf.
    void FlushTables(const volatile void* start, Foundation::Size bytes) noexcept override;
    /// Mf.
    void Fence() noexcept override;

    /// Table ranges written back so far (zero on a unit that snoops).
    [[nodiscard]] Foundation::UInt64 GetTableFlushes() const noexcept
    {
        return m_tableFlushes.LoadAcquire();
    }

private:
    Platform::Synchronization::AtomicUInt64 m_tableFlushes;
};
} // Namespace Asteria::Platform::Ia64::Device

#endif // ASTERIA_PLATFORM_IA64_DEVICE_IA64_VTD_HARDWARE_HPP_INCLUDED
