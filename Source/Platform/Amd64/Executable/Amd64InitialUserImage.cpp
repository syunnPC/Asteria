// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Executable/Amd64InitialUserImage.hpp"

#include "Runtime/Memory.hpp"

namespace Asteria::Platform::Amd64::Executable
{
namespace
{
using ::Asteria::Executable::Elf::Elf64SegmentPermission;
using Kernel::Memory::PagePermission;
using Kernel::Memory::PagePermissions;
constexpr Foundation::Size StorageBytes{InitialUserStoragePages * 4096U};
} // namespace

Amd64InitialUserImageError Amd64InitialUserImage::ValidateSegments(const Foundation::UInt64 physicalStart) noexcept
{
    Foundation::UInt64 pages{0U};
    Foundation::Size ranges{0U};
    bool entryValid{false};
    for (Foundation::Size index{0U}; index < m_plan.GetSegmentCount(); ++index)
    {
        const auto result = m_plan.GetSegment(index);
        if (!result.HasValue())
        {
            return Amd64InitialUserImageError::InvalidSegment;
        }
        const auto& segment = result.GetValue();
        // An empty PT_LOAD (p_memsz zero) is a valid ELF record that
        // describes no memory: it receives no range, page or window check.
        if (segment.PageCount == 0U)
        {
            continue;
        }
        if ((segment.PageStart < InitialUserImageStart) || (segment.PageStart >= InitialUserImageEnd) ||
            (segment.PageCount > (InitialUserImageEnd - segment.PageStart) / 4096U) ||
            !segment.Permissions.Contains(Elf64SegmentPermission::Read))
        {
            return Amd64InitialUserImageError::InvalidSegment;
        }
        if (segment.PageCount > InitialUserStoragePages - InitialUserStackPages - pages)
        {
            return Amd64InitialUserImageError::StorageExhausted;
        }
        PagePermissions permissions{PagePermission::Read};
        permissions = permissions.With(PagePermission::User);
        if (segment.Permissions.Contains(Elf64SegmentPermission::Write))
        {
            permissions = permissions.With(PagePermission::Write);
        }
        if (segment.Permissions.Contains(Elf64SegmentPermission::Execute))
        {
            permissions = permissions.With(PagePermission::Execute);
            entryValid = entryValid || ((m_entry >= segment.VirtualStart) &&
                                        (m_entry - segment.VirtualStart < segment.FileSizeBytes));
        }
        m_ranges[ranges] =
            Amd64InitialUserRange{segment.PageStart, physicalStart + pages * 4096U, segment.PageCount, permissions};
        ++ranges;
        pages += segment.PageCount;
    }
    if (!entryValid)
    {
        return Amd64InitialUserImageError::InvalidEntry;
    }
    m_rangeCount = ranges + 1U;
    m_ranges[m_rangeCount - 1U] = Amd64InitialUserRange{
        InitialUserStackStart, physicalStart + pages * 4096U, InitialUserStackPages,
        PagePermissions{PagePermission::Read}.With(PagePermission::Write).With(PagePermission::User)};
    return Amd64InitialUserImageError::None;
}

void Amd64InitialUserImage::Stage(const ::Asteria::Executable::Elf::Elf64ByteView image,
                                  Foundation::UInt8* const storage) noexcept
{
    static_cast<void>(memset(storage, 0, StorageBytes));
    Foundation::Size offset{0U};
    for (Foundation::Size index{0U}; index < m_plan.GetSegmentCount(); ++index)
    {
        const auto segment = m_plan.GetSegment(index).GetValue();
        if (segment.PageCount == 0U)
        {
            continue;
        }
        static_cast<void>(memcpy(storage + offset + (segment.VirtualStart - segment.PageStart),
                                 image.Data + segment.FileOffset,
                                 static_cast<Foundation::Size>(segment.FileSizeBytes)));
        offset += static_cast<Foundation::Size>(segment.PageCount * 4096U);
    }
}

bool Amd64InitialUserImage::MapModules(const Foundation::UInt64 physicalStart, const Foundation::UInt64 bytes) noexcept
{
    if (!m_ready || physicalStart == 0U || (physicalStart & 4095U) != 0U || bytes == 0U || (bytes & 4095U) != 0U ||
        bytes > Abi::PagerModuleMaximumBytes || physicalStart > ~Foundation::UInt64{0U} - bytes ||
        m_rangeCount >= MaximumInitialUserSegments + 2U)
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < m_rangeCount; ++index)
    {
        const Amd64InitialUserRange& range = m_ranges[index];
        if ((physicalStart < range.PhysicalStart + range.Pages * 4096U &&
             range.PhysicalStart < physicalStart + bytes) ||
            (Abi::Amd64PagerModuleBase < range.VirtualStart + range.Pages * 4096U &&
             range.VirtualStart < Abi::Amd64PagerModuleBase + bytes))
        {
            return false;
        }
    }
    // DirectUserMemoryAccess requires increasing virtual ranges. Keep the
    // initial stack last while inserting the lower module range before it.
    m_ranges[m_rangeCount] = m_ranges[m_rangeCount - 1U];
    m_ranges[m_rangeCount - 1U] = {Abi::Amd64PagerModuleBase, physicalStart, bytes / 4096U,
                                   Kernel::Memory::PagePermissions{Kernel::Memory::PagePermission::Read}.With(
                                       Kernel::Memory::PagePermission::User)};
    ++m_rangeCount;
    return true;
}

Amd64InitialUserImageError Amd64InitialUserImage::Prepare(const ::Asteria::Executable::Elf::Elf64ByteView image,
                                                          Foundation::UInt8* const storage,
                                                          const Foundation::UInt64 physicalStart,
                                                          const Foundation::Size storageBytes) noexcept
{
    if (m_attempted)
    {
        return Amd64InitialUserImageError::AlreadyAttempted;
    }
    m_attempted = true;
    const auto start = reinterpret_cast<Foundation::UIntPointer>(storage);
    const auto source = reinterpret_cast<Foundation::UIntPointer>(image.Data);
    const auto owner = reinterpret_cast<Foundation::UIntPointer>(this);
    constexpr Foundation::UInt64 maximum{~Foundation::UInt64{0U}};
    if ((storage == nullptr) || (storageBytes != StorageBytes) || ((start & 0xFFFU) != 0U) ||
        ((physicalStart & 0xFFFU) != 0U) || (physicalStart == 0U) || (physicalStart > maximum - StorageBytes) ||
        (start > maximum - StorageBytes) || (source > maximum - image.SizeBytes) ||
        ((source < start + StorageBytes) && (start < source + image.SizeBytes)) ||
        ((owner < start + StorageBytes) && (start < owner + sizeof(*this))))
    {
        return Amd64InitialUserImageError::InvalidStorage;
    }
    const auto decoded = ::Asteria::Executable::Elf::Elf64Image::DecodeLoadPlan(
        image, {4096U, 2U * 1024U * 1024U, MaximumInitialUserSegments}, m_plan);
    if (!decoded.HasValue())
    {
        return Amd64InitialUserImageError::ElfRejected;
    }
    if ((decoded.GetValue().Machine != 62U) || (decoded.GetValue().Flags != 0U))
    {
        return Amd64InitialUserImageError::UnsupportedMachine;
    }
    m_entry = decoded.GetValue().EntryAddress;
    const auto error = ValidateSegments(physicalStart);
    if (error != Amd64InitialUserImageError::None)
    {
        return error;
    }
    Stage(image, storage);
    m_ready = true;
    return Amd64InitialUserImageError::None;
}
} // namespace Asteria::Platform::Amd64::Executable
