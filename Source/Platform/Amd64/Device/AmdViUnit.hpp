// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_DEVICE_AMD_VI_UNIT_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_DEVICE_AMD_VI_UNIT_HPP_INCLUDED

#include "Platform/Device/VtdUnit.hpp"
#include "Platform/Amd64/Device/AmdViHandoffRecord.hpp"

namespace Asteria::Platform::Amd64::Device
{
// The adapter, table-page and snapshot vocabulary AMD-Vi shares with the
// ISA-neutral VT-d unit.
using Platform::Device::IVtdCacheMaintenance;
using Platform::Device::IVtdRegisters;
using Platform::Device::VtdDomainFaults;
using Platform::Device::VtdTablePage;
using Platform::Device::VtdUnitError;
using Platform::Device::VtdUnitSnapshot;

/// AMD-Vi v1 DMA translation. The register/cache/page adapters and snapshot
/// vocabulary are shared with VT-d; all hardware encodings here are AMD's.
/// One rank-6 lock, bounded polling, no interrupts, allocation or callbacks.
/// MSI remapping is disabled (DTE IntCtl=pass, IV=0); MSI authority stays in
/// the existing PCI interrupt backend. ATS/PASID/guest translation are off.
class AmdViUnit final
{
public:
    static constexpr Foundation::Size MaximumRequesters{256U};
    static constexpr Foundation::UInt16 MaximumDomain{12U};
    static constexpr Foundation::Size MaximumDevicePages{512U};

    AmdViUnit(IVtdRegisters& registers, IVtdCacheMaintenance& cache) noexcept : m_registers{registers}, m_cache{cache}
    {
    }
    AmdViUnit(const AmdViUnit&) = delete;
    AmdViUnit& operator=(const AmdViUnit&) = delete;

    /// Called only after ordinary endpoints' bus mastering is disabled.
    [[nodiscard]] VtdUnitError PrepareDisabled() noexcept;
    /// Contiguous device/command/event storage: devicePages*4 KiB, 8 KiB,
    /// 8 KiB, and one completion page. All remain reserved for boot lifetime.
    [[nodiscard]] VtdUnitError Configure(VtdTablePage devices, Foundation::Size devicePages, VtdTablePage commands,
                                         VtdTablePage events, VtdTablePage completion,
                                         const Foundation::UInt16* requesters, Foundation::Size count,
                                         Foundation::UInt8 ivhdFlags = 0U) noexcept;
    [[nodiscard]] VtdUnitError Enable() noexcept;
    [[nodiscard]] bool IsEnabled() const noexcept;
    [[nodiscard]] Foundation::UInt8 GetLevels() const noexcept;
    [[nodiscard]] Foundation::UInt8 GetConfiguredLevels() const noexcept;
    [[nodiscard]] Foundation::UInt8 GetAddressBits() const noexcept;
    [[nodiscard]] bool Serves(Foundation::UInt16 requester) const noexcept;
    [[nodiscard]] VtdUnitError AttachContext(Foundation::UInt16 requester, Foundation::UInt16 domain,
                                             Foundation::UInt64 topTable, Foundation::UInt8 levels = 4U) noexcept;
    [[nodiscard]] VtdUnitError SeedContext(Foundation::UInt16 requester, Foundation::UInt16 domain,
                                           Foundation::UInt64 topTable, Foundation::UInt8 levels = 4U) noexcept;
    [[nodiscard]] VtdUnitError DetachContext(Foundation::UInt16 requester, Foundation::UInt16 domain) noexcept;
    [[nodiscard]] VtdUnitError InvalidateDomain(Foundation::UInt16 domain) noexcept;
    [[nodiscard]] VtdUnitError FlushWrites() noexcept;
    void Publish(const volatile void* start, Foundation::Size bytes) noexcept;
    void PollFaults() noexcept;
    [[nodiscard]] VtdDomainFaults GetDomainFaults(Foundation::UInt16 domain) const noexcept;
    [[nodiscard]] VtdUnitSnapshot GetSnapshot() const noexcept;
    void CopyHandoffRecord(AmdViHandoffRecord& output) const noexcept;

private:
    struct Requester
    {
        Foundation::UInt16 Identifier;
        Foundation::UInt16 Domain;
    };
    [[nodiscard]] Requester* Find(Foundation::UInt16 requester) noexcept;
    [[nodiscard]] VtdUnitError Install(Foundation::UInt16 requester, Foundation::UInt16 domain,
                                       Foundation::UInt64 topTable, Foundation::UInt8 levels,
                                       bool firmware = false) noexcept;
    [[nodiscard]] bool Queue(Foundation::UInt64 low, Foundation::UInt64 high) noexcept;
    [[nodiscard]] bool Complete() noexcept;
    [[nodiscard]] bool Invalidate(Foundation::UInt16 domain) noexcept;
    [[nodiscard]] bool Disable() noexcept;
    void CaptureHandoff(bool takenOver) noexcept;
    void PublishLocked(const volatile void* start, Foundation::Size bytes) noexcept;
    void PollLocked() noexcept;

    IVtdRegisters& m_registers;
    IVtdCacheMaintenance& m_cache;
    mutable Synchronization::SpinLock m_lock;
    VtdTablePage m_devices{};
    VtdTablePage m_commands{};
    VtdTablePage m_events{};
    VtdTablePage m_completion{};
    Requester m_requesters[MaximumRequesters]{};
    Foundation::Size m_count{0U};
    Foundation::Size m_devicePages{0U};
    Foundation::UInt64 m_tail{0U};
    Foundation::UInt64 m_sequence{0U};
    Foundation::UInt64 m_controlFlags{0U};
    VtdUnitSnapshot m_snapshot{};
    VtdDomainFaults m_faults[MaximumDomain + 1U]{};
    AmdViHandoffRecord m_handoff{};
    bool m_configured{false};
    bool m_observed{false};
};
} // namespace Asteria::Platform::Amd64::Device

#endif // ASTERIA_PLATFORM_AMD64_DEVICE_AMD_VI_UNIT_HPP_INCLUDED
