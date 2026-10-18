// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <cstring>
#include <new>
#include <type_traits>

#include "Boot/BootInfo.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Memory/AddressSpace.hpp"
#include "Kernel/Memory/BootstrapPageAllocator.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Platform/Ia64/Executable/Ia64InitialUserImageMemoryOwner.hpp"
#include "Platform/Ia64/Ia64UserVirtualLayout.hpp"

#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace
{
constexpr Asteria::Foundation::Size TestImageSizeBytes{0x5000U};
constexpr Asteria::Foundation::Size TestBackingPageCount{184U};
constexpr Asteria::Foundation::Size TestBackingSizeBytes{TestBackingPageCount * 4096U};
} // namespace

extern "C"
{
alignas(4096U) Asteria::Foundation::UInt8 AsteriaIa64InitialUserImageMemoryOwnerTestStorage[TestBackingSizeBytes]{};
}

namespace Asteria::Test
{
namespace
{
[[nodiscard]] Boot::BootFileRegion SourceRegion() noexcept
{
    Boot::BootFileRegion region{};
    region.PhysicalStart = reinterpret_cast<Foundation::UInt64>(AsteriaIa64InitialUserImageMemoryOwnerTestStorage);
    region.PageCount = (20480U + 4095U) / 4096U;
    return region;
}

using Kernel::Memory::BootstrapPageAllocator;
using Kernel::Memory::BootstrapPageAllocatorError;
using Kernel::Memory::MemoryObject;
using Kernel::Memory::MemoryObjectState;
using Kernel::Memory::PagePermission;
using Kernel::Object::IKernelObject;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectPin;
using Kernel::Object::ObjectPinError;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectRegistryError;
using Kernel::Object::ObjectType;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Resource::ResourceDomain;
using Kernel::Resource::ResourceDomainError;
using Kernel::Resource::ResourceDomainLedger;
using Kernel::Resource::ResourceDomainLimits;
using Kernel::Resource::ResourceDomainObjectId;
using Platform::Ia64::Executable::Ia64InitialUserImageGuardPolicy;
using Platform::Ia64::Executable::Ia64InitialUserImageMemoryAccessError;
using Platform::Ia64::Executable::Ia64InitialUserImageMemoryOwner;
using Platform::Ia64::Executable::Ia64InitialUserImageMemoryOwnerError;
using Platform::Ia64::Executable::Ia64InitialUserImageMemoryOwnerState;
using Platform::Ia64::Executable::Ia64InitialUserImageMemoryPurpose;
using Platform::Ia64::Executable::Ia64InitialUserImageOwner;
using Platform::Synchronization::SpinLock;

constexpr Foundation::Size ProgramHeaderTableOffset{64U};
constexpr Foundation::Size ProgramHeaderSizeBytes{56U};
constexpr Foundation::UInt64 RegionOneBase{0x2000'0000'0000'0000ULL};
constexpr Foundation::UInt64 CodeVirtualStart{RegionOneBase + 0x0400'1100U};
constexpr Foundation::UInt64 ReadOnlyVirtualStart{RegionOneBase + 0x0400'3100U};
constexpr Foundation::UInt64 DataVirtualStart{RegionOneBase + 0x0400'8100U};
constexpr Foundation::UInt64 CodeFileOffset{0x1100U};
constexpr Foundation::UInt64 ReadOnlyFileOffset{0x2100U};
constexpr Foundation::UInt64 DataFileOffset{0x3100U};
constexpr Foundation::UInt64 DescriptorAddress{ReadOnlyVirtualStart + 0x20U};
constexpr Foundation::UInt64 EntryBundleAddress{CodeVirtualStart + 0x20U};
constexpr Foundation::UInt64 GlobalPointer{DataVirtualStart + 0x80U};
constexpr Foundation::UInt64 FirmwareWriteBackAttribute{Foundation::UInt64{1U} << 3U};
constexpr Foundation::UInt64 PageSizeBytes{4096U};
constexpr Foundation::UInt8 ValidPhysicalAddressBits{48U};
constexpr Foundation::UInt8 ValidVirtualAddressMostSignificantBit{60U};
constexpr Foundation::UInt64 CurrentLoadPageCount{7U};
constexpr Foundation::UInt64 CurrentArenaPageCount{15U};

alignas(4096U) Foundation::UInt8 g_arenaStorage[TestBackingSizeBytes]{};
Boot::BootInfo g_bootInfo{};
Kernel::Memory::AddressSpace g_unrelatedAddressSpace{};
Foundation::UInt8 g_unrelatedAddressSpaceSnapshot[sizeof(g_unrelatedAddressSpace)]{};

static_assert(std::is_standard_layout_v<Platform::Ia64::Executable::Ia64InitialUserImageMemoryRangePlan>);
static_assert(std::is_trivially_copyable_v<Platform::Ia64::Executable::Ia64InitialUserImageMemoryRangePlan>);
static_assert(std::is_standard_layout_v<Platform::Ia64::Executable::Ia64InitialUserImageMemoryRangeView>);
static_assert(std::is_trivially_copyable_v<Platform::Ia64::Executable::Ia64InitialUserImageMemoryRangeView>);
static_assert(!std::is_copy_constructible_v<Ia64InitialUserImageMemoryOwner>);
static_assert(!std::is_move_constructible_v<Ia64InitialUserImageMemoryOwner>);
static_assert(Platform::Ia64::Executable::Ia64InitialUserImageMaximumMemoryObjectCount == 10U);
static_assert(Platform::Ia64::Executable::Ia64InitialUserImageMaximumObjectRegistrationCount == 11U);
static_assert(Platform::Ia64::Executable::Ia64InitialUserImageMaximumMaterializedPageCount == 360U);

class RegistryFixture final
{
public:
    explicit constexpr RegistryFixture(const Foundation::UInt32 maximumReferences =
                                           Platform::Synchronization::AtomicReferenceCount::MaximumValue) noexcept
        : Registry{Synchronization, maximumReferences}, Issuer{Registry}
    {
    }

    SpinLock Synchronization;
    Asteria::TestSupport::TestObjectRegistry Registry;
    RootCapabilityIssuer Issuer;
};

[[nodiscard]] constexpr ResourceDomainLimits MakeLimits(const Foundation::UInt64 committedPages = 1024U,
                                                        const Foundation::UInt64 residentPages = 1024U,
                                                        const Foundation::UInt64 metadataBytes = 1U << 20U) noexcept
{
    return ResourceDomainLimits{
        .CommittedMemoryPages = committedPages,
        .ResidentMemoryPages = residentPages,
        .KernelMetadataBytes = metadataBytes,
        .CapabilityHandles = ObjectRegistry::MaximumObjects,
        .IpcQueueBytes = 0U,
        .Threads = 0U,
        .ChildDomains = 0U,
    };
}

[[nodiscard]] constexpr bool ResourceUsageIsEqual(const Kernel::Resource::ResourceDomainUsage& first,
                                                  const Kernel::Resource::ResourceDomainUsage& second) noexcept
{
    return (first.CommittedMemoryPages == second.CommittedMemoryPages) &&
           (first.ResidentMemoryPages == second.ResidentMemoryPages) &&
           (first.KernelMetadataBytes == second.KernelMetadataBytes) &&
           (first.CapabilityHandles == second.CapabilityHandles) && (first.IpcQueueBytes == second.IpcQueueBytes) &&
           (first.Threads == second.Threads) && (first.ChildDomains == second.ChildDomains);
}

class DomainFixture final
{
public:
    DomainFixture(RegistryFixture& registry, const ResourceDomainId identifier,
                  const ResourceDomainLimits& limits = MakeLimits()) noexcept
        : Domain{Ledger}, m_registry{registry}
    {
        const auto initialized = Domain.InitializeRoot(identifier, limits);
        if (initialized.Error != ResourceDomainError::None)
        {
            return;
        }
        const auto registered = m_registry.Issuer.CreateObject(Domain);
        if (!registered.HasValue())
        {
            return;
        }
        Object = registered.GetValue();
        Identity = ResourceDomainObjectId{m_registry.Registry, Object};
    }

    ~DomainFixture() noexcept
    {
        if (!Object.IsValid())
        {
            return;
        }
        static_cast<void>(Domain.Quiesce());
        static_cast<void>(m_registry.Issuer.DestroyObject(Object));
        static_cast<void>(m_registry.Issuer.CollectDestroyedObject());
    }

    DomainFixture(const DomainFixture&) = delete;
    DomainFixture& operator=(const DomainFixture&) = delete;

    ResourceDomainLedger Ledger;
    ResourceDomain Domain;
    ObjectId Object{};
    ResourceDomainObjectId Identity{};

private:
    RegistryFixture& m_registry;
};

class TestKernelObject final : public IKernelObject
{
public:
    constexpr TestKernelObject() noexcept = default;

    [[nodiscard]] ObjectType GetObjectType() const noexcept override
    {
        return ObjectType::Thread;
    }

    [[nodiscard]] ResourceDomainId GetChargeOwner() const noexcept override
    {
        return ResourceDomainId{77U};
    }
};

class TerminalRetainedFixture final
{
public:
    TerminalRetainedFixture() noexcept : Registry{1U}, Domain{Registry, ResourceDomainId{91U}}
    {
    }

    RegistryFixture Registry;
    DomainFixture Domain;
    BootstrapPageAllocator Allocator;
    Ia64InitialUserImageOwner StagedOwner;
    Ia64InitialUserImageMemoryOwner Owner;
};

alignas(TerminalRetainedFixture) Foundation::UInt8 g_terminalRetainedFixtureStorage[sizeof(TerminalRetainedFixture)]{};

[[nodiscard]] Foundation::Size GetProgramHeaderOffset(const Foundation::Size index) noexcept
{
    return ProgramHeaderTableOffset + (index * ProgramHeaderSizeBytes);
}

void WriteLittleEndian16(Foundation::UInt8* const bytes, const Foundation::UInt16 value) noexcept
{
    bytes[0U] = static_cast<Foundation::UInt8>(value);
    bytes[1U] = static_cast<Foundation::UInt8>(value >> 8U);
}

void WriteLittleEndian32(Foundation::UInt8* const bytes, const Foundation::UInt32 value) noexcept
{
    for (Foundation::UInt32 index{0U}; index < 4U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> (index * 8U));
    }
}

void WriteLittleEndian64(Foundation::UInt8* const bytes, const Foundation::UInt64 value) noexcept
{
    for (Foundation::UInt32 index{0U}; index < 8U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> (index * 8U));
    }
}

void SetProgramHeader(const Foundation::Size index, const Foundation::UInt32 flags, const Foundation::UInt64 fileOffset,
                      const Foundation::UInt64 virtualStart, const Foundation::UInt64 fileSizeBytes,
                      const Foundation::UInt64 memorySizeBytes) noexcept
{
    Foundation::UInt8* const header = AsteriaIa64InitialUserImageMemoryOwnerTestStorage + GetProgramHeaderOffset(index);
    WriteLittleEndian32(header, 1U);
    WriteLittleEndian32(header + 4U, flags);
    WriteLittleEndian64(header + 8U, fileOffset);
    WriteLittleEndian64(header + 16U, virtualStart);
    WriteLittleEndian64(header + 24U, virtualStart);
    WriteLittleEndian64(header + 32U, fileSizeBytes);
    WriteLittleEndian64(header + 40U, memorySizeBytes);
    WriteLittleEndian64(header + 48U, PageSizeBytes);
}

void FillPayload(const Foundation::UInt64 fileOffset, const Foundation::UInt64 sizeBytes,
                 const Foundation::UInt8 seed) noexcept
{
    for (Foundation::UInt64 index{0U}; index < sizeBytes; ++index)
    {
        AsteriaIa64InitialUserImageMemoryOwnerTestStorage[static_cast<Foundation::Size>(fileOffset + index)] =
            static_cast<Foundation::UInt8>(seed + static_cast<Foundation::UInt8>(index));
    }
}

void SetEntryDescriptor() noexcept
{
    const Foundation::Size descriptorOffset =
        static_cast<Foundation::Size>(ReadOnlyFileOffset + (DescriptorAddress - ReadOnlyVirtualStart));
    WriteLittleEndian64(AsteriaIa64InitialUserImageMemoryOwnerTestStorage + descriptorOffset, EntryBundleAddress);
    WriteLittleEndian64(AsteriaIa64InitialUserImageMemoryOwnerTestStorage + descriptorOffset + 8U, GlobalPointer);
}

void MakeImageHeader(const Foundation::UInt16 programHeaderCount) noexcept
{
    static_cast<void>(std::memset(AsteriaIa64InitialUserImageMemoryOwnerTestStorage, 0, TestImageSizeBytes));
    Foundation::UInt8* const image = AsteriaIa64InitialUserImageMemoryOwnerTestStorage;
    image[0U] = 0x7FU;
    image[1U] = static_cast<Foundation::UInt8>('E');
    image[2U] = static_cast<Foundation::UInt8>('L');
    image[3U] = static_cast<Foundation::UInt8>('F');
    image[4U] = 2U;
    image[5U] = 1U;
    image[6U] = 1U;
    WriteLittleEndian16(image + 16U, 2U);
    WriteLittleEndian16(image + 18U, Platform::Ia64::Executable::Ia64ElfMachine);
    WriteLittleEndian32(image + 20U, 1U);
    WriteLittleEndian64(image + 24U, DescriptorAddress);
    WriteLittleEndian64(image + 32U, ProgramHeaderTableOffset);
    WriteLittleEndian32(image + 48U, Platform::Ia64::Executable::Ia64P64ElfFlags);
    WriteLittleEndian16(image + 52U, 64U);
    WriteLittleEndian16(image + 54U, 56U);
    WriteLittleEndian16(image + 56U, programHeaderCount);
}

void MakeCurrentImage(const Foundation::UInt32 codeFlags = 5U, const bool includeZeroLengthSegment = false) noexcept
{
    MakeImageHeader(includeZeroLengthSegment ? 4U : 3U);
    SetProgramHeader(0U, codeFlags, CodeFileOffset, CodeVirtualStart, 0x180U, 0x300U);
    SetProgramHeader(1U, 4U, ReadOnlyFileOffset, ReadOnlyVirtualStart, 0x300U, 0x1100U);
    SetProgramHeader(2U, 6U, DataFileOffset, DataVirtualStart, 0x900U, 0x3F00U);
    if (includeZeroLengthSegment)
    {
        SetProgramHeader(3U, 4U, 0U, RegionOneBase + 0x0400'C000U, 0U, 0U);
    }
    FillPayload(CodeFileOffset, 0x180U, 0x31U);
    FillPayload(ReadOnlyFileOffset, 0x300U, 0x52U);
    FillPayload(DataFileOffset, 0x900U, 0x73U);
    SetEntryDescriptor();
}

void MakeMaximumSegmentImage() noexcept
{
    MakeImageHeader(8U);
    SetProgramHeader(0U, 5U, CodeFileOffset, CodeVirtualStart, 0x180U, 0x300U);
    SetProgramHeader(1U, 4U, ReadOnlyFileOffset, ReadOnlyVirtualStart, 0x300U, 0x1100U);
    SetProgramHeader(2U, 6U, DataFileOffset, DataVirtualStart, 0x900U, 0x3F00U);
    for (Foundation::Size index{3U}; index < 8U; ++index)
    {
        SetProgramHeader(index, 4U, 0x100U,
                         RegionOneBase + 0x0400'D100U + (static_cast<Foundation::UInt64>(index - 3U) * 0x2000U), 0U,
                         0x200U);
    }
    FillPayload(CodeFileOffset, 0x180U, 0x31U);
    FillPayload(ReadOnlyFileOffset, 0x300U, 0x52U);
    FillPayload(DataFileOffset, 0x900U, 0x73U);
    SetEntryDescriptor();
}

void MakeLoadAfterStackAndRseImage() noexcept
{
    MakeImageHeader(4U);
    SetProgramHeader(0U, 5U, CodeFileOffset, CodeVirtualStart, 0x180U, 0x300U);
    SetProgramHeader(1U, 4U, ReadOnlyFileOffset, ReadOnlyVirtualStart, 0x300U, 0x1100U);
    SetProgramHeader(2U, 6U, DataFileOffset, DataVirtualStart, 0x900U, 0x3F00U);
    SetProgramHeader(3U, 4U, 0x4100U, RegionOneBase + 0x0700'1100U, 0x80U, 0x200U);
    FillPayload(CodeFileOffset, 0x180U, 0x31U);
    FillPayload(ReadOnlyFileOffset, 0x300U, 0x52U);
    FillPayload(DataFileOffset, 0x900U, 0x73U);
    FillPayload(0x4100U, 0x80U, 0x94U);
    SetEntryDescriptor();
}

void ResetBootInfo() noexcept
{
    static_cast<void>(std::memset(&g_bootInfo, 0, sizeof(g_bootInfo)));
    g_bootInfo.Signature = Boot::BootInfoSignature;
    g_bootInfo.Version = Boot::BootInfoVersion;
    g_bootInfo.SizeBytes = static_cast<Foundation::UInt32>(sizeof(g_bootInfo));
    g_bootInfo.MemoryRegionCount = 1U;
    g_bootInfo.FirmwareDescriptorVersion = 1U;
    g_bootInfo.FirmwareDescriptorSize = 40U;
    g_bootInfo.FirmwareMemoryMapSizeBytes = 40U;
    g_bootInfo.MemoryRegions[0U] = Boot::PhysicalMemoryRegion{
        .PhysicalStart = reinterpret_cast<Foundation::UIntPointer>(g_arenaStorage),
        .VirtualStart = 0U,
        .PageCount = TestBackingPageCount,
        .Attributes = FirmwareWriteBackAttribute,
        .FirmwareType = 7U,
        .Kind = Boot::PhysicalMemoryKind::Conventional,
    };
}

[[nodiscard]] bool StageCurrentImage(Ia64InitialUserImageOwner& owner, BootstrapPageAllocator& allocator) noexcept
{
    ResetBootInfo();
    if (allocator.Initialize(g_bootInfo) != BootstrapPageAllocatorError::None)
    {
        return false;
    }
    const auto result = owner.Stage(allocator, ValidPhysicalAddressBits, ValidVirtualAddressMostSignificantBit,
                                    {AsteriaIa64InitialUserImageMemoryOwnerTestStorage, 20480U}, SourceRegion());
    if (!result.HasValue())
    {
        static_cast<void>(std::fprintf(
            stderr, "stage diagnostic: error=%u p64=%u elf=%u segment=%zu\n",
            static_cast<unsigned int>(result.GetError().Error), static_cast<unsigned int>(result.GetError().P64.Error),
            static_cast<unsigned int>(result.GetError().P64.Elf64Error), result.GetError().SegmentIndex));
    }
    return result.HasValue();
}

[[nodiscard]] bool PinMatches(ObjectRegistry& registry, const ObjectId identity, const ObjectType type,
                              IKernelObject& expected) noexcept
{
    ObjectPin pin;
    const ObjectPinError error = registry.PinObject(identity, type, pin);
    const bool matches = (error == ObjectPinError::Ok) && pin.IsValid() && (&pin.GetObject() == &expected);
    pin.Reset();
    return matches;
}

[[nodiscard]] bool ReleaseMaterializedObjects(Ia64InitialUserImageMemoryOwner& owner,
                                              RegistryFixture& registry) noexcept
{
    const auto summary = owner.GetSummary();
    if (!summary.HasValue())
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < summary.GetValue().RangeCount; ++index)
    {
        const auto range = owner.BorrowRange(index);
        if (!range.HasValue() ||
            (range.GetValue().MemoryObjectStorage->Quiesce().Error != Kernel::Memory::MemoryObjectError::None) ||
            (registry.Issuer.DestroyObject(range.GetValue().Plan.MemoryObject.GetObjectId()) !=
             ObjectRegistryError::Ok))
        {
            return false;
        }
    }
    if (registry.Issuer.DestroyObject(summary.GetValue().AggregateFrame.GetObjectId()) != ObjectRegistryError::Ok)
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < summary.GetValue().RangeCount + 1U; ++index)
    {
        if (!registry.Issuer.CollectDestroyedObject().HasValue())
        {
            return false;
        }
    }
    return true;
}

void CheckCurrentRanges(TestContext& runner, Ia64InitialUserImageMemoryOwner& owner, ObjectRegistry& registry) noexcept
{
    constexpr Foundation::UInt64 expectedVirtualStarts[]{
        CodeVirtualStart & ~(PageSizeBytes - 1U),
        ReadOnlyVirtualStart & ~(PageSizeBytes - 1U),
        DataVirtualStart & ~(PageSizeBytes - 1U),
        Platform::Ia64::Ia64InitialUserMemoryStackVirtualStart,
        Platform::Ia64::Ia64InitialUserRseBackingStoreVirtualStart,
    };
    constexpr Foundation::UInt64 expectedPages[]{1U, 2U, 4U, 4U, 4U};
    constexpr Foundation::UInt64 expectedOffsets[]{0U, 1U, 3U, 7U, 11U};
    constexpr Foundation::UInt8 expectedPermissions[]{13U, 9U, 11U, 11U, 11U};
    Kernel::Memory::Frame* aggregateFrame{nullptr};
    bool rangesMatch{true};
    for (Foundation::Size index{0U}; index < 5U; ++index)
    {
        const auto range = owner.BorrowRange(index);
        if (!range.HasValue())
        {
            rangesMatch = false;
            continue;
        }
        const auto& view = range.GetValue();
        if (aggregateFrame == nullptr)
        {
            aggregateFrame = view.FrameStorage;
        }
        const bool segment = index < 3U;
        rangesMatch =
            rangesMatch && (view.FrameStorage == aggregateFrame) && (view.MemoryObjectStorage != nullptr) &&
            (view.Plan.Purpose == (segment ? Ia64InitialUserImageMemoryPurpose::LoadSegment
                                           : (index == 3U ? Ia64InitialUserImageMemoryPurpose::MemoryStack
                                                          : Ia64InitialUserImageMemoryPurpose::RseBackingStore))) &&
            (view.Plan.GuardPolicy == (segment ? Ia64InitialUserImageGuardPolicy::None
                                               : Ia64InitialUserImageGuardPolicy::OnePageBeforeAndAfter)) &&
            (view.Plan.OriginalSegmentIndex ==
             (segment ? index : Platform::Ia64::Executable::Ia64InitialUserImageNoSegment)) &&
            (view.Plan.VirtualStart == expectedVirtualStarts[index]) &&
            (view.Plan.Pages.GetValue() == expectedPages[index]) &&
            (view.Plan.FrameOffsetPages == expectedOffsets[index]) &&
            (view.Plan.Permissions.GetBits() == expectedPermissions[index]) &&
            view.Plan.Permissions.Contains(PagePermission::User) &&
            (view.MemoryObjectStorage->GetSnapshot().State == MemoryObjectState::Ready) &&
            (view.MemoryObjectStorage->GetSnapshot().ActiveMappings == 0U) &&
            PinMatches(registry, view.Plan.MemoryObject.GetObjectId(), ObjectType::MemoryObject,
                       *view.MemoryObjectStorage);
    }
    ASTERIA_EXPECT(runner, "current ranges retain exact RXU/RU/RWU policy, offsets, guards, zero mappings, and pins",
                   rangesMatch);
    const auto summary = owner.GetSummary();
    ASTERIA_EXPECT(
        runner, "the one aggregate Frame identity pins back to the shared embedded Frame", summary.HasValue(),
        aggregateFrame != nullptr,
        PinMatches(registry, summary.GetValue().AggregateFrame.GetObjectId(), ObjectType::Frame, *aggregateFrame));
}

void TestCurrentShapeAndRetryableSource(TestContext& runner) noexcept
{
    MakeCurrentImage();
    RegistryFixture registry{};
    DomainFixture domain{registry, ResourceDomainId{41U}};
    BootstrapPageAllocator allocator{};
    Ia64InitialUserImageOwner stagedOwner{};
    Ia64InitialUserImageMemoryOwner owner{};
    // Byte snapshot of the polymorphic AddressSpace, vtable pointer included.
    static_cast<void>(std::memcpy(g_unrelatedAddressSpaceSnapshot, static_cast<const void*>(&g_unrelatedAddressSpace),
                                  sizeof(g_unrelatedAddressSpace)));

    const auto premature =
        owner.Materialize(stagedOwner, registry.Registry, registry.Issuer, domain.Identity, domain.Domain);
    ASTERIA_EXPECT(runner, "a non-staged source is rejected diagnostically and remains retryable",
                   !premature.HasValue(),
                   premature.GetError().Error == Ia64InitialUserImageMemoryOwnerError::SourceNotStaged,
                   owner.GetState() == Ia64InitialUserImageMemoryOwnerState::Fresh, owner.GetFailure().HasValue(),
                   owner.GetSummary().GetError() == Ia64InitialUserImageMemoryAccessError::NotReady,
                   registry.Registry.GetActiveCount() == 1U);
    ASTERIA_EXPECT(runner, "the current three-segment fixture stages after retryable rejection",
                   StageCurrentImage(stagedOwner, allocator));
    const auto stagedBefore = stagedOwner.GetStageSummary();
    const auto initialDomain = domain.Domain.GetSnapshot();
    const auto materialized =
        owner.Materialize(stagedOwner, registry.Registry, registry.Issuer, domain.Identity, domain.Domain);
    const auto stagedAfter = stagedOwner.GetStageSummary();
    const auto finalDomain = domain.Domain.GetSnapshot();
    ASTERIA_EXPECT(runner, "the current shape creates one Frame, five MemoryObjects, and six registrations",
                   materialized.HasValue(), owner.GetState() == Ia64InitialUserImageMemoryOwnerState::Ready,
                   materialized.GetValue().RangeCount == 5U,
                   materialized.GetValue().AggregateArena.PageCount == CurrentArenaPageCount,
                   registry.Registry.GetActiveCount() == 7U);
    ASTERIA_EXPECT(
        runner, "five pinned MemoryObjects account exactly fifteen committed/resident pages and their metadata",
        finalDomain.Usage.CommittedMemoryPages == initialDomain.Usage.CommittedMemoryPages + 15U,
        finalDomain.Usage.ResidentMemoryPages == initialDomain.Usage.ResidentMemoryPages + 15U,
        finalDomain.Usage.KernelMetadataBytes == initialDomain.Usage.KernelMetadataBytes + (5U * sizeof(MemoryObject)),
        finalDomain.Usage.CapabilityHandles == initialDomain.Usage.CapabilityHandles);
    ASTERIA_EXPECT(runner, "materialization claims the staged owner while preserving its allocator receipt",
                   stagedBefore.HasValue(), stagedAfter.HasValue(),
                   stagedBefore.GetValue().AggregateLoadPageCount == CurrentLoadPageCount,
                   stagedAfter.GetValue().AggregateArena.PhysicalStart ==
                       stagedBefore.GetValue().AggregateArena.PhysicalStart,
                   stagedAfter.GetValue().AggregateArena.PageCount == stagedBefore.GetValue().AggregateArena.PageCount,
                   stagedOwner.GetState() == Platform::Ia64::Executable::Ia64InitialUserImageOwnerState::Claimed);
    CheckCurrentRanges(runner, owner, registry.Registry);
    ASTERIA_EXPECT(runner, "materialization does not receive or mutate an AddressSpace",
                   std::memcmp(g_unrelatedAddressSpaceSnapshot, static_cast<const void*>(&g_unrelatedAddressSpace),
                               sizeof(g_unrelatedAddressSpace)) == 0);
    const auto repeated =
        owner.Materialize(stagedOwner, registry.Registry, registry.Issuer, domain.Identity, domain.Domain);
    ASTERIA_EXPECT(runner, "a ready owner rejects a repeated materialization without changing state",
                   !repeated.HasValue(),
                   repeated.GetError().Error == Ia64InitialUserImageMemoryOwnerError::AlreadyAttempted,
                   owner.GetState() == Ia64InitialUserImageMemoryOwnerState::Ready);

    Ia64InitialUserImageMemoryOwner replayOwner{};
    const Foundation::Size claimedObjectCount = registry.Registry.GetActiveCount();
    const auto claimedUsage = domain.Domain.GetSnapshot().Usage;
    const auto replayed =
        replayOwner.Materialize(stagedOwner, registry.Registry, registry.Issuer, domain.Identity, domain.Domain);
    const ObjectId firstPublishedFrame = materialized.GetValue().AggregateFrame.GetObjectId();
    const auto firstSummary = owner.GetSummary();
    const auto firstRange = owner.BorrowRange(0U);
    ASTERIA_EXPECT(
        runner,
        "a claimed physical arena cannot be materialized by a second owner and the first identity remains unique",
        !replayed.HasValue(), replayed.GetError().Error == Ia64InitialUserImageMemoryOwnerError::SourceNotStaged,
        replayOwner.GetState() == Ia64InitialUserImageMemoryOwnerState::Fresh,
        stagedOwner.GetState() == Platform::Ia64::Executable::Ia64InitialUserImageOwnerState::Claimed,
        stagedOwner.GetStageSummary().HasValue(), stagedOwner.GetSegmentPlan(0U).HasValue(),
        registry.Registry.GetActiveCount() == claimedObjectCount,
        ResourceUsageIsEqual(domain.Domain.GetSnapshot().Usage, claimedUsage), firstSummary.HasValue(),
        firstSummary.GetValue().AggregateFrame.GetObjectId().GetValue() == firstPublishedFrame.GetValue(),
        firstRange.HasValue(),
        PinMatches(registry.Registry, firstPublishedFrame, ObjectType::Frame, *firstRange.GetValue().FrameStorage),
        PinMatches(registry.Registry, firstRange.GetValue().Plan.MemoryObject.GetObjectId(), ObjectType::MemoryObject,
                   *firstRange.GetValue().MemoryObjectStorage));
    ASTERIA_EXPECT(runner, "the hosted success fixture quiesces and collects every test object",
                   ReleaseMaterializedObjects(owner, registry),
                   domain.Domain.GetSnapshot().Usage.CommittedMemoryPages == 0U);
}

void TestIdentityPreflightFailures(TestContext& runner) noexcept
{
    MakeCurrentImage();
    RegistryFixture registry{};
    RegistryFixture foreign{};
    DomainFixture domain{registry, ResourceDomainId{51U}};
    DomainFixture otherDomain{registry, ResourceDomainId{52U}};
    DomainFixture notReadyDomain{registry, ResourceDomainId{53U}};
    BootstrapPageAllocator allocator{};
    Ia64InitialUserImageOwner stagedOwner{};
    Ia64InitialUserImageMemoryOwner owner{};
    ASTERIA_EXPECT(runner, "the identity-rejection fixture stages", StageCurrentImage(stagedOwner, allocator));
    ASTERIA_EXPECT(runner, "the not-ready identity fixture quiesces its registered domain",
                   notReadyDomain.Domain.Quiesce() == ResourceDomainError::None);

    const auto issuerMismatch =
        owner.Materialize(stagedOwner, registry.Registry, foreign.Issuer, domain.Identity, domain.Domain);
    const ResourceDomainObjectId foreignIdentity{foreign.Registry, domain.Object};
    const auto foreignGeneration =
        owner.Materialize(stagedOwner, registry.Registry, registry.Issuer, foreignIdentity, domain.Domain);
    const auto instanceMismatch =
        owner.Materialize(stagedOwner, registry.Registry, registry.Issuer, otherDomain.Identity, domain.Domain);
    const auto notReady = owner.Materialize(stagedOwner, registry.Registry, registry.Issuer, notReadyDomain.Identity,
                                            notReadyDomain.Domain);
    ASTERIA_EXPECT(
        runner, "foreign issuer/generation, mismatched instance, and not-ready domain are retryable and inert",
        !issuerMismatch.HasValue(),
        issuerMismatch.GetError().Error == Ia64InitialUserImageMemoryOwnerError::IssuerRegistryMismatch,
        !foreignGeneration.HasValue(),
        foreignGeneration.GetError().Error == Ia64InitialUserImageMemoryOwnerError::InvalidResourceDomain,
        !instanceMismatch.HasValue(),
        instanceMismatch.GetError().Error == Ia64InitialUserImageMemoryOwnerError::InvalidResourceDomain,
        !notReady.HasValue(), notReady.GetError().Error == Ia64InitialUserImageMemoryOwnerError::InvalidResourceDomain,
        owner.GetState() == Ia64InitialUserImageMemoryOwnerState::Fresh, registry.Registry.GetActiveCount() == 3U);

    const auto success =
        owner.Materialize(stagedOwner, registry.Registry, registry.Issuer, domain.Identity, domain.Domain);
    ASTERIA_EXPECT(runner, "the same owner succeeds with the exact ready root generation", success.HasValue(),
                   success.GetValue().ResourceDomainGeneration.IsEqualTo(domain.Identity));
    ASTERIA_EXPECT(runner, "the identity fixture collects successful materialized objects",
                   ReleaseMaterializedObjects(owner, registry));
}

void TestCapacityPreflightFailures(TestContext& runner) noexcept
{
    MakeCurrentImage();
    BootstrapPageAllocator allocator{};
    Ia64InitialUserImageOwner stagedOwner{};
    ASTERIA_EXPECT(runner, "the capacity fixture stages", StageCurrentImage(stagedOwner, allocator));

    {
        RegistryFixture registry{};
        DomainFixture domain{registry, ResourceDomainId{61U}};
        constexpr Foundation::Size fillerCount{Asteria::TestSupport::TestObjectRegistry::EntryCapacity - 6U};
        TestKernelObject fillers[fillerCount]{};
        ObjectId fillerIdentities[fillerCount]{};
        Foundation::Size registeredFillers{0U};
        for (TestKernelObject& filler : fillers)
        {
            const auto registered = registry.Issuer.CreateObject(filler);
            if (registered.HasValue())
            {
                fillerIdentities[registeredFillers] = registered.GetValue();
                ++registeredFillers;
            }
        }
        const Foundation::Size initialCount = registry.Registry.GetActiveCount();
        const auto initialUsage = domain.Domain.GetSnapshot().Usage;
        Ia64InitialUserImageMemoryOwner owner{};
        const auto result =
            owner.Materialize(stagedOwner, registry.Registry, registry.Issuer, domain.Identity, domain.Domain);
        const auto finalUsage = domain.Domain.GetSnapshot().Usage;
        ASTERIA_EXPECT(runner, "insufficient registry capacity is rejected before object or resource side effects",
                       registeredFillers == fillerCount, !result.HasValue(),
                       result.GetError().Error == Ia64InitialUserImageMemoryOwnerError::RegistryCapacityExceeded,
                       owner.GetState() == Ia64InitialUserImageMemoryOwnerState::Fresh,
                       registry.Registry.GetActiveCount() == initialCount,
                       finalUsage.CommittedMemoryPages == initialUsage.CommittedMemoryPages,
                       finalUsage.KernelMetadataBytes == initialUsage.KernelMetadataBytes);

        bool fillersReleased{true};
        for (Foundation::Size index{0U}; index < registeredFillers; ++index)
        {
            if (registry.Issuer.DestroyObject(fillerIdentities[index]) != ObjectRegistryError::Ok)
            {
                fillersReleased = false;
            }
        }
        for (Foundation::Size index{0U}; index < registeredFillers; ++index)
        {
            if (!registry.Issuer.CollectDestroyedObject().HasValue())
            {
                fillersReleased = false;
            }
        }
        ASTERIA_EXPECT(runner, "every capacity filler is explicitly destroyed and collected before its storage expires",
                       fillersReleased, registry.Registry.GetActiveCount() == 1U);
    }

    {
        RegistryFixture registry{};
        DomainFixture domain{registry, ResourceDomainId{62U}, MakeLimits(14U, 14U)};
        Ia64InitialUserImageMemoryOwner owner{};
        const Foundation::Size initialCount = registry.Registry.GetActiveCount();
        const auto result =
            owner.Materialize(stagedOwner, registry.Registry, registry.Issuer, domain.Identity, domain.Domain);
        ASTERIA_EXPECT(runner, "insufficient committed/resident capacity is rejected before initialization",
                       !result.HasValue(),
                       result.GetError().Error == Ia64InitialUserImageMemoryOwnerError::ResourceCapacityExceeded,
                       owner.GetState() == Ia64InitialUserImageMemoryOwnerState::Fresh,
                       registry.Registry.GetActiveCount() == initialCount,
                       domain.Domain.GetSnapshot().Usage.CommittedMemoryPages == 0U);
    }
}

void TestExecuteOnlyAndZeroLength(TestContext& runner) noexcept
{
    {
        MakeCurrentImage(1U);
        BootstrapPageAllocator allocator{};
        Ia64InitialUserImageOwner stagedOwner{};
        RegistryFixture registry{};
        DomainFixture domain{registry, ResourceDomainId{71U}};
        Ia64InitialUserImageMemoryOwner owner{};
        ASTERIA_EXPECT(runner, "the execute-only segment passes staging for memory-owner policy testing",
                       StageCurrentImage(stagedOwner, allocator));
        const auto result =
            owner.Materialize(stagedOwner, registry.Registry, registry.Issuer, domain.Identity, domain.Domain);
        ASTERIA_EXPECT(runner, "execute-only PT_LOAD is rejected without silently granting read", !result.HasValue(),
                       result.GetError().Error == Ia64InitialUserImageMemoryOwnerError::PermissionRejected,
                       result.GetError().OriginalSegmentIndex == 0U,
                       owner.GetState() == Ia64InitialUserImageMemoryOwnerState::Fresh,
                       registry.Registry.GetActiveCount() == 1U);
    }

    {
        MakeCurrentImage(5U, true);
        BootstrapPageAllocator allocator{};
        Ia64InitialUserImageOwner stagedOwner{};
        RegistryFixture registry{};
        DomainFixture domain{registry, ResourceDomainId{72U}};
        Ia64InitialUserImageMemoryOwner owner{};
        ASTERIA_EXPECT(runner, "the zero-length PT_LOAD fixture stages", StageCurrentImage(stagedOwner, allocator));
        const auto result =
            owner.Materialize(stagedOwner, registry.Registry, registry.Issuer, domain.Identity, domain.Domain);
        ASTERIA_EXPECT(runner, "a zero-page PT_LOAD is skipped without changing aggregate accounting",
                       result.HasValue(), result.GetValue().RangeCount == 5U, registry.Registry.GetActiveCount() == 7U,
                       domain.Domain.GetSnapshot().Usage.CommittedMemoryPages == CurrentArenaPageCount);
        ASTERIA_EXPECT(runner, "the zero-length fixture collects successful objects",
                       ReleaseMaterializedObjects(owner, registry));
    }
}

void TestMaximumRangeCapacity(TestContext& runner) noexcept
{
    MakeMaximumSegmentImage();
    BootstrapPageAllocator allocator{};
    Ia64InitialUserImageOwner stagedOwner{};
    RegistryFixture registry{};
    DomainFixture domain{registry, ResourceDomainId{81U}};
    Ia64InitialUserImageMemoryOwner owner{};
    ASTERIA_EXPECT(runner, "the eight-segment capacity fixture stages", StageCurrentImage(stagedOwner, allocator));
    const auto result =
        owner.Materialize(stagedOwner, registry.Registry, registry.Issuer, domain.Identity, domain.Domain);
    ASTERIA_EXPECT(runner, "eight nonempty PT_LOAD ranges reach the ten-MemoryObject and eleven-registration bound",
                   result.HasValue(), result.GetValue().RangeCount == 10U,
                   result.GetValue().AggregateArena.PageCount == 20U, registry.Registry.GetActiveCount() == 12U,
                   domain.Domain.GetSnapshot().Usage.CommittedMemoryPages == 20U,
                   domain.Domain.GetSnapshot().Usage.KernelMetadataBytes == (10U * sizeof(MemoryObject)));
    ASTERIA_EXPECT(runner, "the maximum-capacity fixture collects successful objects",
                   ReleaseMaterializedObjects(owner, registry));
}

void TestLoadAfterStackAndRseCorridors(TestContext& runner) noexcept
{
    MakeLoadAfterStackAndRseImage();
    BootstrapPageAllocator allocator{};
    Ia64InitialUserImageOwner stagedOwner{};
    RegistryFixture registry{};
    DomainFixture domain{registry, ResourceDomainId{82U}};
    Ia64InitialUserImageMemoryOwner owner{};
    ASTERIA_EXPECT(runner, "the Region 1 post-corridor PT_LOAD fixture stages",
                   StageCurrentImage(stagedOwner, allocator));
    const auto result =
        owner.Materialize(stagedOwner, registry.Registry, registry.Issuer, domain.Identity, domain.Domain);
    const auto fourthLoad = owner.GetRangePlan(3U);
    const auto stack = owner.GetRangePlan(4U);
    const auto rse = owner.GetRangePlan(5U);
    ASTERIA_EXPECT(
        runner,
        "a disjoint ordered PT_LOAD after both guarded corridors is accepted while plans retain staged-load, stack, RSE order",
        result.HasValue(), result.GetValue().RangeCount == 6U, fourthLoad.HasValue(),
        fourthLoad.GetValue().Purpose == Ia64InitialUserImageMemoryPurpose::LoadSegment,
        fourthLoad.GetValue().VirtualStart == (RegionOneBase + 0x0700'1000U), stack.HasValue(),
        stack.GetValue().Purpose == Ia64InitialUserImageMemoryPurpose::MemoryStack, rse.HasValue(),
        rse.GetValue().Purpose == Ia64InitialUserImageMemoryPurpose::RseBackingStore,
        registry.Registry.GetActiveCount() == 8U, domain.Domain.GetSnapshot().Usage.CommittedMemoryPages == 16U);
    ASTERIA_EXPECT(runner, "the post-corridor PT_LOAD fixture collects successful objects",
                   ReleaseMaterializedObjects(owner, registry));
}

void TestTerminalFailureAfterSideEffects(TestContext& runner) noexcept
{
    MakeCurrentImage();
    auto* const fixture = ::new (static_cast<void*>(g_terminalRetainedFixtureStorage)) TerminalRetainedFixture{};
    ASTERIA_EXPECT(runner, "the terminal-failure fixture stages",
                   StageCurrentImage(fixture->StagedOwner, fixture->Allocator));
    const auto result =
        fixture->Owner.Materialize(fixture->StagedOwner, fixture->Registry.Registry, fixture->Registry.Issuer,
                                   fixture->Domain.Identity, fixture->Domain.Domain);
    const auto usage = fixture->Domain.Domain.GetSnapshot().Usage;
    ASTERIA_EXPECT(
        runner, "reference exhaustion after the first MO initialization is terminal and retains committed state",
        !result.HasValue(),
        result.GetError().Error == Ia64InitialUserImageMemoryOwnerError::MemoryObjectInitializationRejected,
        result.GetError().RangeIndex == 1U,
        result.GetError().MemoryObject.Error == Kernel::Memory::MemoryObjectError::ResourceDomainPinRejected,
        result.GetError().MemoryObject.Pin == ObjectPinError::ReferenceExhausted,
        fixture->Owner.GetState() == Ia64InitialUserImageMemoryOwnerState::Failed,
        fixture->StagedOwner.GetState() == Platform::Ia64::Executable::Ia64InitialUserImageOwnerState::Claimed,
        usage.CommittedMemoryPages == 1U, usage.ResidentMemoryPages == 1U,
        usage.KernelMetadataBytes == sizeof(MemoryObject), fixture->Registry.Registry.GetActiveCount() == 1U);
    const auto repeated =
        fixture->Owner.Materialize(fixture->StagedOwner, fixture->Registry.Registry, fixture->Registry.Issuer,
                                   fixture->Domain.Identity, fixture->Domain.Domain);
    const auto repeatedUsage = fixture->Domain.Domain.GetSnapshot().Usage;
    ASTERIA_EXPECT(runner, "a terminal owner cannot retry or roll back its first committed MemoryObject",
                   !repeated.HasValue(),
                   repeated.GetError().Error == Ia64InitialUserImageMemoryOwnerError::AlreadyAttempted,
                   fixture->Owner.GetState() == Ia64InitialUserImageMemoryOwnerState::Failed,
                   ResourceUsageIsEqual(repeatedUsage, usage));

    Ia64InitialUserImageMemoryOwner replayOwner{};
    const Foundation::Size retainedObjectCount = fixture->Registry.Registry.GetActiveCount();
    const auto replayed =
        replayOwner.Materialize(fixture->StagedOwner, fixture->Registry.Registry, fixture->Registry.Issuer,
                                fixture->Domain.Identity, fixture->Domain.Domain);
    ASTERIA_EXPECT(
        runner, "a post-cut failure consumes the staged arena and a second owner cannot duplicate its retained state",
        !replayed.HasValue(), replayed.GetError().Error == Ia64InitialUserImageMemoryOwnerError::SourceNotStaged,
        replayOwner.GetState() == Ia64InitialUserImageMemoryOwnerState::Fresh,
        fixture->Owner.GetState() == Ia64InitialUserImageMemoryOwnerState::Failed,
        fixture->StagedOwner.GetState() == Platform::Ia64::Executable::Ia64InitialUserImageOwnerState::Claimed,
        fixture->Registry.Registry.GetActiveCount() == retainedObjectCount,
        ResourceUsageIsEqual(fixture->Domain.Domain.GetSnapshot().Usage, repeatedUsage));
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64InitialUserImageMemoryOwnerTests"};
    Asteria::Test::TestCurrentShapeAndRetryableSource(runner);
    Asteria::Test::TestIdentityPreflightFailures(runner);
    Asteria::Test::TestCapacityPreflightFailures(runner);
    Asteria::Test::TestExecuteOnlyAndZeroLength(runner);
    Asteria::Test::TestMaximumRangeCapacity(runner);
    Asteria::Test::TestLoadAfterStackAndRseCorridors(runner);
    Asteria::Test::TestTerminalFailureAfterSideEffects(runner);
    return runner.Finish();
}
