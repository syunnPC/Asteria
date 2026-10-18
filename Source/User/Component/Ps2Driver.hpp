// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_COMPONENT_PS2_DRIVER_HPP_INCLUDED
#define ASTERIA_USER_COMPONENT_PS2_DRIVER_HPP_INCLUDED
#include "Abi/ComponentBootstrap.hpp"
#include "User/Component/ComponentProgram.hpp"
#include "User/Input/Ps2Diagnostic.hpp"
#include "User/System/SystemCallClient.hpp"
namespace Asteria::User::Component
{
/// The ready report's method: bits 0-15 hold Ps2ReadyMethod, or
/// Ps2ReadyAuxFailedMethod when the auxiliary probe failed, and bits 16-19
/// the keyboard mode (Input::Ps2KeyboardMode). A fallback mode carries the
/// set-2 path's failure as the frame's transaction identifier (offset 40),
/// encoded like a failure exit code, which is never zero.
inline constexpr Foundation::UInt64 Ps2ReadyMethod{0x8042U};
inline constexpr Foundation::UInt64 Ps2ReadyAuxFailedMethod{0x8043U};
inline constexpr Foundation::UInt32 Ps2ReadyMethodMask{0xFFFFU};
inline constexpr Foundation::UInt32 Ps2ReadyModeShift{16U};
inline constexpr Foundation::Size Ps2ReadyTransactionOffset{40U};
/// A failed driver's exit code: ComponentInputFailedExitCode with the
/// diagnostic (Input::EncodePs2Diagnostic) in bits 32-63.
[[nodiscard]] constexpr Foundation::UInt64 Ps2FailureCode(const Input::Ps2Diagnostic& diagnostic) noexcept
{
    return Abi::ComponentInputFailedExitCode | (Foundation::UInt64{Input::EncodePs2Diagnostic(diagnostic)} << 32U);
}
/// Behaviour 18. With a clock and the block's ticks per microsecond, the
/// controller's waits are time-bounded as well as poll-bounded.
[[nodiscard]] Foundation::UInt64 ServePs2(const System::SystemCallClient& client,
                                          const Foundation::UInt64 (&startBlock)[Abi::ComponentBlockWords],
                                          ComponentClock clock = nullptr) noexcept;
}
#endif
