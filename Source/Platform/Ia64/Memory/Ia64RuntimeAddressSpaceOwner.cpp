// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Memory/Ia64RuntimeAddressSpaceOwner.hpp"

namespace Asteria::Platform::Ia64::Memory
{
namespace
{
using OwnerFailure = Ia64RuntimeAddressSpaceOwnerFailure;
using OwnerResult = Foundation::Result<Ia64RuntimeAddressSpaceOwnerSummary, OwnerFailure>;

[[nodiscard]] constexpr OwnerFailure MakeFailure(const Ia64RuntimeAddressSpaceOwnerError error) noexcept
{
    OwnerFailure failure{};
    failure.Error = error;
    return failure;
}

[[nodiscard]] constexpr bool BytesAreZero(const Foundation::UInt8* const bytes, const Foundation::Size count) noexcept
{
    for (Foundation::Size index{0U}; index < count; ++index)
    {
        if (bytes[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] OwnerFailure PreflightPreparation(const Ia64RuntimeProcessorCapabilities& processorCapabilities,
                                                Ia64RegionIdentifierAllocator& regionIdentifierAllocator,
                                                const InitialAddressSpacePreparation& preparation,
                                                const Kernel::Memory::InitialMappingPlan*& mappingPlan,
                                                Ia64RegionIdentifierAllocatorSnapshot& allocatorBefore) noexcept
{
    const auto commonCapabilities = processorCapabilities.GetCommonCapabilities();
    if (!commonCapabilities.HasValue())
    {
        return MakeFailure(Ia64RuntimeAddressSpaceOwnerError::InvalidConfiguration);
    }

    const auto mappingPlanResult = preparation.TryGetMappingPlan();
    if (!mappingPlanResult.HasValue() || (mappingPlanResult.GetValue() == nullptr) ||
        !mappingPlanResult.GetValue()->IsSealed())
    {
        OwnerFailure failure = MakeFailure(Ia64RuntimeAddressSpaceOwnerError::MappingPlanRejected);
        if (!mappingPlanResult.HasValue())
        {
            failure.MappingPlan = mappingPlanResult.GetError();
        }
        return failure;
    }

    allocatorBefore = regionIdentifierAllocator.GetSnapshot();
    if ((allocatorBefore.State != Ia64RegionIdentifierAllocatorState::Ready) ||
        (allocatorBefore.RegionIdentifierBits != commonCapabilities.GetValue().RegionIdentifierBits))
    {
        return MakeFailure(Ia64RuntimeAddressSpaceOwnerError::InvalidConfiguration);
    }
    mappingPlan = mappingPlanResult.GetValue();
    return OwnerFailure{};
}
} // namespace

Ia64RuntimeAddressSpaceOwnerFailure
Ia64RuntimeAddressSpaceOwner::Fail(const Ia64RuntimeAddressSpaceOwnerError error) noexcept
{
    m_failure.Error = error;
    m_state = Ia64RuntimeAddressSpaceOwnerState::Failed;
    m_summary.State = m_state;
    return m_failure;
}

bool Ia64RuntimeAddressSpaceOwner::HasLiveRegistryBinding() const noexcept
{
    if ((m_registry == nullptr) || !m_summary.AddressSpaceIdentity.IsValid() ||
        !m_summary.AddressSpaceIdentity.IsFromRegistry(*m_registry) || !m_addressSpacePin.IsValid() ||
        (m_addressSpacePin.GetObjectType() != Kernel::Object::ObjectType::AddressSpace) ||
        (m_addressSpacePin.GetObjectId().GetValue() != m_summary.AddressSpaceIdentity.GetObjectId().GetValue()) ||
        (&m_addressSpacePin.GetObject() != &m_addressSpace))
    {
        return false;
    }

    Kernel::Object::ObjectPin verification;
    const Kernel::Object::ObjectPinError pinError = m_registry->PinObject(
        m_summary.AddressSpaceIdentity.GetObjectId(), Kernel::Object::ObjectType::AddressSpace, verification);
    return (pinError == Kernel::Object::ObjectPinError::Ok) && verification.IsValid() &&
           (&verification.GetObject() == &m_addressSpace);
}

bool Ia64RuntimeAddressSpaceOwner::VerifyPostconditions(
    const Kernel::Memory::InitialMappingPlan& mappingPlan,
    const Ia64RegionIdentifierAllocatorSnapshot& allocatorBefore) noexcept
{
    for (Foundation::Size index{0U}; index < Ia64RegionIdentifierCountPerAddressSpace; ++index)
    {
        const auto identifier = m_regionIdentifierReceipt.GetRegionIdentifier(index);
        if (!identifier.HasValue() ||
            (identifier.GetValue() != (m_summary.FirstRegionIdentifier + static_cast<Foundation::UInt32>(index))))
        {
            return false;
        }
    }

    const Ia64RegionIdentifierAllocatorSnapshot allocatorAfter = m_regionIdentifierAllocator->GetSnapshot();
    const Kernel::Memory::AddressSpaceSnapshot addressSpace = m_addressSpace.GetSnapshot();
    if ((allocatorBefore.State != Ia64RegionIdentifierAllocatorState::Ready) ||
        (allocatorAfter.State != Ia64RegionIdentifierAllocatorState::Ready) ||
        (allocatorAfter.AllocatedAddressSpaceCount != (allocatorBefore.AllocatedAddressSpaceCount + 1U)) ||
        (allocatorAfter.NextRegionIdentifier != (m_summary.LastRegionIdentifier + 1U)) ||
        (allocatorAfter.RepresentableLimitExclusive != allocatorBefore.RepresentableLimitExclusive) ||
        (allocatorAfter.RegionIdentifierBits != allocatorBefore.RegionIdentifierBits) ||
        !BytesAreZero(allocatorAfter.Reserved, sizeof(allocatorAfter.Reserved)) || !HasLiveRegistryBinding() ||
        !addressSpace.Identity.IsEqualTo(m_summary.AddressSpaceIdentity) ||
        (addressSpace.MappingCount != mappingPlan.GetMappingCount()) || (addressSpace.ResidentProcessorMask != 0U) ||
        (addressSpace.State != Kernel::Memory::AddressSpaceState::Ready) ||
        addressSpace.ActivatingProcessor.IsValid() || addressSpace.TransitionProcessor.IsValid() ||
        (addressSpace.TargetProcessorMask != 0U) || (addressSpace.AcknowledgedProcessorMask != 0U) ||
        !BytesAreZero(addressSpace.Reserved, sizeof(addressSpace.Reserved)))
    {
        return false;
    }

    m_summary.MappingCount = addressSpace.MappingCount;
    m_summary.AllocatedAddressSpaceCount = allocatorAfter.AllocatedAddressSpaceCount;
    return true;
}

OwnerResult Ia64RuntimeAddressSpaceOwner::Prepare(const Ia64RuntimeProcessorCapabilities& processorCapabilities,
                                                  Ia64RegionIdentifierAllocator& regionIdentifierAllocator,
                                                  const InitialAddressSpacePreparation& preparation,
                                                  Ia64ActiveRuntimeTranslationBindings& activeBindings,
                                                  const Kernel::Object::ResourceDomainId chargeOwner,
                                                  Kernel::Object::ObjectRegistry& registry,
                                                  Kernel::Object::RootCapabilityIssuer& issuer) noexcept
{
    if (m_state == Ia64RuntimeAddressSpaceOwnerState::Ready)
    {
        return OwnerResult::Failure(MakeFailure(Ia64RuntimeAddressSpaceOwnerError::AlreadyPrepared));
    }
    if (m_state == Ia64RuntimeAddressSpaceOwnerState::Failed)
    {
        return OwnerResult::Failure(MakeFailure(Ia64RuntimeAddressSpaceOwnerError::PreviousPreparationFailed));
    }
    if (!chargeOwner.IsValid() || !issuer.IsFromRegistry(registry))
    {
        return OwnerResult::Failure(MakeFailure(Ia64RuntimeAddressSpaceOwnerError::InvalidConfiguration));
    }

    const Kernel::Memory::InitialMappingPlan* mappingPlan{nullptr};
    Ia64RegionIdentifierAllocatorSnapshot allocatorBefore{};
    const OwnerFailure preflight = PreflightPreparation(processorCapabilities, regionIdentifierAllocator, preparation,
                                                        mappingPlan, allocatorBefore);
    if ((preflight.Error != Ia64RuntimeAddressSpaceOwnerError::None) || (mappingPlan == nullptr))
    {
        return OwnerResult::Failure(preflight.Error == Ia64RuntimeAddressSpaceOwnerError::None
                                        ? MakeFailure(Ia64RuntimeAddressSpaceOwnerError::InvalidConfiguration)
                                        : preflight);
    }

    const auto allocation = regionIdentifierAllocator.Allocate();
    if (!allocation.HasValue())
    {
        OwnerFailure failure = MakeFailure(Ia64RuntimeAddressSpaceOwnerError::RegionIdentifierAllocationRejected);
        failure.RegionIdentifierAllocator = allocation.GetError();
        return OwnerResult::Failure(failure);
    }

    m_state = Ia64RuntimeAddressSpaceOwnerState::Failed;
    m_summary.State = m_state;
    m_regionIdentifierAllocator = &regionIdentifierAllocator;
    m_regionIdentifierReceipt = allocation.GetValue();
    const auto first = m_regionIdentifierReceipt.GetRegionIdentifier(0U);
    const auto last = m_regionIdentifierReceipt.GetRegionIdentifier(Ia64RegionIdentifierCountPerAddressSpace - 1U);
    if (!first.HasValue() || !last.HasValue())
    {
        m_failure.RegionIdentifierAllocator = !first.HasValue() ? first.GetError() : last.GetError();
        return OwnerResult::Failure(Fail(Ia64RuntimeAddressSpaceOwnerError::RegionIdentifierReceiptRejected));
    }
    m_summary.FirstRegionIdentifier = first.GetValue();
    m_summary.LastRegionIdentifier = last.GetValue();

    m_failure.Backend = m_addressSpacePageTable.Initialize(processorCapabilities, m_regionIdentifierReceipt,
                                                           preparation, activeBindings);
    if ((m_failure.Backend != Ia64RuntimeAddressSpacePageTableInitializationError::None) ||
        !m_addressSpacePageTable.IsReady())
    {
        return OwnerResult::Failure(Fail(Ia64RuntimeAddressSpaceOwnerError::BackendInitializationRejected));
    }

    m_failure.AddressSpace = m_addressSpace.Initialize(chargeOwner, m_addressSpacePageTable, *mappingPlan);
    if (m_failure.AddressSpace != Kernel::Memory::AddressSpaceError::None)
    {
        return OwnerResult::Failure(Fail(Ia64RuntimeAddressSpaceOwnerError::AddressSpaceInitializationRejected));
    }
    m_summary.BackendClaimed = true;

    const auto registered = issuer.CreateObject(m_addressSpace);
    if (!registered.HasValue())
    {
        m_failure.Registry = registered.GetError();
        return OwnerResult::Failure(Fail(Ia64RuntimeAddressSpaceOwnerError::ObjectRegistrationRejected));
    }
    m_registry = &registry;
    m_summary.AddressSpaceIdentity = Kernel::Memory::AddressSpaceId{registry, registered.GetValue()};
    const Kernel::Object::ObjectPinError pinError = registry.PinObject(
        m_summary.AddressSpaceIdentity.GetObjectId(), Kernel::Object::ObjectType::AddressSpace, m_addressSpacePin);
    if ((pinError != Kernel::Object::ObjectPinError::Ok) || !m_addressSpacePin.IsValid() ||
        (&m_addressSpacePin.GetObject() != &m_addressSpace))
    {
        m_failure.IdentityBinding.Error = pinError == Kernel::Object::ObjectPinError::Ok
                                              ? Kernel::Memory::AddressSpaceError::ObjectInstanceMismatch
                                              : Kernel::Memory::AddressSpaceError::ObjectPinUnavailable;
        m_failure.IdentityBinding.PinError = pinError;
        return OwnerResult::Failure(Fail(Ia64RuntimeAddressSpaceOwnerError::IdentityBindingRejected));
    }
    m_failure.IdentityBinding = m_addressSpace.BindIdentity(registry, m_summary.AddressSpaceIdentity);
    if (m_failure.IdentityBinding.Error != Kernel::Memory::AddressSpaceError::None)
    {
        return OwnerResult::Failure(Fail(Ia64RuntimeAddressSpaceOwnerError::IdentityBindingRejected));
    }

    if (!VerifyPostconditions(*mappingPlan, allocatorBefore))
    {
        return OwnerResult::Failure(Fail(Ia64RuntimeAddressSpaceOwnerError::PostconditionRejected));
    }

    m_failure = {};
    m_state = Ia64RuntimeAddressSpaceOwnerState::Ready;
    m_summary.State = m_state;
    return OwnerResult::Success(m_summary);
}

Foundation::Result<Ia64RuntimeAddressSpaceOwnerView, Ia64RuntimeAddressSpaceOwnerError>
Ia64RuntimeAddressSpaceOwner::BorrowReadyAddressSpace() noexcept
{
    using ResultType = Foundation::Result<Ia64RuntimeAddressSpaceOwnerView, Ia64RuntimeAddressSpaceOwnerError>;
    if (m_state != Ia64RuntimeAddressSpaceOwnerState::Ready)
    {
        return ResultType::Failure(Ia64RuntimeAddressSpaceOwnerError::NotReady);
    }
    const Kernel::Memory::AddressSpaceSnapshot addressSpace = m_addressSpace.GetSnapshot();
    if (!HasLiveRegistryBinding() || (addressSpace.State != Kernel::Memory::AddressSpaceState::Ready) ||
        !addressSpace.Identity.IsEqualTo(m_summary.AddressSpaceIdentity))
    {
        return ResultType::Failure(Ia64RuntimeAddressSpaceOwnerError::PostconditionRejected);
    }
    return ResultType::Success(Ia64RuntimeAddressSpaceOwnerView{
        .AddressSpaceStorage = &m_addressSpace,
        .AddressSpaceIdentity = m_summary.AddressSpaceIdentity,
    });
}

bool Ia64RuntimeAddressSpaceOwner::ConfigureTranslationCachePurge(Ia64LocalTranslationCachePurge& purge) noexcept
{
    return (m_state == Ia64RuntimeAddressSpaceOwnerState::Ready) &&
           m_addressSpacePageTable.ConfigureTranslationCachePurge(purge);
}

Ia64RuntimeAddressSpaceOwnerSnapshot Ia64RuntimeAddressSpaceOwner::GetSnapshot() const noexcept
{
    return Ia64RuntimeAddressSpaceOwnerSnapshot{
        .Summary = m_summary,
        .Failure = m_failure,
        .AddressSpace = m_addressSpace.GetSnapshot(),
        .RegionIdentifierAllocator = m_regionIdentifierAllocator == nullptr
                                         ? Ia64RegionIdentifierAllocatorSnapshot{}
                                         : m_regionIdentifierAllocator->GetSnapshot(),
        .AddressSpaceStorage = &m_addressSpace,
        .State = m_state,
        .Reserved = {},
    };
}
} // namespace Asteria::Platform::Ia64::Memory
