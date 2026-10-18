// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_MEMORY_IA64_RUNTIME_TRANSLATION_LEDGER_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_MEMORY_IA64_RUNTIME_TRANSLATION_LEDGER_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Memory/AddressSpaceMappingRecord.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeTranslationEncoder.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"

namespace Asteria::Platform::Ia64::Memory
{
class Ia64ActiveRuntimeTranslationBindings;
class Ia64RuntimeInterruptVectorTable;

enum class Ia64RuntimeTranslationLedgerError : Foundation::UInt8
{
    None,
    AlreadyInitialized,
    NotInitialized,
    CapacityExhausted,
    OverlappingVirtualRange,
    MappingNotFound,
    EncodingRejected,
};

/// EncodingError is non-None exactly when Code is EncodingRejected.
struct Ia64RuntimeTranslationLedgerFailure
{
    Ia64RuntimeTranslationLedgerError Code;
    Foundation::UInt8 Reserved[3];
    Ia64RuntimeTranslationEncodingError EncodingError;
};

/// Per-record valid state and payload layout consumed directly by the alternate-refill IVT.
struct Ia64RuntimeTranslationLedgerWireRecord
{
    Foundation::UInt64 VirtualStart;
    Foundation::UInt64 BaseTranslationWord;
    Foundation::UInt64 PageCount;
};

/// Complete fixed wire image shared by the C++ ledger and refill assembly.
/// PublicationWord is zero for uninitialized, one for initialized-empty, and
/// n + 1 for an acquired publication prefix of n records.
struct Ia64RuntimeTranslationLedgerWireStorage
{
    TranslationCapabilities Capabilities;
    Ia64RuntimeTranslationLedgerWireRecord Records[Kernel::Memory::MaximumRuntimeAddressSpaceMappings];
    Synchronization::AtomicUInt64 PublicationWord;
};

/// Fixed-capacity refill ledger with reusable slots. PageCount is each record's valid state,
/// published with release and read
/// first with acquire. The acquired publication prefix only grows; Append reuses its lowest
/// zero-count slot. One externally serialized non-interrupt writer owns all mutations, under the
/// owning AddressSpace lock. After Remove, the caller must complete the unmap on every resident
/// processor before another Append. AddressSpace's ShootdownPending state enforces this; failed
/// unmaps fault the space and prevent further publication. No processor quiescence gate is needed.
/// Readers run with interrupts off: an unmap acknowledgement delayed by a refill occurs after its
/// insertion. Storage and immutable capabilities outlive every writer and reader. Lookup is bounded
/// and allocation-free. The retained record is deliberately smaller than AddressSpaceMappingRecord:
/// it contains only the virtual base, physical/base translation word, and page count. Key-zero/PS12
/// ITIR is fixed for every admitted runtime translation and is reconstructed rather than duplicated
/// in every record. The encoded AR field is returned unchanged so IA-64 enforces data and
/// instruction access rights after insertion.
class Ia64RuntimeTranslationLedger final
{
public:
    constexpr Ia64RuntimeTranslationLedger() noexcept = default;
    ~Ia64RuntimeTranslationLedger() noexcept = default;

    Ia64RuntimeTranslationLedger(const Ia64RuntimeTranslationLedger&) = delete;
    Ia64RuntimeTranslationLedger& operator=(const Ia64RuntimeTranslationLedger&) = delete;
    Ia64RuntimeTranslationLedger(Ia64RuntimeTranslationLedger&&) = delete;
    Ia64RuntimeTranslationLedger& operator=(Ia64RuntimeTranslationLedger&&) = delete;

    /// Captures one stable common capability set. The runtime encoder is the
    /// authoritative validator. Failure leaves the ledger uninitialized.
    [[nodiscard]] Ia64RuntimeTranslationLedgerFailure Initialize(const TranslationCapabilities& capabilities) noexcept;

    /// Validates the complete mapping through Ia64RuntimeTranslationEncoder,
    /// rejects any virtual overlap, initializes the unpublished slot, and
    /// publishes it last. Every failure is atomic and retryable.
    [[nodiscard]] Ia64RuntimeTranslationLedgerFailure
    Append(const Kernel::Memory::AddressSpaceMappingRecord& mapping) noexcept;

    /// Tombstones the live record mapping's Append published: the record
    /// with mapping's virtual start, page count and base translation word.
    /// MappingNotFound when no live record is exactly that one;
    /// EncodingRejected when mapping is not one Append could accept. Every
    /// failure changes nothing.
    [[nodiscard]] Ia64RuntimeTranslationLedgerFailure
    Remove(const Kernel::Memory::AddressSpaceMappingRecord& mapping) noexcept;

    /// Looks up the 4 KiB page containing one arbitrary raw fault address.
    /// The mapping's exact AR word is returned; IA-64 enforces access rights on
    /// the restarted data access or instruction fetch after insertion.
    [[nodiscard]] Foundation::Result<EncodedIa64RuntimeTranslation, Ia64RuntimeTranslationLedgerFailure>
    Lookup(Foundation::UInt64 faultAddress) const noexcept;

    /// Returns the immutable assembly-consumed base. The enclosing object is
    /// standard-layout and this is its first and only data member.
    [[nodiscard]] constexpr const Ia64RuntimeTranslationLedgerWireStorage& GetWireStorage() const noexcept
    {
        return m_wire;
    }

private:
    friend class Ia64ActiveRuntimeTranslationBindings;
    friend class Ia64RuntimeInterruptVectorTable;

    Ia64RuntimeTranslationLedgerWireStorage m_wire{};
};

static_assert(sizeof(Ia64RuntimeTranslationLedgerError) == 1U);
static_assert(sizeof(Ia64RuntimeTranslationLedgerFailure) == 8U);
static_assert(alignof(Ia64RuntimeTranslationLedgerFailure) == 4U);
static_assert(sizeof(Ia64RuntimeTranslationLedgerWireRecord) == 24U);
static_assert(alignof(Ia64RuntimeTranslationLedgerWireRecord) == 8U);
static_assert(__is_aggregate(Ia64RuntimeTranslationLedgerWireRecord));
static_assert(__is_standard_layout(Ia64RuntimeTranslationLedgerWireRecord));
static_assert(__is_trivially_copyable(Ia64RuntimeTranslationLedgerWireRecord));
static_assert(__builtin_offsetof(Ia64RuntimeTranslationLedgerWireRecord, VirtualStart) == 0U);
static_assert(__builtin_offsetof(Ia64RuntimeTranslationLedgerWireRecord, BaseTranslationWord) == 8U);
static_assert(__builtin_offsetof(Ia64RuntimeTranslationLedgerWireRecord, PageCount) == 16U);
static_assert(sizeof(Ia64RuntimeTranslationLedgerWireStorage) == 6'176U);
static_assert(alignof(Ia64RuntimeTranslationLedgerWireStorage) == 8U);
static_assert(__is_aggregate(Ia64RuntimeTranslationLedgerWireStorage));
static_assert(__is_standard_layout(Ia64RuntimeTranslationLedgerWireStorage));
static_assert(__is_trivially_copyable(Ia64RuntimeTranslationLedgerWireStorage));
static_assert(__builtin_offsetof(Ia64RuntimeTranslationLedgerWireStorage, Capabilities) == 0U);
static_assert(__builtin_offsetof(Ia64RuntimeTranslationLedgerWireStorage, Records) == 24U);
static_assert(__builtin_offsetof(Ia64RuntimeTranslationLedgerWireStorage, PublicationWord) == 6'168U);
static_assert(sizeof(Ia64RuntimeTranslationLedger) == 6'176U);
static_assert(alignof(Ia64RuntimeTranslationLedger) == 8U);
static_assert(__is_standard_layout(Ia64RuntimeTranslationLedger));
} // Namespace Asteria::Platform::Ia64::Memory

#endif // ASTERIA_PLATFORM_IA64_MEMORY_IA64_RUNTIME_TRANSLATION_LEDGER_HPP_INCLUDED
