// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_DIRECT_USER_MEMORY_ACCESS_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_DIRECT_USER_MEMORY_ACCESS_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Memory/Frame.hpp"
#include "Kernel/Memory/IUserMemoryAccess.hpp"
#include "Kernel/Memory/AddressSpace.hpp"
#include "Kernel/Memory/MemoryTypes.hpp"

namespace Asteria::Kernel::Memory
{
constexpr Foundation::Size MaximumDirectUserMemoryMappings{10U};

/// One caller-proven, pinned user RAM interval and its direct kernel address.
/// The backing storage remains present and stable for the lifetime of the
/// DirectUserMemoryAccess initialized from this descriptor.
struct DirectUserMemoryMapping
{
    VirtualAddress UserStart;
    Foundation::UInt8* DirectStart;
    PageCount Pages;
    PagePermissions Permissions;
    FrameKind Kind;
};

static_assert(sizeof(DirectUserMemoryMapping) == 32U);
static_assert(alignof(DirectUserMemoryMapping) == 8U);

enum class DirectUserMemoryAccessError : Foundation::UInt8
{
    None,
    InvalidAddressSpace,
    InvalidMappingList,
    MappingCapacityExceeded,
    InvalidMapping,
    MappingsNotOrdered,
    MappingsOverlap,
    AlreadyInitialized,
};

class DirectUserMemoryReservationGenerationPolicy final
{
public:
    static constexpr Foundation::UInt64 Maximum{~Foundation::UInt64{0U}};

    DirectUserMemoryReservationGenerationPolicy() = delete;

    /// Advances current without wrapping. Failure leaves next unchanged.
    [[nodiscard]] static constexpr bool TryAdvance(const Foundation::UInt64 current, Foundation::UInt64& next) noexcept
    {
        if (current == Maximum)
        {
            return false;
        }
        next = current + 1U;
        return true;
    }
};

/// Allocation-free IUserMemoryAccess for one currently active address space. Initialization copies
/// and validates a fixed-capacity mapping list. The caller proves that every direct address names
/// ordinary RAM. Native owners provide the AddressSpace lifetime so each copy pins its records; an
/// omitted lifetime requires immutable, permanently pinned mappings. This backend neither walks
/// page tables nor recovers native faults. Concurrency: - The owner serializes all operations. At
/// most one write reservation may be live, including between PrepareCopyToUser and Commit/Cancel.
class DirectUserMemoryAccess final : public IUserMemoryAccess
{
public:
    constexpr DirectUserMemoryAccess() noexcept = default;

    DirectUserMemoryAccess(const DirectUserMemoryAccess&) = delete;
    DirectUserMemoryAccess& operator=(const DirectUserMemoryAccess&) = delete;
    DirectUserMemoryAccess(DirectUserMemoryAccess&&) = delete;
    DirectUserMemoryAccess& operator=(DirectUserMemoryAccess&&) = delete;

    [[nodiscard]] DirectUserMemoryAccessError Initialize(AddressSpaceId addressSpace,
                                                         const DirectUserMemoryMapping* mappings,
                                                         Foundation::Size mappingCount,
                                                         const AddressSpace* lifetime = nullptr) noexcept;

    [[nodiscard]] bool BindLifetime(const AddressSpace& space) noexcept
    {
        if (m_hasActiveReservation || !space.GetIdentity().IsEqualTo(m_addressSpace))
        {
            return false;
        }
        m_lifetime = &space;
        return true;
    }

    [[nodiscard]] AddressSpaceId GetAddressSpaceId() const noexcept override;
    [[nodiscard]] bool IsReady() const noexcept override;

private:
    [[nodiscard]] UserMemoryAccessError CopyFromUserValidated(UserMemoryRange source,
                                                              WritableKernelBuffer destination) noexcept override;
    [[nodiscard]] Foundation::Result<UserMemoryWriteReservation, UserMemoryAccessError>
    PrepareCopyToUserValidated(UserMemoryRange destination) noexcept override;
    [[nodiscard]] UserMemoryAccessError
    CancelCopyToUserValidated(UserMemoryWriteReservation reservation) noexcept override;
    [[nodiscard]] UserMemoryAccessError CommitCopyToUserValidated(UserMemoryWriteReservation reservation,
                                                                  ReadOnlyKernelBuffer source) noexcept override;

    [[nodiscard]] static bool IsMappingValid(const DirectUserMemoryMapping& mapping) noexcept;
    [[nodiscard]] static Foundation::UInt64 GetMappingSizeBytes(const DirectUserMemoryMapping& mapping) noexcept;
    [[nodiscard]] bool IsRangeAccessible(UserMemoryRange range, bool requireWrite) const noexcept;
    [[nodiscard]] bool IsCurrentReservation(UserMemoryWriteReservation reservation) const noexcept;
    void CopyFromMappings(UserMemoryRange source, WritableKernelBuffer destination) const noexcept;
    void CopyToMappings(UserMemoryRange destination, ReadOnlyKernelBuffer source) noexcept;

    DirectUserMemoryMapping m_mappings[MaximumDirectUserMemoryMappings]{};
    Foundation::Size m_mappingCount{0U};
    AddressSpaceId m_addressSpace{};
    const AddressSpace* m_lifetime{nullptr};
    UserMappingUse m_activeUse{};
    UserMemoryRange m_activeRange{};
    Foundation::UInt64 m_lastGeneration{0U};
    Foundation::UInt64 m_activeGeneration{0U};
    bool m_ready{false};
    bool m_hasActiveReservation{false};
};
} // Namespace Asteria::Kernel::Memory

#endif // ASTERIA_KERNEL_MEMORY_DIRECT_USER_MEMORY_ACCESS_HPP_INCLUDED
