// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_DIAGNOSTICS_AMD64_STOP_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_DIAGNOSTICS_AMD64_STOP_HPP_INCLUDED

#include "Boot/BootInfo.hpp"
#include "Hal/IPowerControl.hpp"
#include "Hal/Device/IPortIo.hpp"
#include "Kernel/Diagnostics/StopRecord.hpp"
#include "Kernel/Device/DeviceRegion.hpp"
#include "Platform/Amd64/Interrupt/Amd64UserEntry.hpp"

namespace Asteria::Platform::Amd64::Diagnostics
{
[[nodiscard]] Hal::IPowerControl& ConfigureAmd64Power(const Boot::Acpi::PowerRegisters& facts,
                                                      Hal::Device::IPortIo* ports) noexcept;
struct Amd64StopSnapshot
{
    Foundation::UInt64 Vector;
    Foundation::UInt64 Error;
    Foundation::UInt64 Rip;
    Foundation::UInt64 Cs;
    Foundation::UInt64 Rflags;
    Foundation::UInt64 Rsp;
    Foundation::UInt64 Cr2;
    Foundation::UInt64 Cr3;
};
struct Amd64ProcessorStopRecord
{
    Kernel::Diagnostics::StopRecord Stop;
    Amd64StopSnapshot Snapshot;
};
struct Amd64StopsEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 Winner;
    Foundation::UInt64 OnlineMask;
    Foundation::UInt64 ConfirmedMask;
    Foundation::UInt64 UnconfirmedMask;
    Foundation::UInt64 UptimeTicks;
    Foundation::UInt64 ApertureRoot;
    Foundation::UInt64 ApertureBase;
    Foundation::UInt64 ApertureBytes;
    Amd64ProcessorStopRecord Processors[4U];
    Foundation::UInt64 DrawPublished;
    Foundation::UInt64 DrawStates[4U];
    Foundation::UInt64 StrayInterrupts[4U];
};
static_assert(__builtin_offsetof(Amd64StopSnapshot, Vector) == 0U);
static_assert(__builtin_offsetof(Amd64StopSnapshot, Error) == 8U);
static_assert(__builtin_offsetof(Amd64StopSnapshot, Rip) == 16U);
static_assert(__builtin_offsetof(Amd64StopSnapshot, Cs) == 24U);
static_assert(__builtin_offsetof(Amd64StopSnapshot, Rflags) == 32U);
static_assert(__builtin_offsetof(Amd64StopSnapshot, Rsp) == 40U);
static_assert(__builtin_offsetof(Amd64StopSnapshot, Cr2) == 48U);
static_assert(__builtin_offsetof(Amd64StopSnapshot, Cr3) == 56U);
static_assert(sizeof(Amd64StopSnapshot) == 64U);
static_assert(__builtin_offsetof(Amd64ProcessorStopRecord, Stop) == 0U);
static_assert(__builtin_offsetof(Amd64ProcessorStopRecord, Snapshot) == 48U);
static_assert(sizeof(Amd64ProcessorStopRecord) == 112U);
static_assert(__builtin_offsetof(Amd64StopsEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Amd64StopsEvidence, Winner) == 8U);
static_assert(__builtin_offsetof(Amd64StopsEvidence, OnlineMask) == 16U);
static_assert(__builtin_offsetof(Amd64StopsEvidence, ConfirmedMask) == 24U);
static_assert(__builtin_offsetof(Amd64StopsEvidence, UnconfirmedMask) == 32U);
static_assert(__builtin_offsetof(Amd64StopsEvidence, UptimeTicks) == 40U);
static_assert(__builtin_offsetof(Amd64StopsEvidence, ApertureRoot) == 48U);
static_assert(__builtin_offsetof(Amd64StopsEvidence, ApertureBase) == 56U);
static_assert(__builtin_offsetof(Amd64StopsEvidence, ApertureBytes) == 64U);
static_assert(__builtin_offsetof(Amd64StopsEvidence, Processors) == 72U);
static_assert(__builtin_offsetof(Amd64StopsEvidence, DrawPublished) == 520U);
static_assert(__builtin_offsetof(Amd64StopsEvidence, DrawStates) == 528U);
static_assert(__builtin_offsetof(Amd64StopsEvidence, StrayInterrupts) == 560U);
static_assert(sizeof(Amd64StopsEvidence) == 592U);

/// Supervisor RW/NX leaf attributes matching the kernel PAT layout. Zero for
/// a policy the terminal scanout does not support (only WC and UC are allowed).
[[nodiscard]] constexpr Foundation::UInt64
Amd64StopScanoutAttributes(const Kernel::Memory::MemoryCachePolicy policy) noexcept
{
    constexpr Foundation::UInt64 protection{(Foundation::UInt64{1U} << 63U) | 3U};
    if (policy == Kernel::Memory::MemoryCachePolicy::WriteCombining)
    {
        return protection | 0x80U;
    }
    if (policy == Kernel::Memory::MemoryCachePolicy::Uncached)
    {
        return protection | 0x18U;
    }
    return 0U;
}

/// Boot-only reservation: prepares empty supervisor tables reachable through root 0. Leaves
/// framebuffer unmapped until StopAmd64. Storage is identity-mapped in root 0.
[[nodiscard]] bool PrepareAmd64Stop(const Boot::BootDisplay& display, Foundation::UInt64 root,
                                    Foundation::UInt64 tables, Foundation::Size tablePages,
                                    const Foundation::UInt32* hardwareIds, Foundation::Size processors,
                                    Foundation::UInt64 localApicAddress) noexcept;
/// Boot-only borrowed scanout region; caller keeps it alive until terminal stop.
void SetAmd64StopScanoutRegion(Kernel::Device::DeviceRegion& region) noexcept;
void Amd64StopProcessorOnline(Foundation::UInt64 processor) noexcept;
[[noreturn]] void StopAmd64(Kernel::Diagnostics::StopCode code, Foundation::UInt64 p1 = 0U, Foundation::UInt64 p2 = 0U,
                            Foundation::UInt64 p3 = 0U, Foundation::UInt64 p4 = 0U,
                            const Interrupt::Amd64TrapFrame* frame = nullptr) noexcept;
[[noreturn]] void StopAmd64Trap(const Interrupt::Amd64TrapFrame& frame) noexcept;
void HandleAmd64Nmi(const Interrupt::Amd64TrapFrame& frame) noexcept;
void ObserveAmd64StopPoint(const Foundation::UInt8* text, Foundation::Size bytes) noexcept;
void CheckAmd64StopPoint() noexcept;
void CheckAmd64EarlyStopPoint() noexcept;
void RecordAmd64StrayInterrupt() noexcept;
} // namespace Asteria::Platform::Amd64::Diagnostics

#endif // ASTERIA_PLATFORM_AMD64_DIAGNOSTICS_AMD64_STOP_HPP_INCLUDED
