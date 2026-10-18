// SPDX-License-Identifier: Apache-2.0
#include "Kernel/Object/CapabilitySpace.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RevocationRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <new>

#include "Support/ManualClock.hpp"
#include "Support/ObjectRegistryFixture.hpp"
#include "Support/Random.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

// Model-based test of the capability authority: CapabilitySpace together with
// ObjectRegistry, RevocationRegistry, and RootCapabilityIssuer.
//
// A plain reference model of the documented semantics runs next to the real
// implementation. Every seeded step applies one random operation — including
// stale, forged, cross-space, expired, and revoked handles, and destroyed and
// reused revocation domains — to both, then compares the result code, the slot
// and generation of every issued handle and domain, Query metadata, and the
// active counts. A mismatch reports its seed; set ASTERIA_TEST_SEED to replay it.
namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::UInt8;
using Kernel::Object::CapabilityDescriptor;
using Kernel::Object::CapabilityError;
using Kernel::Object::CapabilityHandle;
using Kernel::Object::CapabilityLookupResult;
using Kernel::Object::CapabilityOperation;
using Kernel::Object::CapabilityRights;
using Kernel::Object::CapabilitySpace;
using Kernel::Object::CapabilityTransferBatch;
using Kernel::Object::CapabilityTransferDisposition;
using Kernel::Object::CapabilityTransferRequest;
using Kernel::Object::CapabilityTransferRequestView;
using Kernel::Object::DerivedRevocationDomain;
using Kernel::Object::IKernelObject;
using Kernel::Object::MonotonicTime;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectRegistryError;
using Kernel::Object::ObjectType;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RevocationDomainId;
using Kernel::Object::RevocationError;
using Kernel::Object::RevocationRegistry;
using Kernel::Object::RootCapabilityIssuer;
using TestSupport::ManualClock;
using TestSupport::Random;

constexpr Size SpaceCount{2U};
constexpr Size SlotCount{CapabilitySpace::MaximumSlots};
constexpr Size ObjectCount{12U};
constexpr Size DomainCapacity{RevocationRegistry::MaximumDomains};
// Every domain ever created, live or destroyed; registry slots are reused.
constexpr Size DomainHistory{256U};
constexpr UInt8 DomainDepthLimit{RevocationRegistry::MaximumDepth};
constexpr Size IssuedCapacity{4096U};
constexpr Size HeldLeaseCapacity{4U};
constexpr Size StepsPerSeed{1500U};
constexpr UInt64 InfiniteTicks{~UInt64{0U}};
constexpr UInt16 KnownRights{0x3FFFU};
// Only a branch in its own child revocation domain may carry Revoke.
constexpr UInt16 RevokeRight{static_cast<UInt16>(Kernel::Object::CapabilityRight::Revoke)};
constexpr Size NoIndex{~Size{0U}};

class ModelledObject final : public IKernelObject
{
public:
    void Configure(const ObjectType type, const UInt64 chargeOwner) noexcept
    {
        m_type = type;
        m_chargeOwner = ResourceDomainId{chargeOwner};
    }

    [[nodiscard]] ObjectType GetObjectType() const noexcept override
    {
        return m_type;
    }

    [[nodiscard]] ResourceDomainId GetChargeOwner() const noexcept override
    {
        return m_chargeOwner;
    }

private:
    ObjectType m_type{ObjectType::Endpoint};
    ResourceDomainId m_chargeOwner{1U};
};

struct DomainState
{
    RevocationDomainId Identity;
    Size Parent;
    UInt32 Epoch;
    UInt32 ParentEpochAtCreation;
    UInt8 Depth;
    bool Live;
};

struct ObjectState
{
    bool Registered;
    bool Quiescing;
    ObjectId Identity;
    ObjectType Type;
    UInt64 ChargeOwner;
    UInt32 Leases;
};

struct SlotState
{
    bool Occupied;
    UInt64 Generation;
    ObjectId Object;
    ObjectType Type;
    UInt16 Rights;
    Size Domain;
    UInt32 Epoch;
    UInt64 Deadline;
    UInt64 ChargeOwner;
    UInt64 Sealed;
};

struct IssuedHandle
{
    Size Space;
    CapabilityHandle Handle;
};

// Operation outcome predicted by the model. For successes that create
// capabilities, Slots lists the destination slots in issue order.
struct Prediction
{
    CapabilityError Error;
    Size Space;
    Size Slots[CapabilityTransferBatch::MaximumTransfers];
    Size SlotCount;
};

// Real system under test.
struct System
{
    Platform::Synchronization::SpinLock Lock;
    Asteria::TestSupport::TestObjectRegistry Registry{Lock};
    RevocationRegistry Revocations{Lock};
    RootCapabilityIssuer Issuer{Registry};
    ManualClock Clock{1000U};
    CapabilitySpace Spaces[SpaceCount]{
        {Registry, Revocations, Clock, 0x0123'4567'89AB'CDEFULL, 0x0F1E'2D3C'4B5A'6978ULL},
        {Registry, Revocations, Clock, 0x7777'1111'2222'3333ULL, 0x9999'AAAA'BBBB'CCCCULL},
    };
    ModelledObject Objects[ObjectCount];
    alignas(CapabilityLookupResult) unsigned char HeldStorage[HeldLeaseCapacity][sizeof(CapabilityLookupResult)]{};
    Size HeldObject[HeldLeaseCapacity]{};
    CapabilityLookupResult* Held[HeldLeaseCapacity]{};

    ~System() noexcept
    {
        for (CapabilityLookupResult* const held : Held)
        {
            if (held != nullptr)
            {
                held->~CapabilityLookupResult();
            }
        }
    }
};

[[nodiscard]] CapabilityRights RightsFrom(const UInt16 bits) noexcept
{
    CapabilityRights rights{};
    for (UInt16 bit{0U}; bit < 16U; ++bit)
    {
        if ((bits & (UInt16{1U} << bit)) != 0U)
        {
            rights = rights.With(static_cast<Kernel::Object::CapabilityRight>(UInt16{1U} << bit));
        }
    }
    return rights;
}

[[nodiscard]] constexpr UInt16 RightFor(const CapabilityOperation operation) noexcept
{
    return static_cast<UInt16>(UInt16{1U} << static_cast<UInt8>(operation));
}

class Model final
{
public:
    Model() noexcept
    {
        for (Size space{0U}; space < SpaceCount; ++space)
        {
            for (SlotState& slot : m_slots[space])
            {
                slot = SlotState{};
            }
        }
        for (Size& slot : DomainSlots)
        {
            slot = NoIndex;
        }
    }

    UInt64 Now{1000U};
    ObjectState Objects[ObjectCount]{};
    DomainState Domains[DomainHistory]{};
    Size DomainCount{0U};
    // Registry slots: the live history index (or NoIndex) and the generation
    // the next domain in that slot receives.
    Size DomainSlots[DomainCapacity]{};
    UInt16 SlotGenerations[DomainCapacity]{};

    [[nodiscard]] Size LiveDomains() const noexcept
    {
        Size count{0U};
        for (Size index{0U}; index < DomainCount; ++index)
        {
            count += Domains[index].Live ? 1U : 0U;
        }
        return count;
    }

    // Destroying a domain ends it and every descendant (by parent chain).
    [[nodiscard]] bool InSubtree(const Size domain, const Size root) const noexcept
    {
        for (Size current{domain}; current != NoIndex; current = Domains[current].Parent)
        {
            if (current == root)
            {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] SlotState& Slot(const Size space, const Size index) noexcept
    {
        return m_slots[space][index];
    }

    [[nodiscard]] Size Active(const Size space) const noexcept
    {
        Size count{0U};
        for (const SlotState& slot : m_slots[space])
        {
            count += slot.Occupied ? 1U : 0U;
        }
        return count;
    }

    [[nodiscard]] Size RegisteredAlive() const noexcept
    {
        Size count{0U};
        for (const ObjectState& object : Objects)
        {
            count += (object.Registered && !object.Quiescing) ? 1U : 0U;
        }
        return count;
    }

    [[nodiscard]] Size DomainIndex(const RevocationDomainId domain) const noexcept
    {
        for (Size index{0U}; index < DomainCount; ++index)
        {
            if (Domains[index].Identity.GetValue() == domain.GetValue())
            {
                return index;
            }
        }
        return NoIndex;
    }

    [[nodiscard]] bool LineageCurrent(const Size domain) const noexcept
    {
        Size child = domain;
        UInt8 traversed{0U};
        while (Domains[child].Parent != NoIndex)
        {
            if (traversed >= DomainDepthLimit)
            {
                return false;
            }
            const Size parent = Domains[child].Parent;
            if (Domains[parent].Epoch != Domains[child].ParentEpochAtCreation)
            {
                return false;
            }
            child = parent;
            ++traversed;
        }
        return true;
    }

    // Resolves a handle to its slot as CapabilitySpace::ResolveHandleLocked does.
    [[nodiscard]] Size Resolve(const Size space, const IssuedHandle& handle, const bool forged) noexcept
    {
        const UInt64 selector = handle.Handle.GetSelector();
        if ((selector == 0U) || forged || (handle.Space != space))
        {
            return NoIndex;
        }
        const Size index = static_cast<Size>(selector & 0xFFU);
        const UInt64 generation = selector >> 8U;
        const SlotState& slot = m_slots[space][index];
        if ((generation == 0U) || !slot.Occupied || (slot.Generation != generation))
        {
            return NoIndex;
        }
        return index;
    }

    [[nodiscard]] bool ObjectCurrent(const SlotState& slot) const noexcept
    {
        for (const ObjectState& object : Objects)
        {
            if (object.Registered && !object.Quiescing && (object.Identity.GetValue() == slot.Object.GetValue()))
            {
                return (object.Type == slot.Type) && (object.ChargeOwner == slot.ChargeOwner);
            }
        }
        return false;
    }

    // GetUsableDescriptorLocked: handle, revocation, expiry, rights, object.
    [[nodiscard]] CapabilityError Usable(const Size space, const IssuedHandle& handle, const bool forged,
                                         const UInt16 required, Size& index) noexcept
    {
        index = Resolve(space, handle, forged);
        if (index == NoIndex)
        {
            return CapabilityError::InvalidHandle;
        }
        const SlotState& slot = m_slots[space][index];
        if (!Domains[slot.Domain].Live || (Domains[slot.Domain].Epoch != slot.Epoch) || !LineageCurrent(slot.Domain))
        {
            return CapabilityError::Revoked;
        }
        if (Expired(slot.Deadline))
        {
            return CapabilityError::Expired;
        }
        if ((slot.Rights & required) != required)
        {
            return CapabilityError::RightsDenied;
        }
        if (!ObjectCurrent(slot))
        {
            return CapabilityError::ObjectNotFound;
        }
        return CapabilityError::Ok;
    }

    [[nodiscard]] bool Expired(const UInt64 deadline) const noexcept
    {
        return (deadline != InfiniteTicks) && (Now >= deadline);
    }

    [[nodiscard]] Size FirstFree(const Size space) const noexcept
    {
        for (Size index{0U}; index < SlotCount; ++index)
        {
            if (!m_slots[space][index].Occupied)
            {
                return index;
            }
        }
        return NoIndex;
    }

    void Occupy(const Size space, const Size index, const SlotState& value) noexcept
    {
        SlotState& slot = m_slots[space][index];
        const UInt64 generation = (slot.Generation == 0U) ? 1U : slot.Generation;
        slot = value;
        slot.Occupied = true;
        slot.Generation = generation;
    }

    void Free(const Size space, const Size index) noexcept
    {
        SlotState& slot = m_slots[space][index];
        const UInt64 generation = slot.Generation + 1U;
        slot = SlotState{};
        slot.Generation = generation;
    }

private:
    SlotState m_slots[SpaceCount][SlotCount]{};
};

class Driver final
{
public:
    Driver(System& system, Model& model, const UInt64 seed) noexcept
        : m_system{system}, m_model{model}, m_random{seed}, m_seed{seed}
    {
    }

    [[nodiscard]] bool Initialize() noexcept
    {
        for (Size index{0U}; index < ObjectCount; ++index)
        {
            const ObjectType type = (index % 3U == 0U)
                                        ? ObjectType::Endpoint
                                        : ((index % 3U == 1U) ? ObjectType::Notification : ObjectType::MemoryObject);
            m_system.Objects[index].Configure(type, 10U + (index % 4U));
            m_model.Objects[index].Type = type;
            m_model.Objects[index].ChargeOwner = 10U + (index % 4U);
            if (!RegisterObject(index))
            {
                return false;
            }
        }
        return CreateDomain(true, 0U);
    }

    [[nodiscard]] bool Step(const Size step) noexcept
    {
        m_step = step;
        const UInt64 choice = m_random.Below(100U);
        if (choice < 13U)
        {
            return Mint();
        }
        if (choice < 26U)
        {
            return DeriveOrDuplicate(true);
        }
        if (choice < 36U)
        {
            return DeriveOrDuplicate(false);
        }
        if (choice < 46U)
        {
            return Query();
        }
        if (choice < 50U)
        {
            return Revoke();
        }
        if (choice < 60U)
        {
            return Close();
        }
        if (choice < 69U)
        {
            return Lookup();
        }
        if (choice < 72U)
        {
            return ReleaseLease();
        }
        if (choice < 81U)
        {
            return Transfer();
        }
        if (choice < 85U)
        {
            // Half of the child requests extend the deepest lineage so the
            // depth limit is reached; the rest pick any parent or none.
            const UInt64 parent = m_random.Chance(1U, 2U) ? DeepestDomain() : m_random.Below(m_model.DomainCount + 1U);
            return CreateDomain(m_random.Chance(1U, 6U), parent);
        }
        if (choice < 87U)
        {
            return DestroyDomain();
        }
        if (choice < 90U)
        {
            return DestroyObject();
        }
        if (choice < 93U)
        {
            return Collect();
        }
        if (choice < 96U)
        {
            return RecreateObject();
        }
        const UInt64 advance = m_random.Below(40U);
        m_system.Clock.Advance(advance);
        m_model.Now += advance;
        return CheckCounts("advance time");
    }

private:
    [[nodiscard]] bool Mismatch(const char* const operation, const char* const detail, const UInt64 expected,
                                const UInt64 actual) const noexcept
    {
        static_cast<void>(
            std::fprintf(stderr, "MODEL MISMATCH seed=%llu step=%zu operation=%s: %s expected=%llu actual=%llu\n",
                         static_cast<unsigned long long>(m_seed), m_step, operation, detail,
                         static_cast<unsigned long long>(expected), static_cast<unsigned long long>(actual)));
        return false;
    }

    [[nodiscard]] bool SameError(const char* const operation, const CapabilityError expected,
                                 const CapabilityError actual) const noexcept
    {
        if (expected == actual)
        {
            return true;
        }
        return Mismatch(operation, "result", static_cast<UInt64>(expected), static_cast<UInt64>(actual));
    }

    [[nodiscard]] bool CheckCounts(const char* const operation) noexcept
    {
        for (Size space{0U}; space < SpaceCount; ++space)
        {
            if (m_system.Spaces[space].GetActiveCount() != m_model.Active(space))
            {
                return Mismatch(operation, "active capabilities", m_model.Active(space),
                                m_system.Spaces[space].GetActiveCount());
            }
        }
        if (m_system.Registry.GetActiveCount() != m_model.RegisteredAlive())
        {
            return Mismatch(operation, "active objects", m_model.RegisteredAlive(), m_system.Registry.GetActiveCount());
        }
        return true;
    }

    // Records an issued handle after checking that it names the slot and
    // generation the model predicted.
    [[nodiscard]] bool AcceptIssued(const char* const operation, const Size space, const Size slot,
                                    const CapabilityHandle handle) noexcept
    {
        const UInt64 selector = handle.GetSelector();
        if ((selector & 0xFFU) != slot)
        {
            return Mismatch(operation, "issued slot", slot, selector & 0xFFU);
        }
        if ((selector >> 8U) != m_model.Slot(space, slot).Generation)
        {
            return Mismatch(operation, "issued generation", m_model.Slot(space, slot).Generation, selector >> 8U);
        }
        if (m_issuedCount < IssuedCapacity)
        {
            m_issued[m_issuedCount++] = IssuedHandle{space, handle};
        }
        else
        {
            m_issued[m_random.Below(IssuedCapacity)] = IssuedHandle{space, handle};
        }
        return true;
    }

    // Chooses a handle: usually an issued one (possibly stale), sometimes a
    // forged, zero, or cross-space handle.
    [[nodiscard]] IssuedHandle PickHandle(const Size space, bool& forged) noexcept
    {
        forged = false;
        if ((m_issuedCount == 0U) || m_random.Chance(1U, 30U))
        {
            return IssuedHandle{space, CapabilityHandle{}};
        }
        IssuedHandle handle = m_issued[m_random.Below(m_issuedCount)];
        if (m_random.Chance(1U, 25U))
        {
            forged = true;
            handle.Handle = CapabilityHandle{handle.Handle.GetSelector(),
                                             handle.Handle.GetAuthenticator() ^ (UInt64{1U} << m_random.Below(64U))};
        }
        return handle;
    }

    [[nodiscard]] Size PickSpace() noexcept
    {
        return static_cast<Size>(m_random.Below(SpaceCount));
    }

    [[nodiscard]] UInt16 PickRights(const UInt16 base) noexcept
    {
        const UInt64 mode = m_random.Below(10U);
        if (mode == 0U)
        {
            return static_cast<UInt16>(base | (UInt16{1U} << (14U + m_random.Below(2U))));
        }
        if (mode < 3U)
        {
            return base;
        }
        if (mode < 8U)
        {
            return static_cast<UInt16>(base & m_random.Next());
        }
        return static_cast<UInt16>(m_random.Next() & KnownRights);
    }

    [[nodiscard]] UInt64 PickDeadline(const UInt64 base) noexcept
    {
        const UInt64 mode = m_random.Below(10U);
        if (mode < 3U)
        {
            return InfiniteTicks;
        }
        if (mode < 5U)
        {
            return base;
        }
        if (mode == 5U)
        {
            return (base == InfiniteTicks) ? m_model.Now + 1U : base + 1U;
        }
        if (mode == 6U)
        {
            return m_model.Now - m_random.Below(3U);
        }
        const UInt64 delta = 1U + m_random.Below(300U);
        if ((base != InfiniteTicks) && (base > m_model.Now) && m_random.Chance(1U, 2U))
        {
            return m_model.Now + ((base - m_model.Now) * m_random.Below(100U)) / 100U;
        }
        return m_model.Now + delta;
    }

    [[nodiscard]] UInt64 DeepestDomain() const noexcept
    {
        Size deepest{0U};
        for (Size index{1U}; index < m_model.DomainCount; ++index)
        {
            if (m_model.Domains[index].Depth > m_model.Domains[deepest].Depth)
            {
                deepest = index;
            }
        }
        return deepest;
    }

    [[nodiscard]] bool RegisterObject(const Size index) noexcept
    {
        const auto created = m_system.Issuer.CreateObject(m_system.Objects[index]);
        if (!created.HasValue())
        {
            return Mismatch("create object", "registration", 1U, 0U);
        }
        ObjectState& object = m_model.Objects[index];
        object.Registered = true;
        object.Quiescing = false;
        object.Identity = created.GetValue();
        object.Leases = 0U;
        return CheckCounts("create object");
    }

    // RevocationRegistry: the lowest free slot, whose generation advanced
    // each time a domain in it was destroyed.
    [[nodiscard]] bool CreateDomain(const bool root, const UInt64 parentChoice) noexcept
    {
        if (m_model.DomainCount >= DomainHistory)
        {
            return true;
        }
        const bool haveParent = !root && (parentChoice < m_model.DomainCount);
        RevocationError expected{RevocationError::Ok};
        if (haveParent)
        {
            const DomainState& parent = m_model.Domains[parentChoice];
            if (!parent.Live)
            {
                expected = RevocationError::InvalidDomain;
            }
            else if (!m_model.LineageCurrent(parentChoice))
            {
                expected = RevocationError::Revoked;
            }
            else if (parent.Depth >= DomainDepthLimit)
            {
                expected = RevocationError::DepthExceeded;
            }
        }
        else if (!root)
        {
            expected = RevocationError::InvalidDomain;
        }
        const Size slot = FreeDomainSlot();
        if ((expected == RevocationError::Ok) && (slot == DomainCapacity))
        {
            expected = RevocationError::DomainExhausted;
        }

        const auto created =
            root ? m_system.Revocations.CreateRoot()
                 : m_system.Revocations.CreateChild(haveParent ? m_model.Domains[parentChoice].Identity
                                                               : RevocationDomainId{0xDEAD'0000'0001ULL});
        const RevocationError actual = created.HasValue() ? RevocationError::Ok : created.GetError();
        if (actual != expected)
        {
            return Mismatch("create domain", "result", static_cast<UInt64>(expected), static_cast<UInt64>(actual));
        }
        if (actual != RevocationError::Ok)
        {
            return true;
        }
        return RecordDomain("create domain", slot, created.GetValue(),
                            haveParent ? static_cast<Size>(parentChoice) : NoIndex);
    }

    [[nodiscard]] Size FreeDomainSlot() const noexcept
    {
        Size slot{0U};
        while ((slot < DomainCapacity) && (m_model.DomainSlots[slot] != NoIndex))
        {
            ++slot;
        }
        return slot;
    }

    [[nodiscard]] RevocationDomainId ExpectedDomainIdentity(const Size slot) const noexcept
    {
        const UInt16 generation = (m_model.SlotGenerations[slot] == 0U) ? UInt16{1U} : m_model.SlotGenerations[slot];
        return RevocationDomainId{(m_registryBits << 32U) | (UInt64{generation} << 16U) | (slot + 1U)};
    }

    // The registry issues the lowest free slot under its current generation.
    [[nodiscard]] bool RecordDomain(const char* const operation, const Size slot, const RevocationDomainId created,
                                    const Size parent) noexcept
    {
        const UInt64 identity = created.GetValue();
        const UInt16 generation = (m_model.SlotGenerations[slot] == 0U) ? UInt16{1U} : m_model.SlotGenerations[slot];
        m_registryBits = (m_model.DomainCount == 0U) ? (identity >> 32U) : m_registryBits;
        if (((identity & 0xFFFFU) != (slot + 1U)) || (((identity >> 16U) & 0xFFFFU) != generation) ||
            ((identity >> 32U) != m_registryBits))
        {
            return Mismatch(operation, "slot, generation, and registry of the identifier",
                            (m_registryBits << 32U) | (UInt64{generation} << 16U) | (slot + 1U), identity);
        }
        m_model.SlotGenerations[slot] = generation;
        m_model.DomainSlots[slot] = m_model.DomainCount;
        DomainState& domain = m_model.Domains[m_model.DomainCount++];
        domain.Identity = created;
        domain.Parent = parent;
        domain.Epoch = 1U;
        domain.ParentEpochAtCreation = (parent != NoIndex) ? m_model.Domains[parent].Epoch : 0U;
        domain.Depth = (parent != NoIndex) ? static_cast<UInt8>(m_model.Domains[parent].Depth + 1U) : 0U;
        domain.Live = true;
        return true;
    }

    // Mostly a live domain, else a destroyed or a malformed identifier.
    // Destroying one ends its whole subtree and frees their slots.
    [[nodiscard]] bool DestroyDomain() noexcept
    {
        Size domain{NoIndex};
        for (Size attempt{0U}; (attempt < 4U) && (m_model.DomainCount != 0U); ++attempt)
        {
            domain = static_cast<Size>(m_random.Below(m_model.DomainCount));
            attempt = (m_model.Domains[domain].Live || m_random.Chance(1U, 5U)) ? 4U : attempt;
        }
        const bool malformed = (domain == NoIndex) || m_random.Chance(1U, 20U);
        const RevocationDomainId identity =
            malformed ? RevocationDomainId{(m_registryBits << 32U) | 0x0001'0041U} : m_model.Domains[domain].Identity;
        const RevocationError expected =
            (!malformed && m_model.Domains[domain].Live) ? RevocationError::Ok : RevocationError::InvalidDomain;
        const RevocationError actual = m_system.Revocations.Destroy(identity);
        if (actual != expected)
        {
            return Mismatch("destroy domain", "result", static_cast<UInt64>(expected), static_cast<UInt64>(actual));
        }
        for (Size index{0U}; (actual == RevocationError::Ok) && (index < m_model.DomainCount); ++index)
        {
            DomainState& ended = m_model.Domains[index];
            if (!ended.Live || !m_model.InSubtree(index, domain))
            {
                continue;
            }
            const UInt64 value = ended.Identity.GetValue();
            const Size slot = static_cast<Size>((value & 0xFFFFU) - 1U);
            ended.Live = false;
            m_model.DomainSlots[slot] = NoIndex;
            m_model.SlotGenerations[slot] = static_cast<UInt16>(((value >> 16U) & 0xFFFFU) + 1U);
        }
        const Size live = m_model.LiveDomains();
        Size occupied{0U};
        for (const Size slot : m_model.DomainSlots)
        {
            occupied += (slot != NoIndex) ? 1U : 0U;
        }
        if (occupied != live)
        {
            return Mismatch("destroy domain", "model slot bookkeeping", live, occupied);
        }
        return CheckCounts("destroy domain");
    }

    [[nodiscard]] bool Mint() noexcept
    {
        const Size space = PickSpace();
        const Size objectIndex = static_cast<Size>(m_random.Below(ObjectCount));
        const ObjectState& object = m_model.Objects[objectIndex];
        const UInt16 rightsBits = PickRights(KnownRights);
        const UInt64 deadline = PickDeadline(InfiniteTicks);
        const UInt64 sealed = m_random.Next();
        const UInt64 domainChoice = m_random.Below(m_model.DomainCount + 2U);
        RevocationDomainId domainIdentity{};
        Size domain{NoIndex};
        if (domainChoice < m_model.DomainCount)
        {
            domain = static_cast<Size>(domainChoice);
            domainIdentity = m_model.Domains[domain].Identity;
        }
        else if (domainChoice == m_model.DomainCount)
        {
            domainIdentity = RevocationDomainId{m_model.Domains[0U].Identity.GetValue() ^ 0x4000U};
        }

        CapabilityError expected{CapabilityError::Ok};
        if (((rightsBits & ~KnownRights) != 0U) || !domainIdentity.IsValid())
        {
            expected = CapabilityError::InvalidArgument;
        }
        else if (m_model.Expired(deadline))
        {
            expected = CapabilityError::Expired;
        }
        else if (!object.Registered || object.Quiescing)
        {
            expected = CapabilityError::ObjectNotFound;
        }
        else if ((domain == NoIndex) || !m_model.Domains[domain].Live)
        {
            expected = CapabilityError::InvalidArgument;
        }
        else if (!m_model.LineageCurrent(domain))
        {
            expected = CapabilityError::Revoked;
        }
        const Size slot = m_model.FirstFree(space);
        if ((expected == CapabilityError::Ok) && (slot == NoIndex))
        {
            expected = CapabilityError::SpaceExhausted;
        }

        const ObjectId identity = object.Registered ? object.Identity : ObjectId{0x0F0F'0001ULL};
        const auto minted = m_system.Issuer.Mint(m_system.Spaces[space], identity, RightsFrom(rightsBits),
                                                 domainIdentity, MonotonicTime{deadline}, sealed);
        const CapabilityError actual = minted.HasValue() ? CapabilityError::Ok : minted.GetError();
        if (!SameError("mint", expected, actual))
        {
            return false;
        }
        if (actual == CapabilityError::Ok)
        {
            SlotState value{};
            value.Object = object.Identity;
            value.Type = object.Type;
            value.Rights = rightsBits;
            value.Domain = domain;
            value.Epoch = m_model.Domains[domain].Epoch;
            value.Deadline = deadline;
            value.ChargeOwner = object.ChargeOwner;
            value.Sealed = sealed;
            m_model.Occupy(space, slot, value);
            if (!AcceptIssued("mint", space, slot, minted.GetValue()))
            {
                return false;
            }
        }
        return CheckCounts("mint");
    }

    [[nodiscard]] bool DeriveOrDuplicate(const bool derive) noexcept
    {
        const char* const operation = derive ? "derive" : "duplicate";
        const Size space = PickSpace();
        bool forged{false};
        const IssuedHandle source = PickHandle(space, forged);
        const Size sourceSlot = m_model.Resolve(space, source, forged);
        const UInt16 baseRights = (sourceSlot == NoIndex) ? KnownRights : m_model.Slot(space, sourceSlot).Rights;
        const UInt64 baseDeadline = (sourceSlot == NoIndex) ? InfiniteTicks : m_model.Slot(space, sourceSlot).Deadline;
        const UInt16 rightsBits = PickRights(baseRights);
        const UInt64 deadline = PickDeadline(baseDeadline);
        const bool child = derive && (m_model.DomainCount < DomainHistory) && m_random.Chance(1U, 4U);

        CapabilityError expected{CapabilityError::Ok};
        Size index{NoIndex};
        if ((rightsBits & ~KnownRights) != 0U)
        {
            expected = CapabilityError::InvalidArgument;
        }
        else
        {
            const UInt16 required = RightFor(derive ? CapabilityOperation::Derive : CapabilityOperation::Duplicate);
            expected = m_model.Usable(space, source, forged, required, index);
        }
        if (expected == CapabilityError::Ok)
        {
            const SlotState& slot = m_model.Slot(space, index);
            if (((rightsBits & slot.Rights) != rightsBits) || (!child && ((rightsBits & RevokeRight) != 0U)))
            {
                expected = CapabilityError::RightsAmplification;
            }
            else if (m_model.Expired(deadline))
            {
                expected = CapabilityError::Expired;
            }
            else if ((slot.Deadline != InfiniteTicks) && ((deadline == InfiniteTicks) || (deadline > slot.Deadline)))
            {
                expected = CapabilityError::LeaseExtension;
            }
            else if (derive && !child && (rightsBits == slot.Rights))
            {
                expected = CapabilityError::NoAttenuation;
            }
        }
        const Size destination = m_model.FirstFree(space);
        if ((expected == CapabilityError::Ok) && (destination == NoIndex))
        {
            expected = CapabilityError::SpaceExhausted;
        }
        // A branch's domain is a child of its source's, which Usable proved
        // live and current; depth or registry exhaustion fails the derive.
        const Size domainSlot = FreeDomainSlot();
        if ((expected == CapabilityError::Ok) && child &&
            ((m_model.Domains[m_model.Slot(space, index).Domain].Depth >= DomainDepthLimit) ||
             (domainSlot == DomainCapacity)))
        {
            expected = CapabilityError::RevocationDomainExhausted;
        }

        const auto created =
            derive ? m_system.Spaces[space].Derive(source.Handle, RightsFrom(rightsBits), MonotonicTime{deadline},
                                                   child ? DerivedRevocationDomain::Child
                                                         : DerivedRevocationDomain::Source)
                   : m_system.Spaces[space].Duplicate(source.Handle, RightsFrom(rightsBits), MonotonicTime{deadline});
        const CapabilityError actual = created.HasValue() ? CapabilityError::Ok : created.GetError();
        if (!SameError(operation, expected, actual))
        {
            return false;
        }
        if (actual == CapabilityError::Ok)
        {
            SlotState value = m_model.Slot(space, index);
            value.Rights = rightsBits;
            value.Deadline = deadline;
            if (child)
            {
                // Query needs Inspect; without it the identifier is the one
                // the registry's allocation rule names.
                const auto descriptor = m_system.Spaces[space].Query(created.GetValue());
                const RevocationDomainId branch =
                    descriptor.HasValue() ? descriptor.GetValue().RevocationDomain : ExpectedDomainIdentity(domainSlot);
                if (!RecordDomain(operation, domainSlot, branch, value.Domain))
                {
                    return false;
                }
                value.Domain = m_model.DomainCount - 1U;
                value.Epoch = 1U;
            }
            m_model.Occupy(space, destination, value);
            if (!AcceptIssued(operation, space, destination, created.GetValue()))
            {
                return false;
            }
        }
        return CheckCounts(operation);
    }

    [[nodiscard]] bool Query() noexcept
    {
        const Size space = PickSpace();
        bool forged{false};
        const IssuedHandle handle = PickHandle(space, forged);
        Size index{NoIndex};
        const CapabilityError expected =
            m_model.Usable(space, handle, forged, RightFor(CapabilityOperation::Inspect), index);
        const auto queried = m_system.Spaces[space].Query(handle.Handle);
        const CapabilityError actual = queried.HasValue() ? CapabilityError::Ok : queried.GetError();
        if (!SameError("query", expected, actual))
        {
            return false;
        }
        if (actual != CapabilityError::Ok)
        {
            return true;
        }
        const CapabilityDescriptor& descriptor = queried.GetValue();
        const SlotState& slot = m_model.Slot(space, index);
        if (descriptor.Object.GetValue() != slot.Object.GetValue())
        {
            return Mismatch("query", "object", slot.Object.GetValue(), descriptor.Object.GetValue());
        }
        if (descriptor.Type != slot.Type)
        {
            return Mismatch("query", "type", static_cast<UInt64>(slot.Type), static_cast<UInt64>(descriptor.Type));
        }
        if (descriptor.Rights.GetRawValue() != slot.Rights)
        {
            return Mismatch("query", "rights", slot.Rights, descriptor.Rights.GetRawValue());
        }
        if (descriptor.LeaseDeadline.GetTicks() != slot.Deadline)
        {
            return Mismatch("query", "deadline", slot.Deadline, descriptor.LeaseDeadline.GetTicks());
        }
        if (descriptor.RevocationDomain.GetValue() != m_model.Domains[slot.Domain].Identity.GetValue())
        {
            return Mismatch("query", "revocation domain", m_model.Domains[slot.Domain].Identity.GetValue(),
                            descriptor.RevocationDomain.GetValue());
        }
        if (descriptor.ChargeOwner.GetValue() != slot.ChargeOwner)
        {
            return Mismatch("query", "charge owner", slot.ChargeOwner, descriptor.ChargeOwner.GetValue());
        }
        if (descriptor.SealedObjectSelector != slot.Sealed)
        {
            return Mismatch("query", "sealed selector", slot.Sealed, descriptor.SealedObjectSelector);
        }
        return true;
    }

    [[nodiscard]] bool Revoke() noexcept
    {
        const Size space = PickSpace();
        bool forged{false};
        const IssuedHandle handle = PickHandle(space, forged);
        Size index{NoIndex};
        const CapabilityError expected =
            m_model.Usable(space, handle, forged, RightFor(CapabilityOperation::Revoke), index);
        const CapabilityError actual = m_system.Spaces[space].Revoke(handle.Handle);
        if (!SameError("revoke", expected, actual))
        {
            return false;
        }
        if (actual == CapabilityError::Ok)
        {
            ++m_model.Domains[m_model.Slot(space, index).Domain].Epoch;
        }
        return CheckCounts("revoke");
    }

    [[nodiscard]] bool Close() noexcept
    {
        const Size space = PickSpace();
        bool forged{false};
        const IssuedHandle handle = PickHandle(space, forged);
        const Size index = m_model.Resolve(space, handle, forged);
        const CapabilityError expected = (index == NoIndex) ? CapabilityError::InvalidHandle : CapabilityError::Ok;
        const CapabilityError actual = m_system.Spaces[space].Close(handle.Handle);
        if (!SameError("close", expected, actual))
        {
            return false;
        }
        if (actual == CapabilityError::Ok)
        {
            m_model.Free(space, index);
        }
        return CheckCounts("close");
    }

    [[nodiscard]] bool Lookup() noexcept
    {
        const Size space = PickSpace();
        bool forged{false};
        const IssuedHandle handle = PickHandle(space, forged);
        const CapabilityOperation operation = static_cast<CapabilityOperation>(m_random.Below(13U));
        const UInt64 typeChoice = m_random.Below(8U);
        const ObjectType type =
            (typeChoice == 0U)
                ? ObjectType::Invalid
                : ((typeChoice < 4U) ? ObjectType::Endpoint
                                     : ((typeChoice < 6U) ? ObjectType::Notification : ObjectType::MemoryObject));
        Size index{NoIndex};
        CapabilityError expected{CapabilityError::InvalidArgument};
        if (type != ObjectType::Invalid)
        {
            expected = m_model.Usable(space, handle, forged, RightFor(operation), index);
            if ((expected == CapabilityError::Ok) && (m_model.Slot(space, index).Type != type))
            {
                expected = CapabilityError::TypeMismatch;
            }
        }
        CapabilityLookupResult result = m_system.Spaces[space].Lookup(handle.Handle, type, operation);
        const CapabilityError actual = result.HasValue() ? CapabilityError::Ok : result.GetError();
        if (!SameError("lookup", expected, actual))
        {
            return false;
        }
        if ((actual == CapabilityError::Ok) && m_random.Chance(1U, 3U))
        {
            for (Size slot{0U}; slot < HeldLeaseCapacity; ++slot)
            {
                if (m_system.Held[slot] != nullptr)
                {
                    continue;
                }
                m_system.Held[slot] = new (m_system.HeldStorage[slot])
                    CapabilityLookupResult{static_cast<CapabilityLookupResult&&>(result)};
                const Size object = ObjectIndexOf(m_model.Slot(space, index).Object);
                m_system.HeldObject[slot] = object;
                ++m_model.Objects[object].Leases;
                break;
            }
        }
        return true;
    }

    [[nodiscard]] bool ReleaseLease() noexcept
    {
        const Size slot = static_cast<Size>(m_random.Below(HeldLeaseCapacity));
        if (m_system.Held[slot] == nullptr)
        {
            return true;
        }
        m_system.Held[slot]->~CapabilityLookupResult();
        m_system.Held[slot] = nullptr;
        --m_model.Objects[m_system.HeldObject[slot]].Leases;
        return CheckCounts("release lease");
    }

    [[nodiscard]] Size ObjectIndexOf(const ObjectId identity) const noexcept
    {
        for (Size index{0U}; index < ObjectCount; ++index)
        {
            if (m_model.Objects[index].Identity.GetValue() == identity.GetValue())
            {
                return index;
            }
        }
        return 0U;
    }

    [[nodiscard]] bool DestroyObject() noexcept
    {
        const Size index = static_cast<Size>(m_random.Below(ObjectCount));
        ObjectState& object = m_model.Objects[index];
        const ObjectId identity = object.Registered ? object.Identity : ObjectId{0x0F0F'0001ULL};
        const ObjectRegistryError expected =
            (object.Registered && !object.Quiescing) ? ObjectRegistryError::Ok : ObjectRegistryError::InvalidObject;
        const ObjectRegistryError actual = m_system.Issuer.DestroyObject(identity);
        if (actual != expected)
        {
            return Mismatch("destroy object", "result", static_cast<UInt64>(expected), static_cast<UInt64>(actual));
        }
        if (actual == ObjectRegistryError::Ok)
        {
            object.Quiescing = true;
        }
        return CheckCounts("destroy object");
    }

    [[nodiscard]] bool Collect() noexcept
    {
        Size expected{NoIndex};
        UInt64 lowestSlot{~UInt64{0U}};
        for (Size index{0U}; index < ObjectCount; ++index)
        {
            const ObjectState& object = m_model.Objects[index];
            const UInt64 registrySlot = object.Identity.GetValue() & 0xFFFFU;
            if (object.Registered && object.Quiescing && (object.Leases == 0U) && (registrySlot < lowestSlot))
            {
                lowestSlot = registrySlot;
                expected = index;
            }
        }
        const auto collected = m_system.Issuer.CollectDestroyedObject();
        if (expected == NoIndex)
        {
            if (collected.HasValue())
            {
                return Mismatch("collect", "unexpected record", 0U, collected.GetValue().Object.GetValue());
            }
            return true;
        }
        if (!collected.HasValue())
        {
            return Mismatch("collect", "missing record", m_model.Objects[expected].Identity.GetValue(), 0U);
        }
        if (collected.GetValue().Object.GetValue() != m_model.Objects[expected].Identity.GetValue())
        {
            return Mismatch("collect", "collected object", m_model.Objects[expected].Identity.GetValue(),
                            collected.GetValue().Object.GetValue());
        }
        m_model.Objects[expected].Registered = false;
        m_model.Objects[expected].Quiescing = false;
        return CheckCounts("collect");
    }

    [[nodiscard]] bool RecreateObject() noexcept
    {
        const Size index = static_cast<Size>(m_random.Below(ObjectCount));
        if (m_model.Objects[index].Registered)
        {
            return true;
        }
        return RegisterObject(index);
    }

    [[nodiscard]] bool Transfer() noexcept
    {
        const Size from = PickSpace();
        const Size to = m_random.Chance(1U, 4U) ? from : (1U - from);
        const Size count = static_cast<Size>(m_random.Below(6U));
        CapabilityTransferRequest requests[5U]{};
        bool forged[5U]{};
        IssuedHandle sources[5U]{};
        for (Size index{0U}; index < count; ++index)
        {
            sources[index] = PickHandle(from, forged[index]);
            if ((index > 0U) && m_random.Chance(1U, 10U))
            {
                sources[index] = sources[index - 1U];
                forged[index] = forged[index - 1U];
            }
            const Size slot = m_model.Resolve(from, sources[index], forged[index]);
            const UInt16 baseRights = (slot == NoIndex) ? KnownRights : m_model.Slot(from, slot).Rights;
            const UInt64 baseDeadline = (slot == NoIndex) ? InfiniteTicks : m_model.Slot(from, slot).Deadline;
            const ObjectType baseType = (slot == NoIndex) ? ObjectType::Endpoint : m_model.Slot(from, slot).Type;
            requests[index].Source = sources[index].Handle;
            requests[index].RequestedRights = RightsFrom(PickRights(baseRights));
            requests[index].RequestedDeadline = MonotonicTime{PickDeadline(baseDeadline)};
            requests[index].ExpectedType = m_random.Chance(1U, 12U)
                                               ? (m_random.Chance(1U, 2U) ? ObjectType::Invalid : ObjectType::Thread)
                                               : baseType;
            const UInt64 disposition = m_random.Below(20U);
            requests[index].Disposition =
                (disposition == 0U)
                    ? CapabilityTransferDisposition::Invalid
                    : ((disposition < 10U) ? CapabilityTransferDisposition::Move : CapabilityTransferDisposition::Copy);
            requests[index].Reserved = m_random.Chance(1U, 40U) ? 1U : 0U;
        }

        // Prediction mirrors PrepareTransferPlanLocked and the slot selection.
        CapabilityError expected{CapabilityError::Ok};
        Size sourceSlots[5U]{};
        if (count > CapabilityTransferBatch::MaximumTransfers)
        {
            expected = CapabilityError::InvalidArgument;
        }
        for (Size index{0U}; (expected == CapabilityError::Ok) && (index < count); ++index)
        {
            const CapabilityTransferRequest& request = requests[index];
            const UInt16 rights = request.RequestedRights.GetRawValue();
            if ((request.Reserved != 0U) || ((rights & ~KnownRights) != 0U) || (rights == 0U) ||
                (request.ExpectedType == ObjectType::Invalid) ||
                (request.Disposition == CapabilityTransferDisposition::Invalid))
            {
                expected = CapabilityError::InvalidArgument;
                break;
            }
            const UInt16 required =
                RightFor(request.Disposition == CapabilityTransferDisposition::Move ? CapabilityOperation::Transfer
                                                                                    : CapabilityOperation::Duplicate);
            Size slot{NoIndex};
            expected = m_model.Usable(from, sources[index], forged[index], required, slot);
            if (expected != CapabilityError::Ok)
            {
                break;
            }
            const SlotState& source = m_model.Slot(from, slot);
            const UInt64 deadline = request.RequestedDeadline.GetTicks();
            if (source.Type != request.ExpectedType)
            {
                expected = CapabilityError::TypeMismatch;
            }
            else if (((rights & source.Rights) != rights) ||
                     ((request.Disposition == CapabilityTransferDisposition::Copy) && ((rights & RevokeRight) != 0U)))
            {
                expected = CapabilityError::RightsAmplification;
            }
            else if (m_model.Expired(deadline))
            {
                expected = CapabilityError::Expired;
            }
            else if ((source.Deadline != InfiniteTicks) &&
                     ((deadline == InfiniteTicks) || (deadline > source.Deadline)))
            {
                expected = CapabilityError::LeaseExtension;
            }
            for (Size previous{0U}; (expected == CapabilityError::Ok) && (previous < index); ++previous)
            {
                if (sourceSlots[previous] == slot)
                {
                    expected = CapabilityError::InvalidArgument;
                }
            }
            sourceSlots[index] = slot;
        }
        Prediction prediction{expected, to, {}, 0U};
        if ((expected == CapabilityError::Ok) && (count > 0U))
        {
            for (Size slot{0U}; (slot < SlotCount) && (prediction.SlotCount < count); ++slot)
            {
                bool available = !m_model.Slot(to, slot).Occupied;
                if ((from == to) && !available)
                {
                    for (Size source{0U}; source < count; ++source)
                    {
                        if ((requests[source].Disposition == CapabilityTransferDisposition::Move) &&
                            (sourceSlots[source] == slot))
                        {
                            available = true;
                        }
                    }
                }
                if (available)
                {
                    prediction.Slots[prediction.SlotCount++] = slot;
                }
            }
            if (prediction.SlotCount != count)
            {
                prediction.Error = CapabilityError::SpaceExhausted;
            }
        }

        const auto transferred = m_system.Spaces[from].TransferBatchTo(
            m_system.Spaces[to], CapabilityTransferRequestView{.Data = requests, .Count = count});
        const CapabilityError actual = transferred.HasValue() ? CapabilityError::Ok : transferred.GetError();
        if (!SameError("transfer", prediction.Error, actual))
        {
            return false;
        }
        if (actual == CapabilityError::Ok)
        {
            SlotState values[5U]{};
            for (Size index{0U}; index < count; ++index)
            {
                values[index] = m_model.Slot(from, sourceSlots[index]);
                values[index].Rights = requests[index].RequestedRights.GetRawValue();
                values[index].Deadline = requests[index].RequestedDeadline.GetTicks();
            }
            for (Size index{0U}; index < count; ++index)
            {
                if (requests[index].Disposition == CapabilityTransferDisposition::Move)
                {
                    m_model.Free(from, sourceSlots[index]);
                }
            }
            for (Size index{0U}; index < count; ++index)
            {
                m_model.Occupy(to, prediction.Slots[index], values[index]);
                if (!AcceptIssued("transfer", to, prediction.Slots[index],
                                  transferred.GetValue().DestinationHandles[index]))
                {
                    return false;
                }
            }
        }
        return CheckCounts("transfer");
    }

    System& m_system;
    Model& m_model;
    Random m_random;
    UInt64 m_seed;
    Size m_step{0U};
    IssuedHandle m_issued[IssuedCapacity]{};
    Size m_issuedCount{0U};
    UInt64 m_registryBits{0U};
};

// The fixtures are tens of kilobytes; they live in static storage and are
// placement-constructed per seed because tests may not allocate.
alignas(System) unsigned char g_systemStorage[sizeof(System)];
alignas(Model) unsigned char g_modelStorage[sizeof(Model)];
alignas(Driver) unsigned char g_driverStorage[sizeof(Driver)];

void RunSeed(TestContext& context, const UInt64 seed) noexcept
{
    System* const system = new (g_systemStorage) System{};
    Model* const model = new (g_modelStorage) Model{};
    Driver* const driver = new (g_driverStorage) Driver{*system, *model, seed};
    bool matched = driver->Initialize();
    for (Size step{0U}; matched && (step < StepsPerSeed); ++step)
    {
        matched = driver->Step(step);
    }
    if (!matched)
    {
        static_cast<void>(
            std::fprintf(stderr, "  reproduce with ASTERIA_TEST_SEED=%llu\n", static_cast<unsigned long long>(seed)));
    }
    ASTERIA_EXPECT(context, "capability authority matches its reference model for one seed", matched);
    driver->~Driver();
    model->~Model();
    system->~System();
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"CapabilitySpaceModelTests"};
    const Asteria::TestSupport::SeedRange seeds = Asteria::TestSupport::GetSeedRange(1U, 64U);
    for (Asteria::Foundation::UInt64 seed = seeds.First; seed < seeds.First + seeds.Count; ++seed)
    {
        Asteria::Test::RunSeed(runner, seed);
    }
    return runner.Finish();
}
