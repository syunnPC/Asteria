// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Memory/Ia64ActiveRuntimeTranslationBindings.hpp"

#include "Platform/Ia64/Memory/Ia64RuntimeRegionActivator.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeRegionSet.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeTranslationLedger.hpp"

namespace Asteria::Platform::Ia64::Memory
{
namespace
{
constexpr Foundation::UInt64 UnadmittedEncoding{0U};
constexpr Foundation::UInt64 AdmittedWithoutLedgerEncoding{1U};
constexpr Foundation::UInt64 RegionZeroLimit{Foundation::UInt64{1U} << 61U};

static_assert(alignof(Ia64RuntimeTranslationLedger) >= 2U);
static_assert((alignof(Ia64RuntimeTranslationLedger) & 1U) == 0U);

[[nodiscard]] bool IsRangeInsideRegionZero(const void* const object, const Foundation::Size sizeBytes) noexcept
{
    const Foundation::UIntPointer pointerValue = reinterpret_cast<Foundation::UIntPointer>(object);
    const Foundation::UInt64 start = static_cast<Foundation::UInt64>(pointerValue);
    const Foundation::UInt64 size = static_cast<Foundation::UInt64>(sizeBytes);
    return (size != 0U) && (start < RegionZeroLimit) && (size <= (RegionZeroLimit - start));
}

[[nodiscard]] Foundation::UInt64 EncodeLedger(const Ia64RuntimeTranslationLedger& ledger) noexcept
{
    return static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(&ledger.GetWireStorage()));
}

[[nodiscard]] bool IsLedgerEncodingValid(const Foundation::UInt64 encoding,
                                         const Ia64ActiveRuntimeTranslationBindingsWireStorage& wire) noexcept
{
    if ((encoding <= AdmittedWithoutLedgerEncoding) || ((encoding % alignof(Ia64RuntimeTranslationLedger)) != 0U))
    {
        return false;
    }
    const Foundation::UIntPointer pointerValue = static_cast<Foundation::UIntPointer>(encoding);
    if (static_cast<Foundation::UInt64>(pointerValue) != encoding)
    {
        return false;
    }
    return (encoding < RegionZeroLimit) &&
           (sizeof(Ia64RuntimeTranslationLedgerWireStorage) <= (RegionZeroLimit - encoding)) &&
           (wire.AdmissionOwnerAddress != 0U) && (encoding >= wire.RuntimeObjectStart) &&
           (encoding < wire.RuntimeObjectEnd) &&
           (sizeof(Ia64RuntimeTranslationLedgerWireStorage) <= (wire.RuntimeObjectEnd - encoding));
}

[[nodiscard]] const Ia64RuntimeTranslationLedger* DecodeLedger(const Foundation::UInt64 encoding) noexcept
{
    return reinterpret_cast<const Ia64RuntimeTranslationLedger*>(static_cast<Foundation::UIntPointer>(encoding));
}

[[nodiscard]] bool IsConfiguredRangeValid(const Ia64ActiveRuntimeTranslationBindingsWireStorage& wire) noexcept
{
    return (wire.AdmissionOwnerAddress != 0U) && (wire.RuntimeObjectStart < wire.RuntimeObjectEnd) &&
           (wire.RuntimeObjectEnd <= RegionZeroLimit);
}

[[nodiscard]] bool IsRangeInsideConfiguredRuntimeObjects(const Ia64ActiveRuntimeTranslationBindingsWireStorage& wire,
                                                         const void* const object,
                                                         const Foundation::Size sizeBytes) noexcept
{
    if (!IsConfiguredRangeValid(wire))
    {
        return false;
    }
    const Foundation::UInt64 start = static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(object));
    const Foundation::UInt64 size = static_cast<Foundation::UInt64>(sizeBytes);
    return (size != 0U) && (start >= wire.RuntimeObjectStart) && (start < wire.RuntimeObjectEnd) &&
           (size <= (wire.RuntimeObjectEnd - start));
}
} // namespace

Ia64ActiveRuntimeTranslationBindingError Ia64ActiveRuntimeTranslationBindings::ValidateAdmission(
    const Kernel::Scheduler::CpuId processor, const Foundation::UInt64 runtimeObjectStart,
    const Foundation::UInt64 runtimeObjectEnd, const Ia64RuntimeInterruptVectorTable& owner) const noexcept
{
    if (!processor.IsValid())
    {
        return Ia64ActiveRuntimeTranslationBindingError::InvalidProcessor;
    }
    if (!IsRangeInsideRegionZero(this, sizeof(*this)))
    {
        return Ia64ActiveRuntimeTranslationBindingError::BindingTableOutsideRegionZero;
    }

    const Foundation::UInt64 ownerAddress =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(&owner));
    if ((runtimeObjectStart >= runtimeObjectEnd) || (runtimeObjectEnd > RegionZeroLimit))
    {
        return Ia64ActiveRuntimeTranslationBindingError::RuntimeObjectRangeMismatch;
    }
    if ((m_wire.AdmissionOwnerAddress != 0U) && (m_wire.AdmissionOwnerAddress != ownerAddress))
    {
        return Ia64ActiveRuntimeTranslationBindingError::AdmissionOwnerMismatch;
    }
    if ((m_wire.AdmissionOwnerAddress != 0U) &&
        ((m_wire.RuntimeObjectStart != runtimeObjectStart) || (m_wire.RuntimeObjectEnd != runtimeObjectEnd)))
    {
        return Ia64ActiveRuntimeTranslationBindingError::RuntimeObjectRangeMismatch;
    }
    if (m_wire.Slots[processor.GetValue()].LoadAcquire() != UnadmittedEncoding)
    {
        return Ia64ActiveRuntimeTranslationBindingError::ProcessorAlreadyAdmitted;
    }
    return Ia64ActiveRuntimeTranslationBindingError::None;
}

Ia64ActiveRuntimeTranslationBindingError Ia64ActiveRuntimeTranslationBindings::AdmitProcessor(
    const Kernel::Scheduler::CpuId processor, const Foundation::UInt64 runtimeObjectStart,
    const Foundation::UInt64 runtimeObjectEnd, const Ia64RuntimeInterruptVectorTable& owner) noexcept
{
    const Ia64ActiveRuntimeTranslationBindingError validation =
        ValidateAdmission(processor, runtimeObjectStart, runtimeObjectEnd, owner);
    if (validation != Ia64ActiveRuntimeTranslationBindingError::None)
    {
        return validation;
    }

    if (m_wire.AdmissionOwnerAddress == 0U)
    {
        m_wire.RuntimeObjectStart = runtimeObjectStart;
        m_wire.RuntimeObjectEnd = runtimeObjectEnd;
        m_wire.AdmissionOwnerAddress =
            static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(&owner));
    }
    Foundation::UInt64 expected{UnadmittedEncoding};
    if (!m_wire.Slots[processor.GetValue()].CompareExchangeStrongAcquireRelease(expected,
                                                                                AdmittedWithoutLedgerEncoding))
    {
        return Ia64ActiveRuntimeTranslationBindingError::ProcessorAlreadyAdmitted;
    }
    return Ia64ActiveRuntimeTranslationBindingError::None;
}

Ia64ActiveRuntimeTranslationBindingError
Ia64ActiveRuntimeTranslationBindings::ActivateAndBind(const Kernel::Scheduler::CpuId processor,
                                                      const Ia64RuntimeRegionSet& regionSet,
                                                      const Ia64RuntimeTranslationLedger& ledger) noexcept
{
    if (!processor.IsValid())
    {
        return Ia64ActiveRuntimeTranslationBindingError::InvalidProcessor;
    }
    if (!IsRangeInsideRegionZero(this, sizeof(*this)))
    {
        return Ia64ActiveRuntimeTranslationBindingError::BindingTableOutsideRegionZero;
    }

    Synchronization::AtomicUInt64& slot = m_wire.Slots[processor.GetValue()];
    const Foundation::UInt64 current = slot.LoadAcquire();
    if (current == UnadmittedEncoding)
    {
        return Ia64ActiveRuntimeTranslationBindingError::ProcessorNotAdmitted;
    }
    if ((current != AdmittedWithoutLedgerEncoding) && !IsLedgerEncodingValid(current, m_wire))
    {
        return Ia64ActiveRuntimeTranslationBindingError::MalformedBindingEncoding;
    }
    if (!IsRangeInsideRegionZero(&regionSet, sizeof(regionSet)))
    {
        return Ia64ActiveRuntimeTranslationBindingError::RegionSetOutsideRegionZero;
    }
    if (!IsRangeInsideRegionZero(&ledger, sizeof(ledger)))
    {
        return Ia64ActiveRuntimeTranslationBindingError::LedgerOutsideRegionZero;
    }
    if (!IsConfiguredRangeValid(m_wire))
    {
        return Ia64ActiveRuntimeTranslationBindingError::RuntimeObjectRangeNotConfigured;
    }
    if (!IsRangeInsideConfiguredRuntimeObjects(m_wire, &regionSet, sizeof(regionSet)))
    {
        return Ia64ActiveRuntimeTranslationBindingError::RegionSetOutsideRuntimeObjectRange;
    }
    if (!IsRangeInsideConfiguredRuntimeObjects(m_wire, &ledger.GetWireStorage(), sizeof(ledger.GetWireStorage())))
    {
        return Ia64ActiveRuntimeTranslationBindingError::LedgerOutsideRuntimeObjectRange;
    }

    const Foundation::UInt64 ledgerEncoding = EncodeLedger(ledger);
    if (!IsLedgerEncodingValid(ledgerEncoding, m_wire))
    {
        return Ia64ActiveRuntimeTranslationBindingError::LedgerOutsideRegionZero;
    }

    const Ia64RuntimeRegionActivationError activation = Ia64RuntimeRegionActivator::Activate(regionSet);
    switch (activation)
    {
    case Ia64RuntimeRegionActivationError::None:
        slot.StoreRelease(ledgerEncoding);
        return Ia64ActiveRuntimeTranslationBindingError::None;
    case Ia64RuntimeRegionActivationError::InvalidRegionSet:
        return Ia64ActiveRuntimeTranslationBindingError::InvalidRegionSet;
    case Ia64RuntimeRegionActivationError::RejectedBeforeCommit:
        return Ia64ActiveRuntimeTranslationBindingError::ActivationRejectedBeforeCommit;
    case Ia64RuntimeRegionActivationError::CommittedStateUnknown:
    default:
        slot.StoreRelease(AdmittedWithoutLedgerEncoding);
        return Ia64ActiveRuntimeTranslationBindingError::ActivationStateUnknown;
    }
}

ASTERIA_NOINLINE_NOIPA
Ia64ActiveRuntimeTranslationBindingError Ia64ActiveRuntimeTranslationBindings::ActivateAndBindPhysicalBootstrap(
    const Kernel::Scheduler::CpuId processor, const Ia64RuntimeRegionSet& regionSet,
    const Ia64RuntimeTranslationLedger& ledger) noexcept
{
    if (!processor.IsValid())
    {
        return Ia64ActiveRuntimeTranslationBindingError::InvalidProcessor;
    }
    if (!IsRangeInsideRegionZero(this, sizeof(*this)))
    {
        return Ia64ActiveRuntimeTranslationBindingError::BindingTableOutsideRegionZero;
    }

    Synchronization::AtomicUInt64& slot = m_wire.Slots[processor.GetValue()];
    const Foundation::UInt64 current = slot.LoadAcquire();
    if (current == UnadmittedEncoding)
    {
        return Ia64ActiveRuntimeTranslationBindingError::ProcessorNotAdmitted;
    }
    if (current != AdmittedWithoutLedgerEncoding)
    {
        if (!IsLedgerEncodingValid(current, m_wire))
        {
            return Ia64ActiveRuntimeTranslationBindingError::MalformedBindingEncoding;
        }
        return Ia64ActiveRuntimeTranslationBindingError::ActiveLedgerMismatch;
    }
    if (!IsRangeInsideRegionZero(&regionSet, sizeof(regionSet)))
    {
        return Ia64ActiveRuntimeTranslationBindingError::RegionSetOutsideRegionZero;
    }
    if (!IsRangeInsideRegionZero(&ledger, sizeof(ledger)))
    {
        return Ia64ActiveRuntimeTranslationBindingError::LedgerOutsideRegionZero;
    }
    if (!IsConfiguredRangeValid(m_wire))
    {
        return Ia64ActiveRuntimeTranslationBindingError::RuntimeObjectRangeNotConfigured;
    }
    if (!IsRangeInsideConfiguredRuntimeObjects(m_wire, &regionSet, sizeof(regionSet)))
    {
        return Ia64ActiveRuntimeTranslationBindingError::RegionSetOutsideRuntimeObjectRange;
    }
    if (!IsRangeInsideConfiguredRuntimeObjects(m_wire, &ledger.GetWireStorage(), sizeof(ledger.GetWireStorage())))
    {
        return Ia64ActiveRuntimeTranslationBindingError::LedgerOutsideRuntimeObjectRange;
    }

    const Foundation::UInt64 ledgerEncoding = EncodeLedger(ledger);
    if (!IsLedgerEncodingValid(ledgerEncoding, m_wire))
    {
        return Ia64ActiveRuntimeTranslationBindingError::LedgerOutsideRegionZero;
    }

    const Ia64RuntimeRegionActivationError activation =
        Ia64RuntimeRegionActivator::ActivatePhysicalBootstrap(regionSet);
    switch (activation)
    {
    case Ia64RuntimeRegionActivationError::None:
        slot.StoreRelease(ledgerEncoding);
        return Ia64ActiveRuntimeTranslationBindingError::None;
    case Ia64RuntimeRegionActivationError::InvalidRegionSet:
        return Ia64ActiveRuntimeTranslationBindingError::InvalidRegionSet;
    case Ia64RuntimeRegionActivationError::RejectedBeforeCommit:
        return Ia64ActiveRuntimeTranslationBindingError::ActivationRejectedBeforeCommit;
    case Ia64RuntimeRegionActivationError::CommittedStateUnknown:
    default:
        slot.StoreRelease(AdmittedWithoutLedgerEncoding);
        return Ia64ActiveRuntimeTranslationBindingError::ActivationStateUnknown;
    }
}

Foundation::Result<const Ia64RuntimeTranslationLedger*, Ia64ActiveRuntimeTranslationBindingError>
Ia64ActiveRuntimeTranslationBindings::AcquireActiveLedger(const Kernel::Scheduler::CpuId processor) const noexcept
{
    using ResultType =
        Foundation::Result<const Ia64RuntimeTranslationLedger*, Ia64ActiveRuntimeTranslationBindingError>;

    if (!processor.IsValid())
    {
        return ResultType::Failure(Ia64ActiveRuntimeTranslationBindingError::InvalidProcessor);
    }
    if (!IsRangeInsideRegionZero(this, sizeof(*this)))
    {
        return ResultType::Failure(Ia64ActiveRuntimeTranslationBindingError::BindingTableOutsideRegionZero);
    }

    const Foundation::UInt64 encoding = m_wire.Slots[processor.GetValue()].LoadAcquire();
    if (encoding == UnadmittedEncoding)
    {
        return ResultType::Failure(Ia64ActiveRuntimeTranslationBindingError::ProcessorNotAdmitted);
    }
    if (encoding == AdmittedWithoutLedgerEncoding)
    {
        return ResultType::Failure(Ia64ActiveRuntimeTranslationBindingError::NoActiveLedger);
    }
    if (!IsLedgerEncodingValid(encoding, m_wire))
    {
        return ResultType::Failure(Ia64ActiveRuntimeTranslationBindingError::MalformedBindingEncoding);
    }
    return ResultType::Success(DecodeLedger(encoding));
}

Ia64ActiveRuntimeTranslationBindingError
Ia64ActiveRuntimeTranslationBindings::ClearActiveLedger(const Kernel::Scheduler::CpuId processor,
                                                        const Ia64RuntimeTranslationLedger& expectedLedger) noexcept
{
    if (!processor.IsValid())
    {
        return Ia64ActiveRuntimeTranslationBindingError::InvalidProcessor;
    }
    if (!IsRangeInsideRegionZero(this, sizeof(*this)))
    {
        return Ia64ActiveRuntimeTranslationBindingError::BindingTableOutsideRegionZero;
    }
    if (!IsRangeInsideRegionZero(&expectedLedger, sizeof(expectedLedger)))
    {
        return Ia64ActiveRuntimeTranslationBindingError::LedgerOutsideRegionZero;
    }
    if (!IsConfiguredRangeValid(m_wire))
    {
        return Ia64ActiveRuntimeTranslationBindingError::RuntimeObjectRangeNotConfigured;
    }
    if (!IsRangeInsideConfiguredRuntimeObjects(m_wire, &expectedLedger.GetWireStorage(),
                                               sizeof(expectedLedger.GetWireStorage())))
    {
        return Ia64ActiveRuntimeTranslationBindingError::LedgerOutsideRuntimeObjectRange;
    }

    Foundation::UInt64 expected = EncodeLedger(expectedLedger);
    if (!IsLedgerEncodingValid(expected, m_wire))
    {
        return Ia64ActiveRuntimeTranslationBindingError::LedgerOutsideRegionZero;
    }
    if (m_wire.Slots[processor.GetValue()].CompareExchangeStrongAcquireRelease(expected, AdmittedWithoutLedgerEncoding))
    {
        return Ia64ActiveRuntimeTranslationBindingError::None;
    }
    if (expected == UnadmittedEncoding)
    {
        return Ia64ActiveRuntimeTranslationBindingError::ProcessorNotAdmitted;
    }
    if (expected == AdmittedWithoutLedgerEncoding)
    {
        return Ia64ActiveRuntimeTranslationBindingError::NoActiveLedger;
    }
    if (!IsLedgerEncodingValid(expected, m_wire))
    {
        return Ia64ActiveRuntimeTranslationBindingError::MalformedBindingEncoding;
    }
    return Ia64ActiveRuntimeTranslationBindingError::ActiveLedgerMismatch;
}

bool Ia64ActiveRuntimeTranslationBindings::IsBound(const Ia64RuntimeTranslationLedger& ledger) const noexcept
{
    if (!IsRangeInsideRegionZero(this, sizeof(*this)) || !IsRangeInsideRegionZero(&ledger, sizeof(ledger)) ||
        !IsConfiguredRangeValid(m_wire) ||
        !IsRangeInsideConfiguredRuntimeObjects(m_wire, &ledger.GetWireStorage(), sizeof(ledger.GetWireStorage())))
    {
        return false;
    }
    const Foundation::UInt64 ledgerEncoding = EncodeLedger(ledger);
    if (!IsLedgerEncodingValid(ledgerEncoding, m_wire))
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < Kernel::Scheduler::MaximumProcessorCount; ++index)
    {
        if (m_wire.Slots[index].LoadAcquire() == ledgerEncoding)
        {
            return true;
        }
    }
    return false;
}
} // namespace Asteria::Platform::Ia64::Memory
