// SPDX-License-Identifier: Apache-2.0

#include "User/Pager/ToolkitLaunch.hpp"
#include "User/Pager/ComponentImage.hpp"
#include "User/Loader/ExecutableFormats.hpp"
#include "User/Session/SessionFrame.hpp"
#include "Inventory/ReportText.hpp"
namespace Asteria::User::Pager
{
namespace
{
namespace S = Session;
constinit LoadedImage g_image{};
ComponentRecord* g_demo{};
ComponentRecord* g_compositor{};
System::CapabilityHandle g_reply{};
Foundation::UInt64 g_id{};
Foundation::UInt64 g_transaction{};
bool g_registered{};
bool g_collecting{};
S::SessionMessage g_message{};
Foundation::UInt64 g_words[S::SessionMaximumWords]{};
alignas(8) Foundation::UInt8 g_frame[S::SessionKernelFrameLimit]{}, g_delivery[S::SessionDeliveryCapacity]{};
[[nodiscard]] bool Exchange(ScenarioContext& context, const Foundation::UInt32 method,
                            Abi::SystemCallError& error) noexcept
{
    const auto bytes = S::WriteSessionFrame(g_frame, method, ++g_transaction, g_words, &g_reply, 1U);
    if (bytes == 0U || context.Client->Send(g_compositor->Service, System::AddressOf(g_frame), bytes).Error !=
                           Abi::SystemCallError::None)
    {
        return false;
    }
    const auto received =
        context.Supervisor->AwaitMessageFor(*g_compositor, g_reply, g_delivery, sizeof(g_delivery), 200U, error);
    return S::ReadSessionFrame(g_delivery, static_cast<Foundation::Size>(received), g_message) &&
           g_message.Method == S::SessionReplyMethod && g_message.Transaction == g_transaction &&
           g_message.Words[1U] == static_cast<Foundation::UInt64>(S::SessionAnswer::Done);
}
} // Namespace
void StartToolkitPreview(ScenarioContext& context, ComponentRecord& compositor,
                         const System::CapabilityHandle reports) noexcept
{
    if (!FindComponentImage(context.Launch->ComponentImages, context.Launch->ComponentImagesBytes,
                            Abi::ComponentToolkitPreviewModule, g_image.Bytes, g_image.Size))
    {
        return;
    }
    Abi::SystemCallError error{Abi::SystemCallError::None};
    if (g_demo != nullptr)
    {
        return;
    }
    g_compositor = &compositor;
    if (Loader::DescribeExecutable(g_image.Bytes, g_image.Size, context.Launch->Component.Image, g_image.Layout) !=
            Loader::LoaderError::None ||
        (g_reply.Selector == 0U && !context.Calls->CreateEndpoint(context.Management, g_reply, error)))
    {
        return;
    }
    for (auto& word : g_words)
    {
        word = 0U;
    }
    g_words[1U] = S::SessionClassSystemModule;
    g_words[2U] = sizeof(Abi::ComponentToolkitDemoName) - 1U;
    const auto& name = Abi::ComponentToolkitDemoName;
    for (Foundation::Size i{}; i < sizeof(name) - 1U; ++i)
    {
        g_words[4U + i / 8U] |= static_cast<Foundation::UInt64>(name[i]) << ((i % 8U) * 8U);
    }
    if (!Exchange(context, S::SessionRegisterClientMethod, error) || g_message.AttachmentCount != 1U)
    {
        return;
    }
    g_id = g_message.Words[2U];
    g_registered = true;
    const auto client = g_message.Attachments[0U];
    const ComponentBuild build{.Image = &g_image,
                               .Tag = context.NextTag++,
                               .Behavior = Abi::ComponentBehaviorToolkit,
                               .Foreign = {},
                               .Report = reports,
                               .Directory = {},
                               .DriverService = client,
                               .DriverGeneration = context.Display->Broker->Tag,
                               .ServiceFlags = context.Bootstrap->Scanout.Format,
                               .Deferred = false};
    g_demo = context.Supervisor->Allocate();
    const bool started = g_demo != nullptr && context.Supervisor->Start(*g_demo, build, error);
    static_cast<void>(context.Calls->Close(client, error));
    char line[128U]{};
    Inventory::ReportText text{line, sizeof(line)};
    text.Text(started ? "# toolkit: component admitted\n" : "# toolkit: admission failed error=");
    if (!started)
    {
        text.Decimal(static_cast<Foundation::UInt64>(error));
        text.Text(" mappings=");
        text.Decimal(g_demo == nullptr ? 0U : g_demo->MappingCount);
        text.Character('\n');
    }
    WriteLine(context, line, text.Bytes());
    if (!started)
    {
        StopToolkitPreview(context);
    }
}
void StopToolkitPreview(ScenarioContext& context) noexcept
{
    if (g_collecting || (!g_registered && g_demo == nullptr))
    {
        return;
    }
    Abi::SystemCallError error{Abi::SystemCallError::None};
    if (g_demo != nullptr && g_demo->State == ComponentState::Running &&
        context.Supervisor->Stop(*g_demo, error) != PagerServeError::None)
    {
        return;
    }
    // Drop before domain drain so the compositor releases borrowed Frames.
    if (g_registered)
    {
        if (g_compositor == nullptr || g_compositor->State != ComponentState::Running)
        {
            return;
        }
        for (auto& word : g_words)
        {
            word = 0U;
        }
        g_words[0U] = g_id;
        if (!Exchange(context, S::SessionDropClientMethod, error))
        {
            // No acknowledgment within two seconds: stop the compositor;
            // ProductDesktop's existing recovery drains it first.
            static_cast<void>(context.Supervisor->Stop(*g_compositor, error));
            return;
        }
        g_registered = false;
    }
    CollectToolkitPreview(context);
}
void CollectToolkitPreview(ScenarioContext& context) noexcept
{
    if (g_collecting)
    {
        return;
    }
    g_collecting = true;
    Abi::SystemCallError error{Abi::SystemCallError::None};
    // Also called after compositor teardown, when all borrowed pins are gone.
    if (g_demo == nullptr || context.Supervisor->Teardown(*g_demo, error))
    {
        g_demo = nullptr;
        g_registered = false;
        g_id = 0U;
    }
    g_collecting = false;
}
void StepToolkitPreview(ScenarioContext& context) noexcept
{
    if (g_demo != nullptr && g_demo->State == ComponentState::Ended)
    {
        StopToolkitPreview(context);
    }
}
} // Namespace Asteria::User::Pager
