// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_DEVICE_VTD_UNIT_HPP_INCLUDED
#define ASTERIA_PLATFORM_DEVICE_VTD_UNIT_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Platform/Device/VtdHandoffRecord.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Platform::Device
{
struct VtdFirmwareState
{
    Foundation::UInt32 Status;
    Foundation::UInt32 FaultStatus;
    Foundation::UInt64 QueueAddress;
    Foundation::UInt64 QueueHead;
    Foundation::UInt64 QueueTail;
};

enum class VtdHandoffFailure : Foundation::UInt8
{
    None,
    QueueError,
    InvalidationTimeout,
    MalformedQueue,
    FirmwareMemory,
    QueueTimeout,
    WaitTimeout,
    CommandTimeout,
    ProtectedMemoryTimeout,
};

[[nodiscard]] constexpr const char* VtdHandoffFailureName(const VtdHandoffFailure failure) noexcept
{
    switch (failure)
    {
    case VtdHandoffFailure::None:
        return "NONE";
    case VtdHandoffFailure::QueueError:
        return "IQE";
    case VtdHandoffFailure::InvalidationTimeout:
        return "ITE";
    case VtdHandoffFailure::MalformedQueue:
        return "MALFORMED-QUEUE";
    case VtdHandoffFailure::FirmwareMemory:
        return "QUEUE-MEMORY-NOT-FIRMWARE-OWNED";
    case VtdHandoffFailure::QueueTimeout:
        return "QUEUE-TIMEOUT";
    case VtdHandoffFailure::WaitTimeout:
        return "WAIT-TIMEOUT";
    case VtdHandoffFailure::CommandTimeout:
        return "COMMAND-TIMEOUT";
    case VtdHandoffFailure::ProtectedMemoryTimeout:
        return "PROTECTED-MEMORY-TIMEOUT";
    }
    return "UNKNOWN";
}

/// The register set of one VT-d remapping unit. 64-bit registers are read
/// and written whole, as the unit acts on a command register's upper half.
class IVtdRegisters
{
public:
    [[nodiscard]] virtual Foundation::UInt32 Read32(Foundation::Size offset) const noexcept = 0;
    [[nodiscard]] virtual Foundation::UInt64 Read64(Foundation::Size offset) const noexcept = 0;
    virtual void Write32(Foundation::Size offset, Foundation::UInt32 value) noexcept = 0;
    virtual void Write64(Foundation::Size offset, Foundation::UInt64 value) noexcept = 0;

    /// Boot handoff access to firmware's RAM invalidation queue. Adapters
    /// without physical-memory access refuse it; QI then remains enabled.
    /// (Pure, without default bodies: a visible default invites speculative
    /// devirtualization, whose function-address comparisons need descriptor
    /// and GOT entries the IA-64 kernel image does not admit.)
    [[nodiscard]] virtual bool ReadQueueWord(Foundation::UInt64 address, Foundation::UInt32& value) const noexcept = 0;
    [[nodiscard]] virtual bool WriteQueueWord(Foundation::UInt64 address, Foundation::UInt32 value) noexcept = 0;
    /// Authorizes the whole queue and the status destination against the
    /// captured firmware map and every retained kernel reservation.
    [[nodiscard]] virtual bool CanWriteFirmwareMemory(Foundation::UInt64 address,
                                                      Foundation::UInt64 bytes) const noexcept = 0;
    /// Boot observations, before any command; native adapter mirrors these
    /// to the visible console and diagnostic channel without allocating.
    virtual void ReportHandoff(const VtdFirmwareState& state) noexcept = 0;
    virtual void ReportRefusal(VtdHandoffFailure failure) noexcept = 0;

protected:
    constexpr IVtdRegisters() noexcept = default;
    ~IVtdRegisters() noexcept = default;
};

/// The processor-cache half of an ISA adapter. A unit whose walks do not snoop the processor's
/// caches (ECAP.C clear) reads its tables from memory, so every table line it may read is written
/// back before the register write (or, for a new mapping, the return to the caller) that makes it
/// reachable; a unit that snoops still needs the stores ordered before that write. The line stride
/// is the ISA's own constant and stays in the adapter.
class IVtdCacheMaintenance
{
public:
    /// Writes back the cache line holding `line` (AMD-Vi's single entries).
    virtual void Flush(const volatile void* line) noexcept = 0;
    /// Writes back every cache line that overlaps [start, start + bytes), so
    /// that the next Fence orders the write-backs before every later memory
    /// and register access. bytes is nonzero and the range does not wrap.
    virtual void FlushTables(const volatile void* start, Foundation::Size bytes) noexcept = 0;
    /// Orders every earlier store and write-back before every later memory
    /// and register access.
    virtual void Fence() noexcept = 0;

protected:
    constexpr IVtdCacheMaintenance() noexcept = default;
    ~IVtdCacheMaintenance() noexcept = default;
};

/// One 4 KiB table page: its 512 entries as the kernel addresses them and the
/// physical address the unit walks.
struct VtdTablePage
{
    volatile Foundation::UInt64* Entries;
    Foundation::UInt64 PhysicalAddress;
};

enum class VtdUnitError : Foundation::UInt8
{
    None,
    InvalidArguments,
    /// No 39- or 48-bit second-level walk, or a register layout outside the set.
    Unsupported,
    /// Enable was called for a unit this kernel already enabled.
    AlreadyActive,
    NotConfigured,
    NotServed,
    /// A command did not complete within its bound; the unit stays failed.
    TimedOut,
    Failed,
};

struct VtdFaultRecord
{
    Foundation::UInt64 Address;
    Foundation::UInt16 Source;
    Foundation::UInt8 Reason;
    bool Read;
    Foundation::UInt8 Reserved[4];
};

struct VtdDomainFaults
{
    Foundation::UInt64 Count;
    VtdFaultRecord Last;
};

struct VtdUnitSnapshot
{
    Foundation::UInt64 Capability;
    Foundation::UInt64 ExtendedCapability;
    Foundation::UInt32 Version;
    /// GSTS when the unit was configured, and now.
    Foundation::UInt32 HandoffStatus;
    Foundation::UInt32 Status;
    Foundation::UInt8 Levels;
    bool Enabled;
    bool Failed;
    Foundation::UInt8 Reserved;
    Foundation::UInt64 Served;
    Foundation::UInt64 DroppedRequesters;
    Foundation::UInt64 Attached;
    Foundation::UInt64 Faults;
    Foundation::UInt64 Overflows;
    /// Faults of a requester no domain holds.
    Foundation::UInt64 Unattributed;
    VtdFaultRecord LastFault;
};

/// One VT-d remapping unit in legacy mode: a root table, one context table per bus of the
/// requesters it serves, and second-level translation for attached requesters only. Every other
/// context entry is not present, so an unattached requester's DMA is blocked and recorded as a
/// fault; there is no pass-through, no queued invalidation and no interrupt remapping. Faults are
/// polled, never signalled. One lock (rank 6, acquiring nothing) serializes the registers and the
/// root and context tables; every spin is bounded, and a command that does not complete leaves the
/// unit failed. Normal context only.
class VtdUnit final
{
public:
    static constexpr Foundation::Size MaximumBuses{4U};
    static constexpr Foundation::Size MaximumRequesters{256U};
    /// Domain identifiers 1-4 belong to user backends; 5-12 to the eight
    /// kernel firmware domains. Zero is unused.
    static constexpr Foundation::UInt16 MaximumDomain{12U};

    VtdUnit(IVtdRegisters& registers, IVtdCacheMaintenance& cache) noexcept : m_registers{registers}, m_cache{cache}
    {
    }

    VtdUnit(const VtdUnit&) = delete;
    VtdUnit& operator=(const VtdUnit&) = delete;

    /// Boot takeover for a unit kept untranslated: records firmware status,
    /// proves QI quiescence before changing TE/QIE, then disables IR/PMRs.
    /// A queue refusal leaves the translation controls and root intact;
    /// the failed unit cannot be configured or retried by this boot.
    /// Requires no supported DMA walk and changes no root/context table.
    [[nodiscard]] VtdUnitError PrepareDisabled() noexcept;
    [[nodiscard]] VtdHandoffFailure GetHandoffFailure() const noexcept;
    /// Copies the saved post-handoff values; performs no register access.
    void CopyHandoffRecord(VtdHandoffRecord& record) const noexcept;

    /// Reads the capabilities, disables firmware translation/QI/IR/PMRs, and
    /// takes the zeroed root page, the context pages and the requesters
    /// (bus << 8 | device << 3 | function) the unit can confine; a requester
    /// on a bus beyond the fourth is not served and is counted in the snapshot.
    /// registerBytes is the validated DRHD register-set size in bytes.
    [[nodiscard]] VtdUnitError Configure(const VtdTablePage& root, const VtdTablePage (&contexts)[MaximumBuses],
                                         const Foundation::UInt16* requesters, Foundation::Size count,
                                         Foundation::Size registerBytes = 4096U) noexcept;

    /// Masks fault events, installs the root table, invalidates the context
    /// cache and the IOTLB globally, and enables translation.
    [[nodiscard]] VtdUnitError Enable() noexcept;

    [[nodiscard]] bool IsEnabled() const noexcept;
    /// 3 or 4 second-level levels, or 0 while the unit cannot translate.
    [[nodiscard]] Foundation::UInt8 GetLevels() const noexcept;
    /// Minimum of MGAW and configured walk width, also usable before Enable.
    [[nodiscard]] Foundation::UInt8 GetAddressBits() const noexcept;
    [[nodiscard]] bool Serves(Foundation::UInt16 requester) const noexcept;

    /// Makes requester translate through domain's top table: the entry's
    /// upper half, then its lower half with the present bit, then a
    /// device-selective context-cache invalidation.
    [[nodiscard]] VtdUnitError AttachContext(Foundation::UInt16 requester, Foundation::UInt16 domain,
                                             Foundation::UInt64 topTable) noexcept;
    /// Boot only, configured but not enabled. Seeds a firmware domain's
    /// context before translation starts; it cannot replace a live context.
    [[nodiscard]] VtdUnitError SeedContext(Foundation::UInt16 requester, Foundation::UInt16 domain,
                                           Foundation::UInt64 topTable) noexcept;
    /// Configured walk depth, including before Enable, or zero on failure.
    [[nodiscard]] Foundation::UInt8 GetConfiguredLevels() const noexcept;

    /// Blocks requester again: the present bit cleared, the context cache and
    /// the domain's IOTLB invalidated, then the upper half cleared.
    [[nodiscard]] VtdUnitError DetachContext(Foundation::UInt16 requester, Foundation::UInt16 domain) noexcept;
    /// Invalidates the domain's IOTLB entries, draining reads and writes
    /// where the unit can, so no DMA uses a removed translation afterwards.
    [[nodiscard]] VtdUnitError InvalidateDomain(Foundation::UInt16 domain) noexcept;
    /// Flushes the unit's write buffer where it has one, after new entries.
    [[nodiscard]] VtdUnitError FlushWrites() noexcept;
    /// Writes back the cache lines of [start, start + bytes) and fences, where
    /// the unit's walks do not snoop.
    void Publish(const volatile void* start, Foundation::Size bytes) noexcept;

    /// Reads and clears every pending fault record and the overflow bit.
    void PollFaults() noexcept;
    /// The faults recorded against domain's requesters, and the last one.
    [[nodiscard]] VtdDomainFaults GetDomainFaults(Foundation::UInt16 domain) const noexcept;
    [[nodiscard]] VtdUnitSnapshot GetSnapshot() const noexcept;

private:
    struct Requester
    {
        Foundation::UInt16 Identifier;
        Foundation::UInt16 Domain;
        Foundation::UInt8 Bus;
        Foundation::UInt8 Reserved[3];
    };

    [[nodiscard]] VtdUnitError InstallContextLocked(Foundation::UInt16 requester, Foundation::UInt16 domain,
                                                    Foundation::UInt64 topTable) noexcept;
    [[nodiscard]] bool WaitForStatusLocked(Foundation::UInt32 bit, bool set) noexcept;
    [[nodiscard]] bool WaitForClearLocked(Foundation::Size offset) noexcept;
    [[nodiscard]] bool InvalidateContextLocked(Foundation::UInt64 granularity) noexcept;
    [[nodiscard]] bool InvalidateIotlbLocked(Foundation::UInt64 granularity) noexcept;
    [[nodiscard]] Requester* FindLocked(Foundation::UInt16 requester) noexcept;
    [[nodiscard]] const Requester* FindLocked(Foundation::UInt16 requester) const noexcept;
    [[nodiscard]] volatile Foundation::UInt64* ContextEntryLocked(const Requester& requester) noexcept;
    void PublishLocked(const volatile void* start, Foundation::Size bytes) noexcept;
    void PollFaultsLocked() noexcept;
    void FailLocked() noexcept;
    [[nodiscard]] bool ResetFirmwareLocked() noexcept;
    [[nodiscard]] bool ResetFirmwareStateLocked() noexcept;
    [[nodiscard]] bool CommandLocked(Foundation::UInt32 bit, bool set) noexcept;
    [[nodiscard]] bool QuiesceQueueLocked() noexcept;
    [[nodiscard]] bool RefuseFirmwareLocked(VtdHandoffFailure reason) noexcept;
    [[nodiscard]] VtdUnitError HandoffErrorLocked() const noexcept;
    void ObserveFirmwareLocked() noexcept;
    [[nodiscard]] bool FlushWritesLocked() noexcept;

    IVtdRegisters& m_registers;
    IVtdCacheMaintenance& m_cache;
    mutable Platform::Synchronization::SpinLock m_synchronization;
    VtdTablePage m_root{};
    VtdTablePage m_contexts[MaximumBuses]{};
    Foundation::UInt8 m_buses[MaximumBuses]{};
    Foundation::Size m_busCount{0U};
    Requester m_requesters[MaximumRequesters]{};
    Foundation::Size m_requesterCount{0U};
    Foundation::UInt64 m_droppedRequesters{0U};
    Foundation::UInt64 m_capability{0U};
    Foundation::UInt64 m_extendedCapability{0U};
    Foundation::Size m_iotlbOffset{0U};
    Foundation::Size m_faultOffset{0U};
    Foundation::Size m_faultRecords{0U};
    Foundation::UInt32 m_version{0U};
    Foundation::UInt32 m_handoffStatus{0U};
    VtdHandoffRecord m_handoffRecord{};
    VtdHandoffFailure m_handoffFailure{VtdHandoffFailure::None};
    Foundation::UInt64 m_faults{0U};
    Foundation::UInt64 m_overflows{0U};
    Foundation::UInt64 m_unattributed{0U};
    VtdDomainFaults m_domainFaults[MaximumDomain + 1U]{};
    VtdFaultRecord m_lastFault{};
    Foundation::UInt8 m_levels{0U};
    bool m_configured{false};
    bool m_observed{false};
    bool m_enabled{false};
    bool m_failed{false};
};

static_assert(sizeof(VtdTablePage) == 16U);
static_assert(sizeof(VtdFaultRecord) == 16U);
} // Namespace Asteria::Platform::Device

#endif // ASTERIA_PLATFORM_DEVICE_VTD_UNIT_HPP_INCLUDED
