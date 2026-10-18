// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_PAGER_ROOT_SERVICE_HPP_INCLUDED
#define ASTERIA_USER_PAGER_ROOT_SERVICE_HPP_INCLUDED

#include "Abi/SystemCallAbi.hpp"
#include "Foundation/Types.hpp"
#include "User/Component/DisplayFrame.hpp"
#include "User/Pager/ComponentLoader.hpp"
#include "User/Pager/ComponentSupervisor.hpp"
#include "User/Pager/DeviceManagerRole.hpp"
#include "User/Pager/ObjectCalls.hpp"
#include "User/Pager/PagerProgram.hpp"
#include "User/Pager/RootContext.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::Pager
{
/// Product init and component ownership. Helpers are shared by root-service
/// link units; the object and its message buffers live in static storage.
/// Link units may extend orchestration through the protected helpers; there
/// is no polymorphic ownership. No allocation; runs on the pager thread and
/// is not thread-safe.
class RootService
{
public:
    constexpr RootService() noexcept = default;

    RootService(const RootService&) = delete;
    RootService& operator=(const RootService&) = delete;

    /// Starts resident console services and echoes published PS/2 or USB
    /// input. Input driver failures retain the console and other sources;
    /// display and Input Broker failures abandon essential services.
    [[nodiscard]] PagerServeOutcome Run(const System::SystemCallClient& client, const PagerBootstrap& bootstrap,
                                        PagerProgram& pager, const PagerThreadLaunch& launch) noexcept;

protected:
    static bool RecoverWatchdogDisplay(void*, ComponentRecord&, Abi::SystemCallError&) noexcept;
    /// Configures the parts and the scenarios' context for one run.
    void Configure(const System::SystemCallClient& client, const PagerBootstrap& bootstrap, PagerProgram& pager,
                   const PagerThreadLaunch& launch) noexcept;
    /// Finds and describes the `component` and `display` images, and the `directory` and
    /// `client` images where the table has them.
    [[nodiscard, gnu::noinline]] bool FindImages() noexcept;
    /// Starts the display broker where the boot handed a scanout and components have domains of
    /// their own and can wait, asks what it draws, mirrors the lines from then on and writes the
    /// first; nothing where it cannot run.
    [[nodiscard, gnu::noinline]] PagerServeError StartDisplay(Abi::SystemCallError& systemCall) noexcept;
    /// A Query the broker answers on the report Endpoint, while no other
    /// component runs: a fence after which every line sent is on the screen.
    [[nodiscard, gnu::noinline]] bool QueryDisplay(Component::DisplayInfo& info,
                                                   Abi::SystemCallError& systemCall) noexcept;
    /// Fences the broker, stops the mirroring, kills and tears the broker
    /// down, and says so on the debug channel.
    [[nodiscard, gnu::noinline]] PagerServeError StopDisplay(Abi::SystemCallError& systemCall) noexcept;
    /// Whether the scenario named ran as served says; records which failed
    /// and how otherwise.
    [[nodiscard]] bool Ran(const char* scenario, PagerServeError served) noexcept;
    /// Ends the run after the failure Ran recorded: the supervisor abandons
    /// every component, and a line names the scenario and says whether its
    /// components were torn down or left standing.
    [[nodiscard, gnu::noinline]] PagerServeOutcome Abandon(Abi::SystemCallError systemCall) noexcept;

    const PagerBootstrap* m_bootstrap{nullptr};
    const PagerThreadLaunch* m_launch{nullptr};
    ObjectCalls m_calls{};
    ComponentLoader m_loader{};
    ComponentSupervisor m_supervisor{};
    /// The Device Manager role: the assignable device and its grants.
    DeviceManagerRole m_devices{};
    ScenarioContext m_context{};
    /// The display broker while it runs, and the mirror of the lines.
    ComponentRecord* m_broker{nullptr};
    Foundation::UInt64 m_displayTag{0x100U};
    DisplayMirror m_display{};
    /// The scenario that failed, and how.
    const char* m_failedScenario{nullptr};
    PagerServeError m_failedError{};
};

/// The PagerRootService of a platform whose pager builds components: runs
/// the one RootService of the pager image.
[[nodiscard]] PagerServeOutcome RunRootService(const System::SystemCallClient& client, const PagerBootstrap& bootstrap,
                                               PagerProgram& pager, const PagerThreadLaunch& launch) noexcept;
} // Namespace Asteria::User::Pager

#endif // ASTERIA_USER_PAGER_ROOT_SERVICE_HPP_INCLUDED
