// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_DIAGNOSTICS_IA64_STOP_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_DIAGNOSTICS_IA64_STOP_HPP_INCLUDED

#include "Boot/BootInfo.hpp"
#include "Hal/IPowerControl.hpp"
#include "Hal/Device/IPortIo.hpp"
#include "Kernel/Diagnostics/StopRecord.hpp"

namespace Asteria::Platform::Ia64::Diagnostics
{
[[nodiscard]] Hal::IPowerControl& ConfigureIa64Power(const Boot::Acpi::PowerRegisters& facts,
                                                     Hal::Device::IPortIo* ports, const Boot::BootSalInformation* sal,
                                                     const Boot::Acpi::AcpiTopology* topology) noexcept;
/// Where a terminal entry left the path that serves a PL3 access. The raw
/// entry AsteriaIa64StopEntry takes a vector word in bank r16 and records it
/// as the snapshot's VectorOffset, which a KernelTrap stop also reports as P1:
/// the architected vector offset in bits 0-15, the exit in bits 16-19 and,
/// for UserFaultBridge, the bridge's Ia64UserFaultBridgeStatus in bits 20-27
/// (zero when the bridge succeeded and a restore check after it failed). The
/// alternate data TLB vector 0x1000 alone has three such exits
enum class Ia64StopExit : Foundation::UInt8
{
    /// Every other terminal entry.
    None = 0U,
    /// An alternate TLB refill handler's validity check or a PL0 ledger miss.
    RefillHandler = 1U,
    /// A user-fault or user-exception entry check before the bridge call
    /// (collection still off).
    UserEntry = 2U,
    /// The bridge returned nonzero, or a restore check after it failed
    /// (collection on).
    UserFaultBridge = 3U,
};
inline constexpr Foundation::UInt64 Ia64StopExitShift{16U};
inline constexpr Foundation::UInt64 Ia64StopBridgeStatusShift{20U};
inline constexpr Foundation::UInt64 Ia64StopBridgeStatusBits{8U};
// Processor stop policy / PowerOff.md: distinct cooperative power request.
inline constexpr Foundation::UInt64 Ia64PowerLatchShift{63U};
inline constexpr Foundation::UInt64 Ia64PowerLatch{Foundation::UInt64{1U} << Ia64PowerLatchShift};
// IA-64 SDM v2 rev1.1 §5.8.4.1, table 5-16: INIT delivery, ignored vector.
inline constexpr Foundation::UInt64 Ia64PowerInitMessage{5U << 8U};
[[nodiscard]] constexpr bool Ia64PowerSalRevision(const Foundation::UInt16 revision) noexcept
{
    // SAL system-table revision is packed BCD: 2.9 is 0290h, not 0209h.
    const auto major = revision >> 8U;
    const auto minor = revision & 0xFFU;
    return (minor & 0xFU) <= 9U && (minor >> 4U) <= 9U && (major == 3U || (major == 2U && minor >= 0x90U));
}
[[nodiscard]] constexpr Foundation::UInt64 Ia64PowerInitAddress(const Foundation::UInt64 pib,
                                                                const Foundation::UInt64 lid) noexcept
{
    return pib + (((lid >> 24U) & 0xFFU) << 12U) + (((lid >> 16U) & 0xFFU) << 4U);
}

struct Ia64StopSnapshot
{
    /// The vector word: see Ia64StopExit.
    Foundation::UInt64 VectorOffset;
    Foundation::UInt64 Iip;
    Foundation::UInt64 Ipsr;
    Foundation::UInt64 Isr;
    Foundation::UInt64 Ifa;
    Foundation::UInt64 Iim;
    Foundation::UInt64 Iipa;
    Foundation::UInt64 RegionRegister;
    Foundation::UInt64 Rid;
    Foundation::UInt64 B0;
};
struct Ia64ProcessorStopRecord
{
    Kernel::Diagnostics::StopRecord Stop;
    Ia64StopSnapshot Snapshot;
};
struct Ia64StopsEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 Winner;
    Foundation::UInt64 OnlineMask;
    Foundation::UInt64 ConfirmedMask;
    Foundation::UInt64 UnconfirmedMask;
    Foundation::UInt64 UptimeTicks;
    Foundation::UInt64 RuntimeAddress;
    Foundation::UInt64 FrameBufferBase;
    Foundation::UInt64 FrameBufferBytes;
    Ia64ProcessorStopRecord Processors[4U];
    Foundation::UInt64 DrawPublished;
    Foundation::UInt64 DrawStates[4U];
};
static_assert(__builtin_offsetof(Ia64StopSnapshot, VectorOffset) == 0U);
static_assert(__builtin_offsetof(Ia64StopSnapshot, Iip) == 8U);
static_assert(__builtin_offsetof(Ia64StopSnapshot, Ipsr) == 16U);
static_assert(__builtin_offsetof(Ia64StopSnapshot, Isr) == 24U);
static_assert(__builtin_offsetof(Ia64StopSnapshot, Ifa) == 32U);
static_assert(__builtin_offsetof(Ia64StopSnapshot, Iim) == 40U);
static_assert(__builtin_offsetof(Ia64StopSnapshot, Iipa) == 48U);
static_assert(__builtin_offsetof(Ia64StopSnapshot, RegionRegister) == 56U);
static_assert(__builtin_offsetof(Ia64StopSnapshot, Rid) == 64U);
static_assert(__builtin_offsetof(Ia64StopSnapshot, B0) == 72U);
static_assert(sizeof(Ia64StopSnapshot) == 80U);
static_assert(__builtin_offsetof(Ia64ProcessorStopRecord, Stop) == 0U);
static_assert(__builtin_offsetof(Ia64ProcessorStopRecord, Snapshot) == 48U);
static_assert(sizeof(Ia64ProcessorStopRecord) == 128U);
static_assert(__builtin_offsetof(Ia64StopsEvidence, Published) == 0U);
static_assert(__builtin_offsetof(Ia64StopsEvidence, Winner) == 8U);
static_assert(__builtin_offsetof(Ia64StopsEvidence, OnlineMask) == 16U);
static_assert(__builtin_offsetof(Ia64StopsEvidence, ConfirmedMask) == 24U);
static_assert(__builtin_offsetof(Ia64StopsEvidence, UnconfirmedMask) == 32U);
static_assert(__builtin_offsetof(Ia64StopsEvidence, UptimeTicks) == 40U);
static_assert(__builtin_offsetof(Ia64StopsEvidence, RuntimeAddress) == 48U);
static_assert(__builtin_offsetof(Ia64StopsEvidence, FrameBufferBase) == 56U);
static_assert(__builtin_offsetof(Ia64StopsEvidence, FrameBufferBytes) == 64U);
static_assert(__builtin_offsetof(Ia64StopsEvidence, Processors) == 72U);
static_assert(__builtin_offsetof(Ia64StopsEvidence, DrawPublished) == 584U);
static_assert(__builtin_offsetof(Ia64StopsEvidence, DrawStates) == 592U);
static_assert(sizeof(Ia64StopsEvidence) == 624U);

/// All bank-zero accesses, terminal stacks and RBS live in the existing
/// locked runtime-memory page. No stop-specific translation is installed.
struct Ia64StopTerminalContext
{
    Foundation::UInt64 StackTop;
    Foundation::UInt64 RbsBase;
    Foundation::UInt64 GlobalPointer;
};
inline constexpr Foundation::Size Ia64StopMemoryStackSizeBytes{8192U};
inline constexpr Foundation::Size Ia64StopBackingStoreSizeBytes{8192U};
struct alignas(16U) Ia64StopStorage
{
    Foundation::UInt64 Latch;
    Foundation::UInt64 OnlineMask;
    Foundation::UInt64 Writer;
    Foundation::UInt64 StartTicks;
    Foundation::UInt64 ProcessorCount;
    Boot::BootDisplay Display;
    Ia64StopsEvidence Evidence;
    Ia64StopTerminalContext Terminal[4U];
    Foundation::UInt64 PowerConfirmed;
    alignas(16U) Foundation::UInt8 MemoryStacks[4U][Ia64StopMemoryStackSizeBytes];
    alignas(16U) Foundation::UInt8 BackingStores[4U][Ia64StopBackingStoreSizeBytes];
};
static_assert(__builtin_offsetof(Ia64StopStorage, Latch) == 0U);
static_assert(__builtin_offsetof(Ia64StopStorage, OnlineMask) == 8U);
static_assert(__builtin_offsetof(Ia64StopStorage, Display) == 40U);
static_assert(__builtin_offsetof(Ia64StopStorage, Evidence) == 72U);
static_assert(__builtin_offsetof(Ia64StopStorage, Terminal) == 696U);
static_assert(__builtin_offsetof(Ia64StopStorage, PowerConfirmed) == 792U);
static_assert(__builtin_offsetof(Ia64StopStorage, MemoryStacks) == 800U);
static_assert(__builtin_offsetof(Ia64StopStorage, BackingStores) == 33568U);
static_assert(sizeof(Ia64StopStorage) == 66336U);
/// ITC subtraction is modulo 2^64. 400 million ticks is one second at
/// itanium2-vpc's 400 MHz; hardware uses the same finite tick bound.
inline constexpr Foundation::UInt64 Ia64StopFreezeTicks{400'000'000U};
/// Called in physical bootstrap, before the runtime IVT is installed.
void PrepareIa64Stop(Ia64StopStorage& storage, const Boot::BootDisplay& display, Foundation::Size processors) noexcept;
// Direct native hooks preserve the closed KernelElf profile.
[[nodiscard]] Foundation::UInt64 Ia64ProgressProcessor() noexcept;
[[nodiscard]] Foundation::UInt64 Ia64ProgressTicks() noexcept;
void Ia64StopProcessorOnline() noexcept;
void PollIa64Stop() noexcept;
[[noreturn]] void StopIa64(Kernel::Diagnostics::StopCode code, Foundation::UInt64 p1 = 0U, Foundation::UInt64 p2 = 0U,
                           Foundation::UInt64 p3 = 0U, Foundation::UInt64 p4 = 0U) noexcept;
void ObserveIa64StopPoint(const Foundation::UInt8* text, Foundation::Size bytes) noexcept;
void CheckIa64StopPoint() noexcept;
void CheckIa64EarlyStopPoint() noexcept;
} // Namespace Asteria::Platform::Ia64::Diagnostics

#endif // ASTERIA_PLATFORM_IA64_DIAGNOSTICS_IA64_STOP_HPP_INCLUDED
