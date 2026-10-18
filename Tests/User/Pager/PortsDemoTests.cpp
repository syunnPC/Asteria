// SPDX-License-Identifier: Apache-2.0
#include <memory>
#include <string>

#include "Guest/Pager/RootScenarios.hpp"
#include "Support/TestContext.hpp"

namespace Asteria::Test
{
namespace
{
struct PortsModel
{
    Foundation::Size Reads{0U};
    Foundation::Size OtherCalls{0U};
    std::string Text{};
};

void Invoke(void* const context, const Abi::SystemCallRequest* const request,
            Abi::SystemCallResponse* const response) noexcept
{
    auto& model = *static_cast<PortsModel*>(context);
    *response = {.Error = Abi::SystemCallError::None, .Value0 = 0U, .Value1 = 0U};
    if (request->Number == static_cast<Foundation::UInt64>(Abi::SystemCallNumber::PortIo))
    {
        ++model.Reads;
        response->Value0 = 0xFFU;
    }
    else if (request->Number == static_cast<Foundation::UInt64>(Abi::SystemCallNumber::DebugWrite))
    {
        model.Text.assign(reinterpret_cast<const char*>(request->Arguments[2U]), request->Arguments[3U]);
    }
    else
    {
        ++model.OtherCalls;
    }
}

void TestAbsentPorts(TestSupport::TestContext& test) noexcept
{
    PortsModel model;
    const User::System::SystemCallClient client{{.Function = Invoke, .Context = &model}};
    const auto bootstrap = std::make_unique<User::Pager::PagerBootstrap>();
    const auto context = std::make_unique<User::Pager::ScenarioContext>();
    context->Client = &client;
    context->Bootstrap = bootstrap.get();
    Abi::SystemCallError error{Abi::SystemCallError::None};
    ASTERIA_EXPECT(test, "unpublished i8042 skips without touching ports or management",
                   User::Pager::RunPortsDemo(*context, error) == User::Pager::PagerServeError::None, model.Reads == 0U,
                   model.OtherCalls == 0U, model.Text == "# ports: i8042 not published, demo skipped\n");
    bootstrap->I8042[0U].Selector = 1U;
    bootstrap->I8042[1U].Selector = 2U;
    model.Text.clear();
    ASTERIA_EXPECT(test, "floating status 0xFF fails before any controller write or line creation",
                   User::Pager::RunPortsDemo(*context, error) == User::Pager::PagerServeError::ManagementFailed,
                   error == Abi::SystemCallError::InvalidConfiguration, model.Reads == 1U, model.OtherCalls == 0U,
                   model.Text.empty());
}
} // namespace
} // namespace Asteria::Test

int main()
{
    Asteria::TestSupport::TestContext context{"PortsDemoTests"};
    Asteria::Test::TestAbsentPorts(context);
    return context.Finish();
}
