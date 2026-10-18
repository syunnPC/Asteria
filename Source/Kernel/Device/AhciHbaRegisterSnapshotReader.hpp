// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_DEVICE_AHCI_HBA_REGISTER_SNAPSHOT_READER_HPP_INCLUDED
#define ASTERIA_KERNEL_DEVICE_AHCI_HBA_REGISTER_SNAPSHOT_READER_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Hal/Device/IMemoryMappedIo.hpp"

namespace Asteria::Kernel::Device
{
enum class AhciHbaSnapshotError : Foundation::UInt8
{
    None,
    InvalidRange,
    MemoryMappedIoReadFailed,
    InvalidRegisterValue,
};

enum class AhciHbaGlobalRegister : Foundation::UInt8
{
    None,
    Capabilities,
    GlobalHostControl,
    InterruptStatus,
    PortsImplemented,
    Version,
};

struct AhciHbaRegisterRange
{
    Foundation::UInt64 BaseAddress;
    Foundation::UInt64 LengthBytes;
};

struct AhciHbaCapabilities
{
    Foundation::UInt8 PortCount;
    Foundation::UInt8 CommandSlotCount;
    /// Raw CAP.ISS encoding: zero is unspecified, one through three name the
    /// SATA generation, and all larger values remain reserved.
    Foundation::UInt8 InterfaceSpeedSupport;
    bool Supports64BitAddressing;
    bool SupportsNativeCommandQueuing;
    bool SupportsPortMultiplier;
    bool SupportsStaggeredSpinUp;
    bool SupportsAggressiveLinkPowerManagement;
};

struct AhciHbaControl
{
    bool AhciModeEnabled;
    bool InterruptsEnabled;
    bool ResetRequested;
    Foundation::UInt8 Reserved[5];
};

struct AhciHbaVersion
{
    /// Raw BCD value preserved from VS[31:16].
    Foundation::UInt16 Major;
    /// Raw BCD value preserved from VS[15:0].
    Foundation::UInt16 Minor;
};

struct AhciHbaRegisterSnapshot
{
    Foundation::UInt32 Capabilities;
    Foundation::UInt32 GlobalHostControl;
    Foundation::UInt32 InterruptStatus;
    Foundation::UInt32 PortsImplemented;
    Foundation::UInt32 Version;
    AhciHbaCapabilities DecodedCapabilities;
    AhciHbaControl DecodedControl;
    AhciHbaVersion DecodedVersion;
};

struct AhciHbaSnapshotFailure
{
    AhciHbaSnapshotError Error;
    Hal::Device::MemoryMappedIoReadError MemoryMappedIoRead;
    AhciHbaGlobalRegister Register;
    Foundation::UInt8 Reserved[5];
};

/// Captures the architecturally fixed AHCI global registers needed for the
/// first read-only controller proof: CAP, GHC, IS, PI, and VS, in that order.
/// CAP2 and BOHC are deliberately excluded because they are optional,
/// version-dependent extensions and are not needed to identify this baseline.
///
/// The supplied range is authoritative, uses [BaseAddress, BaseAddress +
/// LengthBytes) semantics, and must have a representable exclusive end that
/// covers every fixed register. Calls are bounded and allocation-free, perform
/// no MMIO writes, and stop at the first failed read. Failure leaves output
/// unchanged.
///
/// The supplied MMIO backend is non-owning and must outlive this reader and
/// every Read call. Its mapping or physical-access authority must remain live
/// for each call. This API inherits the backend's prohibition on calls from an
/// ISR or other interrupt context and on same-CPU reentry.
class AhciHbaRegisterSnapshotReader final
{
public:
    explicit constexpr AhciHbaRegisterSnapshotReader(Hal::Device::IMemoryMappedIo& memoryMappedIo) noexcept
        : m_memoryMappedIo{memoryMappedIo}
    {
    }

    [[nodiscard]] AhciHbaSnapshotFailure Read(AhciHbaRegisterRange range, AhciHbaRegisterSnapshot& output) noexcept;

private:
    [[nodiscard]] static AhciHbaSnapshotFailure
    MakeFailure(AhciHbaSnapshotError error,
                Hal::Device::MemoryMappedIoReadError memoryMappedIoRead = Hal::Device::MemoryMappedIoReadError::None,
                AhciHbaGlobalRegister reg = AhciHbaGlobalRegister::None) noexcept;

    [[nodiscard]] AhciHbaSnapshotFailure ReadRegister(Foundation::UInt64 physicalAddress, AhciHbaGlobalRegister reg,
                                                      Foundation::UInt32& output) noexcept;

    Hal::Device::IMemoryMappedIo& m_memoryMappedIo;
};

static_assert(sizeof(AhciHbaSnapshotError) == 1U);
static_assert(sizeof(AhciHbaGlobalRegister) == 1U);
static_assert(sizeof(AhciHbaRegisterRange) == 16U);
static_assert(alignof(AhciHbaRegisterRange) == 8U);
static_assert(sizeof(AhciHbaCapabilities) == 8U);
static_assert(alignof(AhciHbaCapabilities) == 1U);
static_assert(sizeof(AhciHbaControl) == 8U);
static_assert(alignof(AhciHbaControl) == 1U);
static_assert(sizeof(AhciHbaVersion) == 4U);
static_assert(alignof(AhciHbaVersion) == 2U);
static_assert(sizeof(AhciHbaRegisterSnapshot) == 40U);
static_assert(alignof(AhciHbaRegisterSnapshot) == 4U);
static_assert(sizeof(AhciHbaSnapshotFailure) == 8U);
static_assert(alignof(AhciHbaSnapshotFailure) == 1U);
} // namespace Asteria::Kernel::Device

#endif // ASTERIA_KERNEL_DEVICE_AHCI_HBA_REGISTER_SNAPSHOT_READER_HPP_INCLUDED
