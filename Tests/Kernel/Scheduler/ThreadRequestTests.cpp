// SPDX-License-Identifier: Apache-2.0
#include "Kernel/Scheduler/ThreadRequest.hpp"
#include "Kernel/Scheduler/ProcessorSet.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Scheduler/ProcessorScheduler.hpp"
#include "Platform/Synchronization/ProcessorContext.hpp"
#include "Support/ManualClock.hpp"
#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"
#include <atomic>
#include <barrier>
#include <thread>

namespace
{
namespace Foundation = Asteria::Foundation;
namespace Kernel = Asteria::Kernel;
namespace Platform = Asteria::Platform;
namespace TestSupport = Asteria::TestSupport;
using Asteria::Kernel::Scheduler::CpuId;
using Asteria::Kernel::Scheduler::ProcessorScheduler;
using Asteria::Kernel::Scheduler::SchedulingClass;
using Asteria::Kernel::Scheduler::SchedulingContext;
using Asteria::Kernel::Scheduler::SchedulingContextConfiguration;
using Asteria::Kernel::Scheduler::SchedulingContextError;
using Asteria::Kernel::Scheduler::SchedulingContextId;
using Asteria::Kernel::Scheduler::SchedulingCriticality;
using Asteria::Kernel::Scheduler::Thread;
using Asteria::Kernel::Scheduler::ThreadError;
using Asteria::Kernel::Scheduler::ThreadId;
using Asteria::Kernel::Scheduler::ThreadRequest;
using Asteria::Kernel::Scheduler::ThreadRequestKind;
using Asteria::Kernel::Scheduler::ThreadRunDisposition;
using Asteria::Kernel::Scheduler::ThreadState;
namespace Sync = Platform::Synchronization;

struct Model
{
    Platform::Synchronization::SpinLock Lock;
    TestSupport::TestObjectRegistry Registry{Lock};
    Kernel::Object::RootCapabilityIssuer Issuer{Registry};
    TestSupport::ManualClock Clocks[2]{TestSupport::ManualClock{1000U}, TestSupport::ManualClock{2000U}};
    ProcessorScheduler Schedulers[2]{{Registry, Clocks[0], CpuId{0U}}, {Registry, Clocks[1], CpuId{1U}}};
    Thread Threads[4]{};
    SchedulingContext Contexts[4]{SchedulingContext{Registry}, SchedulingContext{Registry}, SchedulingContext{Registry},
                                  SchedulingContext{Registry}};
    ThreadId Ids[4]{};
    SchedulingContextId ContextIds[4]{};
    ThreadRequest Requests[4]{};
    std::atomic<unsigned> Binds{0U}, Ends{0U}, Drains{0U};
    std::atomic<bool> Good{true};

    bool Initialize() noexcept
    {
        for (Foundation::UInt8 i{0U}; i < 4U; ++i)
        {
            const Foundation::UInt8 cpu = i % 2U;
            const SchedulingContextConfiguration configuration{.BudgetTicks = 20U,
                                                               .PeriodTicks = 100U,
                                                               .RelativeDeadlineTicks = 80U,
                                                               .Weight = 10U,
                                                               .Priority =
                                                                   static_cast<Foundation::UInt8>(i < 2U ? 8U : 2U),
                                                               .Criticality = SchedulingCriticality::Normal,
                                                               .CpuAffinityMask = Foundation::ProcessorMask{1U} << cpu,
                                                               .Class = SchedulingClass::DeadlineSporadic,
                                                               .Reserved = 0U};
            if (Threads[i].Initialize(Kernel::Object::ResourceDomainId{1U}) != ThreadError::None ||
                Contexts[i].Initialize(configuration, Kernel::Object::ResourceDomainId{1U},
                                       Kernel::Object::MonotonicTime{0U}) != SchedulingContextError::None)
            {
                return false;
            }
            const auto thread = Issuer.CreateObject(Threads[i]);
            const auto context = Issuer.CreateObject(Contexts[i]);
            if (!thread.HasValue() || !context.HasValue())
            {
                return false;
            }
            Ids[i] = ThreadId{Registry, thread.GetValue()};
            ContextIds[i] = SchedulingContextId{Registry, context.GetValue()};
            if (Contexts[i].Bind(Ids[i]) != SchedulingContextError::None || !Requests[i].Reserve())
            {
                return false;
            }
            Requests[i].Publish(Threads[i], CpuId{cpu}, true);
        }
        return true;
    }
    void Serve(const bool point) noexcept
    {
        const auto cpu = Sync::CurrentProcessor();
        for (Foundation::UInt8 i{0U}; i < 4U; ++i)
        {
            const auto request = Requests[i].Take(cpu, point ? nullptr : &Threads[cpu.GetValue()]);
            if (request == ThreadRequestKind::Bind)
            {
                const bool bound =
                    Contexts[i].SetInitialTime(Kernel::Object::MonotonicTime{Clocks[cpu.GetValue()].GetTicks()}) ==
                        SchedulingContextError::None &&
                    Threads[i].MakeReady() == ThreadError::None &&
                    Schedulers[cpu.GetValue()].Enqueue(ContextIds[i], Contexts[i], Ids[i], Threads[i]).HasValue();
                if (!bound)
                {
                    Good = false;
                }
                ++Binds;
                Requests[i].Complete();
            }
            else if (request == ThreadRequestKind::Terminate)
            {
                if (!Schedulers[cpu.GetValue()].Finish(ThreadRunDisposition::Exited, Requests[i].ExitCode()).HasValue())
                {
                    Good = false;
                }
                ++Ends;
                Requests[i].Complete();
            }
        }
    }
};

void CrossTerminate(TestSupport::TestContext& test)
{
    Model model;
    const bool initialized = model.Initialize();
    Sync::BindThreadRequests(&model,
                             [](void* owner, const bool point) noexcept
                             {
                                 static_cast<Model*>(owner)->Serve(point);
                             });
    std::barrier barrier{2};
    auto worker = [&](const Foundation::UInt8 cpu)
    {
        Sync::BindProcessorProgress(CpuId{cpu}, {.Context = &model,
                                                 .Drain = [](void* owner) noexcept
                                                 {
                                                     ++static_cast<Model*>(owner)->Drains;
                                                 }});
        // Bind the main thread first; the second bind is posted during its wait.
        const auto main = model.Requests[cpu].Take(CpuId{cpu}, nullptr);
        if (main != ThreadRequestKind::Bind ||
            model.Contexts[cpu].SetInitialTime(Kernel::Object::MonotonicTime{model.Clocks[cpu].GetTicks()}) !=
                SchedulingContextError::None ||
            model.Threads[cpu].MakeReady() != ThreadError::None ||
            !model.Schedulers[cpu]
                 .Enqueue(model.ContextIds[cpu], model.Contexts[cpu], model.Ids[cpu], model.Threads[cpu])
                 .HasValue() ||
            !model.Schedulers[cpu].DispatchNext().HasValue())
        {
            model.Good = false;
        }
        model.Requests[cpu].Complete();
        barrier.arrive_and_wait();
        const auto peer = static_cast<Foundation::UInt8>(cpu ^ 1U);
        if (!model.Requests[peer].PostTerminate(model.Threads[peer], 0x40U + cpu))
        {
            model.Good = false;
        }
        barrier.arrive_and_wait();
        Sync::ProcessorWaitProgress();
        // The wait drains and binds unrelated work, but cannot kill its own stack.
        if (model.Threads[cpu].GetSnapshot().State != ThreadState::Running ||
            model.Threads[cpu + 2U].GetSnapshot().State != ThreadState::Ready)
        {
            model.Good = false;
        }
        Sync::ServiceThreadRequests(true);
    };
    std::thread first{worker, Foundation::UInt8{0U}};
    std::thread second{worker, Foundation::UInt8{1U}};
    first.join();
    second.join();
    Sync::BindThreadRequests(nullptr, nullptr);
    ASTERIA_EXPECT(
        test, "two processors cross-terminate without waiting and serve other requests during waits", initialized,
        model.Good.load(), model.Binds == 2U, model.Ends == 2U, model.Drains == 2U,
        model.Threads[0].GetSnapshot().State == ThreadState::Exited,
        model.Threads[1].GetSnapshot().State == ThreadState::Exited, model.Threads[0].GetSnapshot().ExitCode == 0x41U,
        model.Threads[1].GetSnapshot().ExitCode == 0x40U,
        model.Contexts[2].GetSnapshot().AbsoluteDeadlineTicks == 1080U,
        model.Contexts[3].GetSnapshot().AbsoluteDeadlineTicks == 2080U,
        model.Contexts[0].SetInitialTime(Kernel::Object::MonotonicTime{5U}) == SchedulingContextError::AlreadyRunning,
        model.Contexts[2].SetInitialTime(Kernel::Object::MonotonicTime{5U}) == SchedulingContextError::InReadyQueue);
}

void RequestOwnership(TestSupport::TestContext& test)
{
    Thread thread;
    ThreadRequest request;
    const bool reserved = request.Reserve();
    request.Publish(thread, CpuId{1U}, false);
    std::atomic<unsigned> winners{0U};
    std::thread posters[4];
    for (unsigned i{0U}; i < 4U; ++i)
    {
        posters[i] = std::thread{[&, i]
                                 {
                                     if (request.PostTerminate(thread, 0x100U + i))
                                     {
                                         ++winners;
                                     }
                                 }};
    }
    for (auto& poster : posters)
    {
        poster.join();
    }
    const bool retirementRefused = !request.Retire(thread);
    const auto wrongHome = request.Take(CpuId{0U}, nullptr);
    const auto deferred = request.Take(CpuId{1U}, &thread);
    const auto served = request.Take(CpuId{1U}, nullptr);
    const auto code = request.ExitCode();
    const bool held = !request.PostTerminate(thread, 0x200U) && !request.Retire(thread);
    request.Complete();
    const bool retired = request.Retire(thread);
    const bool reservedRefusesPost = !request.PostTerminate(thread, 0x200U);
    request.Release();
    const bool reused = request.Reserve();
    ASTERIA_EXPECT(test, "CAS publishes exactly one payload and prevents retirement or reuse of a pending request",
                   reserved, winners == 1U, retirementRefused, wrongHome == ThreadRequestKind::None,
                   deferred == ThreadRequestKind::None, served == ThreadRequestKind::Terminate,
                   code >= 0x100U && code < 0x104U, held, retired, reservedRefusesPost, !request.Matches(thread),
                   reused);
}
void TerminalPowerPark(TestSupport::TestContext& test)
{
    Thread bindThread;
    Thread terminateThread;
    ThreadRequest bind;
    ThreadRequest terminate;
    ThreadRequest fresh;
    const bool reserved = bind.Reserve() && terminate.Reserve();
    bind.Publish(bindThread, CpuId{1U}, true);
    terminate.Publish(terminateThread, CpuId{1U}, false);
    const bool posted = terminate.PostTerminate(terminateThread, 99U);
    auto& processors = Sync::RuntimeProcessorSet();
    const bool admitted = processors.Admit(CpuId{0U}) && processors.Admit(CpuId{1U});
    unsigned callbacks{0U};
    Sync::BindThreadRequests(&callbacks,
                             [](void* owner, bool) noexcept
                             {
                                 ++*static_cast<unsigned*>(owner);
                             });
    Sync::BindProcessorKick(&callbacks,
                            [](void* owner, CpuId) noexcept
                            {
                                ++*static_cast<unsigned*>(owner);
                            });
    Sync::SetHostedProcessor(CpuId{0U});
    unsigned stopPolls{0U};
    Sync::BindProcessorProgress(CpuId{0U},
                                {
                                    .Context = &stopPolls,
                                    .Drain =
                                        [](void* owner) noexcept
                                    {
                                        *static_cast<unsigned*>(owner) += 100U;
                                    },
                                    .PollStop =
                                        [](void* owner) noexcept
                                    {
                                        ++*static_cast<unsigned*>(owner);
                                    },
                                });
    Sync::QueueProcessorKick(CpuId{1U});
    Sync::EnterProcessorLock(); // NMI parking can interrupt a held lock.
    Sync::BeginProcessorPowerPark();
    const bool parked = Sync::MarkProcessorParkedForPower(CpuId{1U});
    const bool stillLocked = Sync::HeldLockDepth() == 1U;
    Sync::LeaveProcessorLock();
    Sync::ServiceThreadRequests(true);
    Sync::ServiceWaitingThreadRequests();
    Sync::KickProcessor(CpuId{1U});
    Sync::ProcessorWaitProgress();
    ASTERIA_EXPECT(test,
                   "terminal power parking closes routing and retains pending requests without serving or waiting",
                   reserved, posted, admitted, parked, stillLocked, callbacks == 0U, stopPolls == 1U,
                   processors.OnlineMask() == 1U, processors.PowerParkedMask() == 2U,
                   processors.UserPlacementMask() == 0U, bind.Take(CpuId{1U}, nullptr) == ThreadRequestKind::None,
                   terminate.Take(CpuId{1U}, nullptr) == ThreadRequestKind::None,
                   !terminate.PostTerminate(terminateThread, 100U), !fresh.Reserve(), !bind.Retire(bindThread),
                   !terminate.Retire(terminateThread), bind.Matches(bindThread), terminate.Matches(terminateThread),
                   Sync::MarkProcessorParkedForPower(CpuId{1U}), !Sync::MarkProcessorParkedForPower(CpuId{}));
    Sync::BindThreadRequests(nullptr, nullptr);
    Sync::BindProcessorKick(nullptr, nullptr);
    Sync::BindProcessorProgress(CpuId{0U}, {});
}
} // namespace
int main()
{
    Asteria::TestSupport::TestContext test{"ThreadRequestTests"};
    CrossTerminate(test);
    RequestOwnership(test);
    TerminalPowerPark(test);
    return test.Finish();
}
