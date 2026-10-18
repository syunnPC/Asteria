// SPDX-License-Identifier: Apache-2.0
#include "Hal/Memory/IAddressSpacePageTable.hpp"
#include "Kernel/Memory/AddressSpace.hpp"
#include "Kernel/Memory/Frame.hpp"
#include "Kernel/Memory/InitialMappingPlan.hpp"
#include "Kernel/Memory/MemoryObject.hpp"
#include "Kernel/Memory/PinnedFrameMapping.hpp"
#include "Kernel/Memory/ProcessorAddressSpaceController.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

#include "Support/ObjectRegistryFixture.hpp"
#include "Support/Random.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

// Model-based test of AddressSpace mapping transactions with the real Frame,
// MemoryObject, PinnedFrameMappingPreparation, ObjectRegistry, and two
// ProcessorAddressSpaceControllers over a fake backend that injects every
// backend outcome; the test plays each CPU of the shootdown path. A reference
// model predicts every result, snapshot, record, backend call, object pin, and
// Frame W^X record and physical claim; each seed ends with a teardown in which
// every object must be collectable. CPUs 0 and 1 belong to controllers sharing
// idle root 0; CPUs 2 and 3 hold raw reservations across steps, interleaving as
// concurrent CPUs would. A fixed scenario checks physical Frame claims and a
// characterization pins a current finding. ASTERIA_TEST_SEED replays one seed.
namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::UInt8;
using Kernel::Memory::AddressSpace;
using Kernel::Memory::AddressSpaceActivationFailure;
using Kernel::Memory::AddressSpaceError;
using Kernel::Memory::AddressSpaceFailure;
using Kernel::Memory::AddressSpaceId;
using Kernel::Memory::AddressSpaceMappingToken;
using Kernel::Memory::AddressSpaceState;
using Kernel::Memory::BootFrame;
using Kernel::Memory::Frame;
using Kernel::Memory::FrameId;
using Kernel::Memory::FrameKind;
using Kernel::Memory::InitialMappingOwner;
using Kernel::Memory::InitialMappingPlan;
using Kernel::Memory::MemoryCachePolicy;
using Kernel::Memory::MemoryCommitment;
using Kernel::Memory::MemoryObject;
using Kernel::Memory::MemoryObjectError;
using Kernel::Memory::MemoryObjectId;
using Kernel::Memory::MemoryObjectState;
using Kernel::Memory::PageCount;
using Kernel::Memory::PagePermission;
using Kernel::Memory::PagePermissions;
using Kernel::Memory::PhysicalAddress;
using Kernel::Memory::PinnedFrameMappingError;
using Kernel::Memory::PinnedFrameMappingPreparation;
using Kernel::Memory::ProcessorAddressSpaceController;
using Kernel::Memory::ProcessorAddressSpaceControllerFailure;
using Kernel::Memory::VirtualAddress;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectPinError;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectRegistryError;
using Kernel::Object::ObjectType;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Resource::ResourceDomain;
using Kernel::Scheduler::CpuId;
using TestSupport::Random;
using ActivationError = Hal::Memory::AddressSpacePageTableActivationError;
using ControllerError = Kernel::Memory::ProcessorAddressSpaceControllerError;
using ControllerState = Kernel::Memory::ProcessorAddressSpaceControllerState;
using OwnershipError = Hal::Memory::AddressSpacePageTableOwnershipError;
using PageTableError = Hal::Memory::AddressSpacePageTableError;
using Record = Kernel::Memory::AddressSpaceMappingRecord;

constexpr Size SpaceCount{3U};
constexpr Size ControllerCount{2U};
constexpr Size CapacitySpace{1U};
constexpr UInt8 FirstRawProcessor{2U};
constexpr UInt8 ProcessorLimit{Kernel::Scheduler::MaximumProcessorCount};
constexpr Size HomeMemoryObjectCount{4U};
constexpr Size ForeignMemoryObject{HomeMemoryObjectCount};
constexpr Size MemoryObjectCount{HomeMemoryObjectCount + 1U};
constexpr Size HomeFrameCount{6U};
constexpr Size ForeignFrame{HomeFrameCount};
constexpr Size FrameCount{HomeFrameCount + 1U};
constexpr Size PreparationCount{6U};
constexpr Size StepsPerSeed{2400U};
constexpr Size AuditInterval{64U};
constexpr Size ObjectLimit{HomeMemoryObjectCount + HomeFrameCount + SpaceCount};
constexpr Size RuntimeSlots{Kernel::Memory::MaximumRuntimeAddressSpaceMappings};
constexpr Size TotalCapacity{Kernel::Memory::MaximumAddressSpaceMappings};
constexpr Size PolicyCapacity{Kernel::Memory::MaximumFrameMappingPolicyRecords};
constexpr UInt64 PageBytes{Kernel::Memory::MappingPageSizeBytes};
constexpr UInt64 MaximumUInt64{~UInt64{0U}};
constexpr UInt64 RuntimeVirtualBase{0x0000'0040'0000'0000ULL};
constexpr UInt64 RuntimeWindowPages{0x1'0000U};
constexpr UInt64 SeedVirtualBase{0xFFFF'8000'0000'0000ULL};
constexpr UInt64 SeedPhysicalBase{0x0010'0000U};
constexpr UInt64 DeviceSeedPhysicalBase{0x0380'0000U};
constexpr UInt64 TopPageStart{MaximumUInt64 - PageBytes + 1U};
constexpr UInt64 UnissuedIdentity{0x00AB'00C8U}; // Slot 200, generation 0xAB: never issued
constexpr UInt32 UnlimitedPins{Platform::Synchronization::AtomicReferenceCount::MaximumValue};
constexpr Size NoIndex{~Size{0U}};
constexpr UInt8 NoProcessor{0xFFU};
constexpr Foundation::ProcessorMask ReadBit{0x01U};
constexpr Foundation::ProcessorMask WriteBit{0x02U};
constexpr Foundation::ProcessorMask ExecuteBit{0x04U};
constexpr Foundation::ProcessorMask UserBit{0x08U};
constexpr UInt8 KnownPermissionBits{0x0FU};

// TightPins limits every home object to a few references; RuntimeCapacity
// fills the runtime slots of root 1 (behind a full 256-entry seed in every
// other such seed); FramePolicy fills Frame 0's 256 W^X records.
enum class Profile : UInt8
{
    General,
    TightPins,
    RuntimeCapacity,
    FramePolicy,
};

enum class MapOutcome : UInt8
{
    Installed,
    Rejected,
    WrongStart,
    WrongPages,
    ReservedNonzero,
};

template <typename TValue> [[nodiscard]] TValue Take(TValue& slot) noexcept
{
    const TValue value = slot;
    slot = TValue{};
    return value;
}

// Fake native backend: counts calls (claim, release, activate, map) and consumes
// the result that the driver injects before every call that can reach it.
// Rejections leave the output untouched; malformed outcomes report success
// with an inexact invalidation range.
class FakePageTable final : public Hal::Memory::IAddressSpacePageTable
{
public:
    [[nodiscard]] bool IsReady() const noexcept override
    {
        return Ready;
    }

    [[nodiscard]] OwnershipError Claim(const AddressSpace& owner, const InitialMappingPlan&) noexcept override
    {
        ++Calls[0U];
        const OwnershipError result = Take(Ownership);
        Owner = (result == OwnershipError::None) ? &owner : Owner;
        return result;
    }

    [[nodiscard]] OwnershipError Release(const AddressSpace&) noexcept override
    {
        ++Calls[1U];
        const OwnershipError result = Take(Ownership);
        Owner = (result == OwnershipError::None) ? nullptr : Owner;
        return result;
    }

    [[nodiscard]] ActivationError Activate(const AddressSpace&, const CpuId) noexcept override
    {
        ++Calls[2U];
        return Take(ActivationResult);
    }

    [[nodiscard]] PageTableError Map(const Record& mapping,
                                     Hal::Memory::AddressSpacePageTableMapResult& output) noexcept override
    {
        ++Calls[3U];
        LastMapping = mapping;
        const MapOutcome outcome = Take(Outcome);
        const PageTableError error = Take(MapError);
        if (outcome == MapOutcome::Rejected)
        {
            return error;
        }
        output = Hal::Memory::AddressSpacePageTableMapResult{
            VirtualAddress{mapping.VirtualStart.GetValue() + ((outcome == MapOutcome::WrongStart) ? PageBytes : 0U)},
            PageCount{mapping.Pages.GetValue() + ((outcome == MapOutcome::WrongPages) ? 1U : 0U)},
            (outcome == MapOutcome::ReservedNonzero) ? 1U : 0U,
        };
        return PageTableError::None;
    }

    [[nodiscard]] PageTableError Unmap(const Record&, Hal::Memory::AddressSpacePageTableMapResult&) noexcept override
    {
        return PageTableError::UnsupportedConfiguration;
    }

    bool Ready{true};
    OwnershipError Ownership{};
    ActivationError ActivationResult{};
    MapOutcome Outcome{};
    PageTableError MapError{};
    const AddressSpace* Owner{nullptr};
    Size Calls[4U]{};
    Record LastMapping{};
};

struct FrameConfiguration
{
    UInt64 PhysicalStart;
    UInt64 Pages;
    MemoryCachePolicy CachePolicy;
    FrameKind Kind;
};

// Frame 1 names exactly Frame 0's pages with another cache policy and Frame 2
// lies inside both: while one of the three holds reservations, its claim
// refuses the others. Frame 3 ends where Frame 0 begins, so their claims
// coexist. Frame 4 covers seed-owned pages, Frame 5 is device memory, and
// Frame 6 lives in a second registry.
constexpr FrameConfiguration FrameConfigurations[FrameCount]{
    {0x0100'0000U, 32U, MemoryCachePolicy::WriteBack, FrameKind::Ram},
    {0x0100'0000U, 32U, MemoryCachePolicy::WriteThrough, FrameKind::Ram},
    {0x0101'0000U, 8U, MemoryCachePolicy::WriteBack, FrameKind::Ram},
    {0x00FF'0000U, 16U, MemoryCachePolicy::WriteThrough, FrameKind::Ram},
    {SeedPhysicalBase, 64U, MemoryCachePolicy::WriteBack, FrameKind::Ram},
    {0x0300'0000U, 8U, MemoryCachePolicy::Uncached, FrameKind::Device},
    {0x0400'0000U, 16U, MemoryCachePolicy::WriteBack, FrameKind::Ram},
};

struct MemoryObjectConfiguration
{
    UInt64 Pages;
    MemoryCachePolicy CachePolicy;
    UInt8 MaximumPermissions;
    MemoryCommitment Commitment;
};

// Object 3 is not pinned; object 4 lives in the second registry.
constexpr MemoryObjectConfiguration MemoryObjectConfigurations[MemoryObjectCount]{
    {32U, MemoryCachePolicy::WriteBack, KnownPermissionBits, MemoryCommitment::Pinned},
    {16U, MemoryCachePolicy::WriteBack, ReadBit | WriteBit, MemoryCommitment::Pinned},
    {16U, MemoryCachePolicy::WriteThrough, ReadBit | ExecuteBit | UserBit, MemoryCommitment::Pinned},
    {32U, MemoryCachePolicy::WriteBack, KnownPermissionBits, MemoryCommitment::BestEffort},
    {16U, MemoryCachePolicy::WriteBack, KnownPermissionBits, MemoryCommitment::Pinned},
};

template <typename TStep> [[nodiscard]] bool ForAll(const Size count, const TStep& step) noexcept
{
    for (Size index{0U}; index < count; ++index)
    {
        if (!step(index))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr Foundation::ProcessorMask ProcessorBit(const UInt8 processor) noexcept
{
    return static_cast<UInt8>(UInt8{1U} << processor);
}

// Frame policy access: bit 0 writable, bit 1 executable.
[[nodiscard]] constexpr UInt8 AccessOf(const UInt8 permissions) noexcept
{
    return static_cast<UInt8>((permissions >> 1U) & 0x03U);
}

[[nodiscard]] constexpr bool AccessConflicts(const UInt8 first, const UInt8 second) noexcept
{
    return (((first << 1U) & second) != 0U) || (((second << 1U) & first) != 0U);
}

// Both ranges are representable: every modelled range was validated first.
[[nodiscard]] constexpr bool Overlaps(const UInt64 first, const UInt64 firstPages, const UInt64 second,
                                      const UInt64 pages) noexcept
{
    return (first < (second + (pages * PageBytes))) && (second < (first + (firstPages * PageBytes)));
}

[[nodiscard]] constexpr bool Overlaps(const Record& first, const Record& second, const bool physical) noexcept
{
    return physical ? Overlaps(first.PhysicalStart.GetValue(), first.Pages.GetValue(), second.PhysicalStart.GetValue(),
                               second.Pages.GetValue())
                    : Overlaps(first.VirtualStart.GetValue(), first.Pages.GetValue(), second.VirtualStart.GetValue(),
                               second.Pages.GetValue());
}

template <typename TPermissions, typename TPermission>
[[nodiscard]] TPermissions PermissionsFrom(const UInt16 bits) noexcept
{
    TPermissions permissions{};
    for (UInt16 bit{1U}; bit <= 0x80U; bit = static_cast<UInt16>(bit << 1U))
    {
        permissions = ((bits & bit) != 0U) ? permissions.With(static_cast<TPermission>(bit)) : permissions;
    }
    return permissions;
}

// Both backend result enumerations are contiguous from zero.
[[nodiscard]] constexpr bool IsKnown(const ActivationError error) noexcept
{
    return static_cast<UInt8>(error) <= static_cast<UInt8>(ActivationError::CommittedStateUnknown);
}

[[nodiscard]] constexpr bool IsKnown(const PageTableError error) noexcept
{
    return static_cast<UInt8>(error) <= static_cast<UInt8>(PageTableError::InternalFailure);
}

// A link of an ordered rule chain: records code when violated; the && chain stops there.
template <typename TError>
[[nodiscard]] constexpr bool Pass(const bool violated, const TError code, TError& error) noexcept
{
    error = violated ? code : error;
    return !violated;
}

// A chain link for an error computed lazily when the chain reaches it.
template <typename TError> [[nodiscard]] constexpr bool PassError(const TError code, TError& error) noexcept
{
    return Pass(code != TError{}, code, error);
}

// The refusal every AddressSpace entry point derives from the state alone.
[[nodiscard]] constexpr AddressSpaceError StateRefusal(const AddressSpaceState state) noexcept
{
    constexpr AddressSpaceError refusals[7U]{
        AddressSpaceError::NotInitialized,    AddressSpaceError::NotReady,         AddressSpaceError::None,
        AddressSpaceError::ActivationPending, AddressSpaceError::ShootdownPending, AddressSpaceError::Faulted,
        AddressSpaceError::Quiesced};
    return refusals[static_cast<Size>(state)];
}

static_assert(static_cast<Size>(AddressSpaceState::Quiesced) == 6U);

// Maps ObjectNotFound, TypeMismatch, or ReferenceExhausted to the caller's codes.
template <typename TError>
[[nodiscard]] constexpr TError PinFailure(const ObjectPinError error, const TError (&codes)[3]) noexcept
{
    const Size index = static_cast<Size>(error) - static_cast<Size>(ObjectPinError::ObjectNotFound);
    return (index < 3U) ? codes[index] : TError{};
}

// Packs every field of a failure so that one comparison covers all of them.
template <typename TError> [[nodiscard]] constexpr UInt64 Encode(const TError error) noexcept
{
    return static_cast<UInt64>(error);
}

[[nodiscard]] constexpr UInt64 Encode(const AddressSpaceFailure& failure) noexcept
{
    return static_cast<UInt64>(failure.Error) | (static_cast<UInt64>(failure.PinError) << 8U) |
           (static_cast<UInt64>(failure.PageTableError) << 16U);
}

[[nodiscard]] constexpr UInt64 Encode(const AddressSpaceActivationFailure& failure) noexcept
{
    return static_cast<UInt64>(failure.Error) | (static_cast<UInt64>(failure.PageTable) << 8U) |
           (static_cast<UInt64>(failure.ProcessorMayBeResident) << 16U);
}

[[nodiscard]] constexpr UInt64 Encode(const ProcessorAddressSpaceControllerFailure& failure) noexcept
{
    return static_cast<UInt64>(failure.Error) | (Encode(failure.Activation) << 8U) |
           (static_cast<UInt64>(failure.Transition) << 32U) | (static_cast<UInt64>(failure.Detach) << 40U) |
           (static_cast<UInt64>(failure.AdmissionRollbackSafe) << 48U);
}

// Registries precede every object that can hold one of their pins.
struct System
{
    explicit System(const UInt32 pinLimit) noexcept
        : Registry{RegistryLock, pinLimit}, Issuer{Registry}, ForeignRegistry{ForeignLock},
          ForeignIssuer{ForeignRegistry}
    {
    }

    Platform::Synchronization::SpinLock RegistryLock;
    Asteria::TestSupport::TestObjectRegistry Registry;
    RootCapabilityIssuer Issuer;
    Platform::Synchronization::SpinLock ForeignLock;
    Asteria::TestSupport::TestObjectRegistry ForeignRegistry;
    RootCapabilityIssuer ForeignIssuer;
    Kernel::Resource::ResourceDomainLedger Ledger;
    ResourceDomain Domain{Ledger};
    Kernel::Resource::ResourceDomainLedger ForeignLedger;
    ResourceDomain ForeignDomain{ForeignLedger};
    MemoryObject MemoryObjects[MemoryObjectCount];
    BootFrame Frames[FrameCount];
    FakePageTable PageTables[SpaceCount];
    InitialMappingPlan Plans[SpaceCount];
    InitialMappingPlan UnsealedPlan;
    AddressSpace Spaces[SpaceCount];
    PinnedFrameMappingPreparation Preparations[PreparationCount];
    ProcessorAddressSpaceController Controllers[ControllerCount];
};

struct ObjectState
{
    bool Registered;
    bool Quiescing;
    UInt64 Identity;
    UInt64 PreviousIdentity;
    UInt32 Pins;
};

// Frame's private W^X ledger: one record per exact (start, pages, access) key.
struct PolicyRecord
{
    UInt64 Start;
    UInt64 Pages;
    UInt64 References;
    UInt8 Access;
};

// Reservations counts every retained mapping, read-only ones included; while
// it is nonzero the Frame claims its whole range in its registry.
struct FrameModel
{
    ObjectState Object;
    PolicyRecord Policies[PolicyCapacity];
    Size PolicyCount;
    UInt64 Reservations;
};

struct MemoryObjectModel
{
    ObjectState Object;
    MemoryObjectState State;
    UInt64 ActiveMappings;
};

// The exact record the root exports, plus the objects a runtime record retains.
struct MappingModel
{
    Record Value;
    Size MemoryObject;
    Size Frame;
};

struct SpaceModel
{
    ObjectState Object;
    UInt64 ChargeOwner;
    UInt64 BoundIdentity;
    AddressSpaceState State;
    Size SeedCount;
    Size RuntimeCount;
    bool ProvisionalRetained; // The hidden record of an in-flight or terminally retained transaction
    MappingModel Provisional;
    UInt64 MappingSequence;
    UInt64 ActivationSequence;
    UInt64 TransitionSequence;
    UInt64 ReservedSequence;
    UInt64 ProcessorReservations[ProcessorLimit]{};
    UInt8 TransitionProcessor{NoProcessor};
    Foundation::ProcessorMask ResidentMask;
    UInt64 ShootdownSequence;
    Foundation::ProcessorMask TargetMask;
    Foundation::ProcessorMask AcknowledgedMask;
    bool ShootdownRejected;
    bool BackendOwned;     // Backend history survives storage reuse
    Size BackendCalls[4U]; // Claim, release, activate, map
    MappingModel Mappings[TotalCapacity];
};

struct PreparationModel
{
    bool Prepared;
    bool Foreign;
    MappingModel Mapping;
};

struct ReceiptModel
{
    bool Valid;
    Size Space;
    UInt64 Sequence;
    UInt8 Processor;
};

struct ControllerModel
{
    ControllerState State;
    UInt8 Processor{NoProcessor};
    Size Idle{NoIndex};
    Size Active{NoIndex};
    Size Previous{NoIndex};
    UInt64 IdleIdentity;
    UInt64 ActiveIdentity;
    UInt64 PreviousIdentity;
    ReceiptModel Source;
    ReceiptModel Destination;
};

struct ActivationOutcome
{
    bool Success;
    UInt64 Sequence;
    AddressSpaceActivationFailure Failure;
};

[[nodiscard]] constexpr ActivationOutcome Refused(const AddressSpaceError error, const ActivationError pageTable,
                                                  const bool mayBeResident = false) noexcept
{
    return ActivationOutcome{false, 0U, AddressSpaceActivationFailure{error, pageTable, mayBeResident, {}}};
}

struct MapInjection
{
    MapOutcome Outcome;
    PageTableError Error;
};

enum class BeginEffect : UInt8
{
    Rejected,          // Refused before the backend; every provisional resource returned
    Published,         // Backend success: ShootdownPending with resources retained
    RolledBack,        // Ordinary backend rejection: resources returned
    RolledBackFaulted, // Corrupt or unready backend: resources returned, root terminal
    Retained,          // Possible native write: resources kept until Quiesce, root terminal
};

struct BeginExpectation
{
    AddressSpaceFailure Failure;
    BeginEffect Effect;
};

struct ControllerExpectation
{
    bool Success;
    ProcessorAddressSpaceControllerFailure Failure;
    UInt64 Previous;
    UInt64 Active;
    UInt8 Processor;
    bool NativeActivation;
};

[[nodiscard]] constexpr ControllerExpectation
ControllerRefusal(const ControllerError error, const bool rollbackSafe,
                  const AddressSpaceError transition = AddressSpaceError::None,
                  const AddressSpaceActivationFailure activation = AddressSpaceActivationFailure{}) noexcept
{
    const ProcessorAddressSpaceControllerFailure failure{error,        activation, transition, AddressSpaceError::None,
                                                         rollbackSafe, {}};
    return ControllerExpectation{false, failure, 0U, 0U, NoProcessor, false};
}

class Model final
{
public:
    Model(const Profile mode, const UInt32 pinLimit) noexcept : Mode{mode}, PinLimit{pinLimit}
    {
    }

    Profile Mode;
    UInt32 PinLimit;
    UInt64 DomainIdentities[2U]{}; // Home, foreign
    MemoryObjectModel MemoryObjects[MemoryObjectCount]{};
    FrameModel Frames[FrameCount]{};
    SpaceModel Spaces[SpaceCount]{};
    PreparationModel Preparations[PreparationCount]{};
    ControllerModel Controllers[ControllerCount]{};

    // A freshly constructed AddressSpace; registry and backend history persist.
    void ResetSpace(const Size index) noexcept
    {
        SpaceModel& space = Spaces[index];
        space.ChargeOwner = 0U;
        space.BoundIdentity = 0U;
        space.State = AddressSpaceState::Uninitialized;
        space.SeedCount = 0U;
        space.RuntimeCount = 0U;
        space.ProvisionalRetained = false;
        space.MappingSequence = 0U;
        space.ActivationSequence = 0U;
        space.TransitionSequence = 0U;
        space.ReservedSequence = 0U;
        for (auto& reservation : space.ProcessorReservations)
        {
            reservation = 0U;
        }
        space.TransitionProcessor = NoProcessor;
        space.ResidentMask = 0U;
        ClearShootdown(space);
    }

    static void ClearShootdown(SpaceModel& space) noexcept
    {
        space.ShootdownSequence = 0U;
        space.TargetMask = 0U;
        space.AcknowledgedMask = 0U;
        space.ShootdownRejected = false;
    }

    [[nodiscard]] ObjectState* ObjectAt(const Size kind, const Size index) noexcept
    {
        ObjectState* const objects[4U]{
            nullptr,
            (index < MemoryObjectCount) ? &MemoryObjects[index].Object : nullptr,
            (index < FrameCount) ? &Frames[index].Object : nullptr,
            (index < SpaceCount) ? &Spaces[index].Object : nullptr,
        };
        return (kind < 4U) ? objects[kind] : nullptr;
    }

    // The live object (kind, index) that an identity names in one registry.
    [[nodiscard]] Size FindLive(const bool foreign, const UInt64 identity, Size& kind) noexcept
    {
        const Size counts[4U]{0U, MemoryObjectCount, FrameCount, foreign ? 0U : SpaceCount};
        for (kind = 1U; kind < 4U; ++kind)
        {
            for (Size index{0U}; index < counts[kind]; ++index)
            {
                const ObjectState& object = *ObjectAt(kind, index);
                const bool foreignObject =
                    ((kind == 1U) && (index == ForeignMemoryObject)) || ((kind == 2U) && (index == ForeignFrame));
                if ((foreignObject == foreign) && object.Registered && !object.Quiescing &&
                    (object.Identity == identity))
                {
                    return index;
                }
            }
        }
        kind = (identity == DomainIdentities[static_cast<Size>(foreign)]) ? 4U : 0U;
        return NoIndex;
    }

    // ObjectRegistry::PinObject: lookup, type, then the home registry's reference limit.
    [[nodiscard]] ObjectPinError PredictPin(const bool foreign, const UInt64 identity, const ObjectType type) noexcept
    {
        constexpr ObjectType types[5U]{ObjectType::Invalid, ObjectType::MemoryObject, ObjectType::Frame,
                                       ObjectType::AddressSpace, ObjectType::ResourceDomain};
        Size kind{0U};
        const Size index = FindLive(foreign, identity, kind);
        ObjectPinError error{ObjectPinError::Ok};
        static_cast<void>(
            Pass(kind == 0U, ObjectPinError::ObjectNotFound, error) &&
            Pass(types[kind] != type, ObjectPinError::TypeMismatch, error) &&
            Pass(!foreign && (ObjectAt(kind, index)->Pins >= PinLimit), ObjectPinError::ReferenceExhausted, error));
        return error;
    }

    // Home objects in a fixed order: memory objects 0-3, Frames 0-5, roots 0-2.
    [[nodiscard]] ObjectState& HomeObject(const Size candidate, Size& kind, Size& index) noexcept
    {
        kind = 1U + static_cast<Size>(candidate >= 4U) + static_cast<Size>(candidate >= 10U);
        index = candidate - (4U * static_cast<Size>(candidate >= 4U)) - (6U * static_cast<Size>(candidate >= 10U));
        return *ObjectAt(kind, index);
    }

    // CollectDestroyedObject returns the pin-free quiescing entry with the lowest slot.
    [[nodiscard]] ObjectState* CollectCandidate(Size& kind, Size& index) noexcept
    {
        ObjectState* best{nullptr};
        for (Size candidate{0U}; candidate < ObjectLimit; ++candidate)
        {
            Size candidateKind{0U};
            Size candidateIndex{0U};
            ObjectState& object = HomeObject(candidate, candidateKind, candidateIndex);
            if (object.Registered && object.Quiescing && (object.Pins == 0U) &&
                ((best == nullptr) || ((object.Identity & 0xFFFFU) < (best->Identity & 0xFFFFU))))
            {
                best = &object;
                kind = candidateKind;
                index = candidateIndex;
            }
        }
        return best;
    }

    [[nodiscard]] Size ActiveObjectCount() noexcept
    {
        Size count{1U};
        for (Size candidate{0U}; candidate < ObjectLimit; ++candidate)
        {
            Size kind{0U};
            Size index{0U};
            const ObjectState& object = HomeObject(candidate, kind, index);
            count += (object.Registered && !object.Quiescing) ? 1U : 0U;
        }
        return count;
    }

    [[nodiscard]] PolicyRecord* FindPolicy(const Size frame, const UInt64 start, const UInt64 pages,
                                           const UInt8 access) noexcept
    {
        FrameModel& model = Frames[frame];
        for (Size index{0U}; index < model.PolicyCount; ++index)
        {
            PolicyRecord& record = model.Policies[index];
            if ((record.Start == start) && (record.Pages == pages) && (record.Access == access))
            {
                return &record;
            }
        }
        return nullptr;
    }

    // A Frame without reservations must first claim its range, which an overlapping
    // claim of another Frame of the same registry refuses.
    [[nodiscard]] bool ClaimConflicts(const Size frame) const noexcept
    {
        const FrameConfiguration& candidate = FrameConfigurations[frame];
        for (Size other{0U}; other < FrameCount; ++other)
        {
            const FrameConfiguration& holder = FrameConfigurations[other];
            if ((other != frame) && ((other == ForeignFrame) == (frame == ForeignFrame)) &&
                (Frames[other].Reservations != 0U) &&
                Overlaps(candidate.PhysicalStart, candidate.Pages, holder.PhysicalStart, holder.Pages))
            {
                return true;
            }
        }
        return false;
    }

    // Frame::ReserveMappingPolicy as the root reports it: a Frame without reservations holds no
    // records, so its claim is the only check that can refuse it.
    [[nodiscard]] AddressSpaceError PolicyError(const MappingModel& mapping) noexcept
    {
        const Record& value = mapping.Value;
        const UInt8 access = AccessOf(value.Permissions.GetBits());
        const FrameModel& frame = Frames[mapping.Frame];
        if ((frame.Reservations == 0U) && ClaimConflicts(mapping.Frame))
        {
            return AddressSpaceError::PhysicalOwnershipConflict;
        }
        for (Size index{0U}; (access != 0U) && (index < frame.PolicyCount); ++index)
        {
            const PolicyRecord& record = frame.Policies[index];
            if (Overlaps(value.PhysicalStart.GetValue(), value.Pages.GetValue(), record.Start, record.Pages) &&
                AccessConflicts(access, record.Access))
            {
                return AddressSpaceError::PhysicalWriteExecuteAlias;
            }
        }
        const bool full =
            (access != 0U) && (frame.PolicyCount >= PolicyCapacity) &&
            (FindPolicy(mapping.Frame, value.PhysicalStart.GetValue(), value.Pages.GetValue(), access) == nullptr);
        return full ? AddressSpaceError::MappingCapacityExceeded : AddressSpaceError::None;
    }

    // A retained mapping holds a MemoryObject pin and reference, a Frame pin and reservation,
    // and its W^X record.
    void Retain(const MappingModel& mapping, const bool retain) noexcept
    {
        const UInt64 step = retain ? 1U : MaximumUInt64;
        MemoryObjects[mapping.MemoryObject].Object.Pins += static_cast<UInt32>(step);
        MemoryObjects[mapping.MemoryObject].ActiveMappings += step;
        Frames[mapping.Frame].Object.Pins += static_cast<UInt32>(step);
        Frames[mapping.Frame].Reservations += step;
        const UInt8 access = AccessOf(mapping.Value.Permissions.GetBits());
        const UInt64 start = mapping.Value.PhysicalStart.GetValue();
        FrameModel& frame = Frames[mapping.Frame];
        PolicyRecord* const record = FindPolicy(mapping.Frame, start, mapping.Value.Pages.GetValue(), access);
        if (access == 0U)
        {
            return;
        }
        if (record == nullptr)
        {
            frame.Policies[frame.PolicyCount] = PolicyRecord{start, mapping.Value.Pages.GetValue(), 1U, access};
            ++frame.PolicyCount;
            return;
        }
        record->References += step;
        if (record->References == 0U)
        {
            --frame.PolicyCount;
            *record = frame.Policies[frame.PolicyCount];
        }
    }

    [[nodiscard]] AddressSpaceError Reserve(const Size index, const UInt8 processor, UInt64& sequence) noexcept
    {
        SpaceModel& space = Spaces[index];
        const AddressSpaceState state = space.State;
        AddressSpaceError error{AddressSpaceError::None};
        if (Pass(processor >= ProcessorLimit, AddressSpaceError::InvalidProcessor, error) &&
            Pass((state == AddressSpaceState::Uninitialized) || (state == AddressSpaceState::Quiesced),
                 StateRefusal(state), error) &&
            Pass(space.ProcessorReservations[processor] != 0U, AddressSpaceError::ProcessorTransitionPending, error) &&
            Pass(state != AddressSpaceState::Ready, StateRefusal(state), error))
        {
            sequence = ++space.TransitionSequence;
            space.ReservedSequence = sequence;
            space.ProcessorReservations[processor] = sequence;
            space.TransitionProcessor = processor;
        }
        return error;
    }

    [[nodiscard]] bool IsCurrent(const Size index, const ReceiptModel& receipt) const noexcept
    {
        const SpaceModel& space = Spaces[index];
        return receipt.Valid && (receipt.Space == index) && receipt.Processor < ProcessorLimit &&
               (receipt.Sequence == space.ProcessorReservations[receipt.Processor]);
    }

    // Bare (receipt null) or reserved activation. The sequence is consumed before the
    // backend call; an ambiguous commit keeps conservative residency and faults the root.
    [[nodiscard]] ActivationOutcome Activate(const Size index, const UInt8 bareProcessor,
                                             const ReceiptModel* const receipt, const ActivationError backend) noexcept
    {
        SpaceModel& space = Spaces[index];
        const UInt8 processor = (receipt != nullptr) ? receipt->Processor : bareProcessor;
        const Foundation::ProcessorMask bit = (processor < ProcessorLimit) ? ProcessorBit(processor) : UInt8{0U};
        const ActivationOutcome refusals[5U]{
            Refused(AddressSpaceError::InvalidProcessorTransitionReceipt, ActivationError::RejectedBeforeCommit),
            Refused(AddressSpaceError::InvalidProcessor, ActivationError::RejectedBeforeCommit),
            Refused(AddressSpaceError::ProcessorTransitionPending, ActivationError::NotReady),
            Refused(StateRefusal(space.State), ActivationError::NotReady),
            Refused(AddressSpaceError::ProcessorAlreadyResident, ActivationError::RejectedBeforeCommit)};
        const bool refused[5U]{(receipt != nullptr) && !IsCurrent(index, *receipt), (receipt == nullptr) && (bit == 0U),
                               (receipt == nullptr) && (space.ReservedSequence != 0U),
                               StateRefusal(space.State) != AddressSpaceError::None, (space.ResidentMask & bit) != 0U};
        for (Size check{0U}; check < 5U; ++check)
        {
            if (refused[check])
            {
                return refusals[check];
            }
        }
        ++space.ActivationSequence;
        ++space.BackendCalls[2U];
        if (IsKnown(backend) && (backend != ActivationError::None) &&
            (backend != ActivationError::CommittedStateUnknown))
        {
            return Refused(AddressSpaceError::PageTableActivationRejected, backend);
        }
        space.ResidentMask = static_cast<UInt8>(space.ResidentMask | bit);
        if (backend == ActivationError::None)
        {
            return ActivationOutcome{true, space.ActivationSequence, {}};
        }
        space.State = AddressSpaceState::Faulted;
        return IsKnown(backend) ? Refused(AddressSpaceError::PageTableActivationCommittedStateUnknown, backend, true)
                                : Refused(AddressSpaceError::PageTableProtocolViolation,
                                          ActivationError::CommittedStateUnknown, true);
    }

    // Bare (receipt null) or reserved detach; a Faulted root admits it.
    [[nodiscard]] AddressSpaceError Detach(const Size index, const UInt8 bareProcessor,
                                           const ReceiptModel* const receipt) noexcept
    {
        SpaceModel& space = Spaces[index];
        const AddressSpaceState state = space.State;
        const UInt8 processor = (receipt != nullptr) ? receipt->Processor : bareProcessor;
        AddressSpaceError error{AddressSpaceError::None};
        const bool authorized =
            (receipt != nullptr)
                ? Pass(!IsCurrent(index, *receipt), AddressSpaceError::InvalidProcessorTransitionReceipt, error)
                : (Pass(processor >= ProcessorLimit, AddressSpaceError::InvalidProcessor, error) &&
                   Pass(space.ReservedSequence != 0U, AddressSpaceError::ProcessorTransitionPending, error));
        if (authorized &&
            Pass((state != AddressSpaceState::Faulted) && (StateRefusal(state) != AddressSpaceError::None),
                 StateRefusal(state), error) &&
            Pass((space.ResidentMask & ProcessorBit(processor)) == 0U, AddressSpaceError::ProcessorNotResident, error))
        {
            space.ResidentMask = static_cast<UInt8>(space.ResidentMask & static_cast<UInt8>(~ProcessorBit(processor)));
        }
        return error;
    }

    [[nodiscard]] AddressSpaceError Release(const Size index, const ReceiptModel& receipt) noexcept
    {
        if (!IsCurrent(index, receipt))
        {
            return AddressSpaceError::InvalidProcessorTransitionReceipt;
        }
        auto& space = Spaces[index];
        space.ProcessorReservations[receipt.Processor] = 0U;
        space.ReservedSequence = 0U;
        space.TransitionProcessor = NoProcessor;
        for (UInt8 cpu{0U}; cpu < ProcessorLimit; ++cpu)
        {
            if (space.ProcessorReservations[cpu] != 0U)
            {
                space.ReservedSequence = space.ProcessorReservations[cpu];
                space.TransitionProcessor = cpu;
            }
        }
        return AddressSpaceError::None;
    }

    // Quiesce releases every runtime reference, including a provisional record
    // retained after an ambiguous commit or failed shootdown. There is no
    // unmap or protect: a W^X reservation ends only on an ordinary backend
    // rejection or here. With no resident CPU it also retires a reservation.
    [[nodiscard]] AddressSpaceError Quiesce(const Size index, const bool releaseFails) noexcept
    {
        SpaceModel& space = Spaces[index];
        const AddressSpaceState state = space.State;
        AddressSpaceError error{AddressSpaceError::None};
        if (!(Pass((state == AddressSpaceState::Uninitialized) || (state == AddressSpaceState::Quiesced) ||
                       (state == AddressSpaceState::ShootdownPending),
                   StateRefusal(state), error) &&
              Pass((space.ReservedSequence != 0U) && (space.ResidentMask != 0U),
                   AddressSpaceError::ProcessorTransitionPending, error) &&
              Pass(space.ResidentMask != 0U, AddressSpaceError::ProcessorsStillResident, error)))
        {
            return error;
        }
        ++space.BackendCalls[1U];
        if (releaseFails)
        {
            return AddressSpaceError::PageTableProtocolViolation;
        }
        for (Size position{space.SeedCount}; position < (space.SeedCount + space.RuntimeCount); ++position)
        {
            Retain(space.Mappings[position], false);
        }
        if (space.ProvisionalRetained)
        {
            Retain(space.Provisional, false);
        }
        const UInt64 chargeOwner = space.ChargeOwner;
        ResetSpace(index);
        space.ChargeOwner = chargeOwner;
        space.BackendOwned = false;
        space.State = AddressSpaceState::Quiesced;
        return AddressSpaceError::None;
    }

    // The per-root ledger in record order. Each record reports its first violation: virtual
    // overlap; then, for physical overlap, another owner (seed, Frame, kind), W^X. Records
    // of one Frame carry its single cache policy, so no cache alias can remain to report.
    [[nodiscard]] static AddressSpaceError LedgerError(const SpaceModel& space, const Record& mapping) noexcept
    {
        AddressSpaceError error{AddressSpaceError::None};
        for (Size position{0U}; position < (space.SeedCount + space.RuntimeCount); ++position)
        {
            const Record& existing = space.Mappings[position].Value;
            const bool physical = Overlaps(mapping, existing, true);
            if (!(Pass(Overlaps(mapping, existing, false), AddressSpaceError::VirtualOverlap, error) &&
                  Pass(physical && (!existing.RuntimeMapping || !existing.Frame.IsEqualTo(mapping.Frame) ||
                                    (existing.Kind != mapping.Kind)),
                       AddressSpaceError::PhysicalOwnershipConflict, error) &&
                  Pass(physical && AccessConflicts(AccessOf(mapping.Permissions.GetBits()),
                                                   AccessOf(existing.Permissions.GetBits())),
                       AddressSpaceError::PhysicalWriteExecuteAlias, error)))
            {
                break;
            }
        }
        return error;
    }

    [[nodiscard]] static bool PinPass(const ObjectPinError pin, const AddressSpaceError (&codes)[3],
                                      AddressSpaceFailure& failure) noexcept
    {
        failure.PinError = pin;
        return Pass(pin != ObjectPinError::Ok, PinFailure(pin, codes), failure.Error);
    }

    // BeginPinnedFrameMapping without side effects.
    [[nodiscard]] BeginExpectation PredictBegin(const Size index, const PreparationModel& preparation,
                                                const bool targetMatches, const MapInjection injection) noexcept
    {
        const SpaceModel& space = Spaces[index];
        const MappingModel& mapping = preparation.Mapping;
        AddressSpaceFailure failure{};
        const bool accepted =
            Pass(space.ReservedSequence != 0U, AddressSpaceError::ProcessorTransitionPending, failure.Error) &&
            PassError(StateRefusal(space.State), failure.Error) &&
            Pass(!preparation.Prepared, AddressSpaceError::InvalidMappingPlan, failure.Error) &&
            Pass(!targetMatches, AddressSpaceError::AddressSpaceMismatch, failure.Error) &&
            Pass(preparation.Foreign, AddressSpaceError::MemoryObjectRegistryMismatch, failure.Error) &&
            Pass((space.RuntimeCount >= RuntimeSlots) || ((space.SeedCount + space.RuntimeCount) >= TotalCapacity),
                 AddressSpaceError::MappingCapacityExceeded, failure.Error) &&
            PassError(LedgerError(space, mapping.Value), failure.Error) &&
            PinPass(PredictPin(false, mapping.Value.MemoryObject.GetValue(), ObjectType::MemoryObject),
                    {AddressSpaceError::MemoryObjectNotFound, AddressSpaceError::MemoryObjectTypeMismatch,
                     AddressSpaceError::MemoryObjectPinUnavailable},
                    failure) &&
            Pass(MemoryObjects[mapping.MemoryObject].State != MemoryObjectState::Ready,
                 AddressSpaceError::MemoryObjectRejected, failure.Error) &&
            PinPass(PredictPin(false, mapping.Value.Frame.GetValue(), ObjectType::Frame),
                    {AddressSpaceError::FrameNotFound, AddressSpaceError::FrameTypeMismatch,
                     AddressSpaceError::FramePinUnavailable},
                    failure) &&
            PassError(PolicyError(mapping), failure.Error);
        const PageTableError error = injection.Error;
        if (!accepted || (injection.Outcome == MapOutcome::Installed))
        {
            return BeginExpectation{failure, accepted ? BeginEffect::Published : BeginEffect::Rejected};
        }
        const bool rejected = injection.Outcome == MapOutcome::Rejected;
        if (!rejected || !IsKnown(error) || (error == PageTableError::CommittedStateUnknown))
        {
            failure.Error = AddressSpaceError::PageTableProtocolViolation;
            failure.PageTableError = rejected ? PageTableError::CommittedStateUnknown : PageTableError::None;
            return BeginExpectation{failure, BeginEffect::Retained};
        }
        failure.Error = AddressSpaceError::PageTableRejected;
        failure.PageTableError = error;
        const bool terminal = (error == PageTableError::NotReady) || (error == PageTableError::CorruptState) ||
                              (error == PageTableError::InternalFailure);
        return BeginExpectation{failure, terminal ? BeginEffect::RolledBackFaulted : BeginEffect::RolledBack};
    }

    // Publication and retention consume the sequence; publication targets the residents.
    void ApplyBegin(const Size index, const MappingModel& mapping, const BeginEffect effect) noexcept
    {
        SpaceModel& space = Spaces[index];
        const bool retained = (effect == BeginEffect::Published) || (effect == BeginEffect::Retained);
        space.BackendCalls[3U] += (effect == BeginEffect::Rejected) ? 0U : 1U;
        if (retained)
        {
            Retain(mapping, true);
            space.Provisional = mapping;
            space.ProvisionalRetained = true;
            ++space.MappingSequence;
        }
        if (effect == BeginEffect::Published)
        {
            space.ShootdownSequence = space.MappingSequence;
            space.TargetMask = space.ResidentMask;
            space.State = AddressSpaceState::ShootdownPending;
        }
        const bool terminal = (effect == BeginEffect::Retained) || (effect == BeginEffect::RolledBackFaulted);
        space.State = terminal ? AddressSpaceState::Faulted : space.State;
    }

    // The first outcome reported by each CPU is final.
    [[nodiscard]] AddressSpaceError Report(const Size index, const AddressSpaceMappingToken& token, const Size owner,
                                           const UInt8 processor, const bool reject) noexcept
    {
        SpaceModel& space = Spaces[index];
        const Foundation::ProcessorMask bit = (processor < ProcessorLimit) ? ProcessorBit(processor) : UInt8{0U};
        AddressSpaceError error{AddressSpaceError::None};
        if (!(Pass(processor >= ProcessorLimit, AddressSpaceError::InvalidProcessor, error) &&
              Pass(!token.IsValid() || (owner != index) || (space.ShootdownSequence != token.GetSequence()),
                   AddressSpaceError::InvalidShootdownToken, error) &&
              Pass((space.TargetMask & bit) == 0U, AddressSpaceError::ProcessorNotTargeted, error)))
        {
            return error;
        }
        const bool fresh = (space.AcknowledgedMask & bit) == 0U;
        space.AcknowledgedMask = static_cast<UInt8>(space.AcknowledgedMask | bit);
        space.ShootdownRejected = space.ShootdownRejected || (fresh && reject);
        return (space.ShootdownRejected && !(fresh && reject)) ? AddressSpaceError::ShootdownFailed
                                                               : AddressSpaceError::None;
    }

    // CompleteMapping or (fail) FailShootdown; any rejection is terminal.
    [[nodiscard]] AddressSpaceError Finish(const Size index, const AddressSpaceMappingToken& token, const Size owner,
                                           const bool fail) noexcept
    {
        SpaceModel& space = Spaces[index];
        const AddressSpaceError refusal = StateRefusal(space.State);
        AddressSpaceError error{AddressSpaceError::None};
        if (!(Pass(space.State != AddressSpaceState::ShootdownPending,
                   (refusal == AddressSpaceError::None) ? AddressSpaceError::NotReady : refusal, error) &&
              Pass(!token.IsValid() || (owner != index) || (token.GetSequence() != space.MappingSequence),
                   AddressSpaceError::InvalidShootdownToken, error) &&
              Pass(!fail && (space.AcknowledgedMask != space.TargetMask), AddressSpaceError::ShootdownIncomplete,
                   error)))
        {
            return error;
        }
        const bool terminal = fail || space.ShootdownRejected;
        ClearShootdown(space);
        space.State = terminal ? AddressSpaceState::Faulted : AddressSpaceState::Ready;
        if (!terminal)
        {
            space.Mappings[space.SeedCount + space.RuntimeCount] = space.Provisional;
            ++space.RuntimeCount;
            space.ProvisionalRetained = false;
        }
        return (terminal && !fail) ? AddressSpaceError::ShootdownFailed : AddressSpaceError::None;
    }

    // Controller branches reachable only after an invariant violation are not
    // modelled; a mutant that takes one still mismatches the prediction.

    [[nodiscard]] static ControllerExpectation ControllerSuccess(const ControllerModel& controller,
                                                                 const UInt64 previous, const bool native) noexcept
    {
        return ControllerExpectation{true, {}, previous, controller.ActiveIdentity, controller.Processor, native};
    }

    [[nodiscard]] ControllerExpectation ControllerInitialize(const Size index, const UInt8 processor, const Size idle,
                                                             const ActivationError backend) noexcept
    {
        ControllerModel& controller = Controllers[index];
        const bool fresh = controller.State == ControllerState::Uninitialized;
        if (!fresh || (processor >= ProcessorLimit))
        {
            const ControllerError error =
                fresh ? ControllerError::InvalidConfiguration : ControllerError::AlreadyInitialized;
            return ControllerRefusal(error, fresh);
        }
        UInt64 sequence{0U};
        const AddressSpaceError reserve = Reserve(idle, processor, sequence);
        if (reserve != AddressSpaceError::None)
        {
            return ControllerRefusal(ControllerError::TransitionRejected, true, reserve);
        }
        const ReceiptModel receipt{true, idle, sequence, processor};
        const ActivationOutcome activation = Activate(idle, processor, &receipt, backend);
        const bool resident = activation.Success || activation.Failure.ProcessorMayBeResident;
        if (resident)
        {
            const UInt64 identity = Spaces[idle].BoundIdentity;
            const ReceiptModel retained = activation.Success ? ReceiptModel{} : receipt;
            controller = ControllerModel{activation.Success ? ControllerState::Ready : ControllerState::Faulted,
                                         processor,
                                         idle,
                                         idle,
                                         NoIndex,
                                         identity,
                                         identity,
                                         0U,
                                         {},
                                         retained};
        }
        if (activation.Success || !resident)
        {
            static_cast<void>(Release(idle, receipt));
        }
        return activation.Success ? ControllerSuccess(controller, 0U, true)
                                  : ControllerRefusal(ControllerError::ActivationRejected, !resident,
                                                      AddressSpaceError::None, activation.Failure);
    }

    // Reserve source, then destination separately; activate, detach the source, release
    // both. Nothing waits: a conflict is a rollback-safe TransitionRejected.
    [[nodiscard]] ControllerExpectation ControllerSwitch(const Size index, const Size destination,
                                                         const ActivationError backend) noexcept
    {
        ControllerModel& controller = Controllers[index];
        if (controller.State != ControllerState::Ready)
        {
            const bool faulted = controller.State == ControllerState::Faulted;
            return ControllerRefusal(faulted ? ControllerError::Faulted : ControllerError::NotReady, !faulted);
        }
        const Size source = controller.Active;
        const UInt64 sourceIdentity = controller.ActiveIdentity;
        UInt64 sequence{0U};
        const AddressSpaceError reserve = Reserve(source, controller.Processor, sequence);
        if (reserve != AddressSpaceError::None)
        {
            const bool contention = (reserve == AddressSpaceError::ProcessorTransitionPending) ||
                                    (reserve == AddressSpaceError::ActivationPending) ||
                                    (reserve == AddressSpaceError::ShootdownPending);
            controller.State = contention ? ControllerState::Ready : ControllerState::Faulted;
            return ControllerRefusal(contention ? ControllerError::TransitionRejected
                                                : ControllerError::ActiveRootInvariantViolation,
                                     contention, reserve);
        }
        const ReceiptModel sourceReceipt{true, source, sequence, controller.Processor};
        const AddressSpaceError destinationReserve =
            (destination == source) ? AddressSpaceError::None : Reserve(destination, controller.Processor, sequence);
        if ((destination == source) || (destinationReserve != AddressSpaceError::None))
        {
            static_cast<void>(Release(source, sourceReceipt));
            return (destination == source)
                       ? ControllerSuccess(controller, sourceIdentity, false)
                       : ControllerRefusal(ControllerError::TransitionRejected, true, destinationReserve);
        }
        const ReceiptModel destinationReceipt{true, destination, sequence, controller.Processor};
        const ActivationOutcome activation = Activate(destination, controller.Processor, &destinationReceipt, backend);
        if (activation.Success || activation.Failure.ProcessorMayBeResident)
        {
            controller.Active = destination;
            controller.ActiveIdentity = Spaces[destination].BoundIdentity;
        }
        if (!activation.Success && activation.Failure.ProcessorMayBeResident)
        {
            controller.State = ControllerState::Faulted;
            controller.Previous = source;
            controller.PreviousIdentity = sourceIdentity;
            controller.Source = sourceReceipt;
            controller.Destination = destinationReceipt;
            return ControllerRefusal(ControllerError::ActivationRejected, false, AddressSpaceError::None,
                                     activation.Failure);
        }
        if (activation.Success)
        {
            static_cast<void>(Detach(source, 0U, &sourceReceipt));
        }
        static_cast<void>(Release(source, sourceReceipt));
        static_cast<void>(Release(destination, destinationReceipt));
        return activation.Success ? ControllerSuccess(controller, sourceIdentity, true)
                                  : ControllerRefusal(ControllerError::ActivationRejected, true,
                                                      AddressSpaceError::None, activation.Failure);
    }

    [[nodiscard]] bool References(const Size controller, const Size space) const noexcept
    {
        const ControllerModel& model = Controllers[controller];
        return (model.Idle == space) || (model.Active == space) || (model.Previous == space) ||
               (model.Source.Valid && (model.Source.Space == space)) ||
               (model.Destination.Valid && (model.Destination.Space == space));
    }
};

struct IdentityInput
{
    bool Foreign;
    UInt64 Value; // Zero stands for an invalid identity
};

struct PrepareInputs
{
    bool Foreign;
    IdentityInput MemoryObject;
    Size MemoryObjectInstance;
    IdentityInput Frame;
    Size FrameInstance;
    UInt64 VirtualStart;
    UInt64 Pages;
    UInt64 MemoryObjectOffset;
    UInt64 FrameOffset;
    UInt8 Permissions;
    Size ReservedIndex;
    UInt8 ReservedByte;
};

// Target and binding choices: 0 is the exact identity; the others are an
// invalid, a foreign-registry, another root's, a Frame's, or an unissued one.
constexpr Size ExactIdentity{0U};

enum class ReceiptCall : UInt8
{
    Activate,
    Detach,
    Release,
};

struct IssuedToken
{
    AddressSpaceMappingToken Token;
    Size Space;
};

struct HeldReceipt
{
    Kernel::Memory::AddressSpaceTransitionReceipt Receipt;
    ReceiptModel Model;
};

class Driver final
{
public:
    Driver(System& system, Model& model, const UInt64 seed) noexcept
        : m_system{system}, m_model{model}, m_random{seed}, m_seed{seed},
          m_faultPercent{((model.Mode == Profile::General) || (model.Mode == Profile::TightPins)) ? 6U : 2U}
    {
    }

    [[nodiscard]] bool Initialize() noexcept
    {
        const Kernel::Resource::ResourceDomainLimits limits{1024U, 1024U, 1U << 20U, 64U, 0U, 0U, 0U};
        ResourceDomain* const domains[2U]{&m_system.Domain, &m_system.ForeignDomain};
        return ForAll(2U,
                      [&](const Size index) noexcept
                      {
                          const auto initialized =
                              domains[index]->InitializeRoot(ResourceDomainId{0x40U + index}, limits);
                          RootCapabilityIssuer& issuer = (index == 0U) ? m_system.Issuer : m_system.ForeignIssuer;
                          const auto created = issuer.CreateObject(*domains[index]);
                          m_model.DomainIdentities[index] = created.HasValue() ? created.GetValue().GetValue() : 0U;
                          return Same("domain", UInt64{0U}, static_cast<UInt64>(initialized.Error)) &&
                                 Same("domain registration", true, created.HasValue());
                      }) &&
               ForAll(MemoryObjectCount,
                      [this](const Size index) noexcept
                      {
                          return CreateMemoryObject(index);
                      }) &&
               ForAll(FrameCount,
                      [this](const Size index) noexcept
                      {
                          return CreateFrame(index);
                      }) &&
               ForAll(SpaceCount,
                      [this](const Size index) noexcept
                      {
                          return InitializeSpace(index, false) && BindSpace(index, ExactIdentity);
                      }) &&
               RunControllerInitialize(0U, 0U, 0U, false) && RunControllerInitialize(1U, 1U, 0U, false) && CheckAll();
    }

    [[nodiscard]] bool CheckPrivateIdleRoots() noexcept
    {
        auto& owner = m_system.Controllers[0U];
        return owner.AdoptIdleRoot(m_system.Spaces[1U]) && owner.GetSnapshot().IdleStorage == &m_system.Spaces[1U] &&
               m_system.Controllers[1U].GetSnapshot().ActiveStorage == &m_system.Spaces[0U] &&
               owner.SwitchTo(m_system.Spaces[2U]).HasValue() && !owner.AdoptIdleRoot(m_system.Spaces[3U]) &&
               owner.SwitchToIdle().HasValue() && owner.GetSnapshot().ActiveStorage == &m_system.Spaces[1U] &&
               m_system.Spaces[0U].GetSnapshot().ResidentProcessorMask == 2U;
    }

    [[nodiscard]] bool Step(const Size step) noexcept
    {
        struct Operation
        {
            const char* Name;
            bool (Driver::*Run)() noexcept;
            UInt16 Weights[4U]; // General, TightPins, RuntimeCapacity, FramePolicy
        };
        static constexpr Operation operations[]{
            {"prepare", &Driver::OpPrepare, {150U, 200U, 65U, 65U}},
            {"audit", &Driver::AuditRecords, {8U, 8U, 8U, 8U}},
            {"begin", &Driver::OpBegin, {150U, 150U, 60U, 60U}},
            {"report", &Driver::OpReport, {95U, 80U, 43U, 43U}},
            {"burst", &Driver::OpBurst, {60U, 30U, 380U, 380U}},
            {"finish", &Driver::OpFinish, {88U, 76U, 42U, 42U}},
            {"quiesce", &Driver::OpQuiesce, {20U, 20U, 5U, 5U}},
            {"lifecycle", &Driver::OpLifecycle, {30U, 25U, 8U, 8U}},
            {"raw transition", &Driver::OpRaw, {205U, 150U, 85U, 85U}},
            {"controller", &Driver::OpController, {85U, 58U, 42U, 42U}},
            {"object lifecycle", &Driver::OpObjectLifecycle, {35U, 55U, 12U, 12U}},
        };
        const Size profile = static_cast<Size>(m_model.Mode);
        UInt64 choice{0U};
        for (const Operation& operation : operations)
        {
            choice += operation.Weights[profile];
        }
        choice = m_random.Below(choice);
        const Operation* selected = operations;
        for (; choice >= selected->Weights[profile]; ++selected)
        {
            choice -= selected->Weights[profile];
        }
        m_step = step;
        m_operation = selected->Name;
        return (this->*(selected->Run))() && (((step % AuditInterval) != 0U) || AuditRecords()) && CheckAll();
    }

    // Tears everything down and requires every home object to be collectable, so any
    // leaked pin, reference, or record fails. A controller's retained receipt can block
    // the other's bare detach, hence two retirement rounds.
    [[nodiscard]] bool Finish() noexcept
    {
        m_operation = "drain";
        const bool drained = ForAll(SpaceCount,
                                    [this](const Size index) noexcept
                                    {
                                        return (m_model.Spaces[index].State != AddressSpaceState::ShootdownPending) ||
                                               AcknowledgeAllAndComplete(index, false);
                                    }) &&
                             ReleaseRawReservations(NoIndex) &&
                             ForAll(2U * ControllerCount,
                                    [this](const Size round) noexcept
                                    {
                                        return RetireController(round % ControllerCount);
                                    }) &&
                             ForAll(SpaceCount,
                                    [this](const Size index) noexcept
                                    {
                                        const AddressSpaceState state = m_model.Spaces[index].State;
                                        return DetachRawProcessors(index) &&
                                               ((state == AddressSpaceState::Uninitialized) ||
                                                (state == AddressSpaceState::Quiesced) || QuiesceSpace(index, false));
                                    });
        for (Size slot{0U}; slot < PreparationCount; ++slot)
        {
            DiscardPreparation(slot);
        }
        return drained &&
               ForAll(ObjectLimit,
                      [this](const Size candidate) noexcept
                      {
                          Size kind{0U};
                          Size index{0U};
                          return DestroyObject(m_model.HomeObject(candidate, kind, index));
                      }) &&
               ForAll(ObjectLimit + 1U,
                      [this](Size) noexcept
                      {
                          return CollectOnce();
                      }) &&
               ForAll(HomeMemoryObjectCount,
                      [this](const Size index) noexcept
                      {
                          const bool ready = m_model.MemoryObjects[index].State == MemoryObjectState::Ready;
                          return !ready || QuiesceMemoryObject(index);
                      }) &&
               Same("drained population", Size{1U}, m_model.ActiveObjectCount()) && CheckAll();
    }

    // A Frame claims its whole range while it holds reservations. Root 2 maps page 0 of
    // Frame 0 writable. Root 1's executable mapping of that page through the twin Frame 1,
    // which has another cache policy, and root 0's through the nested Frame 2 meet Frame 0's
    // claim, while the bordering Frame 3 maps. Once root 2 quiesces and so releases Frame
    // 0's only reservation, the twin maps.
    [[nodiscard]] bool CheckPhysicalFrameClaims() noexcept
    {
        const auto map =
            [this](const Size space, const Size memoryObject, const Size frame, const UInt8 permissions) noexcept
        {
            PrepareInputs inputs = Inputs(space, memoryObject, frame);
            inputs.Pages = 1U;
            inputs.MemoryObjectOffset = 0U;
            inputs.FrameOffset = 0U;
            inputs.Permissions = permissions;
            inputs.VirtualStart = FreeVirtual(space, 1U);
            DiscardPreparation(space);
            return RunPrepare(space, inputs) && RunBegin(space, space, ExactIdentity, MapInjection{}) &&
                   ((m_model.Spaces[space].State != AddressSpaceState::ShootdownPending) ||
                    AcknowledgeAllAndComplete(space, false));
        };
        const auto published = [this](const Size space) noexcept
        {
            return m_model.Spaces[space].RuntimeCount;
        };
        m_operation = "physical Frame claims";
        return map(2U, 0U, 0U, ReadBit | WriteBit) && map(1U, 2U, 1U, ReadBit | ExecuteBit) &&
               map(0U, 0U, 2U, ReadBit | ExecuteBit) && map(0U, 2U, 3U, ReadBit | ExecuteBit) &&
               Same("writable mapping published", Size{1U}, published(2U)) &&
               Same("twin alias refused", Size{0U}, published(1U)) &&
               Same("nested alias refused, bordering Frame mapped", Size{1U}, published(0U)) &&
               QuiesceSpace(2U, false) && map(1U, 2U, 1U, ReadBit | ExecuteBit) &&
               Same("twin maps once Frame 0 holds no reservation", Size{1U}, published(1U));
    }

    // A raw reservation on CPU 2 cannot block controller 0's return to their shared idle root.
    [[nodiscard]] bool CharacterizeIdleContention() noexcept
    {
        m_operation = "idle contention characterization";
        bool switched{false};
        if (!SwitchController(0U, 1U, ActivationError::None, switched) ||
            !Same("switch away from idle", true, switched) || !RunReserve(0U, FirstRawProcessor))
        {
            return false;
        }
        const auto restored = m_system.Controllers[0U].SwitchToIdle();
        return SameController(m_model.ControllerSwitch(0U, 0U, ActivationError::None), restored) && CheckAll() &&
               Same("CPU 0 returns to idle despite CPU 2's reservation", true, restored.HasValue()) &&
               Same("CPU 0 detached root 1", Foundation::ProcessorMask{0U}, m_model.Spaces[1U].ResidentMask);
    }

private:
    [[nodiscard]] bool Mismatch(const char* const detail, const UInt64 expected, const UInt64 actual) const noexcept
    {
        static_cast<void>(
            std::fprintf(stderr, "MODEL MISMATCH seed=%llu step=%zu operation=%s: %s expected=%llu actual=%llu\n",
                         static_cast<unsigned long long>(m_seed), m_step, m_operation, detail,
                         static_cast<unsigned long long>(expected), static_cast<unsigned long long>(actual)));
        return false;
    }

    template <typename TValue, typename TActual>
    [[nodiscard]] bool Same(const char* const detail, const TValue expected, const TActual actual) const noexcept
    {
        return (expected == actual) || Mismatch(detail, static_cast<UInt64>(expected), static_cast<UInt64>(actual));
    }

    // Compares a real Result with its prediction: success, or every packed failure field.
    template <typename TValue, typename TError>
    [[nodiscard]] bool SameOutcome(const char* const detail, const bool success, const UInt64 failure,
                                   const Foundation::Result<TValue, TError>& actual) const noexcept
    {
        return Same(detail, success, actual.HasValue()) &&
               (success || Same(detail, failure, Encode(actual.GetError())));
    }

    [[nodiscard]] bool SameController(const ControllerExpectation& expected,
                                      const Foundation::Result<Kernel::Memory::ProcessorAddressSpaceControllerReceipt,
                                                               ProcessorAddressSpaceControllerFailure>& actual) noexcept
    {
        const Kernel::Memory::ProcessorAddressSpaceControllerReceipt& receipt = actual.GetValue();
        return SameOutcome("controller result", expected.Success, Encode(expected.Failure), actual) &&
               (!expected.Success ||
                (Same("controller roots", true,
                      receipt.Previous.IsEqualTo(SpaceIdentity(expected.Previous)) &&
                          receipt.Active.IsEqualTo(SpaceIdentity(expected.Active))) &&
                 Same("controller processor", expected.Processor, receipt.Processor.GetValue()) &&
                 Same("controller native activation", expected.NativeActivation, receipt.NativeActivationPerformed)));
    }

    [[nodiscard]] AddressSpaceId SpaceIdentity(const UInt64 value) noexcept
    {
        return (value == 0U) ? AddressSpaceId{} : AddressSpaceId{m_system.Registry, ObjectId{value}};
    }

    [[nodiscard]] AddressSpace* StorageOf(const Size index) noexcept
    {
        return (index == NoIndex) ? nullptr : &m_system.Spaces[index];
    }

    [[nodiscard]] bool SameReceipt(const Kernel::Memory::AddressSpaceTransitionReceipt& actual,
                                   const ReceiptModel& expected) noexcept
    {
        return Same("receipt validity", expected.Valid, actual.IsValid()) &&
               (!expected.Valid ||
                Same("receipt", true,
                     actual.IsFor(m_system.Spaces[expected.Space]) && (actual.GetSequence() == expected.Sequence) &&
                         (actual.GetProcessor().GetValue() == expected.Processor)));
    }

    // Byte-exact: owners with their registries, range, policy, and reserved bytes.
    [[nodiscard]] bool SameRecord(const Record& actual, const Record& expected) const noexcept
    {
        const bool same = std::memcmp(&actual, &expected, sizeof(Record)) == 0;
        return same || Mismatch("record", expected.VirtualStart.GetValue(), actual.VirtualStart.GetValue());
    }

    [[nodiscard]] bool CheckAll() noexcept
    {
        const Kernel::Resource::ResourceDomainSnapshot domain = m_system.Domain.GetSnapshot();
        UInt64 committedPages{0U};
        UInt64 readyObjects{0U};
        return ForAll(
                   SpaceCount,
                   [this](const Size index) noexcept
                   {
                       const SpaceModel& expected = m_model.Spaces[index];
                       const FakePageTable& pageTable = m_system.PageTables[index];
                       const Kernel::Memory::AddressSpaceSnapshot actual = m_system.Spaces[index].GetSnapshot();
                       return Same("space state", expected.State, actual.State) &&
                              Same("space identity", true,
                                   actual.Identity.IsEqualTo(SpaceIdentity(expected.BoundIdentity))) &&
                              Same("charge owner", expected.ChargeOwner,
                                   m_system.Spaces[index].GetChargeOwner().GetValue()) &&
                              Same("mapping count", expected.SeedCount + expected.RuntimeCount, actual.MappingCount) &&
                              Same("shootdown sequence", expected.ShootdownSequence, actual.ShootdownSequence) &&
                              Same("shootdown targets", expected.TargetMask, actual.TargetProcessorMask) &&
                              Same("shootdown acknowledgements", expected.AcknowledgedMask,
                                   actual.AcknowledgedProcessorMask) &&
                              Same("reserved transition", expected.ReservedSequence, actual.TransitionSequence) &&
                              Same("transition processor", expected.TransitionProcessor,
                                   actual.TransitionProcessor.GetValue()) &&
                              Same("resident mask", expected.ResidentMask, actual.ResidentProcessorMask) &&
                              Same("activating processor", NoProcessor, actual.ActivatingProcessor.GetValue()) &&
                              Same("backend claims", expected.BackendCalls[0U], pageTable.Calls[0U]) &&
                              Same("backend releases", expected.BackendCalls[1U], pageTable.Calls[1U]) &&
                              Same("backend activations", expected.BackendCalls[2U], pageTable.Calls[2U]) &&
                              Same("backend maps", expected.BackendCalls[3U], pageTable.Calls[3U]) &&
                              Same("backend owner", expected.BackendOwned, pageTable.Owner == &m_system.Spaces[index]);
                   }) &&
               ForAll(MemoryObjectCount,
                      [&](const Size index) noexcept
                      {
                          const MemoryObjectModel& expected = m_model.MemoryObjects[index];
                          const Kernel::Memory::MemoryObjectSnapshot actual =
                              m_system.MemoryObjects[index].GetSnapshot();
                          const bool charged =
                              (index < HomeMemoryObjectCount) && (expected.State == MemoryObjectState::Ready);
                          const bool pinned = MemoryObjectConfigurations[index].Commitment == MemoryCommitment::Pinned;
                          readyObjects += charged ? 1U : 0U;
                          committedPages += (charged && pinned) ? MemoryObjectConfigurations[index].Pages : 0U;
                          return Same("memory object state", expected.State, actual.State) &&
                                 Same("memory object ready", expected.State == MemoryObjectState::Ready,
                                      m_system.MemoryObjects[index].IsInitialized()) &&
                                 Same("memory object mappings", expected.ActiveMappings, actual.ActiveMappings);
                      }) &&
               ForAll(ControllerCount,
                      [this](const Size index) noexcept
                      {
                          const ControllerModel& expected = m_model.Controllers[index];
                          const auto actual = m_system.Controllers[index].GetSnapshot();
                          return Same("controller state", expected.State, actual.State) &&
                                 Same("controller processor", expected.Processor, actual.Processor.GetValue()) &&
                                 Same("controller roots", true,
                                      (actual.IdleStorage == StorageOf(expected.Idle)) &&
                                          (actual.ActiveStorage == StorageOf(expected.Active)) &&
                                          (actual.PreviousStorage == StorageOf(expected.Previous)) &&
                                          actual.Idle.IsEqualTo(SpaceIdentity(expected.IdleIdentity)) &&
                                          actual.Active.IsEqualTo(SpaceIdentity(expected.ActiveIdentity)) &&
                                          actual.Previous.IsEqualTo(SpaceIdentity(expected.PreviousIdentity))) &&
                                 SameReceipt(actual.SourceTransition, expected.Source) &&
                                 SameReceipt(actual.DestinationTransition, expected.Destination);
                      }) &&
               ForAll(PreparationCount,
                      [this](const Size slot) noexcept
                      {
                          return Same("preparation state", m_model.Preparations[slot].Prepared,
                                      m_system.Preparations[slot].IsPrepared());
                      }) &&
               ForAll(HomeFrameCount,
                      [this](const Size index) noexcept
                      {
                          return Same("frame initialized", true, m_system.Frames[index].IsInitialized());
                      }) &&
               Same("domain committed pages", committedPages, domain.Usage.CommittedMemoryPages) &&
               Same("domain resident pages", committedPages, domain.Usage.ResidentMemoryPages) &&
               Same("domain metadata", readyObjects * sizeof(MemoryObject), domain.Usage.KernelMetadataBytes) &&
               Same("registry population", m_model.ActiveObjectCount(), m_system.Registry.GetActiveCount());
    }

    // Every exported record of every root, and the first index past the end.
    [[nodiscard]] bool AuditRecords() noexcept
    {
        return ForAll(SpaceCount,
                      [this](const Size index) noexcept
                      {
                          const SpaceModel& space = m_model.Spaces[index];
                          const Size count = space.SeedCount + space.RuntimeCount;
                          const bool gone = (space.State == AddressSpaceState::Uninitialized) ||
                                            (space.State == AddressSpaceState::Quiesced);
                          const AddressSpaceError pastEnd =
                              gone ? StateRefusal(space.State) : AddressSpaceError::InvalidMappingPlan;
                          return ForAll(count,
                                        [&](const Size position) noexcept
                                        {
                                            const auto record = m_system.Spaces[index].TryGetMapping(position);
                                            return Same("record present", true, record.HasValue()) &&
                                                   SameRecord(record.GetValue(), space.Mappings[position].Value);
                                        }) &&
                                 SameOutcome("record past the end", false, Encode(pastEnd),
                                             m_system.Spaces[index].TryGetMapping(count));
                      });
    }

    [[nodiscard]] Size PickSpace() noexcept
    {
        return static_cast<Size>(m_random.Below(SpaceCount));
    }

    [[nodiscard]] Size OtherSpace(const Size index) noexcept
    {
        return (index + 1U + static_cast<Size>(m_random.Below(SpaceCount - 1U))) % SpaceCount;
    }

    [[nodiscard]] UInt8 PickRawProcessor() noexcept
    {
        return m_random.Chance(1U, 20U) ? static_cast<UInt8>(ProcessorLimit + m_random.Below(0x100U - ProcessorLimit))
                                        : static_cast<UInt8>(FirstRawProcessor + m_random.Below(2U));
    }

    // Capacity roots are spared terminal injections until the seed's last quarter.
    [[nodiscard]] bool IsCapacityRoot(const Size index) const noexcept
    {
        return (index != NoIndex) && (m_step < ((StepsPerSeed * 3U) / 4U)) &&
               (((m_model.Mode == Profile::RuntimeCapacity) && (index == CapacitySpace)) ||
                ((m_model.Mode == Profile::FramePolicy) && (index != 0U)));
    }

    [[nodiscard]] UInt64 FaultPercent(const Size index) const noexcept
    {
        return IsCapacityRoot(index) ? 0U : m_faultPercent;
    }

    template <typename TFault, Size Count>
    [[nodiscard]] TFault PickFault(const UInt64 percent, const TFault (&faults)[Count]) noexcept
    {
        return m_random.Chance(percent, 100U) ? faults[m_random.Below(Count)] : TFault{};
    }

    [[nodiscard]] MapInjection PickMapInjection(const UInt64 percent) noexcept
    {
        constexpr MapOutcome rejected{MapOutcome::Rejected};
        constexpr MapInjection injections[12U]{{rejected, PageTableError::NotReady},
                                               {rejected, PageTableError::InvalidPlan},
                                               {rejected, PageTableError::MappingConflict},
                                               {rejected, PageTableError::CapacityExceeded},
                                               {rejected, PageTableError::CorruptState},
                                               {rejected, PageTableError::UnsupportedConfiguration},
                                               {rejected, PageTableError::CommittedStateUnknown},
                                               {rejected, PageTableError::InternalFailure},
                                               {rejected, static_cast<PageTableError>(0x5AU)},
                                               {MapOutcome::WrongStart, PageTableError::None},
                                               {MapOutcome::WrongPages, PageTableError::None},
                                               {MapOutcome::ReservedNonzero, PageTableError::None}};
        return PickFault(percent, injections);
    }

    [[nodiscard]] ActivationError PickActivationInjection(const UInt64 percent) noexcept
    {
        constexpr ActivationError injections[6U]{ActivationError::NotReady,
                                                 ActivationError::InvalidOwner,
                                                 ActivationError::UnsupportedConfiguration,
                                                 ActivationError::RejectedBeforeCommit,
                                                 ActivationError::CommittedStateUnknown,
                                                 static_cast<ActivationError>(0x66U)};
        return PickFault(percent, injections);
    }

    // A free runtime range (seeds live in the upper half), or the last candidate.
    [[nodiscard]] UInt64 FreeVirtual(const Size index, const UInt64 pages) noexcept
    {
        const SpaceModel& space = m_model.Spaces[index];
        UInt64 candidate{RuntimeVirtualBase};
        bool free{false};
        for (Size attempt{0U}; !free && (attempt < 12U); ++attempt)
        {
            candidate = RuntimeVirtualBase + (m_random.Below(RuntimeWindowPages - pages) * PageBytes);
            free = true;
            const Size end = space.SeedCount + space.RuntimeCount;
            for (Size position{space.SeedCount}; free && (position < end); ++position)
            {
                const Record& existing = space.Mappings[position].Value;
                free = !Overlaps(candidate, pages, existing.VirtualStart.GetValue(), existing.Pages.GetValue());
            }
        }
        return candidate;
    }

    // Usually free; sometimes overlapping or adjacent to a record; rarely misaligned or
    // at the unrepresentable top of the address space.
    [[nodiscard]] UInt64 PickVirtual(const Size index, const UInt64 pages) noexcept
    {
        const SpaceModel& space = m_model.Spaces[index];
        const UInt64 mode = m_random.Below(24U);
        const Size count = space.SeedCount + space.RuntimeCount;
        if ((mode < 7U) && (count != 0U))
        {
            const bool runtime = (mode < 4U) && (space.RuntimeCount != 0U);
            const Size first = runtime ? space.SeedCount : 0U;
            const Record& existing = space.Mappings[first + m_random.Below(count - first)].Value;
            const UInt64 start = existing.VirtualStart.GetValue();
            const UInt64 existingPages = existing.Pages.GetValue();
            if (runtime)
            {
                return start + (m_random.Below(existingPages + pages - 1U) * PageBytes) - ((pages - 1U) * PageBytes);
            }
            return m_random.Chance(1U, 2U) ? start + (existingPages * PageBytes) : start - (pages * PageBytes);
        }
        if ((mode == 7U) && (count != 0U))
        {
            return space.Mappings[m_random.Below(space.SeedCount + 1U) % count].Value.VirtualStart.GetValue();
        }
        if (mode == 8U)
        {
            return FreeVirtual(index, pages) + 0x800U;
        }
        return (mode == 9U) ? TopPageStart - (m_random.Below(3U) * PageBytes) : FreeVirtual(index, pages);
    }

    // Moves the Frame slice next to any root's mapping: adjacent or overlapping by a page.
    void PlaceNearExistingPhysical(PrepareInputs& inputs) noexcept
    {
        const SpaceModel& space = m_model.Spaces[PickSpace()];
        const Size count = space.SeedCount + space.RuntimeCount;
        if (count == 0U)
        {
            return;
        }
        const Record& existing = space.Mappings[m_random.Below(count)].Value;
        const UInt64 start = existing.PhysicalStart.GetValue();
        const UInt64 pages = existing.Pages.GetValue();
        const UInt64 targets[4U]{start + (pages * PageBytes), start - (inputs.Pages * PageBytes),
                                 start + ((pages - 1U) * PageBytes), start - ((inputs.Pages - 1U) * PageBytes)};
        const UInt64 target = targets[m_random.Below(4U)];
        const FrameConfiguration& frame = FrameConfigurations[inputs.FrameInstance];
        const UInt64 offset = (target - frame.PhysicalStart) / PageBytes;
        if ((target >= frame.PhysicalStart) && ((offset + inputs.Pages) <= frame.Pages))
        {
            inputs.FrameOffset = offset;
            const bool fits = (offset + inputs.Pages) <= MemoryObjectConfigurations[inputs.MemoryObjectInstance].Pages;
            inputs.MemoryObjectOffset = fits ? offset : inputs.MemoryObjectOffset;
        }
    }

    [[nodiscard]] UInt8 PickPermissions(const UInt8 allowed) noexcept
    {
        constexpr UInt8 choices[6U]{ReadBit,           ReadBit | WriteBit,           ReadBit | ExecuteBit,
                                    ReadBit | UserBit, ReadBit | WriteBit | UserBit, ReadBit | ExecuteBit | UserBit};
        UInt8 choice = choices[m_random.Below(6U)];
        for (Size attempt{0U}; (attempt < 4U) && ((choice & static_cast<UInt8>(~allowed)) != 0U); ++attempt)
        {
            choice = choices[m_random.Below(6U)];
        }
        return choice;
    }

    // A plausible request: pinned object, cache-compatible Frame, in-range offsets.
    [[nodiscard]] PrepareInputs CandidateInputs(const Size index) noexcept
    {
        const Size memoryObject = m_random.Chance(1U, 12U) ? 3U : static_cast<Size>(m_random.Below(3U));
        constexpr Size writeBackFrames[3U]{0U, 2U, 4U};
        constexpr Size writeThroughFrames[2U]{1U, 3U};
        const Size frame =
            (memoryObject == 2U) ? writeThroughFrames[m_random.Below(2U)] : writeBackFrames[m_random.Below(3U)];
        const UInt64 strayOdds = (memoryObject == 2U) ? 6U : 10U;
        const Size stray = static_cast<Size>(m_random.Below(HomeFrameCount));
        return Inputs(index, memoryObject, m_random.Chance(1U, strayOdds) ? stray : frame);
    }

    [[nodiscard]] PrepareInputs Inputs(const Size index, const Size memoryObject, const Size frame) noexcept
    {
        const MemoryObjectConfiguration& objectConfiguration = MemoryObjectConfigurations[memoryObject];
        const FrameConfiguration& frameConfiguration = FrameConfigurations[frame];
        const bool foreign = memoryObject == ForeignMemoryObject;
        const UInt64 limit = (objectConfiguration.Pages < frameConfiguration.Pages) ? objectConfiguration.Pages
                                                                                    : frameConfiguration.Pages;
        const UInt64 pages = 1U + m_random.Below((limit < 8U) ? limit : 8U);
        const UInt64 objectOffset = m_random.Below(objectConfiguration.Pages - pages + 1U);
        const UInt64 frameOffset = m_random.Below(frameConfiguration.Pages - pages + 1U);
        PrepareInputs inputs{foreign,
                             {foreign, m_model.MemoryObjects[memoryObject].Object.Identity},
                             memoryObject,
                             {frame == ForeignFrame, m_model.Frames[frame].Object.Identity},
                             frame,
                             0U,
                             pages,
                             objectOffset,
                             frameOffset,
                             PickPermissions(objectConfiguration.MaximumPermissions),
                             static_cast<Size>(m_random.Below(7U)),
                             0U};
        inputs.VirtualStart = PickVirtual(index, pages);
        if (!foreign && m_random.Chance(1U, 3U))
        {
            PlaceNearExistingPhysical(inputs);
        }
        return inputs;
    }

    // One perturbation of a plausible request, so that each rule is reached alone:
    // exactly one field changes, the one that `which` selects.
    [[nodiscard]] PrepareInputs PerturbedInputs(const Size index) noexcept
    {
        PrepareInputs inputs = CandidateInputs(index);
        const bool object = m_random.Chance(1U, 2U);
        IdentityInput& identity = object ? inputs.MemoryObject : inputs.Frame;
        Size& instance = object ? inputs.MemoryObjectInstance : inputs.FrameInstance;
        const ObjectState& current = object ? m_model.MemoryObjects[instance].Object : m_model.Frames[instance].Object;
        const ObjectState& other = object ? m_model.Frames[m_random.Below(HomeFrameCount)].Object
                                          : m_model.MemoryObjects[m_random.Below(HomeMemoryObjectCount)].Object;
        constexpr UInt8 invalidPermissions[5U]{ReadBit | WriteBit | ExecuteBit, WriteBit, ExecuteBit | UserBit,
                                               ReadBit | 0x10U, ReadBit | WriteBit | 0x80U};
        constexpr UInt64 invalidPages[3U]{0U, MaximumUInt64 / PageBytes, (MaximumUInt64 / PageBytes) + 1U};
        const UInt64 stale = (current.PreviousIdentity != 0U) ? current.PreviousIdentity : UnissuedIdentity;
        const UInt64 identities[3U]{0U, stale, other.Identity};
        const UInt64 which = m_random.Below(14U);
        identity.Value = (which < 3U) ? identities[which] : identity.Value;
        identity.Foreign = (which == 3U) ? !inputs.Foreign : identity.Foreign;
        const Size instanceCount = object ? HomeMemoryObjectCount : HomeFrameCount;
        instance = (which == 4U) ? static_cast<Size>(m_random.Below(instanceCount)) : instance;
        inputs.VirtualStart += (which == 5U) ? (PageBytes / 2U) : 0U;
        inputs.Pages = (which == 6U) ? invalidPages[m_random.Below(3U)] : inputs.Pages;
        inputs.MemoryObjectOffset += (which == 7U) ? MemoryObjectConfigurations[inputs.MemoryObjectInstance].Pages : 0U;
        inputs.FrameOffset += (which == 8U) ? FrameConfigurations[inputs.FrameInstance].Pages : 0U;
        inputs.Permissions = (which == 9U) ? invalidPermissions[m_random.Below(5U)] : inputs.Permissions;
        inputs.ReservedByte = (which == 10U) ? static_cast<UInt8>(1U + m_random.Below(0xFFU)) : inputs.ReservedByte;
        const UInt8 anyAccess = static_cast<UInt8>(ReadBit | (m_random.Below(8U) << 1U));
        inputs.Permissions = (which >= 12U) ? anyAccess : inputs.Permissions;
        return (which == 11U) ? Inputs(index, ForeignMemoryObject, ForeignFrame) : inputs;
    }

    // Rejection sampling through the model for a request predicted to publish.
    [[nodiscard]] PrepareInputs SuccessfulInputs(const Size index, const Size slot) noexcept
    {
        PrepareInputs inputs = CandidateInputs(index);
        for (Size attempt{0U}; attempt < 16U; ++attempt)
        {
            PreparationModel candidate{true, inputs.Foreign, {}};
            if ((PredictPrepare(slot, inputs, candidate.Mapping) == PinnedFrameMappingError::None) &&
                (m_model.PredictBegin(index, candidate, true, MapInjection{}).Effect == BeginEffect::Published))
            {
                break;
            }
            inputs = CandidateInputs(index);
            inputs.VirtualStart = FreeVirtual(index, inputs.Pages);
        }
        return inputs;
    }

    // FramePolicy: writable Frame 0 slices at new keys, one record per success.
    [[nodiscard]] PrepareInputs FreshPolicyInputs(const Size index) noexcept
    {
        PrepareInputs inputs = Inputs(index, 0U, 0U);
        const UInt64 framePages = FrameConfigurations[0U].Pages;
        bool fresh{false};
        for (Size attempt{0U}; !fresh && (attempt < 24U); ++attempt)
        {
            inputs.Pages = 1U + m_random.Below(framePages);
            inputs.FrameOffset = m_random.Below(framePages - inputs.Pages + 1U);
            const UInt64 start = FrameConfigurations[0U].PhysicalStart + (inputs.FrameOffset * PageBytes);
            fresh = m_model.FindPolicy(0U, start, inputs.Pages, AccessOf(WriteBit)) == nullptr;
        }
        inputs.MemoryObjectOffset = inputs.FrameOffset;
        inputs.Permissions = ReadBit | WriteBit;
        inputs.VirtualStart = FreeVirtual(index, inputs.Pages);
        return inputs;
    }

    [[nodiscard]] ObjectRegistry& RegistryFor(const bool foreign) noexcept
    {
        return foreign ? m_system.ForeignRegistry : m_system.Registry;
    }

    [[nodiscard]] bool Register(Kernel::Object::IKernelObject& instance, const bool foreign,
                                ObjectState& object) noexcept
    {
        const auto created = (foreign ? m_system.ForeignIssuer : m_system.Issuer).CreateObject(instance);
        object = ObjectState{true, false, created.HasValue() ? created.GetValue().GetValue() : 0U, object.Identity, 0U};
        return Same("registration", true, created.HasValue());
    }

    // Storage is reused only after collection; a Ready object is quiesced first.
    [[nodiscard]] bool CreateMemoryObject(const Size index) noexcept
    {
        if ((m_model.MemoryObjects[index].State == MemoryObjectState::Ready) && !QuiesceMemoryObject(index))
        {
            return false;
        }
        m_system.MemoryObjects[index].~MemoryObject();
        new (&m_system.MemoryObjects[index]) MemoryObject{};
        const MemoryObjectConfiguration& configuration = MemoryObjectConfigurations[index];
        const bool foreign = index == ForeignMemoryObject;
        ObjectRegistry& registry = RegistryFor(foreign);
        const PagePermissions permissions =
            PermissionsFrom<PagePermissions, PagePermission>(configuration.MaximumPermissions);
        const Kernel::Memory::MemoryObjectDescriptor descriptor{
            PageCount{configuration.Pages}, configuration.CachePolicy, permissions, configuration.Commitment, {}};
        const auto failure = m_system.MemoryObjects[index].Initialize(
            registry,
            Kernel::Resource::ResourceDomainObjectId{registry, ObjectId{m_model.DomainIdentities[foreign ? 1U : 0U]}},
            foreign ? m_system.ForeignDomain : m_system.Domain, descriptor);
        m_model.MemoryObjects[index].State = MemoryObjectState::Ready;
        m_model.MemoryObjects[index].ActiveMappings = 0U;
        return Same("memory object initialization", MemoryObjectError::None, failure.Error) &&
               Register(m_system.MemoryObjects[index], foreign, m_model.MemoryObjects[index].Object);
    }

    [[nodiscard]] bool CreateFrame(const Size index) noexcept
    {
        m_system.Frames[index].~BootFrame();
        new (&m_system.Frames[index]) BootFrame{};
        const FrameConfiguration& configuration = FrameConfigurations[index];
        const Kernel::Memory::FrameDescriptor descriptor{PhysicalAddress{configuration.PhysicalStart},
                                                         PageCount{configuration.Pages},
                                                         configuration.CachePolicy,
                                                         configuration.Kind,
                                                         {}};
        const Kernel::Memory::FrameError initialized =
            m_system.Frames[index].Initialize(ResourceDomainId{0x42U}, descriptor);
        m_model.Frames[index].PolicyCount = 0U;
        m_model.Frames[index].Reservations = 0U;
        return Same("frame initialization", Kernel::Memory::FrameError::None, initialized) &&
               Register(m_system.Frames[index], index == ForeignFrame, m_model.Frames[index].Object);
    }

    [[nodiscard]] bool DestroyObject(ObjectState& object) noexcept
    {
        const ObjectRegistryError actual = m_system.Issuer.DestroyObject(ObjectId{object.Identity});
        const bool live = object.Registered && !object.Quiescing;
        object.Quiescing = object.Quiescing || live;
        return Same("destroy object", live ? ObjectRegistryError::Ok : ObjectRegistryError::InvalidObject, actual);
    }

    [[nodiscard]] bool CollectOnce() noexcept
    {
        Size kind{0U};
        Size index{0U};
        ObjectState* const expected = m_model.CollectCandidate(kind, index);
        const auto actual = m_system.Issuer.CollectDestroyedObject();
        if (expected == nullptr)
        {
            return SameOutcome("collect", false, Encode(ObjectRegistryError::NoDestructionReady), actual);
        }
        Kernel::Object::IKernelObject* const instances[4U]{nullptr, &m_system.MemoryObjects[index % MemoryObjectCount],
                                                           &m_system.Frames[index % FrameCount],
                                                           &m_system.Spaces[index % SpaceCount]};
        expected->Registered = false;
        expected->Quiescing = false;
        return Same("collect success", true, actual.HasValue()) &&
               Same("collected object", expected->Identity, actual.GetValue().Object.GetValue()) &&
               Same("collected instance", true, actual.GetValue().Instance == instances[kind]);
    }

    [[nodiscard]] bool CollectUntilGone(const ObjectState& object) noexcept
    {
        return ForAll(ObjectLimit + 1U,
                      [&](Size) noexcept
                      {
                          return !object.Registered || CollectOnce();
                      }) &&
               Same("pin-free object collected", false, object.Registered);
    }

    [[nodiscard]] bool Recreate(const bool memoryObject, const Size index) noexcept
    {
        return memoryObject ? CreateMemoryObject(index) : CreateFrame(index);
    }

    [[nodiscard]] bool QuiesceMemoryObject(const Size index) noexcept
    {
        MemoryObjectModel& model = m_model.MemoryObjects[index];
        MemoryObjectError expected{MemoryObjectError::None};
        static_cast<void>(Pass(model.State == MemoryObjectState::Quiesced, MemoryObjectError::Quiesced, expected) &&
                          Pass(model.ActiveMappings != 0U, MemoryObjectError::InUse, expected));
        model.State = (expected == MemoryObjectError::None) ? MemoryObjectState::Quiesced : model.State;
        return Same("memory object quiesce", expected, m_system.MemoryObjects[index].Quiesce().Error);
    }

    // Builds a sealed plan in virtual-address order and stages it in the model.
    [[nodiscard]] bool BuildSeedPlan(const Size index) noexcept
    {
        m_system.Plans[index].~InitialMappingPlan();
        InitialMappingPlan& plan = *new (&m_system.Plans[index]) InitialMappingPlan{};
        SpaceModel& space = m_model.Spaces[index];
        // Every other RuntimeCapacity seed fills the seed table, so both limits bind.
        const bool full =
            (m_model.Mode == Profile::RuntimeCapacity) && (index == CapacitySpace) && (((m_seed / 4U) % 2U) == 0U);
        struct SeedKind
        {
            InitialMappingOwner Owner;
            UInt8 Permissions;
        };
        constexpr UInt8 readWrite{ReadBit | WriteBit};
        constexpr SeedKind kinds[8U]{{InitialMappingOwner::NucleusCode, ReadBit | ExecuteBit},
                                     {InitialMappingOwner::NucleusStack, readWrite},
                                     {InitialMappingOwner::PageTable, readWrite},
                                     {InitialMappingOwner::NucleusReadOnlyData, ReadBit},
                                     {InitialMappingOwner::NucleusWritableData, readWrite},
                                     {InitialMappingOwner::BootHandoff, ReadBit},
                                     {InitialMappingOwner::Device, readWrite},
                                     {InitialMappingOwner::FirmwareRuntimeCode, ReadBit | ExecuteBit}};
        UInt64 cursors[3U]{SeedVirtualBase + (m_random.Below(4U) * PageBytes),
                           SeedPhysicalBase + (m_random.Below(4U) * PageBytes), DeviceSeedPhysicalBase};
        const Size count = full ? Kernel::Memory::MaximumInitialMappings : (3U + static_cast<Size>(m_random.Below(6U)));
        return ForAll(count,
                      [&](const Size position) noexcept
                      {
                          const SeedKind& kind = kinds[(position < 3U) ? position : (3U + m_random.Below(5U))];
                          const bool device = kind.Owner == InitialMappingOwner::Device;
                          const MemoryCachePolicy deviceCache =
                              m_random.Chance(1U, 2U) ? MemoryCachePolicy::Uncached : MemoryCachePolicy::WriteCombining;
                          const MemoryCachePolicy cachePolicy = device ? deviceCache : MemoryCachePolicy::WriteBack;
                          const UInt16 globalBit = static_cast<UInt16>(Kernel::Memory::MappingPermission::Global);
                          const UInt16 global = m_random.Chance(1U, 3U) ? globalBit : UInt16{0U};
                          const UInt64 pages = full ? 1U : (1U + m_random.Below(4U));
                          const UInt64 physical = cursors[device ? 2U : 1U];
                          const Kernel::Memory::InitialMapping mapping{
                              VirtualAddress{cursors[0U]},
                              PhysicalAddress{physical},
                              PageCount{pages},
                              PermissionsFrom<Kernel::Memory::MappingPermissions, Kernel::Memory::MappingPermission>(
                                  static_cast<UInt16>(kind.Permissions | global)),
                              cachePolicy,
                              kind.Owner,
                              0U};
                          space.Mappings[position] =
                              MappingModel{Record{MemoryObjectId{},
                                                  {},
                                                  FrameId{},
                                                  VirtualAddress{cursors[0U]},
                                                  PhysicalAddress{physical},
                                                  PageCount{pages},
                                                  0U,
                                                  PermissionsFrom<PagePermissions, PagePermission>(kind.Permissions),
                                                  cachePolicy,
                                                  device ? FrameKind::Device : FrameKind::Ram,
                                                  static_cast<MemoryCommitment>(0U),
                                                  false,
                                                  {}},
                                           NoIndex, NoIndex};
                          cursors[0U] += (pages + m_random.Below(3U)) * PageBytes;
                          cursors[device ? 2U : 1U] += (pages + m_random.Below(3U)) * PageBytes;
                          return Same("seed mapping", Kernel::Memory::InitialMappingPlanError::None,
                                      plan.AddMapping(mapping));
                      }) &&
               Same("seed seal", Kernel::Memory::InitialMappingPlanError::None, plan.Seal());
    }

    // Uninitialized to Unbound and registered, with optional injected failures.
    [[nodiscard]] bool InitializeSpace(const Size index, const bool allowFaults) noexcept
    {
        SpaceModel& space = m_model.Spaces[index];
        FakePageTable& pageTable = m_system.PageTables[index];
        const bool fresh = space.State == AddressSpaceState::Uninitialized;
        const UInt64 fault = allowFaults ? m_random.Below(24U) : 23U;
        pageTable.Ready = fault != 2U;
        const OwnershipError claimError = static_cast<OwnershipError>(1U + m_random.Below(4U));
        pageTable.Ownership = (fault == 3U) ? claimError : OwnershipError::None;
        if (fresh && !BuildSeedPlan(index))
        {
            return false;
        }
        AddressSpaceError expected{AddressSpaceError::None};
        const bool claimed = Pass(!fresh, AddressSpaceError::AlreadyInitialized, expected) &&
                             Pass(fault == 0U, AddressSpaceError::InvalidConfiguration, expected) &&
                             Pass(fault == 1U, AddressSpaceError::InvalidMappingPlan, expected) &&
                             Pass(fault == 2U, AddressSpaceError::PageTableOwnershipRejected, expected);
        space.BackendCalls[0U] += claimed ? 1U : 0U;
        static_cast<void>(claimed && Pass(fault == 3U, AddressSpaceError::PageTableOwnershipRejected, expected));
        const AddressSpaceError actual =
            m_system.Spaces[index].Initialize((fault != 0U) ? ResourceDomainId{0x50U + index} : ResourceDomainId{},
                                              pageTable, (fault != 1U) ? m_system.Plans[index] : m_system.UnsealedPlan);
        if ((expected != AddressSpaceError::None) || (actual != AddressSpaceError::None))
        {
            return Same("initialize result", expected, actual);
        }
        space.State = AddressSpaceState::Unbound;
        space.SeedCount = m_system.Plans[index].GetMappingCount();
        space.ChargeOwner = 0x50U + index;
        space.BackendOwned = true;
        return Register(m_system.Spaces[index], false, space.Object);
    }

    [[nodiscard]] bool BindSpace(const Size index, const Size choice) noexcept
    {
        SpaceModel& space = m_model.Spaces[index];
        const UInt64 values[6U]{space.Object.Registered ? space.Object.Identity : 0U,
                                0U,
                                space.Object.Identity,
                                m_model.Spaces[OtherSpace(index)].Object.Identity,
                                m_model.Frames[m_random.Below(HomeFrameCount)].Object.Identity,
                                UnissuedIdentity};
        const UInt64 value = values[choice];
        const bool foreign = choice == 2U;
        const AddressSpaceState state = space.State;
        const ObjectPinError pin = m_model.PredictPin(false, value, ObjectType::AddressSpace);
        Size kind{0U};
        AddressSpaceFailure expected{};
        const bool admitted =
            Pass((state == AddressSpaceState::Uninitialized) || (state == AddressSpaceState::Faulted) ||
                     (state == AddressSpaceState::Quiesced),
                 StateRefusal(state), expected.Error) &&
            Pass(state != AddressSpaceState::Unbound, AddressSpaceError::AlreadyBound, expected.Error) &&
            Pass(value == 0U, AddressSpaceError::InvalidIdentity, expected.Error) &&
            Pass(foreign, AddressSpaceError::RegistryMismatch, expected.Error);
        expected.PinError = admitted ? pin : ObjectPinError::Ok;
        const bool bound =
            admitted &&
            Pass(pin != ObjectPinError::Ok,
                 PinFailure(pin, {AddressSpaceError::ObjectNotFound, AddressSpaceError::ObjectTypeMismatch,
                                  AddressSpaceError::ObjectPinUnavailable}),
                 expected.Error) &&
            Pass(m_model.FindLive(false, value, kind) != index, AddressSpaceError::ObjectInstanceMismatch,
                 expected.Error);
        space.State = bound ? AddressSpaceState::Ready : state;
        space.BoundIdentity = bound ? value : space.BoundIdentity;
        const AddressSpaceId identity =
            (value == 0U) ? AddressSpaceId{} : AddressSpaceId{RegistryFor(foreign), ObjectId{value}};
        return Same("bind result", Encode(expected),
                    Encode(m_system.Spaces[index].BindIdentity(m_system.Registry, identity)));
    }

    [[nodiscard]] bool QuiesceSpace(const Size index, const bool allowFault) noexcept
    {
        const bool releaseFails = allowFault && m_random.Chance(1U, 8U);
        m_system.PageTables[index].Ownership =
            releaseFails ? static_cast<OwnershipError>(1U + m_random.Below(4U)) : OwnershipError::None;
        const AddressSpaceError expected = m_model.Quiesce(index, releaseFails);
        return Same("quiesce result", expected, m_system.Spaces[index].Quiesce());
    }

    // Quiesced storage becomes a fresh bound root; users of the old generation go first.
    [[nodiscard]] bool Rebuild(const Size index) noexcept
    {
        SpaceModel& space = m_model.Spaces[index];
        for (Size controller{0U}; (space.State == AddressSpaceState::Quiesced) && (controller < ControllerCount);
             ++controller)
        {
            if (m_model.References(controller, index) && !RetireController(controller))
            {
                return false;
            }
            if (m_model.References(controller, index))
            {
                return true; // The CPU cannot leave yet; the root stays quiesced
            }
        }
        if (space.State == AddressSpaceState::Quiesced)
        {
            if ((space.Object.Registered && !space.Object.Quiescing && !DestroyObject(space.Object)) ||
                !CollectUntilGone(space.Object))
            {
                return false;
            }
            m_system.Spaces[index].~AddressSpace();
            new (&m_system.Spaces[index]) AddressSpace{};
            m_model.ResetSpace(index);
            // Handles must not outlive the AddressSpace generation that issued them.
            m_latest[index] = IssuedToken{};
            m_previous[index] = IssuedToken{};
            m_receipts[index][0U] = HeldReceipt{};
            m_receipts[index][1U] = HeldReceipt{};
        }
        return (space.State != AddressSpaceState::Uninitialized) ||
               (InitializeSpace(index, true) &&
                ((space.State != AddressSpaceState::Unbound) || !m_random.Chance(1U, 5U) ||
                 BindSpace(index, 1U + m_random.Below(5U))) &&
                ((space.State != AddressSpaceState::Unbound) || m_random.Chance(1U, 8U) ||
                 BindSpace(index, ExactIdentity)));
    }

    // Takes every user off one root and quiesces it.
    [[nodiscard]] bool RecoverSpace(const Size index) noexcept
    {
        const bool pending = m_model.Spaces[index].State == AddressSpaceState::ShootdownPending;
        return (!pending || (m_random.Chance(1U, 2U) ? RunFinish(index, m_latest[index], true)
                                                     : AcknowledgeAllAndComplete(index, true))) &&
               ReleaseRawReservations(index) &&
               ForAll(ControllerCount,
                      [&](const Size controller) noexcept
                      {
                          return !m_model.References(controller, index) || RetireController(controller);
                      }) &&
               DetachRawProcessors(index) && QuiesceSpace(index, true);
    }

    // PinnedFrameMappingPreparation::Prepare in its documented rule order.
    [[nodiscard]] PinnedFrameMappingError PredictPrepare(const Size slot, const PrepareInputs& inputs,
                                                         MappingModel& mapping) noexcept
    {
        using Error = PinnedFrameMappingError;
        const UInt8 permissions = inputs.Permissions;
        const UInt64 pages = inputs.Pages;
        const bool requestValid =
            ((inputs.VirtualStart % PageBytes) == 0U) && (pages != 0U) && (pages <= (MaximumUInt64 / PageBytes)) &&
            (inputs.VirtualStart <= (MaximumUInt64 - (pages * PageBytes))) &&
            ((permissions & static_cast<UInt8>(~KnownPermissionBits)) == 0U) && ((permissions & ReadBit) != 0U) &&
            (AccessOf(permissions) != 0x03U) && (inputs.ReservedByte == 0U);
        const ObjectPinError objectPin =
            m_model.PredictPin(inputs.Foreign, inputs.MemoryObject.Value, ObjectType::MemoryObject);
        const ObjectPinError framePin = m_model.PredictPin(inputs.Foreign, inputs.Frame.Value, ObjectType::Frame);
        Size kind{0U};
        const Size object = m_model.FindLive(inputs.Foreign, inputs.MemoryObject.Value, kind);
        const Size frame = m_model.FindLive(inputs.Foreign, inputs.Frame.Value, kind);
        const MemoryObjectConfiguration& objectConfiguration = MemoryObjectConfigurations[object % MemoryObjectCount];
        const FrameConfiguration& frameConfiguration = FrameConfigurations[frame % FrameCount];
        Error error{Error::None};
        const bool accepted =
            Pass(m_model.Preparations[slot].Prepared, Error::AlreadyPrepared, error) &&
            Pass(inputs.MemoryObject.Value == 0U, Error::InvalidMemoryObject, error) &&
            Pass(inputs.Frame.Value == 0U, Error::InvalidFrame, error) &&
            Pass((inputs.MemoryObject.Foreign != inputs.Foreign) || (inputs.Frame.Foreign != inputs.Foreign),
                 Error::RegistryMismatch, error) &&
            Pass(!requestValid, Error::InvalidRequest, error) &&
            Pass(objectPin != ObjectPinError::Ok,
                 PinFailure(objectPin, {Error::MemoryObjectNotFound, Error::MemoryObjectTypeMismatch,
                                        Error::MemoryObjectPinUnavailable}),
                 error) &&
            Pass(object != inputs.MemoryObjectInstance, Error::MemoryObjectInstanceMismatch, error) &&
            Pass(m_model.MemoryObjects[object].State != MemoryObjectState::Ready, Error::InvalidMemoryObject, error) &&
            Pass((inputs.MemoryObjectOffset > objectConfiguration.Pages) ||
                     (pages > (objectConfiguration.Pages - inputs.MemoryObjectOffset)),
                 Error::MemoryObjectRangeExceeded, error) &&
            Pass((permissions & static_cast<UInt8>(~objectConfiguration.MaximumPermissions)) != 0U,
                 Error::MemoryObjectAccessDenied, error) &&
            Pass(objectConfiguration.Commitment != MemoryCommitment::Pinned, Error::MemoryObjectCommitmentMismatch,
                 error) &&
            Pass(framePin != ObjectPinError::Ok,
                 PinFailure(framePin, {Error::FrameNotFound, Error::FrameTypeMismatch, Error::FramePinUnavailable}),
                 error) &&
            Pass(frame != inputs.FrameInstance, Error::FrameInstanceMismatch, error) &&
            Pass(frameConfiguration.Kind != FrameKind::Ram, Error::UnsupportedFrameKind, error) &&
            Pass((inputs.FrameOffset > frameConfiguration.Pages) ||
                     (pages > (frameConfiguration.Pages - inputs.FrameOffset)),
                 Error::FrameRangeExceeded, error) &&
            Pass(frameConfiguration.CachePolicy != objectConfiguration.CachePolicy, Error::MemoryObjectCacheMismatch,
                 error);
        if (accepted)
        {
            ObjectRegistry& registry = RegistryFor(inputs.Foreign);
            mapping = MappingModel{
                Record{MemoryObjectId{registry, ObjectId{inputs.MemoryObject.Value}},
                       {},
                       FrameId{registry, ObjectId{inputs.Frame.Value}},
                       VirtualAddress{inputs.VirtualStart},
                       PhysicalAddress{frameConfiguration.PhysicalStart + (inputs.FrameOffset * PageBytes)},
                       PageCount{pages},
                       inputs.MemoryObjectOffset,
                       PermissionsFrom<PagePermissions, PagePermission>(permissions),
                       frameConfiguration.CachePolicy,
                       FrameKind::Ram,
                       MemoryCommitment::Pinned,
                       true,
                       {}},
                object, frame};
        }
        return error;
    }

    [[nodiscard]] bool RunPrepare(const Size slot, const PrepareInputs& inputs) noexcept
    {
        MappingModel mapping{};
        const PinnedFrameMappingError expected = PredictPrepare(slot, inputs, mapping);
        Kernel::Memory::PinnedFrameMappingRequest request{
            VirtualAddress{inputs.VirtualStart},
            PageCount{inputs.Pages},
            inputs.MemoryObjectOffset,
            inputs.FrameOffset,
            PermissionsFrom<PagePermissions, PagePermission>(inputs.Permissions),
            {}};
        request.Reserved[inputs.ReservedIndex] = inputs.ReservedByte;
        const PinnedFrameMappingError actual = m_system.Preparations[slot].Prepare(
            RegistryFor(inputs.Foreign),
            (inputs.MemoryObject.Value == 0U)
                ? MemoryObjectId{}
                : MemoryObjectId{RegistryFor(inputs.MemoryObject.Foreign), ObjectId{inputs.MemoryObject.Value}},
            m_system.MemoryObjects[inputs.MemoryObjectInstance],
            (inputs.Frame.Value == 0U) ? FrameId{}
                                       : FrameId{RegistryFor(inputs.Frame.Foreign), ObjectId{inputs.Frame.Value}},
            m_system.Frames[inputs.FrameInstance], request);
        if (actual == PinnedFrameMappingError::None)
        {
            m_model.Preparations[slot] = PreparationModel{true, inputs.Foreign, mapping};
            ++m_model.MemoryObjects[mapping.MemoryObject].Object.Pins;
            ++m_model.Frames[mapping.Frame].Object.Pins;
        }
        return Same("prepare result", expected, actual);
    }

    void DiscardPreparation(const Size slot) noexcept
    {
        PreparationModel& preparation = m_model.Preparations[slot];
        m_system.Preparations[slot].~PinnedFrameMappingPreparation();
        new (&m_system.Preparations[slot]) PinnedFrameMappingPreparation{};
        m_model.MemoryObjects[preparation.Mapping.MemoryObject % MemoryObjectCount].Object.Pins -=
            preparation.Prepared ? 1U : 0U;
        m_model.Frames[preparation.Mapping.Frame % FrameCount].Object.Pins -= preparation.Prepared ? 1U : 0U;
        preparation.Prepared = false;
    }

    // Usually the root's latest token; sometimes a stale, foreign, or invalid one.
    [[nodiscard]] IssuedToken PickToken(const Size index) noexcept
    {
        const UInt64 mode = m_random.Below(20U);
        const IssuedToken other = (mode < 19U) ? m_latest[OtherSpace(index)] : IssuedToken{};
        const IssuedToken own = (mode < 15U) ? m_latest[index] : m_previous[index];
        return (mode < 17U) ? own : other;
    }

    [[nodiscard]] bool RunBegin(const Size index, const Size slot, const Size target,
                                const MapInjection injection) noexcept
    {
        const PreparationModel& preparation = m_model.Preparations[slot];
        const UInt64 bound = m_model.Spaces[index].BoundIdentity;
        const AddressSpaceId targets[5U]{SpaceIdentity(bound), AddressSpaceId{},
                                         AddressSpaceId{m_system.ForeignRegistry, ObjectId{bound}},
                                         SpaceIdentity(m_model.Spaces[OtherSpace(index)].BoundIdentity),
                                         AddressSpaceId{m_system.Registry, ObjectId{UnissuedIdentity}}};
        const BeginExpectation expected =
            m_model.PredictBegin(index, preparation, (target == ExactIdentity) && (bound != 0U), injection);
        m_system.PageTables[index].Outcome = injection.Outcome;
        m_system.PageTables[index].MapError = injection.Error;
        const auto actual =
            m_system.Spaces[index].BeginPinnedFrameMapping(targets[target], m_system.Preparations[slot]);
        const bool published = expected.Effect == BeginEffect::Published;
        if (!SameOutcome("begin", published, Encode(expected.Failure), actual) ||
            ((expected.Effect != BeginEffect::Rejected) &&
             !SameRecord(m_system.PageTables[index].LastMapping, preparation.Mapping.Value)))
        {
            return false;
        }
        m_model.ApplyBegin(index, preparation.Mapping, expected.Effect);
        const AddressSpaceMappingToken token = actual.GetValue().GetToken();
        if (published)
        {
            m_previous[index] = m_latest[index];
            m_latest[index] = IssuedToken{token, index};
        }
        return !published || (Same("token", true, token.IsFor(m_system.Spaces[index])) &&
                              Same("token sequence", m_model.Spaces[index].MappingSequence, token.GetSequence()) &&
                              Same("invalidation start", preparation.Mapping.Value.VirtualStart.GetValue(),
                                   actual.GetValue().GetInvalidationStart().GetValue()) &&
                              Same("invalidation pages", preparation.Mapping.Value.Pages.GetValue(),
                                   actual.GetValue().GetInvalidationPages().GetValue()));
    }

    [[nodiscard]] UInt8 PickShootdownProcessor(const Size index) noexcept
    {
        const SpaceModel& space = m_model.Spaces[index];
        const UInt64 mode = m_random.Below(20U);
        const Foundation::ProcessorMask acknowledged = (mode < 15U) ? space.AcknowledgedMask : UInt8{0x0FU};
        const UInt8 pending = static_cast<UInt8>(space.TargetMask & static_cast<UInt8>(~space.AcknowledgedMask));
        const Foundation::ProcessorMask pool = (mode < 13U) ? pending : acknowledged;
        const UInt8 start = static_cast<UInt8>(m_random.Below(ProcessorLimit));
        UInt8 processor = start;
        for (UInt8 offset{0U}; (offset < ProcessorLimit) && ((pool & ProcessorBit(processor)) == 0U); ++offset)
        {
            processor = static_cast<UInt8>((start + offset + 1U) % ProcessorLimit);
        }
        return (mode >= 18U) ? static_cast<UInt8>(ProcessorLimit + m_random.Below(0x100U - ProcessorLimit)) : processor;
    }

    [[nodiscard]] bool RunReport(const Size index, const IssuedToken& token, const UInt8 processor,
                                 const bool reject) noexcept
    {
        const AddressSpaceError expected = m_model.Report(index, token.Token, token.Space, processor, reject);
        return Same("shootdown report", expected,
                    reject ? m_system.Spaces[index].RejectShootdown(token.Token, CpuId{processor})
                           : m_system.Spaces[index].AcknowledgeShootdown(token.Token, CpuId{processor}));
    }

    // Complete (or its fault alias) or, when failing, FailShootdown.
    [[nodiscard]] bool RunFinish(const Size index, const IssuedToken& token, const bool fail) noexcept
    {
        const AddressSpaceError expected = m_model.Finish(index, token.Token, token.Space, fail);
        if (fail)
        {
            return Same("fail shootdown result", expected, m_system.Spaces[index].FailShootdown(token.Token));
        }
        const auto actual = m_random.Chance(1U, 4U) ? m_system.Spaces[index].CompleteFaultMapping(token.Token)
                                                    : m_system.Spaces[index].CompleteMapping(token.Token);
        const SpaceModel& space = m_model.Spaces[index];
        const Size last = space.SeedCount + space.RuntimeCount - 1U;
        const Record& record = space.Mappings[last % TotalCapacity].Value;
        const AddressSpaceFailure failure{expected, {}, {}, {}};
        return SameOutcome("complete", expected == AddressSpaceError::None, Encode(failure), actual) &&
               ((expected != AddressSpaceError::None) ||
                (Same("receipt", true,
                      (actual.GetValue().MappingSequence == space.MappingSequence) &&
                          (actual.GetValue().MappingCount == (last + 1U)) &&
                          (actual.GetValue().VirtualStart.GetValue() == record.VirtualStart.GetValue()) &&
                          (actual.GetValue().Pages.GetValue() == record.Pages.GetValue())) &&
                 SameRecord(m_system.Spaces[index].TryGetMapping(last).GetValue(), record)));
    }

    // Every target reports once in a random rotation; then the initiator completes.
    [[nodiscard]] bool AcknowledgeAllAndComplete(const Size index, const bool allowRejection) noexcept
    {
        const UInt8 start = static_cast<UInt8>(m_random.Below(ProcessorLimit));
        return ForAll(ProcessorLimit,
                      [&](const Size offset) noexcept
                      {
                          const UInt8 processor = static_cast<UInt8>((start + offset) % ProcessorLimit);
                          const bool reject = allowRejection && !IsCapacityRoot(index) && m_random.Chance(1U, 50U);
                          return ((m_model.Spaces[index].TargetMask & ProcessorBit(processor)) == 0U) ||
                                 RunReport(index, m_latest[index], processor, reject);
                      }) &&
               RunFinish(index, m_latest[index], false);
    }

    // Prefers a current receipt; otherwise a held (maybe stale) or rarely an invalid one.
    [[nodiscard]] HeldReceipt PickReceipt(const UInt64 currentPercent) noexcept
    {
        const Size start = static_cast<Size>(m_random.Below(2U * SpaceCount));
        for (Size offset{0U}; m_random.Chance(currentPercent, 100U) && (offset < (2U * SpaceCount)); ++offset)
        {
            const HeldReceipt& held = m_receipts[((start + offset) / 2U) % SpaceCount][(start + offset) % 2U];
            if (m_model.IsCurrent(held.Model.Space, held.Model))
            {
                return held;
            }
        }
        return m_random.Chance(1U, 12U) ? HeldReceipt{} : m_receipts[start / 2U][start % 2U];
    }

    [[nodiscard]] bool SameActivation(
        const ActivationOutcome& expected,
        const Foundation::Result<Kernel::Memory::AddressSpaceActivationReceipt, AddressSpaceActivationFailure>& actual,
        const UInt8 processor) noexcept
    {
        return SameOutcome("activation", expected.Success, Encode(expected.Failure), actual) &&
               (!expected.Success || Same("activation receipt", true,
                                          (actual.GetValue().Sequence == expected.Sequence) &&
                                              (actual.GetValue().Processor.GetValue() == processor)));
    }

    [[nodiscard]] bool RunReserve(const Size index, const UInt8 processor) noexcept
    {
        UInt64 sequence{0U};
        const AddressSpaceError expected = m_model.Reserve(index, processor, sequence);
        const auto actual = m_system.Spaces[index].ReserveProcessorTransition(CpuId{processor});
        const ReceiptModel model{true, index, sequence, processor};
        const bool success = expected == AddressSpaceError::None;
        if (!SameOutcome("reserve", success, Encode(expected), actual) || !actual.HasValue())
        {
            return actual.HasValue() == success;
        }
        // The replaced receipt is stale: the root held no reservation.
        m_receipts[index][processor - FirstRawProcessor] = HeldReceipt{actual.GetValue(), model};
        return SameReceipt(actual.GetValue(), model);
    }

    // ActivateReservedProcessor, DetachReservedProcessor, or ReleaseProcessorTransition.
    [[nodiscard]] bool RunReceiptCall(const ReceiptCall call, const HeldReceipt& held, const Size index) noexcept
    {
        AddressSpace& space = m_system.Spaces[index];
        if (call == ReceiptCall::Activate)
        {
            const ActivationError backend = PickActivationInjection(FaultPercent(index));
            m_system.PageTables[index].ActivationResult = backend;
            return SameActivation(m_model.Activate(index, 0U, &held.Model, backend),
                                  space.ActivateReservedProcessor(held.Receipt), held.Model.Processor);
        }
        if (call == ReceiptCall::Detach)
        {
            return Same("detach reserved", m_model.Detach(index, 0U, &held.Model),
                        space.DetachReservedProcessor(held.Receipt));
        }
        return Same("release", m_model.Release(index, held.Model), space.ReleaseProcessorTransition(held.Receipt));
    }

    // Bare ActivateProcessor (activate true) or DetachProcessor.
    [[nodiscard]] bool RunBare(const bool activate, const Size index, const UInt8 processor) noexcept
    {
        if (!activate)
        {
            return Same("detach", m_model.Detach(index, processor, nullptr),
                        m_system.Spaces[index].DetachProcessor(CpuId{processor}));
        }
        const ActivationError backend = PickActivationInjection(FaultPercent(index));
        m_system.PageTables[index].ActivationResult = backend;
        return SameActivation(m_model.Activate(index, processor, nullptr, backend),
                              m_system.Spaces[index].ActivateProcessor(CpuId{processor}), processor);
    }

    [[nodiscard]] bool ReleaseRawReservations(const Size only) noexcept
    {
        return ForAll(2U * SpaceCount,
                      [&](const Size entry) noexcept
                      {
                          const HeldReceipt held = m_receipts[entry / 2U][entry % 2U];
                          const bool selected = (only == NoIndex) || (held.Model.Space == only);
                          return !(selected && m_model.IsCurrent(held.Model.Space, held.Model)) ||
                                 RunReceiptCall(ReceiptCall::Release, held, held.Model.Space);
                      });
    }

    [[nodiscard]] bool DetachRawProcessors(const Size index) noexcept
    {
        return ForAll(2U,
                      [&](const Size offset) noexcept
                      {
                          const UInt8 processor = static_cast<UInt8>(FirstRawProcessor + offset);
                          const bool resident = (m_model.Spaces[index].ResidentMask & ProcessorBit(processor)) != 0U;
                          return !resident || RunBare(false, index, processor);
                      });
    }

    [[nodiscard]] bool RunControllerInitialize(const Size index, const UInt8 processor, const Size idle,
                                               const bool allowFaults) noexcept
    {
        const ActivationError backend =
            allowFaults ? PickActivationInjection(2U * FaultPercent(idle)) : ActivationError::None;
        m_system.PageTables[idle].ActivationResult = backend;
        const ControllerExpectation expected = m_model.ControllerInitialize(index, processor, idle, backend);
        return SameController(expected,
                              m_system.Controllers[index].Initialize(CpuId{processor}, m_system.Spaces[idle]));
    }

    // SwitchToIdle when destination is NoIndex, otherwise SwitchTo.
    [[nodiscard]] bool SwitchController(const Size index, const Size destination, const ActivationError backend,
                                        bool& succeeded) noexcept
    {
        const ControllerModel& controller = m_model.Controllers[index];
        const Size target = (destination == NoIndex) ? controller.Idle : destination;
        if (target != NoIndex)
        {
            m_system.PageTables[target].ActivationResult = backend;
        }
        const bool faulted = controller.State == ControllerState::Faulted;
        const ControllerError noIdle = faulted ? ControllerError::Faulted : ControllerError::NotReady;
        const ControllerExpectation expected = (target == NoIndex) ? ControllerRefusal(noIdle, !faulted)
                                                                   : m_model.ControllerSwitch(index, target, backend);
        succeeded = expected.Success;
        ProcessorAddressSpaceController& instance = m_system.Controllers[index];
        return SameController(expected, (destination == NoIndex) ? instance.SwitchToIdle()
                                                                 : instance.SwitchTo(m_system.Spaces[target]));
    }

    // Fail-stop teardown: retained receipts detach and release, bare detach removes the
    // rest; the controller is reconstructed once its CPU left every root.
    [[nodiscard]] bool RetireController(const Size index) noexcept
    {
        const ControllerModel& controller = m_model.Controllers[index];
        const auto snapshot = m_system.Controllers[index].GetSnapshot();
        const HeldReceipt retained[2U]{{snapshot.SourceTransition, controller.Source},
                                       {snapshot.DestinationTransition, controller.Destination}};
        const Foundation::ProcessorMask bit = ProcessorBit(static_cast<UInt8>(index));
        const bool released =
            ForAll(2U,
                   [&](const Size which) noexcept
                   {
                       const HeldReceipt& held = retained[which];
                       return !held.Model.Valid || (RunReceiptCall(ReceiptCall::Detach, held, held.Model.Space) &&
                                                    RunReceiptCall(ReceiptCall::Release, held, held.Model.Space));
                   }) &&
            ForAll(SpaceCount,
                   [&](const Size space) noexcept
                   {
                       const bool resident = (m_model.Spaces[space].ResidentMask & bit) != 0U;
                       return !resident || RunBare(false, space, static_cast<UInt8>(index));
                   });
        const bool resident =
            ((m_model.Spaces[0U].ResidentMask | m_model.Spaces[1U].ResidentMask | m_model.Spaces[2U].ResidentMask) &
             bit) != 0U;
        if (released && !resident)
        {
            m_system.Controllers[index].~ProcessorAddressSpaceController();
            new (&m_system.Controllers[index]) ProcessorAddressSpaceController{};
            m_model.Controllers[index] = ControllerModel{};
        }
        return released;
    }

    // A random root, usually moved to a shootdown-pending one (pending) or to one that is
    // neither Ready nor pending (not pending) when there is one.
    [[nodiscard]] Size PreferSpace(const bool pending, const UInt64 percent) noexcept
    {
        const Size index = PickSpace();
        for (Size offset{0U}; m_random.Chance(percent, 100U) && (offset < SpaceCount); ++offset)
        {
            const AddressSpaceState state = m_model.Spaces[(index + offset) % SpaceCount].State;
            const bool shootdown = state == AddressSpaceState::ShootdownPending;
            if ((pending && shootdown) || (!pending && !shootdown && (state != AddressSpaceState::Ready)))
            {
                return (index + offset) % SpaceCount;
            }
        }
        return index;
    }

    [[nodiscard]] bool OpPrepare() noexcept
    {
        const Size slot = static_cast<Size>(m_random.Below(PreparationCount));
        const Size index = PickSpace();
        if (m_random.Chance(4U, 15U))
        {
            DiscardPreparation(slot);
            return true;
        }
        return RunPrepare(slot, m_random.Chance(1U, 2U) ? SuccessfulInputs(index, slot) : PerturbedInputs(index));
    }

    [[nodiscard]] bool OpBegin() noexcept
    {
        const Size index = PickSpace();
        Size slot = static_cast<Size>(m_random.Below(PreparationCount));
        for (Size attempt{0U}; (attempt < 3U) && !m_model.Preparations[slot].Prepared; ++attempt)
        {
            slot = static_cast<Size>(m_random.Below(PreparationCount));
        }
        const UInt64 target = m_random.Below(40U);
        const MapInjection injection = PickMapInjection(FaultPercent(index));
        return RunBegin(index, slot, (target < 4U) ? (target + 1U) : ExactIdentity, injection);
    }

    // One mapping chosen to publish, from preparation to completion.
    [[nodiscard]] bool OpBurst() noexcept
    {
        Size index = PickSpace();
        index = ((m_model.Mode == Profile::RuntimeCapacity) && m_random.Chance(3U, 4U)) ? CapacitySpace : index;
        index = ((m_model.Mode == Profile::FramePolicy) && m_random.Chance(3U, 4U)) ? 1U + m_random.Below(2U) : index;
        const Size slot = static_cast<Size>(m_random.Below(PreparationCount));
        DiscardPreparation(slot);
        const bool policy = m_model.Mode == Profile::FramePolicy;
        return RunPrepare(slot, policy ? FreshPolicyInputs(index) : SuccessfulInputs(index, slot)) &&
               (!m_model.Preparations[slot].Prepared ||
                (RunBegin(index, slot, ExactIdentity, PickMapInjection(FaultPercent(index) / 2U)) &&
                 ((m_model.Spaces[index].State != AddressSpaceState::ShootdownPending) ||
                  AcknowledgeAllAndComplete(index, true))));
    }

    [[nodiscard]] bool OpReport() noexcept
    {
        const Size index = PreferSpace(true, 80U);
        const IssuedToken token = PickToken(index);
        const UInt8 processor = PickShootdownProcessor(index);
        return RunReport(index, token, processor, m_random.Chance(1U, 6U) && !IsCapacityRoot(index));
    }

    [[nodiscard]] bool OpFinish() noexcept
    {
        const Size index = PreferSpace(true, 80U);
        const bool fail = m_random.Chance(1U, 11U);
        return (fail && IsCapacityRoot(index)) || RunFinish(index, PickToken(index), fail);
    }

    [[nodiscard]] bool OpRaw() noexcept
    {
        const UInt64 kind = m_random.Below(205U);
        const Size index = PickSpace();
        const UInt8 processor = PickRawProcessor();
        if (kind < 45U)
        {
            return RunReserve(m_random.Chance(2U, 5U) ? 0U : index, processor);
        }
        if (kind >= 145U)
        {
            return RunBare(m_random.Chance(1U, 2U), index, processor);
        }
        const ReceiptCall detachOrRelease = (kind < 105U) ? ReceiptCall::Detach : ReceiptCall::Release;
        const ReceiptCall call = (kind < 80U) ? ReceiptCall::Activate : detachOrRelease;
        const HeldReceipt held = PickReceipt((call == ReceiptCall::Release) ? 90U : 67U);
        const bool elsewhere = !held.Model.Valid || m_random.Chance(1U, 8U);
        return RunReceiptCall(call, held, elsewhere ? index : held.Model.Space);
    }

    [[nodiscard]] bool OpController() noexcept
    {
        const Size index = static_cast<Size>(m_random.Below(ControllerCount));
        const UInt64 kind = m_random.Below(85U);
        if (kind < 15U)
        {
            const UInt8 processor =
                m_random.Chance(1U, 20U) ? static_cast<UInt8>(4U + m_random.Below(0xFCU)) : static_cast<UInt8>(index);
            return RunControllerInitialize(index, processor, m_random.Chance(4U, 5U) ? 0U : PickSpace(), true);
        }
        if (kind < 25U)
        {
            return RetireController(index);
        }
        const Size destination = m_random.Chance(2U, 5U) ? NoIndex : PickSpace();
        const Size target = (destination == NoIndex) ? m_model.Controllers[index].Idle : destination;
        bool switched{false};
        return SwitchController(index, destination, PickActivationInjection(2U * FaultPercent(target)), switched);
    }

    // A root that quiesces is rebuilt at once; capacity roots are only probed.
    [[nodiscard]] bool OpQuiesce() noexcept
    {
        const Size index = PickSpace();
        const SpaceModel& space = m_model.Spaces[index];
        const bool wouldQuiesce = (space.State != AddressSpaceState::Uninitialized) &&
                                  (space.State != AddressSpaceState::Quiesced) &&
                                  (space.State != AddressSpaceState::ShootdownPending) && (space.ResidentMask == 0U);
        return (wouldQuiesce && (IsCapacityRoot(index) || m_random.Chance(1U, 2U))) ||
               (QuiesceSpace(index, true) && Rebuild(index));
    }

    [[nodiscard]] bool OpLifecycle() noexcept
    {
        const Size index = PreferSpace(false, 75U);
        const AddressSpaceState state = m_model.Spaces[index].State;
        if ((state != AddressSpaceState::Unbound) && m_random.Chance(1U, 4U) && !BindSpace(index, ExactIdentity))
        {
            return false;
        }
        if (state == AddressSpaceState::Faulted)
        {
            return RecoverSpace(index) && Rebuild(index);
        }
        if ((state != AddressSpaceState::Ready) && (state != AddressSpaceState::ShootdownPending))
        {
            return Rebuild(index) && ((state != AddressSpaceState::Unbound) || m_random.Chance(1U, 8U) ||
                                      BindSpace(index, ExactIdentity));
        }
        const UInt64 mode = m_random.Below(6U);
        return (mode == 0U) ? InitializeSpace(index, false)
                            : ((mode != 1U) || IsCapacityRoot(index) || (RecoverSpace(index) && Rebuild(index)));
    }

    // A pin-free object is destroyed, collected, and recreated; a pinned one is probed or,
    // in the general profiles, quiesced or destroyed while its holders retain it.
    [[nodiscard]] bool OpObjectLifecycle() noexcept
    {
        const bool memoryObject = m_random.Chance(3U, 5U);
        const Size index = static_cast<Size>(m_random.Below(memoryObject ? HomeMemoryObjectCount : HomeFrameCount));
        ObjectState& object = memoryObject ? m_model.MemoryObjects[index].Object : m_model.Frames[index].Object;
        const UInt64 mode = m_random.Below(8U);
        if (mode == 7U)
        {
            return CollectOnce();
        }
        if (!object.Registered)
        {
            return Recreate(memoryObject, index);
        }
        if (object.Quiescing)
        {
            return m_random.Chance(3U, 4U) ? CollectOnce() : DestroyObject(object);
        }
        if (object.Pins == 0U)
        {
            return (!memoryObject || m_random.Chance(1U, 2U) || QuiesceMemoryObject(index)) && DestroyObject(object) &&
                   CollectUntilGone(object) && Recreate(memoryObject, index);
        }
        const bool general = (m_model.Mode == Profile::General) || (m_model.Mode == Profile::TightPins);
        return ((mode != 0U) || !memoryObject || QuiesceMemoryObject(index)) &&
               ((mode != 1U) || !general || DestroyObject(object));
    }

    System& m_system;
    Model& m_model;
    Random m_random;
    UInt64 m_seed;
    UInt64 m_faultPercent;
    Size m_step{0U};
    const char* m_operation{"setup"};
    IssuedToken m_latest[SpaceCount]{};
    IssuedToken m_previous[SpaceCount]{};
    HeldReceipt m_receipts[SpaceCount][2U]{}; // Latest receipt per root and raw CPU
};

// The fixtures are hundreds of kilobytes; they live in static storage and are
// placement-constructed per seed because tests may not allocate.
alignas(System) unsigned char g_systemStorage[sizeof(System)];
alignas(Model) unsigned char g_modelStorage[sizeof(Model)];
alignas(Driver) unsigned char g_driverStorage[sizeof(Driver)];

// Runs one seed, or the characterizations on a fresh General-profile system.
void RunSeed(TestContext& context, const UInt64 seed, const bool characterize, const bool privateIdle = false) noexcept
{
    const Profile profile = characterize ? Profile::General : static_cast<Profile>(seed % 4U);
    // TightPins allows four to six references per object: two preparations and a few mappings.
    const UInt32 pinLimit = (profile == Profile::TightPins) ? static_cast<UInt32>(4U + (seed % 3U)) : UnlimitedPins;
    System* const system = new (g_systemStorage) System{pinLimit};
    Model* const model = new (g_modelStorage) Model{profile, pinLimit};
    Driver* const driver = new (g_driverStorage) Driver{*system, *model, seed};
    bool matched = driver->Initialize();
    if (privateIdle)
    {
        ASTERIA_EXPECT(context, "private idle root is adopted only while idle and leaves the other CPU's root intact",
                       matched, driver->CheckPrivateIdleRoots());
    }
    else if (characterize)
    {
        ASTERIA_EXPECT(context, "a reserving Frame's claim refuses overlapping Frames in every root", matched,
                       driver->CheckPhysicalFrameClaims());
        ASTERIA_EXPECT(context, "another processor's reservation permits return to the shared idle root", matched,
                       driver->CharacterizeIdleContention());
    }
    for (Size step{0U}; !characterize && !privateIdle && matched && (step < StepsPerSeed); ++step)
    {
        matched = driver->Step(step);
    }
    if (!characterize && !privateIdle)
    {
        matched = matched && driver->Finish();
        if (!matched)
        {
            static_cast<void>(std::fprintf(stderr, "  reproduce with ASTERIA_TEST_SEED=%llu\n",
                                           static_cast<unsigned long long>(seed)));
        }
        ASTERIA_EXPECT(context, "AddressSpace mapping transactions match their reference model for one seed", matched);
    }
    driver->~Driver();
    model->~Model();
    system->~System();
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"AddressSpaceModelTests"};
    Asteria::Test::RunSeed(runner, 0U, true, true);
    Asteria::Test::RunSeed(runner, 0U, true);
    const Asteria::TestSupport::SeedRange seeds = Asteria::TestSupport::GetSeedRange(1U, 64U);
    for (Asteria::Foundation::UInt64 seed = seeds.First; seed < seeds.First + seeds.Count; ++seed)
    {
        Asteria::Test::RunSeed(runner, seed, false);
    }
    return runner.Finish();
}
