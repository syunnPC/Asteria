// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_ABI_WATCHDOG_HPP_INCLUDED
#define ASTERIA_ABI_WATCHDOG_HPP_INCLUDED

#include "Abi/ComponentBootstrap.hpp"

namespace Asteria::Abi
{
inline constexpr Foundation::UInt64 WatchdogDefaultSeconds{5U};
inline constexpr Foundation::UInt64 WatchdogMaximumSeconds{60U};
inline constexpr Foundation::UInt64 WatchdogCheckMicroseconds{250'000U};
inline constexpr Foundation::Size WatchdogServiceCount{5U};
inline constexpr Foundation::UInt64 WatchdogChecksPerSecond{1'000'000U / WatchdogCheckMicroseconds};
static_assert(WatchdogChecksPerSecond * WatchdogCheckMicroseconds == 1'000'000U);
inline constexpr Foundation::UInt64 WatchdogRestartPeriods{3U};
inline constexpr Foundation::UInt64 WatchdogRootComponent{0x10000U};
inline constexpr Foundation::UInt64 WatchdogInitialComponent{0x10001U};
inline constexpr Foundation::UInt64 WatchdogSpinnerComponent{0x10002U};
inline constexpr Foundation::UInt64 WatchdogUpdatingComponent{~Foundation::UInt64{0U}};
[[nodiscard]] constexpr const char* WatchdogComponentName(const Foundation::UInt64 kind) noexcept
{
    switch (kind)
    {
    case 0U:
        return "idle/kernel";
    case WatchdogRootComponent:
        return "root-service";
    case WatchdogInitialComponent:
        return "initial-user";
    case WatchdogSpinnerComponent:
        return "spinner";
    case WatchdogUpdatingComponent:
        return "updating";
    case ComponentBehaviorCrash:
        return "crash";
    case ComponentBehaviorReport:
        return "report";
    case ComponentBehaviorHang:
        return "hang";
    case ComponentBehaviorEcho:
        return "echo";
    case ComponentBehaviorClient:
        return "client";
    case ComponentBehaviorInterpret:
        return "interpreter";
    case ComponentBehaviorAccount:
        return "account";
    case ComponentBehaviorDriver:
        return "edu";
    case ComponentBehaviorDriverService:
        return "edu-service";
    case ComponentBehaviorInputClient:
        return "input-client";
    case ComponentBehaviorSessionProbe:
        return "session-probe";
    case ComponentBehaviorPatternClient:
        return "pattern-client";
    case ComponentBehaviorToolkit:
        return "toolkit";
    case ComponentBehaviorDisplay:
        return "display-broker";
    case ComponentBehaviorInput:
        return "input-broker";
    case ComponentBehaviorCompositor:
        return "session-compositor";
    case ComponentBehaviorPs2:
        return "ps2";
    case ComponentBehaviorXhci:
        return "usb/xhci";
    case ComponentBehaviorDirectory:
        return "directory";
    case ComponentBehaviorBlockDriver:
        return "ahci";
    case ComponentBehaviorFilesystem:
        return "filesystem";
    case ComponentBehaviorNicDriver:
        return "nic";
    case ComponentBehaviorNetwork:
        return "network";
    case ComponentBehaviorFaultSupervisor:
        return "fault-supervisor";
    case ComponentBehaviorAuth:
        return "auth";
    case ComponentBehaviorLogin:
        return "login";
    default:
        return "component";
    }
}
}
#endif
