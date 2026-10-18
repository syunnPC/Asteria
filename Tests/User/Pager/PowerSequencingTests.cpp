// SPDX-License-Identifier: Apache-2.0
#include "User/Pager/ComponentSupervisor.hpp"
#include "User/Component/FsFrame.hpp"
#include "Support/TestContext.hpp"
namespace
{
namespace Abi = Asteria::Abi;
namespace Foundation = Asteria::Foundation;
namespace TestSupport = Asteria::TestSupport;
namespace User = Asteria::User;
using Foundation::UInt64;
using Foundation::UInt8;
using Asteria::User::Pager::ComponentLoader;
using Asteria::User::Pager::ComponentMode;
using Asteria::User::Pager::ComponentNoDevice;
using Asteria::User::Pager::ComponentPlatform;
using Asteria::User::Pager::ComponentState;
using Asteria::User::Pager::ComponentSupervisor;
using Asteria::User::Pager::DeviceManagerRole;
using Asteria::User::Pager::ObjectCalls;
using Asteria::User::Pager::PagerBootstrap;
using Asteria::User::Pager::PagerProgram;
struct Model
{
    bool TaskStopped{false};
    bool EarlyFlush{false};
    bool ManagerFlush{false};
    bool Timeout{false};
    bool ReplyReady{false};
    UInt64 ReplyKind{User::Component::FsDone};
    UInt64 Waits{0U};
    UInt64 Flushes{0U};
    static void Call(void* context, const Abi::SystemCallRequest* request, Abi::SystemCallResponse* response) noexcept
    {
        auto& model = *static_cast<Model*>(context);
        *response = {};
        const auto& a = request->Arguments;
        switch (static_cast<Abi::SystemCallNumber>(request->Number))
        {
        case Abi::SystemCallNumber::ObjectCreate:
            response->Value0 = 100U;
            response->Value1 = 1U;
            break;
        case Abi::SystemCallNumber::ThreadTerminate:
            model.TaskStopped = true;
            break;
        case Abi::SystemCallNumber::ObjectDestroy:
            if (a[2U] == Abi::ObjectTypeThread)
            {
                if (a[0U] == 1U && model.TaskStopped)
                {
                    response->Value0 = Abi::KilledExitCode;
                }
                else
                {
                    response->Error = Abi::SystemCallError::WouldBlock;
                }
            }
            break;
        case Abi::SystemCallNumber::WaitSetWait:
            ++model.Waits;
            response->Error = Abi::SystemCallError::TimedOut;
            break;
        case Abi::SystemCallNumber::IpcSend:
            model.EarlyFlush = !model.TaskStopped;
            model.ManagerFlush = a[0U] == 32U;
            ++model.Flushes;
            model.ReplyReady = true;
            break;
        case Abi::SystemCallNumber::IpcReceive:
            if (model.ReplyReady && !model.Timeout)
            {
                auto* const buffer = reinterpret_cast<UInt8*>(a[2U]);
                const UInt64 words[User::Component::FsPayloadWords]{22U, model.ReplyKind, 0U, 0U};
                const auto size = User::Component::WriteFsFrame(buffer + User::Component::EchoDeliveryHeaderBytes,
                                                                User::Component::FsReplyMethod, 22U, words, nullptr, 0U,
                                                                nullptr, nullptr);
                response->Value1 = size + User::Component::EchoDeliveryHeaderBytes;
                model.ReplyReady = false;
            }
            else
            {
                response->Error = Abi::SystemCallError::WouldBlock;
            }
            break;
        default:
            break;
        }
    }
};
ComponentSupervisor supervisors[3U];
ComponentLoader loader;
DeviceManagerRole devices;
ObjectCalls calls;
} // namespace
int main() noexcept
{
    TestSupport::TestContext test{"PowerSequencingTests"};
    for (Foundation::Size variant{0U}; variant < 3U; ++variant)
    {
        Model model{};
        model.Timeout = variant == 2U;
        model.ReplyKind = variant == 1U ? User::Component::FsFailed : User::Component::FsDone;
        User::System::SystemCallClient client{{&Model::Call, &model}};
        calls.Configure(client);
        PagerBootstrap bootstrap{};
        bootstrap.Management = {99U, 1U};
        PagerProgram pager{client, bootstrap};
        ComponentPlatform platform{};
        platform.WatchdogMicroseconds = 1000U;
        auto& supervisor = supervisors[variant];
        supervisor.Configure(calls, loader, devices, pager, bootstrap, platform);
        auto* task = supervisor.Allocate();
        task->State = ComponentState::Running;
        task->Mode = ComponentMode::Task;
        task->Thread = {1U, 1U};
        task->Device = ComponentNoDevice;
        auto* fs = supervisor.Allocate();
        fs->State = ComponentState::Running;
        fs->Mode = ComponentMode::Service;
        fs->Thread = {2U, 1U};
        fs->Tag = 22U;
        fs->Behavior = Abi::ComponentBehaviorFilesystem;
        fs->Service = {22U, 1U};
        fs->FsManager = {32U, 1U};
        auto* driver = supervisor.Allocate();
        driver->State = ComponentState::Running;
        driver->Mode = ComponentMode::Service;
        driver->Thread = {3U, 1U};
        driver->Behavior = Abi::ComponentBehaviorBlockDriver;
        Abi::SystemCallError error{};
        const bool prepared = supervisor.PreparePower(error);
        ASTERIA_EXPECT(test, "stop tasks before flush, preserve storage until acknowledgement, close admission",
                       prepared == (variant == 0U), model.TaskStopped, !model.EarlyFlush, model.ManagerFlush,
                       model.Flushes == 1U, driver->State == ComponentState::Running, supervisor.Allocate() == nullptr);
        ASTERIA_EXPECT(test, "filesystem timeout is bounded", model.Waits <= 502U);
    }
    return test.Finish();
}
