// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_MEMORY_MAPPER_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_MEMORY_MAPPER_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Hal/Memory/IAddressSpacePageTable.hpp"
#include "Hal/Memory/ITlbShootdownTransport.hpp"
#include "Kernel/Memory/AddressSpace.hpp"
#include "Kernel/Memory/DeviceFrameRegions.hpp"
#include "Kernel/Memory/IPagerShootdownProgress.hpp"
#include "Kernel/Memory/PinnedFrameMapping.hpp"
#include "Kernel/Memory/TlbShootdownDispatcher.hpp"
#include "Kernel/Object/CapabilitySpace.hpp"

namespace Asteria::Kernel::Memory
{
enum class MemoryMapperError : Foundation::UInt8
{
    None,
    /// The mapper has no shootdown collaborators.
    NotConfigured,
    /// A handle failed its lookup with Map; Capability says why.
    CapabilityRejected,
    /// The objects or the range cannot be mapped as asked, or no mapping of
    /// exactly that range exists to unmap: Preparation or AddressSpace says
    /// why.
    InvalidRequest,
    /// The AddressSpace has no free runtime record or page-table page.
    CapacityExceeded,
    /// Another transaction of the AddressSpace is in flight; retryable.
    Busy,
    /// The native state may have changed and could not be completed; the
    /// mapper and the AddressSpace are terminal.
    Faulted,
};

/// One MemoryMap: the target AddressSpace, the MemoryObject and the Frame, each a capability with
/// Map in the caller's space, and the mapping.
struct MemoryMapRequest
{
    Object::CapabilityHandle AddressSpace;
    Object::CapabilityHandle MemoryObject;
    Object::CapabilityHandle Frame;
    PinnedFrameMappingRequest Mapping;
};

struct MemoryMapperResult
{
    MemoryMapperError Error;
    Object::CapabilityError Capability;
    PinnedFrameMappingError Preparation;
    AddressSpaceError AddressSpace;
    Hal::Memory::AddressSpacePageTableError PageTable;
    Foundation::UInt8 Reserved[3];
};

/// One completed mapping's range.
struct MemoryMapperRange
{
    VirtualAddress VirtualStart;
    PhysicalAddress PhysicalStart;
    PageCount Pages;
};

struct MemoryMapperSnapshot
{
    /// Mappings and unmappings this mapper completed.
    Foundation::UInt64 Mappings;
    Foundation::UInt64 Unmappings;
    /// The last result, kept for diagnostics until the next call.
    MemoryMapperResult Last;
    /// The last completed mapping, or zero.
    MemoryMapperRange LastMapping;
};

/// Installs user mappings of created memory on a caller's request: proves the caller holds the
/// AddressSpace, the MemoryObject and the Frame, each with Map, prepares the exact pinned mapping
/// and runs the AddressSpace's mapping transaction with its shootdown. The AddressSpace keeps both
/// objects pinned while the mapping exists. Only user mappings are admitted: a request without
/// PagePermission::User is invalid. One call at a time, on the processor the shootdown dispatcher
/// is bound to, with local interrupts disabled, outside an ISR and without same-CPU reentry.
/// A timeout stops or drops a retiring processor; uncertain native state
/// faults the mapper.
class MemoryMapper final
{
public:
    /// Regions names the DeviceRegion of a device Frame; null where no device Frame can exist.
    explicit constexpr MemoryMapper(Object::ObjectRegistry& registry,
                                    const IDeviceFrameRegions* const regions = nullptr) noexcept
        : m_registry{registry}, m_regions{regions}
    {
    }

    ~MemoryMapper() noexcept = default;

    MemoryMapper(const MemoryMapper&) = delete;
    MemoryMapper& operator=(const MemoryMapper&) = delete;
    MemoryMapper(MemoryMapper&&) = delete;
    MemoryMapper& operator=(MemoryMapper&&) = delete;

    /// Once only; false when already configured. Every collaborator outlives
    /// this mapper.
    [[nodiscard]] bool Configure(TlbShootdownDispatcher& shootdown, Hal::Memory::ITlbShootdownTransport& transport,
                                 IPagerShootdownProgress& progress) noexcept;

    [[nodiscard]] MemoryMapperResult Map(Object::CapabilitySpace& caller, const MemoryMapRequest& request) noexcept;

    /// Removes the mapping of exactly [virtualStart, virtualStart + pages)
    /// from the AddressSpace space (a capability with Map in caller) names,
    /// through the same shootdown as Map; the AddressSpace releases the
    /// mapping's objects once every resident processor acknowledged.
    [[nodiscard]] MemoryMapperResult Unmap(Object::CapabilitySpace& caller, Object::CapabilityHandle space,
                                           VirtualAddress virtualStart, PageCount pages) noexcept;

    [[nodiscard]] Foundation::UInt64 GetMappings() const noexcept
    {
        return __atomic_load_n(&m_mappings, __ATOMIC_ACQUIRE);
    }
    [[nodiscard]] Foundation::UInt64 GetUnmappings() const noexcept
    {
        return __atomic_load_n(&m_unmappings, __ATOMIC_ACQUIRE);
    }
    [[nodiscard]] MemoryMapperSnapshot GetSnapshot() const noexcept
    {
        return MemoryMapperSnapshot{
            .Mappings = GetMappings(), .Unmappings = GetUnmappings(), .Last = m_last, .LastMapping = m_lastMapping};
    }

private:
    // The call is split so each frame stays within the target stack budget,
    // and each lookup's lease lives until the transaction ends: the space,
    // then the MemoryObject and Frame, then the transaction.
    [[nodiscard, gnu::noinline]] MemoryMapperResult MapWithBacking(Object::CapabilitySpace& caller,
                                                                   const MemoryMapRequest& request) noexcept;
    [[nodiscard, gnu::noinline]] MemoryMapperResult Install(const PinnedFrameMappingRequest& mapping) noexcept;
    [[nodiscard, gnu::noinline]] MemoryMapperResult Complete(const AddressSpaceShootdownRequest& request,
                                                             bool unmapping) noexcept;
    [[nodiscard]] MemoryMapperResult Finish(MemoryMapperResult result) noexcept;
    [[nodiscard]] MemoryMapperResult Fault(MemoryMapperResult result) noexcept;

    Object::ObjectRegistry& m_registry;
    const IDeviceFrameRegions* m_regions{nullptr};
    TlbShootdownDispatcher* m_shootdown{nullptr};
    Hal::Memory::ITlbShootdownTransport* m_transport{nullptr};
    IPagerShootdownProgress* m_progress{nullptr};
    PinnedFrameMappingPreparation m_preparation;
    AddressSpace* m_space{nullptr};
    MemoryObject* m_memory{nullptr};
    Frame* m_frame{nullptr};
    AddressSpaceId m_spaceId{};
    MemoryObjectId m_memoryId{};
    FrameId m_frameId{};
    /// The device Frame's region, invalid for RAM.
    Device::DeviceRegionId m_region{};
    Foundation::UInt64 m_mappings{0U};
    Foundation::UInt64 m_unmappings{0U};
    MemoryMapperResult m_last{};
    MemoryMapperRange m_pendingMapping{};
    MemoryMapperRange m_lastMapping{};
    bool m_faulted{false};
};

static_assert(sizeof(MemoryMapperError) == 1U);
static_assert(sizeof(MemoryMapRequest) == 88U);
static_assert(sizeof(MemoryMapperResult) == 8U);
} // Namespace Asteria::Kernel::Memory

#endif // ASTERIA_KERNEL_MEMORY_MEMORY_MAPPER_HPP_INCLUDED
