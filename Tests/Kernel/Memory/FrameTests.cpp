// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <thread>

#include "Kernel/Memory/Frame.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"

#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
using Kernel::Memory::BootFrame;
using Kernel::Memory::Frame;
using Kernel::Memory::FrameDescriptor;
using Kernel::Memory::FrameError;
using Kernel::Memory::FrameId;
using Kernel::Memory::FrameKind;
using Kernel::Memory::FrameSnapshot;
using Kernel::Memory::FrameState;
using Kernel::Memory::MemoryCachePolicy;
using Kernel::Memory::PageCount;
using Kernel::Memory::PhysicalAddress;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectRegistryError;
using Kernel::Object::ObjectType;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RootCapabilityIssuer;
using Platform::Synchronization::SpinLock;

[[nodiscard]] constexpr FrameDescriptor MakeRamDescriptor() noexcept
{
    return FrameDescriptor{
        .PhysicalStart = PhysicalAddress{0x0020'0000U},
        .Pages = PageCount{16U},
        .CachePolicy = MemoryCachePolicy::WriteBack,
        .Kind = FrameKind::Ram,
        .Reserved = {},
    };
}

[[nodiscard]] bool DescriptorIsEqual(const FrameDescriptor& left, const FrameDescriptor& right) noexcept
{
    if ((left.PhysicalStart.GetValue() != right.PhysicalStart.GetValue()) ||
        (left.Pages.GetValue() != right.Pages.GetValue()) || (left.CachePolicy != right.CachePolicy) ||
        (left.Kind != right.Kind))
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < sizeof(left.Reserved); ++index)
    {
        if ((left.Reserved[index] != 0U) || (right.Reserved[index] != 0U))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool SnapshotIsEqual(const FrameSnapshot& left, const FrameSnapshot& right) noexcept
{
    if (!DescriptorIsEqual(left.Descriptor, right.Descriptor) || (left.State != right.State) ||
        (left.ReadOnly != right.ReadOnly))
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < sizeof(left.Reserved); ++index)
    {
        if ((left.Reserved[index] != 0U) || (right.Reserved[index] != 0U))
        {
            return false;
        }
    }
    return true;
}

void TestInitializationAndRegistration(TestContext& runner) noexcept
{
    BootFrame frame;
    const FrameSnapshot initial = frame.GetSnapshot();
    ASTERIA_EXPECT(runner, "default Frame is inert and has no physical ownership", !frame.IsInitialized(),
                   frame.GetObjectType() == ObjectType::Frame, frame.GetChargeOwner().GetValue() == 0U,
                   initial.State == FrameState::Uninitialized, initial.Descriptor.PhysicalStart.GetValue() == 0U,
                   initial.Descriptor.Pages.GetValue() == 0U);

    SpinLock synchronization;
    Asteria::TestSupport::TestObjectRegistry registry{synchronization};
    RootCapabilityIssuer issuer{registry};
    const auto premature = issuer.CreateObject(frame);
    ASTERIA_EXPECT(runner, "ObjectRegistry rejects an uninitialized Frame", !premature.HasValue(),
                   premature.GetError() == ObjectRegistryError::InvalidArgument, registry.GetActiveCount() == 0U);

    const FrameDescriptor valid = MakeRamDescriptor();
    ASTERIA_EXPECT(runner, "invalid resource ownership is failure-atomic",
                   frame.Initialize(ResourceDomainId{}, valid) == FrameError::InvalidConfiguration,
                   SnapshotIsEqual(initial, frame.GetSnapshot()), frame.GetChargeOwner().GetValue() == 0U);

    FrameDescriptor invalid = valid;
    invalid.PhysicalStart = PhysicalAddress{};
    ASTERIA_EXPECT(runner, "physical page zero is not admitted as a Frame",
                   frame.Initialize(ResourceDomainId{1U}, invalid) == FrameError::InvalidDescriptor,
                   SnapshotIsEqual(initial, frame.GetSnapshot()), frame.GetChargeOwner().GetValue() == 0U);
    invalid = valid;
    invalid.PhysicalStart = PhysicalAddress{0x0020'0001U};
    ASTERIA_EXPECT(runner, "unaligned physical range is rejected",
                   frame.Initialize(ResourceDomainId{1U}, invalid) == FrameError::InvalidDescriptor,
                   SnapshotIsEqual(initial, frame.GetSnapshot()), frame.GetChargeOwner().GetValue() == 0U);
    invalid = valid;
    invalid.Pages = PageCount{};
    ASTERIA_EXPECT(runner, "empty physical range is rejected",
                   frame.Initialize(ResourceDomainId{1U}, invalid) == FrameError::InvalidDescriptor,
                   SnapshotIsEqual(initial, frame.GetSnapshot()), frame.GetChargeOwner().GetValue() == 0U);
    invalid = valid;
    invalid.Pages = PageCount{(~Foundation::UInt64{0U} / 4096U) + 1U};
    ASTERIA_EXPECT(runner, "page-to-byte multiplication overflow is rejected",
                   frame.Initialize(ResourceDomainId{1U}, invalid) == FrameError::InvalidDescriptor,
                   SnapshotIsEqual(initial, frame.GetSnapshot()), frame.GetChargeOwner().GetValue() == 0U);
    invalid = valid;
    invalid.PhysicalStart = PhysicalAddress{0xFFFF'FFFF'FFFF'F000ULL};
    invalid.Pages = PageCount{2U};
    ASTERIA_EXPECT(runner, "exclusive physical end overflow is rejected",
                   frame.Initialize(ResourceDomainId{1U}, invalid) == FrameError::InvalidDescriptor,
                   SnapshotIsEqual(initial, frame.GetSnapshot()), frame.GetChargeOwner().GetValue() == 0U);
    invalid = valid;
    invalid.CachePolicy = static_cast<MemoryCachePolicy>(0xFFU);
    ASTERIA_EXPECT(runner, "unknown cache policy is rejected",
                   frame.Initialize(ResourceDomainId{1U}, invalid) == FrameError::InvalidDescriptor,
                   SnapshotIsEqual(initial, frame.GetSnapshot()), frame.GetChargeOwner().GetValue() == 0U);
    invalid = valid;
    invalid.Kind = static_cast<FrameKind>(0xFFU);
    ASTERIA_EXPECT(runner, "unknown Frame kind is rejected",
                   frame.Initialize(ResourceDomainId{1U}, invalid) == FrameError::InvalidDescriptor,
                   SnapshotIsEqual(initial, frame.GetSnapshot()), frame.GetChargeOwner().GetValue() == 0U);
    invalid = valid;
    invalid.Kind = FrameKind::Device;
    ASTERIA_EXPECT(runner, "device ranges cannot be mapped with ordinary write-back caching",
                   frame.Initialize(ResourceDomainId{1U}, invalid) == FrameError::InvalidDescriptor,
                   SnapshotIsEqual(initial, frame.GetSnapshot()), frame.GetChargeOwner().GetValue() == 0U);
    for (Foundation::Size index{0U}; index < sizeof(invalid.Reserved); ++index)
    {
        invalid = valid;
        invalid.Reserved[index] = 1U;
        ASTERIA_EXPECT(runner, "every nonzero descriptor reserved byte is rejected",
                       frame.Initialize(ResourceDomainId{1U}, invalid) == FrameError::InvalidDescriptor,
                       SnapshotIsEqual(initial, frame.GetSnapshot()), frame.GetChargeOwner().GetValue() == 0U);
    }

    ASTERIA_EXPECT(runner, "valid initialization publishes one immutable RAM range",
                   frame.Initialize(ResourceDomainId{7U}, valid) == FrameError::None, frame.IsInitialized(),
                   frame.GetChargeOwner().GetValue() == 7U, DescriptorIsEqual(frame.GetSnapshot().Descriptor, valid),
                   frame.GetSnapshot().State == FrameState::Ready);
    const FrameSnapshot initialized = frame.GetSnapshot();
    ASTERIA_EXPECT(runner, "successful Frame initialization is single-use",
                   frame.Initialize(ResourceDomainId{8U}, valid) == FrameError::AlreadyInitialized,
                   SnapshotIsEqual(initialized, frame.GetSnapshot()), frame.GetChargeOwner().GetValue() == 7U);

    const auto registered = issuer.CreateObject(frame);
    ASTERIA_EXPECT(runner, "initialized Frame registers with a generation-bound typed identity", registered.HasValue(),
                   registry.GetActiveCount() == 1U, (FrameId{registry, registered.GetValue()}.IsValid()),
                   (FrameId{registry, registered.GetValue()}.IsFromRegistry(registry)));
    if (registered.HasValue())
    {
        SpinLock otherSynchronization;
        Asteria::TestSupport::TestObjectRegistry otherRegistry{otherSynchronization};
        const FrameId localId{registry, registered.GetValue()};
        const FrameId foreignId{otherRegistry, registered.GetValue()};
        ASTERIA_EXPECT(runner, "FrameId equality includes originating registry provenance", localId.IsEqualTo(localId),
                       !localId.IsEqualTo(foreignId), FrameId{}.IsEqualTo(FrameId{}), !FrameId{}.IsValid());

        const auto destroyed = issuer.DestroyObject(registered.GetValue());
        const auto collected = issuer.CollectDestroyedObject();
        ASTERIA_EXPECT(runner, "registered Frame is collected before its storage expires",
                       destroyed == ObjectRegistryError::Ok, collected.HasValue(),
                       collected.GetValue().Instance == &frame);
    }
}

void TestCachePoliciesAndUpperBoundary(TestContext& runner) noexcept
{
    for (const MemoryCachePolicy cachePolicy : {MemoryCachePolicy::Uncached, MemoryCachePolicy::WriteCombining})
    {
        BootFrame frame;
        FrameDescriptor descriptor{
            .PhysicalStart = PhysicalAddress{0x8000'0000U},
            .Pages = PageCount{1U},
            .CachePolicy = cachePolicy,
            .Kind = FrameKind::Device,
            .Reserved = {},
        };
        ASTERIA_EXPECT(runner, "device Frame accepts uncached and write-combining policy",
                       frame.Initialize(ResourceDomainId{10U}, descriptor) == FrameError::None,
                       DescriptorIsEqual(frame.GetSnapshot().Descriptor, descriptor));
    }

    for (const MemoryCachePolicy cachePolicy : {MemoryCachePolicy::WriteBack, MemoryCachePolicy::Uncached,
                                                MemoryCachePolicy::WriteCombining, MemoryCachePolicy::WriteThrough})
    {
        BootFrame frame;
        FrameDescriptor descriptor{
            .PhysicalStart = PhysicalAddress{0x4000'0000U},
            .Pages = PageCount{1U},
            .CachePolicy = cachePolicy,
            .Kind = FrameKind::Ram,
            .Reserved = {},
        };
        ASTERIA_EXPECT(runner, "RAM Frame retains every known platform cache policy",
                       frame.Initialize(ResourceDomainId{11U}, descriptor) == FrameError::None);
    }

    BootFrame boundary;
    const FrameDescriptor boundaryDescriptor{
        .PhysicalStart = PhysicalAddress{0xFFFF'FFFF'FFFF'E000ULL},
        .Pages = PageCount{1U},
        .CachePolicy = MemoryCachePolicy::Uncached,
        .Kind = FrameKind::Device,
        .Reserved = {},
    };
    ASTERIA_EXPECT(runner, "last complete aligned physical page below UInt64 exhaustion is valid",
                   boundary.Initialize(ResourceDomainId{12U}, boundaryDescriptor) == FrameError::None);
}

void TestConcurrentInitialization(TestContext& runner) noexcept
{
    BootFrame frame;
    const FrameDescriptor firstDescriptor = MakeRamDescriptor();
    FrameDescriptor secondDescriptor = MakeRamDescriptor();
    secondDescriptor.PhysicalStart = PhysicalAddress{0x0040'0000U};

    std::atomic<bool> start{false};
    FrameError errors[2]{};
    auto initialize =
        [&](const Foundation::Size index, const ResourceDomainId owner, const FrameDescriptor descriptor) noexcept
    {
        while (!start.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
        errors[index] = frame.Initialize(owner, descriptor);
    };

    std::thread first{initialize, Foundation::Size{0U}, ResourceDomainId{20U}, firstDescriptor};
    std::thread second{initialize, Foundation::Size{1U}, ResourceDomainId{21U}, secondDescriptor};
    start.store(true, std::memory_order_release);
    first.join();
    second.join();

    const bool firstWon = errors[0] == FrameError::None;
    const bool secondWon = errors[1] == FrameError::None;
    const FrameSnapshot snapshot = frame.GetSnapshot();
    ASTERIA_EXPECT(runner, "concurrent initialization publishes exactly one Frame descriptor", firstWon != secondWon,
                   errors[firstWon ? 1U : 0U] == FrameError::AlreadyInitialized, snapshot.State == FrameState::Ready);
    ASTERIA_EXPECT(
        runner, "descriptor and charge owner are published from the same winning attempt",
        firstWon
            ? (DescriptorIsEqual(snapshot.Descriptor, firstDescriptor) && (frame.GetChargeOwner().GetValue() == 20U))
            : (DescriptorIsEqual(snapshot.Descriptor, secondDescriptor) && (frame.GetChargeOwner().GetValue() == 21U)));
}
} // namespace Asteria::Test

int main()
{
    TestContext runner{"FrameTests"};
    Asteria::Test::TestInitializationAndRegistration(runner);
    Asteria::Test::TestCachePoliciesAndUpperBoundary(runner);
    Asteria::Test::TestConcurrentInitialization(runner);
    return runner.Finish();
}
