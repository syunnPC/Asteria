// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_MEMORY_AMD64_CONTROL_REGISTER_4_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_MEMORY_AMD64_CONTROL_REGISTER_4_HPP_INCLUDED
#include "Foundation/Types.hpp"
namespace Asteria::Platform::Amd64::Memory
{
// Intel SDM Vol. 3A 253668-093US, sections 5.10.1 and 5.10.4.1: no
// global translations and no PCIDs.
inline constexpr Foundation::UInt64 DisabledTranslationFeatures{(Foundation::UInt64{1U} << 7U) |
                                                                (Foundation::UInt64{1U} << 17U)};
}
#endif
