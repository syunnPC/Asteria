// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Memory/FrameObjectPool.hpp"

// Target images define placement new once (Kernel/Boot/KernelEntry.cpp).
#if defined(ASTERIA_TARGET_IA64) || defined(ASTERIA_TARGET_AMD64)
void* operator new(Asteria::Foundation::Size, void*) noexcept;
#else
#include <new>
#endif

namespace Asteria::Kernel::Memory
{
namespace
{
using Object::MakeObjectPoolFailure;
using Object::ObjectPoolError;
using Object::ObjectPoolFailure;
} // Namespace

bool FrameObjectPool::AttachSlab(void* const slab) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if ((m_slots != nullptr) || (slab == nullptr) ||
        ((reinterpret_cast<Foundation::UIntPointer>(slab) % SlabAlignment) != 0U))
    {
        return false;
    }
    auto* const slots = static_cast<Slot*>(slab);
    for (Foundation::Size index{0U}; index < Capacity; ++index)
    {
        ::new (static_cast<void*>(&slots[index])) Slot{};
        m_sets.AddFree(index);
    }
    m_slots = slots;
    return true;
}

bool FrameObjectPool::ConfigureScrubber(IFrameScrubber& scrubber) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_scrubber != nullptr)
    {
        return false;
    }
    m_scrubber = &scrubber;
    return true;
}

bool FrameObjectPool::PrimeCreationZeroProbe() noexcept
{
    m_creationZeroProbe = false;
    if (m_pages.GetAllocator().GetSnapshot().DirtyPages != 0U)
    {
        return true;
    }
    // Allocation and Release touch metadata only. Use ordinary allocator
    // transitions, including the unaligned ends of the old pool's range.
    for (Foundation::Size order{PhysicalPageAllocator::MaximumOrder + 1U}; order != 0U; --order)
    {
        for (Foundation::Size block{0U}; block < PhysicalPageAllocator::MaximumManagedPages; ++block)
        {
            const auto run = m_pages.GetAllocator().Allocate(Foundation::UInt64{1U} << (order - 1U), 0U, 0U, true);
            if (!run.HasValue())
            {
                break;
            }
            if (m_pages.GetAllocator().Release(run.GetValue()) != PhysicalPageAllocatorError::None)
            {
                return false;
            }
        }
    }
    return true;
}

// Charge and allocate under the lock, zero outside every pool and AddressSpace lock, then register
// and mint under the lock.
Foundation::Result<FrameCreation, ObjectPoolFailure> FrameObjectPool::Create(const FrameCreateRequest& request) noexcept
{
    using ResultType = Foundation::Result<FrameCreation, ObjectPoolFailure>;
    Slot* slot{nullptr};
    {
        Platform::Synchronization::SpinLockGuard guard{m_synchronization};
        const ObjectPoolFailure reserved = ReserveLocked(request, slot);
        if (reserved.Error != ObjectPoolError::None)
        {
            return ResultType::Failure(reserved);
        }
        if (slot->Region.IsValid())
        {
            Object::CapabilityHandle minted{};
            const ObjectPoolFailure published =
                PublishLocked(*slot, *request.Space, request.Authority, *slot->Domain, minted);
            if (published.Error != ObjectPoolError::None)
            {
                return ResultType::Failure(published);
            }
            SetStateLocked(*slot, SlotState::Active);
            ++m_created;
            return ResultType::Success(
                FrameCreation{.Frame = slot->Identity, .Capability = minted, .PhysicalStart = slot->PhysicalStart});
        }
    }
    const FrameScrubError zero = ZeroSlot(*slot);
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (zero != FrameScrubError::None)
    {
        m_zeroDeferrals += (zero == FrameScrubError::Deferred) ? 1U : 0U;
        m_zeroFaults += (zero == FrameScrubError::Faulted) ? 1U : 0U;
        if (m_pages.Release(slot->PhysicalStart, slot->Pages.GetValue()) != FramePoolError::None)
        {
            return ResultType::Failure(FaultLocked(MakeObjectPoolFailure(ObjectPoolError::InvariantViolation)));
        }
        slot->Pages = PageCount{};
        Foundation::UInt8 alignment{0U};
        while ((Foundation::UInt64{1U} << alignment) < request.AlignmentPages)
        {
            ++alignment;
        }
        // Exactly one clean-only retry, with the charge and slot retained.
        const auto retry =
            m_pages.GetAllocator().Allocate(request.Pages.GetValue(), alignment, request.AddressLimitBits, true);
        if (!retry.HasValue())
        {
            const ObjectPoolFailure released = ReleaseSlotLocked(*slot, false);
            return ResultType::Failure((released.Error == ObjectPoolError::None)
                                           ? MakeObjectPoolFailure(ObjectPoolError::Busy)
                                           : FaultLocked(released));
        }
        slot->PhysicalStart = retry.GetValue().PhysicalStart;
        slot->Pages = request.Pages;
        for (auto& word : slot->DirtyMask)
        {
            word = 0U;
        }
    }
    if (m_state == FrameObjectPoolState::Faulted)
    {
        const ObjectPoolFailure released = ReleaseSlotLocked(*slot, true);
        return ResultType::Failure((released.Error == ObjectPoolError::None)
                                       ? MakeObjectPoolFailure(ObjectPoolError::Faulted)
                                       : FaultLocked(released));
    }
    Object::CapabilityHandle minted{};
    const ObjectPoolFailure published = PublishLocked(*slot, *request.Space, request.Authority, *slot->Domain, minted);
    if (published.Error != ObjectPoolError::None)
    {
        return ResultType::Failure(published);
    }
    SetStateLocked(*slot, SlotState::Active);
    ++m_created;
    return ResultType::Success(
        FrameCreation{.Frame = slot->Identity, .Capability = minted, .PhysicalStart = slot->PhysicalStart});
}

FrameScrubError FrameObjectPool::ZeroSlot(Slot& slot) noexcept
{
    if (slot.Region.IsValid())
    {
        return FrameScrubError::None;
    }
    for (Foundation::UInt64 page{0U}; page < slot.Pages.GetValue();)
    {
        if ((slot.DirtyMask[page / 64U] & (Foundation::UInt64{1U} << (page % 64U))) == 0U)
        {
            ++page;
            continue;
        }
        Foundation::UInt64 count{1U};
        while ((count < PhysicalPageAllocator::MaximumScrubPages) && ((page + count) < slot.Pages.GetValue()) &&
               ((slot.DirtyMask[(page + count) / 64U] & (Foundation::UInt64{1U} << ((page + count) % 64U))) != 0U))
        {
            ++count;
        }
        if (m_scrubber == nullptr)
        {
            return FrameScrubError::Deferred;
        }
        const FrameScrubError error =
            m_scrubber->Zero(PhysicalAddress{slot.PhysicalStart.GetValue() + page * MappingPageSizeBytes}, count);
        if (error != FrameScrubError::None)
        {
            return error;
        }
        for (Foundation::UInt64 offset{0U}; offset < count; ++offset)
        {
            slot.DirtyMask[(page + offset) / 64U] &= ~(Foundation::UInt64{1U} << ((page + offset) % 64U));
        }
        static_cast<void>(__atomic_fetch_add(&m_creationZeroedPages, count, __ATOMIC_RELAXED));
        page += count;
    }
    return FrameScrubError::None;
}

ObjectPoolFailure FrameObjectPool::ReserveLocked(const FrameCreateRequest& request, Slot*& output) noexcept
{
    if (m_state == FrameObjectPoolState::Faulted)
    {
        return (MakeObjectPoolFailure(ObjectPoolError::Faulted));
    }
    if ((request.Space == nullptr) || !request.Space->IsReady() || !request.Space->IsFromRegistry(m_objectRegistry))
    {
        return (MakeObjectPoolFailure(ObjectPoolError::InvalidConfiguration));
    }
    // A Frame of RAM is one frame-pool run; a device Frame is bounded by its region
    // (BindRegionLocked), as a framebuffer is larger than any run.
    if ((request.Pages.GetValue() == 0U) ||
        (!request.Region.IsValid() && (request.Pages.GetValue() > FramePool::MaximumRunPages)) ||
        ((request.AddressLimitBits != 0U) && (request.AddressLimitBits != 32U)) ||
        (request.AlignmentPages > FramePool::MaximumAlignmentPages) ||
        ((request.AlignmentPages & (request.AlignmentPages - 1U)) != 0U) ||
        (request.Region.IsValid() && (request.AlignmentPages > 1U)))
    {
        return (MakeObjectPoolFailure(ObjectPoolError::InvalidRequest));
    }
    Object::CapabilitySpace& space = *request.Space;

    const auto authority =
        space.Lookup(request.Authority, Object::ObjectType::ResourceDomain, Object::CapabilityOperation::Manage);
    if (!authority.HasValue())
    {
        return (MakeObjectPoolFailure(ObjectPoolError::CapabilityRejected, authority.GetError()));
    }
    Slot* const slot = FindFreeSlotLocked();
    if (slot == nullptr)
    {
        return (MakeObjectPoolFailure(ObjectPoolError::CapacityExceeded));
    }

    // The lookup checked the type, and only ResourceDomain instances report ObjectType:
    // ResourceDomain. The pin keeps the domain registered while the slot holds its charge.
    Resource::ResourceDomain& domain = static_cast<Resource::ResourceDomain&>(authority.GetValue().GetObject());
    if ((m_objectRegistry.PinObject(authority.GetValue().GetDescriptor().Object, Object::ObjectType::ResourceDomain,
                                    slot->DomainPin) != Object::ObjectPinError::Ok) ||
        (&slot->DomainPin.GetObject() != static_cast<Object::IKernelObject*>(&domain)))
    {
        slot->DomainPin.Reset();
        return (MakeObjectPoolFailure(ObjectPoolError::ResourceDomainRejected));
    }

    const bool device = request.Region.IsValid();
    if (device)
    {
        const ObjectPoolFailure bound = BindRegionLocked(*slot, space, request);
        if (bound.Error != ObjectPoolError::None)
        {
            slot->DomainPin.Reset();
            return (bound);
        }
    }
    const Resource::ResourceReservation charge = device ? GetDeviceReservation() : GetReservation(request.Pages);
    const Resource::ResourceDomainError charged = domain.TryReserve(charge);
    if (charged != Resource::ResourceDomainError::None)
    {
        slot->RegionPin.Reset();
        slot->Region = Device::DeviceRegionId{};
        slot->DomainPin.Reset();
        return (MakeObjectPoolFailure(ObjectPoolError::ChargeRejected, Object::CapabilityError::Ok, charged));
    }
    // A device Frame's pages are its region's; a RAM Frame takes a run.
    if (!device && m_creationZeroProbe && !PrimeCreationZeroProbe())
    {
        return FaultLocked(MakeObjectPoolFailure(ObjectPoolError::InvariantViolation));
    }
    Foundation::UInt8 alignment{0U};
    while ((Foundation::UInt64{1U} << alignment) < request.AlignmentPages)
    {
        ++alignment;
    }
    const auto run =
        device ? Foundation::Result<PhysicalPageRun, PhysicalPageAllocatorError>::Success(PhysicalPageRun{
                     .PhysicalStart = slot->PhysicalStart, .Pages = request.Pages.GetValue(), .DirtyMask = {}})
               : m_pages.GetAllocator().Allocate(request.Pages.GetValue(), alignment, request.AddressLimitBits, false);
    if (!run.HasValue())
    {
        const Resource::ResourceDomainError released = domain.Release(charge);
        slot->DomainPin.Reset();
        if (released != Resource::ResourceDomainError::None)
        {
            return (FaultLocked(
                MakeObjectPoolFailure(ObjectPoolError::InvariantViolation, Object::CapabilityError::Ok, released)));
        }
        return (MakeObjectPoolFailure(ObjectPoolError::CapacityExceeded));
    }

    slot->Domain = &domain;
    slot->Charge = charge;
    slot->PhysicalStart = run.GetValue().PhysicalStart;
    for (Foundation::Size index{0U}; index < PhysicalPageAllocator::DirtyWords; ++index)
    {
        slot->DirtyMask[index] = run.GetValue().DirtyMask[index];
    }
    slot->Pages = request.Pages;
    slot->CachePolicy = device ? slot->CachePolicy : MemoryCachePolicy::WriteBack;
    if (!device)
    {
        SetStateLocked(*slot, SlotState::Zeroing);
        if (m_sets.GetZeroingCount() > m_peakZeroing)
        {
            m_peakZeroing = m_sets.GetZeroingCount();
        }
    }
    output = slot;
    return MakeObjectPoolFailure(ObjectPoolError::None);
}

ObjectPoolFailure FrameObjectPool::PublishLocked(Slot& slot, Object::CapabilitySpace& space,
                                                 const Object::CapabilityHandle authority,
                                                 Resource::ResourceDomain& domain,
                                                 Object::CapabilityHandle& minted) noexcept
{
    CreatedFrame* const frame = ::new (static_cast<void*>(slot.Storage)) CreatedFrame{};
    slot.Instance = frame;
    if (frame->Initialize(domain.GetIdentifier(),
                          FrameDescriptor{
                              .PhysicalStart = slot.PhysicalStart,
                              .Pages = slot.Pages,
                              .CachePolicy = slot.CachePolicy,
                              .Kind = slot.Region.IsValid() ? FrameKind::Device : FrameKind::Ram,
                              .Reserved = {},
                          }) != FrameError::None)
    {
        const ObjectPoolFailure released = ReleaseSlotLocked(slot, true);
        return (released.Error != ObjectPoolError::None) ? FaultLocked(released)
                                                         : MakeObjectPoolFailure(ObjectPoolError::InvalidRequest);
    }

    const auto registered = m_issuer.CreateObject(*frame);
    if (!registered.HasValue())
    {
        const ObjectPoolFailure released = ReleaseSlotLocked(slot, true);
        return (released.Error != ObjectPoolError::None)
                   ? FaultLocked(released)
                   : MakeObjectPoolFailure(ObjectPoolError::RegistrationRejected, Object::CapabilityError::Ok,
                                           Resource::ResourceDomainError::None, registered.GetError());
    }
    slot.Identity = registered.GetValue();

    const auto capability =
        m_issuer.MintUnderAuthority(space, slot.Identity, CreatedRights, authority, Object::ObjectType::ResourceDomain,
                                    Object::CapabilityOperation::Manage, domain);
    if (!capability.HasValue())
    {
        return AbandonLocked(slot, capability.GetError());
    }
    minted = capability.GetValue();
    return MakeObjectPoolFailure(ObjectPoolError::None);
}

ObjectPoolFailure FrameObjectPool::AbandonLocked(Slot& slot, const Object::CapabilityError mintError) noexcept
{
    // No capability names the Frame, so nothing can hold a reference and it
    // is collectable at once; nothing wrote its pages.
    const Object::ObjectRegistryError destroyed = m_issuer.DestroyObject(slot.Identity);
    const auto collected =
        (destroyed == Object::ObjectRegistryError::Ok)
            ? m_issuer.CollectDestroyedObject(slot.Identity)
            : Foundation::Result<Object::ObjectDestructionRecord, Object::ObjectRegistryError>::Failure(destroyed);
    if (!collected.HasValue())
    {
        return FaultLocked(MakeObjectPoolFailure(ObjectPoolError::InvariantViolation, mintError,
                                                 Resource::ResourceDomainError::None, collected.GetError()));
    }
    const ObjectPoolFailure released = ReleaseSlotLocked(slot, true);
    return (released.Error != ObjectPoolError::None) ? FaultLocked(released)
                                                     : MakeObjectPoolFailure(ObjectPoolError::MintRejected, mintError);
}

Foundation::Result<Object::ObjectId, ObjectPoolFailure>
FrameObjectPool::Destroy(Object::CapabilitySpace& space, const Object::CapabilityHandle frame) noexcept
{
    using ResultType = Foundation::Result<Object::ObjectId, ObjectPoolFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == FrameObjectPoolState::Faulted)
    {
        return ResultType::Failure(MakeObjectPoolFailure(ObjectPoolError::Faulted));
    }
    const auto lease = space.Lookup(frame, Object::ObjectType::Frame, Object::CapabilityOperation::Manage);
    if (!lease.HasValue())
    {
        return ResultType::Failure(MakeObjectPoolFailure(ObjectPoolError::CapabilityRejected, lease.GetError()));
    }
    Slot* const slot = FindSlotLocked(lease.GetValue().GetObject());
    if ((slot == nullptr) || (slot->State != SlotState::Active) ||
        (slot->Identity.GetValue() != lease.GetValue().GetDescriptor().Object.GetValue()))
    {
        return ResultType::Failure(MakeObjectPoolFailure(ObjectPoolError::NotOwned));
    }
    const Object::ObjectRegistryError destroyed = m_issuer.DestroyObject(slot->Identity);
    if (destroyed != Object::ObjectRegistryError::Ok)
    {
        return ResultType::Failure(MakeObjectPoolFailure(ObjectPoolError::DestroyRejected, Object::CapabilityError::Ok,
                                                         Resource::ResourceDomainError::None, destroyed));
    }
    SetStateLocked(*slot, SlotState::Quiescing);
    ++m_destroyed;
    return ResultType::Success(slot->Identity);
}

// A drained Frame that a surviving space still maps stays Quiescing, pinned
// by that space, until it is unmapped.
ObjectPoolFailure FrameObjectPool::DestroyCharged(const Resource::IResourceDomainSubtree& subtree) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == FrameObjectPoolState::Faulted)
    {
        return MakeObjectPoolFailure(ObjectPoolError::Faulted);
    }
    // A drain must not release the owner-less run while Zero executes.
    for (Foundation::Size index = m_sets.FindFirst(SlotState::Zeroing); index < Capacity;
         index = m_sets.FindFirst(SlotState::Zeroing, index + 1U))
    {
        if (subtree.Contains(m_slots[index].Domain))
        {
            return MakeObjectPoolFailure(ObjectPoolError::Busy);
        }
    }
    // Only the live slots: each step finds the next one in the bitmap.
    for (Foundation::Size index = m_sets.FindFirst(SlotState::Active); index < Capacity;
         index = m_sets.FindFirst(SlotState::Active, index + 1U))
    {
        Slot& slot = m_slots[index];
        if (!subtree.Contains(slot.Domain))
        {
            continue;
        }
        const Object::ObjectRegistryError destroyed = m_issuer.DestroyObject(slot.Identity);
        if (destroyed != Object::ObjectRegistryError::Ok)
        {
            return FaultLocked(MakeObjectPoolFailure(ObjectPoolError::InvariantViolation, Object::CapabilityError::Ok,
                                                     Resource::ResourceDomainError::None, destroyed));
        }
        SetStateLocked(slot, SlotState::Quiescing);
        ++m_destroyed;
    }
    return MakeObjectPoolFailure(ObjectPoolError::None);
}

Foundation::Result<Foundation::Size, ObjectPoolFailure> FrameObjectPool::Collect() noexcept
{
    using ResultType = Foundation::Result<Foundation::Size, ObjectPoolFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == FrameObjectPoolState::Faulted)
    {
        return ResultType::Failure(MakeObjectPoolFailure(ObjectPoolError::Faulted));
    }
    Foundation::Size reclaimed{0U};
    // Only the quiescing slots: each step finds the next one in the bitmap, so a Collect with
    // nothing to reclaim reads only its summary.
    for (Foundation::Size index = m_sets.FindFirst(SlotState::Quiescing); index < Capacity;
         index = m_sets.FindFirst(SlotState::Quiescing, index + 1U))
    {
        Slot& slot = m_slots[index];
        const auto collected = m_issuer.CollectDestroyedObject(slot.Identity);
        if (!collected.HasValue() && (collected.GetError() == Object::ObjectRegistryError::NoDestructionReady))
        {
            continue;
        }
        if (!collected.HasValue() ||
            (collected.GetValue().Instance != static_cast<Object::IKernelObject*>(slot.Instance)))
        {
            return ResultType::Failure(FaultLocked(MakeObjectPoolFailure(
                ObjectPoolError::InvariantViolation, Object::CapabilityError::Ok, Resource::ResourceDomainError::None,
                collected.HasValue() ? Object::ObjectRegistryError::Ok : collected.GetError())));
        }
        const ObjectPoolFailure released = ReleaseSlotLocked(slot, false);
        if (released.Error != ObjectPoolError::None)
        {
            return ResultType::Failure(FaultLocked(released));
        }
        ++reclaimed;
        ++m_reclaimed;
    }
    return ResultType::Success(reclaimed);
}

FrameObjectPoolSnapshot FrameObjectPool::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return FrameObjectPoolSnapshot{
        .FreeSlots = m_sets.GetFreeCount(),
        .ActiveSlots = m_sets.GetActiveCount(),
        .QuiescingSlots = m_sets.GetQuiescingCount(),
        .ZeroingSlots = m_sets.GetZeroingCount(),
        .PeakZeroingSlots = m_peakZeroing,
        .ZeroDeferrals = m_zeroDeferrals,
        .ZeroFaults = m_zeroFaults,
        .CreationZeroedPages = __atomic_load_n(&m_creationZeroedPages, __ATOMIC_ACQUIRE),
        .Created = m_created,
        .Destroyed = m_destroyed,
        .Reclaimed = m_reclaimed,
        .State = m_state,
        .Reserved = {0U, 0U, 0U, 0U, 0U, 0U, 0U},
    };
}

FrameObjectPool::Slot* FrameObjectPool::FindFreeSlotLocked() noexcept
{
    const Foundation::Size index = m_sets.FindFirst(SlotState::Free);
    return (index < Capacity) ? &m_slots[index] : nullptr;
}

FrameObjectPool::Slot* FrameObjectPool::FindSlotLocked(const Object::IKernelObject& instance) const noexcept
{
    // A Frame of the slab lies in its slot's storage, so its address names
    // the slot; the slot must then hold exactly that instance.
    const auto address = reinterpret_cast<Foundation::UIntPointer>(&instance);
    const auto base = reinterpret_cast<Foundation::UIntPointer>(m_slots);
    if ((m_slots == nullptr) || (address < base) || ((address - base) >= SlabBytes))
    {
        return nullptr;
    }
    Slot& slot = m_slots[(address - base) / sizeof(Slot)];
    return ((slot.Instance != nullptr) && (static_cast<const Object::IKernelObject*>(slot.Instance) == &instance))
               ? &slot
               : nullptr;
}

Foundation::Size FrameObjectPool::IndexOf(const Slot& slot) const noexcept
{
    return static_cast<Foundation::Size>(&slot - m_slots);
}

void FrameObjectPool::SetStateLocked(Slot& slot, const SlotState state) noexcept
{
    m_sets.Move(IndexOf(slot), slot.State, state);
    slot.State = state;
}

ObjectPoolFailure FrameObjectPool::ReleaseSlotLocked(Slot& slot, const bool clean) noexcept
{
    // A collected Frame has no reservation and no pin: every mapping that
    // reserved its policy also pinned it. A device Frame's pages are its
    // region's, which nothing scrubs or hands out.
    const bool device = slot.Region.IsValid();
    PhysicalPageRun run{.PhysicalStart = slot.PhysicalStart, .Pages = slot.Pages.GetValue(), .DirtyMask = {}};
    for (Foundation::Size index{0U}; index < PhysicalPageAllocator::DirtyWords; ++index)
    {
        run.DirtyMask[index] = slot.DirtyMask[index];
    }
    const PhysicalPageAllocatorError returned =
        (device || (run.Pages == 0U))
            ? PhysicalPageAllocatorError::None
            : (clean ? m_pages.GetAllocator().ReturnVerified(run) : m_pages.GetAllocator().Release(run));
    if (returned != PhysicalPageAllocatorError::None)
    {
        return MakeObjectPoolFailure(ObjectPoolError::InvariantViolation, Object::CapabilityError::Ok,
                                     Resource::ResourceDomainError::None, Object::ObjectRegistryError::Ok,
                                     static_cast<Foundation::UInt8>(returned));
    }
    const Resource::ResourceDomainError released = slot.Domain->Release(slot.Charge);
    if (released != Resource::ResourceDomainError::None)
    {
        return MakeObjectPoolFailure(ObjectPoolError::InvariantViolation, Object::CapabilityError::Ok, released);
    }
    if (slot.Instance != nullptr)
    {
        slot.Instance->~CreatedFrame();
    }
    slot.Instance = nullptr;
    slot.Domain = nullptr;
    slot.DomainPin.Reset();
    slot.Charge = Resource::ResourceReservation{};
    slot.PhysicalStart = PhysicalAddress{};
    slot.Pages = PageCount{};
    slot.Identity = Object::ObjectId{};
    slot.RegionPin.Reset();
    slot.Region = Device::DeviceRegionId{};
    slot.CachePolicy = MemoryCachePolicy::WriteBack;
    SetStateLocked(slot, SlotState::Free);
    return MakeObjectPoolFailure(ObjectPoolError::None);
}

// The region must be memory-mapped and Ready, its base page-aligned, and the Frame's pages within
// it.
ObjectPoolFailure FrameObjectPool::BindRegionLocked(Slot& slot, Object::CapabilitySpace& space,
                                                    const FrameCreateRequest& request) noexcept
{
    const auto lease = space.Lookup(request.Region, Object::ObjectType::DeviceRegion, Object::CapabilityOperation::Map);
    if (!lease.HasValue())
    {
        return MakeObjectPoolFailure(ObjectPoolError::CapabilityRejected, lease.GetError());
    }
    // The lookup checked the type, and only DeviceRegion instances report it.
    const Device::DeviceRegionSnapshot region =
        static_cast<const Device::DeviceRegion&>(lease.GetValue().GetObject()).GetSnapshot();
    const Foundation::UInt64 regionPages = region.Descriptor.LengthBytes / MappingPageSizeBytes;
    if ((region.State != Device::DeviceRegionState::Ready) ||
        (region.Descriptor.Kind != Device::DeviceRegionKind::MemoryMappedIo) ||
        ((region.Descriptor.BaseAddress % MappingPageSizeBytes) != 0U) || (request.RegionOffsetPages >= regionPages) ||
        (request.Pages.GetValue() > (regionPages - request.RegionOffsetPages)))
    {
        return MakeObjectPoolFailure(ObjectPoolError::InvalidRequest);
    }
    const Object::ObjectId object = lease.GetValue().GetDescriptor().Object;
    if ((m_objectRegistry.PinObject(object, Object::ObjectType::DeviceRegion, slot.RegionPin) !=
         Object::ObjectPinError::Ok) ||
        (&slot.RegionPin.GetObject() != &lease.GetValue().GetObject()))
    {
        slot.RegionPin.Reset();
        return MakeObjectPoolFailure(ObjectPoolError::CapabilityRejected, Object::CapabilityError::ObjectNotFound);
    }
    slot.Region = Device::DeviceRegionId{m_objectRegistry, object};
    slot.PhysicalStart =
        PhysicalAddress{region.Descriptor.BaseAddress + (request.RegionOffsetPages * MappingPageSizeBytes)};
    slot.CachePolicy = region.Descriptor.CachePolicy;
    return MakeObjectPoolFailure(ObjectPoolError::None);
}

Device::DeviceRegionId FrameObjectPool::GetDeviceRegion(const Frame& frame) const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    const Slot* const slot = FindSlotLocked(frame);
    return ((slot != nullptr) && (slot->State == SlotState::Active)) ? slot->Region : Device::DeviceRegionId{};
}

ObjectPoolFailure FrameObjectPool::FaultLocked(const ObjectPoolFailure failure) noexcept
{
    m_state = FrameObjectPoolState::Faulted;
    ObjectPoolFailure faulted = failure;
    faulted.Error = ObjectPoolError::InvariantViolation;
    return faulted;
}
} // Namespace Asteria::Kernel::Memory
