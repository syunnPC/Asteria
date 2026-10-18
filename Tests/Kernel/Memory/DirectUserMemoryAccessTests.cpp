// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Kernel/Memory/DirectUserMemoryAccess.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"

#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
using Kernel::Memory::AddressSpaceId;
using Kernel::Memory::DirectUserMemoryAccess;
using Kernel::Memory::DirectUserMemoryAccessError;
using Kernel::Memory::DirectUserMemoryMapping;
using Kernel::Memory::DirectUserMemoryReservationGenerationPolicy;
using Kernel::Memory::FrameKind;
using Kernel::Memory::MappingPageSizeBytes;
using Kernel::Memory::MaximumDirectUserMemoryMappings;
using Kernel::Memory::PageCount;
using Kernel::Memory::PagePermission;
using Kernel::Memory::PagePermissions;
using Kernel::Memory::ReadOnlyKernelBuffer;
using Kernel::Memory::UserMemoryAccessError;
using Kernel::Memory::UserMemoryRange;
using Kernel::Memory::VirtualAddress;
using Kernel::Memory::WritableKernelBuffer;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectRegistry;
using Platform::Synchronization::SpinLock;

constexpr Foundation::UInt64 UserBase{0x1'0000U};
constexpr Foundation::Size PageSize = static_cast<Foundation::Size>(MappingPageSizeBytes);

alignas(4096) Foundation::UInt8 g_firstPage[PageSize]{};
alignas(4096) Foundation::UInt8 g_secondPage[PageSize]{};
alignas(4096) Foundation::UInt8 g_thirdPage[PageSize]{};
alignas(4096) Foundation::UInt8 g_twoPages[PageSize * 2U]{};
alignas(4096) Foundation::UInt8 g_noncontiguousPages[PageSize * 3U]{};
SpinLock g_registrySynchronization;
Asteria::TestSupport::TestObjectRegistry g_registry{g_registrySynchronization};
Asteria::TestSupport::TestObjectRegistry g_foreignRegistry{g_registrySynchronization};
DirectUserMemoryMapping g_capacityMappings[MaximumDirectUserMemoryMappings]{};
DirectUserMemoryAccess g_capacityAccess;

static_assert(MaximumDirectUserMemoryMappings == 10U);

[[nodiscard]] constexpr PagePermissions ReadUser() noexcept
{
    return PagePermissions{PagePermission::Read}.With(PagePermission::User);
}

[[nodiscard]] constexpr PagePermissions ReadWriteUser() noexcept
{
    return ReadUser().With(PagePermission::Write);
}

[[nodiscard]] constexpr DirectUserMemoryMapping MakeMapping(const Foundation::UInt64 userStart,
                                                            Foundation::UInt8* const directStart,
                                                            const PagePermissions permissions = ReadWriteUser(),
                                                            const Foundation::UInt64 pages = 1U,
                                                            const FrameKind kind = FrameKind::Ram) noexcept
{
    return DirectUserMemoryMapping{
        .UserStart = VirtualAddress{userStart},
        .DirectStart = directStart,
        .Pages = PageCount{pages},
        .Permissions = permissions,
        .Kind = kind,
    };
}

[[nodiscard]] constexpr AddressSpaceId MakeAddressSpace() noexcept
{
    return AddressSpaceId{g_registry, ObjectId{41U}};
}

[[nodiscard]] constexpr AddressSpaceId MakeForeignAddressSpace() noexcept
{
    return AddressSpaceId{g_foreignRegistry, ObjectId{41U}};
}

[[nodiscard]] UserMemoryRange MakeRange(const Foundation::UInt64 start, const Foundation::Size size) noexcept
{
    const auto result = UserMemoryRange::Create(start, size);
    return result.HasValue() ? result.GetValue() : UserMemoryRange{};
}

void Fill(Foundation::UInt8* const bytes, const Foundation::Size size, const Foundation::UInt8 value) noexcept
{
    for (Foundation::Size index{0U}; index < size; ++index)
    {
        bytes[index] = value;
    }
}

[[nodiscard]] bool EveryByteEquals(const Foundation::UInt8* const bytes, const Foundation::Size size,
                                   const Foundation::UInt8 value) noexcept
{
    for (Foundation::Size index{0U}; index < size; ++index)
    {
        if (bytes[index] != value)
        {
            return false;
        }
    }
    return true;
}

void TestReadinessAndInitialization(TestContext& runner) noexcept
{
    DirectUserMemoryAccess access;
    Foundation::UInt8 destination[4]{};
    Fill(destination, sizeof(destination), 0xA5U);
    const UserMemoryRange byteRange = MakeRange(UserBase, 1U);
    const UserMemoryRange emptyRange = MakeRange(0U, 0U);
    ASTERIA_EXPECT(runner, "a default backend has no published identity", !access.IsReady(),
                   !access.GetAddressSpaceId().IsValid());
    ASTERIA_EXPECT(runner, "a not-ready copy leaves the kernel destination unchanged",
                   access.CopyFromUser(byteRange, WritableKernelBuffer{destination, 1U}) ==
                       UserMemoryAccessError::NotReady,
                   EveryByteEquals(destination, sizeof(destination), 0xA5U));
    ASTERIA_EXPECT(runner, "a size-mismatched copy leaves destination bytes unchanged",
                   access.CopyFromUser(MakeRange(UserBase, 2U), WritableKernelBuffer{destination, 1U}) ==
                       UserMemoryAccessError::SizeMismatch,
                   EveryByteEquals(destination, sizeof(destination), 0xA5U));
    ASTERIA_EXPECT(runner, "a null-buffer rejection leaves unrelated destination bytes unchanged",
                   access.CopyFromUser(MakeRange(UserBase, 2U), WritableKernelBuffer{nullptr, 2U}) ==
                       UserMemoryAccessError::NullKernelBuffer,
                   EveryByteEquals(destination, sizeof(destination), 0xA5U));
    ASTERIA_EXPECT(runner, "a noncanonical empty buffer leaves destination bytes unchanged",
                   access.CopyFromUser(emptyRange, WritableKernelBuffer{destination, 0U}) ==
                       UserMemoryAccessError::NonCanonicalEmptyBuffer,
                   EveryByteEquals(destination, sizeof(destination), 0xA5U));
    const Foundation::UIntPointer maximumPointer = ~Foundation::UIntPointer{0U};
    Foundation::UInt8* const overflowingKernelBuffer = reinterpret_cast<Foundation::UInt8*>(maximumPointer - 1U);
    ASTERIA_EXPECT(runner, "an overflowing kernel buffer is rejected without touching bytes",
                   access.CopyFromUser(MakeRange(UserBase, 4U), WritableKernelBuffer{overflowingKernelBuffer, 4U}) ==
                       UserMemoryAccessError::KernelBufferOverflow,
                   EveryByteEquals(destination, sizeof(destination), 0xA5U));
    ASTERIA_EXPECT(runner, "canonical empty reads succeed before readiness",
                   access.CopyFromUser(emptyRange, WritableKernelBuffer{nullptr, 0U}) == UserMemoryAccessError::None);
    ASTERIA_EXPECT(runner, "empty write preparation still requires readiness",
                   access.PrepareCopyToUser(emptyRange).GetError() == UserMemoryAccessError::NotReady);

    DirectUserMemoryMapping candidate = MakeMapping(UserBase, g_firstPage);
    ASTERIA_EXPECT(runner, "an invalid address-space identity is rejected",
                   access.Initialize(AddressSpaceId{}, &candidate, 1U) ==
                       DirectUserMemoryAccessError::InvalidAddressSpace);
    ASTERIA_EXPECT(runner, "a null mapping list is rejected",
                   access.Initialize(MakeAddressSpace(), nullptr, 1U) ==
                       DirectUserMemoryAccessError::InvalidMappingList);
    ASTERIA_EXPECT(runner, "an empty mapping list is rejected",
                   access.Initialize(MakeAddressSpace(), &candidate, 0U) ==
                       DirectUserMemoryAccessError::InvalidMappingList);
    ASTERIA_EXPECT(runner, "mapping capacity is enforced before reading the list",
                   access.Initialize(MakeAddressSpace(), &candidate, MaximumDirectUserMemoryMappings + 1U) ==
                       DirectUserMemoryAccessError::MappingCapacityExceeded);

    candidate.Pages = PageCount{0U};
    ASTERIA_EXPECT(runner, "zero-page mappings are rejected",
                   access.Initialize(MakeAddressSpace(), &candidate, 1U) ==
                       DirectUserMemoryAccessError::InvalidMapping);
    candidate = MakeMapping(UserBase + 1U, g_firstPage);
    ASTERIA_EXPECT(runner, "unaligned user mappings are rejected",
                   access.Initialize(MakeAddressSpace(), &candidate, 1U) ==
                       DirectUserMemoryAccessError::InvalidMapping);
    candidate = MakeMapping(UserBase, &g_firstPage[1]);
    ASTERIA_EXPECT(runner, "unaligned direct mappings are rejected",
                   access.Initialize(MakeAddressSpace(), &candidate, 1U) ==
                       DirectUserMemoryAccessError::InvalidMapping);
    candidate = MakeMapping(UserBase, nullptr);
    ASTERIA_EXPECT(runner, "null direct mappings are rejected",
                   access.Initialize(MakeAddressSpace(), &candidate, 1U) ==
                       DirectUserMemoryAccessError::InvalidMapping);
    const Foundation::UIntPointer lastPageStart = maximumPointer - (maximumPointer % MappingPageSizeBytes);
    candidate = MakeMapping(UserBase, reinterpret_cast<Foundation::UInt8*>(lastPageStart));
    ASTERIA_EXPECT(runner, "a page-aligned direct extent may not cross UIntPointer maximum",
                   (lastPageStart % MappingPageSizeBytes) == 0U,
                   access.Initialize(MakeAddressSpace(), &candidate, 1U) ==
                       DirectUserMemoryAccessError::InvalidMapping);
    candidate = MakeMapping((~Foundation::UInt64{0U}) - (MappingPageSizeBytes - 1U), g_firstPage);
    ASTERIA_EXPECT(runner, "wrapping user extents are rejected",
                   access.Initialize(MakeAddressSpace(), &candidate, 1U) ==
                       DirectUserMemoryAccessError::InvalidMapping);
    candidate =
        MakeMapping(UserBase, g_firstPage, ReadWriteUser(), (~Foundation::UInt64{0U} / MappingPageSizeBytes) + 1U);
    ASTERIA_EXPECT(runner, "page-sized extent multiplication may not overflow",
                   access.Initialize(MakeAddressSpace(), &candidate, 1U) ==
                       DirectUserMemoryAccessError::InvalidMapping);
    candidate = MakeMapping(UserBase, g_firstPage, PagePermissions{static_cast<PagePermission>(0x80U)});
    ASTERIA_EXPECT(runner, "unknown page-permission bits are rejected",
                   access.Initialize(MakeAddressSpace(), &candidate, 1U) ==
                       DirectUserMemoryAccessError::InvalidMapping);
    candidate = MakeMapping(UserBase, g_firstPage, PagePermissions{PagePermission::User});
    ASTERIA_EXPECT(runner, "mappings without read permission are rejected",
                   access.Initialize(MakeAddressSpace(), &candidate, 1U) ==
                       DirectUserMemoryAccessError::InvalidMapping);
    candidate = MakeMapping(UserBase, g_firstPage, PagePermissions{PagePermission::Read});
    ASTERIA_EXPECT(runner, "supervisor-only mappings are rejected",
                   access.Initialize(MakeAddressSpace(), &candidate, 1U) ==
                       DirectUserMemoryAccessError::InvalidMapping);
    candidate = MakeMapping(UserBase, g_firstPage, ReadWriteUser(), 1U, FrameKind::Device);
    ASTERIA_EXPECT(runner, "device mappings are rejected",
                   access.Initialize(MakeAddressSpace(), &candidate, 1U) ==
                       DirectUserMemoryAccessError::InvalidMapping);

    const DirectUserMemoryMapping unordered[2]{
        MakeMapping(UserBase + MappingPageSizeBytes, g_secondPage),
        MakeMapping(UserBase, g_firstPage),
    };
    ASTERIA_EXPECT(runner, "mapping order is validated before publication",
                   access.Initialize(MakeAddressSpace(), unordered, 2U) ==
                       DirectUserMemoryAccessError::MappingsNotOrdered);
    const DirectUserMemoryMapping overlapping[2]{
        MakeMapping(UserBase, g_twoPages, ReadWriteUser(), 2U),
        MakeMapping(UserBase + MappingPageSizeBytes, g_secondPage),
    };
    ASTERIA_EXPECT(runner, "overlapping user intervals are rejected",
                   access.Initialize(MakeAddressSpace(), overlapping, 2U) ==
                       DirectUserMemoryAccessError::MappingsOverlap);
    ASTERIA_EXPECT(runner, "all initialization failures leave the backend unpublished", !access.IsReady(),
                   !access.GetAddressSpaceId().IsValid());

    candidate = MakeMapping(UserBase, g_firstPage);
    ASTERIA_EXPECT(runner, "a valid direct RAM mapping publishes successfully",
                   access.Initialize(MakeAddressSpace(), &candidate, 1U) == DirectUserMemoryAccessError::None);
    candidate = MakeMapping(UserBase, g_secondPage, ReadUser());
    g_firstPage[0] = 0x39U;
    ASTERIA_EXPECT(runner, "published identity and copied mapping metadata are immutable", access.IsReady(),
                   access.GetAddressSpaceId().IsEqualTo(MakeAddressSpace()),
                   access.CopyFromUser(byteRange, WritableKernelBuffer{destination, 1U}) == UserMemoryAccessError::None,
                   destination[0] == 0x39U);
    ASTERIA_EXPECT(runner, "reinitialization cannot replace the bound identity",
                   access.Initialize(MakeForeignAddressSpace(), &candidate, 1U) ==
                       DirectUserMemoryAccessError::AlreadyInitialized,
                   access.GetAddressSpaceId().IsEqualTo(MakeAddressSpace()));
}

void TestCrossMappingCopies(TestContext& runner) noexcept
{
    Fill(g_noncontiguousPages, sizeof(g_noncontiguousPages), 0U);
    Foundation::UInt8* const firstDirect = g_noncontiguousPages;
    Foundation::UInt8* const secondDirect = &g_noncontiguousPages[PageSize * 2U];
    const DirectUserMemoryMapping mappings[2]{
        MakeMapping(UserBase, firstDirect),
        MakeMapping(UserBase + MappingPageSizeBytes, secondDirect),
    };
    DirectUserMemoryAccess access;
    const bool initialized = access.Initialize(MakeAddressSpace(), mappings, 2U) == DirectUserMemoryAccessError::None;

    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        firstDirect[PageSize - 8U + index] = static_cast<Foundation::UInt8>(0x10U + index);
        secondDirect[index] = static_cast<Foundation::UInt8>(0x20U + index);
    }
    const UserMemoryRange crossing = MakeRange(UserBase + MappingPageSizeBytes - 8U, 16U);
    Foundation::UInt8 copied[16]{};
    const UserMemoryAccessError copyError = access.CopyFromUser(crossing, WritableKernelBuffer{copied, sizeof(copied)});
    bool exactRead = initialized && (copyError == UserMemoryAccessError::None);
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        exactRead = exactRead && (copied[index] == static_cast<Foundation::UInt8>(0x10U + index)) &&
                    (copied[8U + index] == static_cast<Foundation::UInt8>(0x20U + index));
    }
    ASTERIA_EXPECT(runner, "reads span adjacent direct mappings exactly", exactRead);
    ASTERIA_EXPECT(runner, "adjacent user mappings may use noncontiguous direct RAM pages",
                   reinterpret_cast<Foundation::UIntPointer>(secondDirect) -
                           reinterpret_cast<Foundation::UIntPointer>(firstDirect) ==
                       static_cast<Foundation::UIntPointer>(PageSize * 2U));

    Foundation::UInt8 source[16]{};
    for (Foundation::Size index{0U}; index < sizeof(source); ++index)
    {
        source[index] = static_cast<Foundation::UInt8>(0x80U + index);
    }
    const auto prepared = access.PrepareCopyToUser(crossing);
    const UserMemoryAccessError commitError =
        prepared.HasValue() ? access.CommitCopyToUser(prepared.GetValue(), ReadOnlyKernelBuffer{source, sizeof(source)})
                            : prepared.GetError();
    bool exactWrite = commitError == UserMemoryAccessError::None;
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        exactWrite = exactWrite && (firstDirect[PageSize - 8U + index] == source[index]) &&
                     (secondDirect[index] == source[8U + index]);
    }
    ASTERIA_EXPECT(runner, "writes span adjacent direct mappings exactly", exactWrite);
}

void TestMaximumMappingCapacity(TestContext& runner) noexcept
{
    for (Foundation::Size index{0U}; index < MaximumDirectUserMemoryMappings; ++index)
    {
        g_capacityMappings[index] = MakeMapping(UserBase + (index * MappingPageSizeBytes), g_firstPage);
    }
    g_firstPage[0] = 0x5AU;
    Foundation::UInt8 copied{0U};
    const Foundation::UInt64 lastMappingStart =
        UserBase + ((MaximumDirectUserMemoryMappings - 1U) * MappingPageSizeBytes);
    ASTERIA_EXPECT(runner, "all ten direct user-memory mappings are accepted and reachable",
                   g_capacityAccess.Initialize(MakeAddressSpace(), g_capacityMappings,
                                               MaximumDirectUserMemoryMappings) == DirectUserMemoryAccessError::None,
                   g_capacityAccess.CopyFromUser(MakeRange(lastMappingStart, 1U), WritableKernelBuffer{&copied, 1U}) ==
                       UserMemoryAccessError::None,
                   copied == 0x5AU);
}

void TestAccessRejectionAtomicity(TestContext& runner) noexcept
{
    Fill(g_firstPage, PageSize, 0x11U);
    Fill(g_secondPage, PageSize, 0x22U);
    const DirectUserMemoryMapping mappings[2]{
        MakeMapping(UserBase, g_firstPage, ReadUser()),
        MakeMapping(UserBase + (2U * MappingPageSizeBytes), g_secondPage),
    };
    DirectUserMemoryAccess access;
    static_cast<void>(access.Initialize(MakeAddressSpace(), mappings, 2U));

    const UserMemoryRange acrossGap = MakeRange(UserBase + MappingPageSizeBytes - 8U, 16U);
    Foundation::UInt8 destination[16]{};
    Fill(destination, sizeof(destination), 0xA5U);
    ASTERIA_EXPECT(runner, "a gap rejects a read before modifying the kernel destination",
                   access.CopyFromUser(acrossGap, WritableKernelBuffer{destination, sizeof(destination)}) ==
                       UserMemoryAccessError::AccessDenied,
                   EveryByteEquals(destination, sizeof(destination), 0xA5U));
    ASTERIA_EXPECT(runner, "a gap rejects write preparation",
                   access.PrepareCopyToUser(acrossGap).GetError() == UserMemoryAccessError::AccessDenied);

    const UserMemoryRange readOnlyByte = MakeRange(UserBase, 1U);
    ASTERIA_EXPECT(runner, "a read-only mapping rejects write preparation",
                   access.PrepareCopyToUser(readOnlyByte).GetError() == UserMemoryAccessError::AccessDenied);
    const UserMemoryRange writableByte = MakeRange(UserBase + (2U * MappingPageSizeBytes), 1U);
    const auto writable = access.PrepareCopyToUser(writableByte);
    const Foundation::UInt8 replacement{0x77U};
    ASTERIA_EXPECT(runner, "rejected preparations publish no reservation or user byte", writable.HasValue(),
                   access.CommitCopyToUser(writable.GetValue(), ReadOnlyKernelBuffer{&replacement, 1U}) ==
                       UserMemoryAccessError::None,
                   g_secondPage[0] == replacement);
}

void TestReservationLifecycle(TestContext& runner) noexcept
{
    Fill(g_firstPage, PageSize, 0x33U);
    Fill(g_secondPage, PageSize, 0x44U);
    const DirectUserMemoryMapping firstMapping = MakeMapping(UserBase, g_firstPage);
    const DirectUserMemoryMapping secondMapping = MakeMapping(UserBase, g_secondPage);
    DirectUserMemoryAccess first;
    DirectUserMemoryAccess second;
    static_cast<void>(first.Initialize(MakeAddressSpace(), &firstMapping, 1U));
    static_cast<void>(second.Initialize(MakeForeignAddressSpace(), &secondMapping, 1U));
    const UserMemoryRange range = MakeRange(UserBase + 10U, 2U);

    const auto firstPrepared = first.PrepareCopyToUser(range);
    const auto contended = first.PrepareCopyToUser(range);
    ASTERIA_EXPECT(runner, "only one write reservation may be live", firstPrepared.HasValue(), !contended.HasValue(),
                   contended.GetError() == UserMemoryAccessError::ReservationExhausted);
    const Foundation::UInt8 source[2]{0xA1U, 0xB2U};
    ASTERIA_EXPECT(runner, "a wrong-sized commit is rejected before consuming its reservation",
                   first.CommitCopyToUser(firstPrepared.GetValue(), ReadOnlyKernelBuffer{source, 1U}) ==
                       UserMemoryAccessError::SizeMismatch);
    ASTERIA_EXPECT(runner, "an exact commit publishes all reserved bytes",
                   first.CommitCopyToUser(firstPrepared.GetValue(), ReadOnlyKernelBuffer{source, sizeof(source)}) ==
                       UserMemoryAccessError::None,
                   g_firstPage[10] == source[0], g_firstPage[11] == source[1]);
    ASTERIA_EXPECT(runner, "a committed reservation cannot be replayed",
                   first.CommitCopyToUser(firstPrepared.GetValue(), ReadOnlyKernelBuffer{source, sizeof(source)}) ==
                       UserMemoryAccessError::StaleReservation);

    const auto secondPrepared = second.PrepareCopyToUser(range);
    ASTERIA_EXPECT(runner, "foreign cancellation and commit tokens are rejected", secondPrepared.HasValue(),
                   first.CancelCopyToUser(secondPrepared.GetValue()) == UserMemoryAccessError::ForeignReservation,
                   first.CommitCopyToUser(secondPrepared.GetValue(), ReadOnlyKernelBuffer{source, sizeof(source)}) ==
                       UserMemoryAccessError::ForeignReservation);
    static_cast<void>(second.CancelCopyToUser(secondPrepared.GetValue()));

    const auto cancelPrepared = first.PrepareCopyToUser(range);
    ASTERIA_EXPECT(runner, "cancellation invalidates exactly one reservation generation", cancelPrepared.HasValue(),
                   first.CancelCopyToUser(cancelPrepared.GetValue()) == UserMemoryAccessError::None,
                   first.CancelCopyToUser(cancelPrepared.GetValue()) == UserMemoryAccessError::StaleReservation);

    const auto current = first.PrepareCopyToUser(range);
    ASTERIA_EXPECT(runner, "a stale generation cannot consume the current reservation", current.HasValue(),
                   current.GetValue().GetGeneration() != cancelPrepared.GetValue().GetGeneration(),
                   first.CancelCopyToUser(cancelPrepared.GetValue()) == UserMemoryAccessError::StaleReservation,
                   first.CommitCopyToUser(current.GetValue(), ReadOnlyKernelBuffer{source, sizeof(source)}) ==
                       UserMemoryAccessError::None);
}

void TestEmptyOperationsAndGenerationBoundary(TestContext& runner) noexcept
{
    const DirectUserMemoryMapping mapping = MakeMapping(UserBase, g_thirdPage);
    DirectUserMemoryAccess access;
    static_cast<void>(access.Initialize(MakeAddressSpace(), &mapping, 1U));
    const UserMemoryRange empty = MakeRange(0U, 0U);
    ASTERIA_EXPECT(runner, "the canonical empty read succeeds",
                   access.CopyFromUser(empty, WritableKernelBuffer{nullptr, 0U}) == UserMemoryAccessError::None);
    ASTERIA_EXPECT(runner, "an empty read requires a null kernel buffer",
                   access.CopyFromUser(empty, WritableKernelBuffer{g_thirdPage, 0U}) ==
                       UserMemoryAccessError::NonCanonicalEmptyBuffer);

    const auto prepared = access.PrepareCopyToUser(empty);
    ASTERIA_EXPECT(runner, "an empty reservation survives shape rejection and commits canonically", prepared.HasValue(),
                   prepared.GetValue().GetRange().IsEmpty(),
                   access.CommitCopyToUser(prepared.GetValue(), ReadOnlyKernelBuffer{g_thirdPage, 0U}) ==
                       UserMemoryAccessError::NonCanonicalEmptyBuffer,
                   access.CommitCopyToUser(prepared.GetValue(), ReadOnlyKernelBuffer{nullptr, 0U}) ==
                       UserMemoryAccessError::None);

    Foundation::UInt64 next{77U};
    const bool startsAtOne = DirectUserMemoryReservationGenerationPolicy::TryAdvance(0U, next) && (next == 1U);
    next = 99U;
    const bool reachesMaximum = DirectUserMemoryReservationGenerationPolicy::TryAdvance(
                                    DirectUserMemoryReservationGenerationPolicy::Maximum - 1U, next) &&
                                (next == DirectUserMemoryReservationGenerationPolicy::Maximum);
    const bool rejectsWrap = !DirectUserMemoryReservationGenerationPolicy::TryAdvance(
                                 DirectUserMemoryReservationGenerationPolicy::Maximum, next) &&
                             (next == DirectUserMemoryReservationGenerationPolicy::Maximum);
    ASTERIA_EXPECT(runner, "reservation generations reach UInt64 maximum and never wrap valid", startsAtOne,
                   reachesMaximum, rejectsWrap);
}
} // namespace Asteria::Test

int main()
{
    TestContext runner{"DirectUserMemoryAccessTests"};
    Asteria::Test::TestReadinessAndInitialization(runner);
    Asteria::Test::TestCrossMappingCopies(runner);
    Asteria::Test::TestMaximumMappingCapacity(runner);
    Asteria::Test::TestAccessRejectionAtomicity(runner);
    Asteria::Test::TestReservationLifecycle(runner);
    Asteria::Test::TestEmptyOperationsAndGenerationBoundary(runner);
    return runner.Finish();
}
