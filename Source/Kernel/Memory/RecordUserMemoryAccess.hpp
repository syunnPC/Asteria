// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_RECORD_USER_MEMORY_ACCESS_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_RECORD_USER_MEMORY_ACCESS_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Memory/AddressSpace.hpp"
#include "Kernel/Memory/IUserMemoryAccess.hpp"

namespace Asteria::Kernel::Memory
{
/// IUserMemoryAccess for a domain whose memory is all run-time mappings: a created component's,
/// which MemoryMap built. A range is admitted only if one completed runtime mapping of the
/// AddressSpace holds all of it and that mapping is user, readable RAM (and writable for a write);
/// seed mappings never qualify, so a request cannot name kernel memory. The copy goes through the
/// user virtual address in the active root, which the system-call ingress requires to be this
/// AddressSpace's (GetAddressSpaceId names it). Mapping use counts protect
/// native accesses without holding a lock during a copy. Each thread owns one accessor and at most
/// one write reservation, pinned until Commit or Cancel.
class RecordUserMemoryAccess final : public IUserMemoryAccess
{
public:
    constexpr RecordUserMemoryAccess() noexcept = default;

    RecordUserMemoryAccess(const RecordUserMemoryAccess&) = delete;
    RecordUserMemoryAccess& operator=(const RecordUserMemoryAccess&) = delete;
    RecordUserMemoryAccess(RecordUserMemoryAccess&&) = delete;
    RecordUserMemoryAccess& operator=(RecordUserMemoryAccess&&) = delete;

    /// Binds the accessor to space, whose registered identity is identity;
    /// false when already bound or either is invalid. Rebinding after Reset
    /// serves a new domain.
    [[nodiscard]] bool Bind(const AddressSpace& space, AddressSpaceId identity, bool includeSeed = false) noexcept;

    /// Forgets the bound space; false while a write reservation is live.
    [[nodiscard]] bool Reset() noexcept;

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

    [[nodiscard]] bool IsCurrentReservation(UserMemoryWriteReservation reservation) const noexcept;

    UserMappingUse m_activeUse{};
    bool m_includeSeed{false};
    const AddressSpace* m_space{nullptr};
    AddressSpaceId m_identity{};
    UserMemoryRange m_activeRange{};
    Foundation::UInt64 m_lastGeneration{0U};
    Foundation::UInt64 m_activeGeneration{0U};
    bool m_hasActiveReservation{false};
};
} // Namespace Asteria::Kernel::Memory

#endif // ASTERIA_KERNEL_MEMORY_RECORD_USER_MEMORY_ACCESS_HPP_INCLUDED
