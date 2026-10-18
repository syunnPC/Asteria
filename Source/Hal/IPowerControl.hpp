// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_HAL_I_POWER_CONTROL_HPP_INCLUDED
#define ASTERIA_HAL_I_POWER_CONTROL_HPP_INCLUDED
#include "Foundation/Types.hpp"
namespace Asteria::Hal
{
// PowerOff.md: bit 0 S5, bit 1 reset, bit 2 mapped EFI runtime services.
class IPowerControl
{
public:
    [[nodiscard]] virtual Foundation::UInt64 Available() const noexcept = 0;
    // Returns false before parking when unsupported. An admitted transition
    // is terminal, including a firmware write which unexpectedly returns.
    [[nodiscard]] virtual bool Transition(bool reboot) noexcept = 0;

protected:
    constexpr IPowerControl() noexcept = default;
    ~IPowerControl() noexcept = default;
};
}
#endif
