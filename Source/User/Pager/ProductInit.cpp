// SPDX-License-Identifier: Apache-2.0

#include "User/Pager/RootService.hpp"

#include "User/Pager/DeviceInventory.hpp"
#include "User/Pager/InputTerminal.hpp"
#include "User/Pager/ProductDesktop.hpp"

namespace Asteria::User::Pager
{
namespace
{
[[nodiscard]] constexpr PagerServeOutcome MakeOutcome(const PagerServeError error,
                                                      const Abi::SystemCallError systemCall) noexcept
{
    return PagerServeOutcome{.Error = error, .SystemCall = systemCall, .Resolved = 0U};
}
} // namespace

PagerServeOutcome RootService::Run(const System::SystemCallClient& client, const PagerBootstrap& bootstrap,
                                   PagerProgram& pager, const PagerThreadLaunch& launch) noexcept
{
    Configure(client, bootstrap, pager, launch);
    ConfigureProductDesktop(m_context);
    Abi::SystemCallError systemCall{Abi::SystemCallError::None};
    if (!FindImages() || !m_calls.CreateEndpoint(bootstrap.Management, m_context.Report, systemCall))
    {
        return MakeOutcome(PagerServeError::ManagementFailed, systemCall);
    }
    if (!Ran("display", StartDisplay(systemCall)))
    {
        if (bootstrap.SessionMode != Abi::SessionDesktop)
        {
            return Abandon(systemCall);
        }
        // Optional GUI admission must not abandon the trusted console path.
        auto* const failed = m_supervisor.FindByTag(m_displayTag);
        if (failed != nullptr)
        {
            if (failed->State == ComponentState::Running)
            {
                static_cast<void>(m_supervisor.Stop(*failed, systemCall));
            }
            static_cast<void>(m_supervisor.Teardown(*failed, systemCall));
        }
        m_broker = nullptr;
        m_display.Broker = nullptr;
        m_display.Console = {};
        FallbackProductDesktop(m_context, "display-admission");
        systemCall = Abi::SystemCallError::None;
    }
    ReadDeviceInventory(m_context);
    m_context.NextTag = 1U;
    if (bootstrap.SessionMode == Abi::SessionDesktop || (bootstrap.I8042[0U].Selector != 0U) ||
        m_devices.IsPresent(Abi::PagerDeviceXhci) || m_devices.IsPresent(Abi::PagerDeviceUsbHost))
    {
        if (!Ran("input", m_calls.CreateEndpoint(bootstrap.Management, m_context.Reply, systemCall)
                              ? PagerServeError::None
                              : PagerServeError::ManagementFailed) ||
            !Ran("input", StartInputTerminal(m_context, systemCall)))
        {
            return Abandon(systemCall);
        }
    }
    else
    {
        constexpr char ready[]{"# init: ready: display\n"};
        WriteLine(m_context, ready, sizeof(ready) - 1U);
        // Keep service liveness and pager faults serviced on machines
        // without input hardware as well (Watchdog.md).
        if (m_launch->Component.Clock != nullptr)
        {
            for (;;)
            {
                const auto stepped = m_supervisor.Step(10'000U, systemCall);
                if (stepped != PagerServeError::None)
                {
                    return Abandon(systemCall);
                }
            }
        }
    }
    return MakeOutcome(PagerServeError::None, Abi::SystemCallError::None);
}

PagerServeOutcome RunRootService(const System::SystemCallClient& client, const PagerBootstrap& bootstrap,
                                 PagerProgram& pager, const PagerThreadLaunch& launch) noexcept
{
    static constinit RootService root{};
    return root.Run(client, bootstrap, pager, launch);
}
} // namespace Asteria::User::Pager
