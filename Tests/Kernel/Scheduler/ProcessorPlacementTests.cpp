// SPDX-License-Identifier: Apache-2.0
#include "Kernel/Scheduler/ProcessorPlacement.hpp"
#include "Kernel/Scheduler/ProcessorSet.hpp"
#include "Kernel/Scheduler/SchedulingContext.hpp"
#include "Kernel/Scheduler/Thread.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

int main()
{
    namespace Foundation = Asteria::Foundation;
    namespace Kernel = Asteria::Kernel;
    namespace Platform = Asteria::Platform;
    namespace TestSupport = Asteria::TestSupport;
    using Asteria::Kernel::Scheduler::CpuId;
    using Asteria::Kernel::Scheduler::MaximumProcessorCount;
    using Asteria::Kernel::Scheduler::MaximumReadyQueueEntries;
    using Asteria::Kernel::Scheduler::ProcessorSet;
    using Asteria::Kernel::Scheduler::SchedulingClass;
    using Asteria::Kernel::Scheduler::SchedulingContext;
    using Asteria::Kernel::Scheduler::SchedulingContextConfiguration;
    using Asteria::Kernel::Scheduler::SchedulingContextError;
    using Asteria::Kernel::Scheduler::SchedulingCriticality;
    using Asteria::Kernel::Scheduler::SelectProcessorHome;
    using Asteria::Kernel::Scheduler::Thread;
    using Asteria::Kernel::Scheduler::ThreadError;
    using Asteria::Kernel::Scheduler::ThreadId;
    TestSupport::TestContext test{"ProcessorPlacementTests"};
    Foundation::Size loads[MaximumProcessorCount]{3U, 1U, 1U, 1U};
    CpuId last{0U};
    for (Foundation::UInt8 cpu{1U}; cpu < MaximumProcessorCount; ++cpu)
    {
        const auto home = SelectProcessorHome(15U, loads, last);
        ASTERIA_EXPECT(test, "minimum load with round-robin tie", home.GetValue() == cpu);
        ++loads[cpu];
        last = home;
    }
    ASTERIA_EXPECT(test, "lower load beats BSP tie position", SelectProcessorHome(15U, loads, last).GetValue() == 1U);
    loads[0U] = 2U;
    ASTERIA_EXPECT(test, "BSP participates in eligible ties", SelectProcessorHome(15U, loads, last).GetValue() == 0U);
    ProcessorSet processors;
    for (Foundation::UInt8 cpu{0U}; cpu < MaximumProcessorCount; ++cpu)
    {
        ASTERIA_EXPECT(test, "admit eligible processors", processors.Admit(CpuId{cpu}));
    }
    processors.ExcludeUserPlacement(CpuId{1U});
    loads[1U] = 0U;
    ASTERIA_EXPECT(test, "certain-skew processor is excluded despite smaller load",
                   SelectProcessorHome(processors.UserPlacementMask(), loads, CpuId{0U}).GetValue() == 2U);
    for (auto& load : loads)
    {
        load = MaximumReadyQueueEntries;
    }
    ASTERIA_EXPECT(test, "capacity exhausted before posting a bind", !SelectProcessorHome(15U, loads, last).IsValid());
    loads[3U] = MaximumReadyQueueEntries - 1U;
    ASTERIA_EXPECT(test, "last available binding wins", SelectProcessorHome(15U, loads, last).GetValue() == 3U);
    processors.BeginPowerPark();
    ASTERIA_EXPECT(test, "power fence refuses placement",
                   !SelectProcessorHome(processors.UserPlacementMask(), loads, last).IsValid());

    Platform::Synchronization::SpinLock lock;
    TestSupport::TestObjectRegistry registry{lock};
    Kernel::Object::RootCapabilityIssuer issuer{registry};
    Thread thread;
    SchedulingContext context{registry};
    ASTERIA_EXPECT(test, "home requires initialization",
                   context.SetInitialHome(CpuId{1U}) == SchedulingContextError::NotInitialized);
    const SchedulingContextConfiguration configuration{
        20U, 100U, 80U, 1U, 1U, SchedulingCriticality::Normal, 1U, SchedulingClass::DeadlineSporadic, 0U};
    ASTERIA_EXPECT(test, "thread initialized",
                   thread.Initialize(Kernel::Object::ResourceDomainId{1U}) == ThreadError::None);
    const auto identity = issuer.CreateObject(thread);
    ASTERIA_EXPECT(test, "thread registered", identity.HasValue());
    ASTERIA_EXPECT(test, "context initialized",
                   context.Initialize(configuration, Kernel::Object::ResourceDomainId{1U},
                                      Kernel::Object::MonotonicTime{100U}) == SchedulingContextError::None);
    ASTERIA_EXPECT(test, "home requires its one thread",
                   context.SetInitialHome(CpuId{2U}) == SchedulingContextError::NotBound);
    ASTERIA_EXPECT(test, "context bound",
                   context.Bind(ThreadId{registry, identity.GetValue()}) == SchedulingContextError::None);
    ASTERIA_EXPECT(test, "fresh context accepts selected home",
                   context.SetInitialHome(CpuId{2U}) == SchedulingContextError::None);
    ASTERIA_EXPECT(test, "affinity is exactly the home bit", context.GetConfiguration().CpuAffinityMask == 4U);
    ASTERIA_EXPECT(test, "invalid home refused",
                   context.SetInitialHome(CpuId{}) == SchedulingContextError::InvalidConfiguration);
    ASTERIA_EXPECT(test, "home initializes local admission time",
                   context.SetInitialTime(Kernel::Object::MonotonicTime{900U}) == SchedulingContextError::None);
    return test.Finish();
}
