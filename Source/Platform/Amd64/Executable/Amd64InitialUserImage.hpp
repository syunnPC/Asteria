// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_EXECUTABLE_AMD64_INITIAL_USER_IMAGE_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_EXECUTABLE_AMD64_INITIAL_USER_IMAGE_HPP_INCLUDED

#include "Abi/PagerBootstrap.hpp"
#include "Executable/Elf/Elf64Image.hpp"
#include "Kernel/Memory/MemoryTypes.hpp"

namespace Asteria::Platform::Amd64::Executable
{
inline constexpr Foundation::UInt64 InitialUserImageStart{0x4000'0000U};
inline constexpr Foundation::UInt64 InitialUserImageEnd{0x4100'0000U};
inline constexpr Foundation::UInt64 InitialUserStackStart{0x5000'1000U};
inline constexpr Foundation::UInt64 InitialUserStackPages{4U};
// Each image's staging: its segments and its stack. Module bytes stay in
// Their loader slice and consume no staging pages.
inline constexpr Foundation::Size InitialUserStoragePages{128U};
/// The initial image's lazily backed region: nothing maps it until the image touches a page and the
/// boot pager resolves the fault. Each page starts with Abi::LazyPagePattern and its page index.
inline constexpr Foundation::UInt64 InitialUserLazyStart{0x4800'0000U};
inline constexpr Foundation::UInt64 InitialUserLazyPages{2U};
static_assert((InitialUserLazyStart >= InitialUserImageEnd) &&
              (InitialUserLazyStart + (InitialUserLazyPages * 4096U) <= InitialUserStackStart - 4096U));
/// The window of the boot pager's own space that nothing maps at boot, where it maps the memory it
/// Creates: one Frame run, as large as a component segment.
inline constexpr Foundation::UInt64 InitialUserMappingWindowStart{0x4C00'0000U};
// A 64-page virtual staging span covers the 40-page component segment ceiling; no physical pages
// are reserved by this span.
inline constexpr Foundation::UInt64 InitialUserMappingWindowPages{64U};
static_assert((InitialUserMappingWindowStart >= InitialUserLazyStart + (InitialUserLazyPages * 4096U)) &&
              (InitialUserMappingWindowStart + (InitialUserMappingWindowPages * 4096U) <=
               InitialUserStackStart - 4096U));
static_assert(Abi::Amd64PagerModuleBase >= InitialUserImageEnd);
static_assert(Abi::Amd64PagerModuleBase + Abi::PagerModuleMaximumBytes <= InitialUserLazyStart);
static_assert(Abi::Amd64PagerModuleBase + Abi::PagerModuleMaximumBytes <= InitialUserMappingWindowStart);
static_assert(Abi::Amd64PagerModuleBase + Abi::PagerModuleMaximumBytes <= InitialUserStackStart - 4096U);
inline constexpr Foundation::Size MaximumInitialUserSegments{8U};

struct Amd64InitialUserRange
{
    Foundation::UInt64 VirtualStart;
    Foundation::UInt64 PhysicalStart;
    Foundation::UInt64 Pages;
    Kernel::Memory::PagePermissions Permissions;
};

enum class Amd64InitialUserImageError : Foundation::UInt8
{
    None,
    AlreadyAttempted,
    InvalidStorage,
    ElfRejected,
    UnsupportedMachine,
    InvalidSegment,
    StorageExhausted,
    InvalidEntry,
};

/// One-shot image staging owner. The boot allocator supplies exactly
/// InitialUserStoragePages reserved RAM pages through a temporary writable
/// identity alias. Neither these pages nor their guards belong to the
/// supervisor seed mapping. After staging, only the common Frame/MemoryObject
/// mapper may publish user leaves.
class Amd64InitialUserImage final
{
public:
    constexpr Amd64InitialUserImage() noexcept = default;
    Amd64InitialUserImage(const Amd64InitialUserImage&) = delete;
    Amd64InitialUserImage& operator=(const Amd64InitialUserImage&) = delete;

    [[nodiscard]] Amd64InitialUserImageError Prepare(::Asteria::Executable::Elf::Elf64ByteView image,
                                                     Foundation::UInt8* storage, Foundation::UInt64 physicalStart,
                                                     Foundation::Size storageBytes) noexcept;

    /// Adds the validated, page-isolated loader slice without copying it.
    [[nodiscard]] bool MapModules(Foundation::UInt64 physicalStart, Foundation::UInt64 bytes) noexcept;

    [[nodiscard]] constexpr bool IsReady() const noexcept
    {
        return m_ready;
    }
    [[nodiscard]] constexpr Foundation::UInt64 GetEntry() const noexcept
    {
        return m_ready ? m_entry : 0U;
    }
    [[nodiscard]] constexpr Foundation::Size GetRangeCount() const noexcept
    {
        return m_ready ? m_rangeCount : 0U;
    }
    [[nodiscard]] constexpr const Amd64InitialUserRange* GetRange(const Foundation::Size index) const noexcept
    {
        return m_ready && index < m_rangeCount ? &m_ranges[index] : nullptr;
    }

private:
    [[nodiscard]] Amd64InitialUserImageError ValidateSegments(Foundation::UInt64 physicalStart) noexcept;
    void Stage(::Asteria::Executable::Elf::Elf64ByteView image, Foundation::UInt8* storage) noexcept;

    ::Asteria::Executable::Elf::Elf64LoadPlan m_plan{};
    Amd64InitialUserRange m_ranges[MaximumInitialUserSegments + 2U]{};
    Foundation::Size m_rangeCount{0U};
    Foundation::UInt64 m_entry{0U};
    bool m_attempted{false};
    bool m_ready{false};
};
} // Namespace Asteria::Platform::Amd64::Executable

#endif // ASTERIA_PLATFORM_AMD64_EXECUTABLE_AMD64_INITIAL_USER_IMAGE_HPP_INCLUDED
