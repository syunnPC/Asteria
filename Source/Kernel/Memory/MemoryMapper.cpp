// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Memory/MemoryMapper.hpp"

#include "Kernel/Memory/Frame.hpp"
#include "Kernel/Memory/MemoryObject.hpp"
#include "Kernel/Scheduler/CrossProcessorWait.hpp"

namespace Asteria::Kernel::Memory
{
namespace
{
[[nodiscard]] constexpr MemoryMapperResult
MakeResult(const MemoryMapperError error, const Object::CapabilityError capability = Object::CapabilityError::Ok,
           const PinnedFrameMappingError preparation = PinnedFrameMappingError::None,
           const AddressSpaceFailure addressSpace = {}) noexcept
{
    return MemoryMapperResult{
        .Error = error,
        .Capability = capability,
        .Preparation = preparation,
        .AddressSpace = addressSpace.Error,
        .PageTable = addressSpace.PageTableError,
        .Reserved = {0U, 0U, 0U},
    };
}

/// What a rejected BeginPinnedFrameMapping means for the caller, unless the
/// AddressSpace became terminal.
[[nodiscard]] constexpr MemoryMapperError ClassifyRejection(const AddressSpaceFailure& failure) noexcept
{
    switch (failure.Error)
    {
    case AddressSpaceError::NotReady:
    case AddressSpaceError::ActivationPending:
    case AddressSpaceError::ProcessorTransitionPending:
    case AddressSpaceError::UserMappingInUse:
    case AddressSpaceError::ShootdownPending:
        return MemoryMapperError::Busy;
    case AddressSpaceError::MappingCapacityExceeded:
    case AddressSpaceError::MappingSequenceExhausted:
        return MemoryMapperError::CapacityExceeded;
    case AddressSpaceError::PageTableRejected:
        return (failure.PageTableError == Hal::Memory::AddressSpacePageTableError::CapacityExceeded)
                   ? MemoryMapperError::CapacityExceeded
                   : MemoryMapperError::InvalidRequest;
    default:
        break;
    }
    return MemoryMapperError::InvalidRequest;
}
} // Namespace

bool MemoryMapper::Configure(TlbShootdownDispatcher& shootdown, Hal::Memory::ITlbShootdownTransport& transport,
                             IPagerShootdownProgress& progress) noexcept
{
    if (m_shootdown != nullptr)
    {
        return false;
    }
    m_shootdown = &shootdown;
    m_transport = &transport;
    m_progress = &progress;
    return true;
}

MemoryMapperResult MemoryMapper::Map(Object::CapabilitySpace& caller, const MemoryMapRequest& request) noexcept
{
    if (m_faulted)
    {
        return Finish(MakeResult(MemoryMapperError::Faulted));
    }
    if (m_shootdown == nullptr)
    {
        return Finish(MakeResult(MemoryMapperError::NotConfigured));
    }
    // A user's request maps only memory its own code may reach.
    if (!request.Mapping.Permissions.Contains(PagePermission::User))
    {
        return Finish(MakeResult(MemoryMapperError::InvalidRequest, Object::CapabilityError::Ok,
                                 PinnedFrameMappingError::InvalidRequest));
    }
    const auto space =
        caller.Lookup(request.AddressSpace, Object::ObjectType::AddressSpace, Object::CapabilityOperation::Map);
    if (!space.HasValue())
    {
        return Finish(MakeResult(MemoryMapperError::CapabilityRejected, space.GetError()));
    }
    m_space = &static_cast<AddressSpace&>(space.GetValue().GetObject());
    m_spaceId = AddressSpaceId{m_registry, space.GetValue().GetDescriptor().Object};
    return MapWithBacking(caller, request);
}

MemoryMapperResult MemoryMapper::MapWithBacking(Object::CapabilitySpace& caller,
                                                const MemoryMapRequest& request) noexcept
{
    const auto memory =
        caller.Lookup(request.MemoryObject, Object::ObjectType::MemoryObject, Object::CapabilityOperation::Map);
    if (!memory.HasValue())
    {
        return Finish(MakeResult(MemoryMapperError::CapabilityRejected, memory.GetError()));
    }
    const auto frame = caller.Lookup(request.Frame, Object::ObjectType::Frame, Object::CapabilityOperation::Map);
    if (!frame.HasValue())
    {
        return Finish(MakeResult(MemoryMapperError::CapabilityRejected, frame.GetError()));
    }
    // Map implies Read; CPU writes also require Write.
    if (request.Mapping.Permissions.Contains(PagePermission::Write) &&
        !frame.GetValue().GetDescriptor().Rights.ContainsAll(Object::CapabilityRights{Object::CapabilityRight::Write}))
    {
        return Finish(MakeResult(MemoryMapperError::CapabilityRejected, Object::CapabilityError::RightsDenied));
    }
    m_memory = &static_cast<MemoryObject&>(memory.GetValue().GetObject());
    m_frame = &static_cast<Frame&>(frame.GetValue().GetObject());
    m_memoryId = MemoryObjectId{m_registry, memory.GetValue().GetDescriptor().Object};
    m_frameId = FrameId{m_registry, frame.GetValue().GetDescriptor().Object};
    m_region = (m_regions != nullptr) ? m_regions->GetDeviceRegion(*m_frame) : Device::DeviceRegionId{};
    return Install(request.Mapping);
}

MemoryMapperResult MemoryMapper::Install(const PinnedFrameMappingRequest& mapping) noexcept
{
    const PinnedFrameMappingError prepared =
        m_preparation.Prepare(m_registry, m_memoryId, *m_memory, m_frameId, *m_frame, mapping, m_region);
    if (prepared != PinnedFrameMappingError::None)
    {
        return Finish(MakeResult(MemoryMapperError::InvalidRequest, Object::CapabilityError::Ok, prepared));
    }
    m_pendingMapping = MemoryMapperRange{
        .VirtualStart = mapping.VirtualStart,
        .PhysicalStart = m_preparation.GetMapping().PhysicalStart,
        .Pages = mapping.Pages,
    };
    // The AddressSpace takes its own pins of both objects before it accepts.
    const auto begun = m_space->WaitForPinnedFrameMapping(m_spaceId, m_preparation);
    m_preparation.Reset();
    if (!begun.HasValue())
    {
        const MemoryMapperError error = ClassifyRejection(begun.GetError());
        const MemoryMapperResult result =
            MakeResult(error, Object::CapabilityError::Ok, PinnedFrameMappingError::None, begun.GetError());
        return (m_space->GetSnapshot().State == AddressSpaceState::Faulted) ? Fault(result) : Finish(result);
    }
    return Complete(begun.GetValue(), false);
}

MemoryMapperResult MemoryMapper::Unmap(Object::CapabilitySpace& caller, const Object::CapabilityHandle space,
                                       const VirtualAddress virtualStart, const PageCount pages) noexcept
{
    if (m_faulted)
    {
        return Finish(MakeResult(MemoryMapperError::Faulted));
    }
    if (m_shootdown == nullptr)
    {
        return Finish(MakeResult(MemoryMapperError::NotConfigured));
    }
    const auto lease = caller.Lookup(space, Object::ObjectType::AddressSpace, Object::CapabilityOperation::Map);
    if (!lease.HasValue())
    {
        return Finish(MakeResult(MemoryMapperError::CapabilityRejected, lease.GetError()));
    }
    m_space = &static_cast<AddressSpace&>(lease.GetValue().GetObject());
    const auto begun = m_space->WaitForUnmap(virtualStart, pages);
    if (!begun.HasValue())
    {
        const AddressSpaceFailure failure = begun.GetError();
        const MemoryMapperError error =
            (failure.Error == AddressSpaceError::MappingNotFound)
                ? MemoryMapperError::InvalidRequest
                : ((failure.PageTableError == Hal::Memory::AddressSpacePageTableError::UnsupportedConfiguration)
                       ? MemoryMapperError::NotConfigured
                       : ClassifyRejection(failure));
        const MemoryMapperResult result =
            MakeResult(error, Object::CapabilityError::Ok, PinnedFrameMappingError::None, failure);
        return (m_space->GetSnapshot().State == AddressSpaceState::Faulted) ? Fault(result) : Finish(result);
    }
    return Complete(begun.GetValue(), true);
}

MemoryMapperResult MemoryMapper::Complete(const AddressSpaceShootdownRequest& request, const bool unmapping) noexcept
{
    const AddressSpaceMappingToken token = request.GetToken();
    const auto dispatched = m_shootdown->Dispatch(*m_space, request, *m_transport);
    if (!dispatched.HasValue())
    {
        static_cast<void>(m_space->FailShootdown(token));
        return Fault(MakeResult(MemoryMapperError::Faulted));
    }
    for (Scheduler::CrossProcessorWait wait;;)
    {
        const auto completed = m_space->CompleteMapping(token);
        if (completed.HasValue())
        {
            if ((completed.GetValue().MappingSequence != token.GetSequence()) ||
                (completed.GetValue().VirtualStart.GetValue() != request.GetInvalidationStart().GetValue()) ||
                (completed.GetValue().Pages.GetValue() != request.GetInvalidationPages().GetValue()))
            {
                return Fault(MakeResult(MemoryMapperError::Faulted));
            }
            if (unmapping)
            {
                static_cast<void>(__atomic_fetch_add(&m_unmappings, 1U, __ATOMIC_RELAXED));
            }
            else
            {
                static_cast<void>(__atomic_fetch_add(&m_mappings, 1U, __ATOMIC_RELAXED));
                m_lastMapping = m_pendingMapping;
            }
            return Finish(MakeResult(MemoryMapperError::None));
        }
        if (completed.GetError().Error != AddressSpaceError::ShootdownIncomplete)
        {
            return Fault(MakeResult(MemoryMapperError::Faulted, Object::CapabilityError::Ok,
                                    PinnedFrameMappingError::None, completed.GetError()));
        }
        // A stall is a processor-set decision, never a mapper latch that silently rejects
        // subsequent calls.
        if (!wait.Poll())
        {
            const auto snapshot = m_space->GetSnapshot();
            Scheduler::CrossProcessorWait::Timeout(snapshot.TargetProcessorMask & ~snapshot.AcknowledgedProcessorMask);
        }
        m_progress->Wait();
    }
}

MemoryMapperResult MemoryMapper::Finish(const MemoryMapperResult result) noexcept
{
    m_preparation.Reset();
    m_space = nullptr;
    m_memory = nullptr;
    m_frame = nullptr;
    m_spaceId = AddressSpaceId{};
    m_memoryId = MemoryObjectId{};
    m_frameId = FrameId{};
    m_pendingMapping = MemoryMapperRange{};
    m_last = result;
    return result;
}

MemoryMapperResult MemoryMapper::Fault(MemoryMapperResult result) noexcept
{
    m_faulted = true;
    result.Error = MemoryMapperError::Faulted;
    return Finish(result);
}
} // Namespace Asteria::Kernel::Memory
