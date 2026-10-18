// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_DISPLAY_POLICY_HPP_INCLUDED
#define ASTERIA_BOOT_DISPLAY_POLICY_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Boot
{
enum class DisplayPolicyKind : Foundation::UInt32
{
    Native,
    Keep,
    Resolution,
};

/// Boot-only policy; no runtime mode-setting authority is transferred.
struct DisplayPolicy
{
    DisplayPolicyKind Kind{DisplayPolicyKind::Native};
    Foundation::UInt32 Width{0U};
    Foundation::UInt32 Height{0U};
};
} // namespace Asteria::Boot

#endif // ASTERIA_BOOT_DISPLAY_POLICY_HPP_INCLUDED
