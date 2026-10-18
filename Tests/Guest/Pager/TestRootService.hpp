// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_GUEST_PAGER_TEST_ROOT_SERVICE_HPP_INCLUDED
#define ASTERIA_TESTS_GUEST_PAGER_TEST_ROOT_SERVICE_HPP_INCLUDED

#include "Guest/Pager/RootScenarios.hpp"
#include "User/Pager/RootService.hpp"

namespace Asteria::User::Pager
{
/// Test firmware orchestration. Each scenario drains before the next runs.
class TestRootService final : public RootService
{
public:
    [[nodiscard]] PagerServeOutcome Run(const System::SystemCallClient& client, const PagerBootstrap& bootstrap,
                                        PagerProgram& pager, const PagerThreadLaunch& launch) noexcept;

private:
    [[nodiscard, gnu::noinline]] bool RunAmd64ReturnBoundary(Abi::SystemCallError& error) noexcept;
    [[nodiscard, gnu::noinline]] bool RunAmd64BusyReturn(Abi::SystemCallError& error) noexcept;
    [[nodiscard, gnu::noinline]] bool RunFrameRights(Abi::SystemCallError& error) noexcept;
    [[nodiscard, gnu::noinline]] bool RunFaultChannel(Abi::SystemCallError& error) noexcept;
    [[nodiscard]] bool RunSessionProbe(const LoadedImage& image, Abi::SystemCallError& error) noexcept;
    [[nodiscard, gnu::noinline]] bool RunSessionInput(Abi::SystemCallError& error, bool largeMode = false) noexcept;
    [[nodiscard]] bool RunCompositor(Abi::SystemCallError& error) noexcept;
    [[nodiscard, gnu::noinline]] bool RunServices(Foundation::UInt64 echoTag,
                                                  Abi::SystemCallError& systemCall) noexcept;
};

[[nodiscard]] PagerServeOutcome RunTestRootService(const System::SystemCallClient& client,
                                                   const PagerBootstrap& bootstrap, PagerProgram& pager,
                                                   const PagerThreadLaunch& launch) noexcept;
} // namespace Asteria::User::Pager

#endif // ASTERIA_TESTS_GUEST_PAGER_TEST_ROOT_SERVICE_HPP_INCLUDED
