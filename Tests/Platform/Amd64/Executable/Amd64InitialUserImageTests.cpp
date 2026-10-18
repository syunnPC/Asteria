// SPDX-License-Identifier: Apache-2.0
#include "Platform/Amd64/Executable/Amd64InitialUserImage.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace
{
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt8;
using Asteria::Foundation::UInt64;
using Asteria::Platform::Amd64::Executable::Amd64InitialUserImage;
using Asteria::Platform::Amd64::Executable::Amd64InitialUserImageError;
alignas(4096U) UInt8 g_storage[Asteria::Platform::Amd64::Executable::InitialUserStoragePages * 4096U]{};
UInt8 g_image[320U]{};

void Put(const Size offset, UInt64 value, const Size size) noexcept
{
    for (Size i{0U}; i < size; ++i)
    {
        g_image[offset + i] = static_cast<UInt8>(value & 0xFFU);
        value >>= 8U;
    }
}

void MakeImage() noexcept
{
    for (auto& byte : g_image)
    {
        byte = 0U;
    }
    Put(0U, 0x464C457FU, 4U);
    Put(4U, 2U, 1U);
    Put(5U, 1U, 1U);
    Put(6U, 1U, 1U);
    Put(16U, 2U, 2U);
    Put(18U, 62U, 2U);
    Put(20U, 1U, 4U);
    Put(24U, 0x40000000U, 8U);
    Put(32U, 64U, 8U);
    Put(52U, 64U, 2U);
    Put(54U, 56U, 2U);
    Put(56U, 2U, 2U);
    for (Size index{0U}; index < 2U; ++index)
    {
        const Size base = 64U + index * 56U;
        Put(base, 1U, 4U);
        Put(base + 4U, index == 0U ? 5U : 6U, 4U);
        Put(base + 8U, index == 0U ? 256U : 288U, 8U);
        Put(base + 16U, 0x40000000U + index * 4096U, 8U);
        Put(base + 32U, 8U, 8U);
        Put(base + 40U, index == 0U ? 8U : 8192U, 8U);
        Put(base + 48U, 1U, 8U);
    }
    Put(256U, 0x90909090U, 4U);
    Put(288U, 0x12345678U, 4U);
}

/// Appends a third, empty PT_LOAD record (p_filesz = p_memsz = 0) at `address`,
/// a valid ELF case that describes no memory.
void AddEmptySegment(const UInt64 address) noexcept
{
    constexpr Size base{64U + 2U * 56U};
    Put(56U, 3U, 2U);
    Put(base, 1U, 4U);
    Put(base + 4U, 4U, 4U);
    Put(base + 8U, 0U, 8U);
    Put(base + 16U, address, 8U);
    Put(base + 32U, 0U, 8U);
    Put(base + 40U, 0U, 8U);
    Put(base + 48U, 1U, 8U);
}

[[nodiscard]] Amd64InitialUserImageError Prepare(Amd64InitialUserImage& owner) noexcept
{
    return owner.Prepare({g_image, sizeof(g_image)}, g_storage, 0x200000U, sizeof(g_storage));
}

/// The owner publishes exactly the two file-backed segments and the stack,
/// each range at least one page (Frame and MemoryObject reject empty ones).
[[nodiscard]] bool PublishesOnlyNonemptyRanges(const Amd64InitialUserImage& owner) noexcept
{
    constexpr UInt64 starts[]{0x40000000U, 0x40001000U, 0x50001000U};
    constexpr UInt64 physical[]{0x200000U, 0x201000U, 0x203000U};
    constexpr UInt64 pages[]{1U, 2U, 4U};
    if (owner.GetRangeCount() != 3U)
    {
        return false;
    }
    for (Size index{0U}; index < 3U; ++index)
    {
        const auto* const range = owner.GetRange(index);
        if ((range == nullptr) || (range->VirtualStart != starts[index]) || (range->PhysicalStart != physical[index]) ||
            (range->Pages != pages[index]))
        {
            return false;
        }
    }
    return true;
}
} // namespace

int main()
{
    TestContext runner{"Amd64InitialUserImageTests"};
    MakeImage();
    for (auto& byte : g_storage)
    {
        byte = 0xA5U;
    }
    Amd64InitialUserImage owner;
    ASTERIA_EXPECT(runner, "valid ELF staged", Prepare(owner) == Amd64InitialUserImageError::None);
    ASTERIA_EXPECT(runner, "two segments and guarded stack retained", owner.GetRangeCount() == 3U,
                   owner.GetEntry() == 0x40000000U);
    ASTERIA_EXPECT(runner, "file contents placed in distinct physical slices", g_storage[0U] == 0x90U,
                   g_storage[4096U] == 0x78U);
    ASTERIA_EXPECT(runner, "BSS, padding and stack cleared", g_storage[4096U + 8U] == 0U, g_storage[12287U] == 0U,
                   g_storage[16383U] == 0U);
    ASTERIA_EXPECT(runner, "range access is bounded", owner.GetRange(0U) != nullptr, owner.GetRange(3U) == nullptr);
    ASTERIA_EXPECT(runner, "published storage cannot be staged twice",
                   Prepare(owner) == Amd64InitialUserImageError::AlreadyAttempted);
    ASTERIA_EXPECT(runner, "invalid module extents leave the image unchanged", !owner.MapModules(0x800001U, 4096U),
                   !owner.MapModules(0x800000U, 4097U), !owner.MapModules(~UInt64{4095U}, 4096U),
                   !owner.MapModules(0x200000U, 4096U),
                   !owner.MapModules(0x800000U, Asteria::Abi::PagerModuleMaximumBytes + 4096U),
                   owner.GetRangeCount() == 3U);
    ASTERIA_EXPECT(runner, "module slice maps once without staging its bytes", owner.MapModules(0x800000U, 8192U),
                   !owner.MapModules(0x900000U, 4096U), owner.GetRangeCount() == 4U);
    const auto* const modules = owner.GetRange(2U);
    const auto* const stack = owner.GetRange(3U);
    const Asteria::Kernel::Memory::PagePermissions modulePermissions{
        Asteria::Kernel::Memory::PagePermissions{Asteria::Kernel::Memory::PagePermission::Read}.With(
            Asteria::Kernel::Memory::PagePermission::User)};
    ASTERIA_EXPECT(runner, "module mapping is read-only and remains ordered before the stack", modules != nullptr,
                   stack != nullptr, modules->VirtualStart == Asteria::Abi::Amd64PagerModuleBase,
                   modules->PhysicalStart == 0x800000U, modules->Pages == 2U,
                   modules->Permissions.GetBits() == modulePermissions.GetBits(),
                   stack->VirtualStart == Asteria::Platform::Amd64::Executable::InitialUserStackStart);
    MakeImage();
    Put(18U, 50U, 2U);
    Amd64InitialUserImage wrongMachine;
    ASTERIA_EXPECT(runner, "IA-64 ELF rejected",
                   Prepare(wrongMachine) == Amd64InitialUserImageError::UnsupportedMachine);
    MakeImage();
    Put(68U, 7U, 4U);
    Amd64InitialUserImage writableCode;
    ASTERIA_EXPECT(runner, "W+X rejected", Prepare(writableCode) == Amd64InitialUserImageError::ElfRejected);
    MakeImage();
    Put(24U, 0x40001000U, 8U);
    Amd64InitialUserImage dataEntry;
    ASTERIA_EXPECT(runner, "entry in nonexecuting data rejected",
                   Prepare(dataEntry) == Amd64InitialUserImageError::InvalidEntry);
    MakeImage();
    Put(80U, 0xFFFF800000000000ULL, 8U);
    Put(24U, 0xFFFF800000000000ULL, 8U);
    Amd64InitialUserImage kernelImage;
    ASTERIA_EXPECT(runner, "kernel-address load rejected",
                   Prepare(kernelImage) == Amd64InitialUserImageError::InvalidSegment);
    MakeImage();
    Put(160U, Asteria::Platform::Amd64::Executable::InitialUserStoragePages * 4096U, 8U);
    Amd64InitialUserImage tooLarge;
    ASTERIA_EXPECT(runner, "image must leave storage for user stack",
                   Prepare(tooLarge) == Amd64InitialUserImageError::StorageExhausted);
    // An empty PT_LOAD describes no memory: it receives no range, no storage
    // and no address-window check, wherever it claims to be.
    constexpr UInt64 emptySegmentAddresses[]{0x40002000U, 0x1000U, 0xFFFF800000000000ULL};
    for (const UInt64 address : emptySegmentAddresses)
    {
        MakeImage();
        AddEmptySegment(address);
        for (auto& byte : g_storage)
        {
            byte = 0xA5U;
        }
        Amd64InitialUserImage withEmpty;
        ASTERIA_EXPECT(runner, "an empty PT_LOAD segment is skipped",
                       Prepare(withEmpty) == Amd64InitialUserImageError::None, PublishesOnlyNonemptyRanges(withEmpty),
                       g_storage[0U] == 0x90U, g_storage[4096U] == 0x78U, g_storage[16383U] == 0U);
    }
    MakeImage();
    Amd64InitialUserImage wrongStorage;
    ASTERIA_EXPECT(runner, "unaligned physical storage rejected",
                   wrongStorage.Prepare({g_image, sizeof(g_image)}, g_storage, 0x200001U, sizeof(g_storage)) ==
                       Amd64InitialUserImageError::InvalidStorage);
    Amd64InitialUserImage alias;
    ASTERIA_EXPECT(runner, "source/destination alias rejected before zeroing",
                   alias.Prepare({g_storage, sizeof(g_image)}, g_storage, 0x200000U, sizeof(g_storage)) ==
                       Amd64InitialUserImageError::InvalidStorage);
    return runner.Finish();
}
