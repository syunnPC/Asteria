// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Diagnostics/StopScreen.hpp"

#include "Kernel/Diagnostics/StopDisplay.hpp"
#include "Kernel/Diagnostics/StopRecord.hpp"

namespace Asteria::Kernel::Diagnostics
{
static_assert(sizeof(StopGlyphs) == 95U * 16U);
} // namespace Asteria::Kernel::Diagnostics
