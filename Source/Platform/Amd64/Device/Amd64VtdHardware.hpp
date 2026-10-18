// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_DEVICE_AMD64_VTD_HARDWARE_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_DEVICE_AMD64_VTD_HARDWARE_HPP_INCLUDED

#include "Platform/Device/VtdTopology.hpp"

namespace Asteria::Platform::Amd64::Device
{
/// DRHD register sets and RMRRs must lie below this physical-address width,
/// which the AMD64 boot's physical window and identity mappings reach.
inline constexpr Foundation::UInt8 Amd64VtdAddressBits{48U};
/// The line stride of the AMD64 adapter's clflush loop.
inline constexpr Foundation::Size Amd64VtdFlushLineBytes{64U};

/// A VT-d unit's register page through its uncached identity mapping, which
/// the boot runtime seeds into every root.
class Amd64VtdRegisters final : public Platform::Device::IVtdRegisters
{
public:
    constexpr Amd64VtdRegisters() noexcept = default;

    /// Once, before the first access.
    void Bind(Foundation::UInt64 base) noexcept;

    /// Borrowed for the BSP handoff only, while the physical window is open.
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
    void ReportHandoff(const Platform::Device::VtdFirmwareState& state) noexcept override;
    void ReportRefusal(Platform::Device::VtdHandoffFailure failure) noexcept override;

private:
    Foundation::UInt64 m_base{0U};
    Foundation::UInt8 m_queueAddressBits{0U};
    const Boot::BootInfo* m_bootInfo{nullptr};
    Platform::Device::VtdFirmwareMemoryOwnership m_ownership{};
};

/// clflush and mfence.
class Amd64CacheMaintenance final : public Platform::Device::IVtdCacheMaintenance
{
public:
    constexpr Amd64CacheMaintenance() noexcept = default;

    void Flush(const volatile void* line) noexcept override;
    /// clflush on every Amd64VtdFlushLineBytes line of the range; the
    /// following mfence (Fence) orders them (Intel SDM Vol. 2A, CLFLUSH).
    void FlushTables(const volatile void* start, Foundation::Size bytes) noexcept override;
    void Fence() noexcept override;
};
} // namespace Asteria::Platform::Amd64::Device

#endif // ASTERIA_PLATFORM_AMD64_DEVICE_AMD64_VTD_HARDWARE_HPP_INCLUDED
