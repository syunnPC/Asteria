// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_ABI_BOOTSTRAP_FIXTURE_HPP_INCLUDED
#define ASTERIA_ABI_BOOTSTRAP_FIXTURE_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Abi
{
/// The tag the kernel's boot-time wait fixture registers its timer
/// Notification with in the initial image's WaitSet; a completed WaitSet wait
/// returns it in value 0. The initial image learns what to expect from here,
/// so the kernel fixture and the guest test program share one definition.
inline constexpr Foundation::UInt64 BootstrapWaitTag{0x5741'4954U};
} // namespace Asteria::Abi

#endif // ASTERIA_ABI_BOOTSTRAP_FIXTURE_HPP_INCLUDED
