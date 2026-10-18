// SPDX-License-Identifier: Apache-2.0
#include "Kernel/Interrupt/Interrupt.hpp"
#include "Kernel/Interrupt/MessageSignaledSource.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Platform/Amd64/Interrupt/Amd64DeviceVectors.hpp"

#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"
#include "Support/Random.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::UInt64;
using Hal::Interrupt::InterruptSourceError;
using Kernel::Interrupt::Interrupt;
using Kernel::Interrupt::InterruptError;
using Kernel::Interrupt::InterruptState;
using Kernel::Interrupt::MessageSignaledSource;
using Kernel::Ipc::Notification;
using Kernel::Ipc::NotificationError;
using Kernel::Ipc::NotificationId;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RootCapabilityIssuer;
using Platform::Amd64::Interrupt::Amd64DeviceVectors;
using Platform::Synchronization::SpinLock;

// A message-signaled source tracks ownership and a software mask only, every operation lock-free.
void TestMessageSignaledSource(TestContext& runner) noexcept
{
    MessageSignaledSource source;
    const bool idle = !source.IsClaimed();
    const InterruptSourceError unclaimed = source.Unmask();
    const InterruptSourceError claimed = source.Claim();
    const bool maskedAtClaim = source.IsMasked();
    const InterruptSourceError again = source.Claim();
    const InterruptSourceError unmasked = source.Unmask();
    const bool open = !source.IsMasked();
    const InterruptSourceError acknowledged = source.MaskAndAcknowledge();
    const bool closed = source.IsMasked();
    const InterruptSourceError released = source.Release();
    ASTERIA_EXPECT(runner, "a message-signaled source is claimed masked, unmasked and masked in software", idle,
                   unclaimed == InterruptSourceError::NotClaimed, claimed == InterruptSourceError::None, maskedAtClaim,
                   again == InterruptSourceError::AlreadyClaimed, unmasked == InterruptSourceError::None, open,
                   acknowledged == InterruptSourceError::None, closed, released == InterruptSourceError::None,
                   !source.IsClaimed(), source.Release() == InterruptSourceError::NotClaimed);
}

// A device vector enters the Interrupt bound to it, which signals its Notification once armed; an
// unbound vector is counted as spurious and never stops the machine.
void TestDeviceVectors(TestContext& runner) noexcept
{
    SpinLock lock;
    Asteria::TestSupport::TestObjectRegistry registry{lock};
    RootCapabilityIssuer issuer{registry};
    Notification notification;
    MessageSignaledSource source;
    Interrupt interrupt{registry, source};
    const bool notified = notification.Initialize(ResourceDomainId{7U}) == NotificationError::None;
    const auto object = issuer.CreateObject(notification);
    const bool initialized =
        object.HasValue() &&
        (interrupt.Initialize(ResourceDomainId{7U}, NotificationId{registry, object.GetValue()}, notification).Error ==
         InterruptError::None);
    Amd64DeviceVectors vectors;
    const bool undirected = vectors.Bind(interrupt).Vector == 0U;
    const bool broadcast = !vectors.SetDestination(0xFFU);
    const bool directed = vectors.SetDestination(2U);
    const auto target = vectors.Bind(interrupt);
    const UInt64 vector = target.Vector;
    vectors.Dispatch(vector);
    const InterruptState whileMasked = interrupt.GetSnapshot().State;
    const bool armed = interrupt.Arm().Error == InterruptError::None;
    vectors.Dispatch(vector);
    const InterruptState delivered = interrupt.GetSnapshot().State;
    vectors.Dispatch(0x4FU);
    vectors.Dispatch(0x30U);
    ASTERIA_EXPECT(runner, "a bound vector enters its Interrupt, and unbound vectors are only counted", notified,
                   initialized, undirected, broadcast, directed, vector == Amd64DeviceVectors::FirstVector,
                   target.Address == 0xFEE0'2000U, target.Data == Amd64DeviceVectors::FirstVector,
                   whileMasked == InterruptState::Masked, armed, delivered == InterruptState::Pending,
                   source.IsMasked(), vectors.GetDelivered() == 2U, vectors.GetSpurious() == 2U,
                   Amd64DeviceVectors::IsDeviceVector(0x4FU), !Amd64DeviceVectors::IsDeviceVector(0x50U));

    Interrupt other{registry, source};
    const bool foreign = !vectors.Unbind(vector, other);
    const bool unbound = vectors.Unbind(vector, interrupt);
    vectors.Dispatch(vector);
    ASTERIA_EXPECT(runner, "only the bound Interrupt frees its vector, which is spurious afterwards; binding rotates",
                   foreign, unbound, vectors.GetSpurious() == 3U,
                   vectors.Bind(interrupt).Vector == (Amd64DeviceVectors::FirstVector + 1U));
}
void TestReservationsModel(TestContext& context) noexcept
{
    const auto seeds = TestSupport::GetSeedRange(0x8042U, 8U);
    for (UInt64 run{0U}; run < seeds.Count; ++run)
    {
        const UInt64 seed = seeds.First + run;
        TestSupport::Random random{seed};
        Amd64DeviceVectors vectors;
        static_cast<void>(vectors.SetDestination(2U));
        bool held[16U]{};
        Foundation::Size next{0U};
        for (Foundation::Size step{0U}; step < 512U; ++step)
        {
            const UInt64 operation = random.Below(3U);
            bool matches{true};
            if (operation == 0U)
            {
                UInt64 expected{0U};
                for (Foundation::Size scan{0U}; scan < 16U; ++scan)
                {
                    const Foundation::Size index = (next + scan) % 16U;
                    if (!held[index])
                    {
                        held[index] = true;
                        next = index + 1U;
                        expected = 0x40U + index;
                        break;
                    }
                }
                matches = vectors.Reserve() == expected;
            }
            else
            {
                const Foundation::Size index = random.Below(16U);
                vectors.Release(0x40U + index);
                held[index] = false;
            }
            if (!matches)
            {
                static_cast<void>(std::fprintf(stderr, "vector model seed=%llu step=%zu operation=%llu\n",
                                               static_cast<unsigned long long>(seed), step,
                                               static_cast<unsigned long long>(operation)));
            }
            ASTERIA_EXPECT(context, "shared reservations match the rotating bounded allocator model", matches);
        }
    }
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"Amd64DeviceVectorsTests"};
    Asteria::Test::TestMessageSignaledSource(runner);
    Asteria::Test::TestDeviceVectors(runner);
    Asteria::Test::TestReservationsModel(runner);
    return runner.Finish();
}
