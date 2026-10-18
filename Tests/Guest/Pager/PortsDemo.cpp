// SPDX-License-Identifier: Apache-2.0
#include "Guest/Pager/RootScenarios.hpp"

#include "User/Pager/PagerWords.hpp"

namespace Asteria::User::Pager
{
PagerServeError RunPortsDemo(ScenarioContext& context, Abi::SystemCallError& systemCall) noexcept
{
    const auto& ports = context.Bootstrap->I8042;
    if (ports[0U].Selector == 0U)
    {
        constexpr char skipped[]{"# ports: i8042 not published, demo skipped\n"};
        WriteLine(context, skipped, sizeof(skipped) - 1U);
        return PagerServeError::None;
    }
    const auto& client = *context.Client;
    const auto status = client.PortIo(ports[1U], 0U, 1U, 0U);
    if (!Check(status, systemCall) || (status.Value0 == 0xFFU))
    {
        systemCall =
            (status.Error == Abi::SystemCallError::None) ? Abi::SystemCallError::InvalidConfiguration : status.Error;
        return PagerServeError::ManagementFailed;
    }
    if (!Check(client.PortIo(ports[1U], 0U, 1U, 1U, 0xADU), systemCall) ||
        !Check(client.PortIo(ports[1U], 0U, 1U, 1U, 0xAEU), systemCall))
    {
        return PagerServeError::ManagementFailed;
    }
    if (client.PortIo(ports[1U], 0U, 1U, 1U, 0xFEU).Error != Abi::SystemCallError::WriteValueDenied)
    {
        systemCall = Abi::SystemCallError::InvalidConfiguration;
        return PagerServeError::ManagementFailed;
    }
    System::CapabilityHandle waitSet{};
    System::CapabilityHandle interrupt{};
    context.Calls->ClearRequest();
    if (!context.Calls->Create(context.Bootstrap->Management, Abi::ObjectTypeWaitSet,
                               Abi::ObjectCreateWaitSetRequestSizeBytes, waitSet, systemCall))
    {
        return PagerServeError::ManagementFailed;
    }
    context.Calls->ClearRequest();
    StoreHandle(context.Calls->GetRequest(), ports[2U]);
    StoreWord(context.Calls->GetRequest() + Abi::ObjectCreateInterruptSourceOffset, Abi::InterruptSourceLine);
    if (!context.Calls->Create(context.Bootstrap->Management, Abi::ObjectTypeInterrupt,
                               Abi::ObjectCreateInterruptRequestSizeBytes, interrupt, systemCall) ||
        !Check(client.BindWaitSet(waitSet, interrupt, 0x8042U), systemCall) ||
        !Check(client.ControlInterrupt(interrupt, Abi::InterruptRearm), systemCall) ||
        !context.Calls->Destroy(interrupt, Abi::ObjectTypeInterrupt, systemCall) ||
        !context.Calls->Destroy(waitSet, Abi::ObjectTypeWaitSet, systemCall))
    {
        return PagerServeError::ManagementFailed;
    }
    constexpr char line[]{"# ports: i8042 status read, reset refused, keyboard line armed and masked\n"};
    WriteLine(context, line, sizeof(line) - 1U);
    return PagerServeError::None;
}

} // namespace Asteria::User::Pager
